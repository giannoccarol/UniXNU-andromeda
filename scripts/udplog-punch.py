#!/usr/bin/env python3
"""udplog-ts.py for a PC with a stateful firewall (ufw) and no sudo.

The phone sends its console from an ephemeral UDP port (49152-65535, the
kernel socket of ios-sweet's patch 0077 is never bound) to 192.168.7.1:5140.
A firewall that drops new inbound UDP lets it in once this socket has sent
something to that port first: the phone's datagrams are then replies of a
known flow. So, from port 5140, send one byte to every ephemeral port of the
phone (USB link only), again every 20 s while nothing arrives, and write what
comes back exactly as udplog-ts.py does. No firewall setting is changed.

usage: udplog-punch.py OUT.log [PHONE_IP]
"""
import socket
import sys
import time

path = sys.argv[1] if len(sys.argv) > 1 else "boot.log"
phone = sys.argv[2] if len(sys.argv) > 2 else "192.168.7.2"
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 5140))
sock.settimeout(1.0)
print(f"listening on udp/5140 -> {path} (opening the way from {phone})", file=sys.stderr)


def punch():
    for port in range(49152, 65536):
        try:
            sock.sendto(b"\n", (phone, port))
        except OSError:
            return  # link down: try again later


t0 = None
pending = b""
last_rx = 0.0
last_punch = 0.0
with open(path, "ab", buffering=0) as out:
    while True:
        now = time.time()
        if now - last_rx > 20 and now - last_punch > 20:
            punch()
            last_punch = now
        try:
            data, _ = sock.recvfrom(4096)
        except (socket.timeout, ConnectionRefusedError):
            continue
        now = last_rx = time.time()
        if t0 is None:
            t0 = now
        pending += data
        *lines, pending = pending.split(b"\n")
        for line in lines:
            stamp = time.strftime("%H:%M:%S", time.localtime(now)) + f" +{now - t0:7.1f}s "
            out.write(stamp.encode() + line.rstrip(b"\r") + b"\n")
