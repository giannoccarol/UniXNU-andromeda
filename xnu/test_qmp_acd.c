#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_qmp_acd.h"
struct mock {uint32_t mem[256];unsigned call,fail,end,uncertain,data_writes;uint64_t time;int silent;};
static int step(struct mock *m){return ++m->call==m->fail?-9:0;}
static int begin(void *p,struct ceph_qmp_layout *l){*l=(struct ceph_qmp_layout){64,128};return step(p);}
static int valid(void *p){return step(p);}
static int rd(void *p,uint32_t o,uint32_t *v){struct mock *m=p;assert(!(o&3)&&o<sizeof(m->mem));*v=m->mem[o/4];return step(m);}
static int wr(void *p,uint32_t o,uint32_t v){struct mock *m=p;assert(o>=64 && o<192 && !(o&3));if(o==64)assert(m->data_writes==8);else{assert(m->mem[16]==0);m->data_writes++;}m->mem[o/4]=v;return step(m);}
static int notify(void *p){struct mock *m=p;assert(m->mem[16]==32);assert(!memcmp((char *)&m->mem[17],"{class: gpu, res: acd, value: 1}",32));if(!m->silent)m->mem[16]=0;return step(m);}
static int now(void *p,uint64_t *v){struct mock *m=p;m->time+=1000000;*v=m->time;return step(m);}
static void end(void *p,int u){struct mock *m=p;m->end++;m->uncertain=(unsigned)u;}
int main(void)
{
    char text[33]={0};assert(ceph_qmp_acd_text(text,1)==32 && !strcmp(text,"{class: gpu, res: acd, value: 1}"));
    assert(ceph_qmp_acd_text(text,0)==32 && !strcmp(text,"{class: gpu, res: acd, value: 0}"));
    assert(ceph_qmp_acd_text(text,2)<0);
    struct mock m={0};struct ceph_qmp_ops b={&m,begin,valid,rd,wr,valid,notify,now,valid,end};struct ceph_qmp_tx s={0};
    assert(!ceph_qmp_send_acd(&s,&b,1) && s.consumed && !s.uncertain && s.writes==9);
    unsigned calls=m.call;
    for(unsigned f=1;f<=calls;++f){m=(struct mock){.fail=f};s=(struct ceph_qmp_tx){0};assert(ceph_qmp_send_acd(&s,&b,1)<0 && !s.consumed);assert(s.uncertain==(s.writes!=0));}
    m=(struct mock){.silent=1};s=(struct ceph_qmp_tx){0};assert(ceph_qmp_send_acd(&s,&b,1)==-2 && s.uncertain);
    m=(struct mock){0};m.mem[16]=5;s=(struct ceph_qmp_tx){0};assert(ceph_qmp_send_acd(&s,&b,1)<0 && !s.writes && m.mem[16]==5);
    puts("QMP ACD: fixed text, payload-before-length, consume ACK, busy refusal and all failures PASS (fake mailbox)");
}
