/* Read only the fixed A640 restore-list registers from an owned halted GX.
 * Does not use arbitrary caller offsets or unverified shadow values. No DMA
 * publication or CP packet execution: destination is CPU-owned scratch here.
 */
#ifndef CEPHEUS_CP_SNAPSHOT_MMIO_H
#define CEPHEUS_CP_SNAPSHOT_MMIO_H
#include "cepheus_cp_init.h"
#include "cepheus_cp_protection.h"
struct ceph_cp_snapshot_mmio {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    void *owner;int (*lease)(void *);void (*finish)(void *);
    unsigned active;
};
static inline int ceph_cp_snapshot_mmio_validate(void *ctx)
{
    struct ceph_cp_snapshot_mmio *s=ctx;
    return !s || !s->active || ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease)?-1:0;
}
static inline int ceph_cp_snapshot_mmio_begin(void *ctx)
{
    struct ceph_cp_snapshot_mmio *s=ctx;
    if(!s || s->active || !s->finish ||
        ceph_cp_protection_check(s->gmu,s->gpu,s->bytes,s->owner,s->lease))return -1;
    s->active=1;return 0;
}
static inline int ceph_cp_snapshot_mmio_read(void *ctx,uint32_t reg,uint32_t *value)
{
    struct ceph_cp_snapshot_mmio *s=ctx;
    if(!value || ceph_cp_snapshot_mmio_validate(s))return -1;
    unsigned found=0;for(unsigned i=0;i<66;++i)if(ceph_cp_restore_offsets[i]==reg)found=1;
    if(!found || reg>=s->bytes/4)return -1;
    *value=s->gpu[reg];ceph_gmu_mmio_barrier();return 0;
}
static inline void ceph_cp_snapshot_mmio_end(void *ctx)
{
    struct ceph_cp_snapshot_mmio *s=ctx;if(!s || !s->active)return;
    s->active=0;s->finish(s->owner);
}
static inline int ceph_cp_snapshot_mmio_copy(struct ceph_cp_snapshot_mmio *s,uint32_t *out,unsigned words)
{
    if(!s || !out || words<CE_CP_REGLIST_WORDS)return -1;
    /* Don't let a mistaken output VA overwrite device registers or the lease
     * object itself. Capacity isn't permission to write beyond136 words. */
    uintptr_t dst=(uintptr_t)out,dev=(uintptr_t)s->gpu,self=(uintptr_t)s;
    size_t bytes=CE_CP_REGLIST_WORDS*4;
    if((dst&3u) || bytes>UINTPTR_MAX-dst || s->bytes>UINTPTR_MAX-dev || sizeof(*s)>UINTPTR_MAX-self ||
        (dst<dev+s->bytes && dev<dst+bytes) || (dst<self+sizeof(*s) && self<dst+bytes))return -1;
    struct ceph_cp_snapshot_ops b={s,ceph_cp_snapshot_mmio_begin,ceph_cp_snapshot_mmio_read,
        ceph_cp_snapshot_mmio_validate,ceph_cp_snapshot_mmio_end};
    return ceph_cp_snapshot(&b,out,words);
}
#endif
