#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_cp_protection.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished,keep;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-9:0;}
static void finish(void *p,int keep){struct host *h=p;h->finished++;h->keep=(unsigned)keep;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_cp_protection *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_cp_protection){g,gpu,sizeof(gpu),h,lease,finish,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_cp_protection s;init(&h,&g,&s);
    assert(!ceph_cp_protection_install(&s) && s.configured && s.writes==45 && h.keep);
    assert(gpu[0x842]==1 && gpu[0xf810]==1 && gpu[0x84f]==11 && gpu[0x86f]==0x80011c00 && gpu[0x808]==3 && !gpu[0x807]);
    unsigned calls=h.calls;
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;assert(ceph_cp_protection_install(&s)<0 && !s.configured);
        if(s.writes)assert(h.finished && h.keep);
        assert(gpu[0x808]==3 && !gpu[0x807]);}
    init(&h,&g,&s);gmu[0x1f8d0-0x1a800]=0xc0;assert(ceph_cp_protection_install(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_cp_protection_install(&s)<0 && !s.writes);
    init(&h,&g,&s);assert(!ceph_cp_protection_install(&s));gpu[0xf810]=0;
    assert(ceph_cp_protection_check(&g,gpu,sizeof(gpu),&h,lease)<0);
    puts("A640 protection setup:12 address modes/32 spans/enabled control, CP remains halted, every lease failure PASS (RAM)");
}
