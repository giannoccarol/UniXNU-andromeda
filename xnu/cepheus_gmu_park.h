/* Narrow GMU-only park AFTER HFI slumber has been acknowledged and GX has
 * powered down. This does not implement general live-GPU suspend/reset.
 * begin closes admission, verifies HFI slumber and ownership of clocks, PDC,
 * SMMU mappings and the bus. Readback of GX OFF is mandatory; no blind reset
 * of active graphics. No vote-off or global reset; caller keeps resources.
 */
#ifndef CEPHEUS_GMU_PARK_H
#define CEPHEUS_GMU_PARK_H
#include <stdint.h>
enum ceph_gmu_park_reg {CE_PARK_GX_STATUS,CE_PARK_TCS0,CE_PARK_TCS1,CE_PARK_TCS2,CE_PARK_TCS3,
    CE_PARK_SYSRESET,CE_PARK_GBIF_HALT,CE_PARK_GBIF_ACK,CE_PARK_REG_COUNT};
struct ceph_gmu_park_ops {
    void *ctx;
    int (*begin)(void *); /* bounded slumber+admission barrier, no unknown prior reset */
    int (*validate)(void *);
    int (*read)(void *,enum ceph_gmu_park_reg,uint32_t *);
    int (*write)(void *,enum ceph_gmu_park_reg,uint32_t);
    int (*barrier)(void *);
    int (*now)(void *,uint64_t *);
    int (*pause)(void *);
    void (*finish)(void *,int parked); /* even failure leaves ownership pinned */
};
struct ceph_gmu_park {unsigned attempted;int terminal,parked,error;};
static inline int ceph_park_read(struct ceph_gmu_park_ops *b,enum ceph_gmu_park_reg r,uint32_t *v)
{return b->validate(b->ctx) || b->read(b->ctx,r,v) || *v==UINT32_MAX?-1:0;}
static inline int ceph_park_wait(struct ceph_gmu_park_ops *b,enum ceph_gmu_park_reg r,uint32_t mask,uint32_t value,uint64_t limit)
{
    uint64_t start,last,now;if(b->now(b->ctx,&start))return -1;last=start;
    for(unsigned i=0;i<10000;++i) {
        if(b->now(b->ctx,&now))return -1;
        if(now<last || now-start>=limit)return -2;
        last=now;uint32_t v;if(ceph_park_read(b,r,&v))return -1;
        if((v&mask)==value)return 0;
        if(b->pause(b->ctx))return -1;
    }
    return -2;
}
static inline int ceph_gmu_park_slumbered(struct ceph_gmu_park *s,struct ceph_gmu_park_ops *b)
{
    if(!s || s->terminal || s->attempted || !b || !b->begin || !b->validate || !b->read ||
        !b->write || !b->barrier || !b->now || !b->pause || !b->finish)return -1;
    s->terminal=1;int e=b->begin(b->ctx);if(e){s->error=e;return e;}
    uint32_t gx,halt;
    if(ceph_park_read(b,CE_PARK_GX_STATUS,&gx) || (gx&0xc0u)!=0xc0u){e=-1;goto done;}
    for(unsigned i=0;i<4;++i) {
        e=ceph_park_wait(b,(enum ceph_gmu_park_reg)(CE_PARK_TCS0+i),1,1,1000000);if(e)goto done;
    }
    if(b->validate(b->ctx)){e=-1;goto done;}
    ++s->attempted;e=b->write(b->ctx,CE_PARK_SYSRESET,1);if(e)goto done;
    e=b->barrier(b->ctx);if(e)goto done;
    e=ceph_park_wait(b,CE_PARK_SYSRESET,UINT32_MAX,1,1000000);if(e)goto done;
    if(ceph_park_read(b,CE_PARK_GBIF_HALT,&halt)){e=-1;goto done;}
    ++s->attempted;e=b->write(b->ctx,CE_PARK_GBIF_HALT,halt|2u);if(e)goto done;
    e=b->barrier(b->ctx);if(e)goto done;
    e=ceph_park_wait(b,CE_PARK_GBIF_ACK,2,2,100000000);if(e)goto done;
    if(ceph_park_read(b,CE_PARK_GX_STATUS,&gx) || (gx&0xc0u)!=0xc0u){e=-1;goto done;}
    s->parked=1;
done:s->error=e;b->finish(b->ctx,s->parked);return e;
}
#endif
