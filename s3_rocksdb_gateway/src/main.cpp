#include "http_server.hpp"
#include "s3_api.hpp"
#include "storage.hpp"

#include <boost/asio.hpp>
#include <boost/program_options.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace po = boost::program_options;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

static auth::Mode parse_auth_mode(const std::string& s) {
  if (s == "none") return auth::Mode::None;
  return auth::Mode::SigV4;
}

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:9000";
  std::string db_path = "./s3gw_rocksdb";
  int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  int cache_mb = 512;
  int max_object_mb = 64;
  std::string auth_mode_s = "none";
  std::string access_key = "AKIDEXAMPLE";
  std::string secret_key = "YOURSECRET";
  std::string vhost_suffix = "";
  bool disable_wal = false;
  bool sync = false;

  po::options_description desc("s3_rocksdb_gateway options");
  desc.add_options()
    ("help,h", "Show help")
    ("listen", po::value<std::string>(&listen)->default_value(listen), "Listen address host:port")
    ("db_path", po::value<std::string>(&db_path)->default_value(db_path), "RocksDB path")
    ("threads", po::value<int>(&threads)->default_value(threads), "Worker threads")
    ("cache_mb", po::value<int>(&cache_mb)->default_value(cache_mb), "RocksDB block cache (MiB)")
    ("max_object_mb", po::value<int>(&max_object_mb)->default_value(max_object_mb), "Max PUT object size (MiB)")
    ("auth", po::value<std::string>(&auth_mode_s)->default_value(auth_mode_s), "Auth mode: none | sigv4")
    ("access_key", po::value<std::string>(&access_key)->default_value(access_key), "SigV4 access key")
    ("secret_key", po::value<std::string>(&secret_key)->default_value(secret_key), "SigV4 secret key")
    ("virtual_host_suffix", po::value<std::string>(&vhost_suffix)->default_value(vhost_suffix), "Enable virtual-host style: bucket.<suffix>")
    ("disable_wal", po::bool_switch(&disable_wal)->default_value(disable_wal), "Disable RocksDB WAL (lower latency, weaker durability)")
    ("sync", po::bool_switch(&sync)->default_value(sync), "fsync on write (higher durability, higher latency)");

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (const std::exception& e) {
    std::cerr << "Argument error: " << e.what() << "\n\n" << desc << "\n";
    return 2;
  }

  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  // Parse listen
  auto colon = listen.rfind(':');
  if (colon == std::string::npos) {
    std::cerr << "--listen must be host:port\n";
    return 2;
  }
  std::string host = listen.substr(0, colon);
  int port_i = std::atoi(listen.substr(colon + 1).c_str());
  if (port_i <= 0 || port_i > 65535) {
    std::cerr << "Invalid port\n";
    return 2;
  }

  // RocksDB options (latency-oriented defaults)
  rocksdb::Options opt;
  opt.create_if_missing = true;
  opt.IncreaseParallelism();
  opt.OptimizeLevelStyleCompaction();

  rocksdb::BlockBasedTableOptions table;
  table.block_cache = rocksdb::NewLRUCache(static_cast<size_t>(cache_mb) * 1024u * 1024u);
  table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
  table.cache_index_and_filter_blocks = true;
  table.pin_l0_filter_and_index_blocks_in_cache = true;
  opt.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::DB* raw = nullptr;
  auto st = rocksdb::DB::Open(opt, db_path, &raw);
  if (!st.ok()) {
    std::cerr << "Failed to open RocksDB at " << db_path << ": " << st.ToString() << "\n";
    return 1;
  }
  db.reset(raw);

  rocksdb::WriteOptions wo;
  wo.disableWAL = disable_wal;
  wo.sync = sync;

  server::Metrics metrics;
  storage::RocksObjectStore store(db.get(), wo, &metrics);

  s3::Config s3cfg;
  s3cfg.auth_mode = parse_auth_mode(auth_mode_s);
  s3cfg.creds = auth::Credentials{access_key, secret_key};
  s3cfg.virtual_host_suffix = vhost_suffix;
  s3cfg.max_object_bytes = static_cast<size_t>(std::max(1, max_object_mb)) * 1024u * 1024u;

  s3::Api api(&store, s3cfg);

  asio::io_context ioc{static_cast<int>(std::max(1, threads))};

  tcp::endpoint endpoint{asio::ip::make_address(host), static_cast<unsigned short>(port_i)};
  server::Config scfg;
  scfg.listen_host = host;
  scfg.listen_port = static_cast<unsigned short>(port_i);
  scfg.max_request_body_bytes = s3cfg.max_object_bytes;
  scfg.metrics = &metrics;

  auto listener = std::make_shared<server::Listener>(ioc, endpoint, api, scfg);
  listener->run();

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&ioc]{ ioc.run(); });
  }

  for (auto& t : workers) t.join();
  return 0;
}
