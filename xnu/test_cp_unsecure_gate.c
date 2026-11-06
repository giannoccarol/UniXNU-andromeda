#include <assert.h>
#include <stdio.h>
#include "cepheus_cp_unsecure_gate.h"
int main(void)
{
    struct ceph_cp_unhalt_input ok={1,1,1,1,1,0};
    assert(ceph_cp_unhalt_check(&ok)==CE_CP_UNHALT_OK && !ceph_cp_unhalt_order(CE_CP_UNHALT_OK));
    /* This device has gfx3d_secure but NO PAS/ZAP path, so unhalt must be
     * refused with a specific reason, not silently allowed. */
    struct ceph_cp_unhalt_input no_zap=ok;no_zap.zap_loaded=0;
    assert(ceph_cp_unhalt_check(&no_zap)==CE_CP_UNHALT_ZAP_NOT_LOADED);
    assert(ceph_cp_unhalt_order(ceph_cp_unhalt_check(&no_zap))<0);
    /* The trust-control fallback is never a legal substitute. */
    struct ceph_cp_unhalt_input fb=ok;fb.wants_trust_control_fallback=1;
    assert(ceph_cp_unhalt_check(&fb)==CE_CP_UNHALT_FALLBACK_FORBIDDEN);
    struct ceph_cp_unhalt_input fb2=no_zap;fb2.wants_trust_control_fallback=1;
    assert(ceph_cp_unhalt_check(&fb2)==CE_CP_UNHALT_FALLBACK_FORBIDDEN);
    /* Each remaining precondition is individually enforced. */
    struct ceph_cp_unhalt_input t;
    t=ok;t.instr_base_set=0;assert(ceph_cp_unhalt_check(&t)==CE_CP_UNHALT_INSTR_BASE_UNSET);
    t=ok;t.ring_configured=0;assert(ceph_cp_unhalt_check(&t)==CE_CP_UNHALT_RING_NOT_CONFIGURED);
    t=ok;t.me_init_ready=0;assert(ceph_cp_unhalt_check(&t)==CE_CP_UNHALT_NO_ME_INIT);
    /* Without a secure CB the gate does not apply, and it says so explicitly
     * rather than reporting OK. */
    t=ok;t.secure_context_present=0;assert(ceph_cp_unhalt_check(&t)==CE_CP_UNHALT_NO_SECURE_CB);
    assert(ceph_cp_unhalt_check(0)!=CE_CP_UNHALT_OK);
    /* Ordering helper only admits OK. */
    for(int r=0;r<=CE_CP_UNHALT_FALLBACK_FORBIDDEN;++r)
        if(r!=(int)CE_CP_UNHALT_OK)assert(ceph_cp_unhalt_order((enum ceph_cp_unhalt_refusal)r)<0);
    puts("CP unhalt gate: ZAP/PAS required with secure CB, trust-control fallback always refused PASS");
}
