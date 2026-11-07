#!/usr/bin/env bash
# Update an existing iOS root image in place with this repository's iOS
# programs (phone/*/install-root.sh), instead of rebuilding it from the IPSW
# with build-root.sh. Same container, same checks as scripts/rebuild-root.sh:
# the dyld cache is verified before unmounting and again after fsck, and the
# Android sparse image for `fastboot flash userdata` is written next to it.
#
# usage: ./update-root.sh [IMG] [GPU_USERLAND]
#   IMG           default out/<name>/root/ios16-root.img (modified in place;
#                 keep a copy first, e.g. cp --reflink=always on btrfs)
#   GPU_USERLAND  directory holding turnip/ and metal2/ from the
#                 gpu-userland-andromeda workflow (artifact
#                 andromeda-gpu-userland); default: the newest
#                 inputs/<name>/gpu-userland-*/andromeda-gpu-userland
# The other programs come from inputs/<name>/userland (userland-andromeda).
# Nothing leaves this machine: the root contains Apple's files.
set -euo pipefail
cd "$(dirname "$0")"
NAME="$(python3 -c 'import tomllib; print(tomllib.load(open("board.toml","rb"))["board"]["name"])')"
IMG="${1:-out/$NAME/root/ios16-root.img}"
GPU="${2:-$(ls -d inputs/"$NAME"/gpu-userland-*/andromeda-gpu-userland 2>/dev/null | sort | tail -1)}"
[ -f "$IMG" ] || { echo "missing root image: $IMG" >&2; exit 1; }
[ -n "$GPU" ] && [ -d "$GPU" ] || { echo "missing GPU userland directory" >&2; exit 1; }
docker image inspect ios-root-tools >/dev/null 2>&1 || { echo "build the ios-root-tools image first (./build-root.sh does)" >&2; exit 1; }
IMG_DIR="$(cd "$(dirname "$IMG")" && pwd)"
IMG_BASE="$(basename "$IMG")"
SIMG_BASE="${IMG_BASE%.img}.simg"
GPU_DIR="$(cd "$GPU" && pwd)"
exec docker run --rm --privileged -v /dev:/dev -e TURNIP_SMOKE="${TURNIP_SMOKE:-0}" \
  -v "$PWD/scripts:/scripts:ro" -v "$IMG_DIR:/out" \
  -v "$PWD/phone:/phone:ro" -v "$PWD/inputs/$NAME/userland:/userland:ro" \
  -v "$GPU_DIR:/gpu:ro" \
  ios-root-tools bash -c '
    set -euo pipefail
    img=/out/$1; simg=/out/$2
    mkdir -p /mnt/root /mnt/chk
    echo "== mount $1 read-write =="
    mount -t hfsplus -o loop,force,rw "$img" /mnt/root
    echo "== phone programs =="
    for script in /phone/*/install-root.sh; do
      [ -f "$script" ] || continue
      case "$script" in
        /phone/turnip/*|/phone/metal2/*) bash "$script" /mnt/root /gpu ;;
        *) bash "$script" /mnt/root /userland ;;
      esac
    done
    echo "== verify the cache before unmounting =="
    python3 /scripts/prepare-ios-root.py verify /mnt/root
    umount /mnt/root
    echo "== fsck =="
    fsck.hfsplus -f -y "$img" || true
    fsck.hfsplus -f -n "$img"
    mount -t hfsplus -o loop,force,ro "$img" /mnt/chk
    cache_bytes=$(find /mnt/chk/System/Library/Caches/com.apple.dyld -type f -printf "%s\n" | awk "{s+=\$1} END {print s+0}")
    echo "dyld cache after fsck: $cache_bytes bytes"
    python3 /scripts/prepare-ios-root.py verify /mnt/chk
    ls -la /mnt/chk/System/Library/Extensions/SweetMetal2.bundle /mnt/chk/private/var/local/lib/libvulkan_freedreno.dylib 2>&1 || true
    umount /mnt/chk
    if [ "$cache_bytes" -lt 1073741824 ]; then
      echo "ERROR: fsck emptied the dyld cache ($cache_bytes bytes)." >&2
      exit 1
    fi
    rm -f "$simg"
    img2simg "$img" "$simg"
    chown "$3" "$img" "$simg"
    ls -l "$img" "$simg"
    echo "DONE (offline verified, not yet boot tested): $img"
  ' update-root "$IMG_BASE" "$SIMG_BASE" "$(id -u):$(id -g)"
