/* A640 HFI boot sequence, NOT A630 legacy INIT. Requires already booted CM3,
 * fresh empty rings, immutable real OPP/BCM/thermal inputs and exclusive session.
 * Unknown versions/errors are terminal; no draining/reusing inherited queues.
 */
#ifndef CEPHEUS_HFI_BOOT_H
#define CEPHEUS_HFI_BOOT_H
#include "cepheus_hfi_messages.h"
#include "cepheus_hfi_session.h"
#include "cepheus_gpu_bin.h"
struct ceph_hfi_boot_config {
    uint32_t hfi_version,min_fw_minor,throttle_mask;
    const struct ceph_hfi_opp *gx,*cx;unsigned ng,nc;
    const struct ceph_arc_resource *gfx,*cxl,*mx;
    const struct ceph_hfi_bw *bw;
    const struct ceph_hfi_acd *acd; /* NULL only when platform ACD is not selected */
    const struct ceph_gpu_bin_policy *policy;
};
static inline int ceph_hfi_boot_a640(struct ceph_hfi_session *s,
    const struct ceph_hfi_boot_config *c,uint32_t *fw_version)
{
    if(!fw_version)return CE_HFI_INVALID;
    *fw_version=0;
    if(!s || !s->tx || !s->rx || !c || s->sequence || s->poisoned || s->busy ||
        (c->hfi_version>>28)!=2 || c->min_fw_minor>4095 || !ceph_gpu_bin_policy_valid(c->policy) ||
        c->ng!=c->policy->gx_levels || c->throttle_mask!=c->policy->throttle_mask ||
        (!!c->acd)!=c->policy->acd)
        return CE_HFI_INVALID;
    uint32_t perf[59],bw[160],acd[17],small[4],reply[16];unsigned n;
    if(ceph_hfi_perf(perf,59,c->gx,c->ng,c->cx,c->nc,c->gfx,c->cxl,c->mx)!=59 ||
        ceph_hfi_bandwidth(bw,160,c->bw)!=160 ||
        (c->acd && ceph_hfi_acd_table(acd,17,c->acd)!=17))return CE_HFI_INVALID;
    unsigned r,w;
    int e=ceph_hfi_indices(s->tx,&r,&w);
    if(e || r!=w)return CE_HFI_INVALID;
    e=ceph_hfi_indices(s->rx,&r,&w);
    if(e || r!=w)return CE_HFI_INVALID;
    ceph_hfi_fw_version(small,4,2,c->min_fw_minor);
    e=ceph_hfi_exchange(s,small,2,reply,16,&n);if(e)return e;
    if(n!=4 || (reply[3]>>28)!=2 || ((reply[3]>>16)&4095u)<c->min_fw_minor)
        return ceph_hfi_session_poison(s,CE_HFI_PROTOCOL);
    uint32_t version=reply[3];
    e=ceph_hfi_exchange(s,perf,59,reply,16,&n);if(e)return e;
    e=ceph_hfi_exchange(s,bw,160,reply,16,&n);if(e)return e;
    ceph_hfi_feature(small,4,1,0,0); /* ECP disabled per A640 host driver */
    e=ceph_hfi_exchange(s,small,4,reply,16,&n);if(e)return e;
    if(c->acd) {
        e=ceph_hfi_exchange(s,acd,17,reply,16,&n);if(e)return e;
        ceph_hfi_feature(small,4,12,1,0);
        e=ceph_hfi_exchange(s,small,4,reply,16,&n);if(e)return e;
    }
    /* Source-mandated bin0 disables LM and ACD; do not invent a uniform LM
     * enable policy. Platform still needs independent TSENS/lifetime safeguards. */
    if(c->policy->lm) {
        ceph_hfi_feature(small,4,7,1,c->throttle_mask);
        e=ceph_hfi_exchange(s,small,4,reply,16,&n);if(e)return e;
    }
    ceph_hfi_core_start(small,4);
    e=ceph_hfi_exchange(s,small,2,reply,16,&n);if(e)return e;
    *fw_version=version;return 0;
}
#endif
