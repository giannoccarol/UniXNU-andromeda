#!/usr/bin/env python3
"""Decode source-defined ID fields and compare compiled GMU/GX context policy."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('iommu_dir',type=Path);a=p.parse_args()
    registers=(a.iommu_dir/'arm-smmu-regs.h').read_text();page=(a.iommu_dir/'io-pgtable-arm.c').read_text()
    groups=[(registers,('ID0_S1TS','ID0_SMS','ID0_NUMSIDB_SHIFT','ID1_NUMPAGENDXB_SHIFT','ID1_NUMS2CB_SHIFT',
                        'ID2_PTFS_4K','ID2_UBS_SHIFT','ID2_IAS_SHIFT','ID2_OAS_SHIFT','TTBCR2_SEP_SHIFT','TTBCR2_SEP_UPSTREAM')),
            (page,('ARM_LPAE_TCR_EPD1','ARM_LPAE_TCR_SH0_SHIFT','ARM_LPAE_TCR_SH_OS',
                   'ARM_LPAE_TCR_T0SZ_SHIFT','ARM_LPAE_TCR_PS_40_BIT'))]
    macros=[]
    for text,names in groups:
        for name in names:
            m=re.search(r'^#define\s+'+name+r'\s+[^\n]+$',text,re.M)
            if not m:raise ValueError('missing '+name)
            macros.append(m[0])
    code='''#include <assert.h>
#include "cepheus_gx_smmu_plan.h"
'''+ '\n'.join(macros)+'''
int main(void) {
 uint32_t id0=ID0_S1TS|ID0_SMS|(11u<<ID0_NUMSIDB_SHIFT)|16;
 uint32_t id1=(2u<<ID1_NUMPAGENDXB_SHIFT)|(1u<<ID1_NUMS2CB_SHIFT)|8;
 uint32_t id2=ID2_PTFS_4K|(2u<<ID2_UBS_SHIFT)|(2u<<ID2_IAS_SHIFT)|(2u<<ID2_OAS_SHIFT);
 struct ceph_smmu_geometry g;assert(!ceph_smmu_geometry(id0,id1,id2,0x10000,&g));
 assert(g.va_bits==40 && g.ipa_bits==40 && g.pa_bits==40 && g.s2_contexts==1);
 struct ceph_smmu_stream streams[16]={0};for(unsigned i=0;i<16;++i)streams[i].s2cr=2u<<16;
 struct ceph_gx_smmu_plan gx;assert(!ceph_gx_smmu_plan(streams,16,&g,0,2,3,0x120000000ULL,0,&gx));
 unsigned base=ARM_LPAE_TCR_EPD1|(ARM_LPAE_TCR_SH_OS<<ARM_LPAE_TCR_SH0_SHIFT);
 assert(gx.context.tcr==(base|((64u-39)<<ARM_LPAE_TCR_T0SZ_SHIFT)));
 assert(gx.context.tcr2==(TTBCR2_SEP_UPSTREAM|ARM_LPAE_TCR_PS_40_BIT));
 struct ceph_smmu_plan gmu;assert(!ceph_smmu_gmu_plan(streams,16,1,3,8,5,4,0x120001000ULL,0,&gmu));
 assert(gmu.tcr==(base|((64u-32)<<ARM_LPAE_TCR_T0SZ_SHIFT)) && gmu.tcr2==gx.context.tcr2);
 id2&=~(15u<<ID2_UBS_SHIFT);assert(!ceph_smmu_geometry(id0,id1,id2,0x10000,&g) && g.va_bits==32);
 assert(ceph_gx_smmu_plan(streams,16,&g,0,2,3,0x120000000ULL,0,&gx)<0);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('SMMU source macros agree with VA/IPA/PA geometry and NC GMU32/GX39 TCR policy: PASS (no hardware)')


if __name__=='__main__':main()
