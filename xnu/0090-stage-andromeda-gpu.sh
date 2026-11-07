#!/usr/bin/env bash
# Stage the andromeda GPU driver of patch 0090: Adreno 640 first-stage
# bring-up (port of ios-flame's sweet_a6xx.c / sweet_gpu.c), off unless the
# boot-arg andrgpu=N is set. Runs after 0085/0087/0089; 0090-andromeda-gpu.patch
# adds both source files to bsd/conf/files.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
install -m 0644 "$here/andromeda_gpu.c" "$here/andromeda_gpu_core.c" "$here/andromeda_gpu.h" "$here/andromeda_gpu_governor.h" bsd/dev/
