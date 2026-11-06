#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gmu_park.h"
struct mock {uint32_t r[CE_PARK_REG_COUNT];unsigned calls,fail,done,parked;uint64_t time;int noack;};
static int step(struct mock *m){return ++m->calls==m->fail?-9:0;}
static int simple(void *p){return step(p);}
static int rd(void *p,enum ceph_gmu_park_reg r,uint32_t *v){struct mock *m=p;*v=m->r[r];return step(m);}
static int wr(void *p,enum ceph_gmu_park_reg r,uint32_t v){struct mock *m=p;assert(r==CE_PARK_SYSRESET || r==CE_PARK_GBIF_HALT);
    if(r==CE_PARK_SYSRESET)for(unsigned i=1;i<=4;++i)assert(m->r[i]&1u);
    if(r==CE_PARK_GBIF_HALT){assert(m->r[CE_PARK_SYSRESET]==1);if(!m->noack)m->r[CE_PARK_GBIF_ACK]=v;}
    m->r[r]=v;return step(m);}
static int now(void *p,uint64_t *n){struct mock *m=p;m->time+=10000;*n=m->time;return step(m);}
static void done(void *p,int parked){struct mock *m=p;m->done++;m->parked=(unsigned)parked;}
static void init(struct mock *m){memset(m,0,sizeof(*m));m->r[CE_PARK_GX_STATUS]=0xc0;for(unsigned i=1;i<=4;++i)m->r[i]=1;}
int main(void)
{
    struct mock m;init(&m);struct ceph_gmu_park_ops b={&m,simple,simple,rd,wr,simple,now,simple,done};struct ceph_gmu_park s={0};
    assert(!ceph_gmu_park_slumbered(&s,&b) && s.parked && m.parked && s.attempted==2);
    unsigned calls=m.calls;
    for(unsigned f=1;f<=calls;++f){init(&m);m.fail=f;s=(struct ceph_gmu_park){0};assert(ceph_gmu_park_slumbered(&s,&b)<0 && !s.parked && !m.parked);}
    init(&m);m.r[CE_PARK_GX_STATUS]=0;s=(struct ceph_gmu_park){0};assert(ceph_gmu_park_slumbered(&s,&b)<0 && !s.attempted);
    init(&m);m.r[CE_PARK_TCS2]=0;s=(struct ceph_gmu_park){0};assert(ceph_gmu_park_slumbered(&s,&b)==-2 && !s.attempted);
    init(&m);m.noack=1;s=(struct ceph_gmu_park){0};assert(ceph_gmu_park_slumbered(&s,&b)==-2 && !s.parked && s.attempted==2);
    puts("GMU park: rejects active GX, drains RPMh TCS, CM3 reset then GBIF ACK, bounded failure retention PASS");
}
