#!/usr/bin/env bash
# Build the firmware-only boot images for the device in board.toml, inside an
# Ubuntu 24.04 container (EDK II does not build from a path with spaces, and the
# host lacks iasl/nasm). Output: out/<name>/boot/boot-<name>-uefi-{plain,unixnu}.img
set -euo pipefail
cd "$(dirname "$0")"
docker build -q -t unixnu-firmware -f build/Dockerfile.firmware build/ >/dev/null
exec docker run --rm --network host \
  --user "$(id -u):$(id -g)" -e HOME=/tmp \
  ${VARIANTS:+-e VARIANTS="$VARIANTS"} \
  -v "$PWD:/work" -w /work \
  unixnu-firmware bash scripts/firmware-in-container.sh "$@"
