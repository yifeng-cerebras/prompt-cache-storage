Copy from: https://cerebras.atlassian.net/wiki/spaces/ENG/pages/4075028527/Inference+Off-Wafer+Prompt+Caching

This document describes how the Cerebras inference solution will support off-wafer prompt caching. 

Briefly, if we process sequence with tokens ABCDEFG and then sequence with tokens ABCDHIJK, since the prefix ABCD is common to both, KV values for these common tokens will be identical. They could be evicted when processing the first sequence, and replayed when processing the second one, to save significant compute time.

Some of the common tokens ABCD could be prompt and some could be gen, when processing the initial sequence. In general prompt/gen can be arbitrarily mixed, e.g. 1st sequence prompt AB → gen CDEFG, 2nd sequence prompt ABCDH (reusing previously computed ABCD) → gen IJK. In the initial version, we may not support caching gen tokens.

Common, cacheable prefixes often occur when (1) there are fixed system prompts, or (2) in chat scenarios where the user keeps adding additional lines to an ever-growing conversation over a timescale of minutes. Because of the timescale (and the sheer volume of possible common prefixes), it is impractical to cache the KV values on-wafer.

The full solution consists of:

KV Storage Servers (KVSS), one or more shards per CS box, that communicate with the wafer to receive egress (KV evictions) and provide ingress (KV replay).

SwDriver prompt caching logic - the SwDriver will manage the cache and issue control signals to the KV Storage Servers

Communication protocol between the above

This document primarily describes (2) and (3), only touching on (1) briefly. The full design for (1) is owned by @Summer Zeng 

Note that there is generally one KVSS server per CS box (or, rather, per transformer compute core). The rest of this document will often talk about “sending a message to the KVSS server”, but what is actually meant is a broadcast to all KVSS servers for a given model.

Input to SwDriver
Framework TODO: The SwDriver requires the following pieces of information for each prompt, to be provided by the Framework in GenerationConfig:

isolation_id - typically the Org ID of the organization making the request (to prevent prompt reuse between organizations, which could be a security concern)

cache_length_allowed -The number of tokens at the left of the sequence which are allowed to be cached. 

Again, this is to mitigate potential security / data privacy concerns (some orgs may wish to e.g. cache system prompts but not sensitive user conversations)

This can also be a pricing differentiator (e.g. free / low-tier customers not allowed to participate in prompt caching)

Framework can set this higher than the prompt length (in particular, it can set this to prompt + max gen) to enable caching generated tokens. (Even though the first version of prompt caching may not yet support caching generated tokens).

Future work: In the future we may want to accept additional information, e.g. hints that a certain number of tokens on the left are likely to be reused often and thus should be prioritized when deciding what to keep in the cache.

For now, if there are any multimodal tokens in the allowed cached range, SwDriver will trim cache_length_allowed to encompass only regular text tokens.

Future work:  In the future, twe will need to adjust the design for multimodal input. (Actually there are two sub-scenarios here, reusing the same multimodal input in a different place in the prompt or in the same prompt with the same prefix). Designing prompt caching for multimodal is outside the scope of this document, although the basic approach stays the same.

Future work: In the future, we may need an API for the Framework to communicate per-org settings to the SwDriver (e.g. timed cache cleanup policy). For now, the policy will be global.

Basic SwDriver Algorithm
We handle KV cache replay in units of “cache columns”. E.g. if the cache column size is 64 tokens, we always replay a number of tokens divisible by 64 (even though eviction is done at single-token granularity). In general we will try to cache as many columns as are allowed, and replay as many columns as we have cached (except for a certain off-by-one issue mentioned further down).

The KV Storage Servers have some maximum number of tokens that they are allowed to store (based on memory constraints), which the SwDriver knows (see “Memory Allocation on KVSS” below)

The basic SwDriver algorithm is as follows: (There are complications related to Spec Decode and SWA, discussed later.)

When starting a new sequence: When SwDriver allocates space in the KV cache for a given sequence, the SWDriver attempts to find the longest prefix that the KV Storage Servers have, by hashing the incoming tokens a column at a time (see “Representing the Off-Wafer Cache” below for details). If such a prefix is found (i.e. a cache hit), it initiates a refill request as follows:

In the next token that’s picked, the start_recv metadata counter is set to allow the refll to proceed

Sends a kvss_refill message to all KV Storage Servers to initiate the refill

The estimated amount of time for the refill to complete is computed according to some perf model. (TODO: It is unclear what to measure this in. Timesteps? Absolute clock time? Cycles?)
Stack/Kernel TODO: Provide this perf model..

The sequence will only be picked again once the time above has passed, with wait_recv counter correctly set in the metadata. (Other sequences can of course proceed in the meantime). Picking will start at the token right after the last cached token.

Also when starting a new sequence: Regardless of whether a newly allocated sequence was a cache hit or not, if there is a chance that the sequence will generate new KV evictions, the SwDriver:

Sends a kvss_reserve_space message to the KV Storage Servers indicating the prompt id and estimated size of the evictions. (This should allow the KVSS code to allocate memory better to avoid fragmentation)

When sending a cache-allowed token: When the SwDriver sends in a token (whether prompt or gen) that is allowed to be cached, it:

Checks whether this new eviction would cause the maximum stored tokens limit to be exceeded. 

If so, it sends a kvss_delete message to all KV Storage Servers to delete some columns of tokens (see “Cache Eviction Policy” below), and updates its internal representation to indicate that these columns are no longer in the cache.

Sets the do_eviction predicate in the metadata to true

Sends a kvss_evict message to all KV Storage Servers indicating which token index for which prompt id is about to be evicted

Also when sending a cache-allowed token: When sufficient evictions have built up for a whole column, the SwDriver:

Updates its internal cache representation to indicate that this new column is now cached

When a sequence terminates: If the total number of evicted tokens is not evenly divisible by the column size, the SwDriver:

Sends a kvss_delete message to all KV Storage Servers to delete the extra tokens at the end that aren’t a whole column

Future work: The current code attempts KV cache allocation for some sequence only when we’re ready to pick the first token of that sequence. However, with off-wafer KV refill, we’d like to allocate space in the on-wafer cache and initiate the refill even if we’re not yet ready to pick that sequence. This will be an important optimization the SwDriver will have to do (but likely not in the initial bringup of this feature).

Multiple Models, Multiple Buffers and Multiple Caches
Multiple Models (Draft/Target): The KV caches are separate for the target and the draft models. Target and draft models will have their own KVSS servers (one per target core, and one per draft core, even if physically draft is co-located as another core on target’s box). A given prompt may be a cache hit for one model but a cache miss on another, or the number of available cached tokens may be different on target vs. draft. So, a KVSS message is broadcast only within a given model. For example, KVSS servers for target may get a “refill columns X to Y” message, whereas KVSS servers for draft model may get a “refill columns A to B” message or may not get a refill message at all.

Multiple Buffers per Cache: The “KV cache” for a given model may actually represent multiple non-contiguous PE buffers. For example, with D=2 (two decoders per region), we may actually have 4 caches in 4 separate memory locations on each PE:

d=0 K cache

d=0 V cache

d=1 K cache 

d=1 V cache

This is all abstracted away from the SwDriver and the SwDriver-KVSS protocol. For example, a single kvss_replay request of tokens 0-63 would actually send 4 separate refill messages to the wafer (d=0 K for these 64 tokens, d=0 V for these 64 tokens, etc). 

SwDriver must know how many separate buffers there are, to set metadata start_recv / wait_recv counts correctly. Summer TODO: Provide an API (e.g. on TensorMapping?) that, based on the RT IR, tells us how many buffers the KV cache is composed of (e.g. should return 4 in the example above). This number is referred to as numBuffers in the rest of the document.

Multiple Independent Caches: Some models utilize multiple KV caches per decoder. When the different caches all follow the same allocation rules, this is identical to the “multiple buffers” scenario above (e.g. if there are 2 caches with D=2 and K/V being separate, this would be equivalent to having 8 total buffers). 

However, sometimes the caches have different allocation rules. For example, the Cohere Command-X model utilizes one regular cache and several SWA (sliding window attention) caches. So with a window size of 4096, tokens 0 to 10,000 may be allocated to locations 0 to 10,000 in one cache but keep looping in a window from 0 to 4,095 in the other cache. This is why a single physical KVSS process must be able to maintain multiple logical caches, and most KVSS<->SwDriver protocol messages will take a cache_id parameter. See also “Memory Allocation on KVSS” below.

Representing the Off-Wafer Cache
We do not want KV Storage Servers to make their own decisions about what to keep in the cache and what to discard. If we did that, cache replay would require the SwDriver to ask each KVSS whether a given prompt is in the cache, plus some locking logic, etc. We wish to avoid the perf hit of this back-and-forth.

Therefore, the off-wafer KV cache will instead be managed by the SwDriver:

SwDriver knows how many tokens each KVSS can hold (uses the minimum across all KVSS servers)

SwDriver will never ask KVSS to hold more tokens than that. I.e. when it issues a kvss_evict message, it ensures that it first issues sufficient kvss_delete messages to make room.

KVSS can hold the tokens in an arbitrarily fragmented way. I.e. the SwDriver only needs to worry about how many tokens KVSS holds, it doesn’t worry about where these tokens are. (Of course KVSS might strive to minimize fragmentation using kvss_reserve_space messages, it might have a background thread to do defragmentation, etc).

The SwDriver holds a representation of the state of the cache in its memory. It will work as follows:

KV Cache Representation is a map from isolation_id (i.e. typically the org ID) to an isolated map. (This ensures that KV cache is per-org, for security reasons)

The isolated map, the PrefixMap, is a map from a 64-bit prefix hash to PrefixInfo

PrefixInfo contains:

promptId: 64-bit prompt ID

cachedLength: length so far

evictCount: 64-bit counter of evictions so far

Cache expiry fields (see “Cache Expiry Policy” below)

last_used timestamp 

first_used timestamp

parent_hash

For example, suppose column size is 4 tokens, and suppose some org processed the following (imagine a chat continuation scenario):

Prompt id=111: Tokens ABCDE (suppose token D was evicted as the 1000th eviction ever)

Prompt id=222: Tokens ABCDEFGHIJKLMN (suppose token H is the 2500th eviction ever, token L is the 2580th eviction ever)

Then the isolated PrefixMap will contain:

{Hash of ABCD} → {promptId: 111, cachedLength: 4, evictCount: 1000}

{Hash of ABCD EFGH} → {promptId: 222, cachedLength: 8, evictCount: 2500} 

{Hash of ABCD EFGH IJKL} → {promptId: 222, cachedLength: 12, evictCount: 2580} 

The SwDriver, when it gets a new prompt, will calculate min(prompt_length - 1, cache_length_allowed). The “-1” is required because we always need to explicitly send at least one prompt token (i.e. not have it cached) to receive the first gen token. It will then round down to the cache column size, and will attempt to look up the hash of the 1st column in the PrefixMap. If it gets a hit (hash is in map and has correct cachedLength for one column), it will add on the 2nd column (i.e. calculate combined hash of first 2 columns) and look that up. It will keep going in this way until there’s a cache miss. It will then construct a kvss_refill request to the KVSS servers for the columns that were cache hits.

Future work: Eliminate the “-1” constraint above by replaying the last prompt token even if it was cached.

The SwDriver, whenever it sends a cache-allowed token that is the last token in a column, will calculate the has of all tokens so far, and will make an entry in the PrefixMap under that hash, recording the prompt ID, number of tokens so far, and the eviction count including the eviction that would result from this last-in-column token. (The eviction count is needed for KVSS concurrency, see below).

SwDriver ↔︎ KVSS Protocol
Summer TODO: Implement the protocol below.

The KVSS must be able to hold, it each of its logical caches, at least the allocated number of tokens for that cache (see “Memory Allocation on KVSS” below). The KVSS must allow arbitrary fragmentation of tokens (but will probably strive to minimize fragmentation)

The KVSS should organize its data according to 64-bit prompt IDs and token indices within each prompt ID. The SwDriver will always instruct the KVSS to store a contiguous range of token indicess for each prompt, with no holes. The range can only grow and shrink on the right. Both things can be asserted. E.g. if SwDriver has previously caused wafer eviction for token indices 100 to 150, it is illegal for the SwDriver to cause an eviction for token 152 (because this would cause a hole at 151), or to delete token 125 (because it would cause a hole), or to delete token 100 (because this would shrink on the left), or to cause eviction of token 99 (because it would grow on the left). It is perfectly legal to cause eviction of token 151 (growth on right) or delete token 150 (shrink on right). It is also perfectly legal to cause eviction of e.g. token 125 (in-place replacement of an existing cached value, this is important for Spec Decode).

Basically, the KVSS can always associate a prompt_id with an token index offset (that never changes, e.g. 100 in the example above) and num_tokens (that can grow or shrink, e.g. 50 in the example above). Note again that these 50 tokens can be arbitrarily fragmented in memory. The offset can be determined from the kvss_reserve_space call described below. KVSS should be careful not to leak memory in metadata data structures, e.g. when num_token reaches 0 for a given prompt_id, that prompt_id should be deleted.

KVSS should not assume that prompt_ids are small, nor that they are contiguous. However prompt_id of 0 is reserved and will never be used.

The protocol allows for the following calls. Please note important concurrency requirements below.

KVSS Command: Reserve Space
kvss_reserve_space (int cache_id, uint64_t prompt_id, uint64_t continue_id, int start_token_idx, int num_tokens)
Notifies KVSS that SwDriver is about to start evictions for a given prompt starting at index start_token_idx, with expected size of num_tokens. The start_token_idx will always be accurate, but num_tokens is only an estimate, actual number of evicted tokens may be smaller or larger.

This call may be used by KVSS to try and avoid fragmentation (pre-allocate contiguous memory for a given prompt). It can also be completely ignored in the first version.

SwDriver may also optionally set continue_id to a nonzero prompt ID as a hint that a given prompt continues some other prompt (e.g. in a chat continuation scenario). This is a hint only (to help avoid fragmentation) and can safely be ignored. Note that multiple prompts may continue a given prompt.

KVSS Command: Refill
kvss_refill (vector<RefillRequest>) 

Each RefillRequest is a separate refill request. We send a vector of them, because in a multi-cache scenario we will typically want to refill all caches at once. Each request has:
int cache_id, int sequenceId, int cacheIdx, uint64_t evictCount, vector<RefillChunk>

cache_id is the ID of the cache. If there are multiple RefillRequests for the same cache_id, they must be processed in order (we can disallow this scenario initially, i.e. explicitly assert)

sequenceIdis used by KVSS to fix up the data before it gets sent to the wafer, to ensure it has the correct sequence ID. A full discussion of this feature is outside the scope of this document (owned by Summer).

cacheIdx is the destination index in the on-wafer KV cache where the refill should start. This is used to compute the PE memory addresses for the refill. A full discussion of this feature is outside the scope of this document (owned by Summer).

evictCount - the KVSS must wait for this many evictions to complete before starting work on a given RefillRequest (see note on concurrency below).

Each RefillChunk corresponds to a contiguous range of tokens to send from a given prompt ID. We want an entire vector of these because:

We may need to glue together tokens from multiple prompts, e.g. in a chat continuation scenario. E.g. assuming prompt+gen caching, we could have “What is the capital of France? Paris” (tokens e.g. 0 to 19) from prompt 111, “Great, what can I see there? The Eiffel Tower” from prompt 222 (tokens e.g. 20 to 39)

We may need to glue together tokens from the same prompt, e.g. ina SWA scenario. For example, for a window of size 1000 of which 10 are sink tokens, column size of 100, and prompt size of 1200, we will want to glue together tokens 0-9, then tokens 1000-1199, then 210-999.

Important: The total number of glued tokens in a RefillRequest will always be divisible by column size, but the individual RefillChunks may not be, as shown in the SWA example above.

Important: Each RefillRequest must correspond to a single, glued transfer to the wafer (or rather, to exactly numBuffers transfers in the multi-buffer case). This is important because SwDriver must set the start_recv/wait_recv metadata correctly, and it cannot do so if the KVSS splits up the transfers arbitrarily.

Each RefillChunk has:
uint64_t prompt_id, int startTokenIdx, int numTokens

This is self-explanatory (corresponds to numTokens tokens starting at index startTokenIdx for prompt prompt_id)

KVSS Command: Delete
kvss_delete (vector<DeleteRequest>)

Each DeleteRequest is a separate delete request. We send a vector of them, because in a multi-cache scenario we will typically want to delete from all caches at once. Each request has:
int cache_id, uint64_t prompt_id, uint64_t evictCount, int startTokenIdx, int endTokenIdx

For each deletion request, KVSS waits until we’ve done evictCount complete evictions (see note on concurrency below), then deletes tokens from startTokenIdx to endTokenIdx inclusive. Recall that deletion can only be from the right of the prompt, cannot create any holes, and any prompt that shrinks to size 0 must be completely removed from any metadata.

KVSS Command: Evict
kvss_evict (vector<EvictToken>)

Each kvss_evict call corresponds to a single batch (i.e. G tokens) evicted from the wafer and ingested by KVSS. When we process a single kvss_evict message, we increment the global evictCount (i.e. the “evict count” is a count of completely evicted batches), see note on concurrency below.

The vector<EvictToken>is of size G (where G could of course vary from batch to batch due to dynamic G, G-star, etc). Each EvictToken represents a single token’s worth of data, and is a vector<EvictTokenEntry> of size num_caches (for each cache_id)

Each EvictTokenEntry is:
uint64_t promptId, int tokenIdx, int sequenceId

promptId, if zero, indicates that this token should not be cached (should be received but then discarded). If nonzero, it represents an eviction of a token with index tokenIdx. The sequenceId represents the sequence ID assigned to this token on-wafer (KVSS is free to use this for either validation, or for avoiding useless sequence ID update on refill, or it can just ignore it). 

Recall that an eviction can either overwrite an existing token in KVSS memory, or add a token immediately to the right of any existing token indices (of course this can be arbitrarily fragmented in actual memory). Any other eviction is disallowed (can assert). Of course, in a batch of G tokens we may add multiple consecutive tokens to the right of existing token indices (e.g. have 0-99 already cached, evict 100/101/102).

Note on Concurrency:
Eviction is asynchronous with refill, these are separate streams. This introduces potential race conditions between Evict/Refill (and there’s also a potential race with Evict/Delete and even Delete/Refill). Suggested concurrency design is as follows:

Reserve Space and Evict messages go into the “eviction queue” and are processed by KVSS “evict thread” in order

Delete and Refill messages go into the “refill queue” and are processed by KVSS “refill thread” in order

Synchronization in one direction (“evict thread → refill thread”)

This is done via evictCount. Each successfully processed Evict message will increment a global eviction counter, Delete/Refill must block until the counter reaches the value picked by SwDriver (which will have a linear view of all evicts/refills and will be able to reason about which “version” needs to be refilled/deleted). SwDriver has a linear view since evicts/refills/deletes are all scheduled by a single InferenceCore thread (in particular, refills are scheduled at sequence allocation time at time of pick_single_token). We are assuming here that we never specifically want an older “version” (the only scenario for token overwrite is Spec Decode, and there’s no reason to ever want to bring in an old version of the KV cache for something that turned out to be a spec failure).

Synchronization in the other direction (“refill thread → evict thread”)

kvss_evict may not be able to find space for the additional tokens in the evicted batch (even after taking prompt_id==0 into account, i.e. discarded tokens). In this case it must block until the refill thread processes enough kvss_delete messages for the allocation to become possible. It is the job of the SwDriver to ensure that this never deadlocks (again, easy because SwDriver has a linear view)

 kvss_reserve_space must never block, i.e. KVSS should just ignore it if it cannot allocate sufficient space for the reservation. This is a hint only, and not a real allocation.

Sliding Window Attention
Sliding Window Attention is completely handled on the SwDriver end, there is no KVSS impact (other than the protocol already described).

In Sliding Window Attention, if the total window size (including sink tokens) is not a multiple of the column size, prompt caching becomes very difficult. We will likely disable prompt caching beyond the window size, if for some ML reason we are unable to extend the window size to the next higher multiple of the column size (e.g. for Cohere Command-X where SWA is integral to the model itself).
TODO: Is this acceptable?
Future work: Consider supporting this by replaying tokens from the middle of the window

The rest of this section deals with the case where the cached portion exceeds the total window size, and the total window size is a multiple of cache column size.

For SWA, SwDriver causes eviction of all tokens in order (as if there was no sliding window). When it comes to recording the prefixes in the PrefixMap, however, sink tokens need to be taken into account. We record a prefix whenever we evict the last token in a column, which may correspond to unusual prefix lengths.

E.g. suppose a sliding window of size 100, column size 10, and 3 sink tokens, and allowed cache length of 140 tokens:

We evict tokens 0-9, 10-19, …, 90-99 as usual, which results in PrefixMap entries for lengths 10, 20, …, 100 as usual

We then loop back to the beginning of the window but skip the 3 sink tokens. We evict 7 tokens (100-106) and make a PrefixMap entry for 106

We then evict tokens 107-116, 117-126, 127-136 and make PrefixMap entries for 117, 127 and 137 token length

For refill, we glue together sink tokens + portion of the window that looped around + pre-loop portion of the window, to reconstruct what the window needs to look like, e.g. in the example above we would be able to refill 137 logical tokens (by only physically refilling 100 tokens) by gluing together as follows:

3 sink tokens (0 to 2)

37 loop-around tokens (100 to 136)

60 pre-loop tokens (40 to 99)

Speculative Decoding
Speculative Decoding is completely handled on the SwDriver end, there is no KVSS impact (other than the protocol already described).

With Speculative Decoding, KV data for an evicted token (for either target or draft model) may turn out to be incorrect, and may need to be later overwritten.

In the initial version, SwDriver may not support caching generated tokens at all, or may not support caching them when Spec Decode is enabled. Eventually, however, there is nothing fundamental that prevents us from supporting this. SwDriver will need to carefully keep track of which tokens are known with certainty, and only update the PrefixMaps (for both target and draft) with verified tokens, setting evictionCount correctly to reflect the tokens after any spec failure correction.

Cache Expiry Policy
Typically, the cache will be sized such that we can only keep a few minutes worth of tokens. However, some customers have security concerns about long-term caching, and would like strong guarantees about cache expiry. Specifically:

Customers may want cache contents to have guaranteed expiry from time of last use (e.g. 1 hour)

Customers may want cache contents to have guaranteed expiry from time of first use (e.g. 12 hours)

In the PrefixMap, we will keep track of time of first use (set when we initially record a given hash), and time of last use (set on initial recording and whenever we get a cache hit). Also, each prefix will record the prefix hash that is its “parent”, i.e. the hast that it is a continuation of (e.g. if we have ABC and ABCDEF in the map, the parent entry for ABCDEF will be the hash of ABC)

SwDriver will have a cleanup thread which will monitor the PrefixMap for expired entries:

Any entry with last_use is too far in the past will be marked for deletion

Any entry with first_use too far in the past will be marked for deletion, as well as its children (ones whose parent_hash points to them), and so on recursively. (Note that last_use needs no such mechanism since last_use of child nodes will always be same or later than the last_use of parent nodes)

Actual deletion will be performed by the InferenceCore thread (to avoid concurrency issues), on every iteration outside of the critical path. To delete an entry, we remove it from PrefixMap and send a kvss_delete message to KVSS.

Memory Allocation on KVSS
Initial allocation can be simplistic (e.g. INI to control number of tokens to allocate space for, in the multi-cache case we can hard-code some assumption like “same number of tokens for each cache”).

Future work: Make this smarter

Regardless of how smart we make this, SwDriver must know exactly how many tokens the KVSS has space for (for each cache).

Cross-Replica Considerations
It is highly desirable for a request with a cached portion to land on a replica that actually contains the cache. There are two possible approaches here (as well as a hybrid between the two):

Load balancer affinity. 

Shared KV cache between replicas

Note that load balancer affinity does not have to be perfect, since we can always just take a cache miss. It can be very crude, but it does have to be load-sensitive, e.g.:

Split traffic by org id

If a certain org id becomes too “hot” (insufficient trafffic splitting), split further by hash of the first 100 characters of the prompt text

If a certain org+hash(100) becomes too “hot”, expand to hash(200)

Etc…

Even such a crude scheme will likely get us most of the benefit. Using load balancer affinity gives us the following benefits:

Usable even when the replicas are completely geographically separated (different data centres)

Preserves the notion of strict replica separation.

No single point of failure that can affect multiple replicas (unlike with shared KV state)

Numerically bad replica cannot pollute results of another replica (in case of a bad system)

Trivially easy replica restart (no need to worry about shared connections, etc)

No need to synchronize across replicas

SwDriver has complete view of what’s in the cache, no need for a chatty protocol to discover what’s there (especially across multiple KVSS servers), no need for a locking mechanism, etc

SwDriver can easily and efficiently micromanage the cache at the token level for features like Spec Decode, SWA, dynamic G / G-star, and others in the future

However, a large shared repository of KV cache data (perhaps even disk-backed, and with cache times >>5min) is also compelling.

One idea is to have a 2-level cache, where SwDriver can cause KVSS to evict data to a “SharedKVSS” (instead of deleting data). This will only be done for things we may need long-term (e.g. only whole columns, only verified data for Spec Decode). SwDriver can then query this 2nd-level cache in case of a cache miss in the 1st-level KVSS. Care must be taken around the latency of this query (will affect the non-cached case as well), and around the ability to emergency-reset the shared cache in case of numeric issues.

 