#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace server {

Metrics::Metrics() {
  buckets_ms_ = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
  for (auto& c : bucket_counts_) {
    c.store(0);
  }
  for (auto& c : rdb_bucket_counts_) {
    c.store(0);
  }
}

void Metrics::IncInFlight() {
  inflight_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::DecInFlight() {
  inflight_.fetch_sub(1, std::memory_order_relaxed);
}

Metrics::MethodIndex Metrics::method_index(std::string_view method) {
  if (method == "GET") return kGet;
  if (method == "PUT") return kPut;
  if (method == "POST") return kPost;
  if (method == "DELETE") return kDelete;
  if (method == "HEAD") return kHead;
  return kOther;
}

const char* Metrics::method_name(MethodIndex idx) {
  switch (idx) {
    case kGet: return "GET";
    case kPut: return "PUT";
    case kPost: return "POST";
    case kDelete: return "DELETE";
    case kHead: return "HEAD";
    default: return "OTHER";
  }
}

void Metrics::Observe(std::string_view method,
                      unsigned status,
                      std::size_t req_bytes,
                      std::size_t resp_bytes,
                      double latency_ms) {
  MethodIndex idx = method_index(method);
  req_counts_[idx].fetch_add(1, std::memory_order_relaxed);
  req_bytes_[idx].fetch_add(req_bytes, std::memory_order_relaxed);
  resp_bytes_[idx].fetch_add(resp_bytes, std::memory_order_relaxed);
  if (status >= 400) {
    err_counts_[idx].fetch_add(1, std::memory_order_relaxed);
  }

  latency_count_.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t us = static_cast<std::uint64_t>(std::llround(latency_ms * 1000.0));
  latency_sum_us_.fetch_add(us, std::memory_order_relaxed);

  for (std::size_t i = 0; i < buckets_ms_.size(); ++i) {
    if (latency_ms <= buckets_ms_[i]) {
      bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
}

Metrics::RocksOpIndex Metrics::rocks_op_index(std::string_view op) {
  if (op == "get") return kRdbGet;
  if (op == "put") return kRdbPut;
  if (op == "write") return kRdbWrite;
  if (op == "delete") return kRdbDelete;
  if (op == "iter") return kRdbIter;
  return kRdbOther;
}

const char* Metrics::rocks_op_name(RocksOpIndex idx) {
  switch (idx) {
    case kRdbGet: return "get";
    case kRdbPut: return "put";
    case kRdbWrite: return "write";
    case kRdbDelete: return "delete";
    case kRdbIter: return "iter";
    default: return "other";
  }
}

void Metrics::ObserveRocksdb(std::string_view op,
                             bool ok,
                             std::size_t bytes,
                             double latency_ms) {
  RocksOpIndex idx = rocks_op_index(op);
  rdb_counts_[idx].fetch_add(1, std::memory_order_relaxed);
  rdb_bytes_[idx].fetch_add(bytes, std::memory_order_relaxed);
  if (!ok) {
    rdb_err_counts_[idx].fetch_add(1, std::memory_order_relaxed);
  }

  rdb_latency_count_.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t us = static_cast<std::uint64_t>(std::llround(latency_ms * 1000.0));
  rdb_latency_sum_us_.fetch_add(us, std::memory_order_relaxed);

  for (std::size_t i = 0; i < buckets_ms_.size(); ++i) {
    if (latency_ms <= buckets_ms_[i]) {
      rdb_bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
}

std::string Metrics::RenderPrometheus() const {
  std::ostringstream oss;

  oss << "# HELP s3gw_requests_total Total HTTP requests.\n";
  oss << "# TYPE s3gw_requests_total counter\n";
  for (int i = 0; i < kMethodCount; ++i) {
    oss << "s3gw_requests_total{method=\"" << method_name(static_cast<MethodIndex>(i))
        << "\"} " << req_counts_[i].load() << "\n";
  }

  oss << "# HELP s3gw_request_errors_total HTTP requests with status >= 400.\n";
  oss << "# TYPE s3gw_request_errors_total counter\n";
  for (int i = 0; i < kMethodCount; ++i) {
    oss << "s3gw_request_errors_total{method=\"" << method_name(static_cast<MethodIndex>(i))
        << "\"} " << err_counts_[i].load() << "\n";
  }

  oss << "# HELP s3gw_request_bytes_total Request body bytes.\n";
  oss << "# TYPE s3gw_request_bytes_total counter\n";
  for (int i = 0; i < kMethodCount; ++i) {
    oss << "s3gw_request_bytes_total{method=\"" << method_name(static_cast<MethodIndex>(i))
        << "\"} " << req_bytes_[i].load() << "\n";
  }

  oss << "# HELP s3gw_response_bytes_total Response body bytes.\n";
  oss << "# TYPE s3gw_response_bytes_total counter\n";
  for (int i = 0; i < kMethodCount; ++i) {
    oss << "s3gw_response_bytes_total{method=\"" << method_name(static_cast<MethodIndex>(i))
        << "\"} " << resp_bytes_[i].load() << "\n";
  }

  oss << "# HELP s3gw_inflight_requests In-flight HTTP requests.\n";
  oss << "# TYPE s3gw_inflight_requests gauge\n";
  oss << "s3gw_inflight_requests " << inflight_.load() << "\n";

  oss << "# HELP s3gw_request_latency_ms Request latency in milliseconds.\n";
  oss << "# TYPE s3gw_request_latency_ms histogram\n";

  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < buckets_ms_.size(); ++i) {
    cumulative += bucket_counts_[i].load();
    oss << "s3gw_request_latency_ms_bucket{le=\"" << buckets_ms_[i] << "\"} "
        << cumulative << "\n";
  }
  std::uint64_t count = latency_count_.load();
  oss << "s3gw_request_latency_ms_bucket{le=\"+Inf\"} " << count << "\n";
  double sum_ms = static_cast<double>(latency_sum_us_.load()) / 1000.0;
  oss << "s3gw_request_latency_ms_sum " << sum_ms << "\n";
  oss << "s3gw_request_latency_ms_count " << count << "\n";

  oss << "# HELP s3gw_rocksdb_ops_total RocksDB operations.\n";
  oss << "# TYPE s3gw_rocksdb_ops_total counter\n";
  for (int i = 0; i < kRdbOpCount; ++i) {
    oss << "s3gw_rocksdb_ops_total{op=\"" << rocks_op_name(static_cast<RocksOpIndex>(i))
        << "\"} " << rdb_counts_[i].load() << "\n";
  }

  oss << "# HELP s3gw_rocksdb_errors_total RocksDB operations with non-OK status.\n";
  oss << "# TYPE s3gw_rocksdb_errors_total counter\n";
  for (int i = 0; i < kRdbOpCount; ++i) {
    oss << "s3gw_rocksdb_errors_total{op=\"" << rocks_op_name(static_cast<RocksOpIndex>(i))
        << "\"} " << rdb_err_counts_[i].load() << "\n";
  }

  oss << "# HELP s3gw_rocksdb_bytes_total RocksDB bytes read/written.\n";
  oss << "# TYPE s3gw_rocksdb_bytes_total counter\n";
  for (int i = 0; i < kRdbOpCount; ++i) {
    oss << "s3gw_rocksdb_bytes_total{op=\"" << rocks_op_name(static_cast<RocksOpIndex>(i))
        << "\"} " << rdb_bytes_[i].load() << "\n";
  }

  oss << "# HELP s3gw_rocksdb_latency_ms RocksDB operation latency in milliseconds.\n";
  oss << "# TYPE s3gw_rocksdb_latency_ms histogram\n";
  std::uint64_t rdb_cumulative = 0;
  for (std::size_t i = 0; i < buckets_ms_.size(); ++i) {
    rdb_cumulative += rdb_bucket_counts_[i].load();
    oss << "s3gw_rocksdb_latency_ms_bucket{le=\"" << buckets_ms_[i] << "\"} "
        << rdb_cumulative << "\n";
  }
  std::uint64_t rdb_count = rdb_latency_count_.load();
  oss << "s3gw_rocksdb_latency_ms_bucket{le=\"+Inf\"} " << rdb_count << "\n";
  double rdb_sum_ms = static_cast<double>(rdb_latency_sum_us_.load()) / 1000.0;
  oss << "s3gw_rocksdb_latency_ms_sum " << rdb_sum_ms << "\n";
  oss << "s3gw_rocksdb_latency_ms_count " << rdb_count << "\n";

  return oss.str();
}

} // namespace server
