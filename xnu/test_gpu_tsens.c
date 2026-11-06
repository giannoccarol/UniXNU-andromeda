#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_tsens.h"
static uint32_t ram[2][1024];
static uint32_t srot[2][1024];
struct host {unsigned calls,fail;uint64_t now,step;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static int now(void *p,uint64_t *v){struct host *h=p;h->now+=h->step;*v=h->now;return 0;}
int main(void)
{
    struct host h={.step=1000};struct ceph_gpu_tsens m={{ram[0],ram[1]},{4096,4096},{0xc263000,0xc265000},&h,lease,now,
        {srot[0],srot[1]},{4096,4096},{0xc222000,0xc223000}};
    srot[0][1]=1|(1u<<18);srot[1][1]=1|(1u<<14);
    struct ceph_gpu_temperature s;ram[0][0xe4/4]=ram[1][0xe4/4]=8;
    ram[0][0xdc/4]=(1u<<21)|400;ram[1][0xcc/4]=(1u<<21)|420;
    assert(!ceph_gpu_tsens_sample(&m,&s) && s.valid_mask==3 && s.gpuss0_mc==40000 && s.gpuss1_mc==42000 && s.sampled_ns==1000);
    unsigned calls=h.calls;
    srot[1][1]=1;assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask);srot[1][1]=1|(1u<<14);
    srot[0][1]=1u<<18;assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask);srot[0][1]=1|(1u<<18);
    ram[0][0x14/4]=1u<<31;assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask && ram[0][0x14/4]==(1u<<31));ram[0][0x14/4]=0;
    ram[1][0x14/4]=1u<<11;assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask);ram[1][0x14/4]=0;
    for(unsigned fail=1;fail<=calls;++fail){h=(struct host){.step=1000,.fail=fail};assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask);}
    h=(struct host){.step=1000};ram[0][0xdc/4]=400;assert(ceph_gpu_tsens_sample(&m,&s)<0 && !s.valid_mask);
    ram[0][0xdc/4]=(1u<<21)|0xf9c;assert(!ceph_gpu_tsens_sample(&m,&s) && s.gpuss0_mc==-10000);
    ram[0][0xdc/4]=(1u<<21)|600;assert(!ceph_gpu_tsens_sample(&m,&s));
    struct ceph_gpu_thermal t={0};assert(!ceph_gpu_thermal_start(&t,h.now,30000000000ULL));
    assert(ceph_gpu_thermal_check(&t,&s,h.now)==CE_THERMAL_HOT);
    ram[1][0xe4/4]=0;assert(ceph_gpu_tsens_sample(&m,&s)<0);
    ram[1][0xe4/4]=UINT32_MAX;assert(ceph_gpu_tsens_sample(&m,&s)<0);ram[1][0xe4/4]=8;
    ram[1][0xcc/4]=UINT32_MAX;assert(ceph_gpu_tsens_sample(&m,&s)<0);ram[1][0xcc/4]=(1u<<21)|420;
    h=(struct host){.step=100000001};assert(ceph_gpu_tsens_sample(&m,&s)<0);
    m.pa[1]++;assert(ceph_gpu_tsens_sample(&m,&s)<0);m.pa[1]--;m.tm[1]=m.tm[0];assert(ceph_gpu_tsens_sample(&m,&s)<0);
    int mc;assert(ceph_gpu_tsens_decode((1u<<21)|1251,&mc)<0 && ceph_gpu_tsens_decode((1u<<21)|0x800,&mc)<0);
    puts("TSENS GPU sampler: FDT gpuss0=TSENS0:15/gpuss1=TSENS1:11, dual-valid/leases/sample-age PASS (RAM)");
}
