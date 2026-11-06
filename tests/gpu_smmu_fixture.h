/* RAM model for SMMUv2 side effects/initial inherited contents. Real production
 * adapters perform all install/detach reads and writes. Never runtime defaults. */
#ifndef CEPHEUS_GPU_SMMU_FIXTURE_H
#define CEPHEUS_GPU_SMMU_FIXTURE_H
#include <assert.h>
#include "../xnu/cepheus_smmu_mmio.h"
static void smmu_fixture_set(struct ceph_smmu_mmio *m,enum ceph_smmu_reg r,uint64_t value)
{
    uint32_t off;unsigned width;assert(!ceph_smmu_mmio_offset(m,r,&off,&width));
    if(width==8)*(volatile uint64_t *)(m->base+off)=value;
    else *(volatile uint32_t *)(m->base+off)=(uint32_t)value;
}
static void smmu_fixture_init(struct ceph_smmu_mmio *m,uint64_t *ram,
    const struct ceph_smmu_plan *plan,void *owner,int (*lease)(void *))
{
    *m=(struct ceph_smmu_mmio){.base=(volatile uint8_t *)ram,.mapped_bytes=0x10000,
        .plan=*plan,.actlr=0x303,.owner=owner,.lease=lease};
    /* Explicit emulated capabilities, not live-ID evidence. */
    assert(!ceph_smmu_geometry((1u<<30)|(1u<<27)|(11u<<9)|16,(2u<<28)|8,0x1022,0x10000,&m->geometry));
    smmu_fixture_set(m,CE_SM_S2CR,plan->s2cr_fault);
}
#endif
