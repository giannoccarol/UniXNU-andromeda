/* Retire ONE owned GMU context only after producer admission is closed and
 * actual GMU/CP/bus activity has been drained or reset by a platform backend.
 * Parking must be retained through finish. This does NOT stop GPU hardware by
 * itself. Stream FAULT is not proof of DMA completion; PARK comes first.
 * No allocator token is issued and no published buffer freed by this module.
 */
#ifndef CEPHEUS_SMMU_DETACH_H
#define CEPHEUS_SMMU_DETACH_H
#include "cepheus_smmu_install.h"
enum ceph_detach_state {CE_DET_NEW,CE_DET_PARKED,CE_DET_STREAM_FAULT,CE_DET_CB_OFF,
    CE_DET_SYNCED,CE_DET_DETACHED,CE_DET_FAILED};
struct ceph_smmu_detach {enum ceph_detach_state state;unsigned effects;int error;};
struct ceph_smmu_detach_ops {
    void *ctx;
    int (*park)(void *,const struct ceph_smmu_plan *); /* close admission + bounded hardware stop/drain */
    int (*parked)(void *); /* verify lease/park remains held */
    int (*read)(void *,enum ceph_smmu_reg,uint64_t *);
    int (*write)(void *,enum ceph_smmu_reg,uint64_t);
    int (*barrier)(void *);
    int (*delay_us)(void *,unsigned);
    void (*finish)(void *,int detached); /* failure retains all mappings/backing/producer stop */
};
static inline int ceph_detach_read(struct ceph_smmu_detach_ops *b,enum ceph_smmu_reg r,uint64_t *v)
{
    if(b->parked(b->ctx) || b->read(b->ctx,r,v))return -1;
    return (r==CE_SM_TTBR0 || r==CE_SM_TTBR1)?(*v==UINT64_MAX?-1:0):(*v>=UINT32_MAX?-1:0);
}
static inline int ceph_detach_write(struct ceph_smmu_detach *s,struct ceph_smmu_detach_ops *b,
    enum ceph_smmu_reg r,uint64_t value,int readback)
{
    if(b->parked(b->ctx))return -1;
    ++s->effects;
    if(b->write(b->ctx,r,value) || b->barrier(b->ctx))return -1;
    uint64_t v;
    return readback && (ceph_detach_read(b,r,&v) || v!=value)?-1:0;
}
static inline int ceph_smmu_detach_context(struct ceph_smmu_detach *s,struct ceph_smmu_detach_ops *b,
    const struct ceph_smmu_plan *p)
{
    if(!s || s->state!=CE_DET_NEW || s->effects || !ceph_smmu_plan_canonical(p) || !b ||
        !b->park || !b->parked || !b->read || !b->write || !b->barrier || !b->delay_us || !b->finish)return -1;
    int e=b->park(b->ctx,p);
    if(e){s->state=CE_DET_FAILED;s->error=e;return e;}
    s->state=CE_DET_PARKED;
    /* Do not detach a changed/foreign context. Compare full immutable plan. */
    const enum ceph_smmu_reg regs[]={CE_SM_SMR,CE_SM_S2CR,CE_SM_SCTLR,CE_SM_TTBR0,CE_SM_TTBR1,
        CE_SM_TCR,CE_SM_TCR2,CE_SM_MAIR0,CE_SM_MAIR1,CE_SM_CBAR,CE_SM_CBA2R,CE_SM_ACTLR};
    const uint64_t values[]={p->smr,p->s2cr_translate,p->sctlr,p->ttbr0,p->ttbr1,
        p->tcr,p->tcr2,p->mair0,p->mair1,p->cbar,p->cba2r,0x303};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);++i) {
        uint64_t v;if(ceph_detach_read(b,regs[i],&v) || v!=values[i]){e=-1;goto failed;}
    }
    e=ceph_detach_write(s,b,CE_SM_S2CR,p->s2cr_fault,1);if(e)goto failed;
    s->state=CE_DET_STREAM_FAULT;
    e=ceph_detach_write(s,b,CE_SM_SCTLR,p->sctlr&~1u,1);if(e)goto failed;
    s->state=CE_DET_CB_OFF;
    e=ceph_detach_write(s,b,CE_SM_TLBIASID,p->asid,0);if(e)goto failed;
    e=ceph_detach_write(s,b,CE_SM_TLBSYNC,0,0);if(e)goto failed;
    unsigned i;
    for(i=0;i<1000;++i) {
        uint64_t v;if(ceph_detach_read(b,CE_SM_TLBSTATUS,&v)){e=-1;goto failed;}
        if(!(v&1))break;
        e=b->delay_us(b->ctx,1);if(e)goto failed;
    }
    if(i==1000){e=-2;goto failed;}
    s->state=CE_DET_SYNCED;
    e=ceph_detach_write(s,b,CE_SM_SMR,p->smr&~(1u<<31),1);if(e)goto failed;
    /* Final checks exclude late restore or access by another context owner. */
    uint64_t stream,enabled,fsr;
    if(ceph_detach_read(b,CE_SM_S2CR,&stream) || stream!=p->s2cr_fault ||
        ceph_detach_read(b,CE_SM_SCTLR,&enabled) || enabled!=(p->sctlr&~1u) ||
        ceph_detach_read(b,CE_SM_FSR,&fsr) || fsr){e=-1;goto failed;}
    s->state=CE_DET_DETACHED;s->error=0;b->finish(b->ctx,1);return 0;
failed:
    s->state=CE_DET_FAILED;s->error=e;b->finish(b->ctx,0);return e;
}
#endif
