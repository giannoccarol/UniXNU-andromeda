#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_arc.h"

struct mock { uint8_t data[512]; unsigned reads; };
static uint32_t read32(void *ctx,uint32_t off)
{
    struct mock *m=ctx; assert(!(off&3u) && off<=sizeof(m->data)-4); ++m->reads;
    return (uint32_t)m->data[off] | (uint32_t)m->data[off+1]<<8 |
        (uint32_t)m->data[off+2]<<16 | (uint32_t)m->data[off+3]<<24;
}
static void w16(struct mock *m,unsigned o,unsigned v) { m->data[o]=(uint8_t)v;m->data[o+1]=(uint8_t)(v>>8); }
static void w32(struct mock *m,unsigned o,unsigned v) { w16(m,o,v);w16(m,o+2,v>>16); }
static struct mock fixture(void)
{
    struct mock m={0};
    w32(&m,0,1);w32(&m,4,0x0c0330db);w16(&m,8,3);w16(&m,10,0);
    w16(&m,12,48);w16(&m,14,2);w16(&m,16,0x100);
    memcpy(m.data+144,"cx.lvl",6);w32(&m,152,4);w32(&m,160,0x30000);
    w16(&m,164,12);w16(&m,166,0);
    memcpy(m.data+168,"mx.lvl",6);w32(&m,184,0x30004);w16(&m,188,12);w16(&m,190,12);
    const unsigned levels[]={0,16,48,64,0,0};
    for(unsigned i=0;i<6;++i) { w16(&m,192+2*i,levels[i]);w16(&m,204+2*i,levels[i]); }
    return m;
}
static int lookup(struct mock *m,struct ceph_arc_resource *out)
{
    struct ceph_arc_db db={m,sizeof(m->data),read32};return ceph_arc_lookup(&db,"cx.lvl",out);
}
int main(void)
{
    struct mock m=fixture();struct ceph_arc_db db={&m,sizeof(m.data),read32};
    struct ceph_arc_resource cx,mx;
    assert(!lookup(&m,&cx) && cx.address==0x30000 && cx.count==4 && cx.levels[2]==48);
    assert(cx.priority[0]==4 && cx.aux_version==0x100);
    assert(!ceph_arc_lookup(&db,"mx.lvl",&mx));
    char resource_name[9];struct ceph_arc_resource reverse;
    assert(!ceph_arc_lookup_address(&db,0x30004,resource_name,&reverse) && !strcmp(resource_name,"mx.lvl") && reverse.address==0x30004);
    assert(ceph_arc_lookup_address(&db,0x30090,resource_name,&reverse)==CE_ARC_MISSING);
    unsigned c=99,n=99; assert(!ceph_arc_pair(&cx,&mx,48,64,&c,&n) && c==2 && n==2);
    mx.count=2;c=n=99;assert(ceph_arc_pair(&cx,&mx,48,64,&c,&n)==CE_ARC_RANGE && c==99 && n==99);
    assert(ceph_arc_select(&cx,65,100,&c)==CE_ARC_RANGE);
    assert(!ceph_arc_select(&cx,49,64,&c) && c==3);
    assert(ceph_arc_select(&cx,48,47,&c)==CE_ARC_INVALID);
    assert(ceph_arc_lookup(&db,"missing",&cx)==CE_ARC_MISSING && cx.count==0);
    assert(ceph_arc_lookup(&db,"toolongid",&cx)==CE_ARC_INVALID);
    assert(ceph_arc_lookup(NULL,"cx.lvl",&cx)==CE_ARC_INVALID);
    for(unsigned bytes=0;bytes<216;++bytes) {
        db.bytes=bytes;assert(ceph_arc_lookup(&db,"mx.lvl",&mx)!=0 && !mx.count);
    }
    db.bytes=512;
    const unsigned offsets[]={0,4,140,8,10,12,14,160,164,166};
    const unsigned bad[]={2,0,1,4,1,65534,257,0x40000,33,65534};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        m=fixture();if(offsets[i]==0||offsets[i]==4||offsets[i]==140||offsets[i]==160)
            w32(&m,offsets[i],bad[i]);else w16(&m,offsets[i],bad[i]);
        assert(lookup(&m,&cx)==CE_ARC_INVALID && !cx.count);
    }
    m=fixture();memcpy(m.data+168,"cx.lvl",6);assert(lookup(&m,&cx)==CE_ARC_INVALID);
    m=fixture();w16(&m,202,64);assert(lookup(&m,&cx)==CE_ARC_INVALID); /* data after padding */
    m=fixture();w16(&m,196,16);assert(lookup(&m,&cx)==CE_ARC_INVALID); /* duplicate */
    m=fixture();w16(&m,196,8);assert(lookup(&m,&cx)==CE_ARC_INVALID); /* descending */
    m=fixture();w16(&m,164,11);assert(lookup(&m,&cx)==CE_ARC_INVALID); /* odd aux size */
    m=fixture();w32(&m,184,0x30000);
    assert(ceph_arc_lookup_address(&db,0x30000,resource_name,&reverse)==CE_ARC_INVALID);
    puts("ARC command-db: bounds/duplicates/LE levels/padding/CX-MX dependency: PASS (no writes)");
}
