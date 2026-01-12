#include "../src/s3_storage.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using prompt_cache_poc::S3Storage;

int main() {
    const char* endpoint = std::getenv("S3_ENDPOINT");
    const char* bucket = std::getenv("S3_BUCKET");
    if (!endpoint || !bucket) {
        std::cout << "test_s3_integration skipped (set S3_ENDPOINT and S3_BUCKET)\n";
        return 0;
    }

    S3Storage::Config cfg;
    cfg.endpoint = endpoint;
    cfg.bucket = bucket;
    cfg.verify_tls = true;

    S3Storage storage(cfg);

    const char* create_bucket = std::getenv("S3_CREATE_BUCKET");
    if (create_bucket && std::string(create_bucket) == "1") {
        if (!storage.CreateBucket()) {
            std::cerr << "Failed to create bucket\n";
            return 1;
        }
    }

    std::string obj_id = "test-object";
    std::vector<uint8_t> payload{'A', 'B', 'C', 'D', 'E', 'F'};

    if (!storage.Put(obj_id, payload)) {
        std::cerr << "PUT failed\n";
        return 1;
    }

    std::vector<uint8_t> out;
    if (!storage.GetRange(obj_id, 3, out)) {
        std::cerr << "GET range failed\n";
        return 1;
    }
    assert(out.size() == 3);
    assert(out[0] == 'A');

    if (!storage.Delete(obj_id)) {
        std::cerr << "DELETE failed\n";
        return 1;
    }

    std::cout << "test_s3_integration passed\n";
    return 0;
}
