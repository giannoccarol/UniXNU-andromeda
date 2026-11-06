#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gx_hwcg.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,finished;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p,int keep){assert(keep);++((struct host *)p)->finished;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_gx_hwcg *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_gx_hwcg){g,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},h,lease,finish,0,0,0,0,0};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_gx_hwcg s;init(&h,&g,&s);
    /* Cold: top-level gate is off, so the full sequence runs.
     * 3 GMU CGC + SPTPRAC off + 49 table + SPTPRAC on + top = 55 writes. */
    assert(!ceph_gx_hwcg_configure(&s) && s.configured && !s.skipped && s.writes==55 && h.finished==1);
    assert(gmu[0x23b09 - 0x1a800]==0x00020202 && gmu[0x23b0a - 0x1a800]==0x00010111);
    assert(gmu[0x23b0b - 0x1a800]==0x00005555);
    assert(gpu[0xae]==0x8aa8aa82 && gpu[0xb0]==0x02222222 && gpu[0x11a]==0x00000555);
    /* SPTPRAC clock ends enabled (bit0 set) exactly as the source restores it. */
    assert(gmu[0x1a880 - 0x1a800]==1u);
    /* Never unhalts, resets or touches GBIF halt. */
    assert(gpu[0x808]==3 && !gpu[0x807] && !gpu[0x3c45]);
    /* Early-out: already-on HWCG must not re-program or toggle SPTPRAC. */
    init(&h,&g,&s);gpu[0xae]=0x8aa8aa82;gmu[0x1a880 - 0x1a800]=1u;
    assert(!ceph_gx_hwcg_configure(&s) && s.skipped && s.writes==3 && !s.error);
    assert(gpu[0xb0]==0 && gmu[0x1a880 - 0x1a800]==1u);
    /* Every lease failure point leaves halt intact. */
    unsigned calls=0;
    {struct host h2;struct ceph_gmu_mmio g2;struct ceph_gx_hwcg s2;init(&h2,&g2,&s2);assert(!ceph_gx_hwcg_configure(&s2));calls=h2.calls;}
    for(unsigned f=1;f<=calls;++f){init(&h,&g,&s);h.fail=f;assert(ceph_gx_hwcg_configure(&s)<0 && !s.configured);
        assert(gpu[0x808]==3 && !gpu[0x807]);}
    init(&h,&g,&s);s.input.chipid=0x06040000;assert(ceph_gx_hwcg_configure(&s)<0 && !h.calls && !s.writes);
    init(&h,&g,&s);g.bytes=0x20000;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x823]=1;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x1f8d0 - 0x1a800]=0xc0;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x23b05 - 0x1a800]=1;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    /* A truncated GPU aperture is refused before any write, not part-way. */
    init(&h,&g,&s);s.bytes=0x1000;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    /* A misaligned aperture is equally refused. */
    init(&h,&g,&s);s.gpu=gpu+1;assert(ceph_gx_hwcg_configure(&s)<0 && !s.writes);
    puts("A640 HWCG: 55 exact writes, GMU-AO+SPTPRAC gate ordering, already-on early-out, retained halt PASS (RAM only)");
}
