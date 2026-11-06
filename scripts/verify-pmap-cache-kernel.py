#!/usr/bin/env python3
"""Verify the linked cache callback uses 64-bit PA formation, not low32 shift."""
from pathlib import Path
import argparse
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    args = parser.parse_args()
    data = args.kernel.read_bytes()
    assert data[:8] == bytes.fromhex('cffaedfe0c000001'), 'arm64 Mach-O required'
    commands = struct.unpack_from('<I', data, 16)[0]
    offset = 32
    segments, symtab = [], None
    for _ in range(commands):
        cmd, size = struct.unpack_from('<II', data, offset)
        assert size >= 8 and offset + size <= len(data)
        if cmd == 0x19:
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, offset + 24)
            segments.append((vmaddr, fileoff, filesize))
        if cmd == 2:
            symtab = struct.unpack_from('<IIII', data, offset + 8)
        offset += size
    assert symtab is not None
    symoff, count, stroff, strsize = symtab
    wanted = b'_pmap_sync_page_attributes_phys'
    address = None
    for n in range(count):
        name, kind, section, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * n)
        if name >= strsize:
            continue
        end = data.find(b'\0', stroff + name, stroff + strsize)
        if data[stroff + name:end] == wanted and kind & 0xe == 0xe:
            address = value
            break
    assert address is not None, 'cache callback symbol absent'
    locations = [fileoff + address - vmaddr for vmaddr, fileoff, filesize in segments
                 if vmaddr <= address < vmaddr + filesize]
    assert len(locations) == 1
    # XNU's tail-call callback forms a PA from uint32_t pn into x0. 4KiB:
    # ubfiz x0,x0,#12,#32; 16KiB: ubfiz x0,x0,#14,#32. Fail closed if codegen
    # changes; do not accept mere absence of the known broken instruction.
    word = struct.unpack_from('<I', data, locations[0])[0]
    expected = {0xd3747c00: 4096, 0xd3727c00: 16384}
    assert word in expected, f'cache PA widening absent/unknown opcode {word:#x}; expected 64-bit UBFIZ'
    print(f'Linked pmap cache callback: 64-bit PA formation ({expected[word]}-byte host pages) PASS; not hardware proof')


if __name__ == '__main__':
    main()
