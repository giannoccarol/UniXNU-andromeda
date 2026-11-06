/* A640 LLC configuration AFTER a real platform slice activation/lease.
 * adreno_llc_setup -> a6xx_llc_configure_gpu_scid uses GBIF_SCACHE_CNTL1.
 * GPUHTW SCID is an XBL NoC override on A640; never rewrite it here. MMU-500
 * does not support GBIF cache attribute overrides, so neither CNTL0 nor
 * GPU_CX_MISC_SYSTEM_CACHE_CNTL* is written. No LLCC activation backend here.
 */
#ifndef CEPHEUS_GX_LLC_H
#define CEPHEUS_GX_LLC_H
#include "cepheus_cp_protection.h"
enum { CE_GX_LLC_GPU_SCID=12, CE_GX_LLC_GPUHTW_SCID=11 };
struct ceph_gx_llc {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    uint32_t chipid;unsigned gpu_scid;
    /* Must own both powered GX/OOB and the ACTIVATED GPU LLCC slice for the
     * whole call. A descriptor or cache-slices FDT property is not a lease. */
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_gx_llc_configure(struct ceph_gx_llc *s)
{
    if(!s || s->terminal || s->writes || !s->finish || s->chipid!=0x06040001 ||
        s->gpu_scid!=CE_GX_LLC_GPU_SCID ||
        ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease))return -1;
    s->terminal=1;int e=-1;
    const uint32_t mask=0x01ffffffu; /* five independent 5-bit GPU SCIDs */
    uint32_t value=0;
    for(unsigned i=0;i<5;++i)value=(value<<5)|s->gpu_scid;
    uint32_t before=s->gpu[0x3c02];ceph_gmu_mmio_barrier();
    if(before==UINT32_MAX || ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease))goto done;
    uint32_t next=(before&~mask)|value;
    ++s->writes;ceph_gmu_mmio_barrier();s->gpu[0x3c02]=next;ceph_gmu_mmio_barrier();
    if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease) || s->gpu[0x3c02]!=next)goto done;
    s->configured=1;e=0;
done:s->error=e;s->finish(s->owner,1);return e;
}
#endif
