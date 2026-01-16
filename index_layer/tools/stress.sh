# create cgroup
cgcreate -g memory:/s3gw
echo $((64*1024*1024*1024)) > /sys/fs/cgroup/memory/s3gw/memory.limit_in_bytes
echo 0 > /sys/fs/cgroup/memory/s3gw/memory.swappiness

# run gateway
cd /n0/yifeng/s3_rocksdb_gateway
cgexec -g memory:/s3gw ./bin/s3gw \
  --listen=0.0.0.0:9000 \
  --db_path=/n0/s3gw_rocksdb \
  --threads=8 \
  --cache_mb=1 \
  --disable_wal \
  --auth=none &

cd /n0/yifeng/index_layer
LOG_DIR=/n0/yifeng/stress_logs
mkdir -p "$LOG_DIR"

ENDPOINT=http://127.0.0.1:9000
BUCKET=prompt-cache
PROMPT_LEN=64
BLOCK_SIZE=8
DURATION=30

OBJECTS_LIST="50000 100000"
BYTES_LIST="65536 131072 262144"
THREADS_LIST="4 8 16"

for o in $OBJECTS_LIST; do
  for b in $BYTES_LIST; do
    # Prefill once per dataset size (minimize extra load runs)
    ts=$(date +%Y%m%d_%H%M%S)
    prefill_log="$LOG_DIR/prefill_o${o}_b${b}_${ts}.txt"
    ./bin/stress_e2e \
      --endpoint "$ENDPOINT" \
      --bucket "$BUCKET" \
      --create-bucket \
      --objects "$o" \
      --object-bytes "$b" \
      --prompt-len "$PROMPT_LEN" \
      --block-size "$BLOCK_SIZE" \
      --threads 1 \
      --duration 1 \
      --prometheus | tee "$prefill_log"

    for t in $THREADS_LIST; do
      # cold-start window
      sync; echo 3 > /proc/sys/vm/drop_caches

      ts=$(date +%Y%m%d_%H%M%S)
      log="$LOG_DIR/run_o${o}_b${b}_t${t}_${ts}.txt"
      ./bin/stress_e2e \
        --endpoint "$ENDPOINT" \
        --bucket "$BUCKET" \
        --skip-prefill \
        --objects "$o" \
        --object-bytes "$b" \
        --prompt-len "$PROMPT_LEN" \
        --block-size "$BLOCK_SIZE" \
        --threads "$t" \
        --duration "$DURATION" \
        --prometheus | tee "$log"
    done
  done
done