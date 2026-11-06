#!/usr/bin/env python3
"""Read-only boot/payload validation; writes a new receipt, never uses a phone."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import tomllib


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unpack(path, directory):
    data = path.read_bytes()
    assert data[:8] == b"ANDROID!"
    kernel_size, _, ramdisk_size, _, second, _, _, page = struct.unpack_from("<8I", data, 8)
    assert page == 4096 and not second and struct.unpack_from("<I", data, 40)[0] == 0
    offset = page + ((kernel_size + page - 1) // page) * page
    assert offset + ramdisk_size <= len(data)
    sizes = struct.pack("<III", kernel_size, ramdisk_size, second)
    expected = hashlib.sha1(data[page:page + kernel_size] + data[offset:offset + ramdisk_size] + sizes).digest()[:8]
    assert data[576:584] == expected
    directory.mkdir()
    fat = directory / "fat.img"
    fat.write_bytes(data[offset:offset + ramdisk_size])
    subprocess.run(["mcopy", "-s", "-i", str(fat), "::/EFI", str(directory)], check=True)
    files = {str(p.relative_to(directory / "EFI")): p.read_bytes()
             for p in (directory / "EFI").rglob("*") if p.is_file()}
    return data, offset, ramdisk_size, files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--memory-test", type=int, choices=(1, 2),
                        help="explicit CPU-only memory diagnostic profile; default remains disabled")
    args = parser.parse_args()
    assert not args.receipt.exists(), "keep existing evidence"
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        image, offset, size, files = unpack(args.image, root / "new")
        reference, old_offset, old_size, originals = unpack(args.reference, root / "reference")
        assert (offset, size, len(image)) == (old_offset, old_size, len(reference))
        assert image[:576] == reference[:576]
        assert image[584:offset] == reference[584:offset]
        assert image[offset + size:] == reference[offset + size:]
        assert files.keys() == originals.keys()
        mutable = {"UniXNU/unixnu.toml", "UniXNU/Payloads/kernel.macho"}
        for name in files.keys() - mutable:
            assert files[name] == originals[name], name
        assert files["UniXNU/Payloads/kernel.macho"] == args.kernel.read_bytes()
        config = tomllib.loads(files["UniXNU/unixnu.toml"].decode())
        line = config["entry"][0]["args"]
        tokens = line.split()
        names = [t.split("=", 1)[0] for t in tokens]
        assert len(names) == len(set(names)), "duplicate boot arguments"
        assert len(line) <= 1000
        forbidden = {"cephmem", "cephboost", "cephgpu", "cephboot", "cephpower", "sweetfastboot", "sweetgpu"}
        if args.memory_test is not None:
            forbidden.remove("cephmem")
            assert f"cephmem={args.memory_test}" in tokens
        assert not forbidden.intersection(names)
        required = {"sweetusb=2", "sweettouch=1", "sweetkeys=1", "sweetkeyspononly=1",
                    "sweettouchgpio=1", "sweettouchpower=1", "sweettouchdelay=60",
                    "sweethidonce=1", "sweetfb_noconsole=1", "cephgx=1", "cephsoc=1"}
        assert required.issubset(tokens)
        receipt = {"image": str(args.image), "bytes": len(image), "sha256": digest(image),
                   "reference": str(args.reference), "reference_sha256": digest(reference),
                   "kernel_sha256": digest(args.kernel.read_bytes()),
                   "adt_sha256": digest(files["UniXNU/Payloads/kernel.macho.adt"]),
                   "args": line, "all_other_payload_files_unchanged": True,
                   "firmware_dtb_boot_header_unchanged": True, "boot_checksum_valid": True,
                   "hardware_tested": False,
                   "memory_test": args.memory_test,
                   "scope": "Packaging only. Explicit CPU-only memory selftest enabled." if args.memory_test else
                       "Packaging only. Memory selftest disabled.",
                   "cpu_boost_gpu_startup_disabled": True, "phone_operations": False}
    with args.receipt.open("x") as output:
        json.dump(receipt, output, indent=2)
        output.write("\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
