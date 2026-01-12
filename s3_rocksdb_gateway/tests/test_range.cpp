#include "s3_api.hpp"
#include "storage.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace http = boost::beast::http;

static std::string make_tmp_dir() {
  std::string tmpl = "/tmp/s3gw_test_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  char* dir = mkdtemp(buf.data());
  if (!dir) return "/tmp/s3gw_test_fallback";
  return std::string(dir);
}

int main() {
  std::string dir = make_tmp_dir();
  std::filesystem::create_directories(dir);

  rocksdb::Options opts;
  opts.create_if_missing = true;
  rocksdb::DB* db = nullptr;
  auto st = rocksdb::DB::Open(opts, dir, &db);
  assert(st.ok());

  storage::RocksObjectStore store(db);
  std::string err;
  assert(store.create_bucket("pc", &err));

  storage::ObjectMeta meta;
  std::string payload = "ABCDEFGH";
  assert(store.put_object("pc", "obj", payload, "application/octet-stream", &meta, &err));

  s3::Config cfg;
  cfg.auth_mode = auth::Mode::None;
  s3::Api api(&store, cfg);

  http::request<http::vector_body<char>> req{http::verb::get, "/pc/obj", 11};
  req.set(http::field::host, "localhost");
  req.set(http::field::range, "bytes=0-3");

  auto res = api.handle(req);
  assert(res.result() == http::status::partial_content);
  assert(res.body().size() == 4);
  assert(std::string(res.body().begin(), res.body().end()) == "ABCD");
  assert(res.find("Content-Range") != res.end());

  http::request<http::vector_body<char>> bad_req{http::verb::get, "/pc/obj", 11};
  bad_req.set(http::field::host, "localhost");
  bad_req.set(http::field::range, "bytes=100-200");
  auto bad_res = api.handle(bad_req);
  assert(bad_res.result() == http::status::range_not_satisfiable);

  delete db;
  std::filesystem::remove_all(dir);

  std::cout << "test_range passed\n";
  return 0;
}
