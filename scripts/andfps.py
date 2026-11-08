#!/usr/bin/env python3
"""Frame-rate benchmark from the PC: swipe the home screen pages and read the
framebuffer statistics the kernel prints (boot-arg sweetfb_stats=1).

  scripts/andfps.py LOG [--seconds N] [--unlock] [--mode home|settings] [--label TEXT]

LOG is the live log being received (scripts/udplog-punch.py). The phone must
be on the home screen (or locked, with --unlock). For N seconds (default 20)
it swipes left and right through andctl.py (--mode home), or flings the list
of an open app such as Settings up and down (--mode settings: SpringBoard
caps the page-dot fade after each home swipe at 20-30 fps, found by Scion's
inject_sb.c, so home swipes understate what CA can do), then waits for the next
"SWEET PERF" lines (one every 10 s) and sums those that fall inside the run:
frames backboardd submitted and frames shown per second, mean copy time per
shown frame, vsync ticks missed. The last "gov 60s" line of the GPU governor
is printed too. Nothing is written on the phone.
"""
import argparse
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
PERF = re.compile(r"^(\d\d:\d\d:\d\d) .*SWEET PERF: ms=(\d+) submitted=(\d+) presented=(\d+) "
                  r"overwritten=(\d+) copy_us=(\d+) max_us=(\d+) bytes=(\d+) missed=(\d+)")
GOV = re.compile(r"ANDROMEDA GPU: gov 60s: (.*)")


def ctl(*args):
    subprocess.run([sys.executable, '-I', str(HERE / 'andctl.py'), *map(str, args)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def secs(hms):
    t = datetime.strptime(hms, '%H:%M:%S')
    return t.hour * 3600 + t.minute * 60 + t.second


def main():
    p = argparse.ArgumentParser(description='home screen swipe frame-rate benchmark')
    p.add_argument('log')
    p.add_argument('--seconds', type=int, default=20)
    p.add_argument('--unlock', action='store_true')
    p.add_argument('--mode', choices=('home', 'settings'), default='home')
    p.add_argument('--label', default='')
    a = p.parse_args()
    log = Path(a.log)
    if a.unlock:
        ctl('swipe', 540, 2380, 540, 1100, 250)
        time.sleep(2)
    start = secs(time.strftime('%H:%M:%S'))
    end_at = time.time() + a.seconds
    while time.time() < end_at:
        if a.mode == 'home':
            ctl('swipe', 950, 1200, 150, 1200, 300)
            time.sleep(0.9)
            ctl('swipe', 150, 1200, 950, 1200, 300)
            time.sleep(0.9)
        else:
            ctl('swipe', 540, 1900, 540, 700, 150)
            time.sleep(0.9)
            ctl('swipe', 540, 700, 540, 1900, 150)
            time.sleep(0.9)
    end = secs(time.strftime('%H:%M:%S'))
    time.sleep(12)          # the 10 s window that holds the end of the run
    rows = []
    gov = None
    for line in log.read_text(errors='replace').splitlines():
        m = PERF.match(line)
        if m:
            t = secs(m.group(1))
            # a line reports the 10 s before it: keep windows fully inside the run
            if t - 10 >= start - 1 and t <= end + 1:
                rows.append(tuple(int(x) for x in m.groups()[1:]))
        g = GOV.search(line)
        if g:
            gov = g.group(1)
    if not rows:
        sys.exit('no SWEET PERF window inside the run (boot with sweetfb_stats=1, run >= 20 s)')
    ms = sum(r[0] for r in rows)
    sub, shown, over, copy_us, missed = (sum(r[i] for r in rows) for i in (1, 2, 3, 4, 7))
    mx = max(r[5] for r in rows)
    print(f'{a.label + ": " if a.label else ""}{len(rows)} window(s), {ms / 1000:.0f} s: '
          f'submitted {sub * 1000 / ms:.1f}/s, shown {shown * 1000 / ms:.1f}/s, '
          f'copy {copy_us / max(shown, 1) / 1000:.1f} ms (max {mx / 1000:.0f}), '
          f'overwritten {over}, missed vsync {missed}')
    if gov:
        print(f'last GPU governor line: {gov}')


if __name__ == '__main__':
    main()
