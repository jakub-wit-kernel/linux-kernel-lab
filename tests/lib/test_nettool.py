#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Unit tests for nettool.py, over loopback (no root, no modules)."""
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nettool  # noqa: E402

TOOL = [sys.executable, os.path.join(HERE, "nettool.py")]


def free_port(kind: int) -> int:
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(path: str, timeout: float = 5.0) -> None:
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > end:
            raise TimeoutError(path)
        time.sleep(0.02)


def fields(line: str) -> dict:
    return dict(kv.split("=", 1) for kv in line.split())


class StreamTest(unittest.TestCase):
    def test_exact_length(self):
        for total in (0, 1, 65535, 65536, 65537, 300000):
            self.assertEqual(sum(len(p) for p in nettool.stream(total)), total)

    def test_deterministic_and_not_constant(self):
        a = b"".join(nettool.stream(100000))
        b = b"".join(nettool.stream(100000))
        self.assertEqual(a, b)
        self.assertGreater(len(set(a)), 200)

    def test_prefix_property(self):
        # A shorter stream is a prefix of a longer one.
        short = b"".join(nettool.stream(70000))
        long = b"".join(nettool.stream(140000))
        self.assertEqual(long[:70000], short)


class LoopbackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.ready = os.path.join(self.tmp.name, "ready")

    def tearDown(self):
        self.tmp.cleanup()

    def test_udp_counts_every_datagram(self):
        port = free_port(socket.SOCK_DGRAM)
        rx = subprocess.Popen(TOOL + ["udp-recv", str(port), "--ready",
                                      self.ready, "--max", "50"],
                              stdout=subprocess.PIPE, text=True)
        wait_for(self.ready)
        tx = subprocess.run(TOOL + ["udp-send", "127.0.0.1", str(port), "50",
                                    "--size", "100"],
                            stdout=subprocess.PIPE, text=True, check=True)
        out, _ = rx.communicate(timeout=10)
        self.assertEqual(fields(tx.stdout)["sent"], "50")
        self.assertEqual(fields(out), {"received": "50", "bytes": "5000"})

    def test_udp_reports_tos(self):
        port = free_port(socket.SOCK_DGRAM)
        rx = subprocess.Popen(TOOL + ["udp-recv", str(port), "--ready",
                                      self.ready, "--max", "10", "--tos"],
                              stdout=subprocess.PIPE, text=True)
        wait_for(self.ready)
        subprocess.run(TOOL + ["udp-send", "127.0.0.1", str(port), "10",
                               "--tos", "184"], check=True,
                       stdout=subprocess.DEVNULL)
        out, _ = rx.communicate(timeout=10)
        self.assertEqual(fields(out)["received"], "10")
        self.assertEqual(fields(out)["tos"], "184:10")

    def test_udp_receiver_gives_up_when_nothing_comes(self):
        port = free_port(socket.SOCK_DGRAM)
        t0 = time.monotonic()
        out = subprocess.run(TOOL + ["udp-recv", str(port), "--ready",
                                     self.ready, "--wait", "0.3"],
                             stdout=subprocess.PIPE, text=True, check=True)
        self.assertEqual(fields(out.stdout)["received"], "0")
        self.assertLess(time.monotonic() - t0, 5)

    def test_tcp_digests_match(self):
        port = free_port(socket.SOCK_STREAM)
        rx = subprocess.Popen(TOOL + ["tcp-recv", str(port), "--ready",
                                      self.ready],
                              stdout=subprocess.PIPE, text=True)
        wait_for(self.ready)
        tx = subprocess.run(TOOL + ["tcp-send", "127.0.0.1", str(port),
                                    "1000003"],
                            stdout=subprocess.PIPE, text=True, check=True)
        out, _ = rx.communicate(timeout=30)
        self.assertEqual(fields(tx.stdout), fields(out))
        self.assertEqual(fields(out)["bytes"], "1000003")


if __name__ == "__main__":
    unittest.main()
