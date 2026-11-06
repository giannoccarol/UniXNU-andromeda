#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_pdc_mmio.h"
static uint32_t gmu[0x30000/4],cfg[0x10000/4],seq[0x10000/4];
struct host {unsigned call,fail,finished,kept;};
static int lease(void *p){struct host *h=p;return ++h->call==h->fail?-9:0;}
static void finish(void *p,int keep){struct host *h=p;h->finished++;h->kept=(unsigned)keep;}
static void fixture(struct host *h,struct ceph_gmu_mmio *g,struct ceph_pdc_mmio *m)
{
    *h=(struct host){0};memset(gmu,0,sizeof(gmu));memset(cfg,0,sizeof(cfg));memset(seq,0,sizeof(seq));
    gmu[0x1f800-0x1a800]=1;gmu[0x1f8d0-0x1a800]=0xc0;
    *g=(struct ceph_gmu_mmio){(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,h,lease};
    *m=(struct ceph_pdc_mmio){.gmu=g,.cfg=cfg,.seq=seq,.cfg_pa=0xb290000,.seq_pa=0xb490000,
        .cfg_bytes=sizeof(cfg),.seq_bytes=sizeof(seq),.owner=h,.lease=lease,.finish=finish};
}
int main(void)
{
    struct ceph_pdc_input in={.chipid=0x06040001,.cfg_pa=0xb290000,.seq_pa=0xb490000};
    for(unsigned i=0;i<3;++i)in.resources[i]=(struct ceph_arc_resource){.address=(uint32_t[]){0x30010,0x30000,0x30090}[i],.levels={0,16,48,64},.count=4};
    struct host h;struct ceph_gmu_mmio g;struct ceph_pdc_mmio m;struct ceph_pdc_ops ops;struct ceph_pdc_install s={0};
    fixture(&h,&g,&m);assert(!ceph_pdc_mmio_ops(&m,&ops));assert(!ceph_pdc_install_a640v2(&s,&ops,&in));
    assert(m.cursor==48 && m.terminal && h.finished==1 && h.kept && cfg[0x1140]==0x80000001);
    assert(gmu[0x23580-0x1a800]==0xa7a506a0 && seq[0]==0xfebea1e1);
    unsigned calls=h.call;
    for(unsigned fail=1;fail<=calls;++fail) {
        fixture(&h,&g,&m);h.fail=fail;s=(struct ceph_pdc_install){0};int e=ceph_pdc_mmio_ops(&m,&ops);
        if(!e)e=ceph_pdc_install_a640v2(&s,&ops,&in);
        assert(e);if(m.cursor)assert(m.terminal && h.kept && h.finished==1);
    }
    fixture(&h,&g,&m);cfg[0x1140]=1;assert(ceph_pdc_mmio_begin(&m,&in)<0 && cfg[0x1140]==1);
    fixture(&h,&g,&m);gmu[0x1f800-0x1a800]=0;assert(ceph_pdc_mmio_ops(&m,&ops)<0);
    fixture(&h,&g,&m);gmu[0x1f8d0-0x1a800]=0;assert(ceph_pdc_mmio_ops(&m,&ops)<0);
    fixture(&h,&g,&m);assert(!ceph_pdc_mmio_begin(&m,&in));uint32_t value;
    assert(ceph_pdc_mmio_write(&m,CE_PDC_CFG,0x1140*4,0x80000001)<0 && !m.cursor);
    assert(ceph_pdc_mmio_write(&m,CE_PDC_RSC,0x23404*4-0x6a000,0)<0);
    assert(ceph_pdc_mmio_read(&m,CE_PDC_CFG,0,&value)<0);
    assert(ceph_pdc_mmio_read(&m,(enum ceph_pdc_window)99,0,&value)<0);
    ceph_pdc_mmio_end(&m,0);assert(h.finished==1 && !h.kept);
    puts("PDC MMIO: actual three RAM apertures/48 ordered stores, cold-state and every lease-failure retention PASS");
}
