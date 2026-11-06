/* Bind upload TCM destinations to exact windows of the leased GMU aperture.
 * KGSL A6XX_GMU_CM3_ITCM_START=0x1b400, DTCM_START=0x1c400 (GPU words),
 * hence GMU-relative byte offsets0x3000/0x7000, each16KiB. No extra ioremap,
 * CPU-pointer-to-PA casts or assumed coherent WB backing. Workspace supplies
 * verified NC cache mappings; this adapter publishes with full system ordering.
 */
#ifndef CEPHEUS_GMU_UPLOAD_MMIO_H
#define CEPHEUS_GMU_UPLOAD_MMIO_H
#include "cepheus_gmu_upload.h"
#include "cepheus_gmu_mmio.h"
struct ceph_gmu_upload_mmio {
    struct ceph_gmu_mmio *gmu;
    void *owner;
    void (*finish)(void *,int uncertain); /* keep mappings/power pinned */
    unsigned active,terminal;
};
static inline int ceph_gmu_upload_mmio_reset(void *ctx)
{
    struct ceph_gmu_upload_mmio *m=ctx;uint32_t reset;
    return !m || !m->active || m->terminal || ceph_gmu_mmio_start_read(m->gmu,CE_GMU_SYSRESET,&reset) ||
        reset!=1 || ceph_gmu_mmio_fault(m->gmu)?-1:0;
}
static inline int ceph_gmu_upload_mmio_begin(void *ctx)
{
    struct ceph_gmu_upload_mmio *m=ctx;
    if(!m || m->active || m->terminal || !m->finish)return -1;
    m->active=1;
    if(ceph_gmu_upload_mmio_reset(m)){m->active=0;m->terminal=1;return -1;}
    return 0;
}
static inline int ceph_gmu_upload_mmio_publish(void *ctx)
{
    if(ceph_gmu_upload_mmio_reset(ctx))return -1;
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error "GMU upload requires arm64"
#endif
    return ceph_gmu_upload_mmio_reset(ctx);
}
static inline void ceph_gmu_upload_mmio_end(void *ctx,int uncertain)
{
    struct ceph_gmu_upload_mmio *m=ctx;if(!m || !m->active)return;
    m->active=0;m->terminal=1;m->finish(m->owner,uncertain);
}
static inline int ceph_gmu_upload_mmio_bind(struct ceph_gmu_upload_mmio *m,struct ceph_gmu_upload *out)
{
    if(!m || !out || m->active || m->terminal || !m->finish || ceph_gmu_mmio_ready(m->gmu))return -1;
    *out=(struct ceph_gmu_upload){(volatile uint32_t *)(m->gmu->base+0x3000),
        (volatile uint32_t *)(m->gmu->base+0x7000),4096,4096,NULL,0,m,ceph_gmu_upload_mmio_begin,
        ceph_gmu_upload_mmio_reset,ceph_gmu_upload_mmio_publish,ceph_gmu_upload_mmio_end,0,0};return 0;
}
#endif
