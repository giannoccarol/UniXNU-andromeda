#!/usr/bin/env python3
"""Build the Aloha NOSB firmware volume with the UniXNU/DarwinBoot drivers.

The device comes from board.toml ([board] name, [firmware.defines]
TARGET_DEVICE). Project Aloha ships first-class device directories for the
Xiaomi SM8150 phones (xiaomi-andromeda, xiaomi-cepheus, xiaomi-raphael, ...),
so nothing is cloned from another device.

The vendored aloha patch adds the UniXNU/DarwinBoot modules only to
Device/oneplus-guacamole/*. The device we build compiles its own
Device/<TARGET_DEVICE>/DXE.inc and DXE.dsc.inc, so the same four modules are
appended there (idempotently). Without them the firmware volume has no
DarwinBoot and can never start XNU.

The aloha patch creates Resources/Configs/guacamole.toml; it is mirrored to
<config>.toml because board.toml sets [firmware] config to the device name.
"""
import os
import shutil
import subprocess
import sys
import tomllib
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unixnu" / "scripts"))
sys.path.insert(0, str(ROOT / "scripts"))

from unixnu import firmware  # noqa: E402
from tree import assemble  # noqa: E402

board = tomllib.loads((ROOT / "board.toml").read_text())
NAME = board["board"]["name"]
TARGET_DEVICE = board["firmware"]["defines"]["TARGET_DEVICE"]
CONFIG = board["firmware"]["config"]

UEFI = ROOT / "uefi"
EDK2 = UEFI / "MU_BASECORE"
WORK = ROOT / "out" / NAME
TREE = assemble(WORK, ROOT / "unixnu", ROOT)
LOGS = WORK / "logs"
LOGS.mkdir(parents=True, exist_ok=True)

DEV = UEFI / "Platforms" / "SurfaceDuo1Pkg" / "Device" / TARGET_DEVICE
if not DEV.is_dir():
    raise SystemExit(f"firmware: {DEV} is not in this Aloha tree")
print(f"firmware: using native Aloha device dir {DEV.name}")

MARK = f"# UniXNU / DarwinBoot (appended for {TARGET_DEVICE})"
FDT_LIB = """ {
  <LibraryClasses>
    FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
}"""
# name -> INF path. UNIXNU_MODULES picks a subset (comma separated) so a hang
# can be bisected one module at a time; NO_DARWINBOOT=1 means none of them.
MODULES = {
    "sheng": "AndromedaPkg/Driver/ShengBootMemoryDxe/ShengBootMemoryDxe.inf",
    "unixnu": "UniXNUPkg/Application/UniXNU/UniXNU.inf",
    "unixnudxe": "UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf",
    "darwinboot": "DarwinBootPkg/Driver/DarwinBoot.inf",
}
if os.environ.get("NO_DARWINBOOT") == "1":
    wanted = []
else:
    wanted = [m.strip() for m in os.environ.get("UNIXNU_MODULES", ",".join(MODULES)).split(",") if m.strip()]
    unknown = [m for m in wanted if m not in MODULES]
    if unknown:
        raise SystemExit(f"firmware: unknown UNIXNU_MODULES {unknown}; known: {list(MODULES)}")

for name in ("DXE.dsc.inc", "DXE.inc"):
    path = DEV / name
    text = path.read_text(encoding="utf-8")
    # Always start from the device's own list, then append the chosen modules.
    text = text.split("\n" + MARK)[0].rstrip("\n") + "\n"
    if wanted:
        # Same order as the image tested on the phone: the FDF order sets the
        # firmware volume layout, so keep it for bit-identical rebuilds.
        if name == "DXE.dsc.inc":
            order = ("sheng", "unixnu", "unixnudxe", "darwinboot")
            body = "".join(f"{MODULES[m]}{FDT_LIB}\n" for m in order if m in wanted)
        else:
            order = ("unixnu", "unixnudxe", "darwinboot", "sheng")
            body = "".join(f"INF {MODULES[m]}\n" for m in order if m in wanted)
        text += f"\n{MARK}\n{body}"
    path.write_text(text, encoding="utf-8")
print(f"firmware: UniXNU modules in {DEV.name}: {', '.join(wanted) or '(none: plain Aloha UEFI)'}")

cfg_src = UEFI / "Resources" / "Configs" / "guacamole.toml"
cfg_dst = UEFI / "Resources" / "Configs" / f"{CONFIG}.toml"
if cfg_src.is_file() and not cfg_dst.is_file():
    shutil.copy(cfg_src, cfg_dst)
    print(f"firmware: created {cfg_dst.name}")

# The FDF's flash device must equal the "UEFI FD" entry of the memory map
# (0x9FC00000, 3 MiB on every Aloha SM8150 Xiaomi), and the FV only fits in
# 3 MiB when built RELEASE. The script only touches SurfaceDuo1.fdf.
subprocess.run([sys.executable, str(ROOT / "scripts" / "prepare-cepheus-fv.py"),
                str(UEFI)], check=True)

ctx = types.SimpleNamespace(
    board=board,
    tree=TREE,
    work=WORK,
    logs=LOGS,
    jobs=int(os.environ.get("JOBS", "8")),
    args=types.SimpleNamespace(skip_preflight=False),
    uefi=UEFI,
    device=ROOT,  # newer UniXNU preflight passes UNIXNU_DEVICE to its scripts
)
# RELEASE is required: DEBUG does not fit in the 3 MiB the memory map reserves.
TARGET = os.environ.get("FW_TARGET", "RELEASE")
firmware.build(ctx, None, UEFI, EDK2, TARGET)
