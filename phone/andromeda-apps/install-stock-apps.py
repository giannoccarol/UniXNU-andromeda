#!/usr/bin/env python3
"""Install iOS's own deletable apps into /Applications of a mounted iOS root.

  install-stock-apps.py ROOT

Run as root by phone/andromeda-apps/install-root.sh. The 32 apps of
/private/var/staged_system_apps (Calendar, Clock, Weather, Notes, Mail, Maps,
Music, Calculator, Contacts, Files, Reminders, ...) are copied into
/Applications/NAME.app and listed in /private/var/local/andromeda-apps/
apps.plist with "Stock": true, so the andromeda-apps boot helper registers
them with LaunchServices (andrapps=1).

Why: on an iPhone the data migrator SystemAppMigrator has installd install
them at the first boot after a restore. In this root
/private/var/mobile/Library/Preferences/com.apple.migration.plist already
holds LastSystemVersion 20H392, so no migration runs and the home screen shows
only the apps of /Applications (seen 2026-11-08). Clearing that key would run
every data migrator, some of which need the Secure Enclave. Copying the
bundles into /Applications as System apps is how ios-flame ran YouTube (see
install-apps.py). The staged originals stay where they are; nothing iOS owns
in /Applications is replaced; apps installed by an earlier run are kept as
they are. ANDROMEDA_STOCK_APPS=0 skips the step.
"""
import importlib.util
import os
import plistlib
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('install_apps', here / 'install-apps.py')
ia = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ia)

STAGED = 'private/var/staged_system_apps'


def human(n):
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if n < 1024 or unit == 'GiB':
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024
    return f'{n} B'


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: install-stock-apps.py ROOT')
    if os.environ.get('ANDROMEDA_STOCK_APPS', '1') == '0':
        ia.say('stock apps: skipped (ANDROMEDA_STOCK_APPS=0)')
        return
    root = Path(sys.argv[1])
    apps_dir, staged_dir = root / 'Applications', root / STAGED
    if not apps_dir.is_dir() or apps_dir.is_symlink():
        sys.exit(f'{ia.TAG}: {apps_dir} missing: not an iOS root')
    if not staged_dir.is_dir():
        ia.say(f'stock apps: no /{STAGED}; nothing to do')
        return
    old = ia.read_manifest(root)
    stock_old = {ia.ours_path(e): e for e in old if e.get('Stock') is True}
    taken = ia.system_bundle_ids(root, {ia.ours_path(e) for e in old})
    entries, plan = [], []
    for src in sorted(staged_dir.glob('*.app')):
        if not src.is_dir() or src.is_symlink() or src.name.startswith('.'):
            continue
        dest = f'/Applications/{src.name}'
        try:
            info = plistlib.loads((src / 'Info.plist').read_bytes())
        except (OSError, plistlib.InvalidFileException, ValueError) as e:
            ia.warn(f'stock {src.name}: no readable Info.plist ({e}); skipped')
            continue
        bid = info.get('CFBundleIdentifier')
        if not isinstance(bid, str):
            ia.warn(f'stock {src.name}: no bundle id; skipped')
            continue
        entry = dict(CFBundleIdentifier=bid, Path=dest,
                     Name=str(info.get('CFBundleDisplayName') or info.get('CFBundleName') or src.stem),
                     Version=str(info.get('CFBundleShortVersionString') or ''), Stock=True)
        if dest in stock_old and (root / dest[1:]).is_dir():
            entries.append(entry)
            continue
        if os.path.lexists(root / dest[1:]):
            ia.warn(f'stock {src.name}: {dest} already exists and is not ours; skipped')
            continue
        if bid in taken:
            ia.warn(f'stock {src.name}: {taken[bid]} already has the bundle id {bid}; skipped')
            continue
        plan.append((src, dest, entry))
    need = sum(ia.tree_size(src) for src, _, _ in plan)
    free = shutil.disk_usage(root).free
    if need + ia.MARGIN > free:
        sys.exit(f'{ia.TAG}: the stock apps need {human(need)}, the root has {human(free)} free')
    for src, dest, entry in plan:
        tmp = apps_dir / f'.{src.name}.andromeda-apps-tmp'
        if os.path.lexists(tmp):
            shutil.rmtree(tmp)
        ia.copy_tree(src, tmp)
        os.rename(tmp, root / dest[1:])
        entries.append(entry)
        ia.say(f'stock {dest}: {entry["CFBundleIdentifier"]} {entry["Version"]} ({human(ia.tree_size(root / dest[1:]))})')
    keep = [e for e in old if e.get('Stock') is not True]
    manifest = root / ia.MANIFEST
    ia.ensure_dir(manifest.parent)
    tmp = manifest.with_name(manifest.name + '.tmp')
    with open(tmp, 'wb') as f:
        plistlib.dump({'Version': 1, 'Apps': keep + entries}, f, fmt=plistlib.FMT_XML)
    os.chown(tmp, 0, 0)
    os.chmod(tmp, 0o644)
    os.replace(tmp, manifest)
    ia.say(f'stock apps: {len(plan)} copied, {len(entries)} listed ({human(need)} added)')


if __name__ == '__main__':
    main()
