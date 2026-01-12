# s3_rocksdb_gateway

A small **S3-compatible** HTTP service that stores objects in **RocksDB**.

This project exists because the gRPC service you had before is *not* wire-compatible with S3. If your application speaks gRPC, swapping in a third‑party S3 appliance would require changing the client to HTTP S3.

With this gateway, your app can talk to `http(s)://<endpoint>` using standard S3 REST calls (AWS SDK / s3cmd / aws-cli pointed at a custom endpoint), and you can later swap the endpoint to a real S3 appliance without changing your app code.

## Architecture (gateway internals)

High-level flow:

```
HTTP (S3 REST) -> s3_api -> RocksObjectStore -> RocksDB
```

Details:
- **HTTP server**: Boost.Beast handles connections and request parsing.
- **S3 API layer**: parses bucket/key, validates auth, maps to storage calls.
- **Storage layer**: `RocksObjectStore` stores:
  - bucket marker: `B\0<bucket>`
  - object metadata: `M\0<bucket>\0<key>`
  - object data: `D\0<bucket>\0<key>`
- **Persistence**: single-node RocksDB (no replication). Objects are immutable.

This is a minimal S3-compatible facade so the prompt-cache stack can use
standard S3 APIs now and later swap to dedicated object storage.

## Supported S3 operations (subset)

This implements a pragmatic subset that most “object store” integrations need:

### Buckets
- `PUT /<bucket>` – CreateBucket
- `HEAD /<bucket>` – HeadBucket
- `DELETE /<bucket>` – DeleteBucket (only if empty)
- `GET /` – ListBuckets

### Objects
- `PUT /<bucket>/<key>` – PutObject
- `GET /<bucket>/<key>` – GetObject
- `HEAD /<bucket>/<key>` – HeadObject
- `DELETE /<bucket>/<key>` – DeleteObject
- `GET /<bucket>?list-type=2&prefix=...` – ListObjectsV2 (supports `max-keys` and a simple `continuation-token`)

Range reads:
- `GET /<bucket>/<key>` with `Range: bytes=start-end` (single range only)

It returns S3-style XML for list/error responses and common headers like `ETag`.

## Addressing style

- **Path-style**: `http://127.0.0.1:9000/mybucket/mykey`
- **Virtual-host-style (optional)**: `http://mybucket.s3.local/mykey`

Enable virtual-host-style by starting the server with:

```
--virtual_host_suffix=s3.local
```

## Authentication

Two modes:

- `--auth=none` – no authentication
- `--auth=sigv4` – verifies **AWS Signature Version 4** (Authorization header or presigned URL query)

Default is `--auth=none`. To enable SigV4, run with your own access/secret:

```
--auth=sigv4 --access_key=AKIDEXAMPLE --secret_key=YOURSECRET
```

## Build

### Using vcpkg (recommended)

```
vcpkg install rocksdb boost-program-options boost-system boost-thread openssl

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j8
```

### Using a local RocksDB build (no vcpkg)

This project requires **CMake >= 3.20** and a **RocksDB install** that provides
`RocksDBConfig.cmake`.

Build and install RocksDB:

```bash
cd /path/to/rocksdb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/rocksdb/install \
  -DWITH_GFLAGS=OFF
cmake --build build -j8
cmake --install build
```

Then build the gateway:

```bash
cd /path/to/prompt-cache-storage/s3_rocksdb_gateway
make build CMAKE_PREFIX_PATH=/path/to/rocksdb/install
```

### Using system packages (pkg-config)

If your distro provides `rocksdb-devel` with pkg-config files, no special prefix
is required:

```bash
dnf install -y rocksdb rocksdb-devel pkgconf-pkg-config
make build
```

## Run

```
./build/s3gw \
  --listen=0.0.0.0:9000 \
  --db_path=/var/lib/s3gw/rocksdb \
  --threads=8 \
  --cache_mb=1024 \
  --auth=none
```

## Metrics (Prometheus)

The gateway exposes a Prometheus-compatible endpoint:

```
curl http://127.0.0.1:9000/metrics
```

Metrics include:
- `s3gw_requests_total{method=...}`
- `s3gw_request_errors_total{method=...}`
- `s3gw_request_bytes_total{method=...}`
- `s3gw_response_bytes_total{method=...}`
- `s3gw_inflight_requests`
- `s3gw_request_latency_ms_bucket`, `_sum`, `_count`
- `s3gw_rocksdb_ops_total{op=...}`
- `s3gw_rocksdb_errors_total{op=...}`
- `s3gw_rocksdb_bytes_total{op=...}`
- `s3gw_rocksdb_latency_ms_bucket`, `_sum`, `_count`

## Stress Test

Basic load generator:

```bash
./tools/stress_test.py \
  --endpoint http://127.0.0.1:9000 \
  --bucket prompt-cache \
  --create-bucket \
  --objects 500 \
  --object-bytes 65536 \
  --range-bytes 16384 \
  --threads 8 \
  --duration 30
```

## Notes / limitations

- This is **not** a full S3 implementation. Multipart upload, ACLs, versioning, SSE, etc. are not implemented.
- `PUT` requires a `Content-Length` and buffers the request body in memory (simple + fast for small/medium objects).
- ListObjectsV2 continuation tokens are implementation-specific (base64 of last key).
