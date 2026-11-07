#!/usr/bin/env bash
# Install Turnip and its smoke tests into a mounted iOS root (hook of
# scripts/rebuild-root.sh).
#
#   phone/turnip/install-root.sh ROOT OUTDIR
#
# ROOT    the HFS+ iOS root, mounted read-write; run as root (files are
#         root:wheel).
# OUTDIR  what build.sh got as OUT: the files are in OUTDIR/turnip/ (OUTDIR may
#         also be that turnip directory itself). They come from the
#         gpu-userland-andromeda workflow (artifact andromeda-gpu-userland).
#
# Installs:
#   /private/var/local/lib/libvulkan_freedreno.dylib  (0755; vktest's default)
#   /private/var/local/bin/vktest, v2test             (0755, when built)
#   /private/var/local/share/doc/turnip/              NOTICE.txt, BUILDINFO.txt,
#                                                     licenses/, patches/
# Idempotent. Without a built library it only warns and changes nothing.
#
# Nothing here starts by itself unless TURNIP_SMOKE=1 installs the vktest and
# v2test launchd jobs (see the end of this file); the library is opened
# only by vktest or by SweetMetal2.bundle (phone/metal2). Both need the
# kernel's NativeAdreno ABI v1 user client on IOAcceleratorES (ios-flame patch
# 0115, not ported yet), which our kernel will only publish behind its own
# boot-arg; until then v2test and vktest just report that the service is
# missing.
set -euo pipefail
usage='usage: install-root.sh ROOT OUTDIR'
ROOT=${1:?$usage}
OUTDIR=${2:?$usage}
name=turnip
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$ROOT" && pwd -P)
[[ $ROOT != / ]] || { echo "$name: refusing to install into /" >&2; exit 1; }
[[ -f $ROOT/System/Library/xpc/launchd.plist ]] || { echo "$name: $ROOT is not an iOS root" >&2; exit 1; }

src=
for candidate in "$OUTDIR/turnip" "$OUTDIR"; do
	if [[ -f $candidate/libvulkan_freedreno.dylib ]]; then
		src=$candidate
		break
	fi
done
if [[ -z $src ]]; then
	echo "$name: no libvulkan_freedreno.dylib under $OUTDIR (build it with the gpu-userland-andromeda workflow); not installed" >&2
	exit 0
fi

# 64-bit Mach-O (MH_MAGIC_64, little endian), arm64 (CPU_TYPE_ARM64 0x0100000c).
check_arm64() {
	local header
	header=$(od -An -tx1 -N8 "$1" | tr -d ' \n')
	[[ $header == cffaedfe0c000001 ]] || { echo "$name: $1 is not an arm64 Mach-O" >&2; exit 1; }
}
check_arm64 "$src/libvulkan_freedreno.dylib"
tools=()
for tool in vktest v2test; do
	if [[ -f $src/$tool ]]; then
		check_arm64 "$src/$tool"
		tools+=("$tool")
	else
		echo "$name: warning: $src/$tool missing; not installed" >&2
	fi
done

# Directories as for andromedad (bin 0700); an existing directory keeps its
# owner and mode.
for d in private/var/local:0755 private/var/local/bin:0700 private/var/local/lib:0755 \
	private/var/local/share:0755 private/var/local/share/doc:0755; do
	if [[ ! -d $ROOT/${d%:*} ]]; then
		install -d -o 0 -g 0 -m "${d#*:}" "$ROOT/${d%:*}"
	fi
done
install -o 0 -g 0 -m 0755 "$src/libvulkan_freedreno.dylib" "$ROOT/private/var/local/lib/libvulkan_freedreno.dylib"
for tool in ${tools[@]+"${tools[@]}"}; do
	install -o 0 -g 0 -m 0755 "$src/$tool" "$ROOT/private/var/local/bin/$tool"
done

# Licence notices next to the binaries, replaced as a whole.
doc=$ROOT/private/var/local/share/doc/$name
tmp=$doc.tmp
rm -rf "$tmp"
mkdir "$tmp"
for item in NOTICE.txt BUILDINFO.txt licenses patches; do
	[[ -e $src/$item ]] && cp -R "$src/$item" "$tmp/"
done
chown -R 0:0 "$tmp"
find "$tmp" -type d -exec chmod 0755 {} +
find "$tmp" -type f -exec chmod 0644 {} +
rm -rf "$doc"
mv "$tmp" "$doc"
[[ -f $doc/NOTICE.txt ]] || echo "$name: warning: no NOTICE.txt in $src (licence notices missing)" >&2

echo "$name: installed /private/var/local/lib/libvulkan_freedreno.dylib${tools[*]+ and ${tools[*]} in /private/var/local/bin} from $src"

# Phase F5 smoke tests as launchd jobs, only on request (TURNIP_SMOKE=1, e.g.
# ./update-root.sh with TURNIP_SMOKE=1 in the environment): without it any
# earlier smoke job is removed from the job cache. See the plists for why
# they are launchd jobs (no shell in the root) and when they run.
python3 -I - "$ROOT/System/Library/xpc/launchd.plist" "$here" "${TURNIP_SMOKE:-0}" ${tools[@]+"${tools[@]}"} <<'PY'
import os, plistlib, sys

cache, here, smoke, tools = sys.argv[1], sys.argv[2], sys.argv[3] == '1', sys.argv[4:]
st = os.stat(cache)
with open(cache, 'rb') as f:
    plist = plistlib.load(f)
jobs = plist['LaunchDaemons']
labels = {f'org.andromeda.gpu-{t}' for t in ('v2test', 'vktest')}
for k in [k for k, j in jobs.items() if j.get('Label') in labels]:
    del jobs[k]
added = []
if smoke:
    for tool in tools:
        with open(os.path.join(here, f'org.andromeda.gpu-{tool}.plist'), 'rb') as f:
            job = plistlib.load(f)
        jobs[f"/System/Library/LaunchDaemons/{job['Label']}.plist"] = job
        added.append(job['Label'])
tmp = cache + '.turnip.tmp'
with open(tmp, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
os.chown(tmp, st.st_uid, st.st_gid)
os.chmod(tmp, st.st_mode & 0o7777)
os.replace(tmp, cache)
print('turnip: smoke-test jobs ' + (', '.join(added) if added else 'none (TURNIP_SMOKE unset)'))
PY
