#!/usr/bin/env bash
# Stage the andromeda read-only BT/Wi-Fi/charger probe (patch 0089).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_probe.c" bsd/dev/
