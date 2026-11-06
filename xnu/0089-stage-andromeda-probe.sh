#!/usr/bin/env bash
# Stage the andromeda drivers of patch 0089: read-only BT/Wi-Fi/charger
# probe and CPU full clock with thermal guard.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_probe.c" "$here/andromeda_cpufreq.c" "$here/andromeda_cpu_governor.h" bsd/dev/
