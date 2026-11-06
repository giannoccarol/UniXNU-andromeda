#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_start.h"
struct mock {uint32_t reg[CE_GMU_REG_COUNT];unsigned call,fail,finish,keep,live,pauses;uint64_t now;int mode;};
enum { NORMAL,STUCK_INIT,STUCK_HFI,FAULT_AFTER_START,WRONG_VERSION,REJECT_CLEAR,FROZEN,REVERSE };
static int step(struct mock *m){return ++m->call==m->fail?-9:0;}
static int begin(void *p,const struct ceph_gmu_start_config *c){struct mock *m=p;assert(c->chipid==0x06040001);int e=step(m);if(!e)m->live=1;return e;}
static int valid(void *p){struct mock *m=p;assert(m->live);return step(m);}
static int rd(void *p,enum ceph_gmu_reg r,uint32_t *v){struct mock *m=p;int e=step(m);if(e)return e;*v=m->reg[r];return 0;}
static int wr(void *p,enum ceph_gmu_reg r,uint32_t v)
{
    struct mock *m=p;
    if(r==CE_GMU_SYSRESET) {
        assert(!v && !m->reg[CE_GMU_INIT_RESULT] && m->reg[CE_GMU_QTBL_INFO]==1);
        assert(m->reg[CE_GMU_SFR_ADDR]==0x06040100 && m->reg[CE_GMU_INTER_FRAME_CTRL]==0x09c40407);
        if(m->mode!=STUCK_INIT && m->mode!=FROZEN && m->mode!=REVERSE)m->reg[CE_GMU_INIT_RESULT]=0xbabeface;
        if(m->mode==FAULT_AFTER_START)m->reg[CE_GMU_AO_STATUS]=1;
    }
    if(r==CE_GMU_HFI_INIT) {assert(m->reg[CE_GMU_INIT_RESULT]==0xbabeface);if(m->mode!=STUCK_HFI)m->reg[CE_GMU_HFI_STATUS]=1;}
    if(!(r==CE_GMU_INIT_RESULT && m->mode==REJECT_CLEAR))m->reg[r]=v;
    return step(m);
}
static int now(void *p,uint64_t *v){struct mock *m=p;if(m->mode!=FROZEN)m->now+=1000000;*v=m->mode==REVERSE && m->now>1000000?0:m->now;return step(m);}
static int pausefn(void *p){struct mock *m=p;m->pauses++;return step(m);}
static void finish(void *p,int keep){struct mock *m=p;assert(m->live);m->live=0;m->finish++;m->keep=(unsigned)keep;}
static struct mock fixture(void){struct mock m={0};m.reg[CE_GMU_SYSRESET]=1;m.reg[CE_GMU_INIT_RESULT]=0xbabeface;m.reg[CE_GMU_HFI_VERSION]=0x20000001;return m;}
static struct ceph_gmu_start_ops ops(struct mock *m){return (struct ceph_gmu_start_ops){m,begin,valid,rd,wr,valid,now,pausefn,finish};}
int main(void)
{
    struct ceph_gmu_start_config c={0x06040001,0x60000000,5*4096,0x60005000,4096};
    struct ceph_gmu_start_plan p;assert(!ceph_gmu_start_plan(&c,&p) && p.chip==0x06040100 && p.fence==0x80a008a0);
    uint32_t off;assert(!ceph_gmu_register_offset(CE_GMU_SYSRESET,&off) && off==0x14000);
    assert(!ceph_gmu_register_offset(CE_GMU_FENCE_RANGE,&off) && off==0x24c44);
    assert(ceph_gmu_register_offset(CE_GMU_REG_COUNT,&off)<0);
    struct mock m=fixture();struct ceph_gmu_start_ops b=ops(&m);struct ceph_gmu_start s={0};
    assert(!ceph_gmu_start_cold(&s,&b,&c) && s.state==CE_GS_HFI_READY && s.hfi_version==0x20000001 && m.keep);
    unsigned calls=m.call;assert(ceph_gmu_start_cold(&s,&b,&c)<0 && m.call==calls);
    for(unsigned i=1;i<=calls;++i) {
        m=fixture();m.fail=i;b=ops(&m);s=(struct ceph_gmu_start){0};
        assert(ceph_gmu_start_cold(&s,&b,&c)<0);
        if(i==1)assert(!m.finish);else assert(m.finish==1 && m.keep==(s.effects!=0));
        assert((s.state==CE_GS_QUARANTINED)==(s.effects!=0));
    }
    for(int mode=STUCK_INIT;mode<=REVERSE;++mode) {
        m=fixture();m.mode=mode;if(mode==WRONG_VERSION)m.reg[CE_GMU_HFI_VERSION]=0x10000001;
        b=ops(&m);s=(struct ceph_gmu_start){0};assert(ceph_gmu_start_cold(&s,&b,&c)<0 && m.keep && m.pauses<=10000);
        if(mode==STUCK_INIT || mode==REJECT_CLEAR)assert(!m.reg[CE_GMU_HFI_INIT]);
    }
    m=fixture();b=ops(&m);s=(struct ceph_gmu_start){0};c.log_va=c.hfi_va;
    assert(ceph_gmu_start_cold(&s,&b,&c)<0 && !m.call);
    c.log_va=0x60005000;c.chipid=0x06040002;assert(ceph_gmu_start_cold(&s,&b,&c)<0 && !m.call);
    puts("GMU CM3 register startup: offsets/fresh result/IFPC/HFI/fault/version/all-call failures PASS (fake registers)");
}
