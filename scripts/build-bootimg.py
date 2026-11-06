#!/usr/bin/env python3
"""Pack the Aloha boot image with the device's DTB table.

unixnu's bootimg insists on a single FDT; the Aloha DTBs are tables. For the Mi 9
the table holds one FDT (the phone's own); for the Mi Mix 3 5G it holds eleven
generic base trees (SM8150 v1/v2, SM8150P v1/v2 and unrelated Qualcomm boards),
and the stock ABL picks the one matching the SoC and applies the dtbo overlay.
So the whole table is appended, and /chosen/linux,initrd-start/-end is patched
in EVERY FDT that has a /chosen node, not only the first one.

The device name comes from board.toml ([board] name). --no-payload packs an
empty ramdisk, for a firmware-only test (the UniXNU menu starts, finds no
kernel payload).
"""
import argparse
import gzip
import json
import os
import struct
import sys
import tempfile
import tomllib
import types
from pathlib import Path

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def _patch_one_fdt_initrd(blob, start, end):
    """Set /chosen/linux,initrd-start/end in the first FDT of a DTB table.

    The Aloha DTB and this phone's own live FDT both ALREADY carry
    /chosen/linux,initrd-start/-end (they point at the Android ramdisk the
    stock bootloader hands over). Appending two more properties of the same
    name produces a tree with duplicate property names, which is invalid:

        dtc: ERROR (duplicate_property_names): /chosen:linux,initrd-start

    So this replaces the existing values in place when the property is already
    there, and only appends when it is missing.
    """
    if blob[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit("device tree: bad magic")
    (magic, total, off_struct, off_strings, off_rsv, version, last,
     cpu, size_strings, size_struct) = struct.unpack_from(">10I", blob, 0)
    if total > len(blob) or off_struct + size_struct > total or off_strings + size_strings > total:
        raise SystemExit("device tree: invalid first-blob extents")
    first, rest = blob[:total], b""
    body = bytearray(first[off_struct:off_struct + size_struct])
    strings = bytearray(first[off_strings:off_strings + size_strings])

    def be32(at):
        return struct.unpack_from(">I", body, at)[0]

    def name_at(offset):
        return bytes(strings[offset:strings.index(0, offset)]).decode()

    wanted = {"linux,initrd-start": start, "linux,initrd-end": end}
    replaced = {}

    depth = -1
    in_chosen = False
    insert_at = None
    i = 0
    while i < len(body):
        token = be32(i)
        i += 4
        if token == FDT_BEGIN_NODE:
            nul = body.index(0, i)
            name = bytes(body[i:nul]).decode()
            i = (nul + 4) & ~3
            depth += 1
            if depth == 1 and name == "chosen":
                in_chosen = True
        elif token == FDT_END_NODE:
            if in_chosen and depth == 1:
                insert_at = i - 4
                break
            depth -= 1
        elif token == FDT_PROP:
            length, nameoff = struct.unpack_from(">II", body, i)
            value_at = i + 8
            i = value_at + ((length + 3) & ~3)
            if not in_chosen:
                continue
            pname = name_at(nameoff)
            if pname in wanted:
                new = struct.pack(">I", wanted[pname])
                if length == 4:
                    body[value_at:value_at + 4] = new
                elif length == 8:
                    # a 64-bit cell pair (the live FDT uses <0x0 0x...>)
                    body[value_at:value_at + 8] = struct.pack(">II", 0, wanted[pname])
                else:
                    raise SystemExit(
                        f"device tree: {pname} is {length} bytes, expected 4 or 8")
                replaced[pname] = length
        elif token == FDT_NOP:
            pass
        elif token == FDT_END:
            break
        else:
            raise SystemExit(f"device tree: unknown token {token:#x}")
    if insert_at is None:
        return None  # this FDT has no /chosen node

    def name_offset(name):
        wanted_name = name.encode() + b"\0"
        if wanted_name not in strings:
            offset = len(strings)
            strings.extend(wanted_name)
            return offset
        offset = strings.index(wanted_name)
        if offset and strings[offset - 1] != 0:
            raise SystemExit(f"device tree: substring match for {name}")
        return offset

    def prop(name, value):
        offset = name_offset(name)
        record = struct.pack(">III", FDT_PROP, len(value), offset) + value
        return record + b"\0" * ((-len(value)) & 3)

    missing = [name for name in wanted if name not in replaced]
    if missing:
        props = b""
        for name in missing:
            props += prop(name, struct.pack(">I", wanted[name]))
        body = body[:insert_at] + props + body[insert_at:]
    patched = bytearray(first[:off_struct]) + bytes(body) + bytes(strings)
    struct.pack_into(">I", patched, 4, len(patched))
    struct.pack_into(">I", patched, 12, off_struct + len(body))
    struct.pack_into(">I", patched, 32, len(strings))
    struct.pack_into(">I", patched, 36, len(body))
    return bytes(patched) + rest, sorted(replaced), sorted(missing)


def patch_table_initrd(blob, start, end):
    """Patch /chosen initrd in every FDT of a DTB table; FDTs without /chosen stay as they are."""
    out, offset, index, patched_count = b"", 0, 0, 0
    while offset < len(blob):
        if blob[offset:offset + 4] != b"\xd0\x0d\xfe\xed":
            raise SystemExit(f"device tree: bad magic at {offset:#x}")
        total = struct.unpack_from(">I", blob, offset + 4)[0]
        one = blob[offset:offset + total]
        result = _patch_one_fdt_initrd(one, start, end)
        if result is None:
            out += one
        else:
            new, replaced, appended = result
            out += new
            patched_count += 1
            print(f"device tree #{index}: initrd start={start:#x} end={end:#x} "
                  f"(replaced {replaced}, appended {appended})")
        offset += total
        index += 1
    if not patched_count:
        print("device tree: no FDT has a /chosen node; the ABL sets initrd itself")
    return out


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unixnu" / "scripts"))
sys.path.insert(0, str(ROOT / "scripts"))

from unixnu import bootimg  # noqa: E402
from unixnu.util import run, sha256_file  # noqa: E402
from tree import assemble  # noqa: E402

FW_TARGET = os.environ.get("FW_TARGET", "RELEASE")
NAME = tomllib.loads((ROOT / "board.toml").read_text())["board"]["name"]

parser = argparse.ArgumentParser()
parser.add_argument("--fd", default=str(ROOT / "uefi" / "Build" / "SurfaceDuo1Pkg" / f"{FW_TARGET}_CLANGPDB" / "FV" / "SM8150_EFI.fd"))
parser.add_argument("--dtb", default=str(ROOT / "inputs" / "uefi-prebuilt" / f"{NAME}.dtb"))
parser.add_argument("--ramdisk", default=str(ROOT / "out" / NAME / "payload" / "payload.img"))
parser.add_argument("-o", "--output", default=str(ROOT / "out" / NAME / "boot" / f"boot-{NAME}.img"))
parser.add_argument("--no-payload", action="store_true",
                    help="pack an empty ramdisk (firmware-only test, no kernel)")
args = parser.parse_args()

WORK = ROOT / "out" / NAME
TREE = assemble(WORK, ROOT / "unixnu", ROOT)
UEFI = ROOT / "uefi"
board = tomllib.loads((ROOT / "board.toml").read_text())
ctx = types.SimpleNamespace(board=board, work=WORK, tree=TREE)
image, base, size, requires_header, config = bootimg.settings(ctx, UEFI)
fv = Path(args.fd).read_bytes()
if len(fv) != size:
    raise SystemExit(f"firmware volume is {len(fv):#x} bytes, expected {size:#x}")
dtb = Path(args.dtb).read_bytes()
ramdisk = Path(args.ramdisk)
if args.no_payload:
    ramdisk = WORK / "boot" / "empty-ramdisk.img"
    ramdisk.parent.mkdir(parents=True, exist_ok=True)
    ramdisk.write_bytes(b"\0" * 4096)
    print("bootimg: --no-payload, packing an empty 4 KiB ramdisk")
if ramdisk.is_file() and image.get("ramdisk_offset") is not None:
    ramdisk_start = (
        image.get("abl_base", 0) + image.get("base", 0) + image["ramdisk_offset"]
    )
    dtb = patch_table_initrd(
        dtb,
        ramdisk_start,
        ramdisk_start + ramdisk.stat().st_size,
    )
out = Path(args.output)
out.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="unixnu-bootimg-") as scratch_name:
    scratch = Path(scratch_name)
    shim = bootimg.compile_shim(UEFI, base, size, requires_header, scratch)
    combined = shim + fv
    kernel = gzip.compress(combined, mtime=0) + dtb
    kernel_path = scratch / "Image.gz-dtb"
    kernel_path.write_bytes(kernel)
    command = ["python3", str(UEFI / "Resources" / "Scripts" / "mkbootimg.py"),
               "--kernel", str(kernel_path), "--ramdisk", str(ramdisk), "-o", str(out)]
    for key in bootimg.MKBOOTIMG_FLAGS:
        if key in image:
            command += [f"--{key}", str(image[key])]
    run(command)

report = {
    "kernel_sha256": sha256_file(kernel_path) if kernel_path.exists() else None,
    "image_sha256": sha256_file(out),
    "image_bytes": out.stat().st_size,
    "fd_sha256": sha256_file(args.fd),
    "dtb_sha256": sha256_file(args.dtb),
    "dtb_bytes": len(dtb),
    "ramdisk_bytes": ramdisk.stat().st_size,
    "shim_bytes": len(shim),
    "hardware_tested": False,
}
(out.parent / "boot.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"bootimg: {out} ({out.stat().st_size} bytes)")
