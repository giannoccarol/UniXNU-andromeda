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
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/work" -w /work unixnu-firmware bash -c "
  set -euo pipefail
  bash scripts/get-dtb.sh
  python3 scripts/build-payload.py
  python3 scripts/build-bootimg.py --fd out/$NAME/fv/SM8150_EFI-unixnu.fd -o out/$NAME/boot/boot-$NAME-xnu.img
  mv out/$NAME/boot/boot.json out/$NAME/boot/boot-$NAME-xnu.json
  sha256sum out/$NAME/boot/boot-$NAME-xnu.img
"
