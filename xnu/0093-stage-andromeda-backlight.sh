#!/usr/bin/env bash
# Stage the andromeda brightness driver of patch 0093 (andrbl=1 test,
# andrbl=2 service; off by default): DSI0 DCS 0x51 sender and the minimal
# AppleARMBacklight service. 0093-andromeda-backlight.patch adds both files
# to bsd/conf/files and iokit/conf/files.arm64.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
test -f iokit/conf/files.arm64
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/andromeda_backlight.c" bsd/dev/
install -m 0644 "$here/AndromedaBacklight.cpp" iokit/Kernel/arm/
