/* Kernel-only publication group and retirement API. Source callbacks are
 * trusted platform implementations, not user-controlled function pointers.
 * The production orchestrator executes slumber/park/detach before pool release;
 * caller-supplied ready booleans are not accepted. No default hardware backend.
 */
#ifndef CEPHEUS_GPU_RETIRE_H
#define CEPHEUS_GPU_RETIRE_H
#include <stdint.h>
struct ceph_smmu_plan;
struct ceph_gmu_slumber_ops;
struct ceph_gmu_park_ops;
struct ceph_smmu_detach_ops;
struct ceph_hfi_session;
enum {CE_GPU_WORKSPACE_BUFFERS=6,CE_GPU_WORKSPACE_ROOT=5,CE_GPU_WORKSPACE_VIEWS=(1u<<6)-1};
struct ceph_gpu_publication {
    uint64_t group;
    uint64_t handles[CE_GPU_WORKSPACE_BUFFERS];
    /* Matches owned root/context identity at publication, not a pointer to
     * mutable caller storage. Caller must not map this memory elsewhere. */
    uint64_t ttbr0,ttbr1;
    uint32_t smr,cb,slot,asid;
};
struct ceph_gpu_retire_ops {
    struct ceph_gmu_slumber_ops *slumber;
    struct ceph_gmu_park_ops *park;
    struct ceph_smmu_detach_ops *detach;
    struct ceph_hfi_session *hfi;
    const struct ceph_smmu_plan *plan;
    unsigned bw,freq,bw_levels,gx_levels;
};
enum {CE_GPU_GROUP_SHARED=1,CE_GPU_GROUP_RETIRING,CE_GPU_GROUP_UNCERTAIN,CE_GPU_GROUP_DETACHED_RETAINED};
struct ceph_gpu_group_status {
    struct ceph_gpu_publication identity;
    uint64_t retained_bytes;
    unsigned state,retained_buffers,pool_terminal;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Atomic six-buffer transition CPU+one view -> SHARED+same view. No MMIO.
 * Buffer index5 is root pool. Publish BEFORE exposing addresses to hardware.
 * Pool validates capacities, role/PA bindings, all materialized PTEs and fresh
 * HFI header under its mutex before changing any slot to shared. */
int cepheus_gpu_memory_share_workspace(const uint64_t handles[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_smmu_plan *,struct ceph_gpu_publication *);
int cepheus_gpu_memory_retire_workspace(const struct ceph_gpu_publication *,struct ceph_gpu_retire_ops *);
/* Diagnosis only: no clear/reset/retry controls. Failed hardware retirement
 * ends this diagnostic GPU session for the boot, deliberately not recoverable
 * through a blind timeout retry. Group/context evidence survives partial free. */
int cepheus_gpu_memory_group_status(uint64_t,struct ceph_gpu_group_status *);
/* One-way abandonment after uncertain startup. Never frees, detaches or writes
 * hardware. Blocks new pool allocation and all shared-session access for boot. */
int cepheus_gpu_memory_abandon_workspace(const struct ceph_gpu_publication *);
#ifdef __cplusplus
}
#endif
#endif
