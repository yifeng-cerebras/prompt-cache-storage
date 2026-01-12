#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/beast/http.hpp>

namespace auth {

namespace http = boost::beast::http;

struct Credentials {
  std::string access_key;
  std::string secret_key;
};

enum class Mode {
  None,
  SigV4
};

struct Result {
  bool ok = false;
  std::string error_code;
  std::string error_message;
};

// Verify AWS Signature Version 4.
// Supports:
//  - Authorization header signing
//  - Presigned URL query signing
//
// If mode == None, always returns ok.
Result verify_sigv4(const http::request<http::vector_body<char>>& req,
                   Mode mode,
                   const Credentials& creds);

} // namespace auth
