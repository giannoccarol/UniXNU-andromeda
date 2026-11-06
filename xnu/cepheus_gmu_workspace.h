/* CPU-side GMU memory preparation. Allocates real XNU memory through the
 * internal pool; never maps a SMMU, publishes a DMA target or starts hardware.
 * Workspace is single-owner and address-stable while views are outstanding.
 */
#ifndef CEPHEUS_GMU_WORKSPACE_H
#define CEPHEUS_GMU_WORKSPACE_H
#include "cepheus_gpu_memory.h"
#include "cepheus_gpu_va.h"
#include "cepheus_gpu_retire.h"
enum ceph_workspace_state {CE_WS_NEW,CE_WS_BUILDING,CE_WS_READY_CPU,CE_WS_RELEASED,CE_WS_FAILED,CE_WS_RETAINED,CE_WS_SHARED};
/* RETAINED is terminal for this workspace, not a retry indication. */
struct ceph_gmu_workspace {
    uint64_t handles[CE_GPU_WORKSPACE_BUFFERS];struct ceph_gpu_memory_view views[CE_GPU_WORKSPACE_BUFFERS];unsigned leased;
    struct ceph_gpu_va_plan layout;struct ceph_vm_pool table;
    enum ceph_workspace_state state;
    struct ceph_gpu_publication publication;
};
#ifdef __cplusplus
extern "C" {
#endif
int cepheus_gmu_workspace_prepare(struct ceph_gmu_workspace *);
int cepheus_gmu_workspace_release_cpu(struct ceph_gmu_workspace *);
int cepheus_gmu_workspace_share(struct ceph_gmu_workspace *,const struct ceph_smmu_plan *);
int cepheus_gmu_workspace_retire(struct ceph_gmu_workspace *,struct ceph_gpu_retire_ops *);
void cepheus_gmu_workspace_selftest(void);
#ifdef __cplusplus
}
#endif
#endif
