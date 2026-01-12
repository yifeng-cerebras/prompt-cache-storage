#include "cache.h"
#include "s3_storage.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using prompt_cache_poc::LookupResult;
using prompt_cache_poc::PrefixMap;

namespace {

void PrintUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n";
    std::cerr << "Commands:\n";
    std::cerr << "  store --tokens a,b,c --data-file path [--owner id] [--priority n]\n";
    std::cerr << "  lookup --tokens a,b,c [--max-len n]\n";
    std::cerr << "  load --obj-id id [--usable-len n] [--out-file path]\n";
    std::cerr << "  stats\n";
    std::cerr << "Options:\n";
    std::cerr << "  --block-size n (default 8)\n";
    std::cerr << "  --bytes-per-token n (default 0 = proportional)\n";
    std::cerr << "  --s3-endpoint url (required, e.g. http://127.0.0.1:9000)\n";
    std::cerr << "  --s3-bucket name (default prompt-cache)\n";
    std::cerr << "  --s3-create-bucket (create bucket on startup)\n";
    std::cerr << "  --s3-timeout-ms n (default 5000)\n";
    std::cerr << "  --s3-connect-timeout-ms n (default 2000)\n";
    std::cerr << "  --s3-insecure (disable TLS verification)\n";
}

std::vector<std::string> SplitTokens(const std::string& input) {
    std::vector<std::string> tokens;
    std::stringstream ss(input);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            tokens.push_back(item);
        }
    }
    return tokens;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return false;
    }
    return true;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    int block_size = 8;
    int bytes_per_token = 0;
    std::string s3_endpoint;
    std::string s3_bucket = "prompt-cache";
    bool s3_create_bucket = false;
    long s3_timeout_ms = 5000;
    long s3_connect_timeout_ms = 2000;
    bool s3_insecure = false;

    auto get_arg = [&](const std::string& key) -> std::string {
        for (int i = 1; i < argc - 1; ++i) {
            if (argv[i] == key) {
                return argv[i + 1];
            }
        }
        return "";
    };

    if (!get_arg("--block-size").empty()) {
        block_size = std::stoi(get_arg("--block-size"));
    }
    if (!get_arg("--bytes-per-token").empty()) {
        bytes_per_token = std::stoi(get_arg("--bytes-per-token"));
    }
    if (!get_arg("--s3-endpoint").empty()) {
        s3_endpoint = get_arg("--s3-endpoint");
    }
    if (!get_arg("--s3-bucket").empty()) {
        s3_bucket = get_arg("--s3-bucket");
    }
    if (!get_arg("--s3-timeout-ms").empty()) {
        s3_timeout_ms = std::stol(get_arg("--s3-timeout-ms"));
    }
    if (!get_arg("--s3-connect-timeout-ms").empty()) {
        s3_connect_timeout_ms = std::stol(get_arg("--s3-connect-timeout-ms"));
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--s3-create-bucket") {
            s3_create_bucket = true;
        }
        if (std::string(argv[i]) == "--s3-insecure") {
            s3_insecure = true;
        }
    }

    if (s3_endpoint.empty()) {
        std::cerr << "S3 endpoint is required (in-memory storage removed)\n";
        PrintUsage(argv[0]);
        return 1;
    }

    prompt_cache_poc::S3Storage::Config cfg;
    cfg.endpoint = s3_endpoint;
    cfg.bucket = s3_bucket;
    cfg.timeout_ms = s3_timeout_ms;
    cfg.connect_timeout_ms = s3_connect_timeout_ms;
    cfg.verify_tls = !s3_insecure;

    auto s3_storage = std::make_shared<prompt_cache_poc::S3Storage>(cfg);
    if (s3_create_bucket && !s3_storage->CreateBucket()) {
        std::cerr << "Failed to create bucket\n";
        return 1;
    }

    PrefixMap cache(block_size, bytes_per_token, s3_storage);

    std::string command = argv[1];
    if (command == "store") {
        std::string token_arg = get_arg("--tokens");
        std::string data_file = get_arg("--data-file");
        std::string owner = get_arg("--owner");
        std::string priority_arg = get_arg("--priority");

        if (token_arg.empty() || data_file.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }

        int priority = priority_arg.empty() ? 0 : std::stoi(priority_arg);
        std::vector<uint8_t> data;
        if (!ReadFile(data_file, data)) {
            std::cerr << "Failed to read data file\n";
            return 1;
        }

        std::vector<std::string> tokens = SplitTokens(token_arg);
        std::string obj_id = cache.Store(tokens, data, owner, priority);
        std::cout << "obj_id=" << obj_id << "\n";
        std::cout << "prefixes=" << cache.PrefixCount() << "\n";
        return 0;
    }

    if (command == "lookup") {
        std::string token_arg = get_arg("--tokens");
        std::string max_len_arg = get_arg("--max-len");
        if (token_arg.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }
        int max_len = max_len_arg.empty() ? 0 : std::stoi(max_len_arg);
        std::vector<std::string> tokens = SplitTokens(token_arg);
        LookupResult res = cache.Lookup(tokens, max_len);
        if (!res.hit) {
            std::cout << "hit=false\n";
            return 0;
        }
        std::cout << "hit=true\n";
        std::cout << "obj_id=" << res.obj_id << "\n";
        std::cout << "usable_len_bytes=" << res.usable_len_bytes << "\n";
        std::cout << "prefix_tokens=" << res.prefix_tokens << "\n";
        return 0;
    }

    if (command == "load") {
        std::string obj_id = get_arg("--obj-id");
        std::string usable_arg = get_arg("--usable-len");
        std::string out_file = get_arg("--out-file");
        if (obj_id.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }
        int usable_len = usable_arg.empty() ? 0 : std::stoi(usable_arg);
        std::vector<uint8_t> data;
        if (!cache.Load(obj_id, usable_len, data)) {
            std::cerr << "Object not found\n";
            return 1;
        }
        if (!out_file.empty()) {
            if (!WriteFile(out_file, data)) {
                std::cerr << "Failed to write output\n";
                return 1;
            }
            std::cout << "wrote=" << out_file << "\n";
        } else {
            std::cout.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        return 0;
    }

    if (command == "stats") {
        std::cout << "objects=" << cache.ObjectCount() << "\n";
        std::cout << "prefixes=" << cache.PrefixCount() << "\n";
        std::cout << "block_size=" << cache.BlockSize() << "\n";
        return 0;
    }

    PrintUsage(argv[0]);
    return 1;
}
