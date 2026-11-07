#!/usr/bin/env python3
"""Turn an "andctl.py shot N" thumbnail from the phone's log into a PNG.

  scripts/andshot.py LOG OUT.png [SHOT_NUMBER] [--scale S]

The kernel (andromeda_ctl.c, "shot N") prints the panel as N x N blocks, one
line per row of blocks and per 30 blocks:
  ANDROMEDA SHOT begin K: CxR blocks of N px (panel WxH)
  ANDROMEDA SHOT K RRR.P: RRGGBBRRGGBB...
  ANDROMEDA SHOT end K
Without SHOT_NUMBER the last complete shot in LOG is used. Missing lines (the
USB log can drop some) are drawn magenta and counted. Writes the PNG with the
standard library only. --scale enlarges each block (default: N / 2, at least 1).
"""
import argparse
import re
import struct
import sys
import zlib

BEGIN = re.compile(r"ANDROMEDA SHOT begin (\d+): (\d+)x(\d+) blocks of (\d+) px")
ROW = re.compile(r"ANDROMEDA SHOT (\d+) (\d+)\.(\d+): ([0-9a-f]*)")
END = re.compile(r"ANDROMEDA SHOT end (\d+)")
CHUNK = 30


def png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    p = argparse.ArgumentParser(description="andctl shot thumbnail -> PNG")
    p.add_argument("log")
    p.add_argument("out")
    p.add_argument("shot", nargs="?", type=int)
    p.add_argument("--scale", type=int, default=0)
    a = p.parse_args()
    shots, done = {}, set()
    with open(a.log, errors="replace") as f:
        for line in f:
            m = BEGIN.search(line)
            if m:
                k = int(m.group(1))
                shots[k] = {"cols": int(m.group(2)), "rows": int(m.group(3)), "block": int(m.group(4)),
                            "data": {}}
                done.discard(k)
                continue
            m = ROW.search(line)
            if m and int(m.group(1)) in shots:
                shots[int(m.group(1))]["data"][(int(m.group(2)), int(m.group(3)))] = m.group(4)
                continue
            m = END.search(line)
            if m:
                done.add(int(m.group(1)))
    if not shots:
        sys.exit("no ANDROMEDA SHOT in " + a.log)
    k = a.shot if a.shot is not None else max(done or shots)
    if k not in shots:
        sys.exit(f"shot {k} not in the log")
    s = shots[k]
    cols, rows, block = s["cols"], s["rows"], s["block"]
    scale = a.scale or max(1, block // 2)
    missing = 0
    img = []
    for r in range(rows):
        line = []
        for c0 in range(0, cols, CHUNK):
            hexs = s["data"].get((r, c0 // CHUNK))
            n = min(CHUNK, cols - c0)
            if hexs is None or len(hexs) < 6 * n:
                missing += 1
                line += [255, 0, 255] * n
            else:
                line += list(bytes.fromhex(hexs[: 6 * n]))
        big = []
        for i in range(cols):
            big += line[3 * i: 3 * i + 3] * scale
        img += [big] * scale
    png(a.out, cols * scale, rows * scale, img)
    print(f"shot {k}: {cols}x{rows} blocks of {block} px -> {a.out} ({cols * scale}x{rows * scale}),"
          f" {missing} missing line pieces")


if __name__ == "__main__":
    main()
