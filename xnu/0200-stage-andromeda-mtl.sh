#!/usr/bin/env bash
# Stage phases F5/F6 of patch 0200 (off unless andrmtl=1|2, and nothing is
# published before andrgpu's step 4 has passed): the NativeAdreno ABI v1 user
# client for Turnip and the IOAcceleratorES service for Metal (port of
# ios-flame 0096/0097/0115/0116, MIT; see CREDITS.md). 0200-andromeda-mtl.patch
# adds the C file to bsd/conf/files, the C++ file to iokit/conf/files.arm64 and
# the SweetAccelerator personality to iokit/KernelConfigTables.cpp.
# andromeda_gpu_mtl.h is shared by both sides, so it goes to both directories;
# NativeAdrenoABI.h (UniXNU NativeAdreno, MIT, unchanged) only to the C++ side.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/andromeda_gpu_mtl.c" "$here/andromeda_gpu_mtl.h" bsd/dev/
install -m 0644 "$here/SweetAccelerator.cpp" "$here/andromeda_gpu_mtl.h" "$here/NativeAdrenoABI.h" iokit/Kernel/arm/
