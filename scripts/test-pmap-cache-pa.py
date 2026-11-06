#!/usr/bin/env python3
"""Apply the production patch and compile its actual cache callback, no device."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = Path('osfmk/arm/pmap/pmap.c')
OLD = 'flush_dcache((vm_offset_t) (pp << PAGE_SHIFT), PAGE_SIZE, TRUE);'
NEW = 'flush_dcache((vm_offset_t) ptoa(pp), PAGE_SIZE, TRUE);'
FUNCTION = '''void
pmap_sync_page_attributes_phys(
\tppnum_t pp)
{
\t''' + OLD + '''
}

#if CONFIG_COREDUMP
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, default=os.environ.get('CEPHEUS_XNU_REFERENCE'))
    args = parser.parse_args()
    original = (args.xnu / TARGET).read_text() if args.xnu else FUNCTION
    assert original.count(OLD) == 1, 'expected original 8796 callback'
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        subprocess.run(['git', 'init', '-q', str(work)], check=True)
        file = work / TARGET
        file.parent.mkdir(parents=True)
        file.write_text(original)
        subprocess.run(['git', '-C', str(work), 'apply', str(ROOT / 'xnu/0088-cepheus-pmap-cache-pa.patch')], check=True)
        source = file.read_text()
        assert OLD not in source and source.count(NEW) == 1
        callback = re.search(r'\n?void\npmap_sync_page_attributes_phys\(\n\s*ppnum_t pp\)\n\{.*?\n\}', source, re.S).group().lstrip() + '\n'
        # Match the actual XNU ptoa macro; the cast precedes the shift.
        for shift in (12, 14):
            preamble = f'''#include <stdint.h>
#include <assert.h>
typedef uint32_t ppnum_t;
typedef uint64_t vm_offset_t;
#define PAGE_SHIFT {shift}
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define TRUE 1
#define ptoa(x) ((vm_offset_t)(x) << PAGE_SHIFT)
static uint64_t captured;
void flush_dcache(vm_offset_t pa, unsigned bytes, int physical) {{
    assert(bytes == PAGE_SIZE && physical == TRUE); captured = pa;
}}
'''
            test = preamble + callback + '''
int main(void) {
    const uint64_t pa = 0x18439c000ULL;
    const ppnum_t pn = (ppnum_t)(pa >> PAGE_SHIFT);
    assert((uint64_t)(pn << PAGE_SHIFT) == 0x8439c000ULL);
    pmap_sync_page_attributes_phys(pn);
    assert(captured == pa);
    pmap_sync_page_attributes_phys(UINT32_MAX);
    assert(captured == (uint64_t)UINT32_MAX << PAGE_SHIFT);
}
'''
            unit = work / f'cache{shift}.c'
            unit.write_text(test)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(unit), '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)
            # Compile the production callback for the actual target too.
            unit.write_text(preamble.split('static uint64_t captured;', 1)[0].replace('#include <assert.h>\n', '') +
                            'extern void flush_dcache(vm_offset_t, unsigned, int);\n' + callback)
            subprocess.run(['clang', '--target=aarch64-none-elf', '-std=c11', '-ffreestanding', '-O2',
                            '-S', str(unit), '-o', str(work / 'cache.s')], check=True)
            assembly = (work / 'cache.s').read_text()
            assert f'ubfiz\tx0, x0, #{shift}, #32' in assembly, assembly
            assert 'lsl\tw0' not in assembly
    print('Pmap cache PA: production patch applied, real panic low32 reproduced, 4/16KiB full-PA callback + AArch64 codegen PASS')
    print('Scope: ' + ('explicit XNU source' if args.xnu else 'synthetic original context; CI checks applied source and kernel code separately'))


if __name__ == '__main__':
    main()
