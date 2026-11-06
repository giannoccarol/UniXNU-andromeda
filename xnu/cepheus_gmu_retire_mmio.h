/* Concrete slumber/park register adapters. GMU registers are in the existing
 * GMU aperture; GBIF is in the distinct 0x40000 GPU aperture (CX accessible).
 * Do not accidentally address GBIF through GMU-relative arithmetic. No maps,
 * leases, admission closure, HFI ACK or parked proof are fabricated here.
 */
#ifndef CEPHEUS_GMU_RETIRE_MMIO_H
#define CEPHEUS_GMU_RETIRE_MMIO_H
#include "cepheus_gmu_mmio.h"
#include "cepheus_gmu_slumber.h"
#include "cepheus_gmu_park.h"
static inline int ceph_gmu_mmio_slumber_read(void *ctx,enum ceph_slumber_reg r,uint32_t *v)
{
    if(!v)return -1;
    switch(r) {
    case CE_SL_POWER_COUNTER_ENABLE:return ceph_gmu_mmio_load(ctx,0x1f840,v);
    case CE_SL_RPMH_POWER_STATE:return ceph_gmu_mmio_load(ctx,0x1f8ec,v);
    case CE_SL_GX_STATUS:return ceph_gmu_mmio_load(ctx,0x1f8d0,v);
    case CE_SL_GMU_FAULT: {
        uint32_t ao,hfi;
        if(ceph_gmu_mmio_load(ctx,0x23b05,&ao) || ceph_gmu_mmio_load(ctx,0x1f992,&hfi) ||
            ao==UINT32_MAX || hfi==UINT32_MAX)return -1;
        *v=(ao&0x29u)|(hfi&(1u<<15));return 0;
    }
    default:return -1;
    }
}
static inline int ceph_gmu_mmio_slumber_write(void *ctx,enum ceph_slumber_reg r,uint32_t v)
{return r==CE_SL_POWER_COUNTER_ENABLE && !v?ceph_gmu_mmio_store(ctx,0x1f840,0):-1;}
struct ceph_gmu_park_mmio {
    struct ceph_gmu_mmio *gmu;
    volatile uint32_t *gpu;
    size_t gpu_bytes;
    void *owner;
    int (*lease)(void *); /* admission remains closed, no pending jobs, both apertures held */
};
static inline int ceph_gmu_mmio_park_ready(struct ceph_gmu_park_mmio *m)
{
    if(!m || !m->gpu || ((uintptr_t)m->gpu&3u) || m->gpu_bytes!=0x40000 ||
        !m->lease || m->lease(m->owner) || ceph_gmu_mmio_ready(m->gmu))return -1;
    /* Hardware slumber is necessary, not proof of protocol completion. The
     * retire orchestrator admits park only after its own HFI/slumber engine. */
    uint32_t state;
    return ceph_gmu_mmio_load(m->gmu,0x1f8ec,&state) || state!=15?-1:0;
}
static inline int ceph_gmu_mmio_park_read(void *ctx,enum ceph_gmu_park_reg r,uint32_t *v)
{
    struct ceph_gmu_park_mmio *m=ctx;
    if(!v || ceph_gmu_mmio_park_ready(m))return -1;
    static const uint32_t tcs[]={0x23746,0x237ee,0x23896,0x2393e};
    if(r>=CE_PARK_TCS0 && r<=CE_PARK_TCS3)return ceph_gmu_mmio_load(m->gmu,tcs[r-CE_PARK_TCS0],v);
    switch(r) {
    case CE_PARK_GX_STATUS:return ceph_gmu_mmio_load(m->gmu,0x1f8d0,v);
    case CE_PARK_SYSRESET:return ceph_gmu_mmio_load(m->gmu,0x1f800,v);
    case CE_PARK_GBIF_HALT:case CE_PARK_GBIF_ACK:
        *v=m->gpu[r==CE_PARK_GBIF_HALT?0x3c45:0x3c46];ceph_gmu_mmio_barrier();return 0;
    default:return -1;
    }
}
static inline int ceph_gmu_mmio_park_write(void *ctx,enum ceph_gmu_park_reg r,uint32_t v)
{
    struct ceph_gmu_park_mmio *m=ctx;uint32_t gx;
    if(ceph_gmu_mmio_park_read(m,CE_PARK_GX_STATUS,&gx) || gx==UINT32_MAX || (gx&0xc0)!=0xc0)return -1;
    if(r==CE_PARK_SYSRESET && v==1) {
        for(unsigned i=0;i<4;++i) {
            uint32_t tcs;if(ceph_gmu_mmio_park_read(m,(enum ceph_gmu_park_reg)(CE_PARK_TCS0+i),&tcs) ||
                tcs==UINT32_MAX || !(tcs&1))return -1;
        }
        return ceph_gmu_mmio_store(m->gmu,0x1f800,1);
    }
    if(r==CE_PARK_GBIF_HALT) {
        uint32_t reset,halt;
        if(ceph_gmu_mmio_park_read(m,CE_PARK_SYSRESET,&reset) || reset!=1 ||
            ceph_gmu_mmio_park_read(m,CE_PARK_GBIF_HALT,&halt) || halt==UINT32_MAX || v!=(halt|2u) ||
            ceph_gmu_mmio_park_ready(m))return -1;
        ceph_gmu_mmio_barrier();m->gpu[0x3c45]=v;ceph_gmu_mmio_barrier();return 0;
    }
    return -1; /* No GX reset, fault ACK, clock/vote-off or halt clearing. */
}
#endif
