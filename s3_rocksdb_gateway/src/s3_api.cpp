#include "s3_api.hpp"
#include "util.hpp"


#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace s3 {
namespace http = boost::beast::http;

static std::atomic<std::uint64_t> g_reqid{1};

static std::string new_request_id() {
  auto v = g_reqid.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream oss;
  oss << std::hex << v;
  return oss.str();
}

static std::string iso8601_gmt(std::int64_t epoch_seconds) {
  std::time_t t = static_cast<std::time_t>(epoch_seconds);
  std::tm gm{};
#if defined(_WIN32)
  gmtime_s(&gm, &t);
#else
  gmtime_r(&t, &gm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%S.000Z");
  return oss.str();
}

static std::string xml_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

struct ParsedTarget {
  std::string bucket;
  std::string key;
  std::string path;
  std::string query;
  std::vector<std::pair<std::string, std::string>> query_params;
};

static std::string host_without_port(std::string_view host) {
  size_t colon = host.find(':');
  if (colon == std::string_view::npos) return std::string(host);
  return std::string(host.substr(0, colon));
}

static std::optional<std::string> bucket_from_host(std::string_view host, std::string_view suffix) {
  if (suffix.empty()) return std::nullopt;
  std::string h = host_without_port(host);
  if (h.size() <= suffix.size()) return std::nullopt;
  if (h.compare(h.size() - suffix.size(), suffix.size(), suffix) != 0) return std::nullopt;
  size_t dotpos = h.size() - suffix.size();
  if (dotpos == 0 || h[dotpos - 1] != '.') return std::nullopt;
  std::string bucket = h.substr(0, dotpos - 1);
  if (bucket.empty()) return std::nullopt;
  return bucket;
}

static ParsedTarget parse_target(const Request& req, std::string_view vhost_suffix) {
  ParsedTarget pt;
  std::string_view target(req.target().data(), req.target().size());
  size_t q = target.find('?');
  std::string_view path = (q == std::string_view::npos) ? target : target.substr(0, q);
  std::string_view query = (q == std::string_view::npos) ? std::string_view{} : target.substr(q + 1);

  if (path.empty()) path = "/";
  pt.path = std::string(path);
  pt.query = std::string(query);
  pt.query_params = util::parse_query(query);

  std::string_view host = "";
  if (auto it = req.find(http::field::host); it != req.end()) {
    host = std::string_view(it->value().data(), it->value().size());
  }

  // Virtual-host style overrides path-style if it matches the configured suffix.
  if (auto bh = bucket_from_host(host, vhost_suffix)) {
    pt.bucket = *bh;
    std::string_view key_enc = path;
    if (!key_enc.empty() && key_enc.front() == '/') key_enc.remove_prefix(1);
    auto key_dec = util::percent_decode(key_enc);
    pt.key = key_dec ? *key_dec : std::string(key_enc);
    return pt;
  }

  // Path-style: /bucket or /bucket/key
  std::string_view p = path;
  if (!p.empty() && p.front() == '/') p.remove_prefix(1);
  if (p.empty()) return pt;

  size_t slash = p.find('/');
  std::string_view bucket_enc = (slash == std::string_view::npos) ? p : p.substr(0, slash);
  std::string_view key_enc = (slash == std::string_view::npos) ? std::string_view{} : p.substr(slash + 1);

  auto bdec = util::percent_decode(bucket_enc);
  pt.bucket = bdec ? *bdec : std::string(bucket_enc);
  auto kdec = util::percent_decode(key_enc);
  pt.key = kdec ? *kdec : std::string(key_enc);
  return pt;
}

static std::optional<std::string> qp_get(const ParsedTarget& pt, std::string_view key) {
  for (const auto& kv : pt.query_params) {
    if (kv.first == key) return kv.second;
  }
  return std::nullopt;
}

static Response make_xml_response(http::status st, std::string body_xml, bool keep_alive, unsigned version) {
  Response res{st, version};
  res.set(http::field::server, "s3_rocksdb_gateway");
  res.set(http::field::content_type, "application/xml");
  res.keep_alive(keep_alive);
  res.body().assign(body_xml.begin(), body_xml.end());
  res.content_length(res.body().size());
  return res;
}

static Response make_empty_response(http::status st, bool keep_alive, unsigned version) {
  Response res{st, version};
  res.set(http::field::server, "s3_rocksdb_gateway");
  res.keep_alive(keep_alive);
  res.content_length(0);
  return res;
}

static Response s3_error(http::status st,
                         std::string_view code,
                         std::string_view message,
                         std::string_view resource,
                         std::string_view request_id,
                         bool keep_alive,
                         unsigned version) {
  std::ostringstream oss;
  oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      << "<Error>"
      << "<Code>" << xml_escape(code) << "</Code>"
      << "<Message>" << xml_escape(message) << "</Message>"
      << "<Resource>" << xml_escape(resource) << "</Resource>"
      << "<RequestId>" << xml_escape(request_id) << "</RequestId>"
      << "</Error>";
  return make_xml_response(st, oss.str(), keep_alive, version);
}

static std::pair<http::status, std::string> map_storage_error(std::string_view err) {
  if (err == "NoSuchBucket") return {http::status::not_found, "NoSuchBucket"};
  if (err == "NoSuchKey") return {http::status::not_found, "NoSuchKey"};
  if (err == "BucketNotEmpty") return {http::status::conflict, "BucketNotEmpty"};
  if (err.rfind("Invalid", 0) == 0) return {http::status::bad_request, "InvalidRequest"};
  if (err.find("Invalid continuation-token") != std::string_view::npos) return {http::status::bad_request, "InvalidRequest"};
  return {http::status::internal_server_error, "InternalError"};
}

struct ByteRange {
  std::int64_t start = 0;
  std::int64_t end = 0; // inclusive
};

static std::optional<ByteRange> parse_single_range(std::string_view header_value, std::int64_t size) {
  // Only support a single range: bytes=start-end | bytes=start- | bytes=-suffix
  if (size <= 0) return std::nullopt;
  std::string normalized = util::trim_and_collapse_ws(header_value);
  std::string_view v = normalized;
  if (v.rfind("bytes=", 0) != 0) return std::nullopt;
  v.remove_prefix(6);
  if (v.find(',') != std::string_view::npos) return std::nullopt;

  auto dash = v.find('-');
  if (dash == std::string_view::npos) return std::nullopt;

  std::string_view left = v.substr(0, dash);
  std::string_view right = v.substr(dash + 1);

  auto parse_i64 = [](std::string_view s, std::int64_t* out) -> bool {
    if (s.empty()) return false;
    std::int64_t val = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc{}) return false;
    *out = val;
    return true;
  };

  ByteRange br{};
  if (left.empty()) {
    // bytes=-suffix
    std::int64_t suffix = 0;
    if (!parse_i64(right, &suffix)) return std::nullopt;
    if (suffix <= 0) return std::nullopt;
    if (suffix >= size) {
      br.start = 0;
      br.end = size > 0 ? size - 1 : 0;
      return br;
    }
    br.start = size - suffix;
    br.end = size - 1;
    return br;
  }

  if (!parse_i64(left, &br.start)) return std::nullopt;
  if (!right.empty()) {
    if (!parse_i64(right, &br.end)) return std::nullopt;
  } else {
    br.end = size > 0 ? size - 1 : 0;
  }

  if (br.start < 0 || br.start >= size) return std::nullopt;
  if (br.end < br.start) return std::nullopt;
  if (br.end >= size) br.end = size - 1;
  return br;
}

Api::Api(storage::RocksObjectStore* store, Config cfg) : store_(store), cfg_(std::move(cfg)) {}

Response Api::handle(const Request& req) {
  const std::string request_id = new_request_id();
  const bool keep_alive = req.keep_alive();
  const unsigned version = req.version();

  // Auth
  auto ar = auth::verify_sigv4(req, cfg_.auth_mode, cfg_.creds);
  if (!ar.ok) {
    return s3_error(http::status::forbidden,
                    ar.error_code.empty() ? "AccessDenied" : ar.error_code,
                    ar.error_message.empty() ? "Access denied" : ar.error_message,
                    std::string_view(req.target().data(), req.target().size()),
                    request_id,
                    keep_alive,
                    version);
  }

  // Size guard for PUT
  if (req.method() == http::verb::put && req.body().size() > cfg_.max_object_bytes) {
    return s3_error(http::status::payload_too_large,
                    "EntityTooLarge",
                    "Object too large",
                    std::string_view(req.target().data(), req.target().size()),
                    request_id,
                    keep_alive,
                    version);
  }

  ParsedTarget pt = parse_target(req, cfg_.virtual_host_suffix);

  // Root: ListBuckets
  if (pt.bucket.empty()) {
    if (req.method() != http::verb::get) {
      return s3_error(http::status::method_not_allowed,
                      "MethodNotAllowed",
                      "Unsupported method",
                      pt.path,
                      request_id,
                      keep_alive,
                      version);
    }
    std::string err;
    auto buckets = store_->list_buckets(&err);
    if (!err.empty()) {
      auto [st, code] = map_storage_error(err);
      return s3_error(st, code, err, pt.path, request_id, keep_alive, version);
    }

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        << "<Owner><ID></ID><DisplayName></DisplayName></Owner>"
        << "<Buckets>";
    const std::string now = iso8601_gmt(util::unix_now_seconds());
    for (const auto& b : buckets) {
      oss << "<Bucket><Name>" << xml_escape(b) << "</Name>"
          << "<CreationDate>" << now << "</CreationDate></Bucket>";
    }
    oss << "</Buckets></ListAllMyBucketsResult>";
    return make_xml_response(http::status::ok, oss.str(), keep_alive, version);
  }

  // Bucket-only operations
  const bool bucket_only = pt.bucket.size() > 0 && pt.key.empty();

  if (bucket_only) {
    if (req.method() == http::verb::put) {
      std::string err;
      if (!store_->create_bucket(pt.bucket, &err)) {
        auto [st, code] = map_storage_error(err);
        return s3_error(st, code, err, pt.path, request_id, keep_alive, version);
      }
      return make_empty_response(http::status::ok, keep_alive, version);
    }

    if (req.method() == http::verb::head) {
      std::string err;
      bool exists = store_->bucket_exists(pt.bucket, &err);
      if (!err.empty()) {
        auto [st, code] = map_storage_error(err);
        return s3_error(st, code, err, pt.path, request_id, keep_alive, version);
      }
      if (!exists) {
        return s3_error(http::status::not_found,
                        "NoSuchBucket",
                        "The specified bucket does not exist",
                        pt.path,
                        request_id,
                        keep_alive,
                        version);
      }
      return make_empty_response(http::status::ok, keep_alive, version);
    }

    if (req.method() == http::verb::delete_) {
      std::string err;
      if (!store_->delete_bucket(pt.bucket, &err)) {
        auto [st, code] = map_storage_error(err);
        std::string msg = (code == "BucketNotEmpty") ? "The bucket you tried to delete is not empty" : err;
        return s3_error(st, code, msg, pt.path, request_id, keep_alive, version);
      }
      return make_empty_response(http::status::no_content, keep_alive, version);
    }

    if (req.method() == http::verb::get) {
      // ListObjectsV2
      std::string err;
      std::string prefix = qp_get(pt, "prefix").value_or("");
      std::int64_t max_keys = 1000;
      if (auto mk = qp_get(pt, "max-keys")) {
        try { max_keys = std::stoll(*mk); } catch (...) { max_keys = 1000; }
      }
      std::string cont = qp_get(pt, "continuation-token").value_or("");

      auto lr = store_->list_objects_v2(pt.bucket, prefix, max_keys, cont, &err);
      if (!err.empty()) {
        auto [st, code] = map_storage_error(err);
        return s3_error(st, code, err, pt.path, request_id, keep_alive, version);
      }

      std::ostringstream oss;
      oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
          << "<Name>" << xml_escape(pt.bucket) << "</Name>"
          << "<Prefix>" << xml_escape(prefix) << "</Prefix>"
          << "<MaxKeys>" << max_keys << "</MaxKeys>"
          << "<KeyCount>" << lr.objects.size() << "</KeyCount>"
          << "<IsTruncated>" << (lr.is_truncated ? "true" : "false") << "</IsTruncated>";
      if (!cont.empty()) {
        oss << "<ContinuationToken>" << xml_escape(cont) << "</ContinuationToken>";
      }
      if (lr.is_truncated && !lr.next_continuation_token.empty()) {
        oss << "<NextContinuationToken>" << xml_escape(lr.next_continuation_token) << "</NextContinuationToken>";
      }

      for (const auto& obj : lr.objects) {
        oss << "<Contents>"
            << "<Key>" << xml_escape(obj.key) << "</Key>"
            << "<LastModified>" << iso8601_gmt(obj.meta.mtime) << "</LastModified>"
            << "<ETag>\"" << xml_escape(obj.meta.etag) << "\"</ETag>"
            << "<Size>" << obj.meta.size << "</Size>"
            << "<StorageClass>STANDARD</StorageClass>"
            << "</Contents>";
      }

      oss << "</ListBucketResult>";
      return make_xml_response(http::status::ok, oss.str(), keep_alive, version);
    }

    return s3_error(http::status::method_not_allowed,
                    "MethodNotAllowed",
                    "Unsupported method",
                    pt.path,
                    request_id,
                    keep_alive,
                    version);
  }

  // Object operations
  if (pt.key.empty()) {
    // This happens for path-style "/bucket" which we already handled.
    return s3_error(http::status::bad_request,
                    "InvalidRequest",
                    "Missing key",
                    pt.path,
                    request_id,
                    keep_alive,
                    version);
  }

  const std::string resource = pt.path;

  if (req.method() == http::verb::put) {
    std::string content_type = "application/octet-stream";
    if (auto it = req.find(http::field::content_type); it != req.end()) {
      content_type = std::string(it->value().data(), it->value().size());
    }

    storage::ObjectMeta meta;
    std::string err;
    std::string_view body(req.body().data(), req.body().size());
    if (!store_->put_object(pt.bucket, pt.key, body, content_type, &meta, &err)) {
      auto [st, code] = map_storage_error(err);
      return s3_error(st, code, err, resource, request_id, keep_alive, version);
    }

    Response res{http::status::ok, version};
    res.set(http::field::server, "s3_rocksdb_gateway");
    res.set("ETag", "\"" + meta.etag + "\"");
    res.keep_alive(keep_alive);
    res.content_length(0);
    return res;
  }

  if (req.method() == http::verb::get) {
    std::string data;
    storage::ObjectMeta meta;
    std::string err;
    if (!store_->get_object(pt.bucket, pt.key, &data, &meta, &err)) {
      auto [st, code] = map_storage_error(err);
      std::string msg = (code == "NoSuchKey") ? "The specified key does not exist" : err;
      return s3_error(st, code, msg, resource, request_id, keep_alive, version);
    }

    const std::int64_t size = static_cast<std::int64_t>(data.size());
    std::optional<ByteRange> range;
    if (auto it = req.find(http::field::range); it != req.end()) {
      range = parse_single_range(std::string_view(it->value().data(), it->value().size()), size);
      if (!range) {
        Response res = s3_error(http::status::range_not_satisfiable,
                                "InvalidRange",
                                "The requested range is not satisfiable",
                                resource,
                                request_id,
                                keep_alive,
                                version);
        res.set("Content-Range", "bytes */" + std::to_string(size));
        return res;
      }
    }

    Response res{range ? http::status::partial_content : http::status::ok, version};
    res.set(http::field::server, "s3_rocksdb_gateway");
    res.set(http::field::content_type, meta.content_type.empty() ? "application/octet-stream" : meta.content_type);
    res.set("ETag", "\"" + meta.etag + "\"");
    res.set(http::field::last_modified, util::rfc1123_gmt(meta.mtime));
    res.set("Accept-Ranges", "bytes");
    res.keep_alive(keep_alive);

    if (range) {
      const auto& r = *range;
      const std::size_t start = static_cast<std::size_t>(r.start);
      const std::size_t end = static_cast<std::size_t>(r.end);
      res.set("Content-Range",
              "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size));
      res.body().assign(data.begin() + start, data.begin() + end + 1);
    } else {
      res.body().assign(data.begin(), data.end());
    }
    res.content_length(res.body().size());
    return res;
  }

  if (req.method() == http::verb::head) {
    storage::ObjectMeta meta;
    std::string err;
    if (!store_->head_object(pt.bucket, pt.key, &meta, &err)) {
      auto [st, code] = map_storage_error(err);
      std::string msg = (code == "NoSuchKey") ? "The specified key does not exist" : err;
      return s3_error(st, code, msg, resource, request_id, keep_alive, version);
    }

    Response res{http::status::ok, version};
    res.set(http::field::server, "s3_rocksdb_gateway");
    res.set(http::field::content_type, meta.content_type.empty() ? "application/octet-stream" : meta.content_type);
    res.set("ETag", "\"" + meta.etag + "\"");
    res.set(http::field::last_modified, util::rfc1123_gmt(meta.mtime));
    res.set("Accept-Ranges", "bytes");
    res.keep_alive(keep_alive);
    res.content_length(static_cast<std::uint64_t>(meta.size));
    return res;
  }

  if (req.method() == http::verb::delete_) {
    std::string err;
    if (!store_->delete_object(pt.bucket, pt.key, &err)) {
      auto [st, code] = map_storage_error(err);
      return s3_error(st, code, err, resource, request_id, keep_alive, version);
    }
    return make_empty_response(http::status::no_content, keep_alive, version);
  }

  return s3_error(http::status::method_not_allowed,
                  "MethodNotAllowed",
                  "Unsupported method",
                  resource,
                  request_id,
                  keep_alive,
                  version);
}

} // namespace s3
