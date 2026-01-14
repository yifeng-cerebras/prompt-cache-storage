#include "util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sstream>
#include <string>

namespace util {

std::int64_t unix_now_seconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string rfc1123_gmt(std::int64_t epoch_seconds) {
  std::time_t t = static_cast<std::time_t>(epoch_seconds);
  std::tm gm{};
#if defined(_WIN32)
  gmtime_s(&gm, &t);
#else
  gmtime_r(&t, &gm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&gm, "%a, %d %b %Y %H:%M:%S GMT");
  return oss.str();
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::optional<std::string> percent_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return std::nullopt;
      int hi = hex_value(in[i + 1]);
      int lo = hex_value(in[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

static bool is_unreserved(unsigned char c) {
  // RFC 3986 unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
  return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string percent_encode(std::string_view in, bool encode_slash) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (!encode_slash && c == '/') {
      out.push_back('/');
      continue;
    }
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query) {
  std::vector<std::pair<std::string, std::string>> out;
  if (query.empty()) return out;
  size_t start = 0;
  while (start <= query.size()) {
    size_t amp = query.find('&', start);
    if (amp == std::string_view::npos) amp = query.size();
    std::string_view part = query.substr(start, amp - start);
    if (!part.empty()) {
      size_t eq = part.find('=');
      std::string_view k = (eq == std::string_view::npos) ? part : part.substr(0, eq);
      std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : part.substr(eq + 1);
      auto kd = percent_decode(k);
      auto vd = percent_decode(v);
      if (!kd) kd = std::string(k);
      if (!vd) vd = std::string(v);
      out.emplace_back(std::move(*kd), std::move(*vd));
    }
    if (amp == query.size()) break;
    start = amp + 1;
  }
  return out;
}

std::string canonical_query_string(const std::vector<std::pair<std::string, std::string>>& params,
                                   std::optional<std::string_view> exclude_key) {
  std::vector<std::pair<std::string, std::string>> p;
  p.reserve(params.size());
  for (const auto& kv : params) {
    if (exclude_key && kv.first == *exclude_key) continue;
    p.push_back(kv);
  }
  std::sort(p.begin(), p.end(), [](auto& a, auto& b) {
    if (a.first < b.first) return true;
    if (a.first > b.first) return false;
    return a.second < b.second;
  });

  std::string out;
  bool first = true;
  for (const auto& kv : p) {
    if (!first) out.push_back('&');
    first = false;
    out += percent_encode(kv.first, true);
    out.push_back('=');
    out += percent_encode(kv.second, true);
  }
  return out;
}

std::string trim_and_collapse_ws(std::string_view s) {
  // Trim
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;

  std::string out;
  out.reserve(e - b);
  bool in_ws = false;
  for (size_t i = b; i < e; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c)) {
      if (!in_ws) {
        out.push_back(' ');
        in_ws = true;
      }
    } else {
      out.push_back(static_cast<char>(c));
      in_ws = false;
    }
  }
  return out;
}

static std::vector<std::uint8_t> digest_bin(const EVP_MD* md, std::string_view data) {
  std::vector<std::uint8_t> out;
  if (!md) return out;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return out;

  if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return out;
  }
  if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    return out;
  }

  unsigned int len = 0;
  int out_len = EVP_MD_size(md);
  if (out_len <= 0) {
    EVP_MD_CTX_free(ctx);
    return out;
  }
  out.resize(static_cast<size_t>(out_len));
  if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
    EVP_MD_CTX_free(ctx);
    out.clear();
    return out;
  }
  out.resize(static_cast<size_t>(len));
  EVP_MD_CTX_free(ctx);
  return out;
}

std::vector<std::uint8_t> sha256_bin(std::string_view data) {
  return digest_bin(EVP_sha256(), data);
}

std::string hex_lower(const std::vector<std::uint8_t>& bytes) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = hex[(bytes[i] >> 4) & 0xF];
    out[2 * i + 1] = hex[bytes[i] & 0xF];
  }
  return out;
}

std::string sha256_hex(std::string_view data) {
  return hex_lower(sha256_bin(data));
}

std::vector<std::uint8_t> hmac_sha256(const std::vector<std::uint8_t>& key, std::string_view data) {
  unsigned int len = EVP_MAX_MD_SIZE;
  std::vector<std::uint8_t> out(len);
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data(), &len);
  out.resize(len);
  return out;
}

std::vector<std::uint8_t> hmac_sha256(std::string_view key, std::string_view data) {
  return hmac_sha256(std::vector<std::uint8_t>(key.begin(), key.end()), data);
}

std::string md5_hex(std::string_view data) {
  auto v = digest_bin(EVP_md5(), data);
  return hex_lower(v);
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

std::string base64_encode(std::string_view in) {
  // EVP_EncodeBlock expects output length >= 4*ceil(n/3) + 1
  const auto in_len = static_cast<int>(in.size());
  const int out_len = 4 * ((in_len + 2) / 3);
  std::string out;
  out.resize(out_len);
  int wrote = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(in.data()), in_len);
  out.resize(wrote);
  return out;
}

std::optional<std::string> base64_decode(std::string_view in) {
  // EVP_DecodeBlock writes 3/4 of input length
  std::string out;
  out.resize((in.size() * 3) / 4 + 3);
  int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
  if (len < 0) return std::nullopt;

  // Remove padding influence
  size_t pad = 0;
  if (!in.empty() && in.back() == '=') pad++;
  if (in.size() >= 2 && in[in.size() - 2] == '=') pad++;

  out.resize(static_cast<size_t>(len) - pad);
  return out;
}

} // namespace util
