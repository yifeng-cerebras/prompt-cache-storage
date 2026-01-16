#!/usr/bin/env bash
set -euo pipefail

DB_BENCH=${DB_BENCH:-/n0/rocksdb-6.26.1/db_bench}
FIO=${FIO:-fio}
LOG_ROOT=${LOG_ROOT:-/n0/bench_mp/logs_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG_ROOT"

# Toggle sections (set to 0 to skip).
RUN_FIO=${RUN_FIO:-1}
RUN_SEQ_WRITE=${RUN_SEQ_WRITE:-1}
RUN_SEQ_READ=${RUN_SEQ_READ:-1}
RUN_RAND_READ=${RUN_RAND_READ:-1}
RUN_RAND_WRITE=${RUN_RAND_WRITE:-1}
RUN_RAND_READ_2P=${RUN_RAND_READ_2P:-1}
RUN_RAND_WRITE_4P=${RUN_RAND_WRITE_4P:-1}

# Shared db_bench settings.
VALUE_SIZE=${VALUE_SIZE:-131072}

# FIO settings (adjust to taste; size is per job).
FIO_DIR=${FIO_DIR:-/n0/fio}
FIO_SIZE_GB=${FIO_SIZE_GB:-400}
FIO_RUNTIME=${FIO_RUNTIME:-60}
FIO_JOBS=${FIO_JOBS:-4}
FIO_IODEPTH=${FIO_IODEPTH:-32}

# DB paths.
DB_SEQ_WRITE=${DB_SEQ_WRITE:-/n0/bench3/rocksdb_seq_write}
DB_SEQ_READ=${DB_SEQ_READ:-/n0/bench_mp/seqwrite_p1}
DB_RAND_READ=${DB_RAND_READ:-/n0/bench2/rocksdb_rand}
DB_RAND_WRITE=${DB_RAND_WRITE:-/n0/bench3/rocksdb_rand_write}

if [[ "$RUN_FIO" -eq 1 ]]; then
  mkdir -p "$FIO_DIR"
  "$FIO" --name=seq_read --directory="$FIO_DIR" --rw=read --bs=1m \
    --size="${FIO_SIZE_GB}G" --numjobs="$FIO_JOBS" --iodepth="$FIO_IODEPTH" \
    --ioengine=libaio --direct=1 --time_based --runtime="$FIO_RUNTIME" \
    --group_reporting --output="$LOG_ROOT/fio_seq_read.txt"

  "$FIO" --name=seq_write --directory="$FIO_DIR" --rw=write --bs=1m \
    --size="${FIO_SIZE_GB}G" --numjobs="$FIO_JOBS" --iodepth="$FIO_IODEPTH" \
    --ioengine=libaio --direct=1 --time_based --runtime="$FIO_RUNTIME" \
    --group_reporting --output="$LOG_ROOT/fio_seq_write.txt"

  "$FIO" --name=rand_read_64k --directory="$FIO_DIR" --rw=randread --bs=64k \
    --size="${FIO_SIZE_GB}G" --numjobs="$FIO_JOBS" --iodepth="$FIO_IODEPTH" \
    --ioengine=libaio --direct=1 --time_based --runtime="$FIO_RUNTIME" \
    --group_reporting --output="$LOG_ROOT/fio_randread_64k.txt"

  "$FIO" --name=rand_write_64k --directory="$FIO_DIR" --rw=randwrite --bs=64k \
    --size="${FIO_SIZE_GB}G" --numjobs="$FIO_JOBS" --iodepth="$FIO_IODEPTH" \
    --ioengine=libaio --direct=1 --time_based --runtime="$FIO_RUNTIME" \
    --group_reporting --output="$LOG_ROOT/fio_randwrite_64k.txt"
fi

if [[ "$RUN_SEQ_WRITE" -eq 1 ]]; then
  "$DB_BENCH" \
    --benchmarks=fillseq \
    --db="$DB_SEQ_WRITE" \
    --num=1000000 \
    --value_size="$VALUE_SIZE" \
    --compression_type=none \
    --disable_wal=true \
    --use_direct_io_for_flush_and_compaction=true \
    --writable_file_max_buffer_size=1048576 \
    --max_background_jobs=32 \
    --subcompactions=8 \
    --write_buffer_size=536870912 \
    --max_write_buffer_number=8 \
    --max_bytes_for_level_base=8589934592 \
    --level0_file_num_compaction_trigger=8 \
    --level0_slowdown_writes_trigger=32 \
    --level0_stop_writes_trigger=64 \
    > "$LOG_ROOT/dbbench_fillseq.txt" 2>&1
fi

if [[ "$RUN_SEQ_READ" -eq 1 ]]; then
  "$DB_BENCH" \
    --benchmarks=readseq \
    --db="$DB_SEQ_READ" \
    --use_existing_db=true \
    --readonly=true \
    --duration=60 \
    --threads=1 \
    --value_size="$VALUE_SIZE" \
    --block_size=131072 \
    --readahead_size=$((4*1024*1024)) \
    --advise_random_on_open=false \
    --use_direct_reads=false \
    --open_files=-1 \
    --cache_index_and_filter_blocks=true \
    > "$LOG_ROOT/dbbench_readseq.txt" 2>&1
fi

if [[ "$RUN_RAND_READ" -eq 1 ]]; then
  "$DB_BENCH" \
    --benchmarks=readrandom \
    --db="$DB_RAND_READ" \
    --use_existing_db=true \
    --readonly=true \
    --duration=30 \
    --threads=8 \
    --value_size="$VALUE_SIZE" \
    --use_direct_reads=false \
    --open_files=-1 \
    --cache_size=$((200*1024*1024*1024)) \
    --block_size=16384 \
    --cache_index_and_filter_blocks=true \
    > "$LOG_ROOT/dbbench_readrandom.txt" 2>&1
fi

if [[ "$RUN_RAND_WRITE" -eq 1 ]]; then
  "$DB_BENCH" \
    --benchmarks=fillrandom \
    --db="$DB_RAND_WRITE" \
    --num=1000000 \
    --value_size="$VALUE_SIZE" \
    --compression_type=none \
    --disable_wal=true \
    --use_direct_io_for_flush_and_compaction=true \
    --writable_file_max_buffer_size=1048576 \
    --max_background_jobs=32 \
    --subcompactions=8 \
    --write_buffer_size=536870912 \
    --max_write_buffer_number=8 \
    --max_bytes_for_level_base=8589934592 \
    --level0_file_num_compaction_trigger=8 \
    --level0_slowdown_writes_trigger=32 \
    --level0_stop_writes_trigger=64 \
    > "$LOG_ROOT/dbbench_fillrandom.txt" 2>&1
fi

if [[ "$RUN_RAND_READ_2P" -eq 1 ]]; then
  for i in 1 2; do
    "$DB_BENCH" \
      --benchmarks=readrandom \
      --db="$DB_RAND_READ" \
      --use_existing_db=true \
      --readonly=true \
      --duration=30 \
      --threads=4 \
      --value_size="$VALUE_SIZE" \
      --use_direct_reads=false \
      --open_files=-1 \
      --cache_size=$((200*1024*1024*1024)) \
      --block_size=16384 \
      --cache_index_and_filter_blocks=true \
      > "$LOG_ROOT/dbbench_readrandom_p${i}.txt" 2>&1 &
  done
  wait
fi

if [[ "$RUN_RAND_WRITE_4P" -eq 1 ]]; then
  RW_DBROOT=${RW_DBROOT:-/n0/bench_mp/randwrite_best_4p}
  RW_NUM_PER_PROC=${RW_NUM_PER_PROC:-250000}
  for i in 1 2 3 4; do
    DB="${RW_DBROOT}_p${i}"
    rm -rf "$DB"
    "$DB_BENCH" \
      --benchmarks=fillrandom \
      --db="$DB" \
      --num="$RW_NUM_PER_PROC" \
      --value_size="$VALUE_SIZE" \
      --threads=4 \
      --compression_type=none \
      --disable_wal=true \
      --use_direct_io_for_flush_and_compaction=true \
      --writable_file_max_buffer_size=1048576 \
      --max_background_jobs=32 \
      --subcompactions=8 \
      --write_buffer_size=536870912 \
      --max_write_buffer_number=8 \
      --max_bytes_for_level_base=8589934592 \
      --level0_file_num_compaction_trigger=8 \
      --level0_slowdown_writes_trigger=32 \
      --level0_stop_writes_trigger=64 \
      > "$LOG_ROOT/dbbench_fillrandom_p${i}.txt" 2>&1 &
  done
  wait
fi

echo "Logs written to $LOG_ROOT"
