/* Read-only fault record, no acknowledgement/clear/reset. Backend acquires
 * current power/GMU fence/SMMU ownership for this snapshot; absent GX access is
 * reported as absent, never as clean. Addresses are selected register IDs only.
 */
#ifndef CEPHEUS_GPU_FAULT_H
#define CEPHEUS_GPU_FAULT_H
#include <stdint.h>
enum ceph_fault_reg {CE_FAULT_RBBM_IRQ,CE_FAULT_CP_IRQ,CE_FAULT_CP_HW,CE_FAULT_CP_PROTECT,
    CE_FAULT_GMU_AO,CE_FAULT_GMU_HFI,CE_FAULT_CB_FSR,CE_FAULT_CB_FSYNR0,CE_FAULT_CB_FAR,CE_FAULT_REG_COUNT};
enum {CE_FAULT_DOMAIN_GX=1,CE_FAULT_DOMAIN_GMU=2,CE_FAULT_DOMAIN_SMMU=4};
struct ceph_gpu_fault_record {
    uint64_t time_ns,far;
    uint32_t rbbm,cp,cp_hw,cp_protect,gmu_ao,gmu_hfi,fsr,fsynr0;
    unsigned domains;int coherent;
};
struct ceph_gpu_fault_ops {
    void *ctx;
    int (*begin)(void *,unsigned *domains);
    int (*read)(void *,enum ceph_fault_reg,uint64_t *);
    int (*now)(void *,uint64_t *);
    void (*end)(void *);
};
static inline int ceph_fault_reg32(struct ceph_gpu_fault_ops *b,enum ceph_fault_reg r,uint32_t *out)
{
    uint64_t v;if(b->read(b->ctx,r,&v) || v>=UINT32_MAX)return -1;
    *out=(uint32_t)v;return 0;
}
static inline int ceph_gpu_fault_snapshot(struct ceph_gpu_fault_ops *b,struct ceph_gpu_fault_record *out)
{
    if(!out)return -1;
    *out=(struct ceph_gpu_fault_record){0};
    if(!b || !b->begin || !b->read || !b->now || !b->end)return -1;
    struct ceph_gpu_fault_record r={0};
    if(b->begin(b->ctx,&r.domains))return -1;
    int e=-1;
    if(!r.domains || (r.domains&~7u) || b->now(b->ctx,&r.time_ns))goto done;
    uint32_t before=0,after=0;
    if(r.domains&CE_FAULT_DOMAIN_SMMU) {
        if(ceph_fault_reg32(b,CE_FAULT_CB_FSR,&before) || ceph_fault_reg32(b,CE_FAULT_CB_FSYNR0,&r.fsynr0) ||
            b->read(b->ctx,CE_FAULT_CB_FAR,&r.far) || r.far==UINT64_MAX ||
            ceph_fault_reg32(b,CE_FAULT_CB_FSR,&after))goto done;
        r.fsr=after;
        /* A multi-fault/stalled record needs care even if status is unchanged;
         * never pretend FAR is atomic relative to new hardware faults. */
        r.coherent=before==after && !(after&(1u<<31));
    }
    if(r.domains&CE_FAULT_DOMAIN_GMU) {
        if(ceph_fault_reg32(b,CE_FAULT_GMU_AO,&r.gmu_ao) || ceph_fault_reg32(b,CE_FAULT_GMU_HFI,&r.gmu_hfi))goto done;
    }
    if(r.domains&CE_FAULT_DOMAIN_GX) {
        if(ceph_fault_reg32(b,CE_FAULT_RBBM_IRQ,&r.rbbm) || ceph_fault_reg32(b,CE_FAULT_CP_IRQ,&r.cp))goto done;
        if((r.cp&4u) && ceph_fault_reg32(b,CE_FAULT_CP_HW,&r.cp_hw))goto done;
        if((r.cp&16u) && ceph_fault_reg32(b,CE_FAULT_CP_PROTECT,&r.cp_protect))goto done;
    }
    *out=r;e=0;
done:
    b->end(b->ctx);return e;
}
/* clean=0, detected=1, incomplete=-1. Never treat unreadable domains as clean. */
static inline int ceph_gpu_fault_domains_status(const struct ceph_gpu_fault_record *r,unsigned required)
{
    if(!r || !required || (required&~7u) || (r->domains&~7u))return -1;
    const uint32_t rbbm_errors=(1u<<1)|(1u<<6)|(1u<<7)|(1u<<9)|(1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28);
    if(((r->domains&CE_FAULT_DOMAIN_GX) && ((r->rbbm&rbbm_errors) || r->cp)) ||
        ((r->domains&CE_FAULT_DOMAIN_GMU) && ((r->gmu_ao&0x29u)||(r->gmu_hfi&(1u<<15)))) ||
        ((r->domains&CE_FAULT_DOMAIN_SMMU) && (r->fsr&0xc00001feu)))return 1;
    return (r->domains&required)==required && (!(required&CE_FAULT_DOMAIN_SMMU) || r->coherent)?0:-1;
}
/* Full GPU health still requires ALL domains. GMU startup can explicitly ask
 * for GMU+SMMU (6) without misrepresenting powered-down GX as observed clean. */
static inline int ceph_gpu_fault_status(const struct ceph_gpu_fault_record *r)
{return ceph_gpu_fault_domains_status(r,7);}
#endif
