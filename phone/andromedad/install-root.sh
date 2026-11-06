#!/usr/bin/env bash
# Install andromedad into a mounted iOS root (hook of scripts/rebuild-root.sh).
#
#   phone/andromedad/install-root.sh ROOT OUTDIR
#
# ROOT    the HFS+ iOS root, mounted read-write; run as root (files are
#         root:wheel).
# OUTDIR  what build.sh got as OUT: the binary is OUTDIR/andromedad/andromedad
#         (OUTDIR may also be that andromedad directory itself).
#
# Installs /private/var/local/bin/andromedad (0755) and the launchd job
# org.andromeda.andromedad (org.andromeda.andromedad.plist next to this
# script) into the job cache /System/Library/xpc/launchd.plist, keeping its
# owner and mode. Idempotent. Without a built binary it only warns and
# leaves the image unchanged. The daemon itself stays idle unless the kernel
# is booted with andrslider=1 andrslidercam=1|2 (patch 0097).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
usage='usage: install-root.sh ROOT OUTDIR'
ROOT=${1:?$usage}
OUTDIR=${2:?$usage}
ROOT=$(cd "$ROOT" && pwd -P)
[[ $ROOT != / ]] || { echo 'andromedad: refusing to install into /' >&2; exit 1; }
cache=$ROOT/System/Library/xpc/launchd.plist
[[ -f $cache ]] || { echo "andromedad: $cache missing: not an iOS root" >&2; exit 1; }
job=$here/org.andromeda.andromedad.plist
[[ -f $job ]] || { echo "andromedad: $job missing" >&2; exit 1; }

bin=
for candidate in "$OUTDIR/andromedad/andromedad" "$OUTDIR/andromedad"; do
	if [[ -f $candidate ]]; then
		bin=$candidate
		break
	fi
done
if [[ -z $bin ]]; then
	echo "andromedad: no binary under $OUTDIR (build it with phone/andromedad/build.sh on macOS); not installed" >&2
	exit 0
fi
# 64-bit Mach-O (MH_MAGIC_64, little endian), arm64 (CPU_TYPE_ARM64 0x0100000c).
header=$(od -An -tx1 -N8 "$bin" | tr -d ' \n')
[[ $header == cffaedfe0c000001 ]] || { echo "andromedad: $bin is not an arm64 Mach-O" >&2; exit 1; }

# Directories as planned for wland (docs/research/wifi-flame.md 5.4); an
# existing directory keeps its owner and mode.
for d in private/var/local:0755 private/var/local/bin:0700; do
	if [[ ! -d $ROOT/${d%:*} ]]; then
		install -d -o 0 -g 0 -m "${d#*:}" "$ROOT/${d%:*}"
	fi
done
install -o 0 -g 0 -m 0755 "$bin" "$ROOT/private/var/local/bin/andromedad"

python3 -I - "$cache" "$job" <<'PY'
import os, plistlib, sys

cache, job_path = sys.argv[1], sys.argv[2]
st = os.stat(cache)
with open(cache, 'rb') as f:
    plist = plistlib.load(f)
with open(job_path, 'rb') as f:
    job = plistlib.load(f)
jobs = plist['LaunchDaemons']
label = job['Label']
key = f'/System/Library/LaunchDaemons/{label}.plist'
for k in [k for k, j in jobs.items() if j.get('Label') == label and k != key]:
    del jobs[k]
jobs[key] = job
tmp = cache + '.andromedad.tmp'
with open(tmp, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
os.chown(tmp, st.st_uid, st.st_gid)
os.chmod(tmp, st.st_mode & 0o7777)
os.replace(tmp, cache)
print(f'andromedad: launchd job {label} installed ({len(jobs)} jobs in the cache)')
PY
echo "andromedad: installed /private/var/local/bin/andromedad from $bin"
