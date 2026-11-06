/* A640 GBIF/QSB block: the first thing a6xx_start() does, via
 * adreno_vbif_start(adreno_dev, a6xx_vbif_platforms, ...).
 *
 * A640 selects the a640_gbif table, NOT a630_vbif: four GBIF_QSB_SIDE
 * registers get 0x00071620 and RBBM_GBIF_CLIENT_QOS_CNTL gets 0x3. The a630
 * table also writes RBBM_VBIF_CLIENT_QOS_CNTL, which A640 does not.
 *
 * Two of these registers live in the distinct GBIF window (0x3c03..0x3c06,
 * reachable through the GPU aperture) while the QoS control is RBBM-relative.
 * CP stays halted, the caller owns powered GX/SPTP/fence, and this module does
 * not halt/deassert GBIF, reset anything, or touch clocks/votes.
 */
#ifndef CEPHEUS_GX_GBIF_H
#define CEPHEUS_GX_GBIF_H
#include "cepheus_gx_core.h"
struct ceph_gx_gbif_plan {struct ceph_gx_write writes[5];};
static inline int ceph_gx_gbif_plan(const struct ceph_gx_core_input *i,struct ceph_gx_gbif_plan *out)
{
    if(!i || !out || i->chipid!=0x06040001)return -1;
    *out=(struct ceph_gx_gbif_plan){{{0x3c03,0x00071620},{0x3c04,0x00071620},
        {0x3c05,0x00071620},{0x3c06,0x00071620},{0x11,0x3}}};
    return 0;
}
struct ceph_gx_gbif {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    struct ceph_gx_core_input input;
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_gx_gbif_configure(struct ceph_gx_gbif *s)
{
    struct ceph_gx_gbif_plan p;
    if(!s || s->terminal || s->writes || !s->finish || ceph_gx_gbif_plan(&s->input,&p) ||
        ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease))return -1;
    s->terminal=1;int e=0;
    for(unsigned n=0;n<5;++n) {
        if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease)){e=-1;goto done;}
        ++s->writes;ceph_gmu_mmio_barrier();s->gpu[p.writes[n].word]=p.writes[n].value;ceph_gmu_mmio_barrier();
        if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease) ||
            s->gpu[p.writes[n].word]!=p.writes[n].value){e=-1;goto done;}
    }
    s->configured=1;
done:s->error=e;s->finish(s->owner,1);return e;
}
#endif
