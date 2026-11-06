#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_bcm.h"
static uint8_t bytes[256];
static uint32_t rd(void *p,uint32_t o){(void)p;assert(!(o&3)&&o<=252);return (uint32_t)bytes[o]|(uint32_t)bytes[o+1]<<8|(uint32_t)bytes[o+2]<<16|(uint32_t)bytes[o+3]<<24;}
static void wr(unsigned o,uint32_t n){for(unsigned i=0;i<4;++i)bytes[o+i]=(uint8_t)(n>>(8*i));}
int main(void)
{
    wr(0,1);wr(4,0x0c0330db);wr(8,5);wr(12,(1u<<16)|24);memcpy(bytes+144,"SH0",3);
    wr(160,0x50000);wr(164,8);wr(168,1000);wr(172,(2u<<16)|16);
    struct ceph_arc_db db={NULL,sizeof(bytes),rd};struct ceph_bcm_resource r;
    assert(!ceph_bcm_lookup(&db,"SH0",&r) && r.address==0x50000 && r.unit==1000 && r.width==16 && r.vcd==2);
    uint64_t x,y;assert(!ceph_bcm_normalize(&r,32000,64000,32,2,&x,&y) && x==8 && y==32);
    assert(!ceph_bcm_normalize(&r,1,1,32,2,&x,&y) && x==1 && y==1);
    assert(ceph_bcm_normalize(&r,UINT64_MAX,1,32,2,&x,&y)<0);
    assert(ceph_bcm_normalize(&r,1,1,0,2,&x,&y)<0);
    struct ceph_bcm_vote v[3]={{{0x50000,1000,16,2},1,2},{{0x50004,1000,16,1},0,0},{{0x50008,1000,16,2},999999,999999}};
    struct ceph_bcm_command out[3];uint32_t wait;
    assert(ceph_bcm_commands(v,3,out,3,&wait)==3 && wait==5);
    assert(out[0].address==0x50004 && out[0].data==0x40000000);
    assert(out[1].address==0x50000 && out[1].data==0x20004002);
    assert(out[2].address==0x50008 && out[2].data==0x6fffffff);
    v[2].resource.address=v[0].resource.address;assert(ceph_bcm_commands(v,3,out,3,&wait)<0);
    wr(172,0xff020010);assert(ceph_bcm_lookup(&db,"SH0",&r)<0 && !r.unit);
    wr(172,0x20010);wr(168,0);assert(ceph_bcm_lookup(&db,"SH0",&r)<0);
    wr(168,1000);wr(164,7);assert(ceph_bcm_lookup(&db,"SH0",&r)<0);
    wr(164,8);memset(bytes+144,0,8);memcpy(bytes+144,"ACV",3);
    wr(168,0);wr(172,3u<<16);
    assert(ceph_bcm_lookup(&db,"ACV",&r)<0 && !r.unit);
    assert(!ceph_bcm_lookup_acv(&db,&r) && !r.unit && !r.width && r.vcd==3);
    assert(ceph_bcm_normalize(&r,1,1,4,4,&x,&y)<0);
    struct ceph_bcm_vote acv={r,0,8};
    assert(ceph_bcm_commands(&acv,1,out,3,&wait)<0);
    assert(ceph_bcm_commands_kind(&acv,1,out,3,&wait,0)==1 && out[0].data==0x60000008);
    acv.y=1;assert(ceph_bcm_commands_kind(&acv,1,out,3,&wait,0)<0);
    acv.y=8;acv.x=1;assert(ceph_bcm_commands_kind(&acv,1,out,3,&wait,0)<0);
    memcpy(bytes+144,"SH0",3);assert(ceph_bcm_lookup_acv(&db,&r)<0);
    assert(ceph_bcm_lookup_kind(&db,"SH0",&r,1)<0); /* cannot relax arbitrary BCM */
    memset(bytes+144,0,8);memcpy(bytes+144,"ACV",3);
    struct ceph_bcm_metadata raw;
    assert(!ceph_bcm_inspect(&db,"ACV",&r,1,&raw) && raw.bounded_aux_read && raw.aux_bytes==8 && raw.info==3u<<16);
    wr(172,0x01030000); /* reserved byte is NOT waived for bit votes */
    assert(ceph_bcm_inspect(&db,"ACV",&r,1,&raw)<0 && !r.address && raw.bounded_aux_read && raw.info==0x01030000);
    wr(164,7);assert(ceph_bcm_inspect(&db,"ACV",&r,1,&raw)<0 && raw.aux_bytes==7 && !raw.bounded_aux_read);
    wr(164,8|((sizeof(bytes)-4-168u)<<16));
    assert(ceph_bcm_inspect(&db,"ACV",&r,1,&raw)<0 && !raw.bounded_aux_read);
    puts("BCM: bounded metadata, exact KGSL normalization, VCD grouping/commit/wait/saturation PASS (no votes)");
}
