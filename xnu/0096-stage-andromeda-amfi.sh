#!/usr/bin/env bash
# Stage the AppleMobileFileIntegrity stand-in of patch 0096 (port of
# ios-flame's 0103, MIT; see CREDITS.md).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -d iokit/Kernel/arm || mkdir -p iokit/Kernel/arm
install -m 0644 "$here/SweetAMFI.cpp" iokit/Kernel/arm/
