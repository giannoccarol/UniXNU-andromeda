#!/usr/bin/env python3
"""The kernel's NativeAdreno ABI is the one the userland is built against.

Checks, on the host (no XNU, no phone):
1. xnu/NativeAdrenoABI.h is byte for byte the header phone/turnip/build.sh pins
   (ABI_HEADER_SHA256: UniXNU's drivers/NativeAdreno/NativeAdrenoABI.h, the copy
   ios-flame's phone/turnip and Turnip's darwin backend compile against);
2. the C records of xnu/andromeda_gpu_mtl.h have the size and the field offsets
   of that header: the static_assert block of xnu/SweetAccelerator.cpp (the
   same block the kernel build evaluates) compiled with the host C++ compiler;
3. the C header alone compiles as C11.
"""
from pathlib import Path
import hashlib
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / 'xnu'


def main():
    pin = re.search(r'^ABI_HEADER_SHA256=([0-9a-f]{64})$', (ROOT / 'phone/turnip/build.sh').read_text(), re.M)
    if not pin:
        raise SystemExit('phone/turnip/build.sh: no ABI_HEADER_SHA256 pin')
    got = hashlib.sha256((XNU / 'NativeAdrenoABI.h').read_bytes()).hexdigest()
    if got != pin.group(1):
        raise SystemExit(f'xnu/NativeAdrenoABI.h sha256 {got}, userland pins {pin.group(1)}: ABI mismatch')
    print(f'NativeAdrenoABI.h = the userland pin ({got[:12]}) PASS')

    source = (XNU / 'SweetAccelerator.cpp').read_text()
    block = source.split('/* The C mirror', 1)[1].split('/* ---- the service', 1)[0]
    asserts = block.count('static_assert(')
    if asserts < 15:
        raise SystemExit(f'SweetAccelerator.cpp: only {asserts} ABI static_asserts found')
    stub = '\n'.join([
        '#include <stddef.h>',
        '#include <stdint.h>',
        'typedef int IOReturn;',
        '#define kIOReturnSuccess 0',
        '#define kIOReturnNoMemory ((IOReturn)0xe00002bdu)',
        '#define kIOReturnNoResources ((IOReturn)0xe00002beu)',
        '#define kIOReturnBadArgument ((IOReturn)0xe00002c2u)',
        '#define kIOReturnUnsupported ((IOReturn)0xe00002c7u)',
        '#define kIOReturnVMError ((IOReturn)0xe00002c8u)',
        '#define kIOReturnBusy ((IOReturn)0xe00002d5u)',
        '#define kIOReturnTimeout ((IOReturn)0xe00002d6u)',
        '#define kIOReturnNotReady ((IOReturn)0xe00002d8u)',
        '#include "NativeAdrenoABI.h"',
        '#include "andromeda_gpu_mtl.h"',
        '/* The C mirror' + block,
        'int main(void) { return 0; }',
    ])
    with tempfile.TemporaryDirectory() as directory:
        unit = Path(directory) / 'abi.cpp'
        unit.write_text(stub)
        cxx = os.environ.get('CXX', 'c++')
        subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-I', str(XNU), str(unit)],
                       check=True)
        print(f'{asserts} record/constant checks of SweetAccelerator.cpp against NativeAdrenoABI.h PASS')
        cunit = Path(directory) / 'abi.c'
        cunit.write_text('#include "andromeda_gpu_mtl.h"\nint main(void) { return 0; }\n')
        cc = os.environ.get('CC', 'cc')
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', '-I', str(XNU), str(cunit)],
                       check=True)
        print('andromeda_gpu_mtl.h compiles as C11 PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
