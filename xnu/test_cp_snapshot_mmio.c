#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_cp_snapshot_mmio.h"
#include "cepheus_cp_ring_config.h"
#include "cepheus_gx_core.h"
static uint32_t gpu[0x40000/4],gmu[0x30000/4];
struct host {unsigned calls,fail,ended;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void finish(void *p){++((struct host *)p)->ended;}
static void keep(void *p,int kept){(void)p;assert(kept);}
int main(void)
{
    struct host h={0};struct ceph_gmu_mmio g={(volatile uint8_t *)gmu,sizeof(gmu),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,lease};
    gpu[0x808]=3;
    struct ceph_gx_core core={&g,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},&h,lease,keep,0,0,0,0};
    assert(!ceph_gx_core_configure(&core));
    struct ceph_cp_protection protection={&g,gpu,sizeof(gpu),&h,lease,keep,0,0,0,0};
    assert(!ceph_cp_protection_install(&protection));
    struct ceph_cp_ring_config ring={.gmu=&g,.gpu=gpu,.bytes=sizeof(gpu),
        .input={0x100000000ULL,0x100010000ULL,0x100020000ULL,32768,4096,32768},
        .owner=&h,.lease=lease,.publish=lease,.finish=keep};
    assert(!ceph_cp_ring_configure_halted(&ring));
    /* Remaining initialized GX values are test input, not runtime defaults. */
    gpu[0x10]=0x123;gpu[0x11]=0x456;
    struct ceph_cp_snapshot_mmio s={&g,gpu,sizeof(gpu),&h,lease,finish,0};
    uint32_t out[137];memset(out,0xa5,sizeof(out));h.calls=0;
    assert(!ceph_cp_snapshot_mmio_copy(&s,out,137) && out[3]==0x4c0084 && out[5]==0x123 && out[7]==0x456 && out[136]==0xa5a5a5a5);
    assert(gpu[0xe05]==0xffffffc0 && gpu[0xe0b]==0x100000 && gpu[0x8e08]==0x14);
    unsigned calls=h.calls;
    uint32_t packet[12];assert(ceph_cp_me_init(packet,12,0x100030000ULL,4096)==12 && packet[1]==0x16f);
    assert(gpu[0x808]==3 && gpu[0x807]==0); /* generated only, never submitted */
    for(unsigned f=1;f<=calls;++f) {
        h.calls=0;h.fail=f;memset(out,0xa5,sizeof(out));
        assert(ceph_cp_snapshot_mmio_copy(&s,out,137)<0 && !s.active && out[0]==0xa5a5a5a5 && out[135]==0xa5a5a5a5);
    }
    h.fail=0;
    assert(ceph_cp_snapshot_mmio_copy(&s,gpu,136)<0);
    assert(ceph_cp_snapshot_mmio_copy(&s,(uint32_t *)&s,136)<0);
    gpu[0x860]^=1;assert(ceph_cp_snapshot_mmio_copy(&s,out,136)<0);
    gpu[0x860]^=1;assert(!ceph_cp_snapshot_mmio_begin(&s));uint32_t value;
    assert(ceph_cp_snapshot_mmio_read(&s,0x808,&value)<0);ceph_cp_snapshot_mmio_end(&s);
    puts("CP setup chain: protection -> halted ring/SQE -> fixed MMIO restore snapshot -> ME_INIT generation PASS (RAM, no submission)");
}
