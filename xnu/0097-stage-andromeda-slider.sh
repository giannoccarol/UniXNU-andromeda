#!/usr/bin/env bash
# Stage the andromeda camera-slider sysctls of patch 0097 (andrslidercam=1/2,
# off by default): andromeda.slider and andromeda.slider_wait, fed by the
# slider thread of andromeda_probe.c (andrslider=1) and read by the
# phone/andromedad daemon. 0097-andromeda-slider.patch adds the file to
# bsd/conf/files (runs after 0085/0087/0089/0090/0092/0093).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_slider.c" "$here/andromeda_slider.h" bsd/dev/
