#!/usr/bin/env bash
# Stage the andromeda Wi-Fi/modem driver of patch 0110 (andrwlan=N, off by
# default): the W0 read-only reconnaissance and the W1 TrustZone PAS modem
# boot of docs/research/wifi-flame.md. 0110-andromeda-wlan.patch adds the file
# to bsd/conf/files after andromeda_cam.c (patch 0098), i.e. it runs after
# 0085/0087/0089/0090/0092/0093/0097/0098.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_wlan.c" bsd/dev/
