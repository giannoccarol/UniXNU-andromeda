#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_gbif.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p,int keep){assert(keep);++((struct host *)p)->finished;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_gx_gbif *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_gx_gbif){g,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},h,lease,finish,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_gx_gbif s;init(&h,&g,&s);
    assert(!ceph_gx_gbif_configure(&s) && s.configured && s.writes==5 && h.finished==1);
    /* A640 uses a640_gbif: four QSB sides + RBBM QoS, and NOT the a630-only
     * RBBM_VBIF_CLIENT_QOS_CNTL register. */
    for(unsigned w=0x3c03;w<=0x3c06;++w)assert(gpu[w]==0x00071620);
    assert(gpu[0x11]==3 && gpu[0x12]==0);
    /* Nothing may halt/deassert GBIF, reset, or unhalt the CP. */
    assert(gpu[0x808]==3 && !gpu[0x807] && !gpu[0x3c45] && !gpu[0x3c46]);
    unsigned calls=h.calls;
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;assert(ceph_gx_gbif_configure(&s)<0 && !s.configured);
        assert(gpu[0x808]==3 && !gpu[0x3c45]);if(s.writes)assert(h.finished==1);}
    init(&h,&g,&s);s.input.chipid=0x06040000;assert(ceph_gx_gbif_configure(&s)<0 && !h.calls && !s.writes);
    init(&h,&g,&s);g.bytes=0x20000;assert(ceph_gx_gbif_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_gx_gbif_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x823]=1;assert(ceph_gx_gbif_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x1f8d0 - 0x1a800]=0xc0;assert(ceph_gx_gbif_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x23b05 - 0x1a800]=1;assert(ceph_gx_gbif_configure(&s)<0 && !s.writes);
    puts("A640 GBIF block: a640_gbif five exact writes, no a630 QoS reg, retained halt PASS (RAM only)");
}
