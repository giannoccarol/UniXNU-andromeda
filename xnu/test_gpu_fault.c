#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_fault.h"
struct mock {uint64_t r[CE_FAULT_REG_COUNT];unsigned domains,call,fail,end,reads,fsr_reads;int changing;};
static int step(struct mock *m){return ++m->call==m->fail?-1:0;}
static int begin(void *p,unsigned *d){struct mock *m=p;*d=m->domains;return step(m);}
static int rd(void *p,enum ceph_fault_reg r,uint64_t *v){struct mock *m=p;m->reads++;if(r<=CE_FAULT_CP_PROTECT)assert(m->domains&1);if(r==CE_FAULT_CB_FSR && ++m->fsr_reads>1 && m->changing)m->r[r]=8;*v=m->r[r];return step(m);}
static int now(void *p,uint64_t *n){*n=12345;return step(p);}
static void end(void *p){((struct mock *)p)->end++;}
int main(void)
{
    struct mock m={.domains=7};struct ceph_gpu_fault_ops b={&m,begin,rd,now,end};struct ceph_gpu_fault_record r;
    assert(!ceph_gpu_fault_snapshot(&b,&r) && ceph_gpu_fault_status(&r)==0 && r.time_ns==12345);
    unsigned calls=m.call;
    for(unsigned i=1;i<=calls;++i){m=(struct mock){.domains=7,.fail=i};assert(ceph_gpu_fault_snapshot(&b,&r)<0 && !r.domains && m.end==(i==1?0u:1u));}
    m=(struct mock){.domains=6};assert(!ceph_gpu_fault_snapshot(&b,&r) && ceph_gpu_fault_status(&r)==-1);
    m=(struct mock){.domains=7};m.r[CE_FAULT_CP_IRQ]=20;m.r[CE_FAULT_CP_HW]=0x44;m.r[CE_FAULT_CP_PROTECT]=0x100123;
    assert(!ceph_gpu_fault_snapshot(&b,&r) && ceph_gpu_fault_status(&r)==1 && r.cp_hw==0x44 && r.cp_protect==0x100123);
    m=(struct mock){.domains=4,.changing=1};m.r[CE_FAULT_CB_FSR]=2;m.r[CE_FAULT_CB_FAR]=0x123456789;
    assert(!ceph_gpu_fault_snapshot(&b,&r) && !r.coherent && r.fsr==8 && ceph_gpu_fault_status(&r)==1);
    m=(struct mock){.domains=7};m.r[CE_FAULT_RBBM_IRQ]=1u<<20;
    assert(!ceph_gpu_fault_snapshot(&b,&r) && ceph_gpu_fault_status(&r)==0); /* fence interrupt isn't an error */
    m=(struct mock){.domains=7};m.r[CE_FAULT_GMU_AO]=UINT32_MAX;assert(ceph_gpu_fault_snapshot(&b,&r)<0);
    puts("GPU fault snapshot: gated reads, CP details, SMMU FAR coherence, incomplete != clean, no clears PASS");
}
