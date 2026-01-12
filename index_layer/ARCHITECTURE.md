## Prompt Cache Storage System - Architecture (Detailed)

### 1. Goals
- Memory-only hit decision: prefix hit/miss does not touch storage.
- One fetch per hit: single range read for `[0..usable_len)`.
- Cross-replica reuse: cached KV bytes are shared.
- Clear ownership and GC semantics.

### 2. Updated Note: Storage RPC Compatibility
The data-plane RPC layer must be **S3-compatible** (e.g., S3 API or S3-style
range reads), so the system can later swap in a dedicated object storage backend
without changing Layer-1/Layer-2 semantics or call patterns.

### 3. High-Level Architecture
```
Layer 1: Inference Engine
  |
Layer 2: Middle Layer (per replica)
  |        \
  |         -> etcd (metadata pub/sub + GC election)
  |
Layer 3: Data Plane
  - S3-compatible RPC facade
  - Backed by local RocksDB/NVMe or external object storage
  - Range reads for `[0..usable_len)` only
```

### 3.1 Repository Components

This repo maps to the layers as follows:
- **index_layer/**: Layer 2 (PrefixMap + lookup/load/store logic).
- **s3_rocksdb_gateway/**: Layer 3 (S3-compatible facade over RocksDB).

### 4. Layer Responsibilities

#### 4.1 Layer 1 - Inference Engine
- `Lookup(prefix_tokens) -> {obj_id, usable_len}`
- `Load(obj_id, [0..usable_len))`
- `Store(tokens, block_idx, bytes, priority?)`
- No etcd access.

#### 4.2 Layer 2 - Middle Layer
In-memory metadata:

```
PrefixMap:
  prefix_hash -> {obj_id, usable_len_bytes, version, owner_id, priority}

ObjectTable:
  obj_id -> {storage_key, total_bytes, lru_node, inflight_reads}
```

Hit/miss is memory-only. Metadata is synchronized via etcd ADVERTISE/TOMBSTONE.

Additional invariants:
- `PrefixMap` is the only structure used for hit/miss.
- `ObjectTable` is advisory (placement, GC stats, inflight tracking).
- A restart loses local metadata but does not break correctness.

#### 4.3 Layer 3 - Data Plane (S3-Compatible RPC)
- `GET(obj_id, range)`
- `PUT(obj_id, bytes)`
- `DEL(obj_id)`

Properties:
- Objects are immutable after PUT.
- Placement is deterministic (rendezvous hash of obj_id).
- S3-compatible interface enables migration to a dedicated storage service
  without changing Layer-1/Layer-2 behavior.

Data plane contract:
- Single GET per hit for `[0..usable_len)`.
- Read-after-write consistency is not required, only eventual visibility.
- Missing object on GET is treated as a cache miss (invalidate prefix).

### 5. Ownership Semantics
- Everyone can read.
- Only owner can delete.
- Ownership limits deletion, not sharing.

Ownership is not exclusivity. It applies only to destructive actions
and GC decisions.

### 6. Eviction/GC (Two-Phase)
1) Publish `TOMBSTONE(obj_id)`
2) Readers drop references and stop reads
3) After grace window: owner/leader performs delete

Grace window protects inflight reads.

### 7. Metadata Plane (etcd)
etcd is used for:
- Pub/Sub broadcast + replay.
- GC leader election.
- Global capacity coordination (Phase 2+).

Events are idempotent:

**ADVERTISE**
```
(prefix_hash, obj_id, usable_len, version, owner_id, priority, ts)
```

**TOMBSTONE**
```
(obj_id, epoch, owner_id)
```

### 8. Object Model (Immutable)
Objects are immutable. Growth creates a new object:
- New object ID and placement.
- Publish ADVERTISE for each prefix covered.
- Old objects stay until GC.

### 9. Lookup / Load Flow (Fast Path)
1) Layer 1 calls `Lookup(prefix_tokens)` on Layer 2.
2) Layer 2 checks `PrefixMap` only.
3) If hit, returns `{obj_id, usable_len}`.
4) Layer 1 performs single GET `[0..usable_len)` via Layer 3.

### 10. Store Flow
1) Layer 2 chooses placement (rendezvous hash).
2) Layer 3 PUTs object.
3) Layer 2 publishes ADVERTISE for covered prefixes.
4) Replicas update PrefixMap.

### 11. Failure Handling
- Storage miss on GET: treat as cache miss and evict prefix.
- Replica restart: rebuilds metadata from etcd replay.
- Data-plane node loss: objects on that node are lost; cache refills from source.

### 12. Implementation Phases
1) MVP: local storage backend, no global GC.
2) GC leader election + global budget.
3) Priority-aware eviction, optional replication.

### 13. Current MVP Status (This Repo)
The current implementation in `index_layer/` is **MVP only**:
- No etcd integration.
- No GC or eviction logic.
- No multi-replica coordination.
- Storage is external only (S3 gateway or object store).

It is meant to validate the PrefixMap hit/miss logic and the lookup/load/store
interfaces before wiring in the metadata plane (etcd/GC).

### 14. Pairing with the S3 Gateway
To pair the index layer with the S3 gateway in this repo:
1) Start the gateway in `../s3_rocksdb_gateway` (default auth is `none`).
2) Use the built-in S3 client adapter (libcurl-based) by passing:
   - `--s3-endpoint http://127.0.0.1:9000`
   - `--s3-bucket prompt-cache`
   - `--s3-create-bucket` (optional)
3) The adapter performs:
   - `PUT`s objects into a bucket keyed by `obj_id`.
   - `GET`s using a single `Range: bytes=0-(usable_len-1)` header.
4) Leave PrefixMap decision logic unchanged.

### 15. Stress Testing Guidance
Basic stress testing setup:
1) Run the gateway with RocksDB on local NVMe.
2) Use the end-to-end tool in `index_layer`:
   - Build: `make stress`
   - Run: `./bin/stress_e2e --endpoint http://127.0.0.1:9000 --bucket prompt-cache`
3) The tool will:
   - Preload objects via `PUT`.
   - Issue repeated `GET` with range headers using cached prefixes.
4) Measure:
   - p50/p99 latency of GET.
   - QPS achieved before latency degrades.
   - CPU usage in the gateway and client.

Notes:
- SigV4 is disabled by default to reduce overhead during stress tests.
- For higher QPS, avoid large objects; use block-sized KV payloads.
