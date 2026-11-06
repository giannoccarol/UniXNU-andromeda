#!/usr/bin/env bash
# Read-only data collection from a Xiaomi Mi Mix 3 5G (andromeda) over adb.
#
# Nothing on the phone is changed: partitions are only read (dd into
# /data/local/tmp, pulled, then the temporary copy is deleted).
#
# Without root: properties, memory, SoC and board ids, partitions list, display.
# With root (adb already root, or `su` from Magisk/KernelSU; the phone may ask
# you to grant it): the live device tree, /proc/iomem, and backups of the
# partitions that hold IMEI and calibration (persist, modemst1/2, fsg, fsc)
# plus boot, dtbo and vbmeta.
#
# Output: phone/<serial>/ (ignored by git: it holds the phone's identity data).
# usage: scripts/pull-phone-info.sh [serial]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

pick_serial() {
  local s dev
  for s in $(adb devices | awk 'NR>1 && $2=="device" {print $1}'); do
    dev="$(adb -s "$s" shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
    if [ "$dev" = andromeda ]; then echo "$s"; return 0; fi
    dev="$(adb -s "$s" shell getprop ro.build.product 2>/dev/null | tr -d '\r')"
    if [ "$dev" = andromeda ]; then echo "$s"; return 0; fi
  done
  return 1
}

SERIAL="${1:-}"
if [ -z "$SERIAL" ]; then
  SERIAL="$(pick_serial)" || { echo "No andromeda phone found over adb (adb devices -l):" >&2; adb devices -l >&2; exit 1; }
fi
A=(adb -s "$SERIAL")
DEV="$("${A[@]}" shell getprop ro.product.device | tr -d '\r')"
BP="$("${A[@]}" shell getprop ro.build.product | tr -d '\r')"
if [ "$DEV" != andromeda ] && [ "$BP" != andromeda ]; then
  echo "Refusing: $SERIAL is '$DEV' / '$BP', not andromeda." >&2; exit 1
fi

OUT="$ROOT/phone/$SERIAL"; INFO="$OUT/info"; BK="$OUT/backup"
mkdir -p "$INFO" "$BK"
echo "phone: $SERIAL ($DEV) -> ${OUT#$ROOT/}"

sh_() { "${A[@]}" shell "$@" 2>&1 | tr -d '\r'; }

# Root: adb already root, else su.
if [ "$(sh_ id -u)" = 0 ]; then
  RUN() { sh_ "$1"; }; ROOTED="adb root"
elif [ "$(sh_ 'su -c "id -u"' 2>/dev/null | tail -1)" = 0 ]; then
  RUN() { sh_ "su -c \"$1\""; }; ROOTED="su"
else
  RUN() { sh_ "$1"; }; ROOTED="no"
fi
echo "root: $ROOTED"

echo "== info (no root needed)"
sh_ getprop > "$INFO/getprop.txt"
for p in ro.product.device ro.product.model ro.build.fingerprint ro.build.version.release \
         ro.build.display.id ro.build.type ro.bootloader ro.boot.hwversion ro.boot.hwc \
         ro.boot.verifiedbootstate ro.boot.flash.locked ro.boot.slot_suffix ro.product.board; do
  printf '%-30s %s\n' "$p" "$(grep -F "[$p]" "$INFO/getprop.txt" | sed 's/.*\]: \[\(.*\)\]/\1/')"
done | tee "$INFO/summary.txt"
sh_ cat /proc/meminfo > "$INFO/meminfo.txt"
sh_ uname -a > "$INFO/uname.txt"
for f in soc_id revision machine family; do
  printf '%-10s %s\n' "$f" "$(sh_ cat /sys/devices/soc0/$f)"
done | tee "$INFO/soc0.txt"
for p in qcom,board-id qcom,msm-id qcom,pmic-id model compatible; do
  printf '%-14s %s\n' "$p" "$(sh_ "cat /sys/firmware/devicetree/base/$p 2>/dev/null | od -An -tx1 | tr -s ' \n' ' '")"
done | tee "$INFO/dt-ids.txt"
sh_ wm size > "$INFO/display.txt"; sh_ wm density >> "$INFO/display.txt"
sh_ ls -l /dev/block/by-name/ > "$INFO/partitions.txt" || true
sh_ cat /proc/partitions > "$INFO/proc-partitions.txt" || true
grep MemTotal "$INFO/meminfo.txt"

if [ "$ROOTED" = no ]; then
  echo "No root: skipping the live device tree, /proc/iomem and the partition backups."
  echo "Enable root (Magisk, or 'Rooted debugging' in developer options) and run again."
  exit 0
fi

echo "== info (root)"
RUN "cat /proc/cmdline" > "$INFO/cmdline.txt"
RUN "cat /proc/iomem" > "$INFO/iomem.txt"
RUN "cp /sys/firmware/fdt /data/local/tmp/live-fdt.dtb && chmod 644 /data/local/tmp/live-fdt.dtb" >/dev/null
"${A[@]}" pull /data/local/tmp/live-fdt.dtb "$OUT/live-fdt.dtb" >/dev/null
RUN "rm -f /data/local/tmp/live-fdt.dtb" >/dev/null
dtc -I dtb -O dts -o "$OUT/live-fdt.dts" "$OUT/live-fdt.dtb" 2>/dev/null || true
echo "live FDT: $(wc -c < "$OUT/live-fdt.dtb") bytes; panel: $(grep -o 'msm_drm.dsi_display0=[^ ]*' "$INFO/cmdline.txt" || echo '?')"

echo "== partition backups (read only)"
for part in persist modemst1 modemst2 fsg fsc boot dtbo vbmeta; do
  if ! RUN "test -e /dev/block/by-name/$part && echo yes" | grep -q yes; then
    echo "  $part: not present"; continue
  fi
  RUN "dd if=/dev/block/by-name/$part of=/data/local/tmp/$part.img bs=1M 2>/dev/null; chmod 644 /data/local/tmp/$part.img" >/dev/null
  remote="$(RUN "sha256sum /data/local/tmp/$part.img" | awk '{print $1}')"
  "${A[@]}" pull "/data/local/tmp/$part.img" "$BK/$part.img" >/dev/null
  RUN "rm -f /data/local/tmp/$part.img" >/dev/null
  local_sum="$(sha256sum "$BK/$part.img" | awk '{print $1}')"
  if [ "$remote" = "$local_sum" ]; then
    echo "  $part: $(wc -c < "$BK/$part.img") bytes, sha256 ok"
  else
    echo "  $part: CHECKSUM MISMATCH (phone $remote, PC $local_sum)" >&2; exit 1
  fi
done
(cd "$BK" && sha256sum *.img > SHA256SUMS)
echo "done: ${OUT#$ROOT/} (keep a second copy of backup/ somewhere safe)"
