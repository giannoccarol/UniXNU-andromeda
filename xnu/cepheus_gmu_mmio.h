/* Real volatile GMU aperture adapter; shared by CM3 boot/OOB/HFI. Does NOT map
 * memory or acquire ownership. Physical aperture, current firmware generation,
 * power/clock/SMMU/thermal and PDC leases must be proven by the platform before
 * constructing this object. No automatic startup hook, arbitrary offsets or GX
 * access. Whitelisted writes are the exact values used by the concrete engines.
 */
#ifndef CEPHEUS_GMU_MMIO_H
#define CEPHEUS_GMU_MMIO_H
#include "cepheus_gmu_start.h"
#include "cepheus_gmu_oob.h"
struct ceph_gmu_mmio {
    volatile uint8_t *base;
    size_t bytes;
    struct ceph_gmu_start_config config;
    uint32_t hfi_version;
    void *owner;
    int (*lease)(void *); /* stable resource generation; not just pointer validity */
};
static inline void ceph_gmu_mmio_barrier(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb osh" ::: "memory");
#elif !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error "GMU MMIO requires arm64"
#endif
}
static inline int ceph_gmu_mmio_ready(struct ceph_gmu_mmio *m)
{
    struct ceph_gmu_start_plan p;
    return !m || !m->base || ((uintptr_t)m->base&3u) || m->bytes!=0x30000 ||
        !m->lease || ceph_gmu_start_plan(&m->config,&p) || m->lease(m->owner)?-1:0;
}
/* Internal helper, not exported as a general kernel/user register service. */
static inline int ceph_gmu_mmio_load(struct ceph_gmu_mmio *m,uint32_t word,uint32_t *v)
{
    if(!v || word<0x1a800 || word>0x267ff || ceph_gmu_mmio_ready(m))return -1;
    uint32_t offset=word*4-0x6a000;
    if(offset>m->bytes-4)return -1;
    *v=*(volatile uint32_t *)(m->base+offset);ceph_gmu_mmio_barrier();return 0;
}
static inline int ceph_gmu_mmio_store(struct ceph_gmu_mmio *m,uint32_t word,uint32_t v)
{
    if(word<0x1a800 || word>0x267ff || ceph_gmu_mmio_ready(m))return -1;
    uint32_t offset=word*4-0x6a000;
    if(offset>m->bytes-4)return -1;
    ceph_gmu_mmio_barrier();*(volatile uint32_t *)(m->base+offset)=v;ceph_gmu_mmio_barrier();return 0;
}
static inline int ceph_gmu_mmio_start_read(void *ctx,enum ceph_gmu_reg reg,uint32_t *v)
{
    uint32_t offset;if(ceph_gmu_register_offset(reg,&offset))return -1;
    return ceph_gmu_mmio_load(ctx,(offset+0x6a000)/4,v);
}
static inline int ceph_gmu_mmio_start_write(void *ctx,enum ceph_gmu_reg reg,uint32_t v)
{
    struct ceph_gmu_mmio *m=ctx;struct ceph_gmu_start_plan p;uint32_t offset;
    if(!m || ceph_gmu_start_plan(&m->config,&p) || ceph_gmu_register_offset(reg,&offset))return -1;
    int allowed=0;
    switch(reg) {
    case CE_GMU_SYSRESET: allowed=!v;break; /* cold start only; park owns reset=1 */
    case CE_GMU_INIT_RESULT: case CE_GMU_WFI_CONFIG:allowed=!v;break;
    case CE_GMU_BOOT_CONFIG:allowed=v==2;break;
    case CE_GMU_CM3_CFG:allowed=v==0x4052;break;
    case CE_GMU_QTBL_ADDR:allowed=v==m->config.hfi_va;break;
    case CE_GMU_QTBL_INFO: case CE_GMU_SYS_BUS: case CE_GMU_ICACHE_CFG:
    case CE_GMU_DCACHE_CFG: case CE_GMU_HFI_INIT:allowed=v==1;break;
    case CE_GMU_TCM_RETENTION:allowed=v==1;break;
    case CE_GMU_FENCE_RANGE:allowed=v==p.fence;break;
    case CE_GMU_SFR_ADDR:allowed=v==p.chip;break;
    case CE_GMU_LOG_INFO:allowed=v==p.log;break;
    case CE_GMU_INTER_FRAME_HYST: case CE_GMU_SPTPRAC_HYST:allowed=v==0xa1680;break;
    case CE_GMU_INTER_FRAME_CTRL:allowed=v==0x09c40407;break;
    case CE_GMU_RPMH_CTRL: {
        uint32_t old;
        if(ceph_gmu_mmio_start_read(ctx,reg,&old) || old==UINT32_MAX)return -1;
        allowed=v==(old|0xf11u);break;
    }
    default:return -1; /* no status/IRQ clear through configuration interface */
    }
    return allowed?ceph_gmu_mmio_store(m,(offset+0x6a000)/4,v):-1;
}
static inline int ceph_gmu_mmio_oob_read(void *ctx,enum ceph_oob_reg r,uint32_t *v)
{
    if(r!=CE_OOB_G2H_INFO && r!=CE_OOB_GX_STATUS)return -1;
    return ceph_gmu_mmio_load(ctx,r==CE_OOB_G2H_INFO?0x1f992:0x1f8d0,v);
}
static inline int ceph_gmu_mmio_oob_write(void *ctx,enum ceph_oob_reg r,uint32_t v)
{
    struct ceph_gmu_mmio *m=ctx;struct ceph_oob_bits bits;
    if(!m || ceph_gmu_oob_gpu_bits(m->hfi_version,&bits))return -1;
    if(r==CE_OOB_H2G_SET && (v==bits.request || v==bits.release))
        return ceph_gmu_mmio_store(m,0x1f994,v);
    if(r==CE_OOB_G2H_CLEAR && v==bits.ack)return ceph_gmu_mmio_store(m,0x1f991,v);
    return -1;
}
static inline int ceph_gmu_mmio_hfi_notify(void *ctx)
{
    struct ceph_gmu_mmio *m=ctx;
    if(!m || (m->hfi_version>>28)!=2)return -1;
    return ceph_gmu_mmio_store(m,0x1f994,1);
}
static inline int ceph_gmu_mmio_fault(void *ctx)
{
    uint32_t ao,hfi;
    if(ceph_gmu_mmio_load(ctx,0x23b05,&ao) || ceph_gmu_mmio_load(ctx,0x1f992,&hfi) ||
        ao==UINT32_MAX || hfi==UINT32_MAX)return -1;
    return (ao&0x29u) || (hfi&(1u<<15))?-1:0;
}
/* GX register preflight under externally held keepalive: GX/SPTP power+clocks
 * on and AHB fence ALLOW. Never force the fence open or wake GX here. */
static inline int ceph_gmu_mmio_gx_access(struct ceph_gmu_mmio *m)
{
    uint32_t state,fence;
    if(ceph_gmu_mmio_load(m,0x1f8d0,&state) || state==UINT32_MAX || (state&0xd4u) ||
        ceph_gmu_mmio_load(m,0x23b10,&fence) || fence)return -1;
    return 0;
}
#endif
