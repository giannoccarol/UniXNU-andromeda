#!/usr/bin/env bash
# Install AndromedaAudio.driver into a mounted iOS root (hook of
# scripts/rebuild-root.sh, which runs it after sweet-root-tweaks.py).
#
#   phone/andromeda-audio/install-root.sh ROOT OUTDIR
#
# ROOT    the HFS+ iOS root, mounted read-write; run as root (files are
#         root:wheel).
# OUTDIR  what build.sh got as OUT: the bundle is
#         OUTDIR/andromeda-audio/AndromedaAudio.driver (OUTDIR may also be
#         that andromeda-audio directory itself).
#
# 1. Installs the bundle as
#    /System/Library/Audio/Plug-Ins/HAL/AndromedaAudio.driver (where the audio
#    server loads AudioServerPlugIns, as HakiOS installs HakiosAudio.driver):
#    directories 0755, files 0644, the executable 0755, root:wheel. A bundle
#    has to live there, not in /private/var/local/bin.
# 2. Puts VirtualAudio.plugin back from /Library/Audio/Plug-Ins/HAL.disabled
#    into /Library/Audio/Plug-Ins/HAL, where sweet-root-tweaks.py (audio())
#    parked it because mediaserverd crashed without any audio device. With our
#    devices it has what it needs. StartRecorderServer=false (the other half of
#    audio()) stays as it is.
#
# Idempotent. Without a built bundle it only warns and changes nothing (and so
# VirtualAudio stays parked: without our devices it would crash mediaserverd).
# Undo: delete the bundle and run sweet-root-tweaks.py again (it parks
# VirtualAudio again).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
usage='usage: install-root.sh ROOT OUTDIR'
ROOT=${1:?$usage}
OUTDIR=${2:?$usage}
ROOT=$(cd "$ROOT" && pwd -P)
[[ $ROOT != / ]] || { echo 'andromeda-audio: refusing to install into /' >&2; exit 1; }
[[ -f $ROOT/System/Library/xpc/launchd.plist ]] || { echo "andromeda-audio: $ROOT is not an iOS root" >&2; exit 1; }
hal=$ROOT/System/Library/Audio/Plug-Ins/HAL
[[ -d $hal ]] || { echo "andromeda-audio: $hal missing: not an iOS 16 root?" >&2; exit 1; }
name=AndromedaAudio.driver
exe=AndromedaAudio

src=
for candidate in "$OUTDIR/andromeda-audio/$name" "$OUTDIR/$name"; do
	if [[ -f $candidate/$exe && -f $candidate/Info.plist ]]; then
		src=$candidate
		break
	fi
done
if [[ -z $src ]]; then
	echo "andromeda-audio: no $name under $OUTDIR (build it with phone/andromeda-audio/build.sh on macOS); not installed, VirtualAudio left as it is" >&2
	exit 0
fi
# 64-bit Mach-O (MH_MAGIC_64, little endian), arm64 (CPU_TYPE_ARM64 0x0100000c).
header=$(od -An -tx1 -N8 "$src/$exe" | tr -d ' \n')
[[ $header == cffaedfe0c000001 ]] || { echo "andromeda-audio: $src/$exe is not an arm64 Mach-O" >&2; exit 1; }
# The Info.plist must name our executable and factory, under the AudioServerPlugIn type.
python3 -I - "$src/Info.plist" "$here/Info.plist" <<'PY'
import plistlib, sys

built, source = (plistlib.load(open(p, 'rb')) for p in sys.argv[1:3])
plugin_type = '443ABAB8-E7B3-491A-B985-BEB9187030DB'
for key in ('CFBundleExecutable', 'CFPlugInTypes', 'CFPlugInFactories'):
    if built.get(key) != source.get(key):
        sys.exit(f'andromeda-audio: {sys.argv[1]}: {key} differs from {sys.argv[2]}')
factories = built['CFPlugInTypes'].get(plugin_type) or []
if len(factories) != 1 or built['CFPlugInFactories'].get(factories[0]) != 'AndromedaAudio_Create':
    sys.exit(f'andromeda-audio: {sys.argv[1]}: no AudioServerPlugIn factory AndromedaAudio_Create')
PY

# 1. The bundle: assembled next to its place, then moved in (never half there).
tmp=$hal/.andromeda-audio.tmp
rm -rf "$tmp"
mkdir "$tmp"
cp -R "$src/." "$tmp/"
chown -R 0:0 "$tmp"
find "$tmp" -type d -exec chmod 0755 {} +
find "$tmp" -type f -exec chmod 0644 {} +
chmod 0755 "$tmp/$exe"
rm -rf "${hal:?}/$name"
mv "$tmp" "$hal/$name"
echo "andromeda-audio: installed ${hal#"$ROOT"}/$name from $src ($(find "$hal/$name" -type f | wc -l) files)"

# 2. VirtualAudio back where the audio server loads it.
lib=$ROOT/Library/Audio/Plug-Ins
if [[ -d $lib/HAL/VirtualAudio.plugin ]]; then
	echo "andromeda-audio: VirtualAudio.plugin already in /Library/Audio/Plug-Ins/HAL"
	if [[ -e $lib/HAL.disabled/VirtualAudio.plugin ]]; then
		echo "andromeda-audio: warning: a second VirtualAudio.plugin is in HAL.disabled; left there" >&2
	fi
elif [[ -d $lib/HAL.disabled/VirtualAudio.plugin ]]; then
	if [[ ! -d $lib/HAL ]]; then
		install -d -o 0 -g 0 -m 0755 "$lib/HAL"
	fi
	mv "$lib/HAL.disabled/VirtualAudio.plugin" "$lib/HAL/VirtualAudio.plugin"
	rmdir "$lib/HAL.disabled" 2>/dev/null || true
	echo "andromeda-audio: VirtualAudio.plugin moved back from HAL.disabled to /Library/Audio/Plug-Ins/HAL"
else
	echo "andromeda-audio: warning: no VirtualAudio.plugin in $lib/HAL or HAL.disabled: iOS will not route sound to our devices" >&2
fi
