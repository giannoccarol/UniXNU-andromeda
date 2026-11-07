#!/usr/bin/env bash
# Read-only extraction of the Wi-Fi / modem firmware from a Xiaomi Mi Mix 3 5G
# (andromeda), for docs/research/wifi-flame.md W1-W3. NOTHING on the phone is
# written: the modem partition (a FAT "NON-HLOS" image) is read whole to the
# PC and the files the Wi-Fi needs are pulled out of the image on the PC with
# mtools; the phone is never mounted and userdata (the iOS HFS+ root) is never
# touched.
#
# Method mirrors docs/research/gpu.md 6.3b (vendor extraction): the OFFICIAL
# TWRP 3.7.0 booted ONLY in RAM (fastboot boot, never flash), then dd of the
# whole partition over adb, verified by SHA256 against the phone. The parsing
# of modem.mdt is done offline by scripts/ (not here).
#
# Output: inputs/andromeda/modemfw/ (git-ignored: inputs/). Only per-file
# SHA256SUMS of modem.mdt/.bNN, wlanmdsp.mbn, bdwlan.* and the .jsn files go
# into the research docs; the EFS images and their digests identify the phone
# and stay local (phone/<serial>/backup/ already has them).
#
# This script is a PLAN. It is not run from the agent and must be launched by
# the phone's owner, who confirms each step. It uses bash (the login shell is
# fish) and the host tools listed in need(). See CREDITS.md: the read-only
# "boot TWRP in RAM, dd, mtools" recipe follows nomorecoolnicknames'
# hakios-sm8150 pull-firmware.sh and ios-flame's firmware notes.
#
# usage: scripts/pull-modem-fw.sh [SERIAL]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/inputs/andromeda/modemfw"

need() {
  local missing=0 t
  for t in "$@"; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t" >&2; missing=1; }
  done
  [ "$missing" -eq 0 ] || exit 1
}
need adb fastboot dd mdir mcopy sha256sum

# ---- 0. identify the phone -------------------------------------------------
SERIAL="${1:-}"
pick_fastboot_serial() {
  local s prod
  for s in $(fastboot devices | awk '{print $1}'); do
    prod="$(fastboot -s "$s" getvar product 2>&1 | sed -n 's/^product: *//p' | tr -d '\r')"
    [ "$prod" = andromeda ] && { echo "$s"; return 0; }
  done
  return 1
}

echo "== 1. boot the OFFICIAL TWRP 3.7.0 in RAM (fastboot boot, NEVER flash) =="
echo "   The phone must be in fastboot (bootloader) mode."
: "${TWRP:?set TWRP=/path/to/twrp-3.7.0-andromeda.img (the same image used for gpu.md 6.3b)}"
[ -f "$TWRP" ] || { echo "TWRP image not found: $TWRP" >&2; exit 1; }

if [ -z "$SERIAL" ]; then
  SERIAL="$(pick_fastboot_serial)" || {
    echo "No andromeda phone in fastboot. 'fastboot devices':" >&2; fastboot devices >&2; exit 1; }
fi
prod="$(fastboot -s "$SERIAL" getvar product 2>&1 | sed -n 's/^product: *//p' | tr -d '\r')"
[ "$prod" = andromeda ] || { echo "Refusing: $SERIAL reports product '$prod', not andromeda." >&2; exit 1; }
echo "   phone: $SERIAL (product=$prod)"

echo "   booting TWRP in RAM..."
fastboot -s "$SERIAL" boot "$TWRP"

echo "   waiting for TWRP's adb (grant the prompt on the phone if asked)..."
adb -s "$SERIAL" wait-for-any-device   # TWRP shows up as "recovery", not "device"
# Guard: refuse to continue unless this really is a recovery/TWRP ramdisk, so
# we never dd a live/booted Android that could be mid-write.
STATE="$(adb -s "$SERIAL" get-state 2>/dev/null | tr -d '\r' || true)"
BOOTMODE="$(adb -s "$SERIAL" shell getprop ro.twrp.boot 2>/dev/null | tr -d '\r' || true)"
BUILDTYPE="$(adb -s "$SERIAL" shell getprop ro.build.type 2>/dev/null | tr -d '\r' || true)"
echo "   adb state=$STATE ro.build.type=$BUILDTYPE ro.twrp.boot=$BOOTMODE"
if [ "$STATE" != recovery ] && [ -z "$BOOTMODE" ]; then
  echo "Refusing: this does not look like a recovery/TWRP ramdisk; aborting to keep userdata safe." >&2
  exit 1
fi

# ---- 2. locate the modem block device, read-only --------------------------
BYNAME=/dev/block/bootdevice/by-name
MODEM_DEV="$(adb -s "$SERIAL" shell "ls -l $BYNAME/modem 2>/dev/null" | tr -d '\r' | sed 's/.* -> //')"
[ -n "$MODEM_DEV" ] || MODEM_DEV="$BYNAME/modem"
echo "== 2. modem partition = $MODEM_DEV (read only) =="

mkdir -p "$OUT"
echo "   dd the whole modem partition to the PC (read only)..."
# TWRP's adbd gives 0 bytes for exec-out dd (2026-11-07); pull the block device.
adb -s "$SERIAL" pull "$MODEM_DEV" "$OUT/modem.img"
sz="$(wc -c < "$OUT/modem.img")"
[ "$sz" -gt 0 ] || { echo "modem.img is empty; aborting." >&2; exit 1; }
echo "   got $sz bytes"

echo "   cross-check the SHA256 against the phone (no write on the phone):"
PHONE_SUM="$(adb -s "$SERIAL" shell "sha256sum $BYNAME/modem" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
PC_SUM="$(sha256sum "$OUT/modem.img" | awk '{print $1}')"
echo "     phone: ${PHONE_SUM:-<unavailable>}"
echo "     PC:    $PC_SUM"
if [ -n "$PHONE_SUM" ] && [ "$PHONE_SUM" != "$PC_SUM" ]; then
  echo "CHECKSUM MISMATCH: the copy differs from the phone; aborting." >&2
  exit 1
fi

# ---- 3. extract the Wi-Fi files from the FAT image, on the PC -------------
# The modem FAT image carries the firmware under image/. mtools reads the FAT
# without mounting it; wildcards are single-quoted so mtools (not the shell)
# expands them. Missing files are reported, not fatal (bdwlan.* is one of a
# family; the exact one is chosen at run time by the WLFW CAP board_id).
echo "== 3. list and extract image/ (mtools, no mount) =="
echo "   image/ listing (modem/wlan/bdwlan/jsn):"
mdir -i "$OUT/modem.img" ::/image 2>/dev/null | grep -Ei 'modem|wlan|bdwlan|jsn|mbn' || \
  echo "   (no obvious matches in ::/image; inspect with: mdir -i '$OUT/modem.img' ::/image)"

for pat in 'modem.mdt' 'modem.b*' 'wlanmdsp.mbn' 'bdwlan.*' '*.jsn'; do
  if mcopy -n -m -i "$OUT/modem.img" "::/image/$pat" "$OUT/" 2>/dev/null; then
    echo "   extracted: image/$pat"
  else
    echo "   no file for: image/$pat"
  fi
done

# ---- 4. EFS images: already backed up, keep only a working copy of /readwrite
# The EFS (modemst1/2, fsg, fsc) are the phone's identity (IMEI, calibration);
# scripts/pull-phone-info.sh already backs them up to phone/<serial>/backup/.
# Here we only stage a working copy of the modem /readwrite tree from the
# persist backup if debugfs and that backup are present (optional, read only).
BK="$ROOT/phone/$SERIAL/backup"
if command -v debugfs >/dev/null 2>&1 && [ -f "$BK/persist.img" ]; then
  echo "== 4. working copy of /readwrite from the persist backup (read only) =="
  mkdir -p "$OUT/rw"
  debugfs -R "rdump /rfs/msm/mpss $OUT/rw" "$BK/persist.img" 2>/dev/null \
    && echo "   staged $OUT/rw from persist.img /rfs/msm/mpss" \
    || echo "   /rfs/msm/mpss not found in persist.img (skipped)"
else
  echo "== 4. skipped /readwrite staging (needs debugfs and phone/$SERIAL/backup/persist.img) =="
fi

# ---- 5. SHA256SUMS of the extracted files ---------------------------------
echo "== 5. SHA256SUMS =="
( cd "$OUT" && sha256sum -- * 2>/dev/null | grep -v ' modem.img$' > SHA256SUMS || true )
# modem.img itself identifies the phone; record its digest only in the local
# per-serial area, never in inputs/ which may be shared.
sha256sum "$OUT/modem.img" > "$OUT/modem.img.sha256.local"
cat "$OUT/SHA256SUMS" 2>/dev/null || true

echo
echo "Done. Files in ${OUT#$ROOT/} (git-ignored)."
echo "Next: run the offline modem.mdt program-header check (scripts/, python3 -I),"
echo "put ONLY the per-file SHA256 of modem.mdt/.bNN, wlanmdsp.mbn, bdwlan.* and"
echo "the .jsn files into docs/research/wifi-flame.md 4.2; never commit the files"
echo "or the EFS/modem.img digests. Reboot the phone out of TWRP when finished"
echo "(TWRP was only in RAM; a normal reboot leaves the phone untouched)."
