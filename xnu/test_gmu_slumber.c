#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gmu_slumber.h"
struct mock {
    uint32_t tx[64],rx[64],tr,tw,rr,rw,state,gx,fault;
    struct ceph_hfi_queue qtx,qrx,fw_rx,fw_tx;struct ceph_hfi_session session;
    unsigned calls,fail,finish,success,counter_off;uint64_t time;int pending,reject;
};
static int step(struct mock *m){return ++m->calls==m->fail?-9:0;}
static int okay(void *p){(void)p;return 0;}
static int notify(void *p){struct mock *m=p;uint32_t cmd[3];unsigned n;assert(!ceph_hfi_read(&m->fw_rx,cmd,3,&n));
    assert(n==3 && (cmd[0]&0xffff)==0x321 && cmd[1]==3 && cmd[2]==1 && m->counter_off);
    uint32_t ack[]={ceph_hfi_header(33,3,1,0),cmd[0],m->reject?1u:0u};assert(!ceph_hfi_write(&m->fw_tx,ack,3));
    if(!m->pending){m->state=15;m->gx=0xc0;}return 0;}
static int now(void *p,uint64_t *n){struct mock *m=p;m->time+=1000000;*n=m->time;return step(m);}
static int simple(void *p){return step(p);}
static int rd(void *p,enum ceph_slumber_reg r,uint32_t *v){struct mock *m=p;*v=r==CE_SL_RPMH_POWER_STATE?m->state:r==CE_SL_GX_STATUS?m->gx:m->fault;return step(m);}
static int wr(void *p,enum ceph_slumber_reg r,uint32_t v){struct mock *m=p;assert(r==CE_SL_POWER_COUNTER_ENABLE && !v);m->counter_off=1;return step(m);}
static void finish(void *p,int okay){struct mock *m=p;m->finish++;m->success=(unsigned)okay;}
static void init(struct mock *m){memset(m,0,sizeof(*m));m->qtx=(struct ceph_hfi_queue){m->tx,&m->tr,&m->tw,64,2,{m,okay,okay,okay,notify},0};
    m->qrx=(struct ceph_hfi_queue){m->rx,&m->rr,&m->rw,64,2,{m,okay,okay,okay,okay},0};m->fw_rx=m->qtx;m->fw_rx.io.notify=okay;m->fw_tx=m->qrx;
    m->session=(struct ceph_hfi_session){&m->qtx,&m->qrx,m,now,okay,okay,8,0,0,0};}
int main(void){struct mock m;init(&m);struct ceph_gmu_slumber_ops b={&m,simple,simple,rd,wr,simple,now,simple,finish};struct ceph_gmu_slumber s={0};
    assert(!ceph_gmu_slumber_request(&s,&b,&m.session,3,1,12,8) && s.slumbered && m.success);
    unsigned calls=m.calls;
    for(unsigned f=1;f<=calls;++f){init(&m);m.fail=f;s=(struct ceph_gmu_slumber){0};assert(ceph_gmu_slumber_request(&s,&b,&m.session,3,1,12,8)<0 && !m.success);}
    init(&m);m.pending=1;s=(struct ceph_gmu_slumber){0};assert(ceph_gmu_slumber_request(&s,&b,&m.session,3,1,12,8)==-2 && !m.success);
    init(&m);m.reject=1;s=(struct ceph_gmu_slumber){0};assert(ceph_gmu_slumber_request(&s,&b,&m.session,3,1,12,8)<0 && !m.success);
    init(&m);m.fault=1;s=(struct ceph_gmu_slumber){0};assert(ceph_gmu_slumber_request(&s,&b,&m.session,3,1,12,8)<0 && !m.success);
    puts("GMU slumber: direct HFI PREPARE_SLUMBER, ACK != power-off, GX/state/fault checks and timeout PASS");}
