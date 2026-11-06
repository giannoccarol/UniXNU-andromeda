/* CPU-only CP backing. Separate from the GMU SID5 workspace; NO GPU-address
 * assignment, SMMU publication or DMA retirement API exists here yet. A future
 * GX address-space owner must map/seal it before any CP register configuration.
 */
#ifndef CEPHEUS_CP_WORKSPACE_H
#define CEPHEUS_CP_WORKSPACE_H
#include "cepheus_gpu_memory.h"
#include "cepheus_sqe.h"
#include "cepheus_gpu_vm39.h"
enum {CE_CP_RING,CE_CP_RPTR,CE_CP_SQE,CE_CP_RESTORE,CE_CP_SCRATCH,CE_CP_ROOT,CE_CP_BUFFERS};
/* Device mapping permissions, not host pmap: KGSL ring RO, SQE RO+UCODE,
 * scratch/RPTR/restore privileged RW; all current backing Normal-NC. */
static inline unsigned ceph_cp_workspace_permissions(unsigned role)
{
    if(role==CE_CP_RING)return CE_VM_READ|CE_VM_UNPRIV|CE_VM_NOCACHE;
    if(role==CE_CP_SQE)return CE_VM_READ|CE_VM_EXEC|CE_VM_UNPRIV|CE_VM_NOCACHE;
    if(role==CE_CP_RPTR || role==CE_CP_RESTORE || role==CE_CP_SCRATCH)return CE_VM_READ|CE_VM_WRITE|CE_VM_NOCACHE;
    return 0;
}
enum ceph_cp_workspace_state {CE_CP_WS_NEW,CE_CP_WS_BUILDING,CE_CP_WS_READY,CE_CP_WS_RELEASED,CE_CP_WS_RETAINED,CE_CP_WS_TABLED};
struct ceph_cp_workspace {
    uint64_t handles[CE_CP_BUFFERS],firmware;
    struct ceph_gpu_memory_view views[CE_CP_BUFFERS];
    struct ceph_sqe_image sqe;
    struct ceph_vm_pool table;
    struct ceph_vm_region regions[CE_CP_ROOT];
    unsigned leased,firmware_view,restore_ready;
    enum ceph_cp_workspace_state state;
};
struct ceph_cp_snapshot_mmio;
#ifdef __cplusplus
extern "C" {
#endif
int cepheus_cp_workspace_prepare(struct ceph_cp_workspace *,const uint8_t *,size_t);
int cepheus_cp_workspace_release(struct ceph_cp_workspace *);
/* Build only CPU page-table data from caller-reserved GPU VAs. Does not install
 * a context or publish DMA. VA reservation/GMEM carveout ownership is external. */
int cepheus_cp_workspace_table(struct ceph_cp_workspace *,const uint64_t va[CE_CP_ROOT]);
/* CPU-only integrity check before later mapping/configuration integration.
 * Verifies live pool view identity, fixed SQE payload digest/tail and page tables.
 * Does not seal CPU VAs or prove GPU mapping/PAS authentication. */
int cepheus_cp_workspace_validate(struct ceph_cp_workspace *);
/* Read fixed protected GX regs into CPU-owned restore buffer and encode one
 * ME_INIT packet for later use. Does not publish, unhalt or ring a doorbell. */
int cepheus_cp_workspace_restore(struct ceph_cp_workspace *,struct ceph_cp_snapshot_mmio *,uint32_t packet[12]);
#ifdef __cplusplus
}
#endif
#endif
