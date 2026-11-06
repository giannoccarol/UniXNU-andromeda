/* Internal C/C++ bridge, not a public driver interface. Only retirement
 * orchestrator may thaw/release a publication after actual protocol success.
 * Kernel code remains trusted; this is lifetime discipline, not a security
 * boundary against arbitrary privileged callers. No IOUserClient exposes it.
 */
#ifndef CEPHEUS_GPU_MEMORY_PRIVATE_H
#define CEPHEUS_GPU_MEMORY_PRIVATE_H
#include "cepheus_gpu_retire.h"
#include "cepheus_gpu_memory.h"
#ifdef __cplusplus
extern "C" {
#endif
int ceph_memory_share_group(const uint64_t[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_gpu_publication *,struct ceph_gpu_publication *);
int ceph_memory_validate_workspace(const struct ceph_gpu_memory_view[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_gpu_publication *);
int ceph_memory_retire_begin(const struct ceph_gpu_publication *);
/* success means the orchestrator finished all protocol operations, not a
 * caller assertion. False permanently quarantines the entire group. */
int ceph_memory_retire_finish(const struct ceph_gpu_publication *,int success);
#ifdef __cplusplus
}
#endif
#endif
