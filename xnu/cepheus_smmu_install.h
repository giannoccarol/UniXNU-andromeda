/* Install one reserved GMU context. No global reset/TLBIALL/foreign-CB writes.
 * begin acquires ownership of exact SMR/CB/ASID/root allocation, verifies TBU,
 * firmware skip-init/secure reservations and GMU reset, and pins it to finish.
 * Every callback is bounded. Failure after publication retains ALL allocations.
 * Backend exposes only the selected slot/CB via named registers; it cannot be
 * a userspace arbitrary-MMIO service. No hardware backend is enabled by this file.
 */
#ifndef CEPHEUS_SMMU_INSTALL_H
#define CEPHEUS_SMMU_INSTALL_H
#include "cepheus_smmu_plan.h"
enum ceph_smmu_reg { CE_SM_SMR,CE_SM_S2CR,CE_SM_CBAR,CE_SM_CBA2R,CE_SM_TCR2,CE_SM_TCR,
    CE_SM_TTBR0,CE_SM_TTBR1,CE_SM_MAIR0,CE_SM_MAIR1,CE_SM_ACTLR,CE_SM_SCTLR,
    CE_SM_TLBIASID,CE_SM_TLBSYNC,CE_SM_TLBSTATUS,CE_SM_FSR,CE_SM_REG_COUNT };
enum ceph_smmu_state { CE_SM_NEW,CE_SM_OWNED,CE_SM_PUBLISHED,CE_SM_CONTEXT,
    CE_SM_ENABLED,CE_SM_REFUSED,CE_SM_QUARANTINED };
struct ceph_smmu_install { enum ceph_smmu_state state;unsigned effects;int error; };
struct ceph_smmu_ops {
    void *ctx;
    int (*begin)(void *,const struct ceph_smmu_plan *,uint32_t actlr);
    int (*validate)(void *); /* current lease, powered path, GMU reset */
    int (*read)(void *,enum ceph_smmu_reg,uint64_t *);
    int (*write)(void *,enum ceph_smmu_reg,uint64_t);
    int (*publish_tables)(void *); /* clean+barrier of ALL table pages */
    int (*barrier)(void *); /* completes posted MMIO before dependent phase */
    int (*delay_us)(void *,unsigned);
    void (*finish)(void *,int keep);
};
static inline int ceph_smmu_plan_canonical(const struct ceph_smmu_plan *p)
{
    if(!p || p->slot>=128 || p->cb>=128 || !p->asid || p->asid>255)return 0;
    uint32_t sid=p->smr&0x7fffu;
    uint64_t root=p->ttbr0&0xffffffffffULL;
    return (sid==4 || sid==5) && p->smr==(0x84000000u|sid) &&
        p->s2cr_fault==(2u<<16) && p->s2cr_translate==p->cb &&
        p->cbar==0x1f300 && p->cba2r==1 && p->tcr==0x802020 && p->tcr2==0x38002 &&
        p->mair0==0x44ff && !p->mair1 && p->sctlr==0xc01127 &&
        root && !(root&4095u) && p->ttbr0==(root|((uint64_t)p->asid<<48)) &&
        p->ttbr1==((uint64_t)p->asid<<48);
}
static inline int ceph_smmu_read(struct ceph_smmu_ops *b,enum ceph_smmu_reg r,uint64_t *v)
{
    if(b->validate(b->ctx) || b->read(b->ctx,r,v))return -1;
    if(r==CE_SM_TTBR0 || r==CE_SM_TTBR1) return *v==UINT64_MAX?-1:0;
    return *v>=UINT32_MAX?-1:0;
}
static inline int ceph_smmu_write_verify(struct ceph_smmu_install *s,struct ceph_smmu_ops *b,
    enum ceph_smmu_reg r,uint64_t v)
{
    if(b->validate(b->ctx))return -1;
    ++s->effects;
    if(b->write(b->ctx,r,v) || b->barrier(b->ctx))return -1;
    uint64_t actual;if(ceph_smmu_read(b,r,&actual) || actual!=v)return -1;
    return 0;
}
static inline int ceph_smmu_install_context(struct ceph_smmu_install *s,struct ceph_smmu_ops *b,
    const struct ceph_smmu_plan *p,uint32_t actlr)
{
    if(!s || s->state!=CE_SM_NEW || s->effects || !ceph_smmu_plan_canonical(p) ||
        actlr!=0x303 || !b || !b->begin || !b->validate || !b->read || !b->write ||
        !b->publish_tables || !b->barrier || !b->delay_us || !b->finish)return -1;
    int e=b->begin(b->ctx,p,actlr);
    if(e) {s->state=CE_SM_REFUSED;s->error=e;return e;}
    s->state=CE_SM_OWNED;
    uint64_t smr,s2cr,sctlr,fsr;
    if(ceph_smmu_read(b,CE_SM_SMR,&smr) || ceph_smmu_read(b,CE_SM_S2CR,&s2cr) ||
        ceph_smmu_read(b,CE_SM_SCTLR,&sctlr) || ceph_smmu_read(b,CE_SM_FSR,&fsr) ||
        (smr&(1u<<31)) || s2cr!=p->s2cr_fault || (sctlr&1u) || fsr) {e=-1;goto failed;}
    /* Keep stream faulting while making tables/context visible. */
    s->state=CE_SM_PUBLISHED;++s->effects;
    e=b->publish_tables(b->ctx);if(e)goto failed;
    const enum ceph_smmu_reg regs[]={CE_SM_CBA2R,CE_SM_CBAR,CE_SM_TCR2,CE_SM_TCR,
        CE_SM_TTBR0,CE_SM_TTBR1,CE_SM_MAIR0,CE_SM_MAIR1,CE_SM_ACTLR};
    const uint64_t values[]={p->cba2r,p->cbar,p->tcr2,p->tcr,p->ttbr0,p->ttbr1,p->mair0,p->mair1,actlr};
    s->state=CE_SM_CONTEXT;
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);++i) {
        e=ceph_smmu_write_verify(s,b,regs[i],values[i]);if(e)goto failed;
    }
    /* Only this context/ASID is invalidated. Trigger registers aren't readable
     * latches, so use barrier+TLBSTATUS instead of equality readback. */
    if(b->validate(b->ctx)) {e=-1;goto failed;}
    ++s->effects;e=b->write(b->ctx,CE_SM_TLBIASID,p->asid);if(e)goto failed;
    e=b->barrier(b->ctx);if(e)goto failed;
    if(b->validate(b->ctx)) {e=-1;goto failed;}
    ++s->effects;e=b->write(b->ctx,CE_SM_TLBSYNC,0);if(e)goto failed;
    e=b->barrier(b->ctx);if(e)goto failed;
    unsigned i;
    for(i=0;i<1000;++i) {
        uint64_t busy;e=ceph_smmu_read(b,CE_SM_TLBSTATUS,&busy);if(e)goto failed;
        if(!(busy&1u))break;
        e=b->delay_us(b->ctx,1);if(e)goto failed;
    }
    if(i==1000) {e=-2;goto failed;}
    /* Recheck the lease/stream before the irreversible enable transition. */
    if(ceph_smmu_read(b,CE_SM_SMR,&smr) || ceph_smmu_read(b,CE_SM_S2CR,&s2cr) ||
        ceph_smmu_read(b,CE_SM_FSR,&fsr) || (smr&(1u<<31)) || s2cr!=p->s2cr_fault || fsr) {
        e=-1;goto failed;
    }
    e=ceph_smmu_write_verify(s,b,CE_SM_SCTLR,p->sctlr);if(e)goto failed;
    e=ceph_smmu_write_verify(s,b,CE_SM_SMR,p->smr);if(e)goto failed;
    e=ceph_smmu_write_verify(s,b,CE_SM_S2CR,p->s2cr_translate);if(e)goto failed;
    if(ceph_smmu_read(b,CE_SM_FSR,&fsr) || fsr) {e=-1;goto failed;}
    s->state=CE_SM_ENABLED;s->error=0;b->finish(b->ctx,1);return 0;
failed:
    s->state=s->effects?CE_SM_QUARANTINED:CE_SM_REFUSED;s->error=e;
    b->finish(b->ctx,s->effects!=0);return e;
}
#endif
