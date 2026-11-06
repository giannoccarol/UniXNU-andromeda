/* Build HFI bandwidth input from a SINGLE known route and source-bound BCM
 * metadata. No live vote replacement. AB/IB are bytes/sec, route node widths
 * and aggregation-port counts come from actual FDT. ACV is apps-RSC bit 3,
 * never normalized as bandwidth; unsupported RSC/special BCM is refused.
 */
#ifndef CEPHEUS_GPU_BW_H
#define CEPHEUS_GPU_BW_H
#include "cepheus_bcm.h"
#include "cepheus_hfi_messages.h"
struct ceph_bw_node {unsigned bcm_index;uint32_t buswidth,ports;};
struct ceph_bw_route {
    const struct ceph_bcm_resource *bcms;unsigned bcm_count;
    const struct ceph_bw_node *nodes;unsigned node_count;
    int acv_index; /* -1 means no ACV; caller validates identity from FDT name */
    uint32_t rsc_id; /* exact FDT cell-id; apps=8000, not DRV number */
};
struct ceph_bw_level {uint64_t ab,ib;};
static inline int ceph_bw_route_valid(const struct ceph_bw_route *r)
{
    if(!r || !r->bcms || !r->nodes || !r->bcm_count || r->bcm_count>8 ||
        !r->node_count || r->node_count>16 || r->acv_index < -1 ||
        r->acv_index>=(int)r->bcm_count || r->rsc_id!=8000)return 0;
    unsigned used=0;
    for(unsigned i=0;i<r->bcm_count;++i) {
        const struct ceph_bcm_resource *b=&r->bcms[i];
        if((b->address>>16)!=5 || (b->address&3) || b->vcd>=16 ||
            ((int)i!=r->acv_index && (!b->unit || !b->width)))return 0;
        for(unsigned j=0;j<i;++j)if(r->bcms[j].address==b->address)return 0;
    }
    for(unsigned i=0;i<r->node_count;++i) {
        const struct ceph_bw_node *n=&r->nodes[i];
        if(n->bcm_index>=r->bcm_count || !n->buswidth || !n->ports || n->buswidth>UINT32_MAX/n->ports)return 0;
        used|=1u<<n->bcm_index;
    }
    return used==((1u<<r->bcm_count)-1);
}
static inline int ceph_bw_level_commands(const struct ceph_bw_route *r,const struct ceph_bw_level *l,
    struct ceph_bcm_command *commands,uint32_t *wait)
{
    if(!ceph_bw_route_valid(r) || !l || !commands || !wait)return -1;
    struct ceph_bcm_vote v[8]={0};
    for(unsigned i=0;i<r->bcm_count;++i)v[i].resource=r->bcms[i];
    for(unsigned i=0;i<r->node_count;++i) {
        const struct ceph_bw_node *n=&r->nodes[i];unsigned j=n->bcm_index;
        if((int)j==r->acv_index)continue;
        uint64_t x,y;
        if(ceph_bcm_normalize(&r->bcms[j],l->ab,l->ib,n->buswidth,n->ports,&x,&y))return -1;
        if(x>v[j].x)v[j].x=x;
        if(y>v[j].y)v[j].y=y;
    }
    if(r->acv_index>=0) {v[r->acv_index].x=0;v[r->acv_index].y=(l->ab||l->ib)?8:0;}
    return ceph_bcm_commands_kind(v,r->bcm_count,commands,8,wait,r->acv_index);
}
static inline int ceph_gpu_bw_table(const struct ceph_bw_route *cnoc,const struct ceph_bw_level cnoc_levels[2],
    const struct ceph_bw_route *ddr,const struct ceph_bw_level *ddr_levels,unsigned count,struct ceph_hfi_bw *out)
{
    if(!out || !cnoc_levels || !ddr_levels || count<2 || count>16 ||
        !ceph_bw_route_valid(cnoc) || cnoc->bcm_count>6 || !ceph_bw_route_valid(ddr))return -1;
    if(cnoc_levels[0].ab || cnoc_levels[0].ib || ddr_levels[0].ab || ddr_levels[0].ib)return -1;
    struct ceph_hfi_bw b={0};b.levels=count;b.cnoc_count=cnoc->bcm_count;b.ddr_count=ddr->bcm_count;
    for(unsigned which=0;which<2;++which) {
        const struct ceph_bw_route *r=which?ddr:cnoc;
        const struct ceph_bw_level *levels=which?ddr_levels:cnoc_levels;
        unsigned n=which?count:2;
        for(unsigned i=0;i<n;++i) {
            if(i && (levels[i].ab<levels[i-1].ab || levels[i].ib<levels[i-1].ib ||
                !(levels[i].ab||levels[i].ib)))return -1;
            struct ceph_bcm_command commands[8];uint32_t wait;
            int encoded=ceph_bw_level_commands(r,&levels[i],commands,&wait);
            if(encoded<=0 || encoded>8 || (unsigned)encoded!=r->bcm_count)return -1;
            uint32_t *addresses=which?b.ddr_addr:b.cnoc_addr;
            uint32_t *data=which?b.ddr_data[i]:b.cnoc_data[i];
            uint32_t *wait_out=which?&b.ddr_wait:&b.cnoc_wait;
            if(i && *wait_out!=wait)return -1;
            *wait_out=wait;
            for(unsigned c=0;c<r->bcm_count;++c) {
                if(i && addresses[c]!=commands[c].address)return -1;
                addresses[c]=commands[c].address;data[c]=commands[c].data;
            }
        }
    }
    *out=b;return 0;
}
#endif
