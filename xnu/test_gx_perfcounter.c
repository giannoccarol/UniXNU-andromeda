#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_perfcounter.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p,int keep){assert(keep);++((struct host *)p)->finished;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_gx_perfcounter *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    /* Pre-seed SELECT_0 so the masked RMW is observable: only bits7:0 change. */
    gmu[0x1f841 - 0x1a800]=0xffffff00u;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_gx_perfcounter){g,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},h,lease,finish,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_gx_perfcounter s;init(&h,&g,&s);
    assert(!ceph_gx_perf_configure(&s) && s.configured && s.writes==5 && h.finished==1);
    assert(gpu[0x50b]==0xffffffffu && gpu[0x500]==1u);
    assert(gmu[0x23b0e - 0x1a800]==0xff000000u && gmu[0x1f840 - 0x1a800]==1u);
    /* Masked RMW keeps the untouched upper bits instead of clobbering them. */
    assert(gmu[0x1f841 - 0x1a800]==0xffffff20u);
    /* Nothing here may unhalt, reset or repower the engine. */
    assert(gpu[0x808]==3 && !gpu[0x807] && !gpu[0x800] && !gmu[0x1f800 - 0x1a800]);
    unsigned calls=h.calls;
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;assert(ceph_gx_perf_configure(&s)<0 && !s.configured);
        assert(gpu[0x808]==3 && !gpu[0x807]);if(s.writes)assert(h.finished==1);}
    /* A wrong chip identity is refused before any lease or write. */
    init(&h,&g,&s);s.core.chipid=0x06040000;assert(ceph_gx_perf_configure(&s)<0 && !h.calls && !s.writes);
    /* Aperture mismatch is refused rather than silently addressing the wrong block. */
    init(&h,&g,&s);g.bytes=0x10000;assert(ceph_gx_perf_configure(&s)<0 && !s.writes);
    /* CP no longer halted (halt bit1 clear): refuse even if the caller insists. */
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_gx_perf_configure(&s)<0 && !s.writes);
    /* A CP fault register set is equally disqualifying while halted. */
    init(&h,&g,&s);gpu[0x823]=1;assert(ceph_gx_perf_configure(&s)<0 && !s.writes);
    /* GMU-side fault must also block the GPU-aperture writes. */
    init(&h,&g,&s);gmu[0x23b05 - 0x1a800]=1;assert(ceph_gx_perf_configure(&s)<0 && !s.writes);
    puts("A640 GX perfcounter/busy block: 5 exact writes across GPU+GMU apertures, masked SELECT_0, retained halt PASS (RAM only)");
}
