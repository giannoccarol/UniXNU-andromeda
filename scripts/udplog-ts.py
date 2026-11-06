#!/usr/bin/env python3
"""Receive the phone's live kernel console (UDP 5140) with host timestamps.

Like udplog.py, but every line gets the host time and the seconds since the
first datagram, so boot milestones can be timed. Writes until Ctrl-C (it does
not stop when the phone goes quiet).
usage: udplog-ts.py OUT.log
"""
import socket
import sys
import time

path = sys.argv[1] if len(sys.argv) > 1 else "boot.log"
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 5140))
print(f"listening on udp/5140 -> {path}", file=sys.stderr)
t0 = None
pending = b""
with open(path, "ab", buffering=0) as out:
    while True:
        data, _ = sock.recvfrom(4096)
        now = time.time()
        if t0 is None:
            t0 = now
        pending += data
        *lines, pending = pending.split(b"\n")
        for line in lines:
            stamp = time.strftime("%H:%M:%S", time.localtime(now)) + f" +{now - t0:7.1f}s "
            out.write(stamp.encode() + line.rstrip(b"\r") + b"\n")
