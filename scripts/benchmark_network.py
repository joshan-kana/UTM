#!/usr/bin/env python3
"""Measure TCP request latency and verified transfer rates between two machines.

Run a server on the Mac, then use the same client on native macOS and in the VM.
The default binds only to host loopback; guest QEMU user-mode networking can
reach the Mac through its 10.0.2.2 gateway. This does not measure bridged NICs.
"""

import argparse
import hashlib
import json
import math
import platform
import socket
import statistics
import time

BLOCK = bytes(range(256)) * 256
ROUND_TRIPS = 200
DEFAULT_PORT = 47831


def receive_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise ConnectionError("Peer disconnected during transfer")
        result.extend(part)
    return bytes(result)


def measured_stats(samples):
    ordered = sorted(samples)
    return {
        "median_ms": round(statistics.median(ordered), 4),
        "p95_ms": round(ordered[math.ceil(len(ordered) * 0.95) - 1], 4),
        "p99_ms": round(ordered[math.ceil(len(ordered) * 0.99) - 1], 4),
        "samples_ms": [round(value, 6) for value in samples],
    }


def verify_receiver(sock, count, digest):
    hasher = hashlib.sha256()
    remaining = count
    while remaining:
        piece = sock.recv(min(len(BLOCK), remaining))
        if not piece:
            raise ConnectionError("Peer disconnected during transfer")
        hasher.update(piece)
        remaining -= len(piece)
    if hasher.digest() != digest:
        raise ValueError("Payload integrity failure")
    sock.sendall(hasher.digest())


def serve_client(sock, count, digest):
    sock.settimeout(30)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for _ in range(ROUND_TRIPS):
        if receive_exact(sock, 1) != b"?":
            raise ValueError("Invalid latency request")
        sock.sendall(b"!")
    verify_receiver(sock, count, digest)
    remaining = count
    while remaining:
        piece = BLOCK[:min(remaining, len(BLOCK))]
        sock.sendall(piece)
        remaining -= len(piece)
    if receive_exact(sock, len(digest)) != digest:
        raise ValueError("Client rejected download checksum")
    sock.sendall(b"!")


def run_server(port, connections, count, digest):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(connections)
        listener.settimeout(60)
        print(json.dumps({"listening": "127.0.0.1", "port": port, "connections": connections}), flush=True)
        for _ in range(connections):
            conn, address = listener.accept()
            with conn:
                serve_client(conn, count, digest)
            print(json.dumps({"completed_client": address[0]}), flush=True)


def run_client(host, port, count, digest):
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.settimeout(30)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        latencies = []
        for _ in range(ROUND_TRIPS):
            start = time.perf_counter_ns()
            sock.sendall(b"?")
            if receive_exact(sock, 1) != b"!":
                raise ValueError("Invalid latency response")
            latencies.append((time.perf_counter_ns() - start) / 1_000_000)

        start = time.perf_counter_ns()
        remaining = count
        while remaining:
            chunk = BLOCK[:min(remaining, len(BLOCK))]
            sock.sendall(chunk)
            remaining -= len(chunk)
        if receive_exact(sock, len(digest)) != digest:
            raise ValueError("Server rejected upload checksum")
        upload_seconds = (time.perf_counter_ns() - start) / 1e9

        start = time.perf_counter_ns()
        verify_receiver(sock, count, digest)
        if receive_exact(sock, 1) != b"!":
            raise ValueError("Server rejected download checksum")
        download_seconds = (time.perf_counter_ns() - start) / 1e9

    return {
        "host": platform.platform(),
        "peer": host,
        "bytes_per_direction": count,
        "round_trips": ROUND_TRIPS,
        "tcp_rtt": measured_stats(latencies),
        "upload_mib_s": round(count / upload_seconds / 1048576, 2),
        "download_mib_s": round(count / download_seconds / 1048576, 2),
        "sha256_verified": digest.hex(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["server", "client"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--mib", type=int, default=32)
    parser.add_argument("--connections", type=int, default=2)
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535 or args.mib < 1 or args.connections < 1:
        parser.error("port, MiB, and connections must be positive and port <= 65535")
    count = args.mib * 1048576
    h = hashlib.sha256()
    remaining = count
    while remaining:
        chunk = BLOCK[:min(len(BLOCK), remaining)]
        h.update(chunk)
        remaining -= len(chunk)
    digest = h.digest()
    if args.role == "server":
        run_server(args.port, args.connections, count, digest)
    else:
        print(json.dumps(run_client(args.host, args.port, count, digest), indent=2))


if __name__ == "__main__":
    main()
