/* A640 CM3 register startup AFTER power, SMMU, PDC/RSC and image publication.
 * Explicitly cold-start only. Named registers are GPU-relative WORD offsets;
 * ceph_gmu_register_offset() converts them to the mapped GMU byte aperture.
 * begin verifies/pins actual prerequisites including TCM retention, never just
 * caller 'ready' booleans. No default backend, guessed PDC scripts or reset retry.
 */
#ifndef CEPHEUS_GMU_START_H
#define CEPHEUS_GMU_START_H
#include <stddef.h>
#include <stdint.h>
enum ceph_gmu_reg {
    CE_GMU_SYSRESET,CE_GMU_INIT_RESULT,CE_GMU_BOOT_CONFIG,CE_GMU_CM3_CFG,
    CE_GMU_QTBL_ADDR,CE_GMU_QTBL_INFO,CE_GMU_FENCE_RANGE,CE_GMU_SFR_ADDR,
    CE_GMU_LOG_INFO,CE_GMU_SYS_BUS,CE_GMU_ICACHE_CFG,CE_GMU_DCACHE_CFG,
    CE_GMU_INTER_FRAME_CTRL,CE_GMU_INTER_FRAME_HYST,CE_GMU_SPTPRAC_HYST,
    CE_GMU_RPMH_CTRL,CE_GMU_WFI_CONFIG,CE_GMU_HFI_INIT,CE_GMU_HFI_STATUS,
    CE_GMU_HFI_VERSION,CE_GMU_AO_STATUS,CE_GMU_HFI_IRQ_STATUS,CE_GMU_TCM_RETENTION,CE_GMU_REG_COUNT
};
static inline int ceph_gmu_register_offset(enum ceph_gmu_reg r,uint32_t *offset)
{
    static const uint32_t words[]={0x1f800,0x1f81c,0x1f801,0x1f82d,0x1f985,
        0x1f984,0x23b11,0x1f982,0x1f900,0x1f40f,0x1f400,0x1f401,
        0x1f8c0,0x1f8c1,0x1f8c2,0x1f8e8,0x1f802,0x1f986,0x1f980,
        0x1f981,0x23b05,0x1f992,0x1f9cc};
    if(!offset || (unsigned)r>=CE_GMU_REG_COUNT)return -1;
    *offset=words[r]*4u-0x6a000u;
    return *offset<=0x30000u-4u?0:-1;
}
struct ceph_gmu_start_config {uint32_t chipid,hfi_va,hfi_bytes,log_va,log_bytes;};
struct ceph_gmu_start_plan {uint32_t chip,log,fence;};
static inline int ceph_gmu_start_plan(const struct ceph_gmu_start_config *c,struct ceph_gmu_start_plan *p)
{
    if(!c || !p || c->chipid!=0x06040001 || c->hfi_bytes!=5*4096 || c->log_bytes!=4096 ||
        ((c->hfi_va|c->log_va)&4095u) || c->hfi_va<0x60000000 || c->log_va<0x60000000 ||
        c->hfi_va>0x80000000u-c->hfi_bytes || c->log_va>0x80000000u-c->log_bytes ||
        (c->hfi_va<c->log_va+c->log_bytes && c->log_va<c->hfi_va+c->hfi_bytes))return -1;
    *p=(struct ceph_gmu_start_plan){(c->chipid&0xffff0000u)|((c->chipid&0xffu)<<8),
        c->log_va,0x80a008a0u};
    return 0;
}
enum ceph_gmu_start_state {CE_GS_NEW,CE_GS_OWNED,CE_GS_CONFIGURED,CE_GS_RELEASED,
    CE_GS_CM3_READY,CE_GS_HFI_READY,CE_GS_REFUSED,CE_GS_QUARANTINED};
struct ceph_gmu_start {enum ceph_gmu_start_state state;unsigned effects;int error;uint32_t hfi_version;};
struct ceph_gmu_start_ops {
    void *ctx;
    int (*begin)(void *,const struct ceph_gmu_start_config *); /* owns validated prerequisites */
    int (*validate)(void *); /* ownership/power/mappings/thermal; not reset after release */
    int (*read)(void *,enum ceph_gmu_reg,uint32_t *);
    int (*write)(void *,enum ceph_gmu_reg,uint32_t);
    int (*barrier)(void *);
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *); /* yields <=100us; total bounded by real time AND count */
    void (*finish)(void *,int keep);
};
static inline int ceph_gmu_start_read(struct ceph_gmu_start_ops *b,enum ceph_gmu_reg r,uint32_t *v)
{
    if(b->validate(b->ctx) || b->read(b->ctx,r,v) || *v==UINT32_MAX)return -1;
    return 0;
}
static inline int ceph_gmu_start_fault(struct ceph_gmu_start_ops *b)
{
    uint32_t ao,hfi;
    return ceph_gmu_start_read(b,CE_GMU_AO_STATUS,&ao) || ceph_gmu_start_read(b,CE_GMU_HFI_IRQ_STATUS,&hfi) ||
        (ao&0x29u) || (hfi&(1u<<15));
}
static inline int ceph_gmu_start_write(struct ceph_gmu_start *s,struct ceph_gmu_start_ops *b,
    enum ceph_gmu_reg r,uint32_t v,int verify)
{
    if(b->validate(b->ctx))return -1;
    ++s->effects;if(b->write(b->ctx,r,v) || b->barrier(b->ctx))return -1;
    uint32_t actual;
    if(verify && (ceph_gmu_start_read(b,r,&actual) || actual!=v))return -1;
    return 0;
}
static inline int ceph_gmu_start_wait(struct ceph_gmu_start_ops *b,enum ceph_gmu_reg r,uint32_t mask,uint32_t wanted)
{
    uint64_t start,last,now;
    if(b->now_ns(b->ctx,&start))return -1;
    last=start;
    for(unsigned i=0;i<10000;++i) {
        if(b->now_ns(b->ctx,&now) || ceph_gmu_start_fault(b))return -1;
        if(now<last || now-start>=100000000ULL)return -2;
        last=now;
        uint32_t v;if(ceph_gmu_start_read(b,r,&v))return -1;
        if((v&mask)==wanted)return 0;
        if(b->pause(b->ctx))return -1;
    }
    return -2;
}
static inline int ceph_gmu_start_cold(struct ceph_gmu_start *s,struct ceph_gmu_start_ops *b,
    const struct ceph_gmu_start_config *c)
{
    struct ceph_gmu_start_plan p;
    if(!s || s->state!=CE_GS_NEW || s->effects || ceph_gmu_start_plan(c,&p) || !b ||
        !b->begin || !b->validate || !b->read || !b->write || !b->barrier ||
        !b->now_ns || !b->pause || !b->finish)return -1;
    int e=b->begin(b->ctx,c);if(e){s->error=e;s->state=CE_GS_REFUSED;return e;}
    s->state=CE_GS_OWNED;uint32_t reset,hfi;
    if(ceph_gmu_start_read(b,CE_GMU_SYSRESET,&reset) || reset!=1 ||
        ceph_gmu_start_read(b,CE_GMU_HFI_STATUS,&hfi) || hfi || ceph_gmu_start_fault(b)) {e=-1;goto failed;}
    {
    const enum ceph_gmu_reg regs[]={CE_GMU_INIT_RESULT,CE_GMU_BOOT_CONFIG,CE_GMU_QTBL_ADDR,
        CE_GMU_QTBL_INFO,CE_GMU_FENCE_RANGE,CE_GMU_CM3_CFG,CE_GMU_SFR_ADDR,CE_GMU_LOG_INFO,
        CE_GMU_SYS_BUS,CE_GMU_ICACHE_CFG,CE_GMU_DCACHE_CFG,
        CE_GMU_INTER_FRAME_HYST,CE_GMU_SPTPRAC_HYST,CE_GMU_INTER_FRAME_CTRL};
    /* A640 with IFPC includes SPTP: explicitly retain low-power policy instead
     * of disabling sleep to make initialization appear to work. */
    const uint32_t values[]={0,2,c->hfi_va,1,p.fence,0x4052,p.chip,p.log,1,1,1,
        0x000a1680,0x000a1680,0x09c40407};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);++i) {
        e=ceph_gmu_start_write(s,b,regs[i],values[i],1);if(e)goto failed;
    }
    }
    uint32_t rpmh;
    if(ceph_gmu_start_read(b,CE_GMU_RPMH_CTRL,&rpmh)) {e=-1;goto failed;}
    e=ceph_gmu_start_write(s,b,CE_GMU_RPMH_CTRL,rpmh|0xf11u,1);if(e)goto failed;
    e=ceph_gmu_start_write(s,b,CE_GMU_WFI_CONFIG,0,1);if(e)goto failed;
    s->state=CE_GS_CONFIGURED;
    /* Last fresh-reset/result check before any CM3 execution. */
    uint32_t result;
    if(ceph_gmu_start_read(b,CE_GMU_SYSRESET,&reset) || reset!=1 ||
        ceph_gmu_start_read(b,CE_GMU_INIT_RESULT,&result) || result || ceph_gmu_start_fault(b)) {e=-1;goto failed;}
    s->state=CE_GS_RELEASED;
    e=ceph_gmu_start_write(s,b,CE_GMU_SYSRESET,0,1);if(e)goto failed;
    e=ceph_gmu_start_wait(b,CE_GMU_INIT_RESULT,UINT32_MAX,0xbabeface);if(e)goto failed;
    s->state=CE_GS_CM3_READY;
    e=ceph_gmu_start_write(s,b,CE_GMU_HFI_INIT,1,0);if(e)goto failed;
    e=ceph_gmu_start_wait(b,CE_GMU_HFI_STATUS,1,1);if(e)goto failed;
    uint32_t version;
    if(ceph_gmu_start_read(b,CE_GMU_HFI_VERSION,&version) || version>>28!=2) {e=-3;goto failed;}
    s->hfi_version=version;s->state=CE_GS_HFI_READY;s->error=0;b->finish(b->ctx,1);return 0;
failed:
    s->error=e;s->state=s->effects?CE_GS_QUARANTINED:CE_GS_REFUSED;
    b->finish(b->ctx,s->effects!=0);return e;
}
#endif
