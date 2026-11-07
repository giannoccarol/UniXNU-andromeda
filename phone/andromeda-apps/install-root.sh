#!/usr/bin/env bash
# Install the owner's own apps (staged on this PC by scripts/add-ipa.py) and
# the andromeda-apps helper into a mounted iOS root (hook of
# scripts/rebuild-root.sh).
#
#   phone/andromeda-apps/install-root.sh ROOT OUTDIR
#
# ROOT    the HFS+ iOS root, mounted read-write; run as root (files are
#         root:wheel).
# OUTDIR  what build.sh got as OUT: the helper is
#         OUTDIR/andromeda-apps/andromeda-apps (OUTDIR may also be that
#         andromeda-apps directory itself).
# The staged apps are read from $ANDROMEDA_APPS if set, else /apps (where
# build-root.sh's container mounts inputs/<board>/apps), else
# <repo>/inputs/andromeda/apps.
#
# 1. Apps -> /Applications/NAME.app (install-apps.py next to this script):
#    every staged bundle is checked again with scripts/add-ipa.py, copied
#    root:wheel, and listed in /private/var/local/andromeda-apps/apps.plist.
#    /Applications, not /private/var/containers/Bundle/Application: apps
#    there are System apps that LaunchServices finds by itself (at its first
#    database build, or a rebuild), which is how ios-flame ran YouTube and
#    how ios-apollo installs apps on the same iOS build. User apps need
#    installd and containermanagerd bookkeeping, and installd fails on this
#    port (no clonefile on HFS+, no signature validation: ios-flame).
#    Nothing iOS owns is replaced. Without a staging directory /Applications
#    is left as it is.
# 2. Helper -> /private/var/local/bin/andromeda-apps (0755) and the launchd
#    job org.andromeda.andromeda-apps in /System/Library/xpc/launchd.plist
#    (owner and mode kept). It runs once per boot and does nothing unless the
#    kernel is booted with andrapps=1|2 (see andromeda-apps.m). Without a
#    built helper only the apps are installed: a new root registers them by
#    itself, there is just no log line saying so.
# Idempotent.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
usage='usage: install-root.sh ROOT OUTDIR'
ROOT=${1:?$usage}
OUTDIR=${2:?$usage}
ROOT=$(cd "$ROOT" && pwd -P)
[[ $ROOT != / ]] || { echo 'andromeda-apps: refusing to install into /' >&2; exit 1; }
cache=$ROOT/System/Library/xpc/launchd.plist
[[ -f $cache ]] || { echo "andromeda-apps: $cache missing: not an iOS root" >&2; exit 1; }
[[ -d $ROOT/Applications ]] || { echo "andromeda-apps: $ROOT/Applications missing: not an iOS root" >&2; exit 1; }
job=$here/org.andromeda.andromeda-apps.plist
[[ -f $job ]] || { echo "andromeda-apps: $job missing" >&2; exit 1; }

# 1. The apps.
apps=${ANDROMEDA_APPS:-}
if [[ -z $apps ]]; then
	for candidate in /apps "$repo/inputs/andromeda/apps"; do
		if [[ -d $candidate ]]; then
			apps=$candidate
			break
		fi
	done
fi
add_ipa=$repo/scripts/add-ipa.py      # /scripts/add-ipa.py in build-root.sh's container
if [[ -z $apps || ! -d $apps ]]; then
	echo "andromeda-apps: no staged apps (${apps:-/apps or $repo/inputs/andromeda/apps}; stage them with scripts/add-ipa.py); /Applications unchanged" >&2
elif [[ ! -f $add_ipa ]]; then
	echo "andromeda-apps: $add_ipa missing: cannot check the staged apps; /Applications unchanged" >&2
else
	python3 -I "$here/install-apps.py" "$ROOT" "$apps" "$add_ipa"
fi
# 1b. iOS's own deletable apps (/private/var/staged_system_apps), whatever is
# staged: see install-stock-apps.py. ANDROMEDA_STOCK_APPS=0 skips them.
python3 -I "$here/install-stock-apps.py" "$ROOT"

# 2. The helper.
bin=
for candidate in "$OUTDIR/andromeda-apps/andromeda-apps" "$OUTDIR/andromeda-apps"; do
	if [[ -f $candidate ]]; then
		bin=$candidate
		break
	fi
done
if [[ -z $bin ]]; then
	echo "andromeda-apps: no helper under $OUTDIR (build it with phone/andromeda-apps/build.sh on macOS); helper not installed" >&2
	exit 0
fi
# 64-bit Mach-O (MH_MAGIC_64, little endian), arm64 (CPU_TYPE_ARM64 0x0100000c).
header=$(od -An -tx1 -N8 "$bin" | tr -d ' \n')
[[ $header == cffaedfe0c000001 ]] || { echo "andromeda-apps: $bin is not an arm64 Mach-O" >&2; exit 1; }

# Directories as for andromedad; an existing directory keeps its owner and mode.
for d in private/var/local:0755 private/var/local/bin:0700; do
	if [[ ! -d $ROOT/${d%:*} ]]; then
		install -d -o 0 -g 0 -m "${d#*:}" "$ROOT/${d%:*}"
	fi
done
install -o 0 -g 0 -m 0755 "$bin" "$ROOT/private/var/local/bin/andromeda-apps"

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
tmp = cache + '.andromeda-apps.tmp'
with open(tmp, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
os.chown(tmp, st.st_uid, st.st_gid)
os.chmod(tmp, st.st_mode & 0o7777)
os.replace(tmp, cache)
print(f'andromeda-apps: launchd job {label} installed ({len(jobs)} jobs in the cache)')
PY
echo "andromeda-apps: installed /private/var/local/bin/andromeda-apps from $bin"
