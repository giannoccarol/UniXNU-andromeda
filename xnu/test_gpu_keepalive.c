/* Source-backed engines on one RAM aperture. No claim of actual GX startup. */
#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_mmio.h"
#include "cepheus_gx_core.h"
#include "cepheus_cp_protection.h"
#include "cepheus_cp_ring_config.h"
static uint32_t gmu[0x30000/4],gpu[0x40000/4];
struct host {struct ceph_gmu_mmio mmio;struct ceph_gmu_oob oob;uint64_t time;int busy;};
static int lease(void *p){(void)p;return 0;}
static int oob_begin(void *p){struct host *h=p;return ceph_gmu_mmio_ready(&h->mmio);}
static int oob_read(void *p,enum ceph_oob_reg r,uint32_t *v){return ceph_gmu_mmio_oob_read(&((struct host *)p)->mmio,r,v);}
static int oob_write(void *p,enum ceph_oob_reg r,uint32_t v){struct host *h=p;
    int e=ceph_gmu_mmio_oob_write(&h->mmio,r,v);if(e)return e;
    if(r==CE_OOB_H2G_SET && v==h->oob.bits.request){gmu[0x1f992-0x1a800]|=h->oob.bits.ack;gmu[0x1f8d0-0x1a800]=0;}
    if(r==CE_OOB_G2H_CLEAR)gmu[0x1f992-0x1a800]&=~v;
    return 0;}
static int barrier(void *p){(void)p;ceph_gmu_mmio_barrier();return 0;}
static int now(void *p,uint64_t *v){struct host *h=p;h->time+=1000;*v=h->time;return 0;}
static int drained(void *p){struct host *h=p;return h->busy || gpu[0x808]!=3 || gpu[0x806]!=gpu[0x807]?-1:0;}
static void end(void *p,int keep){(void)p;assert(keep);}
static int gx_lease(void *p){struct host *h=p;return h->oob.state==CE_OOB_HELD?0:-1;}
int main(void)
{
    struct host h={0};h.mmio=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,lease};
    gmu[0x1f8d0-0x1a800]=0xc0;gpu[0x808]=3;
    struct ceph_oob_ops ops={&h,oob_begin,lease,oob_read,oob_write,barrier,now,lease,drained,end};
    struct ceph_gx_core core={&h.mmio,gpu,sizeof(gpu),{0x06040001,0x100000,0x100000,15,32,3},&h,gx_lease,end,0,0,0,0};
    assert(ceph_gx_core_configure(&core)<0 && !core.writes);
    assert(!ceph_gmu_oob_acquire(&h.oob,&ops,h.mmio.hfi_version) && h.oob.state==CE_OOB_HELD);
    assert(!ceph_gx_core_configure(&core));
    struct ceph_cp_protection protect={&h.mmio,gpu,sizeof(gpu),&h,gx_lease,end,0,0,0,0};
    assert(!ceph_cp_protection_install(&protect));
    struct ceph_cp_ring_config ring={.gmu=&h.mmio,.gpu=gpu,.bytes=sizeof(gpu),
        .input={0x100000000ULL,0x100010000ULL,0x100020000ULL,32768,4096,32768},
        .owner=&h,.lease=gx_lease,.publish=barrier,.finish=end};
    assert(!ceph_cp_ring_configure_halted(&ring));
    h.busy=1;assert(ceph_gmu_oob_release(&h.oob,&ops)<0 && h.oob.state==CE_OOB_HELD);
    h.busy=0;assert(!ceph_gmu_oob_release(&h.oob,&ops) && h.oob.state==CE_OOB_RELEASED);
    assert(gx_lease(&h)<0 && gpu[0x808]==3 && !gpu[0x807]);
    /* No assertion that release turns GX off: needs slumber/park separately. */
    puts("OOB keepalive -> GX subset -> CP protection/ring -> drained release chain PASS (RAM, no execution/turnoff claim)");
}
