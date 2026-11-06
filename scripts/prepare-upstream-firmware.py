#!/usr/bin/env python3
"""Adapt the reviewed November upstream firmware pins in CLEAN, isolated trees.

Does not build/pack/boot an image, modify the kernel or touch a phone. Refuses
unknown heads and local edits rather than using --reject or discarding changes.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args, **kwargs):
    return subprocess.run(["git", "-C", str(path), *args], check=True,
                          text=True, capture_output=True, **kwargs).stdout.strip()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"upstream context drift: {old[:100]}")
    return text.replace(old, new)


def apply(path, patch, check=False):
    git(path, "apply", *(["--check"] if check else []), "--whitespace=nowarn", "-", input=patch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", required=True, type=Path)
    parser.add_argument("--uefi", required=True, type=Path)
    args = parser.parse_args()
    unixnu, uefi = args.unixnu.resolve(), args.uefi.resolve()
    lock = json.loads((ROOT / "upstream-lock.json").read_text())["firmware_candidate"]
    darwin = unixnu / "drivers/DarwinBoot"
    for path, key in ((unixnu, "unixnu"), (uefi, "aloha"), (darwin, "darwinboot")):
        if git(path, "rev-parse", "HEAD") != lock[key]:
            raise ValueError(f"{key}: not the reviewed upstream pin")
        if git(path, "status", "--porcelain"):
            raise ValueError(f"{key}: local edits present; use a new isolated checkout")

    # Upstream 1833131 includes our connect-all semantic fix. Preserve its
    # current menu implementation rather than replaying stale Main.c context.
    main_source = (unixnu / "package/UniXNUPkg/Library/UniXNULib/Main.c").read_text()
    if main_source.count("UniXNUConnectAll ();") != 1 or "if (Host == UniXNUHostFirmware)" in main_source:
        raise ValueError("connect-all prerequisite drift")
    patch = (ROOT / "patches/unixnu-cepheus.patch").read_text()
    excluded = {"package/UniXNUPkg/Library/UniXNULib/Main.c", "scripts/unixnu/firmware.py"}
    parts = patch.split("diff --git ")[1:]
    patch = "".join("diff --git " + part for part in parts if part.splitlines()[0].split()[0][2:] not in excluded)

    # Retain upstream preflight argument/environment support, add only the
    # two Cepheus transport/define substitutions. Do not downgrade firmware.py.
    firmware = unixnu / "scripts/unixnu/firmware.py"
    text = firmware.read_text()
    text = replace_once(text, "'update', '--init', '--depth', '1', '--'", "'update', '--init', '--'")
    old = "    defines.update({key: str(value) for key, value in fw.get('defines', {}).items()})\n    fields = {'uefi': str(uefi), 'edk2': str(edk2), 'tree': str(ctx.tree)}"
    new = "    fields = {'uefi': str(uefi), 'edk2': str(edk2), 'tree': str(ctx.tree)}\n    defines.update({key: str(value).format(**fields) for key, value in fw.get('defines', {}).items()})"
    text = replace_once(text, old, new)
    aloha_patch = (ROOT / "patches/aloha-cepheus.patch").read_text()
    darwin_patch = (ROOT / "patches/darwinboot-cepheus.patch").read_text()
    # Precheck all three before the first edit. No silent reject files.
    for path, content in ((unixnu, patch), (uefi, aloha_patch), (darwin, darwin_patch)):
        apply(path, content, check=True)
    for path, content in ((unixnu, patch), (uefi, aloha_patch), (darwin, darwin_patch)):
        apply(path, content)
    firmware.write_text(text)

    # The historical patch targets guacamole. Include the same modules in the
    # native Cepheus device, without enabling new GPU/security/runtime paths.
    device = uefi / "Platforms/SurfaceDuo1Pkg/Device/xiaomi-cepheus"
    modules = (
        "AndromedaPkg/Driver/ShengBootMemoryDxe/ShengBootMemoryDxe.inf",
        "UniXNUPkg/Application/UniXNU/UniXNU.inf",
        "UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf",
        "DarwinBootPkg/Driver/DarwinBoot.inf",
    )
    dsc, fdf = device / "DXE.dsc.inc", device / "DXE.inc"
    dsc.write_text(dsc.read_text() + "\n# Cepheus upstream candidate (not hardware qualified)\n" +
                   "".join(f"{module} {{\n  <LibraryClasses>\n    FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf\n}}\n" for module in modules))
    fdf.write_text(fdf.read_text() + "\n# Cepheus upstream candidate\n" + "".join(f"INF {module}\n" for module in modules))
    print(json.dumps({"pins": lock, "status": "patches applied; not compiled or hardware-tested"}, indent=2))


if __name__ == "__main__":
    main()
