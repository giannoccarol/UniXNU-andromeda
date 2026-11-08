#!/usr/bin/env bash
# Stage the andromeda scanout of patch 0207 (andrscanout, off by default): the
# display reads iOS's frames itself instead of the CPU copy into the panel
# memory. andromeda_scanout.c owns the display's SMMU bank and pipe;
# andromeda_scanout.h is shared with iokit/Kernel/arm/SweetFramebuffer.cpp,
# which 0207-andromeda-scanout.patch changes (after 0205), together with the
# bsd/conf/files line after andromeda_gpu_mtl.c of patch 0200.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/andromeda_scanout.c" "$here/andromeda_scanout.h" bsd/dev/
install -m 0644 "$here/andromeda_scanout.h" iokit/Kernel/arm/
