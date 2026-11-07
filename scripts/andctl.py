#!/usr/bin/env python3
"""Send one command to the andromeda runtime control and print the answer.

  scripts/andctl.py status              state of every driver, one line
  scripts/andctl.py help                the command list, from the phone
  scripts/andctl.py gpu 2               start (or continue) a driver now
  scripts/andctl.py idle keys 50        set a P1 saving; "idle" shows them
  scripts/andctl.py --timeout 5 fastboot

The phone needs the boot-arg andrctl=1 (xnu/andromeda_ctl.c). Its kernel
listens on udp/5141 and answers only datagrams that come from 192.168.7.1
over its USB Ethernet (ue0), so this PC must have that address on the USB
link (ANDROMEDA.md). The answer is a reply on the same flow: a stateful
firewall such as ufw lets it in without a rule and without sudo, the same
reason scripts/udplog-punch.py works. The answer also appears in the kernel
log as "ANDROMEDA CTL: ..." (scripts/andstatus.py shows the last one).

Commands (the phone checks them again; see xnu/andromeda_ctl.h):
  status | help
  gpu 1-4 | bt 1-2 | hap 1 | cam 1 | wlan 1-2 | bl 1-2
      start now at that level, with the driver's own checks; a boot-arg run
      still in its delay starts at once; gpu and wlan continue a run that
      passed with the next levels; nothing that ran or failed runs again
  pwr 0|1                       ANDROMEDA PWR statistics off/on
  idle [cpu 0-1 | fb 0-1 | usb 0,2-20 | keys 0,20-200 | touch 0,20-1000]
  slider 1 | slidercam 0-2 | fastboot
  shot 4-60                     thumbnail of the panel into the log, N px blocks
                                (scripts/andshot.py LOG OUT.png makes a PNG)
  fbsync 0|1                    CPU copy of Metal frames waits for the GPU (1)
  key 0-3                       press and release: 0 power, 1 vol+, 2 vol-, 3 home
  touch X Y                     tap (touch canvas 1080x2400, finger 9)
  swipe X1 Y1 X2 Y2 [MS]        drag in MS ms (default 300); unlock: swipe 540 2390 540 1200

One datagram per command. Only "status", "help" and "idle" without a value
are sent again when no answer comes; a command that starts something is
sent once (ask "status" to see what happened). Exit status: 0 "ok", 1 "no",
2 no answer, 64 usage.
"""
import argparse
import socket
import sys
import time

PORT = 5141
MAX_LEN = 96
RETRY_SAFE = {"status", "help", "idle"}


def ask(command, phone, port, timeout, tries):
    """The phone's answer line, or None; raises ConnectionRefusedError."""
    data = command.encode("ascii")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((phone, port))
        for _ in range(tries):
            sock.send(data)
            deadline = time.monotonic() + timeout
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                sock.settimeout(left)
                try:
                    reply = sock.recv(4096)
                except socket.timeout:
                    break
                text = reply.decode("utf-8", "replace").strip()
                if text:
                    return text
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="andromeda runtime control (andrctl=1): one command, one answer",
        epilog="commands: status, help, gpu N, bt N, hap 1, cam 1, wlan N, bl N, pwr 0|1, "
               "idle [NAME VALUE], slider 1, slidercam N, fastboot, shot N, fbsync 0|1, key N, "
               "touch X Y, swipe X1 Y1 X2 Y2 [MS]")
    parser.add_argument("--phone", default="192.168.7.2", help="phone address (default 192.168.7.2)")
    parser.add_argument("--port", type=int, default=PORT, help=f"UDP port (default {PORT})")
    parser.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for the answer")
    parser.add_argument("words", nargs="+", metavar="COMMAND", help="e.g. status, gpu 2, idle keys 50")
    try:
        args = parser.parse_args(argv)
    except SystemExit as stop:      # usage errors: 64, as documented; --help: 0
        return 0 if stop.code in (0, None) else 64

    command = " ".join(args.words)
    words = command.split()
    if not words or len(command) > MAX_LEN or not command.isascii() or not command.isprintable():
        print(f"andctl: the command must be printable ASCII, at most {MAX_LEN} characters",
              file=sys.stderr)
        return 64
    tries = 3 if words[0] in RETRY_SAFE and (words[0] != "idle" or len(words) == 1) else 1
    try:
        answer = ask(command, args.phone, args.port, args.timeout, tries)
    except ConnectionRefusedError:
        print(f"andctl: {args.phone} refused udp/{args.port}: is the phone booted with andrctl=1?",
              file=sys.stderr)
        return 2
    except OSError as err:
        print(f"andctl: {args.phone}:{args.port}: {err.strerror or err}", file=sys.stderr)
        return 2
    if answer is None:
        print(f"andctl: no answer from {args.phone}:{args.port} in {args.timeout:g} s"
              + (f" ({tries} tries)" if tries > 1 else " (sent once: ask \"status\")")
              + "; andrctl=1 set, and 192.168.7.1 on the USB link?", file=sys.stderr)
        return 2
    print(answer)
    return 0 if answer.startswith("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
