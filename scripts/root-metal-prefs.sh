#!/usr/bin/env bash
# Core Animation preferences for Metal on the Adreno (andrmtl=2), written into
# an iOS root image. With the deep framebuffer on, backboardd's framebuffer is
# 'b3a8' (BGRA10_XR): "Unexpected framebuffer format b3a8" and CA draws
# nothing, so the screen stays black (seen here 2026-11-07: swaps of 'b3a8'
# surfaces only with Metal). ios-flame sets these keys globally (HANDOFF.md,
# "prefs -g KEY bool NO"; Nick Wylynko, see CREDITS.md); SweetMetal2.bundle
# sets them only inside each process, too late for backboardd's framebuffer.
#
# Keys set to false: CAEnableDeepFramebuffer, CAEnableFramebufferCompression,
# CAEnableIOSurfaceCompression. Files: the any-user global domain, the global
# domain of mobile and of root, and com.apple.backboardd for both users.
#
# usage: scripts/root-metal-prefs.sh IMG
# Then run update-root.sh (fsck, dyld cache check, sparse image).
set -euo pipefail
IMG="${1:?usage: root-metal-prefs.sh IMG}"
[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }
IMG_DIR="$(cd "$(dirname "$IMG")" && pwd)"
docker run --rm --privileged -v /dev:/dev -v "$IMG_DIR:/out" ios-root-tools bash -c '
  set -euo pipefail
  mkdir -p /mnt/r
  mount -t hfsplus -o loop,force,rw "/out/$1" /mnt/r
  python3 -I - /mnt/r <<"PY"
import os, plistlib, sys

root = sys.argv[1]
keys = ("CAEnableDeepFramebuffer", "CAEnableFramebufferCompression",
        "CAEnableIOSurfaceCompression")
# (path, uid, gid, mode)
files = [
    ("Library/Preferences/.GlobalPreferences.plist", 0, 0, 0o644),
    ("private/var/mobile/Library/Preferences/.GlobalPreferences.plist", 501, 501, 0o600),
    ("private/var/root/Library/Preferences/.GlobalPreferences.plist", 0, 0, 0o600),
    ("private/var/mobile/Library/Preferences/com.apple.backboardd.plist", 501, 501, 0o600),
    ("private/var/root/Library/Preferences/com.apple.backboardd.plist", 0, 0, 0o600),
]
for rel, uid, gid, mode in files:
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = {}
    if os.path.exists(path):
        st = os.stat(path)
        uid, gid, mode = st.st_uid, st.st_gid, st.st_mode & 0o7777
        with open(path, "rb") as f:
            try:
                data = plistlib.load(f)
            except Exception:
                data = {}
    for k in keys:
        data[k] = False
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    os.chown(tmp, uid, gid)
    os.chmod(tmp, mode)
    os.replace(tmp, path)
    joined = ", ".join(keys)
    print(f"{rel}: {len(data)} keys, {joined} = NO")
PY
  umount /mnt/r
' prefs "$(basename "$IMG")"
