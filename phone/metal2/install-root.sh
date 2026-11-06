#!/usr/bin/env bash
# Install SweetMetal2.bundle (Metal on metal2vulkan on Turnip) into a mounted
# iOS root (hook of scripts/rebuild-root.sh).
#
#   phone/metal2/install-root.sh ROOT OUTDIR
#
# ROOT    the HFS+ iOS root, mounted read-write; run as root (files are
#         root:wheel).
# OUTDIR  what build.sh got as OUT: the bundle is
#         OUTDIR/metal2/SweetMetal2.bundle (OUTDIR may also be that metal2
#         directory itself). It comes from the gpu-userland-andromeda workflow
#         (artifact andromeda-gpu-userland).
#
# Installs:
#   /System/Library/Extensions/SweetMetal2.bundle   where Metal.framework loads
#       the plug-in an IOAcceleratorES service names in MetalPluginName
#       (directories 0755, files 0644, executables 0755, root:wheel; built
#       next to its place, then moved in)
#   /private/var/local/bin/m2vtest                  the bridge's self-test
#   /private/var/local/share/doc/metal2/            NOTICE.txt, BUILDINFO.txt,
#       licenses/ (metal2vulkan's GPL-2.0, GPL-3.0 and LGPL-3.0 texts, MIT
#       notices, crates). The corresponding source stays in OUTDIR/metal2/source/
#       (not copied: the root is never distributed; NOTICE.txt says where it is).
#   For the bundle's run-time files, only when absent (ios-flame's layout):
#   /private/var/local/m2v-cache (0777)             shader caches per process
#   /private/var/local/mtl2-clients                 "SpringBoard": processes
#       besides backboardd that get a Metal device (ios-flame: without
#       SpringBoard the unlock swipe does nothing)
# Idempotent. Without a built bundle it only warns and changes nothing.
#
# Inactive until the kernel side exists: Metal.framework loads the bundle only
# when an IOAcceleratorES service publishes MetalPluginName "SweetMetal2" and
# MetalPluginClassName "SweetMTL2Device" (ios-flame patches 0096/0097/0116,
# boot-arg sweetmtl=2 there). Our kernel has no such service yet; when it is
# ported it will be published only behind its own boot-arg (CONTRIBUTING.md 4).
# Undo: delete the bundle (and the files above).
set -euo pipefail
usage='usage: install-root.sh ROOT OUTDIR'
ROOT=${1:?$usage}
OUTDIR=${2:?$usage}
name=metal2
bundle=SweetMetal2.bundle
exe=SweetMetal2
class=SweetMTL2Device
ROOT=$(cd "$ROOT" && pwd -P)
[[ $ROOT != / ]] || { echo "$name: refusing to install into /" >&2; exit 1; }
[[ -f $ROOT/System/Library/xpc/launchd.plist ]] || { echo "$name: $ROOT is not an iOS root" >&2; exit 1; }
ext=$ROOT/System/Library/Extensions
[[ -d $ext ]] || { echo "$name: $ext missing: not an iOS 16 root?" >&2; exit 1; }

src=
for candidate in "$OUTDIR/metal2" "$OUTDIR"; do
	if [[ -f $candidate/$bundle/$exe ]]; then
		src=$candidate
		break
	fi
done
if [[ -z $src ]]; then
	echo "$name: no $bundle under $OUTDIR (build it with the gpu-userland-andromeda workflow); not installed" >&2
	exit 0
fi

# 64-bit Mach-O (MH_MAGIC_64, little endian), arm64 (CPU_TYPE_ARM64 0x0100000c).
check_arm64() {
	local header
	header=$(od -An -tx1 -N8 "$1" | tr -d ' \n')
	[[ $header == cffaedfe0c000001 ]] || { echo "$name: $1 is not an arm64 Mach-O" >&2; exit 1; }
}
for f in "$exe" libvulkan_freedreno.dylib; do
	[[ -f $src/$bundle/$f ]] || { echo "$name: incomplete bundle: no $src/$bundle/$f" >&2; exit 1; }
	check_arm64 "$src/$bundle/$f"
done
[[ -f $src/$bundle/_CodeSignature/CodeResources ]] ||
	{ echo "$name: $src/$bundle is not signed (no _CodeSignature/CodeResources)" >&2; exit 1; }
# The Info.plist must name the executable and the class the kernel will name.
python3 -I - "$src/$bundle/Info.plist" "$exe" "$class" <<'PY'
import plistlib, sys

path, exe, cls = sys.argv[1:4]
with open(path, 'rb') as f:
    info = plistlib.load(f)
for key, want in (('CFBundleExecutable', exe), ('NSPrincipalClass', cls), ('CFBundlePackageType', 'BNDL')):
    if info.get(key) != want:
        sys.exit(f'metal2: {path}: {key} is {info.get(key)!r}, expected {want!r}')
PY

# 1. The bundle, assembled next to its place, then moved in (never half there).
tmp=$ext/.$bundle.tmp
rm -rf "$tmp"
mkdir "$tmp"
cp -R "$src/$bundle/." "$tmp/"
chown -R 0:0 "$tmp"
find "$tmp" -type d -exec chmod 0755 {} +
find "$tmp" -type f -exec chmod 0644 {} +
chmod 0755 "$tmp/$exe" "$tmp/libvulkan_freedreno.dylib"
rm -rf "${ext:?}/$bundle"
mv "$tmp" "$ext/$bundle"
echo "$name: installed ${ext#"$ROOT"}/$bundle from $src ($(find "$ext/$bundle" -type f | wc -l | tr -d ' ') files)"

# 2. m2vtest and the notices.
for d in private/var/local:0755 private/var/local/bin:0700 private/var/local/share:0755 \
	private/var/local/share/doc:0755; do
	if [[ ! -d $ROOT/${d%:*} ]]; then
		install -d -o 0 -g 0 -m "${d#*:}" "$ROOT/${d%:*}"
	fi
done
if [[ -f $src/m2vtest ]]; then
	check_arm64 "$src/m2vtest"
	install -o 0 -g 0 -m 0755 "$src/m2vtest" "$ROOT/private/var/local/bin/m2vtest"
	echo "$name: installed /private/var/local/bin/m2vtest"
fi
doc=$ROOT/private/var/local/share/doc/$name
tmp=$doc.tmp
rm -rf "$tmp"
mkdir "$tmp"
for item in NOTICE.txt BUILDINFO.txt licenses; do
	[[ -e $src/$item ]] && cp -R "$src/$item" "$tmp/"
done
chown -R 0:0 "$tmp"
find "$tmp" -type d -exec chmod 0755 {} +
find "$tmp" -type f -exec chmod 0644 {} +
rm -rf "$doc"
mv "$tmp" "$doc"
[[ -f $doc/NOTICE.txt ]] || echo "$name: warning: no NOTICE.txt in $src (licence notices missing)" >&2

# 3. Run-time files of the bundle, never overwritten (they are settings).
if [[ ! -d $ROOT/private/var/local/m2v-cache ]]; then
	install -d -o 0 -g 0 -m 0777 "$ROOT/private/var/local/m2v-cache"
	echo "$name: created /private/var/local/m2v-cache"
fi
clients=$ROOT/private/var/local/mtl2-clients
if [[ ! -e $clients ]]; then
	printf 'SpringBoard\n' >"$clients"
	chown 0:0 "$clients"
	chmod 0644 "$clients"
	echo "$name: created /private/var/local/mtl2-clients (SpringBoard)"
fi
