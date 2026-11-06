/* A640 perfcounter/GPU-busy block from a6xx_start(), NOT full GX init.
 * Values are transcribed from adreno_a6xx.c; the two power-counter registers
 * live in the GMU aperture while the RBBM ones are GPU-relative, so the two
 * address spaces are kept explicitly separate here. CP stays halted and the
 * caller owns powered GX/SPTP/fence. No clock, vote, HWCG or VBIF writes.
 */
#ifndef CEPHEUS_GX_PERFCOUNTER_H
#define CEPHEUS_GX_PERFCOUNTER_H
#include "cepheus_gx_core.h"
struct ceph_gx_perf_write {uint32_t word,value;int gmu;};
struct ceph_gx_perf_plan {struct ceph_gx_perf_write writes[5];};
/* RBBM_PERFCTR_CNTL/GPU_BUSY_MASKED are GPU-relative; GMU_AO_GPU_CX_BUSY_MASK,
 * GMU_CX_GMU_POWER_COUNTER_ENABLE and _SELECT_0 are GMU-relative word offsets. */
static inline int ceph_gx_perf_plan(const struct ceph_gx_core_input *core,struct ceph_gx_perf_plan *out)
{
    if(!core || !out || core->chipid!=0x06040001)return -1;
    *out=(struct ceph_gx_perf_plan){{{0x50b,0xffffffffu,0},{0x500,1u,0},
        {0x23b0e,0xff000000u,1},{0x1f841,0x20u,1},{0x1f840,1u,1}}};
    return 0;
}
struct ceph_gx_perfcounter {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    struct ceph_gx_core_input core;
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_gx_perf_ready(struct ceph_gx_perfcounter *s)
{return ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease);}
/* SELECT_0 is a masked RMW in the source (rmw 0xff -> 0x20); the other four
 * are plain writes. A read that cannot be masked exactly is a failure, never
 * a blind overwrite of the neighbouring counter-select fields. */
static inline int ceph_gx_perf_apply(struct ceph_gx_perfcounter *s,const struct ceph_gx_perf_write *w)
{
    if(!w)return -1;
    if(w->gmu) {
        uint32_t actual;
        if(w->word==0x1f841) {
            if(ceph_gmu_mmio_load(s->gmu,w->word,&actual))return -1;
            uint32_t v=(actual&~0xffu)|w->value;
            if(ceph_gmu_mmio_store(s->gmu,w->word,v) || ceph_gmu_mmio_load(s->gmu,w->word,&actual) ||
                actual!=v)return -1;
            return 0;
        }
        if(ceph_gmu_mmio_store(s->gmu,w->word,w->value) ||
            ceph_gmu_mmio_load(s->gmu,w->word,&actual) || actual!=w->value)return -1;
        return 0;
    }
    if(w->word>=s->bytes/4)return -1;
    ceph_gmu_mmio_barrier();s->gpu[w->word]=w->value;ceph_gmu_mmio_barrier();
    return s->gpu[w->word]!=w->value?-1:0;
}
static inline int ceph_gx_perf_configure(struct ceph_gx_perfcounter *s)
{
    struct ceph_gx_perf_plan p;
    if(!s || s->terminal || s->writes || !s->finish || ceph_gx_perf_plan(&s->core,&p) ||
        ceph_gx_perf_ready(s))return -1;
    s->terminal=1;int e=0;
    for(unsigned i=0;i<5;++i) {
        if(ceph_gx_perf_ready(s)){e=-1;goto done;}
        ++s->writes;
        if(ceph_gx_perf_apply(s,&p.writes[i]) || ceph_gx_perf_ready(s)){e=-1;goto done;}
    }
    s->configured=1;
done:
    /* Never unhalt, reset or re-enable anything on the way out. */
    s->error=e;s->finish(s->owner,1);return e;
}
#endif
