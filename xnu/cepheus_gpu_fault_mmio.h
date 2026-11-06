/* Read-only fault transport for already owned windows. Domain acquisition is
 * provided by the real platform, not inferred from a non-NULL pointer. Failed
 * or unreadable domains never become clean, and no W1C/reset path exists.
 */
#ifndef CEPHEUS_GPU_FAULT_MMIO_H
#define CEPHEUS_GPU_FAULT_MMIO_H
#include "cepheus_gpu_fault.h"
#include "cepheus_gmu_mmio.h"
#include "cepheus_smmu_mmio.h"
struct ceph_gpu_fault_mmio {
    struct ceph_gmu_mmio *gmu;
    struct ceph_smmu_mmio *smmu;
    volatile uint32_t *gpu;size_t gpu_bytes;
    void *owner;
    int (*acquire)(void *,unsigned *domains);
    int (*lease)(void *,unsigned domains);
    int (*now_ns)(void *,uint64_t *);
    void (*release)(void *);
    unsigned domains,active;
};
static inline int ceph_gpu_fault_mmio_validate(struct ceph_gpu_fault_mmio *m)
{
    if(!m || !m->active || !m->domains || (m->domains&~7u) || !m->lease ||
        m->lease(m->owner,m->domains))return -1;
    if(m->domains&(CE_FAULT_DOMAIN_GMU|CE_FAULT_DOMAIN_GX)) {
        if(ceph_gmu_mmio_ready(m->gmu))return -1;
        if(m->domains&CE_FAULT_DOMAIN_GX) {
            if(!m->gpu || ((uintptr_t)m->gpu&3u) || m->gpu_bytes!=0x40000 ||
                ceph_gmu_mmio_gx_access(m->gmu))return -1;
        }
    }
    if(m->domains&CE_FAULT_DOMAIN_SMMU) {
        uint32_t off;unsigned width;
        if(ceph_smmu_mmio_offset(m->smmu,CE_SM_FSR,&off,&width) ||
            m->smmu->lease(m->smmu->owner))return -1;
    }
    return 0;
}
static inline int ceph_gpu_fault_mmio_begin(void *ctx,unsigned *domains)
{
    struct ceph_gpu_fault_mmio *m=ctx;
    if(!domains || !m || m->active || !m->acquire || !m->lease || !m->now_ns || !m->release)return -1;
    *domains=0;unsigned d=0;
    if(m->acquire(m->owner,&d))return -1;
    m->domains=d;m->active=1;
    if(ceph_gpu_fault_mmio_validate(m)){m->active=0;m->domains=0;m->release(m->owner);return -1;}
    *domains=d;return 0;
}
static inline int ceph_gpu_fault_mmio_read(void *ctx,enum ceph_fault_reg r,uint64_t *v)
{
    struct ceph_gpu_fault_mmio *m=ctx;
    if(!v || (unsigned)r>=CE_FAULT_REG_COUNT || ceph_gpu_fault_mmio_validate(m))return -1;
    unsigned domain=r<=CE_FAULT_CP_PROTECT?CE_FAULT_DOMAIN_GX:r<=CE_FAULT_GMU_HFI?CE_FAULT_DOMAIN_GMU:CE_FAULT_DOMAIN_SMMU;
    if(!(m->domains&domain))return -1;
    if(domain==CE_FAULT_DOMAIN_GX) {
        static const uint32_t words[]={0x201,0x823,0x821,0x824};
        *v=m->gpu[words[r]];ceph_gmu_mmio_barrier();return 0;
    }
    if(domain==CE_FAULT_DOMAIN_GMU) {
        uint32_t value;int e=ceph_gmu_mmio_load(m->gmu,r==CE_FAULT_GMU_AO?0x23b05:0x1f992,&value);
        if(e)return e;
        *v=value;return 0;
    }
    if(r==CE_FAULT_CB_FSR)return ceph_smmu_mmio_read(m->smmu,CE_SM_FSR,v);
    uint32_t off;unsigned width;
    if(ceph_smmu_mmio_offset(m->smmu,CE_SM_FSR,&off,&width))return -1;
    off+=r==CE_FAULT_CB_FAR?8:16;width=r==CE_FAULT_CB_FAR?8:4;
    if(off>m->smmu->mapped_bytes-width || (off&(width-1)) || m->smmu->lease(m->smmu->owner))return -1;
    ceph_smmu_mmio_order();
    *v=width==8?*(volatile uint64_t *)(m->smmu->base+off):*(volatile uint32_t *)(m->smmu->base+off);
    ceph_smmu_mmio_order();return 0;
}
static inline int ceph_gpu_fault_mmio_now(void *ctx,uint64_t *n)
{struct ceph_gpu_fault_mmio *m=ctx;return !n || ceph_gpu_fault_mmio_validate(m)?-1:m->now_ns(m->owner,n);}
static inline void ceph_gpu_fault_mmio_end(void *ctx)
{struct ceph_gpu_fault_mmio *m=ctx;if(!m || !m->active)return;m->active=0;m->domains=0;m->release(m->owner);}
static inline int ceph_gpu_fault_mmio_snapshot(struct ceph_gpu_fault_mmio *m,struct ceph_gpu_fault_record *out)
{
    struct ceph_gpu_fault_ops b={m,ceph_gpu_fault_mmio_begin,ceph_gpu_fault_mmio_read,ceph_gpu_fault_mmio_now,ceph_gpu_fault_mmio_end};
    return ceph_gpu_fault_snapshot(&b,out);
}
static inline int ceph_gpu_fault_mmio_gmu_health(struct ceph_gpu_fault_mmio *m)
{
    struct ceph_gpu_fault_record r;
    return ceph_gpu_fault_mmio_snapshot(m,&r)?-1:ceph_gpu_fault_domains_status(&r,CE_FAULT_DOMAIN_GMU|CE_FAULT_DOMAIN_SMMU);
}
#endif
