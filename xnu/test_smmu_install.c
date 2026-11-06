#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_smmu_install.h"
struct mock {uint64_t r[CE_SM_REG_COUNT];unsigned call,fail,finish,keep,live,published,tlbi,sync,delays;int timeout;};
static int step(struct mock *m) {return ++m->call==m->fail?-9:0;}
static int begin(void *p,const struct ceph_smmu_plan *plan,uint32_t actlr)
{struct mock *m=p;assert(plan->cb==3 && plan->slot==2 && actlr==0x303);int e=step(m);if(!e)m->live=1;return e;}
static int valid(void *p) {struct mock *m=p;assert(m->live);return step(m);}
static int rd(void *p,enum ceph_smmu_reg r,uint64_t *v) {struct mock *m=p;int e=step(m);if(e)return e;*v=m->r[r];return 0;}
static int wr(void *p,enum ceph_smmu_reg r,uint64_t v)
{
    struct mock *m=p;assert(m->published);
    if(r==CE_SM_TTBR0) assert(m->r[CE_SM_TCR]==0x802020 && m->r[CE_SM_TCR2]==0x38002);
    if(r==CE_SM_TLBIASID) {assert(v==4);m->tlbi=1;}
    if(r==CE_SM_TLBSYNC) {assert(m->tlbi);m->sync=1;m->r[CE_SM_TLBSTATUS]=m->timeout?1:0;}
    if(r==CE_SM_SCTLR)assert(m->sync && !m->r[CE_SM_TLBSTATUS]);
    if(r==CE_SM_SMR)assert(m->r[CE_SM_SCTLR]&1u);
    if(r==CE_SM_S2CR)assert(m->r[CE_SM_SMR]==0x84000005 && m->sync);
    m->r[r]=v;return step(m);
}
static int publish(void *p) {struct mock *m=p;m->published=1;return step(m);}
static int delayus(void *p,unsigned us) {struct mock *m=p;assert(us==1);m->delays++;return step(m);}
static void finish(void *p,int keep) {struct mock *m=p;assert(m->live);m->live=0;m->finish++;m->keep=(unsigned)keep;}
static struct ceph_smmu_ops ops(struct mock *m) {return (struct ceph_smmu_ops){m,begin,valid,rd,wr,publish,valid,delayus,finish};}
int main(void)
{
    struct ceph_smmu_stream streams[8]={0};for(unsigned i=0;i<8;++i)streams[i].s2cr=2u<<16;
    struct ceph_smmu_plan plan;assert(!ceph_smmu_gmu_plan(streams,8,2,3,8,5,4,0x110000000ULL,0,&plan));
    assert(ceph_smmu_plan_canonical(&plan));
    struct mock m={0};m.r[CE_SM_S2CR]=2u<<16;struct ceph_smmu_ops b=ops(&m);struct ceph_smmu_install s={0};
    assert(ceph_smmu_install_context(&s,&b,&plan,0x10)<0 && !m.call && !s.effects);
    assert(!ceph_smmu_install_context(&s,&b,&plan,0x303) && s.state==CE_SM_ENABLED && m.r[CE_SM_S2CR]==3 && m.keep);
    unsigned calls=m.call;
    for(unsigned f=1;f<=calls;++f) {
        m=(struct mock){.fail=f};m.r[CE_SM_S2CR]=2u<<16;b=ops(&m);s=(struct ceph_smmu_install){0};
        assert(ceph_smmu_install_context(&s,&b,&plan,0x303)!=0);
        if(f==1)assert(!m.finish && !s.effects);
        else assert(m.finish==1 && m.keep==(s.effects!=0));
        assert((s.state==CE_SM_QUARANTINED)==(s.effects!=0));
    }
    m=(struct mock){.timeout=1};m.r[CE_SM_S2CR]=2u<<16;b=ops(&m);s=(struct ceph_smmu_install){0};
    assert(ceph_smmu_install_context(&s,&b,&plan,0x303)==-2 && m.delays==1000 && m.r[CE_SM_S2CR]==(2u<<16));
    assert(!(m.r[CE_SM_SCTLR]&1u) && s.state==CE_SM_QUARANTINED);
    m=(struct mock){0};m.r[CE_SM_S2CR]=2u<<16;m.r[CE_SM_FSR]=4;b=ops(&m);s=(struct ceph_smmu_install){0};
    assert(ceph_smmu_install_context(&s,&b,&plan,0x303)<0 && !s.effects && !m.keep);
    plan.sctlr=0;s=(struct ceph_smmu_install){0};assert(ceph_smmu_install_context(&s,&b,&plan,0x303)<0);
    puts("SMMU install: table-before-TTBR, FAULT-before-TRANS, scoped ASID sync, every-call failure quarantine PASS (fake registers)");
}
