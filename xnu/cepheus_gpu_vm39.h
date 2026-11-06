/* Sparse CPU-built AArch64 39-bit VA / 40-bit PA stage1 tables for future GX.
 * Independent of GMU32/SID5. 4K pages, L1/L2/L3, max16 regions of<=256KiB;
 * matches bounded CP allocation sizes, not a general userspace VM manager.
 * Does NOT select a GPU CB/ASID/SMR, TCR or install/unmap any live translation.
 */
#ifndef CEPHEUS_GPU_VM39_H
#define CEPHEUS_GPU_VM39_H
#include "cepheus_gpu_vm.h"
static inline int ceph_vm39_valid(const struct ceph_vm_region *r)
{
    if(!r || !r->bytes || r->bytes>256*1024 || ((r->va|r->pa|r->bytes)&4095u) ||
        r->va>=(1ULL<<39) || r->bytes>(1ULL<<39)-r->va || r->pa>=(1ULL<<40) ||
        r->bytes>(1ULL<<40)-r->pa || !(r->permissions&CE_VM_READ) || (r->permissions&~31u))return 0;
    return 1;
}
static inline int ceph_vm39_build(struct ceph_vm_pool *p,const struct ceph_vm_region *r,unsigned count)
{
    if(!p || !p->words || ((uintptr_t)p->words&7u) || (p->pa&4095u) || p->used ||
        !p->pages || p->pages>64 || p->pa>=(1ULL<<40) || (uint64_t)p->pages*4096>(1ULL<<40)-p->pa ||
        !r || !count || count>16)return -1;
    uintptr_t out=(uintptr_t)p->words,in=(uintptr_t)r;
    size_t out_bytes=p->pages*4096u,in_bytes=count*sizeof(*r);
    if(out_bytes>UINTPTR_MAX-out || in_bytes>UINTPTR_MAX-in ||
        (out<in+in_bytes && in<out+out_bytes))return -1;
    /* Every bounded region crosses at most one2MiB boundary, hence <=32
     * distinct L2/L3 keys. Avoid a39-bit-address-space bitmap on kernel stack. */
    unsigned l2[32],l3[32],n2=0,n3=0;
    for(unsigned i=0;i<count;++i) {
        if(!ceph_vm39_valid(&r[i]) ||
            (r[i].pa<p->pa+(uint64_t)p->pages*4096 && p->pa<r[i].pa+r[i].bytes))return -1;
        for(unsigned j=0;j<i;++j)
            if((r[i].va<r[j].va+r[j].bytes && r[j].va<r[i].va+r[i].bytes) ||
                (r[i].pa<r[j].pa+r[j].bytes && r[j].pa<r[i].pa+r[i].bytes))return -1;
        for(uint64_t va=r[i].va;va<r[i].va+r[i].bytes;va=((va>>21)+1)<<21) {
            unsigned k2=(unsigned)(va>>30),k3=(unsigned)(va>>21),found2=0,found3=0;
            for(unsigned j=0;j<n2;++j)if(l2[j]==k2)found2=1;
            for(unsigned j=0;j<n3;++j)if(l3[j]==k3)found3=1;
            if(!found2){if(n2==32)return -2;l2[n2++]=k2;}
            if(!found3){if(n3==32)return -2;l3[n3++]=k3;}
        }
    }
    if(1+n2+n3>p->pages)return -2;
    for(unsigned i=0;i<p->pages*512;++i)p->words[i]=0;
    p->used=1;
    for(unsigned i=0;i<count;++i)for(uint64_t off=0;off<r[i].bytes;off+=4096) {
        uint64_t va=r[i].va+off;uint64_t *table=p->words;
        unsigned indices[]={(unsigned)(va>>30),(unsigned)((va>>21)&511),(unsigned)((va>>12)&511)};
        for(unsigned level=0;level<2;++level) {
            if(!table[indices[level]])table[indices[level]]=(p->pa+(uint64_t)p->used++*4096)|3;
            table=ceph_vm_table(p,table[indices[level]]);
            if(!table){p->used=0;return -1;}
        }
        table[indices[2]]=ceph_vm_leaf(r[i].pa+off,r[i].permissions);
    }
    return 0;
}
static inline int ceph_vm39_lookup(struct ceph_vm_pool *p,uint64_t va,uint64_t *pa,uint64_t *flags)
{
    if(!p || !p->words || ((uintptr_t)p->words&7u) || (p->pa&4095u) || !p->used ||
        p->used>p->pages || p->pages>64 || va>=(1ULL<<39) || !pa || !flags)return -1;
    uint64_t *t=ceph_vm_table(p,p->words[va>>30]);if(!t)return -1;
    t=ceph_vm_table(p,t[(va>>21)&511]);if(!t)return -1;
    uint64_t leaf=t[(va>>12)&511];if((leaf&3)!=3)return -1;
    *pa=(leaf&CE_VM_PA_MASK)|(va&4095);*flags=leaf&~CE_VM_PA_MASK;return 0;
}
static inline int ceph_vm39_matches(struct ceph_vm_pool *p,const struct ceph_vm_region *r,unsigned n)
{return ceph_vm_matches_limit(p,r,n,39);}
#endif
