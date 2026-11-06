#!/usr/bin/env bash
# Collect the WCN399x Bluetooth firmware for step B2 (xnu/andromeda_bt.c,
# andrbt=2) into inputs/andromeda/btfw/ (git-ignored: inputs/), with
# SHA256SUMS. make-andromeda-adt.py --btfw then embeds every file there as an
# Apple device tree property /chosen "btfw,<name>", the way the GPU firmware
# travels (gpufw,<name>). Nothing on the phone is touched by this script.
#
# The kernel picks the pair from the chip's ROM version (btqca.c naming):
#   rom 0x0201 (WCN3990 2.1) and 0x1001 (WCN3998)  -> crbtfw21.tlv + crnv21.bin
#   rom 0x0302                                      -> crbtfw32.tlv + crnv32.bin
#   soc_id 0x40014320 (WCN3991)                     -> crbtfw32.tlv + crnv32u.bin
# so both sets are collected until the B1/B2 log shows which one this phone
# needs. BTFW_FILES="..." overrides the list.
#
# Sources, in order of preference:
#   --image IMG          a raw read-only dump of the phone's "bluetooth"
#                        partition (FAT, mounted by Android at
#                        /vendor/bt_firmware; files under image/). Read with
#                        mtools, never mounted. Make the dump the same way
#                        scripts/pull-modem-fw.sh dumps "modem" (TWRP booted
#                        in RAM, adb exec-out dd of by-name/bluetooth), by the
#                        phone's owner.
#   --linux-firmware [DIR]  Qualcomm's files as shipped by linux-firmware
#                        (default /usr/lib/firmware/qca; .zst/.xz are
#                        decompressed). Same names; not the phone's own copy.
# The vendor (super) partition does NOT hold these files: only the HAL that
# names them.
#
# The files are Qualcomm's (linux-firmware: LICENSE.qcom / WHENCE). They are
# never committed and never published by a workflow. See CREDITS.md.
#
# usage: scripts/pull-bt-fw.sh --image bluetooth.img
#        scripts/pull-bt-fw.sh --linux-firmware [/usr/lib/firmware/qca]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${BTFW_OUT:-$ROOT/inputs/andromeda/btfw}"
FILES=(${BTFW_FILES:-crbtfw21.tlv crnv21.bin crbtfw32.tlv crnv32.bin crnv32u.bin})

need() {
  local missing=0 t
  for t in "$@"; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t" >&2; missing=1; }
  done
  [ "$missing" -eq 0 ] || exit 1
}

mode="${1:-}"
case "$mode" in
  --image)
    IMG="${2:?usage: $0 --image bluetooth.img}"
    [ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 1; }
    need mdir mcopy sha256sum
    mkdir -p "$OUT"
    echo "image/ of $IMG:"
    mdir -i "$IMG" ::/image 2>/dev/null | grep -Ei 'tlv|nv|bin' || echo "  (nothing matching; inspect: mdir -i '$IMG' ::/image)"
    for f in "${FILES[@]}"; do
      if mcopy -n -o -i "$IMG" "::/image/$f" "$OUT/$f" 2>/dev/null; then
        echo "  extracted image/$f"
      else
        echo "  not in the image: $f"
      fi
    done
    origin="phone bluetooth partition dump $(basename "$IMG") sha256 $(sha256sum "$IMG" | awk '{print $1}')"
    ;;
  --linux-firmware)
    SRC="${2:-/usr/lib/firmware/qca}"
    [ -d "$SRC" ] || { echo "no such directory: $SRC" >&2; exit 1; }
    need sha256sum
    mkdir -p "$OUT"
    first=""
    for f in "${FILES[@]}"; do
      if [ -f "$SRC/$f" ]; then
        src="$SRC/$f"; cp -L "$src" "$OUT/$f"
      elif [ -f "$SRC/$f.zst" ]; then
        need zstd
        src="$SRC/$f.zst"; zstd -q -d -f "$src" -o "$OUT/$f"
      elif [ -f "$SRC/$f.xz" ]; then
        need xz
        src="$SRC/$f.xz"; xz -d -c "$src" > "$OUT/$f"
      else
        echo "  not in $SRC: $f"
        continue
      fi
      first="${first:-$src}"
      echo "  copied $f"
    done
    pkg=""
    if [ -n "$first" ] && command -v pacman >/dev/null 2>&1; then
      pkg="$(LC_ALL=C pacman -Qo "$first" 2>/dev/null | sed 's/.* is owned by //')" || true
    fi
    origin="linux-firmware ${pkg:-(package version unknown)} from $SRC"
    ;;
  *)
    sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac

# Dotfiles are not embedded by make-andromeda-adt.py; SHA256SUMS is skipped too.
printf '%s\n' "$origin" > "$OUT/.source"
( cd "$OUT" && ls -1 | grep -v '^SHA256SUMS$' | sort | xargs -r sha256sum > SHA256SUMS )
echo "== $OUT"
cat "$OUT/SHA256SUMS"
echo "source: $origin"
