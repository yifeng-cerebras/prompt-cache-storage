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
        std::cout << "test_prefix_map skipped (set S3_ENDPOINT and S3_BUCKET)\n";
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

    PrefixMap cache(4, 1, storage);

    std::vector<std::string> tokens{"A", "B", "C", "D", "E", "F", "G", "H"};
    std::vector<uint8_t> data(8, 42);

    std::string obj_id = cache.Store(tokens, data, "replica-1", 1);
    assert(!obj_id.empty());
    assert(cache.PrefixCount() == 2);

    auto hit = cache.Lookup(tokens, 0);
    assert(hit.hit);
    assert(hit.obj_id == obj_id);
    assert(hit.prefix_tokens == 8);
    assert(hit.usable_len_bytes == 8);

    std::vector<uint8_t> out;
    bool ok = cache.Load(obj_id, hit.usable_len_bytes, out);
    assert(ok);
    assert(out.size() == 8);
    assert(out[0] == 42);

    std::cout << "test_prefix_map passed\n";
    return 0;
}
