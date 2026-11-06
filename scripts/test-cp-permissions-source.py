#!/usr/bin/env python3
"""Compile downstream KGSL protection conversion against current CP role policy.

The domain-attribute test stub selects the explicit NC/no-LLC-hint baseline;
this is not discovery of the live GPU domain's attributes.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernel',type=Path);a=p.parse_args()
    kgsl=a.kernel/'drivers/gpu/msm';source=(kgsl/'kgsl_iommu.c').read_text()
    function=re.search(r'static unsigned int _get_protection_flags\([^)]*\)\s*\{.*?\n\}',source,re.S)
    if not function:raise ValueError('missing source conversion')
    # Ensure source allocations still select the role flags being compared.
    ring=(kgsl/'adreno_ringbuffer.c').read_text();adreno=(kgsl/'adreno_a6xx.c').read_text()
    for text,pattern in ((ring,r'&rb->buffer_desc,\s*KGSL_RB_SIZE,\s*KGSL_MEMFLAGS_GPUREADONLY,\s*0,\s*"ringbuffer"'),
                         (ring,r'priv\s*=\s*KGSL_MEMDESC_RANDOM\s*\|\s*KGSL_MEMDESC_PRIVILEGED'),
                         (adreno,r'&firmware->memdesc, fw->size - 4,\s*KGSL_MEMFLAGS_GPUREADONLY, KGSL_MEMDESC_UCODE'),
                         (adreno,r'&adreno_dev->pwrup_reglist,\s*PAGE_SIZE, 0, KGSL_MEMDESC_CONTIG \| KGSL_MEMDESC_PRIVILEGED')):
        if not re.search(pattern,text):raise ValueError('source allocation flags changed: '+pattern)
    macros=['#define BIT(n) (1u << (n))']
    for path,names in ((a.kernel/'include/linux/iommu.h',('IOMMU_READ','IOMMU_WRITE','IOMMU_NOEXEC','IOMMU_PRIV','IOMMU_CACHE','IOMMU_USE_UPSTREAM_HINT','IOMMU_USE_LLC_NWA')),
                       (a.kernel/'include/uapi/linux/msm_kgsl.h',('KGSL_MEMFLAGS_GPUREADONLY','KGSL_MEMFLAGS_IOCOHERENT')),
                       (kgsl/'kgsl.h',('KGSL_MEMDESC_PRIVILEGED','KGSL_MEMDESC_UCODE'))):
        text=path.read_text()
        for name in names:
            match=re.search(r'^#define\s+'+name+r'\s+[^\n]+$',text,re.M)
            if not match:raise ValueError('missing macro '+name)
            macros.append(match[0])
    code='''#include <assert.h>
#include "cepheus_cp_workspace.h"
'''+ '\n'.join(macros)+'''
struct kgsl_iommu_pt {void *domain;};
struct kgsl_pagetable {void *priv;};
struct kgsl_memdesc {uint64_t flags,priv;};
enum {DOMAIN_ATTR_USE_UPSTREAM_HINT,DOMAIN_ATTR_USE_LLC_NWA};
static int iommu_domain_get_attr(void *d,int key,int *out){(void)d;(void)key;*out=0;return 0;}
'''+function[0]+'''
int main(void) {
 struct kgsl_iommu_pt domain={0};struct kgsl_pagetable pt={&domain};
 struct kgsl_memdesc desc[5]={{KGSL_MEMFLAGS_GPUREADONLY,0},{0,KGSL_MEMDESC_PRIVILEGED},
   {KGSL_MEMFLAGS_GPUREADONLY,KGSL_MEMDESC_UCODE},{0,KGSL_MEMDESC_PRIVILEGED},{0,KGSL_MEMDESC_PRIVILEGED}};
 for(unsigned i=0;i<5;++i) {
  unsigned flags=_get_protection_flags(&pt,&desc[i]);
  unsigned expected=CE_VM_READ|CE_VM_NOCACHE;
  if(flags&IOMMU_WRITE)expected|=CE_VM_WRITE;
  if(!(flags&IOMMU_NOEXEC))expected|=CE_VM_EXEC;
  if(!(flags&IOMMU_PRIV))expected|=CE_VM_UNPRIV;
  assert(!(flags&IOMMU_CACHE) && ceph_cp_workspace_permissions(i)==expected);
 }
 assert(!ceph_cp_workspace_permissions(CE_CP_ROOT));
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('CP role permissions agree with extracted KGSL conversion and allocation flags: PASS (NC baseline, not live domain)')


if __name__=='__main__':main()
