/* Gate in front of CP unhalt / unsecure entry.
 *
 * Downstream a6xx_rb_start() does, in order:
 *   a6xx_microcode_load()  -> writes CP_SQE_INSTR_BASE_{LO,HI}, then, ONLY if
 *                             device->mmu.secured, requires the ZAP subsystem
 *                             to be loadable through PIL (returns -ENODEV/-EPROBE
 *                             when it is not)
 *   CP_SQE_CNTL = 1        -> clears SQE halt, the CP starts executing
 *   a6xx_send_cp_init()    -> CP_ME_INIT through the ring (needs a working CP)
 *   adreno_set_unsecured_mode():
 *       if (zap_handle_ptr) adreno_switch_to_unsecure_mode()   <- proper path
 *       else                RBBM_SECVID_TRUST_CONTROL = 0      <- fallback
 *
 * This device's FDT declares gfx3d_secure, so mmu.secured is true and the ZAP
 * load is mandatory. The fallback branch writes the trust-control register to
 * disable secure verification; doing that to "make it boot" would defeat the
 * security firmware rather than satisfy it. This module therefore encodes the
 * preconditions and REFUSES the fallback unconditionally: no ZAP/PAS path means
 * no unhalt. It performs no writes and grants nothing.
 */
#ifndef CEPHEUS_CP_UNSECURE_GATE_H
#define CEPHEUS_CP_UNSECURE_GATE_H
#include <stdint.h>
enum ceph_cp_unhalt_refusal {
    CE_CP_UNHALT_OK = 0,
    CE_CP_UNHALT_NO_SECURE_CB,      /* FDT has no gfx3d_secure: gate not required */
    CE_CP_UNHALT_ZAP_NOT_LOADED,    /* secure CB present, ZAP/PAS unavailable */
    CE_CP_UNHALT_INSTR_BASE_UNSET,  /* SQE instruction base not programmed */
    CE_CP_UNHALT_RING_NOT_CONFIGURED,
    CE_CP_UNHALT_NO_ME_INIT,        /* CP_ME_INIT packet/restore list absent */
    CE_CP_UNHALT_FALLBACK_FORBIDDEN /* caller asked for the trust-control=0 path */
};
struct ceph_cp_unhalt_input {
    unsigned secure_context_present;
    unsigned zap_loaded;        /* a real PAS-authenticated ZAP, not a stub */
    unsigned instr_base_set;
    unsigned ring_configured;
    unsigned me_init_ready;
    unsigned wants_trust_control_fallback;
};
static inline enum ceph_cp_unhalt_refusal ceph_cp_unhalt_check(const struct ceph_cp_unhalt_input *i)
{
    if(!i)return CE_CP_UNHALT_RING_NOT_CONFIGURED;
    /* Never honour a request to disable secure verification. */
    if(i->wants_trust_control_fallback)return CE_CP_UNHALT_FALLBACK_FORBIDDEN;
    if(!i->secure_context_present)return CE_CP_UNHALT_NO_SECURE_CB;
    if(!i->zap_loaded)return CE_CP_UNHALT_ZAP_NOT_LOADED;
    if(!i->instr_base_set)return CE_CP_UNHALT_INSTR_BASE_UNSET;
    if(!i->ring_configured)return CE_CP_UNHALT_RING_NOT_CONFIGURED;
    if(!i->me_init_ready)return CE_CP_UNHALT_NO_ME_INIT;
    return CE_CP_UNHALT_OK;
}
/* The only admissible order once everything is ready. Kept explicit so a future
 * caller cannot reorder unhalt before the ZAP handshake. */
static inline int ceph_cp_unhalt_order(enum ceph_cp_unhalt_refusal gate)
{return gate==CE_CP_UNHALT_OK?0:-1;}
#endif
