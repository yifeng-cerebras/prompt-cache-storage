#include "../src/cache.h"
#include "../src/s3_storage.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using prompt_cache_poc::PrefixMap;
using prompt_cache_poc::S3Storage;

int main() {
    const char* endpoint = std::getenv("S3_ENDPOINT");
    const char* bucket = std::getenv("S3_BUCKET");
    if (!endpoint || !bucket) {
        std::cout << "test_e2e skipped (set S3_ENDPOINT and S3_BUCKET)\n";
        return 0;
    }

    S3Storage::Config cfg;
    cfg.endpoint = endpoint;
    cfg.bucket = bucket;

    auto storage = std::make_shared<S3Storage>(cfg);
    const char* create_bucket = std::getenv("S3_CREATE_BUCKET");
    if (create_bucket && std::string(create_bucket) == "1") {
        if (!storage->CreateBucket()) {
            std::cerr << "Failed to create bucket\n";
            return 1;
        }
    }

    PrefixMap cache(2, 0, storage);

    std::vector<std::string> tokens{"hello", "world", "from", "cache"};
    std::vector<uint8_t> data;
    const std::string payload = "hello world from cache";
    data.assign(payload.begin(), payload.end());

    std::string obj_id = cache.Store(tokens, data, "replica-2", 2);
    assert(!obj_id.empty());

    auto lookup = cache.Lookup(tokens, 0);
    assert(lookup.hit);

    std::vector<uint8_t> out;
    bool ok = cache.Load(lookup.obj_id, lookup.usable_len_bytes, out);
    assert(ok);

    std::string out_str(out.begin(), out.end());
    assert(!out_str.empty());

    std::cout << "test_e2e passed\n";
    return 0;
}
