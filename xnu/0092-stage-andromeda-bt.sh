#!/usr/bin/env bash
# Stage the andromeda Bluetooth step B1 of patch 0092 (andrbt=1, off by
# default): WCN3990 power-on over SE13 UART. 0092-andromeda-bt.patch adds the
# file to bsd/conf/files; 0092-andromeda-bt-rpmh.patch (copied straight into
# the build's patch directory, after the 0084 generator) adds its RPMh rails.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_bt.c" bsd/dev/
