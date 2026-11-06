#!/usr/bin/env bash
# Stage the andromeda camera step C1 of patch 0098 (andrcam=1, off by
# default): power up the rear IMX363 alone, read its chip ID over GPIO I2C on
# the CCI0 pins, power it down again. 0098-andromeda-cam.patch adds the file to
# bsd/conf/files (runs after 0085/0087/0089/0090/0092/0093);
# 0098-andromeda-cam-rpmh.patch (copied straight into the build's patch
# directory, after 0092-andromeda-bt-rpmh.patch) adds its two RPMh rails.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_cam.c" bsd/dev/
