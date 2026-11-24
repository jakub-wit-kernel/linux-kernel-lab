#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Traffic for the networking tests, standard library only.

    nettool.py udp-recv PORT --ready FILE [--wait S] [--idle S] [--max N]
        Bind 0.0.0.0:PORT, create FILE once bound, count datagrams until
        --max arrived, nothing came for --wait s (before the first one)
        or --idle s (after it). Prints "received=N bytes=B".
    nettool.py udp-send HOST PORT COUNT [--size N]
        Send COUNT datagrams of --size bytes. Prints "sent=N".
    nettool.py tcp-recv PORT --ready FILE [--timeout S]
        Accept one connection and read to EOF.
        Prints "bytes=N sha256=HEX".
    nettool.py tcp-send HOST PORT BYTES
        Send BYTES of a deterministic stream, then close.
        Prints "bytes=N sha256=HEX".

The ready file lets a test start a receiver in the background and send
only once it is listening.
"""
import argparse
import hashlib
import os
import socket
import sys
from typing import Iterator, List, Optional

CHUNK = 65536


def stream(total: int) -> Iterator[bytes]:
    """Deterministic, incompressible bytes: SHA-256 in counter mode."""
    block = 0
    sent = 0
    while sent < total:
        buf = bytearray()
        while len(buf) < CHUNK:
            buf += hashlib.sha256(block.to_bytes(8, "little")).digest()
            block += 1
        piece = bytes(buf[:min(CHUNK, total - sent)])
        sent += len(piece)
        yield piece


def mark_ready(path: str) -> None:
    with open(path + ".tmp", "w") as f:
        f.write("ready\n")
    os.replace(path + ".tmp", path)   # appears atomically


def udp_recv(port: int, ready: str, wait: float, idle: float,
             limit: Optional[int]) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(("0.0.0.0", port))
    mark_ready(ready)
    count = nbytes = 0
    s.settimeout(wait)
    while limit is None or count < limit:
        try:
            data = s.recv(65536)
        except socket.timeout:
            break
        count += 1
        nbytes += len(data)
        s.settimeout(idle)
    s.close()
    return f"received={count} bytes={nbytes}"


def udp_send(host: str, port: int, count: int, size: int) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = bytes(size)
    for _ in range(count):
        s.sendto(payload, (host, port))
    s.close()
    return f"sent={count}"


def tcp_recv(port: int, ready: str, timeout: float) -> str:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    srv.settimeout(timeout)
    mark_ready(ready)
    conn, _ = srv.accept()
    conn.settimeout(timeout)
    h = hashlib.sha256()
    total = 0
    while True:
        data = conn.recv(CHUNK)
        if not data:
            break
        h.update(data)
        total += len(data)
    conn.close()
    srv.close()
    return f"bytes={total} sha256={h.hexdigest()}"


def tcp_send(host: str, port: int, total: int) -> str:
    s = socket.create_connection((host, port), timeout=30)
    h = hashlib.sha256()
    for piece in stream(total):
        s.sendall(piece)
        h.update(piece)
    s.shutdown(socket.SHUT_WR)
    s.close()
    return f"bytes={total} sha256={h.hexdigest()}"


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("udp-recv")
    p.add_argument("port", type=int)
    p.add_argument("--ready", required=True)
    p.add_argument("--wait", type=float, default=5.0)
    p.add_argument("--idle", type=float, default=1.0)
    p.add_argument("--max", type=int)

    p = sub.add_parser("udp-send")
    p.add_argument("host")
    p.add_argument("port", type=int)
    p.add_argument("count", type=int)
    p.add_argument("--size", type=int, default=64)

    p = sub.add_parser("tcp-recv")
    p.add_argument("port", type=int)
    p.add_argument("--ready", required=True)
    p.add_argument("--timeout", type=float, default=30.0)

    p = sub.add_parser("tcp-send")
    p.add_argument("host")
    p.add_argument("port", type=int)
    p.add_argument("bytes", type=int)

    a = ap.parse_args(argv)
    if a.cmd == "udp-recv":
        out = udp_recv(a.port, a.ready, a.wait, a.idle, a.max)
    elif a.cmd == "udp-send":
        out = udp_send(a.host, a.port, a.count, a.size)
    elif a.cmd == "tcp-recv":
        out = tcp_recv(a.port, a.ready, a.timeout)
    else:
        out = tcp_send(a.host, a.port, a.bytes)
    print(out, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
