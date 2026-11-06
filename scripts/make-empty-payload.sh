#!/usr/bin/env bash
# Make a GUACESP FAT32 payload with no kernel, for firmware tests.
#
# Why not an empty ramdisk: when the ramdisk is not a GUACESP FAT,
# ShengBootMemoryDxe falls back to scanning every memory descriptor, Reserved
# ones included. On andromeda that reads the Hypervisor region (0x85700000) and
# the firmware hangs before drawing anything.
#
# Why 34 MiB: the ABL loads the ramdisk at 0x80800000, and the andromeda memory
# map ends the "Kernel" region at 0x82A00000 ("HLOS 1", firmware boot-services
# data, starts at 0x83722000). 0x80800000 + 34 MiB = 0x82A00000 exactly. FAT32
# needs at least 65525 clusters, so clusters are one 512-byte sector.
#
# usage: scripts/make-empty-payload.sh [output] [size MiB]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/out/andromeda/payload/empty-payload.img}"
MIB="${2:-34}"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
mkfs.fat -F 32 -S 512 -s 1 -n GUACESP -C "$OUT" $((MIB * 1024)) >/dev/null
mmd -i "$OUT" ::/EFI ::/EFI/UniXNU
printf 'Firmware test payload for andromeda: no kernel, no config.\n' > /tmp/unixnu-payload-readme.txt
mcopy -i "$OUT" /tmp/unixnu-payload-readme.txt ::/EFI/UniXNU/README.txt
rm -f /tmp/unixnu-payload-readme.txt
python3 - "$OUT" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read(512)
bps, spc = struct.unpack_from('<HB', b, 11)
total = struct.unpack_from('<I', b, 32)[0]
fatsz = struct.unpack_from('<I', b, 36)[0]
rsvd = struct.unpack_from('<H', b, 14)[0]
clusters = (total - rsvd - 2 * fatsz) // spc
ok = b[510:512] == b'\x55\xaa' and b[82:90] == b'FAT32   ' and clusters >= 65525
print(f"payload: {sys.argv[1]} {total * bps // 1048576} MiB, label {b[71:82].decode().strip()}, "
      f"{clusters} clusters, FAT32 check {'ok' if ok else 'FAILED'}")
sys.exit(0 if ok else 1)
EOF
