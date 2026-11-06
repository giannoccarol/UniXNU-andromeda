#!/usr/bin/env python3
"""Compare restore register offsets and CP ordinals against explicit KGSL source."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('kgsl_dir',type=Path);a=p.parse_args()
    source=(a.kgsl_dir/'adreno_a6xx.c').read_text();regs=(a.kgsl_dir/'a6xx_reg.h').read_text()
    defs={k:int(v,0) for k,v in re.findall(r'^#define\s+(A6XX_\w+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)\s*$',regs,re.M)}
    arrays=[]
    for name in ('a6xx_ifpc_pwrup_reglist','a6xx_pwrup_reglist'):
        match=re.search(name+r'\[\]\s*=\s*\{(.*?)\n\};',source,re.S)
        if not match:raise ValueError('missing source register list')
        values=[defs[k]+int(extra or 0) for k,extra in re.findall(r'\{\s*(A6XX_\w+)(?:\s*\+\s*(\d+))?\s*,',match.group(1))]
        arrays.extend(values)
    if len(arrays)!=66:raise ValueError('source register count changed')
    lock=re.search(r'struct cpu_gpu_lock\s*\{[^{}]*\};',(a.kgsl_dir/'adreno_a6xx.h').read_text()).group(0)
    spans=re.search(r'a630_protected_regs\[\]\s*=\s*\{(.*?)\n\};',source,re.S).group(1)
    expected_spans=[]
    for index,first,last,deny in re.findall(r'\{\s*A6XX_CP_PROTECT_REG\s*\+\s*(\d+)\s*,\s*(0x[0-9a-f]+)\s*,\s*(0x[0-9a-f]+)\s*,\s*([01])\s*\}',spans):
        if int(index)!=len(expected_spans):raise ValueError('unexpected protection order')
        a,b=int(first,16),int(last,16);expected_spans.append(a|((b-a)<<18)|(int(deny)<<31))
    if len(expected_spans)!=32:raise ValueError('source protection count changed')
    mode_body=re.search(r'static void a6xx_enable_64bit\([^)]*\)\s*\{(.*?)\n\}',source,re.S).group(1)
    address_modes=[defs[k] for k in re.findall(r'kgsl_regwrite\(device,\s*(A6XX_\w+),\s*0x1\)',mode_body)]
    if len(address_modes)!=12:raise ValueError('source address mode count changed')
    snippet=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "cepheus_cp_init.h"
#include "cepheus_cp_ring_config.h"
''' + lock + '\nstatic const uint32_t offsets[]={' + ','.join(hex(v) for v in arrays) + '};\n' + \
        'static const uint32_t spans[]={' + ','.join(hex(v) for v in expected_spans) + '};\n' + \
        'static const uint32_t ringregs[]={' + ','.join(hex(defs[k]) for k in (
            'A6XX_CP_RB_RPTR_ADDR_LO','A6XX_CP_RB_RPTR_ADDR_HI','A6XX_CP_RB_CNTL',
            'A6XX_CP_RB_BASE','A6XX_CP_RB_BASE_HI','A6XX_CP_SQE_INSTR_BASE_LO','A6XX_CP_SQE_INSTR_BASE_HI')) + '};\n' + \
        'static const uint32_t modes[]={' + ','.join(hex(v) for v in address_modes) + '};\n' + r'''
_Static_assert(sizeof(struct cpu_gpu_lock)==16,"lock size");
_Static_assert(offsetof(struct cpu_gpu_lock,list_length)==12,"length offset");
_Static_assert(offsetof(struct cpu_gpu_lock,list_offset)==14,"list offset");
int main(void) {
    for(unsigned i=0;i<66;++i)assert(offsets[i]==ceph_cp_restore_offsets[i]);
    for(unsigned i=0;i<32;++i){uint32_t value;assert(!ceph_cp_protect_word(i,&value) && value==spans[i]);}
    for(unsigned i=0;i<12;++i){uint32_t reg,value;assert(!ceph_cp_protection_word(i,&reg,&value) && reg==modes[i] && value==1);}
    uint32_t packet[12];assert(ceph_cp_me_init(packet,12,0x100004000ULL,4096)==12);
    const uint32_t expected[]={0x70c8000b,0x16f,3,0x20000000,0,0,0,2,0x4000,1,0,0};
    for(unsigned i=0;i<12;++i)assert(packet[i]==expected[i]);
    struct ceph_cp_ring_input ri={0x100000000ULL,0x100010000ULL,0x100020000ULL,32768,4096,32768};
    struct ceph_cp_ring_plan rp;assert(!ceph_cp_ring_plan(&ri,&rp));
    for(unsigned i=0;i<7;++i)assert(rp.writes[i].word==ringregs[i]);
    unsigned log=0;for(unsigned n=8192/2;n>1;n>>=1)++log;
    assert(rp.writes[2].value==((1u<<27)|(2u<<8)|log));
}
'''
    # Fixed source mask is independently recomputed from declared bit positions.
    bits={name:int(n) for name,n in re.findall(r'^#define\s+(CP_INIT_\w+)\s+BIT\((\d+)\)',source,re.M)}
    enabled=('MAX_CONTEXT','ERROR_DETECTION_CONTROL','HEADER_DUMP','DEFAULT_RESET_STATE','UCODE_WORKAROUND_MASK','OPERATION_MODE_MASK','REGISTER_INIT_LIST_WITH_SPINLOCK')
    if sum(1<<bits['CP_INIT_'+name] for name in enabled)!=0x16f:raise ValueError('source ordinal mask changed')
    with tempfile.TemporaryDirectory(prefix='ceph-cp-source-') as tmp:
        c=Path(tmp)/'test.c';c.write_text(snippet);binary=Path(tmp)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('CP restore offsets/lock ABI/ME_INIT mask match supplied KGSL source: PASS')


if __name__=='__main__':main()
