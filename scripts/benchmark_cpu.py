#!/usr/bin/env python3
"""Measure deterministic CPU workloads for native and VM comparisons.

Usage: python3 scripts/benchmark_cpu.py --samples 30 --warmups 3 > results.json
Run sequentially on each system with matching Python and OpenSSL versions.
The four-worker SHA test hashes 1 GiB per sample; worker startup is excluded
from measured durations. Nearest-rank percentiles are not confidence intervals.
"""

import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
import math
import multiprocessing
import os
import platform
import ssl
import statistics
import sys
import time


PAYLOAD = bytes(range(256)) * 131072  # 32 MiB
EXPECTED_SHA256 = "e09320c5b00b34bb704802136c599a95b3996332ba84d7c7f21112b6231b6bd0"
EXPECTED_INTEGER = "0xd50a99767a4874ce"
MASK64 = (1 << 64) - 1
PARALLEL_WORKERS = 4
SHA256_REPEATS_PER_WORKER = 8


def sha256():
    return hashlib.sha256(PAYLOAD).hexdigest()


def integer_mix():
    value = 0x123456789abcdef0
    for _ in range(400000):
        value ^= (value << 13) & MASK64
        value ^= value >> 7
        value ^= (value << 17) & MASK64
    return hex(value)


def sha256_repeats(repetitions):
    result = None
    for _ in range(repetitions):
        result = sha256()
    return result


def measure(workload, expected, samples, warmups):
    for _ in range(warmups):
        if workload() != expected:
            raise RuntimeError("Workload checksum mismatch during warmup")

    measurements = []
    for _ in range(samples):
        start = time.perf_counter_ns()
        result = workload()
        duration_ms = (time.perf_counter_ns() - start) / 1_000_000
        if result != expected:
            raise RuntimeError("Workload checksum mismatch")
        measurements.append(duration_ms)

    ordered = sorted(measurements)

    def percentile(p):
        return ordered[math.ceil(samples * p) - 1]

    return {
        "samples_ms": [round(value, 6) for value in measurements],
        "median_ms": round(statistics.median(ordered), 3),
        "p95_ms": round(percentile(0.95), 3),
        "p99_ms": round(percentile(0.99), 3),
        "checksum": expected,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmups", type=int, default=3)
    args = parser.parse_args()
    if args.samples < 1 or args.warmups < 0:
        parser.error("--samples must be positive and --warmups nonnegative")

    output = {
        "environment": {
            "os": platform.platform(),
            "architecture": platform.machine(),
            "python": sys.version.split()[0],
            "openssl": ssl.OPENSSL_VERSION,
            "logical_cpus": os.cpu_count(),
        },
        "method": {
            "clock": "time.perf_counter_ns",
            "samples": args.samples,
            "warmups": args.warmups,
            "parallel_workers": PARALLEL_WORKERS,
            "sha256_repeats_per_worker": SHA256_REPEATS_PER_WORKER,
        },
        "workloads": {
            "sha256_32mib": measure(sha256, EXPECTED_SHA256, args.samples, args.warmups),
            "python_integer_400k": measure(
                integer_mix, EXPECTED_INTEGER, args.samples, args.warmups
            ),
        },
    }
    with ProcessPoolExecutor(
        max_workers=PARALLEL_WORKERS, mp_context=multiprocessing.get_context("spawn")
    ) as pool:
        def four_worker_sha256():
            return tuple(pool.map(sha256_repeats, (SHA256_REPEATS_PER_WORKER,) * PARALLEL_WORKERS))

        parallel = measure(
            four_worker_sha256, (EXPECTED_SHA256,) * PARALLEL_WORKERS, args.samples, args.warmups
        )
    total_mib = (
        len(PAYLOAD) // 1048576 * PARALLEL_WORKERS * SHA256_REPEATS_PER_WORKER
    )
    parallel["throughput_mib_s"] = round(total_mib * 1000 / parallel["median_ms"], 1)
    output["workloads"]["sha256_4workers_1024mib"] = parallel

    json.dump(output, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
