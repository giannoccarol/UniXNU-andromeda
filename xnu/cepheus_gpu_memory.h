#ifndef CEPHEUS_GPU_MEMORY_H
#define CEPHEUS_GPU_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* True when every 4KiB page of [pa, pa+bytes) lies inside the pmap-managed
 * window [first, last). phystokv()/ml_static_vtop() panic outside it, so GPU
 * backing must never be handed such a range. Pure predicate, no MMIO. */
static inline int cepheus_gpu_pa_range_managed(uint64_t pa,size_t bytes,uint64_t first,uint64_t last)
{
    if(!bytes || (pa&4095u) || (bytes&4095u) || last<=first || pa<first)return 0;
    if(pa>UINT64_MAX-bytes)return 0;
    return pa+bytes<=last;
}
/* Encode a conservative upper bound with EXACTLY 12 trailing zero bits.
 * IOBMD decodes the trailing zero run as alignment and fills it with ones for
 * maxPhys. An arbitrary rounded PA can silently increase both. Round the top
 * down to 8KiB so mask bit12 is always set; at most one 4KiB page is excluded.
 * This cannot express a lower bound: validate the returned PA before remapping. */
static inline uint64_t cepheus_gpu_pa_window_mask(uint64_t first,uint64_t last,size_t host_page)
{
    if((host_page!=4096 && host_page!=16384) || !first || last<=first ||
        last>(1ULL<<40) || ((first|last)&(host_page-1)))return 0;
    uint64_t top=last&~8191ULL;
    if(top<=first || top-first<host_page)return 0;
    return top-4096;
}
/* Thread context, opt-in only. Allocates/tests/frees private DRAM, no GPU DMA. */
void cepheus_gpu_memory_selftest(void);
/* Internal kernel API, not an IOUserClient dispatch. Views are CPU leases;
 * return them before publish/release. No raw userspace pointers or DMA commands.
 * Published buffers intentionally cannot be freed until a verified retirement
 * backend exists. Synchronize CPU access yourself; cache type is Normal-NC. */
/* bytes in a view is actual host-page-rounded capacity (SWEET:16KiB). */
struct ceph_gpu_memory_view {void *cpu;uint64_t pa;size_t bytes;};
int cepheus_gpu_memory_alloc(size_t bytes,uint64_t *handle);
int cepheus_gpu_memory_view_get(uint64_t handle,struct ceph_gpu_memory_view *view);
int cepheus_gpu_memory_view_put(uint64_t handle);
int cepheus_gpu_memory_publish(uint64_t handle);
int cepheus_gpu_memory_release_cpu(uint64_t handle);
/* Exact-size GMU(kind=0) or SQE(kind=1) firmware snapshot. Input must be a
 * readable, pinned KERNEL buffer, never a raw userspace address. Snapshotting
 * hashes the PRIVATE copy, seals mutable views, and stores kind/valid length.
 * Readonly leases block release for the whole hash/use interval. They are not
 * hardware page protection against malicious kernel code; no user mapping. */
struct ceph_gpu_firmware_view {const uint8_t *data;size_t bytes;unsigned kind;};
int cepheus_gpu_firmware_snapshot(unsigned kind,const uint8_t *data,size_t bytes,uint64_t *handle);
int cepheus_gpu_firmware_get(uint64_t handle,unsigned expected_kind,struct ceph_gpu_firmware_view *view);
#ifdef __cplusplus
}
#endif
#endif
