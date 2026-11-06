#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_vm39.h"
static uint64_t tables[64*512];
int main(void)
{
    struct ceph_vm_pool p={tables,0x110000000ULL,64,0};
    struct ceph_vm_region r[]={{0xfc000000,0x120000000ULL,32768,CE_VM_READ|CE_VM_NOCACHE},
        {0x100000000ULL,0x120010000ULL,16384,CE_VM_READ|CE_VM_WRITE|CE_VM_NOCACHE},
        {0x5fffff000ULL,0x120020000ULL,8192,CE_VM_READ|CE_VM_WRITE|CE_VM_NOCACHE},
        {(1ULL<<39)-4096,0x120030000ULL,4096,CE_VM_READ|CE_VM_NOCACHE}};
    assert(!ceph_vm39_build(&p,r,4));
    assert(!ceph_vm39_matches(&p,r,4) && ceph_vm_matches(&p,r,4)<0);
    tables[63*512]=3;assert(ceph_vm39_matches(&p,r,4)<0);tables[63*512]=0;
    for(unsigned i=0;i<4;++i)for(uint64_t off=0;off<r[i].bytes;off+=4096) {
        uint64_t pa,flags;assert(!ceph_vm39_lookup(&p,r[i].va+off+123,&pa,&flags) && pa==r[i].pa+off+123);
        assert(flags==(ceph_vm_leaf(r[i].pa,r[i].permissions)&~CE_VM_PA_MASK));
    }
    uint64_t pa,flags;assert(ceph_vm_lookup(&p,0x100000000ULL,&pa,&flags)<0); /* GMU32 not widened */
    assert(ceph_vm39_lookup(&p,1ULL<<39,&pa,&flags)<0 && ceph_vm39_lookup(&p,0x200000000ULL,&pa,&flags)<0);
    unsigned needed=p.used;assert(needed>8 && needed<64);
    p.used=0;p.pages=needed-1;tables[0]=0xabc;
    assert(ceph_vm39_build(&p,r,4)==-2 && !p.used && tables[0]==0xabc);
    p.pages=64;r[1].pa=r[0].pa;assert(ceph_vm39_build(&p,r,4)<0 && tables[0]==0xabc);r[1].pa=0x120010000ULL;
    r[3].bytes=8192;assert(ceph_vm39_build(&p,r,4)<0);r[3].bytes=4096;
    r[0].bytes=256*1024+4096;assert(ceph_vm39_build(&p,r,4)<0);r[0].bytes=32768;
    r[0].pa=p.pa;assert(ceph_vm39_build(&p,r,4)<0);
    puts("GX sparse39bit VA/40bit PA tables: >4GiB/top-page/cross-L1/aliases/budget atomicity PASS (CPU only, no SMMU)");
}
