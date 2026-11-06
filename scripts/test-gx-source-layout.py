#!/usr/bin/env python3
"""Source/FDT cross-check for the deliberately partial GX core plan."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('kgsl',type=Path);p.add_argument('fdt',type=Path);a=p.parse_args()
    spec=importlib.util.spec_from_file_location('fdt',ROOT/'scripts/gpu-fdt-config.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    config=mod.report(a.fdt.read_bytes())['gx']
    if [config[k] for k in ('highest_bank_bit','min_access_length','ubwc_mode','macrotiling_channels')]!=[15,32,3,None]:
        raise ValueError('source profile changed')
    regs=(a.kgsl/'a6xx_reg.h').read_text();source=(a.kgsl/'adreno_a6xx.c').read_text()
    defs={k:int(v,0) for k,v in re.findall(r'^#define\s+(A6XX_\w+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)\s*$',regs,re.M)}
    start=source.split('static void a6xx_start(',1)[1].split('static int a6xx_microcode_load(',1)[0]
    literals={k:int(v,0) for k,v in re.findall(r'kgsl_regwrite\(device,\s*(A6XX_\w+),\s*(0x[0-9a-fA-F]+|\d+)\s*\)',start)}
    # Branch-specific A640 values extracted from that branch, not the last
    # generic branch with the same register names.
    roq=start.split('if (adreno_is_a640(adreno_dev) || adreno_is_a680(adreno_dev)) {',1)[1].split('} else',1)[0]
    literals.update({k:int(v,0) for k,v in re.findall(r'kgsl_regwrite\(device,\s*(A6XX_\w+),\s*(0x[0-9a-fA-F]+|\d+)\s*\)',roq)})
    core=(a.kgsl/'adreno-gpulist.h').read_text().split('.gpurev = ADRENO_REV_A640,',1)[1].split('\n\t},',1)[0]
    if not re.search(r'\.gmem_size\s*=\s*SZ_1M\s*,',core):raise ValueError('GMEM source differs')
    names=['UCHE_WRITE_RANGE_MAX_LO','UCHE_WRITE_RANGE_MAX_HI','UCHE_TRAP_BASE_LO','UCHE_TRAP_BASE_HI',
           'UCHE_WRITE_THRU_BASE_LO','UCHE_WRITE_THRU_BASE_HI','UCHE_GMEM_RANGE_MIN_LO','UCHE_GMEM_RANGE_MIN_HI',
           'UCHE_GMEM_RANGE_MAX_LO','UCHE_GMEM_RANGE_MAX_HI','UCHE_FILTER_CNTL','UCHE_CACHE_WAYS',
           'CP_ROQ_THRESHOLDS_2','CP_ROQ_THRESHOLDS_1','CP_MEM_POOL_SIZE','PC_DBG_ECO_CNTL','CP_AHB_CNTL',
           'RB_NC_MODE_CNTL','TPL1_NC_MODE_CNTL','SP_NC_MODE_CNTL','UCHE_MODE_CNTL','RBBM_INTERFACE_HANG_INT_CNTL','UCHE_CLIENT_PF']
    calculated={'UCHE_GMEM_RANGE_MIN_LO':0x100000,'UCHE_GMEM_RANGE_MAX_LO':0x1fffff,
        'PC_DBG_ECO_CNTL':0x400<<11,'RB_NC_MODE_CNTL':(1<<4)|(2<<1),
        'TPL1_NC_MODE_CNTL':2<<1,'SP_NC_MODE_CNTL':2<<1,'UCHE_MODE_CNTL':2<<21,
        'RBBM_INTERFACE_HANG_INT_CNTL':(1<<30)|0xcfffff}
    values=[calculated[n] if n in calculated else literals['A6XX_'+n] for n in names]
    expected=','.join('{'+hex(defs['A6XX_'+n])+','+hex(v)+'}' for n,v in zip(names,values))
    code='''#include <assert.h>
#include "cepheus_gx_core.h"
static const struct ceph_gx_write expected[]={'''+expected+'''};
int main(void){struct ceph_gx_core_input i={0x06040001,0x100000,0x100000,15,32,3};struct ceph_gx_core_plan p;
 assert(!ceph_gx_core_plan(&i,&p));for(unsigned n=0;n<23;++n)assert(p.writes[n].word==expected[n].word && p.writes[n].value==expected[n].value);}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('GX core subset: supplied KGSL register/literal/GMEM + saved FDT UBWC profile agrees (not full GX init)')


if __name__=='__main__':main()
