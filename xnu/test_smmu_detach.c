#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_smmu_detach.h"
struct mock {uint64_t r[CE_SM_REG_COUNT];unsigned call,fail,parked,done,detached,delay,tlbi;int timeout;};
static int step(struct mock *m){return ++m->call==m->fail?-9:0;}
static int park(void *p,const struct ceph_smmu_plan *plan){struct mock *m=p;assert(plan->cb==3);int e=step(m);if(!e)m->parked=1;return e;}
static int parked(void *p){struct mock *m=p;assert(m->parked);return step(m);}
static int rd(void *p,enum ceph_smmu_reg r,uint64_t *v){struct mock *m=p;*v=m->r[r];return step(m);}
static int wr(void *p,enum ceph_smmu_reg r,uint64_t v){struct mock *m=p;assert(m->parked);
    if(r==CE_SM_SCTLR)assert(m->r[CE_SM_S2CR]==(2u<<16));
    if(r==CE_SM_TLBIASID){assert(!(m->r[CE_SM_SCTLR]&1));m->tlbi=1;}
    if(r==CE_SM_TLBSYNC){assert(m->tlbi);m->r[CE_SM_TLBSTATUS]=m->timeout?1:0;}
    if(r==CE_SM_SMR)assert(m->tlbi && !m->r[CE_SM_TLBSTATUS] && !(v&(1u<<31)));
    m->r[r]=v;return step(m);}
static int delayus(void *p,unsigned us){struct mock *m=p;assert(us==1);m->delay++;return step(m);}
static void finish(void *p,int detached){struct mock *m=p;assert(m->parked);m->done++;m->detached=(unsigned)detached;}
static void init(struct mock *m,const struct ceph_smmu_plan *p){memset(m,0,sizeof(*m));
    m->r[CE_SM_SMR]=p->smr;m->r[CE_SM_S2CR]=p->s2cr_translate;m->r[CE_SM_SCTLR]=p->sctlr;
    m->r[CE_SM_TTBR0]=p->ttbr0;m->r[CE_SM_TTBR1]=p->ttbr1;m->r[CE_SM_TCR]=p->tcr;m->r[CE_SM_TCR2]=p->tcr2;
    m->r[CE_SM_MAIR0]=p->mair0;m->r[CE_SM_MAIR1]=p->mair1;m->r[CE_SM_CBAR]=p->cbar;m->r[CE_SM_CBA2R]=p->cba2r;
    m->r[CE_SM_ACTLR]=0x303;
}
int main(void)
{
    struct ceph_smmu_stream streams[8]={0};for(unsigned i=0;i<8;++i)streams[i].s2cr=2u<<16;
    struct ceph_smmu_plan p;assert(!ceph_smmu_gmu_plan(streams,8,2,3,8,5,4,0x110000000ULL,0,&p));
    struct mock m;init(&m,&p);struct ceph_smmu_detach_ops b={&m,park,parked,rd,wr,parked,delayus,finish};struct ceph_smmu_detach s={0};
    assert(!ceph_smmu_detach_context(&s,&b,&p) && s.state==CE_DET_DETACHED && m.detached);
    unsigned calls=m.call;
    for(unsigned f=1;f<=calls;++f){init(&m,&p);m.fail=f;s=(struct ceph_smmu_detach){0};
        assert(ceph_smmu_detach_context(&s,&b,&p)<0 && s.state==CE_DET_FAILED && !m.detached);
        assert(m.done==(f==1?0u:1u));}
    init(&m,&p);m.timeout=1;s=(struct ceph_smmu_detach){0};assert(ceph_smmu_detach_context(&s,&b,&p)==-2 && m.delay==1000 && !m.detached);
    init(&m,&p);m.r[CE_SM_TTBR0]^=0x1000;s=(struct ceph_smmu_detach){0};
    assert(ceph_smmu_detach_context(&s,&b,&p)<0 && !s.effects);
    init(&m,&p);m.r[CE_SM_ACTLR]=0x302;s=(struct ceph_smmu_detach){0};
    assert(ceph_smmu_detach_context(&s,&b,&p)<0 && !s.effects && !m.detached);
    init(&m,&p);m.r[CE_SM_FSR]=2;s=(struct ceph_smmu_detach){0};
    assert(ceph_smmu_detach_context(&s,&b,&p)<0 && !m.detached);
    puts("SMMU detach: parked-before-FAULT, context identity, scoped sync, no free on failure PASS (fake registers)");
}
