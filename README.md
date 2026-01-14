# Prompt Cache Storage POC (C++)

This repo contains two components:
- `s3_rocksdb_gateway`: S3-compatible HTTP gateway backed by RocksDB.
- `index_layer`: PrefixMap + CLI + stress tool for end-to-end testing.

## Build Matrix (toolchains / environments)

### Gateway (s3_rocksdb_gateway)

**Option A: System packages with pkg-config (Rocky 8/9)**

```
dnf install -y rocksdb rocksdb-devel boost-devel openssl-devel pkgconf-pkg-config
cd /n0/yifeng/s3_rocksdb_gateway
make build
```

Notes:
- Uses pkg-config fallback when `RocksDBConfig.cmake` is not available.
- Outputs binaries to `bin/` (e.g., `bin/s3gw`).

**Option B: Local RocksDB install (CMake config)**

```
cd /path/to/rocksdb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/rocksdb/install \
  -DWITH_GFLAGS=OFF
cmake --build build -j
cmake --install build

cd /n0/yifeng/s3_rocksdb_gateway
make build CMAKE_PREFIX_PATH=/path/to/rocksdb/install
```

**Option C: vcpkg**

```
vcpkg install rocksdb boost-program-options boost-system boost-thread openssl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
```

### Index Layer (index_layer)

Dependencies:
- libcurl (and pthread)
- GCC 8+: Makefile auto-selects `-std=gnu++2a` for GCC < 9.

```
cd /n0/yifeng/index_layer
make clean
make
make stress
```

## Run: End-to-End Test

### 1) Start gateway

```
/n0/yifeng/s3_rocksdb_gateway/bin/s3gw \
  --listen=0.0.0.0:9000 \
  --db_path=/n0/s3gw_rocksdb \
  --threads=8 \
  --cache_mb=512 \
  --disable_wal \
  --auth=none
```

If you hit "Too many open files":
```
ulimit -n 1048576
```
then restart `s3gw` from the same shell.

### 2) Run stress

```
/n0/yifeng/index_layer/bin/stress_e2e \
  --endpoint http://127.0.0.1:9000 \
  --bucket prompt-cache \
  --create-bucket \
  --objects 500 \
  --prompt-len 64 \
  --block-size 8 \
  --object-bytes 65536 \
  --threads 8 \
  --duration 30 \
  --prometheus
```

Notes:
- Prefill prints progress and `prefill_ms`.
- Payload is random by default.
- Use `--skip-prefill` to reuse already-populated storage.

### 3) Metrics

Gateway:
```
curl http://127.0.0.1:9000/metrics
```

Index layer:
```
--prometheus
```

