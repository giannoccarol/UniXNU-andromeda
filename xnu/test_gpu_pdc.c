#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_pdc.h"
struct mock {uint32_t r[3][0xc000];unsigned calls,fail,ended,kept,writes;};
static int step(struct mock *m){return ++m->calls==m->fail?-9:0;}
static int begin(void *p,const struct ceph_pdc_input *in){assert(in->cfg_pa==0xb290000);return step(p);}
static int valid(void *p){return step(p);}
static int rd(void *p,enum ceph_pdc_window w,uint32_t o,uint32_t *v){struct mock *m=p;assert(w<=CE_PDC_SEQ && !(o&3) && o<0x30000);*v=m->r[w][o/4];return step(m);}
static int wr(void *p,enum ceph_pdc_window w,uint32_t o,uint32_t v){struct mock *m=p;++m->writes;m->r[w][o/4]=v;return step(m);}
static void end(void *p,int keep){struct mock *m=p;m->ended++;m->kept=(unsigned)keep;}
int main(void)
{
    struct ceph_pdc_input in={.chipid=0x06040001,.cfg_pa=0xb290000,.seq_pa=0xb490000,
        .resources={{.address=0x30010,.levels={0,16,48,64},.count=4},{.address=0x30000,.levels={0,16,48,64},.count=4},
                    {.address=0x30090,.levels={0,1,2,3},.count=4}}};
    struct ceph_pdc_plan p;assert(!ceph_pdc_plan_a640v2(&in,&p) && p.count==48);
    assert(p.writes[0].offset==0x23010 && p.writes[0].value==0x1000000);
    assert(p.writes[47].window==CE_PDC_CFG && p.writes[47].offset==0x4500 && p.writes[47].value==0x80000001);
    static struct mock m;struct ceph_pdc_ops b={&m,begin,valid,rd,wr,valid,end};struct ceph_pdc_install s={0};
    assert(!ceph_pdc_install_a640v2(&s,&b,&in) && s.attempted==48 && !s.quarantined && m.kept);
    assert(m.r[CE_PDC_CFG][0x1576]==0x30010 && m.r[CE_PDC_CFG][0x157b]==0 && m.r[CE_PDC_CFG][0x15df]==3);
    assert(m.r[CE_PDC_CFG][0x1580-2]==0x30090);
    unsigned calls=m.calls;
    for(unsigned f=1;f<=calls;++f) {
        memset(&m,0,sizeof(m));m.fail=f;s=(struct ceph_pdc_install){0};
        assert(ceph_pdc_install_a640v2(&s,&b,&in)<0);
        if(f==1)assert(!m.ended);else assert(m.ended==1 && m.kept==(s.attempted!=0));
        assert(s.quarantined==(s.attempted!=0));
    }
    memset(&m,0,sizeof(m));m.r[CE_PDC_CFG][0x1140]=0x80000001;s=(struct ceph_pdc_install){0};
    assert(ceph_pdc_install_a640v2(&s,&b,&in)<0 && !s.attempted);
    in.cfg_pa=0xb280000;assert(ceph_pdc_plan_a640v2(&in,&p)<0);in.cfg_pa=0xb290000;
    in.resources[2].address=0x30080;assert(ceph_pdc_plan_a640v2(&in,&p)<0);
    puts("A640v2 PDC/RSC: exact48 writes, late enables, cold-owner refusal and every-call quarantine PASS (fake registers)");
}
