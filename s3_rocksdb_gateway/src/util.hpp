#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace util {

// RFC 1123 date, e.g. "Wed, 21 Oct 2015 07:28:00 GMT"
std::string rfc1123_gmt(std::int64_t epoch_seconds);
std::int64_t unix_now_seconds();

// Percent-decode (URL decoding). Returns std::nullopt on malformed encoding.
std::optional<std::string> percent_decode(std::string_view in);

// Percent-encode for SigV4 canonicalization.
// If encode_slash is false, '/' is left as-is.
std::string percent_encode(std::string_view in, bool encode_slash);

// Parse query string "a=b&c=d" into vector of (k,v). Decodes percent-encoding.
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query);

// Build canonical query string for SigV4: sort by key then value; percent-encode.
std::string canonical_query_string(const std::vector<std::pair<std::string, std::string>>& params,
                                   std::optional<std::string_view> exclude_key = std::nullopt);

// Trim and normalize spaces per SigV4 canonical header rules.
std::string trim_and_collapse_ws(std::string_view s);

// Cryptography helpers
std::string sha256_hex(std::string_view data);
std::vector<std::uint8_t> sha256_bin(std::string_view data);

std::vector<std::uint8_t> hmac_sha256(const std::vector<std::uint8_t>& key, std::string_view data);
std::vector<std::uint8_t> hmac_sha256(std::string_view key, std::string_view data);

std::string hex_lower(const std::vector<std::uint8_t>& bytes);

std::string md5_hex(std::string_view data);

bool constant_time_equal(std::string_view a, std::string_view b);

// Base64 for continuation tokens
std::string base64_encode(std::string_view in);
std::optional<std::string> base64_decode(std::string_view in);

} // namespace util
