#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_va.h"
static uint64_t tables[8*512];
int main(void)
{
    struct ceph_gpu_backing b[]={{0x120000000ULL,0x3c000},{0x120040000ULL,0x3c000},
        {0x120080000ULL,0x8000},{0x120090000ULL,0x4000},{0x1200a0000ULL,0x4000}};
    struct ceph_gpu_va_plan plan;assert(!ceph_gpu_va_plan(b,&plan));
    assert(plan.start.log_va==0x60200000 && plan.regions[2].bytes==0x5000 && plan.regions[4].va==0x60000000);
    struct ceph_vm_pool pool={tables,0x110000000ULL,8,0};
    assert(!ceph_vm_build(&pool,plan.regions,CE_BUF_COUNT) && pool.used==6);
    uint64_t pa,flags;assert(!ceph_vm_lookup(&pool,0x4000,&pa,&flags) && pa==b[0].pa);
    assert((flags&(3u<<6))==0 && (flags&(7u<<2))==(1u<<2) && !(flags&(3ULL<<53)));
    for(unsigned i=0;i<CE_BUF_COUNT;++i)for(uint64_t off=0;off<plan.regions[i].bytes;off+=4096) {
        assert(!ceph_vm_lookup(&pool,plan.regions[i].va+off,&pa,&flags) && pa==b[i].pa+off);
        uint64_t expected=3u|(1u<<2)|(2u<<8)|(1u<<10)|(1u<<11)|(i==CE_BUF_HFI?1u<<6:0);
        assert(flags==expected); /* KGSL RW, no NOEXEC; only HFI is unprivileged */
    }
    assert(!ceph_vm_lookup(&pool,0x60104000,&pa,&flags) && (flags&(3u<<6))==(1u<<6));
    assert(!ceph_vm_lookup(&pool,0x60000000,&pa,&flags) && pa==b[4].pa && (flags&(3u<<6))==0);
    assert(ceph_vm_lookup(&pool,0x60105000,&pa,&flags)<0); /* host slack NOT mapped */
    assert(ceph_vm_lookup(&pool,0x600ff000,&pa,&flags)<0); /* 1MiB-spaced log */
    struct ceph_gpu_va_plan saved=plan;b[1].pa=b[0].pa;
    assert(ceph_gpu_va_plan(b,&plan)<0 && !memcmp(&saved,&plan,sizeof(plan)));
    b[1].pa=0x120040000ULL;b[2].capacity=4096;assert(ceph_gpu_va_plan(b,&plan)<0);
    b[2].capacity=0x8000;b[3].pa=(1ULL<<40)-4096;assert(ceph_gpu_va_plan(b,&plan)<0);
    puts("GMU VA plan: owned PA ranges, fixed cache/HFI/log VAs, exact per-page KGSL device AP/NC/XN, unmapped host slack PASS");
}
