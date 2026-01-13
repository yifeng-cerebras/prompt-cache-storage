#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rocksdb/db.h>

namespace server {
class Metrics;
} // namespace server

namespace storage {

struct ObjectMeta {
  std::string etag;        // hex md5
  std::int64_t mtime = 0;  // epoch seconds
  std::int64_t size = 0;
  std::string content_type;
};

struct ListedObject {
  std::string key;
  ObjectMeta meta;
};

struct ListResult {
  std::vector<ListedObject> objects;
  bool is_truncated = false;
  std::string next_continuation_token;
};

class RocksObjectStore {
public:
  explicit RocksObjectStore(rocksdb::DB* db,
                            rocksdb::WriteOptions write_opts = rocksdb::WriteOptions{},
                            server::Metrics* metrics = nullptr);

  // Buckets
  bool bucket_exists(std::string_view bucket, std::string* err);
  bool create_bucket(std::string_view bucket, std::string* err);
  bool delete_bucket(std::string_view bucket, std::string* err);
  std::vector<std::string> list_buckets(std::string* err);

  // Objects
  bool put_object(std::string_view bucket, std::string_view key,
                  std::string_view data,
                  std::string_view content_type,
                  ObjectMeta* out_meta,
                  std::string* err);

  bool get_object(std::string_view bucket, std::string_view key,
                  std::string* out_data,
                  ObjectMeta* out_meta,
                  std::string* err);

  bool get_object_data(std::string_view bucket, std::string_view key,
                       std::string* out_data,
                       std::string* err);

  bool head_object(std::string_view bucket, std::string_view key,
                   ObjectMeta* out_meta,
                   std::string* err);

  bool delete_object(std::string_view bucket, std::string_view key,
                     std::string* err);

  ListResult list_objects_v2(std::string_view bucket,
                            std::string_view prefix,
                            std::int64_t max_keys,
                            std::string_view continuation_token,
                            std::string* err);

private:
  rocksdb::DB* db_;
  rocksdb::WriteOptions wo_;
  server::Metrics* metrics_;
};

} // namespace storage
