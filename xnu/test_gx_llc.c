#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_llc.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p,int keep){assert(keep);++((struct host *)p)->finished;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_gx_llc *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    gpu[0x3c02]=0xd6000000u; /* retain XBL's HTW/no-override high bits */
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_gx_llc){g,gpu,sizeof(gpu),0x06040001,12,h,lease,finish,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_gx_llc s;init(&h,&g,&s);
    assert(!ceph_gx_llc_configure(&s) && s.configured && s.writes==1 && h.finished==1);
    for(unsigned i=0;i<5;++i)assert(((gpu[0x3c02]>>(i*5))&31)==12);
    assert((gpu[0x3c02]&0xfe000000u)==0xd6000000u);
    assert(!gpu[0x3c01] && gpu[0x808]==3 && !gpu[0x807] && !gpu[0x3c45]);
    unsigned calls=h.calls;assert(ceph_gx_llc_configure(&s)<0 && h.calls==calls);
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;
        assert(ceph_gx_llc_configure(&s)<0 && !s.configured);
        if(s.writes)assert(h.finished==1);
        assert((gpu[0x3c02]&0xfe000000u)==0xd6000000u && gpu[0x808]==3);}
    init(&h,&g,&s);s.gpu_scid=11;assert(ceph_gx_llc_configure(&s)<0 && !h.calls && !s.writes);
    init(&h,&g,&s);s.chipid=0x06030000;assert(ceph_gx_llc_configure(&s)<0 && !h.calls);
    init(&h,&g,&s);s.bytes=0x1000;assert(ceph_gx_llc_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x3c02]=UINT32_MAX;assert(ceph_gx_llc_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_gx_llc_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x823]=1;assert(ceph_gx_llc_configure(&s)<0 && !s.writes);
    puts("A640 LLC: five SCID12 fields, HTW/XBL bits preserved, no MMU-500 override, lease failures PASS (RAM only)");
}
