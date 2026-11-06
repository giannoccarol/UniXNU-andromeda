#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_smmu_mmio.h"
static uint64_t aperture[0x10000/8];
static int lease(void *p){return *(int *)p;}
int main(void)
{
    struct ceph_smmu_stream streams[16]={0};for(unsigned i=0;i<16;++i)streams[i].s2cr=2u<<16;
    struct ceph_smmu_mmio m={0};int busy=0;m.base=(volatile uint8_t *)aperture;m.mapped_bytes=sizeof(aperture);m.actlr=0x303;m.owner=&busy;m.lease=lease;
    assert(!ceph_smmu_geometry((1u<<30)|(1u<<27)|(11u<<9)|16,(2u<<28)|8,0x1022,0x10000,&m.geometry));
    assert(!ceph_smmu_gmu_plan(streams,16,2,3,8,5,4,0x110000000ULL,0,&m.plan));
    uint32_t off;unsigned width;uint64_t value;
    assert(!ceph_smmu_mmio_offset(&m,CE_SM_TTBR0,&off,&width) && off==0xb020 && width==8);
    assert(!ceph_smmu_mmio_write(&m,CE_SM_TTBR0,m.plan.ttbr0));
    assert(!ceph_smmu_mmio_read(&m,CE_SM_TTBR0,&value) && value==m.plan.ttbr0);
    assert(ceph_smmu_mmio_write(&m,CE_SM_TTBR0,m.plan.ttbr0+4096)<0);
    assert(!ceph_smmu_mmio_write(&m,CE_SM_SMR,m.plan.smr));assert(((uint32_t *)aperture)[0x808/4]==m.plan.smr);
    assert(ceph_smmu_mmio_write(&m,CE_SM_FSR,0xffffffff)<0 && ceph_smmu_mmio_write(&m,CE_SM_REG_COUNT,0)<0);
    assert(ceph_smmu_mmio_read(&m,CE_SM_TLBIASID,&value)<0);
    busy=1;assert(ceph_smmu_mmio_write(&m,CE_SM_S2CR,3)<0);busy=0;
    m.plan.cb=8;assert(ceph_smmu_mmio_read(&m,CE_SM_SCTLR,&value)<0);m.plan.cb=3;
    m.geometry.s2_contexts=4;assert(ceph_smmu_mmio_read(&m,CE_SM_SCTLR,&value)<0);m.geometry.s2_contexts=0;
    m.mapped_bytes=0x8000;assert(ceph_smmu_mmio_read(&m,CE_SM_SCTLR,&value)<0);
    puts("SMMU MMIO adapter: bounded named offsets, exact plan values, 64bit TTBR, lease/refusal PASS (RAM aperture)");
}
