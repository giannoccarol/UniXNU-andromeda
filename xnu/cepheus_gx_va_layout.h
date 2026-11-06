/* GX VA partition when the platform has a secure context bank.
 *
 * The saved Cepheus FDT declares /soc/qcom,kgsl-iommu/gfx3d_secure
 * (SID2/mask0x400), so kgsl_iommu_probe sets device->mmu.secured=true and
 * adreno _set_secvid() actually runs. In that configuration
 * setup_64bit_pagetable() gives the SECURE pagetable
 *   [secure_base, KGSL_IOMMU_GLOBAL_MEM_BASE64)
 * and confines every non-secure pagetable to
 *   [svm_base32, secure_base)  (32-bit/compat)  and
 *   [KGSL_IOMMU_VA_BASE64, KGSL_IOMMU_VA_END64)  (64-bit)
 * with secure_base = KGSL_IOMMU_GLOBAL_MEM_BASE64 - secure_size
 * and secure_size defaulting to SZ_256M (no qcom,secure-size in this FDT).
 *
 * This module only computes that partition and rejects overlaps. It does not
 * allocate a CB, program SMR/S2CR, authenticate ZAP/PAS, or write SECVID
 * registers: those still require a trusted SCM/PAS path that does not exist in
 * this port. Nothing here authorises a hardware write.
 */
#ifndef CEPHEUS_GX_VA_LAYOUT_H
#define CEPHEUS_GX_VA_LAYOUT_H
#include <stdint.h>
/* Values from kgsl_iommu.h on this kernel revision. */
enum {
    CE_GX_GLOBAL_MEM_BASE64 = 0xfc000000u,
    CE_GX_VA_BASE64_LO      = 0x500000000ull,
    CE_GX_VA_BASE64_HI      = 0x600000000ull, /* KGSL_IOMMU_VA_END64 */
    CE_GX_SECURE_SIZE       = 0x10000000u     /* SZ_256M */
};
struct ceph_gx_va_range {uint64_t start,end;}; /* [start,end) */
struct ceph_gx_va_partition {
    struct ceph_gx_va_range secure,user32,user64;
    unsigned secure_context_present;
};
/* The GMU workspace deliberately lives at 0x60000000 (see ceph_gpu_va), which
 * is far below the 64-bit user window and must not be disturbed by this split. */
static inline int ceph_gx_va_partition(unsigned secured,uint64_t secure_size,
    struct ceph_gx_va_partition *out)
{
    if(!out || secured>1)return -1;
    if(!secured) {
        /* No secure CB: the whole 32-bit window is available to user space. */
        *out=(struct ceph_gx_va_partition){{0,0},{0x300000ull,CE_GX_GLOBAL_MEM_BASE64},
            {CE_GX_VA_BASE64_LO,CE_GX_VA_BASE64_HI},0};
        return 0;
    }
    /* secure_size must leave a non-empty user window above svm_base32. */
    if(!secure_size || secure_size>CE_GX_SECURE_SIZE || secure_size&4095u)return -1;
    uint64_t base=(uint64_t)CE_GX_GLOBAL_MEM_BASE64-secure_size;
    if(base<=0x300000ull)return -1;
    *out=(struct ceph_gx_va_partition){{base,CE_GX_GLOBAL_MEM_BASE64},
        {0x300000ull,base},{CE_GX_VA_BASE64_LO,CE_GX_VA_BASE64_HI},1};
    return 0;
}
/* Any GX mapping must fall entirely inside exactly one non-secure range. */
static inline int ceph_gx_va_fits(const struct ceph_gx_va_partition *p,uint64_t start,uint64_t bytes)
{
    if(!p || !bytes || (start|bytes)&4095u || start>UINT64_MAX-bytes)return -1;
    const struct ceph_gx_va_range *r[2]={&p->user32,&p->user64};
    for(unsigned i=0;i<2;++i)
        if(start>=r[i]->start && start+bytes<=r[i]->end)return (int)i;
    return -1;
}
/* A 39-bit page table cannot address the 64-bit window at all, and the secure
 * window must never appear in a non-secure table. */
static inline int ceph_gx_va_plan_check(const struct ceph_gx_va_partition *p,unsigned va_bits)
{
    if(!p || (va_bits!=39 && va_bits!=48))return -1;
    /* Every advertised window must be addressable by the page-table geometry.
     * A 39-bit table reaches 512GiB, so the 64-bit user window (20-24GiB) does
     * fit; what must never happen is a window extending past the VA space. */
    uint64_t limit=1ull<<va_bits;
    if(p->user32.end>limit || p->user64.end>limit)return -1;
    if(p->user32.start>=p->user32.end)return -1;
    if(p->secure_context_present &&
        (p->user32.end!=p->secure.start || p->secure.end!=CE_GX_GLOBAL_MEM_BASE64))return -1;
    return 0;
}
#endif
