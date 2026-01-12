#pragma once

#include "sigv4.hpp"
#include "storage.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <boost/beast/http.hpp>

namespace s3 {

namespace http = boost::beast::http;

struct Config {
  auth::Mode auth_mode = auth::Mode::None;
  auth::Credentials creds{};
  std::string virtual_host_suffix; // e.g. "s3.local"
  std::size_t max_object_bytes = 64u * 1024u * 1024u; // 64 MiB
};

using Request = http::request<http::vector_body<char>>;
using Response = http::response<http::vector_body<char>>;

class Api {
public:
  Api(storage::RocksObjectStore* store, Config cfg);

  Response handle(const Request& req);

private:
  storage::RocksObjectStore* store_;
  Config cfg_;
};

} // namespace s3
