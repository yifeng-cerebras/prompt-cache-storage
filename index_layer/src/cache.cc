#include "cache.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace prompt_cache_poc {

PrefixMap::PrefixMap(int block_size, int bytes_per_token, std::shared_ptr<Storage> storage)
    : block_size_(block_size),
      bytes_per_token_(bytes_per_token),
      storage_(std::move(storage)) {
    if (!storage_) {
        throw std::invalid_argument("storage must not be null");
    }
}

std::string PrefixMap::Store(
    const std::vector<std::string>& tokens,
    const std::vector<uint8_t>& data,
    const std::string& owner_id,
    int priority
) {
    const std::string obj_id = HashBytesHex(data);
    if (!storage_->Put(obj_id, data)) {
        return "";
    }

    version_clock_++;
    ObjectMeta meta;
    meta.total_bytes = static_cast<int>(data.size());
    meta.last_access = std::chrono::steady_clock::now();
    meta.inflight_reads = 0;
    obj_table_[obj_id] = meta;

    if (tokens.size() < static_cast<size_t>(block_size_)) {
        return obj_id;
    }

    for (int prefix_len = block_size_; prefix_len <= static_cast<int>(tokens.size()); prefix_len += block_size_) {
        const uint64_t hash = HashTokens(tokens, static_cast<size_t>(prefix_len));
        const int usable = UsableBytes(prefix_len, static_cast<int>(tokens.size()), static_cast<int>(data.size()));
        PrefixEntry entry;
        entry.obj_id = obj_id;
        entry.usable_len_bytes = usable;
        entry.version = version_clock_;
        entry.owner_id = owner_id;
        entry.priority = priority;
        prefix_map_[hash] = entry;
    }

    return obj_id;
}

LookupResult PrefixMap::Lookup(const std::vector<std::string>& tokens, int max_len_tokens) const {
    if (tokens.size() < static_cast<size_t>(block_size_)) {
        return {};
    }

    if (max_len_tokens <= 0 || max_len_tokens > static_cast<int>(tokens.size())) {
        max_len_tokens = static_cast<int>(tokens.size());
    }

    PrefixEntry last_entry;
    int last_prefix = 0;

    for (int prefix_len = block_size_; prefix_len <= max_len_tokens; prefix_len += block_size_) {
        const uint64_t hash = HashTokens(tokens, static_cast<size_t>(prefix_len));
        auto it = prefix_map_.find(hash);
        if (it == prefix_map_.end()) {
            break;
        }
        last_entry = it->second;
        last_prefix = prefix_len;
    }

    if (last_prefix == 0) {
        return {};
    }

    LookupResult res;
    res.hit = true;
    res.obj_id = last_entry.obj_id;
    res.usable_len_bytes = last_entry.usable_len_bytes;
    res.prefix_tokens = last_prefix;
    return res;
}

bool PrefixMap::Load(const std::string& obj_id, int usable_len_bytes, std::vector<uint8_t>& out) const {
    if (!storage_ || !storage_->GetRange(obj_id, usable_len_bytes, out)) {
        return false;
    }
    return true;
}

size_t PrefixMap::PrefixCount() const {
    return prefix_map_.size();
}

size_t PrefixMap::ObjectCount() const {
    return obj_table_.size();
}

int PrefixMap::BlockSize() const {
    return block_size_;
}

int PrefixMap::UsableBytes(int prefix_len, int total_tokens, int total_bytes) const {
    if (bytes_per_token_ > 0) {
        int bytes = prefix_len * bytes_per_token_;
        if (bytes > total_bytes) {
            bytes = total_bytes;
        }
        return bytes;
    }

    if (total_tokens == 0 || total_bytes == 0) {
        return 0;
    }

    double frac = static_cast<double>(prefix_len) / static_cast<double>(total_tokens);
    int bytes = static_cast<int>(frac * static_cast<double>(total_bytes));
    if (bytes < 1) {
        bytes = 1;
    }
    if (bytes > total_bytes) {
        bytes = total_bytes;
    }
    return bytes;
}

uint64_t PrefixMap::HashTokens(const std::vector<std::string>& tokens, size_t count) {
    std::ostringstream oss;
    for (size_t i = 0; i < count; ++i) {
        if (i) {
            oss << "\x1f";
        }
        oss << tokens[i];
    }

    std::string input = oss.str();
    std::hash<std::string> hasher;
    return static_cast<uint64_t>(hasher(input));
}

std::string PrefixMap::HashBytesHex(const std::vector<uint8_t>& data) {
    std::hash<std::string> hasher;
    std::string as_string(data.begin(), data.end());
    size_t h = hasher(as_string);

    std::ostringstream oss;
    oss << std::hex << std::setw(sizeof(size_t) * 2) << std::setfill('0') << h;
    return oss.str();
}

} // namespace prompt_cache_poc
