#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_smmu_plan.h"
#include "cepheus_smmu_install.h"
int main(void)
{
    struct ceph_smmu_geometry g;assert(!ceph_smmu_geometry((1u<<30)|(1u<<27)|(11u<<9)|16,(2u<<28)|8,0x1222,0x10000,&g));
    struct ceph_smmu_stream streams[16]={0};for(unsigned i=0;i<16;++i)streams[i].s2cr=2u<<16;
    streams[2]=(struct ceph_smmu_stream){0x84000005,3}; /* separate live GMU kernel */
    struct ceph_gx_smmu_plan p;
    assert(!ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p));
    assert(p.context.smr==0x84010000 && p.context.tcr==0x802019 && p.actlr==0x303);
    assert(!ceph_smmu_plan_canonical(&p.context)); /* GMU installer MUST reject */
    for(unsigned sid=0;sid<0x800;++sid)
        assert(ceph_smmu_overlap(p.context.smr,sid,0)==(sid==0 || sid==1 || sid==0x400 || sid==0x401));
    struct ceph_gx_smmu_plan saved=p;
    g.va_bits=32;assert(ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p)<0);g.va_bits=40;
    g.ipa_bits=36;assert(ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p)<0);g.ipa_bits=40;
    assert(ceph_gx_smmu_plan(streams,16,&g,0,3,2,0x120000000ULL,0,&p)<0 && !memcmp(&saved,&p,sizeof(p)));
    streams[1]=(struct ceph_smmu_stream){0x80000401,4};assert(ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p)<0);
    streams[1]=(struct ceph_smmu_stream){0x84000002,4}; /* secure stream nonoverlap still preserved */
    assert(!ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p));
    assert(ceph_gx_smmu_plan(streams,16,&g,1,1,2,0x120000000ULL,0,&p)<0);
    assert(ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,1,&p)<0);
    g.s2_contexts=2;assert(ceph_gx_smmu_plan(streams,16,&g,0,1,2,0x120000000ULL,0,&p)<0);
    puts("GX SMMU pure plan: separate SID0/mask401/39bit, GMU and secure streams preserved, GMU installer rejection PASS (not installed)");
}
