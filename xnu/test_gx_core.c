#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_core.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p,int keep){assert(keep);++((struct host *)p)->finished;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_gx_core *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_gx_core){g,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},h,lease,finish,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_gx_core s;init(&h,&g,&s);
    assert(!ceph_gx_core_configure(&s) && s.configured && s.writes==23 && h.finished==1);
    assert(gpu[0xe0b]==0x100000 && gpu[0xe0d]==0x1fffff && gpu[0x8e08]==0x14 && gpu[0xe01]==0x400000);
    assert(gpu[0x808]==3 && !gpu[0x807]);unsigned calls=h.calls;
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;assert(ceph_gx_core_configure(&s)<0 && !s.configured);
        assert(gpu[0x808]==3 && !gpu[0x807]);if(s.writes)assert(h.finished==1);}
    init(&h,&g,&s);s.input.gmem_bytes=0x200000;assert(ceph_gx_core_configure(&s)<0 && !h.calls);
    init(&h,&g,&s);s.input.ubwc_mode=2;assert(ceph_gx_core_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x1f8d0-0x1a800]=0xc0;assert(ceph_gx_core_configure(&s)<0 && !s.writes);
    puts("A640 GX core subset: UCHE/1MiB GMEM/ROQ/UBWC3 exact23 writes, retained halt/every lease failure PASS (RAM only)");
}
