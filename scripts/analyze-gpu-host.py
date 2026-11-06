#!/usr/bin/env python3
"""Clang static analysis of production units using host ABI/IOKit test doubles.

Warnings fail this gate. This cannot prove XNU/device behavior or data-race
freedom; run the actual XNU build and hardware acceptance separately.
"""
from pathlib import Path
import os
import subprocess

ROOT=Path(__file__).resolve().parents[1]
C_UNITS=('cepheus_gmu_bringup.c','cepheus_gmu_hfi_link.c','cepheus_gmu_workspace.c',
         'cepheus_gpu_retire.c','cepheus_cp_workspace.c','cepheus_gpu_compile.c')


def analyze(compiler,standard,source,extra=()):
    command=[compiler,'--analyze','-std='+standard,'-I',str(ROOT/'xnu'),*extra,
             '-Xanalyzer','-analyzer-output=text','-o',os.devnull,str(ROOT/'xnu'/source)]
    result=subprocess.run(command,capture_output=True,text=True)
    if result.stdout:print(result.stdout,end='')
    if result.stderr:print(result.stderr,end='')
    if result.returncode or 'warning:' in result.stderr or 'error:' in result.stderr:
        raise SystemExit(f'static analysis failed: {source}')
    print('static analysis PASS:',source)


def main():
    for source in C_UNITS:analyze(os.environ.get('CLANG','clang'),'c11',source)
    analyze(os.environ.get('CLANGXX','clang++'),'c++17','cepheus_gpu_memory.cpp',
            ('-DCEPHEUS_GPU_MEMORY_HOST_TEST=1','-I',str(ROOT/'tests/gpu-memory-host')))
    print('7 production units: host Clang static analysis no warnings (not XNU/hardware proof)')


if __name__=='__main__':main()
