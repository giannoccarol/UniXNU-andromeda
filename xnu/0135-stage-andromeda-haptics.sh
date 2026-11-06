#!/usr/bin/env bash
# Stage the andromeda vibration test of patch 0135 (andrhap=1, off by
# default): step V1 of docs/research/sensori-vibrazione.md, two 200 ms pulses
# of the PM8150B haptics block at no more than 2552 mV, then every changed
# register written back. 0135-andromeda-haptics.patch adds the file to
# bsd/conf/files before cepheus_gpu_compile.c, i.e. after andromeda_wlan.c of
# patch 0110. Its SPMI write helper lives in andromeda_probe.c (patch 0089).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_haptics.c" bsd/dev/
