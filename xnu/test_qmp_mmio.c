#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_qmp_mmio.h"
struct host {uint32_t ram[1024],irq;unsigned calls,fail,released,uncertain,mode;uint64_t time;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-9:0;}
static int now(void *p,uint64_t *v){struct host *h=p;h->time+=1000000;*v=h->time;
    if(h->irq && !h->mode)h->ram[128/4]=0;
    if(h->irq && h->mode==2)h->ram[5]=0xffff0000;
    return 0;}
static int pause_io(void *p){(void)p;return 0;}
static void release(void *p,int uncertain){struct host *h=p;h->released++;h->uncertain=(unsigned)uncertain;}
static void fixture(struct host *h,struct ceph_qmp_mmio *m)
{
    memset(h,0,sizeof(*h));h->ram[0]=0x4d41494c;h->ram[1]=1;
    for(unsigned c=0;c<2;++c)for(unsigned i=0;i<4;++i)h->ram[3+6*c+i]=0xffff;
    h->ram[7]=64;h->ram[8]=64;h->ram[13]=128;h->ram[14]=128;
    *m=(struct ceph_qmp_mmio){.ram=h->ram,.irq=&h->irq,.ram_pa=0xc300000,.irq_pa=0x17c0000c,
        .bytes=4096,.owner=h,.lease=lease,.now_ns=now,.pause=pause_io,.release=release};
}
int main(void)
{
    struct host h;struct ceph_qmp_mmio m;struct ceph_qmp_ops b;struct ceph_qmp_tx s={0};
    fixture(&h,&m);assert(!ceph_qmp_mmio_bind(&m,1) && !ceph_qmp_mmio_ops(&m,&b));
    assert(!ceph_qmp_send_acd(&s,&b,1) && s.consumed && h.irq==1 && m.writes==9 && h.released==1 && !h.uncertain);
    assert(!memcmp(h.ram+33,"{class: gpu, res: acd, value: 1}",32));
    unsigned calls=h.calls;assert(ceph_qmp_mmio_bind(&m,1)<0);
    for(unsigned fail=1;fail<=calls;++fail) {
        fixture(&h,&m);h.fail=fail;s=(struct ceph_qmp_tx){0};
        int e=ceph_qmp_mmio_bind(&m,1);
        if(!e){assert(!ceph_qmp_mmio_ops(&m,&b));e=ceph_qmp_send_acd(&s,&b,1);}
        assert(e && !s.consumed);if(s.writes)assert(h.uncertain && h.released==1);
    }
    for(unsigned mode=1;mode<=2;++mode) {
        fixture(&h,&m);h.mode=mode;s=(struct ceph_qmp_tx){0};
        assert(!ceph_qmp_mmio_bind(&m,1) && !ceph_qmp_mmio_ops(&m,&b));
        assert(ceph_qmp_send_acd(&s,&b,1)<0 && s.uncertain && h.released==1 && h.uncertain);
    }
    fixture(&h,&m);h.ram[32]=4;assert(ceph_qmp_mmio_bind(&m,1)<0 && h.ram[32]==4 && !h.irq);
    fixture(&h,&m);h.ram[16]=4;assert(ceph_qmp_mmio_bind(&m,1)<0 && h.ram[16]==4);
    fixture(&h,&m);assert(!ceph_qmp_mmio_bind(&m,1));struct ceph_qmp_layout l;
    assert(!ceph_qmp_mmio_begin(&m,&l));
    assert(ceph_qmp_mmio_write(&m,0,0)<0 && ceph_qmp_mmio_write(&m,128,32)<0 &&
           ceph_qmp_mmio_write(&m,132,0)<0 && ceph_qmp_mmio_notify(&m)<0 && !m.writes && !h.irq);
    ceph_qmp_mmio_end(&m,0);
    struct ceph_qmp_descriptor d;struct ceph_qmp_connection c;
    fixture(&h,&m);memcpy(d.words,h.ram,64);assert(!ceph_qmp_descriptor_decode(&d,&c));
    for(unsigned i=0;i<16;++i) {struct ceph_qmp_descriptor bad=d;bad.words[i]=UINT32_MAX;assert(ceph_qmp_descriptor_decode(&bad,&c)<0);}
    d.words[14]=64;assert(ceph_qmp_descriptor_decode(&d,&c)<0);d.words[14]=4092;assert(ceph_qmp_descriptor_decode(&d,&c)<0);
    puts("QMP MMIO: real descriptor/connected checks, exact ACD-only writes, every lease failure, disconnect/timeout retention PASS (RAM only)");
}
