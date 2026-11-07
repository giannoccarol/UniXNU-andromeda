#!/usr/bin/env bash
# Replace the HEIC layers of the stock wallpapers (/Library/Wallpaper) in an
# iOS root image with PNG data, keeping the .HEIC names, and set the Metal
# client list. The phone has no HEVC hardware: ImageIO decodes HEIC to an
# empty image, and with Core Animation on Metal the opaque layer of the iOS 16
# default wallpaper (CollectionsPoster, "WWDC 2022") is drawn black. ImageIO
# sniffs the content, so PNG data under the old name works. Workaround and
# client list ("* -chronod": every process but chronod, whose widgets stay
# white with a device; also without PridePosterExtension, which aborts with a
# Metal device here; Photos is back on Metal since its abort turned out to be
# the sandbox, 2026-11-08) from ios-flame's HANDOFF.md (Nick Wylynko); see
# CREDITS.md. The originals are kept in /private/var/local/wallpaper-heic-backup.
#
# usage: scripts/root-wallpaper-png.sh IMG
# Needs heif-convert (libheif) on this machine and the ios-root-tools image.
# Run scripts' update-root.sh afterwards: it does the fsck, the dyld cache
# check and the sparse image. Nothing leaves this machine.
set -euo pipefail
IMG="${1:?usage: root-wallpaper-png.sh IMG}"
[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }
command -v heif-convert >/dev/null || { echo "missing heif-convert (libheif)" >&2; exit 1; }
IMG_DIR="$(cd "$(dirname "$IMG")" && pwd)"
IMG_BASE="$(basename "$IMG")"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/in" "$W/out"
docker run --rm --privileged -v /dev:/dev -v "$IMG_DIR:/out:ro" -v "$W:/w" ios-root-tools bash -c '
  set -euo pipefail
  mkdir -p /mnt/c
  mount -t hfsplus -o loop,force,ro "/out/$1" /mnt/c
  cd /mnt/c
  find Library/Wallpaper -xdev -iname "*.heic" -type f > /w/list.txt
  while IFS= read -r f; do mkdir -p "/w/in/$(dirname "$f")"; cp "$f" "/w/in/$f"; done < /w/list.txt
  chown -R "$2" /w
  cd /
  umount /mnt/c
' extract "$IMG_BASE" "$(id -u):$(id -g)"
while IFS= read -r f; do
	mkdir -p "$W/out/$(dirname "$f")"
	heif-convert "$W/in/$f" "$W/out/${f%.*}.png" >/dev/null
	[ -f "$W/out/${f%.*}.png" ] || { echo "no PNG for $f" >&2; exit 1; }
done < "$W/list.txt"
echo "converted $(wc -l < "$W/list.txt") wallpaper layers"
docker run --rm --privileged -v /dev:/dev -v "$IMG_DIR:/out" -v "$W:/w:ro" ios-root-tools bash -c '
  set -euo pipefail
  mkdir -p /mnt/r
  mount -t hfsplus -o loop,force,rw "/out/$1" /mnt/r
  cd /mnt/r
  B=private/var/local/wallpaper-heic-backup
  while IFS= read -r f; do
    mkdir -p "$B/$(dirname "$f")"
    [ -e "$B/$f" ] || cp -a "$f" "$B/$f"
    own=$(stat -c %u:%g "$f"); mode=$(stat -c %a "$f")
    cat "/w/out/${f%.*}.png" > "$f"
    chown "$own" "$f"; chmod "$mode" "$f"
  done < /w/list.txt
  printf "* -chronod -PridePosterExtension\n" > private/var/local/mtl2-clients
  chown 0:0 private/var/local/mtl2-clients
  chmod 0644 private/var/local/mtl2-clients
  echo "wallpaper layers written as PNG (originals in /$B); mtl2-clients: * -chronod -PridePosterExtension"
  cd /
  umount /mnt/r
' write "$IMG_BASE"
