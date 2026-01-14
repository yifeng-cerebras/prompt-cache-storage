#include "../src/cache.h"
#include "../src/s3_storage.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <curl/curl.h>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using prompt_cache_poc::LookupResult;
using prompt_cache_poc::PrefixMap;
using prompt_cache_poc::S3Storage;

struct Config {
    std::string endpoint;
    std::string bucket = "prompt-cache";
    bool create_bucket = false;
    int objects = 100;
    int prompt_len = 64;
    int block_size = 8;
    int object_bytes = 65536;
    int bytes_per_token = 0;
    int max_len_tokens = 0;
    int threads = 4;
    int duration_sec = 30;
    int hotset_size = 0;
    double hotset_traffic = 0.9;
    long timeout_ms = 5000;
    long connect_timeout_ms = 2000;
    bool insecure = false;
    unsigned seed = 1;
};

struct Metrics {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> bytes_read{0};
};

static void Usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --endpoint <url> [options]\n";
    std::cerr << "Options:\n";
    std::cerr << "  --bucket name\n";
    std::cerr << "  --create-bucket\n";
    std::cerr << "  --objects n\n";
    std::cerr << "  --prompt-len n\n";
    std::cerr << "  --block-size n\n";
    std::cerr << "  --object-bytes n\n";
    std::cerr << "  --bytes-per-token n (default 0 = auto)\n";
    std::cerr << "  --max-len-tokens n (default 0 = full)\n";
    std::cerr << "  --threads n\n";
    std::cerr << "  --duration n (seconds)\n";
    std::cerr << "  --hotset-size n (0 = uniform)\n";
    std::cerr << "  --hotset-traffic f (0..1)\n";
    std::cerr << "  --timeout-ms n\n";
    std::cerr << "  --connect-timeout-ms n\n";
    std::cerr << "  --insecure\n";
    std::cerr << "  --skip-prefill (do not PUT objects; assumes storage already populated)\n";
    std::cerr << "  --prometheus (emit Prometheus text to stdout)\n";
    std::cerr << "  --seed n\n";
}

static bool ReadArg(int argc, char** argv, const std::string& key, std::string& out) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

static bool HasFlag(int argc, char** argv, const std::string& key) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == key) {
            return true;
        }
    }
    return false;
}

static double Percentile(std::vector<double>& values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>((pct / 100.0) * (values.size() - 1));
    return values[idx];
}

static size_t CurlWriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static bool FetchGatewayMetrics(const Config& cfg, std::string& out, std::string& err) {
    std::string url = cfg.endpoint;
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/metrics";

    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, cfg.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (cfg.insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (status != 200) {
        err = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    Config cfg;
    std::string val;
    bool prometheus = false;

    if (!ReadArg(argc, argv, "--endpoint", cfg.endpoint)) {
        Usage(argv[0]);
        return 1;
    }
    if (ReadArg(argc, argv, "--bucket", val)) cfg.bucket = val;
    if (ReadArg(argc, argv, "--objects", val)) cfg.objects = std::stoi(val);
    if (ReadArg(argc, argv, "--prompt-len", val)) cfg.prompt_len = std::stoi(val);
    if (ReadArg(argc, argv, "--block-size", val)) cfg.block_size = std::stoi(val);
    if (ReadArg(argc, argv, "--object-bytes", val)) cfg.object_bytes = std::stoi(val);
    if (ReadArg(argc, argv, "--bytes-per-token", val)) cfg.bytes_per_token = std::stoi(val);
    if (ReadArg(argc, argv, "--max-len-tokens", val)) cfg.max_len_tokens = std::stoi(val);
    if (ReadArg(argc, argv, "--threads", val)) cfg.threads = std::stoi(val);
    if (ReadArg(argc, argv, "--duration", val)) cfg.duration_sec = std::stoi(val);
    if (ReadArg(argc, argv, "--hotset-size", val)) cfg.hotset_size = std::stoi(val);
    if (ReadArg(argc, argv, "--hotset-traffic", val)) cfg.hotset_traffic = std::stod(val);
    if (ReadArg(argc, argv, "--timeout-ms", val)) cfg.timeout_ms = std::stol(val);
    if (ReadArg(argc, argv, "--connect-timeout-ms", val)) cfg.connect_timeout_ms = std::stol(val);
    if (ReadArg(argc, argv, "--seed", val)) cfg.seed = static_cast<unsigned>(std::stoul(val));
    cfg.create_bucket = HasFlag(argc, argv, "--create-bucket");
    cfg.insecure = HasFlag(argc, argv, "--insecure");
    bool skip_prefill = HasFlag(argc, argv, "--skip-prefill");
    prometheus = HasFlag(argc, argv, "--prometheus");

    if (cfg.bytes_per_token == 0 && cfg.prompt_len > 0) {
        cfg.bytes_per_token = std::max(1, cfg.object_bytes / cfg.prompt_len);
    }

    S3Storage::Config s3cfg;
    s3cfg.endpoint = cfg.endpoint;
    s3cfg.bucket = cfg.bucket;
    s3cfg.timeout_ms = cfg.timeout_ms;
    s3cfg.connect_timeout_ms = cfg.connect_timeout_ms;
    s3cfg.verify_tls = !cfg.insecure;

    auto s3 = std::make_shared<S3Storage>(s3cfg);
    if (cfg.create_bucket && !s3->CreateBucket()) {
        std::cerr << "Failed to create bucket\n";
        return 1;
    }

    PrefixMap cache(cfg.block_size, cfg.bytes_per_token, s3);

    std::vector<std::vector<std::string>> prompts;
    prompts.reserve(static_cast<size_t>(cfg.objects));

    auto prefill_start = std::chrono::steady_clock::now();
    const int log_every = std::max(1, cfg.objects / 20);

    std::mt19937 prefill_rng(cfg.seed);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int i = 0; i < cfg.objects; ++i) {
        std::vector<std::string> tokens;
        tokens.reserve(static_cast<size_t>(cfg.prompt_len));
        for (int t = 0; t < cfg.prompt_len; ++t) {
            tokens.push_back("tok" + std::to_string(i) + "_" + std::to_string(t));
        }
        std::vector<uint8_t> data(static_cast<size_t>(cfg.object_bytes));
        for (auto& b : data) {
            b = static_cast<uint8_t>(byte_dist(prefill_rng));
        }
        cache.Store(tokens, data, "stress", 1, skip_prefill);
        prompts.emplace_back(std::move(tokens));

        if (!skip_prefill && ((i + 1) % log_every == 0 || i + 1 == cfg.objects)) {
            std::cout << "prefill " << (i + 1) << "/" << cfg.objects << "\n";
        }
    }

    auto prefill_end = std::chrono::steady_clock::now();
    double prefill_ms =
        std::chrono::duration<double, std::milli>(prefill_end - prefill_start).count();
    if (!skip_prefill) {
        std::cout << "prefill_ms " << prefill_ms << "\n";
    }

    Metrics metrics;
    std::vector<std::thread> workers;
    std::vector<std::vector<double>> latencies(static_cast<size_t>(cfg.threads));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.duration_sec);

    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back([&, i] {
            std::mt19937 rng(cfg.seed + static_cast<unsigned>(i));
            std::uniform_real_distribution<double> pick(0.0, 1.0);
            std::uniform_int_distribution<int> all_dist(0, cfg.objects - 1);
            std::uniform_int_distribution<int> hot_dist(0, std::max(0, cfg.hotset_size - 1));

            while (std::chrono::steady_clock::now() < deadline) {
                int idx = 0;
                if (cfg.hotset_size > 0 && pick(rng) < cfg.hotset_traffic) {
                    idx = hot_dist(rng);
                } else {
                    idx = all_dist(rng);
                }

                const auto& tokens = prompts[static_cast<size_t>(idx)];
                auto start = std::chrono::steady_clock::now();
                LookupResult res = cache.Lookup(tokens, cfg.max_len_tokens);
                bool ok = res.hit;
                std::vector<uint8_t> out;
                if (ok) {
                    ok = cache.Load(res.obj_id, res.usable_len_bytes, out);
                }
                auto end = std::chrono::steady_clock::now();

                double ms = std::chrono::duration<double, std::milli>(end - start).count();
                latencies[static_cast<size_t>(i)].push_back(ms);

                metrics.requests.fetch_add(1, std::memory_order_relaxed);
                if (!ok) {
                    metrics.errors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    metrics.bytes_read.fetch_add(out.size(), std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    std::vector<double> all_lat;
    for (const auto& vec : latencies) {
        all_lat.insert(all_lat.end(), vec.begin(), vec.end());
    }

    double p50 = Percentile(all_lat, 50.0);
    double p95 = Percentile(all_lat, 95.0);
    double p99 = Percentile(all_lat, 99.0);

    double seconds = static_cast<double>(cfg.duration_sec);
    double qps = metrics.requests.load() / seconds;
    double mbps = (metrics.bytes_read.load() / (1024.0 * 1024.0)) / seconds;

    if (prometheus) {
        std::cout << "# HELP index_layer_stress_requests_total Total requests.\n";
        std::cout << "# TYPE index_layer_stress_requests_total counter\n";
        std::cout << "index_layer_stress_requests_total " << metrics.requests.load() << "\n";

        std::cout << "# HELP index_layer_stress_errors_total Total failed requests.\n";
        std::cout << "# TYPE index_layer_stress_errors_total counter\n";
        std::cout << "index_layer_stress_errors_total " << metrics.errors.load() << "\n";

        std::cout << "# HELP index_layer_stress_bytes_read_total Total bytes read.\n";
        std::cout << "# TYPE index_layer_stress_bytes_read_total counter\n";
        std::cout << "index_layer_stress_bytes_read_total " << metrics.bytes_read.load() << "\n";

        std::cout << "# HELP index_layer_stress_qps Requests per second.\n";
        std::cout << "# TYPE index_layer_stress_qps gauge\n";
        std::cout << "index_layer_stress_qps " << qps << "\n";

        std::cout << "# HELP index_layer_stress_throughput_mb_s Throughput in MiB/s.\n";
        std::cout << "# TYPE index_layer_stress_throughput_mb_s gauge\n";
        std::cout << "index_layer_stress_throughput_mb_s " << mbps << "\n";

        std::cout << "# HELP index_layer_stress_latency_ms Latency percentiles.\n";
        std::cout << "# TYPE index_layer_stress_latency_ms gauge\n";
        std::cout << "index_layer_stress_latency_ms{quantile=\"0.50\"} " << p50 << "\n";
        std::cout << "index_layer_stress_latency_ms{quantile=\"0.95\"} " << p95 << "\n";
        std::cout << "index_layer_stress_latency_ms{quantile=\"0.99\"} " << p99 << "\n";
    } else {
        std::cout << "requests " << metrics.requests.load() << "\n";
        std::cout << "errors " << metrics.errors.load() << "\n";
        std::cout << "qps " << qps << "\n";
        std::cout << "throughput_mb_s " << mbps << "\n";
        std::cout << "p50_ms " << p50 << "\n";
        std::cout << "p95_ms " << p95 << "\n";
        std::cout << "p99_ms " << p99 << "\n";
    }

    std::string gw_metrics;
    std::string gw_err;
    if (FetchGatewayMetrics(cfg, gw_metrics, gw_err)) {
        if (prometheus) {
            std::cout << "# gateway_metrics_begin\n";
            std::cout << gw_metrics;
            if (!gw_metrics.empty() && gw_metrics.back() != '\n') {
                std::cout << "\n";
            }
            std::cout << "# gateway_metrics_end\n";
        } else {
            std::cout << "gateway_metrics_begin\n";
            std::cout << gw_metrics;
            if (!gw_metrics.empty() && gw_metrics.back() != '\n') {
                std::cout << "\n";
            }
            std::cout << "gateway_metrics_end\n";
        }
    } else {
        if (prometheus) {
            std::cout << "# gateway_metrics_error " << gw_err << "\n";
        } else {
            std::cout << "gateway_metrics_error " << gw_err << "\n";
        }
    }

    return 0;
}
