#include <assert.h>
#include <stdio.h>
#include "cepheus_smmu_geometry.h"
int main(void)
{
    uint32_t id0=(1u<<30)|(1u<<27)|(11u<<9)|16;
    uint32_t id1=(2u<<28)|(1u<<16)|8,id2=(1u<<12)|(2u<<4)|2;
    struct ceph_smmu_geometry g;assert(!ceph_smmu_geometry(id0,id1,id2,0x10000,&g));
    assert(g.cb_base==0x8000 && g.gr1==0x1000 && g.contexts==8 && g.s2_contexts==1 && g.pa_bits==40);
    assert(g.ipa_bits==40 && g.va_bits>=32);
    uint32_t off;assert(!ceph_smmu_cb_offset(&g,3,0x20,&off) && off==0xb020);
    assert(ceph_smmu_cb_offset(&g,0,0x20,&off)<0 && ceph_smmu_cb_offset(&g,8,0x20,&off)<0);
    assert(ceph_smmu_geometry(id0|(1u<<8),id1,id2,0x10000,&g)<0);
    assert(ceph_smmu_geometry(id0,id1,id2&~(1u<<12),0x10000,&g)<0);
    assert(ceph_smmu_geometry(id0,id1,id2,0x20000,&g)<0);
    assert(ceph_smmu_geometry(id0,id1|0xff,id2,0x10000,&g)<0);
    assert(ceph_smmu_geometry(id0,id1,0x1000,0x10000,&g)<0); /* only32bit PA */
    assert(ceph_smmu_geometry(UINT32_MAX,id1,id2,0x10000,&g)<0);
    puts("SMMU geometry: ID-derived banks/stride/PA/SID, stage2/EXIDS/size mismatch refusal PASS");
}
