#include "sigv4.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace auth {
namespace http = boost::beast::http;

static std::string to_lower(std::string_view s) {
  std::string out(s.begin(), s.end());
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
  return out;
}

static std::vector<std::string> split(std::string_view s, char delim) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(delim, start);
    if (pos == std::string_view::npos) pos = s.size();
    out.emplace_back(s.substr(start, pos - start));
    if (pos == s.size()) break;
    start = pos + 1;
  }
  return out;
}

static std::optional<std::string_view> header_value(const http::request<http::vector_body<char>>& req, std::string_view name) {
  auto it = req.find(name.data());
  if (it == req.end()) return std::nullopt;
  return std::string_view(it->value().data(), it->value().size());
}

static std::string canonical_uri(std::string_view path) {
  // SigV4 requires URI-encoding each path segment; preserve '/'
  // If the incoming path is already percent-encoded, decoding then encoding normalizes it.
  auto decoded = util::percent_decode(path);
  if (!decoded) {
    // Fall back to raw path
    return util::percent_encode(path, false);
  }
  return util::percent_encode(*decoded, false);
}

struct ParsedTarget {
  std::string path;  // includes leading '/'
  std::string query; // without '?'
};

static ParsedTarget parse_target(std::string_view target) {
  ParsedTarget pt;
  size_t q = target.find('?');
  if (q == std::string_view::npos) {
    pt.path = std::string(target);
    pt.query.clear();
  } else {
    pt.path = std::string(target.substr(0, q));
    pt.query = std::string(target.substr(q + 1));
  }
  if (pt.path.empty()) pt.path = "/";
  return pt;
}

static std::string canonical_headers(const http::request<http::vector_body<char>>& req,
                                     const std::vector<std::string>& signed_headers_lower,
                                     std::string* out_signed_headers_joined) {
  // Build canonical headers string in signed header order; header names must be lowercase.
  std::string ch;
  std::string sh;
  bool first = true;
  for (const auto& hname : signed_headers_lower) {
    auto it = req.find(hname);
    if (it == req.end()) {
      // Host can be synthesized from :authority? Not in HTTP/1.1. Require present.
      continue;
    }
    std::string v = util::trim_and_collapse_ws(std::string_view(it->value().data(), it->value().size()));
    ch += hname;
    ch += ':';
    ch += v;
    ch += '\n';

    if (!first) sh.push_back(';');
    first = false;
    sh += hname;
  }
  if (out_signed_headers_joined) *out_signed_headers_joined = sh;
  return ch;
}

static std::vector<std::uint8_t> derive_signing_key(std::string_view secret_key,
                                                    std::string_view yyyymmdd,
                                                    std::string_view region,
                                                    std::string_view service) {
  std::string ksecret = "AWS4";
  ksecret += secret_key;
  auto k_date = util::hmac_sha256(ksecret, yyyymmdd);
  auto k_region = util::hmac_sha256(k_date, region);
  auto k_service = util::hmac_sha256(k_region, service);
  auto k_signing = util::hmac_sha256(k_service, "aws4_request");
  return k_signing;
}

static Result fail(std::string code, std::string msg) {
  Result r;
  r.ok = false;
  r.error_code = std::move(code);
  r.error_message = std::move(msg);
  return r;
}

static Result ok() {
  Result r;
  r.ok = true;
  return r;
}

struct AuthHeader {
  std::string access_key;
  std::string date;
  std::string region;
  std::string service;
  std::string signed_headers; // original
  std::string signature;
  std::vector<std::string> signed_headers_list; // lower-case
  std::string amz_date;
  std::string payload_hash;
};

static std::optional<AuthHeader> parse_authorization_sigv4(const http::request<http::vector_body<char>>& req) {
  auto auth = header_value(req, "Authorization");
  if (!auth) return std::nullopt;

  std::string_view s = *auth;
  // Example:
  // AWS4-HMAC-SHA256 Credential=AKID/20240101/us-east-1/s3/aws4_request, SignedHeaders=host;x-amz-date, Signature=...
  const std::string_view prefix = "AWS4-HMAC-SHA256";
  if (s.substr(0, prefix.size()) != prefix) return std::nullopt;
  s.remove_prefix(prefix.size());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);

  std::map<std::string, std::string> kv;
  // Split by ','
  while (!s.empty()) {
    size_t comma = s.find(',');
    std::string_view part = (comma == std::string_view::npos) ? s : s.substr(0, comma);
    if (comma != std::string_view::npos) s.remove_prefix(comma + 1); else s = {};

    // trim
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);

    size_t eq = part.find('=');
    if (eq == std::string_view::npos) continue;
    std::string k(part.substr(0, eq));
    std::string v(part.substr(eq + 1));
    kv[k] = v;
  }

  AuthHeader ah;
  auto cred_it = kv.find("Credential");
  auto sh_it = kv.find("SignedHeaders");
  auto sig_it = kv.find("Signature");
  if (cred_it == kv.end() || sh_it == kv.end() || sig_it == kv.end()) return std::nullopt;

  // Credential scope: AKID/YYYYMMDD/REGION/SERVICE/aws4_request
  auto parts = split(cred_it->second, '/');
  if (parts.size() < 5) return std::nullopt;
  ah.access_key = parts[0];
  ah.date = parts[1];
  ah.region = parts[2];
  ah.service = parts[3];
  // parts[4] should be aws4_request

  ah.signed_headers = sh_it->second;
  ah.signature = sig_it->second;

  // SignedHeaders list lower-case
  auto sh_parts = split(ah.signed_headers, ';');
  ah.signed_headers_list.clear();
  for (auto& h : sh_parts) {
    ah.signed_headers_list.push_back(to_lower(h));
  }

  // Required amz date
  auto amz_date = header_value(req, "x-amz-date");
  if (!amz_date) return std::nullopt;
  ah.amz_date = std::string(*amz_date);

  auto payload = header_value(req, "x-amz-content-sha256");
  if (payload) {
    ah.payload_hash = std::string(*payload);
  } else {
    // Compute from body
    ah.payload_hash = util::sha256_hex(std::string_view(req.body().data(), req.body().size()));
  }

  return ah;
}

struct PresignedQuery {
  std::string access_key;
  std::string date;
  std::string region;
  std::string service;
  std::string signed_headers;
  std::string signature;
  std::string amz_date;
  std::string expires;
  std::vector<std::string> signed_headers_list;
};

static std::optional<std::string> query_get(const std::vector<std::pair<std::string, std::string>>& q,
                                            std::string_view k) {
  for (const auto& kv : q) {
    if (kv.first == k) return kv.second;
  }
  return std::nullopt;
}

static std::optional<PresignedQuery> parse_presigned(const http::request<http::vector_body<char>>& req) {
  auto pt = parse_target(std::string_view(req.target().data(), req.target().size()));
  auto q = util::parse_query(pt.query);

  auto alg = query_get(q, "X-Amz-Algorithm");
  auto cred = query_get(q, "X-Amz-Credential");
  auto date = query_get(q, "X-Amz-Date");
  auto exp = query_get(q, "X-Amz-Expires");
  auto sh = query_get(q, "X-Amz-SignedHeaders");
  auto sig = query_get(q, "X-Amz-Signature");

  if (!alg || !cred || !date || !exp || !sh || !sig) return std::nullopt;
  if (*alg != "AWS4-HMAC-SHA256") return std::nullopt;

  PresignedQuery pq;
  pq.amz_date = *date;
  pq.expires = *exp;
  pq.signed_headers = *sh;
  pq.signature = *sig;

  // Credential scope in query is URL-encoded already; parse_query decoded it.
  auto parts = split(*cred, '/');
  if (parts.size() < 5) return std::nullopt;
  pq.access_key = parts[0];
  pq.date = parts[1];
  pq.region = parts[2];
  pq.service = parts[3];

  auto sh_parts = split(pq.signed_headers, ';');
  for (auto& h : sh_parts) pq.signed_headers_list.push_back(to_lower(h));
  return pq;
}

static Result verify_with_parts(const http::request<http::vector_body<char>>& req,
                               const Credentials& creds,
                               std::string_view access_key,
                               std::string_view yyyymmdd,
                               std::string_view region,
                               std::string_view service,
                               std::string_view amz_date,
                               const std::vector<std::string>& signed_headers_lower,
                               std::string_view payload_hash,
                               std::string_view signature_hex,
                               bool presigned) {
  if (service != "s3") {
    return fail("InvalidRequest", "Credential scope service must be s3");
  }
  if (access_key != creds.access_key) {
    return fail("SignatureDoesNotMatch", "Unknown access key");
  }

  // Parse target
  auto pt = parse_target(std::string_view(req.target().data(), req.target().size()));
  const std::string can_uri = canonical_uri(pt.path);

  // Query
  auto params = util::parse_query(pt.query);
  std::string can_query;
  if (presigned) {
    // Exclude X-Amz-Signature when calculating canonical query
    can_query = util::canonical_query_string(params, std::optional<std::string_view>("X-Amz-Signature"));
  } else {
    can_query = util::canonical_query_string(params);
  }

  // Headers
  std::string signed_headers_joined;
  std::string can_headers = canonical_headers(req, signed_headers_lower, &signed_headers_joined);

  // Canonical request
  std::ostringstream cr;
  cr << req.method_string() << '\n'
     << can_uri << '\n'
     << can_query << '\n'
     << can_headers << '\n'
     << signed_headers_joined << '\n'
     << payload_hash;

  const std::string canonical_request = cr.str();
  const std::string canonical_request_hash = util::sha256_hex(canonical_request);

  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256" << '\n'
      << amz_date << '\n'
      << yyyymmdd << '/' << region << '/' << service << "/aws4_request" << '\n'
      << canonical_request_hash;

  const std::string string_to_sign = sts.str();

  auto signing_key = derive_signing_key(creds.secret_key, yyyymmdd, region, service);
  auto sig_bin = util::hmac_sha256(signing_key, string_to_sign);
  const std::string sig_hex = util::hex_lower(sig_bin);

  if (!util::constant_time_equal(sig_hex, signature_hex)) {
    return fail("SignatureDoesNotMatch", "The request signature we calculated does not match the signature you provided.");
  }

  return ok();
}

Result verify_sigv4(const http::request<http::vector_body<char>>& req, Mode mode, const Credentials& creds) {
  if (mode == Mode::None) return ok();

  // First, try Authorization header signing
  if (auto ah = parse_authorization_sigv4(req)) {
    // Reject unsupported payload signing modes (streaming chunk signatures etc.)
    const std::string_view ph = ah->payload_hash;
    if (ph == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD") {
      return fail("NotImplemented", "Streaming SigV4 payload signing is not implemented");
    }
    return verify_with_parts(req, creds,
                             ah->access_key, ah->date, ah->region, ah->service,
                             ah->amz_date, ah->signed_headers_list,
                             ah->payload_hash, ah->signature,
                             /*presigned=*/false);
  }

  // Then, presigned URL
  if (auto pq = parse_presigned(req)) {
    // For presigned URLs, SigV4 uses UNSIGNED-PAYLOAD
    const std::string payload_hash = "UNSIGNED-PAYLOAD";
    return verify_with_parts(req, creds,
                             pq->access_key, pq->date, pq->region, pq->service,
                             pq->amz_date, pq->signed_headers_list,
                             payload_hash, pq->signature,
                             /*presigned=*/true);
  }

  return fail("AccessDenied", "Missing or invalid authentication");
}

} // namespace auth
