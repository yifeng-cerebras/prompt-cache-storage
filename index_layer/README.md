# Index Layer (L2) POC (C++ CLI)

Minimal C++ POC for the **index layer** (Layer 2). It implements memory-only
hit decisions via a PrefixMap and exposes a CLI for store/lookup/load to
exercise the logic. Storage is external (S3 gateway).

Role in the architecture:
- **Hit/miss decision** from PrefixMap only (no storage calls).
- **Object metadata** tracking (advisory, not authoritative).
- **Interfaces** for lookup/load/store that will later talk to the S3 gateway.

## Build

```bash
cd /Users/yifeng/ws/prompt-cache-storage/index_layer
make
```

Dependencies:
- C++20 compiler (GCC 8 uses `-std=gnu++2a` fallback)
- libcurl (required)

## CLI

```bash
# store (S3 gateway required)
./bin/prompt_cache_poc store \
  --tokens A,B,C,D \
  --data-file /tmp/data.bin \
  --owner replica-1 \
  --priority 1 \
  --s3-endpoint http://127.0.0.1:9000 \
  --s3-bucket prompt-cache

# lookup
./bin/prompt_cache_poc lookup --tokens A,B,C,D

# load
./bin/prompt_cache_poc load --obj-id <id> --usable-len 32 --out-file /tmp/out.bin

# stats
./bin/prompt_cache_poc stats
```

### Using the S3 gateway

The S3 gateway is required. Use `--s3-endpoint` and `--s3-bucket` for all commands.
Add `--s3-create-bucket` once if the bucket does not exist.

## Tests

```bash
make test
```

Optional S3 integration test:
- Set `S3_ENDPOINT` and `S3_BUCKET` to run `test_s3_integration`.
- Set `S3_CREATE_BUCKET=1` to create the bucket before the test.

## End-to-End Stress Test

Build the stress tool:

```bash
make stress
```

Run (requires gateway running):

```bash
./bin/stress_e2e \
  --endpoint http://127.0.0.1:9000 \
  --bucket prompt-cache \
  --create-bucket \
  --objects 500 \
  --prompt-len 64 \
  --block-size 8 \
  --object-bytes 65536 \
  --threads 8 \
  --duration 30
```

Prometheus output (one-shot snapshot to stdout):

```bash
./bin/stress_e2e \
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
- Prefix hashing uses a simple string hash over token lists.
- No etcd integration yet.
- Storage is external only; in-memory storage has been removed.
- Pair with `../s3_rocksdb_gateway`.
- Stress tool prefill data is random by default (to avoid unrealistic cache/compression effects).
- Use `--skip-prefill` to avoid PUTs (assumes objects already exist in storage).
- Prefill logs progress and prints `prefill_ms` once complete.
- Stress tool auto-fetches `GET /metrics` from the gateway and prints it at the end.
