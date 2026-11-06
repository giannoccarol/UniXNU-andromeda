#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_scm_discovery.h"
struct host {unsigned calls,leases,fail,missing,fallback,resumes;int busy,transport_fail,interrupt_loop,malformed_self;};
static int lease(void *p){struct host *h=p;return ++h->leases==h->fail?-1:0;}
static int invoke(void *p,struct ceph_scm_regs *r)
{
    struct host *h=p;++h->calls;uint32_t fn=(uint32_t)r->x[0];
    if(h->transport_fail)return -1;
    if(h->malformed_self && (fn&~0xc0000000u)==0x02000601u && (fn&0x80000000u)) {r->x[0]=0;r->x[1]=2;return 0;}
    if(h->busy){r->x[0]=(uint64_t)(int64_t)CE_SCM_BUSY;return 0;}
    if(h->interrupt_loop){r->x[0]=1;return 0;}
    if(h->resumes==1){assert(fn==1 && r->x[6]==0x13579);h->resumes=2;}
    else {
        assert((fn&~0xc0000000u)==0x02000601u || (fn&~0xc0000000u)==0x02000207u);
        assert(r->x[1]==1 && !r->x[3] && !r->x[4] && !r->x[5] && !r->x[7]);
        if(h->fallback && fn==0xc2000601u){r->x[0]=UINT64_MAX;r->x[1]=0;return 0;}
        if(!h->resumes && h->missing==99){r->x[0]=1;r->x[6]=0x13579;h->resumes=1;return 0;}
    }
    r->x[0]=0;r->x[1]=h->calls==h->missing?0:1;return 0;
}
int main(void)
{
    struct ceph_scm_regs r;
    assert(!ceph_scm_query_regs(CE_SCM_ARMV8_64,CE_SCM_SELF,&r) && r.x[0]==0xc2000601u && r.x[2]==0x82000601u);
    assert(!ceph_scm_query_regs(CE_SCM_ARMV8_32,CE_SCM_PAS_INIT_AVAILABLE,&r) && r.x[0]==0x02000601u && r.x[2]==0x02000201u);
    assert(!ceph_scm_query_regs(CE_SCM_ARMV8_64,CE_SCM_PAS_SUPPORTED,&r) && r.x[0]==0x42000207u && r.x[2]==13);
    assert(ceph_scm_query_regs(CE_SCM_UNKNOWN,CE_SCM_SELF,&r)<0);
    assert(ceph_scm_query_regs(CE_SCM_ARMV8_64,(enum ceph_scm_query)99,&r)<0);
    struct host h={0};struct ceph_scm_discovery s={0};struct ceph_scm_discovery_ops b={&h,lease,invoke};
    assert(!ceph_scm_discover_pas13(&s,&b) && s.convention==CE_SCM_ARMV8_64 && s.available_mask==15 && s.supported && h.calls==6);
    assert(ceph_scm_discover_pas13(&s,&b)<0 && h.calls==6);
    h=(struct host){.fallback=1};s=(struct ceph_scm_discovery){0};
    assert(!ceph_scm_discover_pas13(&s,&b) && s.convention==CE_SCM_ARMV8_32 && h.calls==7);
    for(unsigned f=1;f<=6;++f){h=(struct host){.fail=f};s=(struct ceph_scm_discovery){0};
        assert(ceph_scm_discover_pas13(&s,&b)<0 && !s.supported && h.calls<f);}
    for(unsigned q=2;q<=6;++q){h=(struct host){.missing=q};s=(struct ceph_scm_discovery){0};
        assert(ceph_scm_discover_pas13(&s,&b)<0 && !s.supported && h.calls==q);}
    h=(struct host){.busy=1};s=(struct ceph_scm_discovery){0};assert(ceph_scm_discover_pas13(&s,&b)<0 && h.calls==1);
    h=(struct host){.transport_fail=1};s=(struct ceph_scm_discovery){0};assert(ceph_scm_discover_pas13(&s,&b)<0 && h.calls==1);
    h=(struct host){.interrupt_loop=1};s=(struct ceph_scm_discovery){0};assert(ceph_scm_discover_pas13(&s,&b)<0 && h.calls==16);
    h=(struct host){.missing=99};s=(struct ceph_scm_discovery){0};assert(!ceph_scm_discover_pas13(&s,&b) && h.resumes==2 && h.calls==7);
    h=(struct host){.malformed_self=1};s=(struct ceph_scm_discovery){0};
    assert(ceph_scm_discover_pas13(&s,&b)<0 && h.calls==1 && s.convention==CE_SCM_UNKNOWN);
    h=(struct host){0};s=(struct ceph_scm_discovery){.available_mask=15};
    assert(ceph_scm_discover_pas13(&s,&b)<0 && !h.calls);
    s=(struct ceph_scm_discovery){.supported=1};assert(ceph_scm_discover_pas13(&s,&b)<0 && !h.calls);
    puts("SCM PAS13 discovery: fixed info-only queries, ABI fallback, x6 resume, fail-closed budget PASS (no SMC executed)");
}
