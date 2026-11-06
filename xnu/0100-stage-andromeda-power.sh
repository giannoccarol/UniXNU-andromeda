#!/usr/bin/env bash
# Stage the andromeda power statistics of patch 0100 (andrpwr=1, off by
# default; docs/research/sospensione.md phase P0) and the state shared by the
# P1 patches 0101-0105. 0100-andromeda-power-stats.patch adds the file to
# bsd/conf/files and starts it from sweet_ufs.c.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_power.c" "$here/andromeda_power.h" bsd/dev/
