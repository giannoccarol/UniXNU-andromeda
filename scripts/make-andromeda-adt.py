#!/usr/bin/env python3
"""UniXNU ADT step for andromeda (Xiaomi Mi Mix 3 5G, SM8150).

andromeda is the same SM8150 silicon as guacamole (OnePlus 7 Pro); only the
board compatible string and the panel resolution differ (1080x2340). The
UniXNU payload driver passes the generic ADT flags (chip-id, owned-memory-size,
root-match UUID, CPU registers, PSCI method, trust cache, ...). The andromeda
hardware profile in mkadt.py owns those values; accept the flags and generate
the D22AP/andromeda tree with the requested DRAM size.

GPU firmware: with --gpufw DIR (or the ANDROMEDA_GPUFW environment variable, or
the default inputs/andromeda/gpufw), every file in that directory is embedded
into /chosen as a property "gpufw,<filename>", the convention ios-flame uses
(Nick Wylynko / NWylynko, tools/mkpayload.py --gpufw) so the in-kernel Adreno
640 driver (bsd/dev/andromeda_gpu.c, boot-arg andrgpu) can read and SHA-256
verify them without a filesystem. Absent directory: no properties added, and
the step is otherwise unchanged. Firmware bytes never enter git; the directory
is git-ignored. See CREDITS.md.

Bluetooth firmware: --btfw DIR (or ANDROMEDA_BTFW, or the default
inputs/andromeda/btfw, filled by scripts/pull-bt-fw.sh) is embedded the same
way as /chosen "btfw,<filename>" for the WCN399x step B2 (bsd/dev/
andromeda_bt.c, andrbt=2: Qualcomm rampatch crbtfwXX.tlv and NVM crnvXX.bin).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
import adt  # noqa: E402  (scripts/adt.py: ADT parse/serialize)

# The in-kernel driver only reads these, but embedding the whole set is cheap
# and keeps the ADT a faithful copy of the vendor firmware directory.
_SKIP = {"SHA256SUMS", "SHA256SUMS.txt"}


def _fw_dir(explicit, env_name, default):
    if explicit:
        return Path(explicit)
    env = os.environ.get(env_name)
    if env:
        return Path(env)
    return SCRIPTS.parent / default


def embed_gpufw(output, gpufw_dir):
    """Add one /chosen "gpufw,<name>" property per firmware file. No-op (and no
    error) if the directory is absent, so a tree without local firmware still
    builds; the kernel then logs the missing images and the GPU stays off."""
    embed_fw(output, _fw_dir(gpufw_dir, "ANDROMEDA_GPUFW", "inputs/andromeda/gpufw"), "gpufw", "gpu")


def embed_btfw(output, btfw_dir):
    """Same for the Bluetooth firmware, as /chosen "btfw,<name>". Absent
    directory: nothing added; andrbt=2 then logs the missing file and powers
    the chip down again."""
    embed_fw(output, _fw_dir(btfw_dir, "ANDROMEDA_BTFW", "inputs/andromeda/btfw"), "btfw", "bluetooth")


def embed_fw(output, directory, prefix, what):
    if not directory.is_dir():
        print(f"make-andromeda-adt: no {what} firmware at {directory}; ADT left without {prefix} properties",
              file=sys.stderr)
        return
    files = sorted(p for p in directory.iterdir()
                   if p.is_file() and not p.name.startswith(".") and p.name not in _SKIP)
    if not files:
        print(f"make-andromeda-adt: {directory} has no firmware files to embed", file=sys.stderr)
        return
    blob = Path(output).read_bytes()
    root, off = adt.parse(blob)
    if off != len(blob):
        raise SystemExit("make-andromeda-adt: generated ADT did not parse cleanly")
    chosen = next((c for c in root.children if c.name == "chosen"), None)
    if chosen is None:
        raise SystemExit("make-andromeda-adt: no /chosen node in the generated ADT")
    existing = {name for name, _flags, _val in chosen.props}
    total = 0
    for path in files:
        prop = f"{prefix},{path.name}"
        if len(prop.encode()) > 32:
            raise SystemExit(f"make-andromeda-adt: property name too long for the ADT: {prop}")
        if prop in existing:
            continue
        data = path.read_bytes()
        chosen.props.append((prop, 0, data))
        total += len(data)
        print(f"make-andromeda-adt: embedded {prop} ({len(data)} bytes)", file=sys.stderr)
    Path(output).write_bytes(adt.serialize(root))
    print(f"make-andromeda-adt: {len(files)} {what} firmware files, {total} bytes added to /chosen",
          file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--output", required=True)
    parser.add_argument("--dram-size", default="0xc0000000")
    parser.add_argument("--owned-memory-size", type=lambda value: int(value, 0))
    parser.add_argument("--base-d22ap", default=None,
                        help="path to base-d22ap.adt (iPhone X artifact)")
    parser.add_argument("--gpufw", default=None,
                        help="directory of GPU firmware to embed as /chosen gpufw,<name> "
                             "(default: $ANDROMEDA_GPUFW or inputs/andromeda/gpufw)")
    parser.add_argument("--btfw", default=None,
                        help="directory of Bluetooth firmware to embed as /chosen btfw,<name> "
                             "(default: $ANDROMEDA_BTFW or inputs/andromeda/btfw)")
    known, _unknown = parser.parse_known_args()
    tool = SCRIPTS / "mkadt.py"
    command = [sys.executable, str(tool), known.output, "--device", "andromeda",
               "--dram-size", known.dram_size]
    if known.owned_memory_size is not None:
        command += ["--owned-memory-size", str(known.owned_memory_size)]
    if known.base_d22ap is not None:
        env = dict(os.environ)
        env["UNIXNU_D22AP_ADT"] = known.base_d22ap
        subprocess.run(command, check=True, env=env)
    else:
        subprocess.run(command, check=True)
    embed_gpufw(known.output, known.gpufw)
    embed_btfw(known.output, known.btfw)


if __name__ == "__main__":
    main()
