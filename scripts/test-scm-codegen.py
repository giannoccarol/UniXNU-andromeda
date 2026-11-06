#!/usr/bin/env python3
"""Compile the actual private SMC callback for AArch64; never execute it."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/cepheus_gpu_compile.c').read_text()
body = source.split('static int ceph_scm_info_invoke', 1)[1].split('int cepheus_gpu_scm_probe_once', 1)[0]
preamble = '''
typedef unsigned long long uint64_t;
#define NULL ((void *)0)
struct ceph_scm_regs { uint64_t x[8]; };
extern int ceph_scm_info_context(void *);
'''
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    file = work / 'scm.c'
    file.write_text(preamble + 'int ceph_scm_info_invoke' + body)
    assembly = work / 'scm.s'
    subprocess.run(['clang', '--target=aarch64-none-elf', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-ffreestanding', '-S', str(file), '-o', str(assembly)], check=True)
    text = assembly.read_text()
    assert text.count('smc\t#0') == 1, 'one physical SMC per callback'
    # Return the A6 quirk cookie and input/output values, not stale inputs.
    assert 'x6' in text and 'x7' in text and 'ceph_scm_info_context' in text
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(assembly), '-o', str(work / 'scm.o')], check=True)
print('Production SMC callback: AArch64 compile/assemble PASS; not executed and not a PAS proof')
