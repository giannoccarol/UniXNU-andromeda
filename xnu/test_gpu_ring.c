#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_ring.h"
static uint32_t memory[8192];
struct mock {unsigned read,call,fail,end,uncertain,published,bells,next;};
static int step(struct mock *m){return ++m->call==m->fail?-9:0;}
static int begin(void *p){return step(p);}
static int rptr(void *p,unsigned *r){struct mock *m=p;*r=m->read;return step(m);}
static int publish(void *p,unsigned start,unsigned words){struct mock *m=p;assert(start+words<=8192);m->published++;return step(m);}
static int doorbell(void *p,unsigned next){struct mock *m=p;assert(m->published);m->bells++;m->next=next;return step(m);}
static void end(void *p,int uncertain){struct mock *m=p;m->end++;m->uncertain=(unsigned)uncertain;}
static struct ceph_gpu_ring ring(struct mock *m,unsigned w){return (struct ceph_gpu_ring){memory,0x100000,w,m,begin,rptr,publish,doorbell,end,0,0,0};}
int main(void)
{
    struct ceph_ring_span p;
    assert(!ceph_ring_reserve(0,0,10,&p) && p.start==0 && p.next==10 && !p.pad_words);
    assert(!ceph_ring_reserve(400,8190,10,&p) && p.start==0 && p.pad_start==8190 && p.pad_words==2);
    assert(ceph_ring_reserve(10,8190,10,&p)==1);
    assert(ceph_ring_reserve(110,100,10,&p)==1);
    assert(!ceph_ring_reserve(111,100,10,&p) && p.next==110);
    assert(ceph_ring_reserve(8192,0,10,&p)==-1);
    assert(ceph_ring_reserve(0,8191,10,&p)==-1);
    for(unsigned read=0;read<8192;read+=17)for(unsigned write=0;write<=8190;write+=19) {
        int e=ceph_ring_reserve(read,write,10,&p);
        if(!e) {
            unsigned used=(write+8192-read)%8192;
            assert(used+p.pad_words+10<8192);
            assert(p.start+10<=8190 && p.next==p.start+10);
        }
    }
    struct mock m={.read=400};struct ceph_gpu_ring r=ring(&m,8190);
    assert(!ceph_gpu_ring_scratch(&r,0x200000,4096,0x200000,0x1234,0x200004,17));
    assert(m.published==2 && m.next==10 && r.inflight && r.sequence==17);
    assert(ceph_pm4_valid_type7(memory[8190]) && ((memory[8190]>>16)&127)==0x10 && memory[8191]==0);
    assert(memory[0]==0x703d8003 && memory[3]==0x1234 && memory[9]==17);
    unsigned calls=m.call;assert(ceph_gpu_ring_scratch(&r,0x200000,4096,0x200000,0,0x200004,18)==-1 && calls==m.call);
    for(unsigned fail=1;fail<=calls;++fail) {
        memset(memory,0,sizeof(memory));m=(struct mock){.read=400,.fail=fail};r=ring(&m,8190);
        assert(ceph_gpu_ring_scratch(&r,0x200000,4096,0x200000,1,0x200004,17)==-9);
        if(fail<=2)assert(!r.inflight && !r.poisoned && !memory[0]);
        else assert(r.inflight && r.poisoned && m.uncertain);
    }
    m=(struct mock){.read=10};r=ring(&m,8190);memory[0]=0xabc;
    assert(ceph_gpu_ring_scratch(&r,0x200000,4096,0x200000,1,0x200004,17)==1 && memory[0]==0xabc && !m.bells);
    assert(ceph_gpu_ring_scratch(&r,0x100000,4096,0x100000,1,0x100004,17)==-1);
    puts("A640 scratch ring: reservation invariants, NOP wrap, publish-before-WPTR, all failure points PASS (RAM only)");
}
