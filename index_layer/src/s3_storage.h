#pragma once

#include "cache.h"

#include <string>

namespace prompt_cache_poc {

class S3Storage final : public Storage {
public:
    struct Config {
        std::string endpoint; // e.g. http://127.0.0.1:9000
        std::string bucket;   // bucket name
        long timeout_ms = 5000;
        long connect_timeout_ms = 2000;
        bool verify_tls = true;
    };

    explicit S3Storage(Config cfg);
    ~S3Storage() override;

    bool CreateBucket();

    bool Put(const std::string& obj_id, const std::vector<uint8_t>& data) override;
    bool GetRange(const std::string& obj_id, int max_bytes, std::vector<uint8_t>& out) const override;
    bool Delete(const std::string& obj_id) override;
    size_t Size() const override;

private:
    std::string BuildBucketUrl() const;
    std::string BuildObjectUrl(const std::string& obj_id) const;

    bool PerformRequest(const std::string& url,
                        const std::string& method,
                        const std::vector<uint8_t>* body,
                        std::vector<uint8_t>* out,
                        const std::string& range_header,
                        long* http_code) const;

    Config cfg_;
};

} // namespace prompt_cache_poc
