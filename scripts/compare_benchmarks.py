#!/usr/bin/env python3
"""Summarize checksum-verified native/guest CPU and Vulkan measurements."""

import json
import sys


def load(filename):
    with open(filename, encoding="utf-8") as source:
        return json.load(source)


def validate(label, native, guest, method_keys, environment_keys):
    for key in method_keys:
        if native["method"][key] != guest["method"][key]:
            raise ValueError(f"{label}: different {key}")
    for key in environment_keys:
        if native["environment"][key] != guest["environment"][key]:
            raise ValueError(f"{label}: different {key}")
    if native["workloads"].keys() != guest["workloads"].keys():
        raise ValueError(f"{label}: workloads differ")
    for name, result in native["workloads"].items():
        other = guest["workloads"][name]
        if result["checksum"] != other["checksum"]:
            raise ValueError(f"{label}/{name}: mismatched output checksum")
        for value in (result, other):
            values = value["wall_submit_wait"] if "wall_submit_wait" in value else value
            if values["median_ms"] <= 0:
                raise ValueError(f"{label}/{name}: invalid median")


def compare(label, native, guest):
    native_workloads = native["workloads"]
    guest_workloads = guest["workloads"]

    print(f"\n{label}")
    print("| Workload | Native median | Guest median | Guest/native | Native P95 | Guest P95 |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for name, native_result in native_workloads.items():
        guest_result = guest_workloads[name]
        if "wall_submit_wait" in native_result:
            native_time = native_result["wall_submit_wait"]
            guest_time = guest_result["wall_submit_wait"]
        else:
            native_time, guest_time = native_result, guest_result

        median_native = native_time["median_ms"]
        median_guest = guest_time["median_ms"]
        print(
            f"| {name} | {median_native:.3f} ms | {median_guest:.3f} ms "
            f"| {median_guest / median_native:.3f}× "
            f"| {native_time['p95_ms']:.3f} ms "
            f"| {guest_time['p95_ms']:.3f} ms |"
        )


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "Usage: compare_benchmarks.py native-cpu.json guest-cpu.json "
            "native-vulkan.json guest-vulkan.json"
        )

    native_cpu, guest_cpu, native_vulkan, guest_vulkan = map(load, sys.argv[1:])
    validate(
        "CPU", native_cpu, guest_cpu,
        ("samples", "warmups", "parallel_workers", "sha256_repeats_per_worker"),
        ("python", "openssl"),
    )
    validate(
        "Vulkan", native_vulkan, guest_vulkan,
        ("api", "samples", "warmups", "compute_words", "compute_rounds",
         "buffer_copy_bytes", "shader_fnv64"),
        ("vendor_id",),
    )
    compare("CPU", native_cpu, guest_cpu)
    compare("Vulkan submission-to-completion", native_vulkan, guest_vulkan)
    print(
        "\nCPU workload runtimes and Vulkan compute shaders must be identical. "
        "Vulkan GPU-only timestamps are deliberately not compared because "
        "some virtual drivers return invalid timestamp results."
    )


if __name__ == "__main__":
    try:
        main()
    except (KeyError, ValueError, ZeroDivisionError, OSError) as error:
        raise SystemExit(str(error)) from error
