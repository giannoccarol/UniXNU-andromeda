#!/usr/bin/env bash
# Boot without a PC: write the UEFI + XNU image to the phone's `boot`
# partition, or put Android's original boot image back.
#
#   scripts/install-boot.sh SERIAL            write out/<name>/boot/boot-<name>-xnu.img
#   scripts/install-boot.sh SERIAL --restore  write phone/SERIAL/backup/boot.img back
#
# This WRITES A PARTITION. It only runs with the phone in fastboot, checks
# the serial, the image and the Android backup first, and asks for an
# explicit "SI" before writing. It never touches userdata (iOS) or any other
# partition. Afterwards a normal power-on starts iOS; power + volume down
# still opens fastboot, and --restore brings Android's kernel back (Android
# itself also needs its userdata back: `fastboot -w` or a ROM install).
set -euo pipefail
cd "$(dirname "$0")/.."
SERIAL="${1:?usage: install-boot.sh SERIAL [--restore]}"
MODE="${2:-install}"
NAME="$(python3 -c 'import tomllib; print(tomllib.load(open("board.toml","rb"))["board"]["name"])')"
BACKUP="phone/$SERIAL/backup"
IMAGE="out/$NAME/boot/boot-$NAME-xnu.img"

fastboot devices | grep -q "^$SERIAL[[:space:]]*fastboot" ||
  { echo "phone $SERIAL is not in fastboot" >&2; exit 1; }
[ -s "$BACKUP/boot.img" ] && [ -s "$BACKUP/SHA256SUMS" ] ||
  { echo "no Android boot backup in $BACKUP: refusing" >&2; exit 1; }
(cd "$BACKUP" && grep ' boot.img$' SHA256SUMS | sha256sum -c --quiet) ||
  { echo "the Android boot backup does not match its checksum: refusing" >&2; exit 1; }

case "$MODE" in
  install)
    [ -s "$IMAGE" ] || { echo "missing $IMAGE (./build-boot.sh)" >&2; exit 1; }
    size=$(stat -c %s "$IMAGE")
    max=$(stat -c %s "$BACKUP/boot.img")   # the backup is the whole partition
    [ "$size" -le "$max" ] || { echo "image ($size B) larger than the partition ($max B)" >&2; exit 1; }
    head -c 8 "$IMAGE" | grep -q 'ANDROID!' || { echo "$IMAGE is not an Android boot image" >&2; exit 1; }
    WRITE="$IMAGE"
    echo "Write the iOS boot image to $SERIAL's boot partition:"
    echo "  $IMAGE  $size bytes  sha256 $(sha256sum "$IMAGE" | cut -c1-16)"
    if [ -f "out/$NAME/payload/payload.json" ]; then
      python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("  boot-args:", d.get("kernel_args", "?"))' \
        "out/$NAME/payload/payload.json" 2>/dev/null || true
    fi
    echo "Android's boot image stays in $BACKUP/boot.img (install-boot.sh $SERIAL --restore)."
    ;;
  --restore)
    WRITE="$BACKUP/boot.img"
    echo "Write Android's original boot image back to $SERIAL's boot partition:"
    echo "  $WRITE  sha256 $(sha256sum "$WRITE" | cut -c1-16)"
    ;;
  *) echo "unknown mode: $MODE" >&2; exit 1 ;;
esac

read -r -p 'Type SI to write the boot partition: ' answer
[ "$answer" = "SI" ] || { echo "nothing written"; exit 1; }
fastboot -s "$SERIAL" flash boot "$WRITE"
echo "done: boot partition written. 'fastboot -s $SERIAL reboot' starts it."
