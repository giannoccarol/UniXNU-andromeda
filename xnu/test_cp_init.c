#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_cp_init.h"
struct mock {unsigned calls,fail,ends;int invalid_protect,invalid_mode;};
static int step(struct mock *m){return ++m->calls==m->fail?-1:0;}
static int begin(void *p){return step(p);}
static int readreg(void *p,uint32_t off,uint32_t *value){struct mock *m=p;*value=off^0x98760000u;
    if(off==0x84f)*value=m->invalid_protect?0:11;
    if(off>=0x850 && off<=0x86f)assert(!ceph_cp_protect_word(off-0x850,value));
    for(unsigned i=38;i<48;++i)if(off==ceph_cp_restore_offsets[i])*value=m->invalid_mode?0:1;
    return step(m);}
static void end(void *p){((struct mock *)p)->ends++;}
int main(void)
{
    struct mock m={0};struct ceph_cp_snapshot_ops b={&m,begin,readreg,begin,end};
    uint32_t out[137];memset(out,0xa5,sizeof(out));
    assert(!ceph_cp_snapshot(&b,out,136) && m.ends==1 && out[3]==0x4c0084 && out[136]==0xa5a5a5a5);
    assert(out[4]==0x10 && out[4+76]==0xc01 && out[5+76]==1 && out[4+2*65]==0x8e50);
    unsigned calls=m.calls;
    for(unsigned f=1;f<=calls;++f){m=(struct mock){.fail=f};memset(out,0xa5,sizeof(out));
        assert(ceph_cp_snapshot(&b,out,136)<0 && out[0]==0xa5a5a5a5 && out[135]==0xa5a5a5a5 && m.ends==(f==1?0u:1u));}
    m=(struct mock){.invalid_protect=1};assert(ceph_cp_snapshot(&b,out,136)<0);
    m=(struct mock){.invalid_mode=1};assert(ceph_cp_snapshot(&b,out,136)<0);
    m=(struct mock){0};assert(ceph_cp_snapshot(&b,out,135)<0 && !m.calls);
    uint32_t protected;
    assert(!ceph_cp_protect_word(0,&protected) && protected==(0x4ffu<<18));
    assert(!ceph_cp_protect_word(31,&protected) && protected==0x80011c00);
    assert(ceph_cp_protect_word(32,&protected)<0);
    assert(ceph_cp_me_init(out,12,0x100004000ULL,4096)==12);
    assert(out[0]==0x70c8000b && ceph_pm4_valid_type7(out[0]) && out[1]==0x16f && out[2]==3 && out[3]==0x20000000);
    assert(out[7]==2 && out[8]==0x4000 && out[9]==1 && out[10]==0 && out[11]==0);
    assert(ceph_cp_me_init(out,11,0x1000,4096)<0 && ceph_cp_me_init(out,12,0,4096)<0);
    assert(ceph_cp_me_init(out,12,0x1001,4096)<0 && ceph_cp_me_init(out,12,0x1000,135*4)<0);
    puts("A640 CP init: exact12-DWORD ordinals, actual register read snapshot, 64-bit/protection gates and failure atomicity PASS");
}
