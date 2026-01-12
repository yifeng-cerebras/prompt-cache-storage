#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace server {

class Metrics {
public:
  Metrics();

  void IncInFlight();
  void DecInFlight();

  void Observe(std::string_view method,
               unsigned status,
               std::size_t req_bytes,
               std::size_t resp_bytes,
               double latency_ms);

  void ObserveRocksdb(std::string_view op,
                      bool ok,
                      std::size_t bytes,
                      double latency_ms);

  std::string RenderPrometheus() const;

private:
  enum MethodIndex {
    kGet = 0,
    kPut = 1,
    kPost = 2,
    kDelete = 3,
    kHead = 4,
    kOther = 5,
    kMethodCount = 6,
  };

  static MethodIndex method_index(std::string_view method);
  static const char* method_name(MethodIndex idx);

  static constexpr std::size_t kBucketCount = 13;

  std::array<std::atomic<std::uint64_t>, kMethodCount> req_counts_{};
  std::array<std::atomic<std::uint64_t>, kMethodCount> err_counts_{};
  std::array<std::atomic<std::uint64_t>, kMethodCount> req_bytes_{};
  std::array<std::atomic<std::uint64_t>, kMethodCount> resp_bytes_{};

  std::atomic<std::uint64_t> latency_count_{0};
  std::atomic<std::uint64_t> latency_sum_us_{0};
  std::array<double, kBucketCount> buckets_ms_{};
  std::array<std::atomic<std::uint64_t>, kBucketCount> bucket_counts_{};

  std::atomic<std::int64_t> inflight_{0};

  enum RocksOpIndex {
    kRdbGet = 0,
    kRdbPut = 1,
    kRdbWrite = 2,
    kRdbDelete = 3,
    kRdbIter = 4,
    kRdbOther = 5,
    kRdbOpCount = 6,
  };

  static RocksOpIndex rocks_op_index(std::string_view op);
  static const char* rocks_op_name(RocksOpIndex idx);

  std::array<std::atomic<std::uint64_t>, kRdbOpCount> rdb_counts_{};
  std::array<std::atomic<std::uint64_t>, kRdbOpCount> rdb_err_counts_{};
  std::array<std::atomic<std::uint64_t>, kRdbOpCount> rdb_bytes_{};

  std::atomic<std::uint64_t> rdb_latency_count_{0};
  std::atomic<std::uint64_t> rdb_latency_sum_us_{0};
  std::array<std::atomic<std::uint64_t>, kBucketCount> rdb_bucket_counts_{};
};

} // namespace server
