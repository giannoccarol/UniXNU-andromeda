#!/usr/bin/env bash
# Rebuild the iOS root image from the IPSW, with a working dyld shared cache.
#
# Our old image had zero-length cache catalog records. The exact cause of
# the upstream CI result and its relation to the SpringBoardUI assertion
# are not established. Local copies survive fsck; validate the actual files.
# The unsuffixed cache is a small header; check all subcaches, not just it.
# Adapted from ios-sweet tools/mkiosroot.sh (45263bee).
# Usage: rebuild-root.sh IPSW OUT.img [SIZE]
set -euo pipefail

# Run as root in a container, or as a user with sudo on a host.
if [ "$(id -u)" -eq 0 ]; then
	sudo() { "$@"; }
fi

IPSW="${1:?usage: rebuild-root.sh IPSW OUT.img [SIZE]}"
OUT="${2:?usage: rebuild-root.sh IPSW OUT.img [SIZE]}"
SIZE="${3:-16G}"
[[ $OUT == *.img ]] || { echo 'output must end in .img' >&2; exit 1; }
[[ ! -e $OUT && ! -e ${OUT%.img}.simg ]] || { echo 'refusing to overwrite output' >&2; exit 1; }

# Component names inside the iPhone X 16.7.16 (20H392) IPSW.
OS_DMG=087-86698-044.dmg           # the OS root filesystem
SYSOS_DMG=087-86593-047.dmg        # Cryptex1,SystemOS (holds the dyld cache)
APPOS_DMG=087-86715-047.dmg        # Cryptex1,AppOS

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prepare="${PREPARE_IOS_ROOT:-$here/prepare-ios-root.py}"
[[ -f $prepare ]] || prepare="$here/../cepheus/scripts/prepare-ios-root.py"
[[ -f $prepare && -f $here/sweet-root-tweaks.py ]] || { echo 'missing root preparation helpers' >&2; exit 1; }
for tool in apfs-fuse mkfs.hfsplus fsck.hfsplus rsync img2simg unzip python3 mountpoint; do
	command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
work="$(mktemp -d)"
cleanup() {
	local busy=0
	for m in "$work/chk" "$work/root" "$work/os" "$work/sysos" "$work/appos"; do
		if mountpoint -q "$m"; then
			sudo umount "$m" 2>/dev/null || sudo fusermount -u "$m" 2>/dev/null || true
		fi
		mountpoint -q "$m" && busy=1
	done
	# Never recurse into a still-mounted image, especially a writable root.
	if [[ $busy == 0 ]]; then rm -rf "$work"; else echo "left mounted under $work" >&2; fi
}
trap cleanup EXIT

echo "== extracting the three components =="
mkdir -p "$work/dmg"
unzip -j -o "$IPSW" "$OS_DMG" "$SYSOS_DMG" "$APPOS_DMG" -d "$work/dmg"
ls -l "$work/dmg"

# apfs-fuse splits options on ',', which iPhone10,3 paths contain: go through
# a symlink, and keep the .dmg suffix (it only parses UDIF by name).
apfs_mount() {
	ln -s "$1" "$work/$2.dmg"
	mkdir -p "$work/$2"
	sudo apfs-fuse "$work/$2.dmg" "$work/$2"
}
apfs_mount "$work/dmg/$OS_DMG" os
apfs_mount "$work/dmg/$SYSOS_DMG" sysos
apfs_mount "$work/dmg/$APPOS_DMG" appos

echo "== checking apfs-fuse actually read the cryptex =="
dsc_rel="private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
probe_dir="$work/sysos/root/System/Library/Caches/com.apple.dyld"
if [ ! -d "$probe_dir" ]; then
	echo "ERROR: $probe_dir missing - apfs-fuse did not expose the cache" >&2
	exit 1
fi
# The cache is split: the unsuffixed file is a few hundred KB of header and
# the .01/.03/... shards hold the bulk, so check the directory total.
probe_total=$(find "$probe_dir" -type f -printf '%s\n' | awk '{s+=$1} END {print s+0}')
echo "dyld shared cache in the cryptex: $probe_total bytes ($(du -sh "$probe_dir" 2>/dev/null | cut -f1))"
if [ "$probe_total" -lt 1073741824 ]; then
	echo "ERROR: the cryptex cache totals $probe_total bytes; expected > 1 GiB." >&2
	echo "apfs-fuse cannot read this APFS image; do not build a root from it." >&2
	exit 1
fi

echo "== creating $OUT ($SIZE) =="
truncate -s "$SIZE" "$OUT"
mkfs.hfsplus -s -J -v iOSRoot "$OUT"
mkdir -p "$work/root"
sudo mount -t hfsplus -o loop,force "$OUT" "$work/root"

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	sudo mkdir -p "$2"
	sudo rsync -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os/root" "$work/root"
copy "$work/sysos/root" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos/root" "$work/root/private/preboot/Cryptexes/App"

echo "== placing the dyld cache where dyld looks at ignition level 1 =="
# Copy rather than use HFS+ hard links: link records can have empty data
# forks that a minimal reader cannot resolve. Local independent copies have
# been verified after fsck. This costs an extra ~3 GiB of the 16 GiB volume.
sudo mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc_rel"/*; do
	sudo cp -a "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

echo "== copying cryptex files that have no counterpart in the OS root =="
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" &&
	 sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! sudo test -e "$work/root/$f" && ! sudo test -L "$work/root/$f"; then
			sudo mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			# Copy, not hard-link: see the dyld cache note above.
			sudo cp -a "$src" "$work/root/$f"
		fi
	done
done

echo "== restore-time keybag and logd stores =="
sudo python3 "$prepare" prepare "$work/root"

echo "== bring-up tweaks =="
sudo python3 "$here/sweet-root-tweaks.py" "$work/root"

# iOS programs of this repository (phone/<name>/, built by the
# userland-andromeda workflow into inputs/<board>/userland/): each
# install-root.sh copies its files and launchd job, or warns if not built.
# Modem firmware for the Wi-Fi driver (andrwlan=2 reads /fw/modem), if it
# was pulled with scripts/pull-modem-fw.sh. Stays on this machine.
if [ -d /modemfw ] && [ -n "$(ls -A /modemfw 2>/dev/null)" ]; then
	echo "== modem firmware -> /fw/modem =="
	sudo mkdir -p "$work/root/fw/modem"
	sudo cp -a /modemfw/. "$work/root/fw/modem/"
fi
if [ -d /phone ]; then
	echo "== phone programs =="
	for script in /phone/*/install-root.sh; do
		[ -f "$script" ] || continue
		sudo bash "$script" "$work/root" /userland
	done
fi

echo "== verify the cache before unmounting =="
sudo python3 "$prepare" verify "$work/root"

echo "== unmount, fsck, sparse =="
sudo umount "$work/root"
sudo fsck.hfsplus -f -y "$OUT" || true
sudo fsck.hfsplus -f -n "$OUT"

# Re-check after fsck: if the cache went to zero here, the repair rewrote the
# files and the image is unusable even though fsck said "OK".
sudo mkdir -p "$work/chk"
sudo mount -t hfsplus -o loop,force,ro "$OUT" "$work/chk"
cache_bytes=$(sudo find "$work/chk/System/Library/Caches/com.apple.dyld" -type f -printf '%s\n' | awk '{s+=$1} END {print s+0}')
echo "dyld cache after fsck: $cache_bytes bytes"
sudo python3 "$prepare" verify "$work/chk"
sudo umount "$work/chk"
if [ "$cache_bytes" -lt 1073741824 ]; then
	echo "ERROR: fsck emptied the dyld cache ($cache_bytes bytes)." >&2
	exit 1
fi
img2simg "$OUT" "${OUT%.img}.simg"
ls -l "$OUT" "${OUT%.img}.simg" 2>/dev/null || true
echo "DONE (offline verified, not yet boot tested): $OUT"
