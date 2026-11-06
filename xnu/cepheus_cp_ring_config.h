/* A640 ring/SQE address configuration ONLY while SQE halt is already asserted.
 * Not GX initialization, PAS authentication, CP_ME_INIT or non-secure entry.
 * Never writes SQE_CNTL/WPTR/TRUST_CONTROL. Caller must own GPU mappings,
 * initialized GX/protection/address modes, OOB keepalive and a verified SQE
 * payload. No runtime hook or default lease can manufacture these conditions.
 */
#ifndef CEPHEUS_CP_RING_CONFIG_H
#define CEPHEUS_CP_RING_CONFIG_H
#include "cepheus_gmu_mmio.h"
#include "cepheus_cp_protection.h"
struct ceph_cp_ring_input {uint64_t ring_va,rptr_va,sqe_va;size_t ring_bytes,rptr_bytes,sqe_bytes;};
struct ceph_cp_ring_write {uint32_t word,value;};
struct ceph_cp_ring_plan {struct ceph_cp_ring_write writes[7];};
static inline int ceph_cp_ring_plan(const struct ceph_cp_ring_input *in,struct ceph_cp_ring_plan *out)
{
    if(!in || !out || in->ring_bytes!=32768 || in->rptr_bytes<4096 || in->rptr_bytes>16384 ||
        in->sqe_bytes!=32768)return -1;
    uint64_t va[]={in->ring_va,in->rptr_va,in->sqe_va};
    size_t bytes[]={in->ring_bytes,in->rptr_bytes,in->sqe_bytes};
    for(unsigned i=0;i<3;++i) {
        if(va[i]<0x300000 || ((va[i]|bytes[i])&4095u) || va[i]>=(1ULL<<39) || bytes[i]>(1ULL<<39)-va[i])return -1;
        for(unsigned j=0;j<i;++j)if(va[i]<va[j]+bytes[j] && va[j]<va[i]+bytes[i])return -1;
    }
    /* KGSL_RB_DWORDS=8192: log2(8192/2)=12; RPTR writeback block=log2(4)=2. */
    *out=(struct ceph_cp_ring_plan){{{0x804,(uint32_t)in->rptr_va},{0x805,(uint32_t)(in->rptr_va>>32)},
        {0x802,0x0800020c},{0x800,(uint32_t)in->ring_va},{0x801,(uint32_t)(in->ring_va>>32)},
        {0x830,(uint32_t)in->sqe_va},{0x831,(uint32_t)(in->sqe_va>>32)}}};return 0;
}
struct ceph_cp_ring_config {
    struct ceph_gmu_mmio *gmu;
    volatile uint32_t *gpu;size_t bytes;
    struct ceph_cp_ring_input input;
    void *owner;
    int (*lease)(void *); /* stable GX/fence/OOB/SMMU/buffer/SQE-digest identity */
    int (*publish)(void *); /* exact ring+RPTR initialized and SQE payload visible */
    void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_cp_ring_config_ready(struct ceph_cp_ring_config *s)
{
    if(!s || !s->gpu || ((uintptr_t)s->gpu&3u) || s->bytes!=0x40000 || !s->lease ||
        !s->publish || !s->finish || s->lease(s->owner) || ceph_gmu_mmio_ready(s->gmu) || ceph_gmu_mmio_fault(s->gmu))return -1;
    if(ceph_gmu_mmio_gx_access(s->gmu))return -1;
    uint32_t halt=s->gpu[0x808],fault=s->gpu[0x823];ceph_gmu_mmio_barrier();
    return halt==UINT32_MAX || !(halt&2) || fault?-1:0;
}
static inline int ceph_cp_ring_configure_halted(struct ceph_cp_ring_config *s)
{
    struct ceph_cp_ring_plan plan;
    if(!s || s->terminal || s->writes || ceph_cp_ring_plan(&s->input,&plan) || ceph_cp_ring_config_ready(s))return -1;
    s->terminal=1;int e=0;
    e=ceph_cp_protection_check(s->gmu,s->gpu,s->bytes,s->owner,s->lease);if(e)goto done;
    /* Cold context only. Do not erase an inherited ring/SQE/RPTR or pending
     * pointer values. A halted foreign context is still somebody else's. */
    static const uint32_t empty[]={0x800,0x801,0x804,0x805,0x806,0x807,0x830,0x831};
    for(unsigned i=0;i<8;++i) {
        if(ceph_cp_ring_config_ready(s) || s->gpu[empty[i]]){e=-1;goto done;}
    }
    e=s->publish(s->owner);if(e)goto done;
    for(unsigned i=0;i<7;++i) {
        if(ceph_cp_ring_config_ready(s)){e=-1;goto done;}
        const struct ceph_cp_ring_write *w=&plan.writes[i];
        ++s->writes;ceph_gmu_mmio_barrier();s->gpu[w->word]=w->value;ceph_gmu_mmio_barrier();
        if(ceph_cp_ring_config_ready(s) || s->gpu[w->word]!=w->value){e=-1;goto done;}
    }
    s->configured=1;
done:
    s->error=e;s->finish(s->owner,1); /* even success keeps halt and mappings */
    return e;
}
#endif
