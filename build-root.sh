#!/usr/bin/env bash
# Phase 3: build the HFS+ iOS root that the kernel mounts from userdata
# (rd=ufsroot0), from an IPSW you downloaded from Apple yourself.
#
# Runs scripts/rebuild-root.sh (iPhone X iOS 16.7.16, 20H392) in the
# ios-root-tools container (build/Dockerfile.apfs + build/Dockerfile.root).
# The container is privileged with /dev mounted: loop (HFS+) and FUSE
# (apfs-fuse) mounts need it. Work files go to out/<name>/root/tmp on this
# disk, not inside Docker. The result is never published: Apple's root
# filesystem stays on this machine.
#
# Output: out/<name>/root/ios16-root.img (16 GiB HFS+) and ios16-root.simg
# (Android sparse, for `fastboot flash userdata`).
# usage: ./build-root.sh [IPSW] [SIZE]
set -euo pipefail
cd "$(dirname "$0")"
NAME="$(python3 -c 'import tomllib; print(tomllib.load(open("board.toml","rb"))["board"]["name"])')"
IPSW="${1:-inputs/ipsw/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw}"
SIZE="${2:-16G}"
OUT="out/$NAME/root"
[ -f "$IPSW" ] || { echo "missing IPSW: $IPSW" >&2; exit 1; }
[ ! -e "$OUT/ios16-root.img" ] || { echo "$OUT/ios16-root.img exists: move it away first" >&2; exit 1; }
mkdir -p "$OUT/tmp"
docker image inspect ios-root-tools >/dev/null 2>&1 || {
  docker build -q -t apfs-tools -f build/Dockerfile.apfs build/ >/dev/null
  docker build -q -t ios-root-tools -f build/Dockerfile.root build/ >/dev/null
}
IPSW_DIR="$(cd "$(dirname "$IPSW")" && pwd)"
exec docker run --rm --privileged -v /dev:/dev -e TMPDIR=/out/tmp \
  -v "$PWD/scripts:/scripts:ro" -v "$IPSW_DIR:/ipsw:ro" -v "$PWD/$OUT:/out" \
  ios-root-tools bash -c '
    rc=0
    bash /scripts/rebuild-root.sh "/ipsw/$1" /out/ios16-root.img "$2" || rc=$?
    rmdir /out/tmp 2>/dev/null || true
    chown -R "$3" /out
    exit $rc
  ' build-root "$(basename "$IPSW")" "$SIZE" "$(id -u):$(id -g)"
