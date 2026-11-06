#include <assert.h>
#include <stdio.h>
#include "cepheus_a640_pm4.h"
#include "cepheus_gpu_submit.h"
struct mock {unsigned call,fail,finish,keep,observes,pause,quiesce;uint64_t time;int late,backward,frozen;};
static int step(struct mock *m) {return ++m->call==m->fail?-9:0;}
static int begin(void *p,uint32_t s) {assert(s==17);return step(p);}
static int simple(void *p) {return step(p);}
static int observe(void *p,uint32_t *f) {struct mock *m=p;m->observes++;*f=m->late?16:17;return step(m);}
static int now(void *p,uint64_t *t) {struct mock *m=p;if(!m->frozen)m->time+=1000000;*t=m->backward&&m->time>1000000?0:m->time;return step(m);}
static int pausefn(void *p) {struct mock *m=p;m->pause++;return step(m);}
static int quiet(void *p) {struct mock *m=p;m->quiesce++;return step(m);}
static void finish(void *p,int keep) {struct mock *m=p;m->finish++;m->keep=(unsigned)keep;}
static struct ceph_submit_ops ops(struct mock *m) {return (struct ceph_submit_ops){m,begin,simple,simple,observe,now,pausefn,quiet,finish};}
int main(void)
{
    uint32_t cmd[11]={0};cmd[10]=0xabc;
    assert(ceph_a640_fenced_write_test(cmd,10,0x100001000ULL,4096,0x100001000ULL,9,0x100001004ULL,17)==10);
    assert(cmd[5]==0x70460004 && cmd[6]==4 && cmd[7]==0x1004 && cmd[8]==1 && cmd[9]==17 && cmd[10]==0xabc);
    assert(ceph_pm4_valid_type7(cmd[5]));
    assert(ceph_a640_fenced_write_test(cmd,9,0x1000,4096,0x1000,9,0x1004,17)==-1);
    assert(ceph_a640_fenced_write_test(cmd,10,0x1000,4096,0x1000,9,0x1000,17)==-1);
    assert(ceph_a640_fenced_write_test(cmd,10,0x1000,4096,0x1000,9,0x1004,0)==-1);
    struct mock m={0};struct ceph_submit_ops b=ops(&m);struct ceph_submit s={0};
    assert(!ceph_submit_once(&s,&b,17) && s.state==CE_SUB_COMPLETE && m.quiesce==1 && !m.keep);
    unsigned calls=m.call;assert(ceph_submit_once(&s,&b,17)==-1 && m.call==calls);
    for(unsigned i=1;i<=calls;++i) {
        m=(struct mock){.fail=i};b=ops(&m);s=(struct ceph_submit){0};
        assert(ceph_submit_once(&s,&b,17)!=0);
        if(i==1) assert(s.state==CE_SUB_REFUSED && !m.finish);
        else assert(s.state==CE_SUB_QUARANTINED && m.keep && m.finish==1);
    }
    for(unsigned i=0;i<3;++i) {
        m=(struct mock){.late=1,.backward=i==1,.frozen=i==2};b=ops(&m);s=(struct ceph_submit){0};
        assert(ceph_submit_once(&s,&b,17)==-2 && m.keep && m.pause<=10000 && !m.quiesce);
    }
    puts("GPU scratch fence: packet/address bounds, stale fence, timeout, fault/quiescence quarantine PASS (mock)");
}
