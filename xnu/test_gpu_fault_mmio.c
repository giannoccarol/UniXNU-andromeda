#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_fault_mmio.h"
#include "../tests/gpu_smmu_fixture.h"
static uint32_t gmu[0x30000/4],gpu[0x40000/4];static uint64_t smmu[0x10000/8];
struct host {unsigned grant,held,released,calls,fail;};
static int acquire(void *p,unsigned *d){struct host *h=p;assert(!h->held);h->held=1;*d=h->grant;return 0;}
static int lease(void *p,unsigned d){struct host *h=p;assert(h->held && d==h->grant);return ++h->calls==h->fail?-1:0;}
static int okay(void *p){(void)p;return 0;}
static int now(void *p,uint64_t *n){(void)p;*n=10;return 0;}
static void release(void *p){struct host *h=p;assert(h->held);h->held=0;h->released++;}
int main(void)
{
    struct host h={.grant=7};struct ceph_gmu_mmio g={(volatile uint8_t *)gmu,sizeof(gmu),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,okay};
    struct ceph_smmu_stream streams[16]={0};for(unsigned i=0;i<16;++i)streams[i].s2cr=2u<<16;
    struct ceph_smmu_plan p;assert(!ceph_smmu_gmu_plan(streams,16,2,3,8,5,4,0x120000000ULL,0,&p));
    struct ceph_smmu_mmio m;smmu_fixture_init(&m,smmu,&p,&h,okay);
    struct ceph_gpu_fault_mmio f={&g,&m,gpu,sizeof(gpu),&h,acquire,lease,now,release,0,0};
    struct ceph_gpu_fault_record out;
    assert(!ceph_gpu_fault_mmio_snapshot(&f,&out) && !ceph_gpu_fault_status(&out) && h.released==1);
    assert(!ceph_gpu_fault_domains_status(&out,6) && ceph_gpu_fault_domains_status(&out,8)<0 && ceph_gpu_fault_domains_status(&out,0)<0);
    unsigned calls=h.calls;
    for(unsigned fail=1;fail<=calls;++fail) {
        h.calls=0;h.fail=fail;assert(ceph_gpu_fault_mmio_snapshot(&f,&out)<0 && !out.domains && !h.held);
    }
    h.fail=0;gpu[0x823]=20;gpu[0x821]=123;gpu[0x824]=456;
    smmu_fixture_set(&m,CE_SM_FSR,2);*(uint32_t *)((uint8_t *)smmu+0xb068)=7;*(uint64_t *)((uint8_t *)smmu+0xb060)=0x123456789ULL;
    assert(!ceph_gpu_fault_mmio_snapshot(&f,&out) && ceph_gpu_fault_status(&out)==1);
    assert(out.cp_hw==123 && out.cp_protect==456 && out.far==0x123456789ULL && out.fsynr0==7 && out.fsr==2);
    assert(gpu[0x823]==20 && *(uint32_t *)((uint8_t *)smmu+0xb058)==2); /* no clears */
    gmu[0x1f8d0-0x1a800]=0xc0;assert(ceph_gpu_fault_mmio_snapshot(&f,&out)<0); /* GX read forbidden */
    h.grant=CE_FAULT_DOMAIN_GMU;f.gpu=NULL;f.smmu=NULL;
    assert(!ceph_gpu_fault_mmio_snapshot(&f,&out) && out.domains==2 && ceph_gpu_fault_status(&out)==-1);
    assert(ceph_gpu_fault_domains_status(&out,6)==-1 && !ceph_gpu_fault_domains_status(&out,2));
    assert(ceph_gpu_fault_mmio_gmu_health(&f)==-1); /* missing SMMU not waived */
    gmu[0x23b05-0x1a800]=1;assert(!ceph_gpu_fault_mmio_snapshot(&f,&out) && ceph_gpu_fault_status(&out)==1);
    gmu[0x23b05-0x1a800]=UINT32_MAX;assert(ceph_gpu_fault_mmio_snapshot(&f,&out)<0);
    assert(!h.held);
    puts("Fault MMIO: source offsets/64bit FAR/domain leases/GX-off refusal/missing != clean/no clears PASS (RAM)");
}
