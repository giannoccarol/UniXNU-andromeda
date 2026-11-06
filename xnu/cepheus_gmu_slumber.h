/* A640 GMU slumber request directly through HFI, followed by actual state poll.
 * Only valid after CP/ring admission has closed and jobs/fences are quiescent.
 * Backend owns GX/GMU/SMMU until finish; does not unmap/free/turn off CX here.
 * IFPC/SPTP must be GMU-owned; a backend with host-managed SPTPRAC must refuse
 * admission rather than skip its required transition. No recovery retries.
 */
#ifndef CEPHEUS_GMU_SLUMBER_H
#define CEPHEUS_GMU_SLUMBER_H
#include "cepheus_hfi_session.h"
#include "cepheus_hfi_messages.h"
enum ceph_slumber_reg {CE_SL_POWER_COUNTER_ENABLE,CE_SL_RPMH_POWER_STATE,CE_SL_GX_STATUS,CE_SL_GMU_FAULT};
struct ceph_gmu_slumber_ops {
    void *ctx;
    int (*begin)(void *); /* block new submissions, drain pending work and hold resources */
    int (*validate)(void *);
    int (*read)(void *,enum ceph_slumber_reg,uint32_t *);
    int (*write)(void *,enum ceph_slumber_reg,uint32_t);
    int (*barrier)(void *);
    int (*now)(void *,uint64_t *);
    int (*pause)(void *);
    void (*finish)(void *,int slumbered);
};
struct ceph_gmu_slumber {int terminal,error,slumbered;};
static inline int ceph_gmu_slumber_request(struct ceph_gmu_slumber *s,struct ceph_gmu_slumber_ops *b,
    struct ceph_hfi_session *hfi,unsigned bw,unsigned freq,unsigned bw_levels,unsigned gx_levels)
{
    uint32_t cmd[3];
    if(!s || s->terminal || !b || !b->begin || !b->validate || !b->read || !b->write ||
        !b->barrier || !b->now || !b->pause || !b->finish || !hfi || !hfi->sequence ||
        hfi->busy || hfi->poisoned || ceph_hfi_slumber(cmd,3,bw,freq,bw_levels,gx_levels)!=3)return -1;
    s->terminal=1;int e=b->begin(b->ctx);if(e){s->error=e;return e;}
    uint32_t reply[16];unsigned n=0;
    if(b->validate(b->ctx)){e=-1;goto done;}
    e=b->write(b->ctx,CE_SL_POWER_COUNTER_ENABLE,0);if(e)goto done;
    e=b->barrier(b->ctx);if(e)goto done;
    e=ceph_hfi_exchange(hfi,cmd,3,reply,16,&n);if(e)goto done;
    uint64_t start,last,now;
    e=b->now(b->ctx,&start);if(e)goto done;last=start;
    for(unsigned i=0;i<10000;++i) {
        uint32_t state,gx,fault;
        if(b->validate(b->ctx) || b->now(b->ctx,&now)){e=-1;goto done;}
        if(now<last || now-start>=100000000ULL){e=-2;goto done;}last=now;
        if(b->read(b->ctx,CE_SL_GMU_FAULT,&fault) || fault ||
            b->read(b->ctx,CE_SL_RPMH_POWER_STATE,&state) || state==UINT32_MAX ||
            b->read(b->ctx,CE_SL_GX_STATUS,&gx) || gx==UINT32_MAX){e=-1;goto done;}
        if(state==15 && (gx&0xc0u)==0xc0u){s->slumbered=1;e=0;goto done;}
        e=b->pause(b->ctx);if(e)goto done;
    }
    e=-2;
done:
    s->error=e;b->finish(b->ctx,s->slumbered);return e;
}
#endif
