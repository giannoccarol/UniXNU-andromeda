#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_retire_mmio.h"
static uint32_t gmu[0x30000/4],gpu[0x40000/4];
struct host {int blocked,silent;uint64_t now;};
static int lease(void *p){return ((struct host *)p)->blocked;}
static int begin(void *p){return ceph_gmu_mmio_park_ready(p);}
static int fence(void *p){if(begin(p))return -1;ceph_gmu_mmio_barrier();return 0;}
static int now(void *p,uint64_t *n){struct ceph_gmu_park_mmio *m=p;struct host *h=m->owner;
    if(gpu[0x3c45]&2 && !h->silent)gpu[0x3c46]|=2;
    h->now+=10000;*n=h->now;return 0;}
static void finish(void *p,int parked){(void)p;(void)parked;}
int main(void)
{
    struct host h={0};struct ceph_gmu_mmio m={(volatile uint8_t *)gmu,sizeof(gmu),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,lease};
    uint32_t v;
    assert(!ceph_gmu_mmio_slumber_write(&m,CE_SL_POWER_COUNTER_ENABLE,0));
    assert(ceph_gmu_mmio_slumber_write(&m,CE_SL_POWER_COUNTER_ENABLE,1)<0);
    assert(ceph_gmu_mmio_slumber_write(&m,CE_SL_RPMH_POWER_STATE,15)<0);
    gmu[0x1f8ec-0x1a800]=15;assert(!ceph_gmu_mmio_slumber_read(&m,CE_SL_RPMH_POWER_STATE,&v) && v==15);
    gmu[0x1f992-0x1a800]=1u<<15;assert(!ceph_gmu_mmio_slumber_read(&m,CE_SL_GMU_FAULT,&v) && v);
    gmu[0x1f992-0x1a800]=0;gmu[0x23b05-0x1a800]=UINT32_MAX;assert(ceph_gmu_mmio_slumber_read(&m,CE_SL_GMU_FAULT,&v)<0);
    gmu[0x23b05-0x1a800]=0;
    struct ceph_gmu_park_mmio pm={&m,gpu,sizeof(gpu),&h,lease};
    assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_SYSRESET,1)<0); /* GX active */
    gmu[0x1f8d0-0x1a800]=0xc0;
    assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_SYSRESET,1)<0); /* TCS not drained */
    const uint32_t tcs[]={0x23746,0x237ee,0x23896,0x2393e};
    for(unsigned i=0;i<4;++i)gmu[tcs[i]-0x1a800]=1;
    assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_GBIF_HALT,2)<0); /* reset not held */
    struct ceph_gmu_park_ops ops={&pm,begin,begin,ceph_gmu_mmio_park_read,ceph_gmu_mmio_park_write,fence,now,begin,finish};
    struct ceph_gmu_park state={0};gpu[0x3c45]=4;
    assert(!ceph_gmu_park_slumbered(&state,&ops) && state.parked && gpu[0x3c45]==6 && gmu[0x1f800-0x1a800]==1);
    assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_GBIF_HALT,2)<0); /* retain other bits */
    assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_GBIF_ACK,2)<0);
    h.blocked=1;assert(ceph_gmu_mmio_park_write(&pm,CE_PARK_SYSRESET,1)<0);
    assert(ceph_gmu_mmio_slumber_write(&m,CE_SL_POWER_COUNTER_ENABLE,0)<0);h.blocked=0;
    gmu[0x1f8ec-0x1a800]=0;assert(ceph_gmu_mmio_park_read(&pm,CE_PARK_GBIF_HALT,&v)<0);gmu[0x1f8ec-0x1a800]=15;
    h.silent=1;gpu[0x3c46]=0;state=(struct ceph_gmu_park){0};
    assert(ceph_gmu_park_slumbered(&state,&ops)==-2 && !state.parked);
    puts("GMU retirement MMIO: exact two-aperture registers, GX-off/TCS/reset gates, halt OR-only and real park engine PASS (RAM)");
}
