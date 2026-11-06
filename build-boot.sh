#!/usr/bin/env bash
# Build the XNU boot image for the device in board.toml, inside the build
# container: GUACESP payload (kernel + Apple Device Tree), then the boot image
# with the UniXNU firmware volume built by build-uefi.sh.
#
# Needs: inputs/<name>/kernel.macho (artifact of the kernel-<name> workflow),
# scripts/base-d22ap.adt (scripts/get-base-d22ap.sh), out/<name>/fv/SM8150_EFI-unixnu.fd.
# Output: out/<name>/boot/boot-<name>-xnu.img
set -euo pipefail
cd "$(dirname "$0")"
NAME="$(python3 -c 'import tomllib; print(tomllib.load(open("board.toml","rb"))["board"]["name"])')"
for f in "inputs/$NAME/kernel.macho" scripts/base-d22ap.adt "out/$NAME/fv/SM8150_EFI-unixnu.fd"; do
  [ -s "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
docker build -q -t unixnu-firmware -f build/Dockerfile.firmware build/ >/dev/null
# Test images next to the normal one: ANDROMEDA_OWNED=0x... (another kernel
# window) and/or ANDROMEDA_ARGS="a=1 b=2" (kernel args added or replaced),
# named boot-<name>-xnu-<ANDROMEDA_TAG or OWNED>.img.
TAG="${ANDROMEDA_TAG:-${ANDROMEDA_OWNED:-}}"
SUFFIX="${TAG:+-$TAG}"
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e ANDROMEDA_OWNED="${ANDROMEDA_OWNED:-}" -e ANDROMEDA_ARGS="${ANDROMEDA_ARGS:-}" -v "$PWD:/work" -w /work unixnu-firmware bash -c "
  set -euo pipefail
  bash scripts/get-dtb.sh
  python3 scripts/build-payload.py
  python3 scripts/build-bootimg.py --fd out/$NAME/fv/SM8150_EFI-unixnu.fd -o out/$NAME/boot/boot-$NAME-xnu$SUFFIX.img
  mv out/$NAME/boot/boot.json out/$NAME/boot/boot-$NAME-xnu$SUFFIX.json
  sha256sum out/$NAME/boot/boot-$NAME-xnu$SUFFIX.img
"
