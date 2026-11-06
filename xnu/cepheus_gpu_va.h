/* Immutable GMU virtual-address plan from actual owned allocation descriptors.
 * All backing memory is CPU Normal-NC. Firmware regions use privileged access;
 * HFI requires unprivileged RW. Log is privileged RW. All these KGSL mappings
 * omit IOMMU_NOEXEC: preserve device XN=0 even on writable cache backing. This
 * does not make any CPU address executable. GPU VA never equals a
 * cast CPU pointer. Allocator handles/leases remain owned by the caller.
 */
#ifndef CEPHEUS_GPU_VA_H
#define CEPHEUS_GPU_VA_H
#include "cepheus_gpu_vm.h"
#include "cepheus_gmu_start.h"
enum ceph_gmu_buffer_role {CE_BUF_ICACHE,CE_BUF_DCACHE,CE_BUF_HFI,CE_BUF_LOG,CE_BUF_PERSIST,CE_BUF_COUNT};
struct ceph_gpu_backing {uint64_t pa;size_t capacity;};
struct ceph_gpu_va_plan {struct ceph_vm_region regions[CE_BUF_COUNT];struct ceph_gmu_start_config start;};
static inline int ceph_gpu_va_plan(const struct ceph_gpu_backing backing[CE_BUF_COUNT],struct ceph_gpu_va_plan *out)
{
    if(!backing || !out)return -1;
    /* KGSL requires dummy/persist as first uncached slot; next slots are
     * separated by1MiB. HFI/log addresses are given to firmware explicitly. */
    static const uint32_t va[]={0x4000,0x44000,0x60100000,0x60200000,0x60000000};
    static const uint32_t length[]={0x3c000,0x3c000,5*4096,4096,4096};
    static const unsigned permissions[]={CE_VM_READ|CE_VM_WRITE|CE_VM_EXEC|CE_VM_NOCACHE,
        CE_VM_READ|CE_VM_WRITE|CE_VM_EXEC|CE_VM_NOCACHE,
        CE_VM_READ|CE_VM_WRITE|CE_VM_EXEC|CE_VM_UNPRIV|CE_VM_NOCACHE,
        CE_VM_READ|CE_VM_WRITE|CE_VM_EXEC|CE_VM_NOCACHE,
        CE_VM_READ|CE_VM_WRITE|CE_VM_EXEC|CE_VM_NOCACHE};
#ifdef __cplusplus
    struct ceph_gpu_va_plan p={};
#else
    struct ceph_gpu_va_plan p={0};
#endif
    for(unsigned i=0;i<CE_BUF_COUNT;++i) {
        const struct ceph_gpu_backing *b=&backing[i];
        if(!b->pa || (b->pa&4095u) || b->capacity<length[i] || b->capacity>256*1024 ||
            (b->capacity&4095u) || b->pa>=(1ULL<<40) || b->capacity>(1ULL<<40)-b->pa)return -1;
        for(unsigned j=0;j<i;++j) if(b->pa<backing[j].pa+backing[j].capacity && backing[j].pa<b->pa+b->capacity)return -1;
        p.regions[i]=(struct ceph_vm_region){va[i],b->pa,length[i],permissions[i]};
        if(!ceph_vm_valid(&p.regions[i]))return -1;
    }
    p.start=(struct ceph_gmu_start_config){0x06040001,va[CE_BUF_HFI],length[CE_BUF_HFI],va[CE_BUF_LOG],length[CE_BUF_LOG]};
    struct ceph_gmu_start_plan start;if(ceph_gmu_start_plan(&p.start,&start))return -1;
    *out=p;return 0;
}
#endif
