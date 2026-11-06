#!/usr/bin/env bash
# Put the device tree table the boot image appends at inputs/uefi-prebuilt/<name>.dtb.
#
# Order of preference:
#   1. inputs/live-fdt/<name>.dtb: the phone's own live FDT (/sys/firmware/fdt,
#      needs root), when it has been pulled. One FDT with the real msm-id and
#      board-id, as the Mi 9 port uses.
#   2. Aloha's Device/xiaomi-<name>/DeviceTreeBlob/Android/android-<name>.dtb.
#      For andromeda this is a table of generic SM8150 base trees; the ABL picks
#      the one that matches the SoC and applies the dtbo overlay.
# Files under 1 KiB are placeholders (Aloha's Linux/*.dtb) and are skipped.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="$(python3 -c 'import tomllib,sys; print(tomllib.load(open(sys.argv[1],"rb"))["board"]["name"])' "$ROOT/board.toml")"
DEST="$ROOT/inputs/uefi-prebuilt/$NAME.dtb"
mkdir -p "$(dirname "$DEST")"

for SRC in "$ROOT/inputs/live-fdt/$NAME.dtb" \
           "$ROOT/uefi/Platforms/SurfaceDuo1Pkg/Device/xiaomi-$NAME/DeviceTreeBlob/Android/android-$NAME.dtb"; do
  [ -f "$SRC" ] || continue
  SZ=$(wc -c < "$SRC")
  if [ "$SZ" -le 1024 ]; then
    echo "skip placeholder DTB: ${SRC#$ROOT/} ($SZ bytes)"
    continue
  fi
  cp "$SRC" "$DEST"
  echo "DTB: ${SRC#$ROOT/} ($SZ bytes) -> ${DEST#$ROOT/}"
  exit 0
done
echo "ERROR: no DTB for $NAME (expected inputs/live-fdt/$NAME.dtb or the Aloha device dir)" >&2
exit 1
