#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_qmp_connect.h"
struct host {uint32_t ram[1024],irq;unsigned calls,fail,release,uncertain,mode;uint64_t time;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-9:0;}
static int now(void *p,uint64_t *n){struct host *h=p;h->time+=1000000;*n=h->time;return 0;}
static int pause_io(void *p){struct host *h=p;
    if(!h->irq || h->mode==1)return 0;
    h->irq=0;
    if(h->ram[9]==CE_QMP_UP)h->ram[10]=CE_QMP_UP;
    if(h->ram[11]==CE_QMP_UP) {
        h->ram[5]=CE_QMP_UP;
        if(h->mode!=3 || h->ram[6]==CE_QMP_UP)h->ram[12]=CE_QMP_UP;
    }
    if(h->mode==2)h->ram[14]+=4;
    return 0;}
static void release(void *p,int uncertain){struct host *h=p;h->release++;h->uncertain=(unsigned)uncertain;}
static void init(struct host *h,struct ceph_qmp_mmio *m)
{
    memset(h,0,sizeof(*h));h->ram[0]=0x4d41494c;h->ram[1]=1;
    for(unsigned c=0;c<2;++c)for(unsigned i=0;i<4;++i)h->ram[3+c*6+i]=CE_QMP_DOWN;
    h->ram[3]=CE_QMP_UP;h->ram[7]=64;h->ram[8]=64;h->ram[13]=128;h->ram[14]=128;
    *m=(struct ceph_qmp_mmio){.ram=h->ram,.irq=&h->irq,.ram_pa=0xc300000,.irq_pa=0x17c0000c,
        .bytes=4096,.owner=h,.lease=lease,.now_ns=now,.pause=pause_io,.release=release};
}
int main(void)
{
    struct host h;struct ceph_qmp_mmio m;struct ceph_qmp_connect s={0};init(&h,&m);
    assert(!ceph_qmp_connect_cold(&s,&m) && s.connected && s.writes==4 && s.notifies==3 && !h.release);
    unsigned calls=h.calls;assert(!ceph_qmp_mmio_bind(&m,1));
    assert(ceph_qmp_connect_cold(&s,&m)<0);
    init(&h,&m);h.mode=3;s=(struct ceph_qmp_connect){0};
    assert(!ceph_qmp_connect_cold(&s,&m) && s.connected); /* peer waits for its ACK */
    for(unsigned f=1;f<=calls;++f) {
        init(&h,&m);h.fail=f;s=(struct ceph_qmp_connect){0};assert(ceph_qmp_connect_cold(&s,&m)<0 && !s.connected);
        if(s.writes)assert(m.terminal && h.release==1 && h.uncertain);
        assert(h.ram[0]==0x4d41494c && h.ram[7]==64 && h.ram[13]==128);
    }
    for(unsigned mode=1;mode<=2;++mode) {
        init(&h,&m);h.mode=mode;s=(struct ceph_qmp_connect){0};
        assert(ceph_qmp_connect_cold(&s,&m)<0 && m.terminal && h.uncertain);
    }
    const unsigned inherited[]={4,5,6,9,10,11,12};
    for(unsigned i=0;i<7;++i) {
        init(&h,&m);h.ram[inherited[i]]=CE_QMP_UP;s=(struct ceph_qmp_connect){0};
        assert(ceph_qmp_connect_cold(&s,&m)<0 && !s.writes && !h.irq && !h.release);
    }
    init(&h,&m);h.ram[32]=4;s=(struct ceph_qmp_connect){0};assert(ceph_qmp_connect_cold(&s,&m)<0 && !s.writes && h.ram[32]==4);
    puts("QMP cold connect: link/channel ACK handshake, exact host fields, every lease failure, timeout/layout/inherited refusal PASS (RAM)");
}
