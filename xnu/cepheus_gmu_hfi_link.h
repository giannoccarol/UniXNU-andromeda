/* Bind real HFI queue indices/rings inside a published NC workspace. No
 * separate shadow indices: host and firmware use the ABI table in shared DRAM.
 * Owner keeps workspace/link address-stable and serializes exchanges. After
 * retirement begins this link admits only the active PREPARE_SLUMBER command;
 * ordinary commands are refused while the same resources remain pinned.
 */
#ifndef CEPHEUS_GMU_HFI_LINK_H
#define CEPHEUS_GMU_HFI_LINK_H
#include "cepheus_gmu_workspace.h"
#include "cepheus_hfi_session.h"
struct ceph_gmu_hfi_transport {
    void *ctx;
    int (*validate)(void *); /* GMU power/fault/thermal lease, bound to this workspace */
    int (*doorbell)(void *); /* exact H2G command queue interrupt */
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *);
    int (*fault)(void *);
};
struct ceph_gmu_hfi_link {
    struct ceph_gmu_workspace *workspace;
    struct ceph_gpu_publication identity;
    struct ceph_gmu_hfi_transport transport;
    struct ceph_hfi_queue tx,rx,debug;
    struct ceph_hfi_session session;
    uint32_t last_debug[4],last_error[18];
    unsigned debug_messages,error_words;
    int debug_error;
    int bound;
};
#ifdef __cplusplus
extern "C" {
#endif
int cepheus_gmu_hfi_bind(struct ceph_gmu_hfi_link *,struct ceph_gmu_workspace *,const struct ceph_gmu_hfi_transport *);
void cepheus_gmu_hfi_invalidate(struct ceph_gmu_hfi_link *);
/* Bounded async queue service; safe numeric snapshots, no firmware format
 * strings. Caller serializes with exchanges/retirement. Errors latch. */
int cepheus_gmu_hfi_poll_debug(struct ceph_gmu_hfi_link *);
#ifdef __cplusplus
}
#endif
#endif
