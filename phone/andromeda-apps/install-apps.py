#!/usr/bin/env python3
"""Copy the apps staged by scripts/add-ipa.py into /Applications of a mounted iOS root.

  install-apps.py ROOT STAGING ADD_IPA_PY

Run as root by phone/andromeda-apps/install-root.sh. ADD_IPA_PY is
scripts/add-ipa.py, whose checks are run again on every staged NAME.app
(--verify: no encrypted code, arm64 only, every page matching its signature,
an iPhone app for iOS <= 16.7.16) before it is copied.

- Each bundle goes to /Applications/NAME.app, root:wheel, directories 0755,
  files 0644 (0755 if executable), symlinks kept. Root-owned on purpose:
  ios-flame saw MobileInstallation, unable to validate an ad-hoc signature,
  try to chmod -x the executable of a sideloaded app it could write.
- Never replaces anything iOS owns: an app whose folder name or bundle id is
  already taken by a system app (in /Applications or
  /System/Library/CoreServices) is skipped with a warning.
- What we installed is listed in /private/var/local/andromeda-apps/apps.plist
  (read by the andromeda-apps boot helper). A rerun replaces our own copies,
  removes the ones no longer staged and touches nothing else; an app that is
  staged but now fails the checks keeps its previous copy.
- Refuses to fill the root: the apps must leave 256 MiB free.
"""
import importlib.util
import os
import plistlib
import shutil
import stat
import sys
from pathlib import Path

TAG = 'andromeda-apps'
MANIFEST = 'private/var/local/andromeda-apps/apps.plist'
MARGIN = 256 << 20


def say(msg):
    print(f'{TAG}: {msg}', flush=True)


def warn(msg):
    print(f'{TAG}: warning: {msg}', file=sys.stderr, flush=True)


def load_add_ipa(path):
    spec = importlib.util.spec_from_file_location('add_ipa', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ours_path(entry):
    """The manifest entry's /Applications/NAME.app path, or None if it is anything else."""
    p = entry.get('Path') if isinstance(entry, dict) else None
    if not isinstance(p, str) or not p.startswith('/Applications/') or not p.endswith('.app'):
        return None
    name = p[len('/Applications/'):]
    if '/' in name or name in ('.app', '..app') or name.startswith('.'):
        return None
    return p


def read_manifest(root):
    path = root / MANIFEST
    try:
        data = plistlib.loads(path.read_bytes())
    except FileNotFoundError:
        return []
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        warn(f'{path}: unreadable ({e}); treated as empty')
        return []
    apps = data.get('Apps') if isinstance(data, dict) else None
    return [e for e in apps or [] if ours_path(e)]


def system_bundle_ids(root, ours):
    """{bundle id: path} of the apps already in the root, except the ones we installed."""
    ids = {}
    for base in ('Applications', 'System/Library/CoreServices'):
        d = root / base
        if not d.is_dir():
            continue
        for app in sorted(d.glob('*.app')):
            rel = '/' + app.relative_to(root).as_posix()
            if rel in ours:
                continue
            try:
                bid = plistlib.loads((app / 'Info.plist').read_bytes()).get('CFBundleIdentifier')
            except (OSError, plistlib.InvalidFileException, ValueError, AttributeError):
                continue
            if isinstance(bid, str):
                ids.setdefault(bid, rel)
    return ids


def tree_size(path):
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            st = os.lstat(os.path.join(d, f))
            if stat.S_ISREG(st.st_mode):
                total += st.st_size
    return total


def copy_tree(src, dst):
    """SRC copied to DST as root:wheel: directories 0755, files 0644 or 0755, symlinks as symlinks."""
    os.mkdir(dst)
    os.chmod(dst, 0o755)
    os.lchown(dst, 0, 0)
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        out = dst if rel == '.' else os.path.join(dst, rel)
        for name in dirnames + filenames:
            s, t = os.path.join(dirpath, name), os.path.join(out, name)
            st = os.lstat(s)
            if stat.S_ISLNK(st.st_mode):
                os.symlink(os.readlink(s), t)
            elif stat.S_ISDIR(st.st_mode):
                os.mkdir(t)
                os.chmod(t, 0o755)
            elif stat.S_ISREG(st.st_mode):
                shutil.copyfile(s, t)
                os.chmod(t, 0o755 if st.st_mode & 0o111 else 0o644)
            else:
                raise OSError(f'{s}: not a file, directory or symlink')
            os.lchown(t, 0, 0)


def ensure_dir(path, mode=0o755):
    """Create PATH (root:wheel, MODE) and missing parents; existing ones keep owner and mode."""
    missing = []
    p = path
    while not p.exists():
        missing.append(p)
        p = p.parent
    for d in reversed(missing):
        os.mkdir(d)
        os.chmod(d, mode)
        os.chown(d, 0, 0)


def main():
    if len(sys.argv) != 4:
        sys.exit('usage: install-apps.py ROOT STAGING ADD_IPA_PY')
    root, staging = Path(sys.argv[1]), Path(sys.argv[2])
    a = load_add_ipa(sys.argv[3])
    apps_dir = root / 'Applications'
    if not apps_dir.is_dir() or apps_dir.is_symlink():
        sys.exit(f'{TAG}: {apps_dir} missing: not an iOS root')

    old = read_manifest(root)
    ours = {ours_path(e) for e in old}
    taken = system_bundle_ids(root, ours)
    staged = [p for p in sorted(staging.glob('*.app'))
              if p.is_dir() and not p.is_symlink() and not p.name.startswith('.')]
    say(f'{len(staged)} app(s) staged in {staging}')

    plan, refused = [], set()
    for src in staged:
        dest = f'/Applications/{src.name}'
        try:
            rep = a.process_app(src, 'never', fix=False)
        except a.Refused as e:
            warn(f'{src.name}: not installed: {e}')
            refused.add(dest)
            continue
        bid = rep['bundle_id']
        if bid in taken:
            warn(f'{src.name}: not installed: {taken[bid]} in the root has the same bundle id {bid}')
            continue
        if dest not in ours and os.path.lexists(root / dest[1:]):
            warn(f'{src.name}: not installed: {dest} belongs to iOS')
            continue
        if any(r['bundle_id'] == bid for _, r in plan):
            warn(f'{src.name}: not installed: another staged app has the bundle id {bid}')
            continue
        for w in rep['warnings']:
            warn(f'{src.name}: {w}')
        plan.append((src, rep))

    keep = [e for e in old if ours_path(e) in refused and (root / ours_path(e)[1:]).is_dir()]
    for e in keep:
        warn(f'{ours_path(e)}: the staged copy failed the checks; the installed one is kept')
    need = sum(tree_size(src) for src, _ in plan)
    freed = sum(tree_size(root / ours_path(e)[1:]) for e in old
                if e not in keep and (root / ours_path(e)[1:]).is_dir())
    free = shutil.disk_usage(root).free
    if need - freed + MARGIN > free:
        sys.exit(f'{TAG}: the apps need {a.human(need)} and the root has {a.human(free + freed)} '
                 f'for them (256 MiB must stay free): remove some with scripts/add-ipa.py --remove, '
                 'or build a bigger root (build-root.sh IPSW SIZE)')

    installed = []
    for src, rep in plan:
        name = src.name
        dest, tmp = apps_dir / name, apps_dir / f'.{name}.andromeda-apps-tmp'
        if os.path.lexists(tmp):
            shutil.rmtree(tmp)
        copy_tree(src, tmp)
        if os.path.lexists(dest):          # ours: checked above
            shutil.rmtree(dest)
        os.rename(tmp, dest)
        installed.append(dict(CFBundleIdentifier=rep['bundle_id'], Path=f'/Applications/{name}',
                              Name=rep['name'], Version=rep['version']))
        say(f'installed /Applications/{name}: {rep["bundle_id"]} {rep["version"]} '
            f'({a.human(tree_size(dest))}, {rep["machos"]} Mach-O file(s))')

    current = {e['Path'] for e in installed} | {ours_path(e) for e in keep}
    for e in old:
        p = ours_path(e)
        if p not in current and os.path.lexists(root / p[1:]):
            shutil.rmtree(root / p[1:])
            say(f'removed {p} (no longer staged)')

    manifest = root / MANIFEST
    if not installed and not keep and not os.path.lexists(manifest):
        say('nothing to install')
        return
    ensure_dir(manifest.parent)
    tmp = manifest.with_name(manifest.name + '.tmp')
    with open(tmp, 'wb') as f:
        plistlib.dump({'Version': 1, 'Apps': installed + keep}, f, fmt=plistlib.FMT_XML)
    os.chown(tmp, 0, 0)
    os.chmod(tmp, 0o644)
    os.replace(tmp, manifest)
    say(f'{len(installed) + len(keep)} app(s) listed in /{MANIFEST}')
    if installed:
        say('a new root registers them when LaunchServices builds its first database; '
            'boot with andrapps=1 to check, andrapps=2 to rebuild it on a root that has already booted')


if __name__ == '__main__':
    main()
