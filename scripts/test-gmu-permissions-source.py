#!/usr/bin/env python3
"""Compare actual GMU mapping calls against device-PTE policy; no execution."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import os

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernel',type=Path);a=p.parse_args()
    source=(a.kernel/'drivers/gpu/msm/kgsl_gmu.c').read_text()
    pg=(a.kernel/'drivers/iommu/io-pgtable-arm.c').read_text()
    smmu=(a.kernel/'drivers/iommu/arm-smmu.c').read_text()
    # Refuse a changed parser/PRIVCFG implementation rather than silently
    # claiming that just finding the mapping-call flags proves semantics.
    for term in ('if (prot & IOMMU_WRITE)','(prot & IOMMU_PRIV) ? ARM_LPAE_PTE_AP_PRIV_RW',
                 'if (prot & IOMMU_NOEXEC)','pte |= ARM_LPAE_PTE_XN;'):
        if term not in pg:raise ValueError('source PTE semantics changed')
    if 's2cr[idx].privcfg = S2CR_PRIVCFG_DEFAULT;' not in smmu:raise ValueError('source stream privilege changed')
    mair=re.search(r'^#define\s+ARM_LPAE_MAIR_ATTR_NC\s+(0x[0-9a-fA-F]+)',pg,re.M)
    if not mair or int(mair[1],0)!=0x44:raise ValueError('source NC encoding changed')
    roles=('icache_mem','dcache_mem','hfi_mem','gmu_log','persist_mem');expected=[]
    for role in roles:
        m=re.search(r'gmu->'+role+r'\s*=\s*allocate_gmu_kmem\(gmu,.*?\((IOMMU_[A-Z_ |]+)\)\);',source,re.S)
        if not m:raise ValueError('missing mapping '+role)
        flags=set(re.findall(r'IOMMU_\w+',m[1]))
        if not flags<={'IOMMU_READ','IOMMU_WRITE','IOMMU_PRIV','IOMMU_NOEXEC'} or 'IOMMU_READ' not in flags:
            raise ValueError('unexpected mapping flags')
        # Compare AP/XN/shareability/AF/nG directly. AttrIndx is an arbitrary
        # slot: KGSL uses NC slot0 while our MAIR uses NC slot1. Decode MAIR
        # separately rather than calling numerically different PTEs identical.
        value=3|(2<<8)|(1<<10)|(1<<11)
        if 'IOMMU_WRITE' not in flags:value|=2<<6
        if 'IOMMU_PRIV' not in flags:value|=1<<6
        if 'IOMMU_NOEXEC' in flags:value|=3<<53
        expected.append(value)
    snippet='''#include <assert.h>
#include "cepheus_gpu_va.h"
#include "cepheus_smmu_plan.h"
static uint64_t words[8*512];
static const uint64_t expected[]={'''+','.join(str(v)+'ULL' for v in expected)+'''};
int main(void) {
 struct ceph_gpu_backing b[5]={{0x120000000ULL,0x3c000},{0x120040000ULL,0x3c000},{0x120080000ULL,0x8000},{0x120090000ULL,0x4000},{0x1200a0000ULL,0x4000}};
 struct ceph_gpu_va_plan p;assert(!ceph_gpu_va_plan(b,&p));
 struct ceph_vm_pool pool={words,0x110000000ULL,8,0};assert(!ceph_vm_build(&pool,p.regions,5));
 struct ceph_smmu_stream stream[8]={0};for(unsigned i=0;i<8;++i)stream[i].s2cr=2u<<16;
 struct ceph_smmu_plan s;assert(!ceph_smmu_gmu_plan(stream,8,0,1,8,5,2,pool.pa,0,&s));
 for(unsigned i=0;i<5;++i)for(uint64_t off=0;off<p.regions[i].bytes;off+=4096) {
  uint64_t pa,flags;assert(!ceph_vm_lookup(&pool,p.regions[i].va+off,&pa,&flags));assert((flags&~(7ULL<<2))==expected[i]);
  unsigned index=(unsigned)((flags>>2)&7);assert(index<4 && ((s.mair0>>(8*index))&255)==0x44);
 }
 assert(!(s.s2cr_translate&(3u<<24))); /* PRIVCFG default, not force-privileged */
 assert(!(s.sctlr&((1u<<19)|(1u<<20)))); /* no WXN/UWXN override */
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(snippet);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('GMU per-page AP/XN/NC + stream privilege matches supplied KGSL source: PASS (device tables only)')


if __name__=='__main__':main()
