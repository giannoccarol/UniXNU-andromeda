#!/usr/bin/env bash
# Stage phase F4 of patch 0120 (off unless andrgpupresent=1, after andrgpu=4
# has passed): the Adreno 640 copies the fitted frame onto the panel, the CPU
# copy of 0094 stays the fallback. 0120-andromeda-gpu-present.patch adds the C
# file to bsd/conf/files and hooks SweetFramebuffer.cpp, which includes the
# shared header from its own directory; the same header goes to bsd/dev/.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/andromeda_gpu_present.c" "$here/andromeda_gpu_present.h" bsd/dev/
install -m 0644 "$here/andromeda_gpu_present.h" iokit/Kernel/arm/
