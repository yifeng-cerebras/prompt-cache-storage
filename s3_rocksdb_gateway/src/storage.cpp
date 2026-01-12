#include "storage.hpp"
#include "metrics.hpp"
#include "util.hpp"

#include <chrono>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace storage {

namespace {

using Clock = std::chrono::steady_clock;

void observe_rocksdb(server::Metrics* metrics,
                     std::string_view op,
                     const rocksdb::Status& st,
                     std::size_t bytes,
                     Clock::time_point start) {
  if (!metrics) return;
  auto end = Clock::now();
  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  bool ok = st.ok() || st.IsNotFound();
  metrics->ObserveRocksdb(op, ok, bytes, ms);
}

} // namespace

static bool contains_nul(std::string_view s) {
  return s.find('\0') != std::string_view::npos;
}

static std::string bucket_key(std::string_view bucket) {
  std::string k;
  k.reserve(2 + bucket.size());
  k.push_back('B');
  k.push_back('\0');
  k.append(bucket.data(), bucket.size());
  return k;
}

static std::string meta_prefix(std::string_view bucket) {
  std::string k;
  k.reserve(3 + bucket.size());
  k.push_back('M');
  k.push_back('\0');
  k.append(bucket.data(), bucket.size());
  k.push_back('\0');
  return k;
}

static std::string meta_key(std::string_view bucket, std::string_view key) {
  std::string k = meta_prefix(bucket);
  k.append(key.data(), key.size());
  return k;
}

static std::string data_key(std::string_view bucket, std::string_view key) {
  std::string k;
  k.reserve(3 + bucket.size() + key.size());
  k.push_back('D');
  k.push_back('\0');
  k.append(bucket.data(), bucket.size());
  k.push_back('\0');
  k.append(key.data(), key.size());
  return k;
}

static std::string encode_meta(const ObjectMeta& m) {
  // size\0mtime\0etag\0content_type
  std::string out;
  out.reserve(64 + m.etag.size() + m.content_type.size());
  out += std::to_string(m.size);
  out.push_back('\0');
  out += std::to_string(m.mtime);
  out.push_back('\0');
  out += m.etag;
  out.push_back('\0');
  out += m.content_type;
  return out;
}

static std::optional<ObjectMeta> decode_meta(std::string_view v) {
  ObjectMeta m;
  size_t p1 = v.find('\0');
  if (p1 == std::string_view::npos) return std::nullopt;
  size_t p2 = v.find('\0', p1 + 1);
  if (p2 == std::string_view::npos) return std::nullopt;
  size_t p3 = v.find('\0', p2 + 1);
  if (p3 == std::string_view::npos) return std::nullopt;

  auto parse_i64 = [](std::string_view s, std::int64_t* out) -> bool {
    std::int64_t val = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc{}) return false;
    *out = val;
    return true;
  };

  if (!parse_i64(v.substr(0, p1), &m.size)) return std::nullopt;
  if (!parse_i64(v.substr(p1 + 1, p2 - (p1 + 1)), &m.mtime)) return std::nullopt;

  m.etag = std::string(v.substr(p2 + 1, p3 - (p2 + 1)));
  m.content_type = std::string(v.substr(p3 + 1));
  return m;
}

RocksObjectStore::RocksObjectStore(rocksdb::DB* db,
                                   rocksdb::WriteOptions write_opts,
                                   server::Metrics* metrics)
    : db_(db), wo_(write_opts), metrics_(metrics) {}

bool RocksObjectStore::bucket_exists(std::string_view bucket, std::string* err) {
  if (contains_nul(bucket)) {
    if (err) *err = "Invalid bucket";
    return false;
  }
  std::string value;
  auto start = Clock::now();
  auto st = db_->Get(rocksdb::ReadOptions{}, bucket_key(bucket), &value);
  observe_rocksdb(metrics_, "get", st, value.size(), start);
  if (st.ok()) return true;
  if (st.IsNotFound()) return false;
  if (err) *err = st.ToString();
  return false;
}

bool RocksObjectStore::create_bucket(std::string_view bucket, std::string* err) {
  if (contains_nul(bucket)) {
    if (err) *err = "Invalid bucket";
    return false;
  }
  // Idempotent
  std::string value;
  auto start = Clock::now();
  auto st = db_->Get(rocksdb::ReadOptions{}, bucket_key(bucket), &value);
  observe_rocksdb(metrics_, "get", st, value.size(), start);
  if (st.ok()) return true;
  if (!st.IsNotFound() && !st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }
  start = Clock::now();
  st = db_->Put(wo_, bucket_key(bucket), "");
  observe_rocksdb(metrics_, "put", st, 0, start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }
  return true;
}

std::vector<std::string> RocksObjectStore::list_buckets(std::string* err) {
  std::vector<std::string> out;
  rocksdb::ReadOptions ro;
  auto start = Clock::now();
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

  const std::string prefix = std::string("B\0", 2);
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    auto k = it->key();
    std::string_view ks(k.data(), k.size());
    if (ks.rfind(prefix, 0) != 0) break;
    out.emplace_back(ks.substr(prefix.size()));
  }
  auto st = it->status();
  observe_rocksdb(metrics_, "iter", st, 0, start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
  }
  return out;
}

bool RocksObjectStore::delete_bucket(std::string_view bucket, std::string* err) {
  if (contains_nul(bucket)) {
    if (err) *err = "Invalid bucket";
    return false;
  }
  // ensure exists
  if (!bucket_exists(bucket, err)) {
    if (err && err->empty()) *err = "NoSuchBucket";
    return false;
  }

  // check empty: any meta key with this bucket?
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));
  const std::string mp = meta_prefix(bucket);
  it->Seek(mp);
  if (it->Valid()) {
    auto k = it->key();
    std::string_view ks(k.data(), k.size());
    if (ks.rfind(mp, 0) == 0) {
      if (err) *err = "BucketNotEmpty";
      return false;
    }
  }
  if (!it->status().ok()) {
    if (err) *err = it->status().ToString();
    return false;
  }

  auto start = Clock::now();
  auto st = db_->Delete(wo_, bucket_key(bucket));
  observe_rocksdb(metrics_, "delete", st, 0, start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }
  return true;
}

bool RocksObjectStore::put_object(std::string_view bucket, std::string_view key,
                                 std::string_view data,
                                 std::string_view content_type,
                                 ObjectMeta* out_meta,
                                 std::string* err) {
  if (contains_nul(bucket) || contains_nul(key)) {
    if (err) *err = "Invalid bucket/key";
    return false;
  }
  if (!bucket_exists(bucket, err)) {
    if (err && err->empty()) *err = "NoSuchBucket";
    return false;
  }

  ObjectMeta m;
  m.size = static_cast<std::int64_t>(data.size());
  m.mtime = util::unix_now_seconds();
  m.etag = util::md5_hex(data);
  m.content_type = content_type.empty() ? "application/octet-stream" : std::string(content_type);

  rocksdb::WriteBatch batch;
  batch.Put(data_key(bucket, key), rocksdb::Slice(data.data(), data.size()));
  const std::string meta_val = encode_meta(m);
  batch.Put(meta_key(bucket, key), meta_val);

  auto start = Clock::now();
  auto st = db_->Write(wo_, &batch);
  observe_rocksdb(metrics_, "write", st, data.size(), start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }

  if (out_meta) *out_meta = m;
  return true;
}

bool RocksObjectStore::head_object(std::string_view bucket, std::string_view key,
                                  ObjectMeta* out_meta,
                                  std::string* err) {
  if (contains_nul(bucket) || contains_nul(key)) {
    if (err) *err = "Invalid bucket/key";
    return false;
  }
  if (!bucket_exists(bucket, err)) {
    if (err && err->empty()) *err = "NoSuchBucket";
    return false;
  }

  std::string meta_val;
  auto start = Clock::now();
  auto st = db_->Get(rocksdb::ReadOptions{}, meta_key(bucket, key), &meta_val);
  observe_rocksdb(metrics_, "get", st, meta_val.size(), start);
  if (st.IsNotFound()) {
    if (err) *err = "NoSuchKey";
    return false;
  }
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }
  auto m = decode_meta(meta_val);
  if (!m) {
    if (err) *err = "Corrupt metadata";
    return false;
  }
  if (out_meta) *out_meta = *m;
  return true;
}

bool RocksObjectStore::get_object(std::string_view bucket, std::string_view key,
                                 std::string* out_data,
                                 ObjectMeta* out_meta,
                                 std::string* err) {
  ObjectMeta m;
  if (!head_object(bucket, key, &m, err)) return false;

  std::string data_val;
  auto start = Clock::now();
  auto st = db_->Get(rocksdb::ReadOptions{}, data_key(bucket, key), &data_val);
  observe_rocksdb(metrics_, "get", st, data_val.size(), start);
  if (st.IsNotFound()) {
    if (err) *err = "NoSuchKey";
    return false;
  }
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }

  if (out_data) *out_data = std::move(data_val);
  if (out_meta) *out_meta = std::move(m);
  return true;
}

bool RocksObjectStore::delete_object(std::string_view bucket, std::string_view key,
                                    std::string* err) {
  if (contains_nul(bucket) || contains_nul(key)) {
    if (err) *err = "Invalid bucket/key";
    return false;
  }
  if (!bucket_exists(bucket, err)) {
    if (err && err->empty()) *err = "NoSuchBucket";
    return false;
  }
  rocksdb::WriteBatch batch;
  batch.Delete(meta_key(bucket, key));
  batch.Delete(data_key(bucket, key));
  auto start = Clock::now();
  auto st = db_->Write(wo_, &batch);
  observe_rocksdb(metrics_, "write", st, 0, start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
    return false;
  }
  return true;
}

ListResult RocksObjectStore::list_objects_v2(std::string_view bucket,
                                            std::string_view prefix,
                                            std::int64_t max_keys,
                                            std::string_view continuation_token,
                                            std::string* err) {
  ListResult res;
  if (contains_nul(bucket) || contains_nul(prefix) || contains_nul(continuation_token)) {
    if (err) *err = "Invalid bucket/prefix/token";
    return res;
  }
  if (!bucket_exists(bucket, err)) {
    if (err && err->empty()) *err = "NoSuchBucket";
    return res;
  }

  if (max_keys <= 0) max_keys = 1000;
  if (max_keys > 1000) max_keys = 1000;

  const std::string mp = meta_prefix(bucket);

  std::string seek_key;
  if (!continuation_token.empty()) {
    auto decoded = util::base64_decode(continuation_token);
    if (!decoded) {
      if (err) *err = "Invalid continuation-token";
      return res;
    }
    seek_key = *decoded;
  } else {
    seek_key = mp;
    seek_key.append(prefix.data(), prefix.size());
  }

  rocksdb::ReadOptions ro;
  auto start = Clock::now();
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));
  it->Seek(seek_key);

  // If continuation token points at an existing key, start *after* it.
  if (!continuation_token.empty() && it->Valid()) {
    auto k = it->key();
    std::string_view ks(k.data(), k.size());
    if (ks == seek_key) it->Next();
  }

  std::int64_t count = 0;
  std::string last_meta_key;
  for (; it->Valid(); it->Next()) {
    auto k = it->key();
    std::string_view ks(k.data(), k.size());
    if (ks.rfind(mp, 0) != 0) break;

    std::string_view obj_key = ks.substr(mp.size());
    if (!prefix.empty()) {
      if (obj_key.rfind(prefix, 0) != 0) {
        // Since we sought to mp+prefix, once we don't match prefix we can stop.
        break;
      }
    }

    auto v = it->value();
    std::string_view vs(v.data(), v.size());
    auto meta = decode_meta(vs);
    if (!meta) continue;

    res.objects.push_back(ListedObject{std::string(obj_key), *meta});
    last_meta_key.assign(ks.data(), ks.size());

    count++;
    if (count >= max_keys) {
      // see if there is another matching key
      auto it2 = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));
      it2->Seek(last_meta_key);
      if (it2->Valid()) it2->Next();
      if (it2->Valid()) {
        auto k2 = it2->key();
        std::string_view ks2(k2.data(), k2.size());
        if (ks2.rfind(mp, 0) == 0) {
          std::string_view obj_key2 = ks2.substr(mp.size());
          if (prefix.empty() || obj_key2.rfind(prefix, 0) == 0) {
            res.is_truncated = true;
            res.next_continuation_token = util::base64_encode(last_meta_key);
          }
        }
      }
      break;
    }
  }

  auto st = it->status();
  observe_rocksdb(metrics_, "iter", st, 0, start);
  if (!st.ok()) {
    if (err) *err = st.ToString();
  }

  return res;
}

} // namespace storage
