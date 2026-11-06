#!/usr/bin/env python3
"""Synthetic Mach-O parser/gate regressions, not a linked kernel build claim."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    # Small bounded arm64 Mach-O carrier with one callback symbol and code.
    data = bytearray(512)
    struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 96, 0, 0)
    struct.pack_into('<II16sQQQQIIII', data, 32, 0x19, 72, b'__TEXT_EXEC',
                     0xfffffe0007000000, 32, 160, 32, 5, 5, 0, 0)
    struct.pack_into('<6I', data, 104, 2, 24, 256, 1, 272, 64)
    struct.pack_into('<IBBHQ', data, 256, 1, 0xe, 1, 0, 0xfffffe0007000000)
    name = b'\0_pmap_sync_page_attributes_phys\0'
    data[272:272 + len(name)] = name
    file = work / 'fixture.macho'
    for opcode, success in ((0xd3747c00, True), (0xd3727c00, True),
                            (0x53124400, False), (0xd503201f, False)):
        struct.pack_into('<I', data, 160, opcode)
        file.write_bytes(data)
        result = subprocess.run([sys.executable, str(root / 'scripts/verify-pmap-cache-kernel.py'), str(file)],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
print('Pmap cache linked gate: 4/16KiB wide opcodes accepted, truncating/unknown code rejected PASS (synthetic carrier)')
