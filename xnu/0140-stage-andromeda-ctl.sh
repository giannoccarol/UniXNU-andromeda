#!/usr/bin/env bash
# Stage the andromeda runtime control of patch 0140 (andrctl=1, off by
# default): a kernel UDP socket on udp/5141 that takes the closed command list
# of andromeda_ctl.h from 192.168.7.1 on ue0 only and starts the andromeda
# drivers at a level, with their own checks (scripts/andctl.py on the PC).
# andromeda_ctl.h is also included by the drivers it starts (andromeda_gpu.c,
# _bt.c, _cam.c, _haptics.c, _wlan.c, _backlight.c), whose stage scripts run
# before any patch. 0140-andromeda-ctl.patch adds the file to bsd/conf/files
# after andromeda_haptics.c of patch 0135.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_ctl.c" "$here/andromeda_ctl.h" bsd/dev/
