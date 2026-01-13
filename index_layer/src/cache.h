#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>

namespace prompt_cache_poc {

struct PrefixEntry {
    std::string obj_id;
    int usable_len_bytes = 0;
    int64_t version = 0;
    std::string owner_id;
    int priority = 0;
};

struct ObjectMeta {
    int total_bytes = 0;
    std::chrono::steady_clock::time_point last_access;
    int inflight_reads = 0;
};

struct LookupResult {
    bool hit = false;
    std::string obj_id;
    int usable_len_bytes = 0;
    int prefix_tokens = 0;
};

class Storage {
public:
    virtual ~Storage() = default;
    virtual bool Put(const std::string& obj_id, const std::vector<uint8_t>& data) = 0;
    virtual bool GetRange(const std::string& obj_id, int max_bytes, std::vector<uint8_t>& out) const = 0;
    virtual bool Delete(const std::string& obj_id) = 0;
    virtual size_t Size() const = 0;
};

class PrefixMap {
public:
    explicit PrefixMap(int block_size,
                       int bytes_per_token,
                       std::shared_ptr<Storage> storage);

    std::string Store(
        const std::vector<std::string>& tokens,
        const std::vector<uint8_t>& data,
        const std::string& owner_id,
        int priority,
        bool skip_put = false
    );

    LookupResult Lookup(const std::vector<std::string>& tokens, int max_len_tokens = 0) const;

    bool Load(const std::string& obj_id, int usable_len_bytes, std::vector<uint8_t>& out) const;

    size_t PrefixCount() const;
    size_t ObjectCount() const;
    int BlockSize() const;

private:
    int UsableBytes(int prefix_len, int total_tokens, int total_bytes) const;
    static uint64_t HashTokens(const std::vector<std::string>& tokens, size_t count);
    static std::string HashBytesHex(const std::vector<uint8_t>& data);

    int block_size_ = 0;
    int bytes_per_token_ = 0;
    int64_t version_clock_ = 0;

    std::shared_ptr<Storage> storage_;
    std::unordered_map<uint64_t, PrefixEntry> prefix_map_;
    std::unordered_map<std::string, ObjectMeta> obj_table_;
};

} // namespace prompt_cache_poc
