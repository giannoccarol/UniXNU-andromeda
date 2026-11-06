#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_cp_ring_config.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,published,finished,keep;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-9:0;}
static int publish(void *p){struct host *h=p;h->published=1;return lease(p);}
static void finish(void *p,int keep){struct host *h=p;h->finished++;h->keep=(unsigned)keep;}
static void init(struct host *h,struct ceph_gmu_mmio *g,struct ceph_cp_ring_config *s)
{
    *h=(struct host){0};memset(gpu,0,sizeof(gpu));memset(gmu,0,sizeof(gmu));gpu[0x808]=3;
    for(unsigned i=0;i<45;++i){uint32_t reg,value;assert(!ceph_cp_protection_word(i,&reg,&value));gpu[reg]=value;}
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *s=(struct ceph_cp_ring_config){.gmu=g,.gpu=gpu,.bytes=sizeof(gpu),
        .input={0x100000000ULL,0x100010000ULL,0x100020000ULL,32768,4096,32768},
        .owner=h,.lease=lease,.publish=publish,.finish=finish};
}
int main(void)
{
    struct host h;struct ceph_gmu_mmio g;struct ceph_cp_ring_config s;init(&h,&g,&s);
    assert(!ceph_cp_ring_configure_halted(&s) && s.configured && s.writes==7 && h.published && h.finished && h.keep);
    assert(gpu[0x802]==0x0800020c && gpu[0x805]==1 && gpu[0x831]==1 && gpu[0x808]==3 && !gpu[0x807]);
    unsigned calls=h.calls;assert(ceph_cp_ring_configure_halted(&s)<0);
    for(unsigned f=1;f<=calls;++f) {
        init(&h,&g,&s);h.fail=f;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.configured);
        assert(gpu[0x808]==3 && !gpu[0x807]);if(s.writes)assert(h.keep && h.finished);
    }
    init(&h,&g,&s);gpu[0x808]=1;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes && !h.published);
    init(&h,&g,&s);gpu[0x800]=1;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes && gpu[0x800]==1);
    init(&h,&g,&s);gpu[0x842]=0;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes);
    init(&h,&g,&s);gpu[0x860]^=1;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x1f8d0-0x1a800]=0xc0;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x1f8d0-0x1a800]=4;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes);
    init(&h,&g,&s);gmu[0x23b10-0x1a800]=1;assert(ceph_cp_ring_configure_halted(&s)<0 && !s.writes && gmu[0x23b10-0x1a800]==1);
    init(&h,&g,&s);s.input.sqe_va=s.input.ring_va;assert(ceph_cp_ring_configure_halted(&s)<0 && !h.calls);
    init(&h,&g,&s);s.input.sqe_va=(1ULL<<40)-4096;assert(ceph_cp_ring_configure_halted(&s)<0);
    init(&h,&g,&s);s.input.rptr_bytes=1;assert(ceph_cp_ring_configure_halted(&s)<0);
    init(&h,&g,&s);s.input.ring_va=0x100000;assert(ceph_cp_ring_configure_halted(&s)<0 && !h.calls);
    init(&h,&g,&s);s.input.sqe_va=1ULL<<39;assert(ceph_cp_ring_configure_halted(&s)<0 && !h.calls);
    puts("CP ring preconfiguration: exact7 address/control writes, halt retained, no WPTR/trust/start, every lease failure PASS (RAM)");
}
