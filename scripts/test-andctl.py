#!/usr/bin/env python3
"""Tests for scripts/andctl.py against a fake phone on 127.0.0.1.

  python3 scripts/test-andctl.py
"""
import socket
import subprocess
import sys
import threading
import unittest
from pathlib import Path

ANDCTL = Path(__file__).resolve().parent / "andctl.py"


class FakePhone:
    """UDP server: answers with answer(datagram) (None = stays silent)."""

    def __init__(self, answer):
        self.answer = answer
        self.got = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.stop = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop:
            try:
                data, peer = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            self.got.append(data)
            reply = self.answer(data, len(self.got))
            if reply is not None:
                self.sock.sendto(reply, peer)

    def close(self):
        self.stop = True
        self.thread.join()
        self.sock.close()


def run(port, *words, timeout="0.4"):
    return subprocess.run([sys.executable, str(ANDCTL), "--phone", "127.0.0.1", "--port", str(port),
                           "--timeout", timeout, *words], capture_output=True, text=True, timeout=30)


class AndctlTest(unittest.TestCase):
    def phone(self, answer):
        p = FakePhone(answer)
        self.addCleanup(p.close)
        return p

    def test_ok_answer(self):
        p = self.phone(lambda d, n: b"ok " + d + b": running levels 1..2 now\n")
        r = run(p.port, "gpu", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "ok gpu 2: running levels 1..2 now\n")
        self.assertEqual(p.got, [b"gpu 2"])

    def test_no_answer_is_exit_1(self):
        p = self.phone(lambda d, n: b"no bt 2: it runs once per boot\n")
        r = run(p.port, "bt", "2")
        self.assertEqual(r.returncode, 1)
        self.assertIn("once per boot", r.stdout)

    def test_status_is_retried(self):
        p = self.phone(lambda d, n: None if n == 1 else b"ok status: up 10 s\n")
        r = run(p.port, "status")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(p.got, [b"status", b"status"])

    def test_start_is_sent_once(self):
        p = self.phone(lambda d, n: None)
        r = run(p.port, "hap", "1", timeout="0.3")
        self.assertEqual(r.returncode, 2)
        self.assertIn("sent once", r.stderr)
        self.assertEqual(p.got, [b"hap 1"])

    def test_idle_with_value_is_sent_once(self):
        p = self.phone(lambda d, n: None)
        self.assertEqual(run(p.port, "idle", "keys", "50", timeout="0.2").returncode, 2)
        self.assertEqual(p.got, [b"idle keys 50"])

    def test_idle_alone_is_retried(self):
        p = self.phone(lambda d, n: None)
        self.assertEqual(run(p.port, "idle", timeout="0.2").returncode, 2)
        self.assertEqual(p.got, [b"idle"] * 3)

    def test_refused(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()           # nothing listens there: ICMP port unreachable
        r = run(port, "status")
        self.assertEqual(r.returncode, 2)
        self.assertIn("andrctl=1", r.stderr)

    def test_usage(self):
        self.assertEqual(run(1, "gpu", "è").returncode, 64)
        self.assertEqual(run(1, "x" * 97).returncode, 64)
        self.assertEqual(run(1, "gpu\t1").returncode, 64)
        self.assertEqual(run(1, "   ").returncode, 64)
        self.assertEqual(subprocess.run([sys.executable, str(ANDCTL)], capture_output=True).returncode, 64)
        self.assertEqual(subprocess.run([sys.executable, str(ANDCTL), "--help"], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
