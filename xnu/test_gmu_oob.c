#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_oob.h"
struct mock {struct ceph_oob_bits bits;uint32_t info,gx;unsigned calls,fail,kept,cleared,released;uint64_t time;int silent,busy;};
static int step(struct mock *m){return ++m->calls==m->fail?-9:0;}
static int simple(void *p){return step(p);}
static int rd(void *p,enum ceph_oob_reg r,uint32_t *v){struct mock *m=p;assert(r==CE_OOB_G2H_INFO||r==CE_OOB_GX_STATUS);*v=r==CE_OOB_G2H_INFO?m->info:m->gx;return step(m);}
static int wr(void *p,enum ceph_oob_reg r,uint32_t v){struct mock *m=p;
    if(r==CE_OOB_H2G_SET){if(v==m->bits.request){if(!m->silent)m->info|=m->bits.ack;}else{assert(v==m->bits.release);m->released++;}}
    else{assert(r==CE_OOB_G2H_CLEAR&&v==m->bits.ack);m->info&=~v;m->cleared++;}return step(m);}
static int now(void *p,uint64_t *n){struct mock *m=p;m->time+=1000000;*n=m->time;return step(m);}
static int drained(void *p){return ((struct mock *)p)->busy;}
static void end(void *p,int keep){((struct mock *)p)->kept=(unsigned)keep;}
int main(void){
    struct ceph_oob_bits bits;assert(!ceph_gmu_oob_gpu_bits(0x20000001,&bits)&&bits.ack==0x80000000);
    struct mock m={.bits=bits};struct ceph_oob_ops b={&m,simple,simple,rd,wr,simple,now,simple,drained,end};struct ceph_gmu_oob s={0};
    assert(!ceph_gmu_oob_acquire(&s,&b,0x20000001)&&s.state==CE_OOB_HELD&&m.cleared==1&&!m.info);
    unsigned calls=m.calls;m.busy=1;assert(ceph_gmu_oob_release(&s,&b)<0&&!m.released&&s.state==CE_OOB_HELD);
    m.busy=0;assert(!ceph_gmu_oob_release(&s,&b)&&m.released==1&&s.state==CE_OOB_RELEASED);
    for(unsigned f=1;f<=calls;++f){m=(struct mock){.bits=bits,.fail=f};s=(struct ceph_gmu_oob){0};assert(ceph_gmu_oob_acquire(&s,&b,0x20000001)<0&&s.state==CE_OOB_FAILED);}
    m=(struct mock){.bits=bits,.silent=1};s=(struct ceph_gmu_oob){0};assert(ceph_gmu_oob_acquire(&s,&b,0x20000001)==-2&&m.kept);
    m=(struct mock){.bits=bits,.info=bits.ack};s=(struct ceph_gmu_oob){0};assert(ceph_gmu_oob_acquire(&s,&b,0x20000001)<0&&!s.writes);
    m=(struct mock){.bits=bits,.gx=0xc0};s=(struct ceph_gmu_oob){0};assert(ceph_gmu_oob_acquire(&s,&b,0x20000001)<0&&m.kept);
    assert(!ceph_gmu_oob_gpu_bits(0x20000000,&bits)&&bits.request==0x10000&&bits.release==0x1000000);
    m=(struct mock){.bits=bits};s=(struct ceph_gmu_oob){0};assert(!ceph_gmu_oob_acquire(&s,&b,0x20000000));
    puts("GMU OOB0: versioned bits, fresh ACK, GX check, busy-release refusal and failure retention PASS");}
