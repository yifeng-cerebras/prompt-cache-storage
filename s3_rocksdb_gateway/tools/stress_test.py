#!/usr/bin/env python3
import argparse
import os
import random
import ssl
import sys
import threading
import time
from dataclasses import dataclass
from http.client import HTTPConnection, HTTPSConnection
from urllib.parse import urlparse


@dataclass
class Result:
    count: int = 0
    errors: int = 0
    bytes_read: int = 0
    latencies_ms: list = None

    def __post_init__(self):
        if self.latencies_ms is None:
            self.latencies_ms = []


def make_connection(parsed, insecure):
    if parsed.scheme == "https":
        ctx = ssl._create_unverified_context() if insecure else ssl.create_default_context()
        return HTTPSConnection(parsed.hostname, parsed.port or 443, context=ctx)
    return HTTPConnection(parsed.hostname, parsed.port or 80)


def request(conn, method, path, body=None, headers=None):
    if headers is None:
        headers = {}
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    return resp.status, data


def ensure_bucket(conn, bucket):
    status, _ = request(conn, "PUT", f"/{bucket}")
    return status in (200, 204)


def put_object(conn, bucket, key, payload):
    headers = {"Content-Type": "application/octet-stream"}
    status, _ = request(conn, "PUT", f"/{bucket}/{key}", body=payload, headers=headers)
    return 200 <= status < 300


def get_range(conn, bucket, key, range_bytes):
    headers = {}
    if range_bytes > 0:
        headers["Range"] = f"bytes=0-{range_bytes - 1}"
    status, data = request(conn, "GET", f"/{bucket}/{key}", headers=headers)
    return status, data


def delete_object(conn, bucket, key):
    status, _ = request(conn, "DELETE", f"/{bucket}/{key}")
    return 200 <= status < 300


def percentile(sorted_vals, pct):
    if not sorted_vals:
        return 0.0
    idx = int((pct / 100.0) * (len(sorted_vals) - 1))
    return sorted_vals[idx]


def worker(parsed, args, object_keys, result):
    conn = make_connection(parsed, args.insecure)
    end_time = time.time() + args.duration
    local_lat = []
    count = 0
    errors = 0
    bytes_read = 0

    while time.time() < end_time:
        key = random.choice(object_keys)
        start = time.perf_counter()
        status, data = get_range(conn, args.bucket, key, args.range_bytes)
        elapsed = (time.perf_counter() - start) * 1000.0
        if status not in (200, 206):
            errors += 1
        else:
            bytes_read += len(data)
        local_lat.append(elapsed)
        count += 1

        # Optional writes
        if args.write_ratio > 0 and random.random() < args.write_ratio:
            write_key = f"{key}-w{count}"
            payload = os.urandom(args.object_bytes)
            if not put_object(conn, args.bucket, write_key, payload):
                errors += 1

    conn.close()

    result.count += count
    result.errors += errors
    result.bytes_read += bytes_read
    result.latencies_ms.extend(local_lat)


def main():
    parser = argparse.ArgumentParser(description="S3 gateway stress test")
    parser.add_argument("--endpoint", required=True, help="http://host:port")
    parser.add_argument("--bucket", default="prompt-cache")
    parser.add_argument("--create-bucket", action="store_true")
    parser.add_argument("--objects", type=int, default=100)
    parser.add_argument("--object-bytes", type=int, default=65536)
    parser.add_argument("--range-bytes", type=int, default=16384)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--write-ratio", type=float, default=0.0)
    parser.add_argument("--random", action="store_true", help="randomize payloads")
    parser.add_argument("--insecure", action="store_true", help="disable TLS verification")
    args = parser.parse_args()

    parsed = urlparse(args.endpoint)
    if parsed.scheme not in ("http", "https"):
        print("endpoint must be http:// or https://", file=sys.stderr)
        return 2

    conn = make_connection(parsed, args.insecure)
    if args.create_bucket:
        if not ensure_bucket(conn, args.bucket):
            print("failed to create bucket", file=sys.stderr)
            return 1
    conn.close()

    # Preload objects
    object_keys = []
    for i in range(args.objects):
        key = f"obj-{i:06d}"
        if args.random:
            payload = os.urandom(args.object_bytes)
        else:
            payload = (bytes([i % 256]) * args.object_bytes)
        conn = make_connection(parsed, args.insecure)
        ok = put_object(conn, args.bucket, key, payload)
        conn.close()
        if not ok:
            print(f"failed to put {key}", file=sys.stderr)
            return 1
        object_keys.append(key)

    # Run load
    result = Result()
    threads = []
    for _ in range(args.threads):
        t = threading.Thread(target=worker, args=(parsed, args, object_keys, result))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    total = result.count
    duration = float(args.duration)
    qps = total / duration if duration > 0 else 0.0
    mbps = (result.bytes_read / (1024.0 * 1024.0)) / duration if duration > 0 else 0.0

    result.latencies_ms.sort()
    p50 = percentile(result.latencies_ms, 50)
    p95 = percentile(result.latencies_ms, 95)
    p99 = percentile(result.latencies_ms, 99)

    print("requests", total)
    print("errors", result.errors)
    print("qps", f"{qps:.2f}")
    print("throughput_mb_s", f"{mbps:.2f}")
    print("p50_ms", f"{p50:.2f}")
    print("p95_ms", f"{p95:.2f}")
    print("p99_ms", f"{p99:.2f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
