#include <assert.h>
#include <stdio.h>
#include "cepheus_smmu_plan.h"
int main(void)
{
    struct ceph_smmu_stream s[8]={0};for(unsigned i=0;i<8;++i)s[i].s2cr=2u<<16;
    struct ceph_smmu_plan p;
    assert(!ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000000ULL,0,&p));
    assert(p.smr==0x84000005 && p.tcr==0x00802020 && p.tcr2==0x38002);
    assert(p.ttbr0==0x0004000110000000ULL && p.mair0==0x44ff && !(p.sctlr&(1u<<7)));
    assert(ceph_smmu_overlap(0x80000405,5,0x400)); /* masked upper SID variant */
    assert(ceph_smmu_overlap(0x84000004,4,0x400));
    assert(!ceph_smmu_overlap(0x84000004,5,0x400));
    s[0].smr=0x80000405;assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000000ULL,0,&p)==-2);
    s[0].smr=0x80000002;s[0].s2cr=3;assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000000ULL,0,&p)==-2);
    s[0].s2cr=2u<<16;s[2].s2cr=1u<<16;
    assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000000ULL,0,&p)==-2);
    s[2].s2cr=2u<<16;
    assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000000ULL,1,&p)==-1);
    assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,256,0x110000000ULL,0,&p)==-1);
    assert(ceph_smmu_gmu_plan(s,8,2,3,8,5,4,0x110000001ULL,0,&p)==-1);
    assert(ceph_smmu_gmu_plan(s,8,2,3,8,2,4,0x110000000ULL,0,&p)==-1);
    assert(ceph_smmu_gmu_plan(s,8,2,8,8,5,4,0x110000000ULL,0,&p)==-1);
    puts("GMU SMMU plan: SID mask collisions/foreign CB/PA/ASID/TCR/MAIR PASS (not installed)");
}
