#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_bw.h"
int main(void)
{
    const struct ceph_bcm_resource ddr[]={{0x50000,1000,16,2},{0x50004,1000,4,1},{0x50008,1,1,1}};
    const struct ceph_bw_node dnodes[]={{0,16,4},{1,4,4},{2,4,4}};
    const struct ceph_bcm_resource cnoc[]={{0x50010,1000,8,3},{0x50014,1000,8,3},{0x50018,1000,8,4}};
    const struct ceph_bw_node cnodes[]={{0,8,1},{1,8,1},{2,8,1}};
    struct ceph_bw_route d={ddr,3,dnodes,3,2,8000},c={cnoc,3,cnodes,3,-1,8000};
    const struct ceph_bw_level dl[]={{0,0},{0,1000000},{2000000,3000000}},cl[]={{0,0},{0,100000}};
    struct ceph_hfi_bw b;memset(&b,0xa5,sizeof(b));
    assert(!ceph_gpu_bw_table(&c,cl,&d,dl,3,&b));
    assert(b.ddr_addr[0]==0x50004 && b.ddr_addr[1]==0x50008 && b.ddr_addr[2]==0x50000 && b.ddr_wait==6);
    assert(b.cnoc_wait==6 && b.cnoc_count==3 && b.ddr_count==3);
    assert(b.ddr_data[0][0]==0 && b.ddr_data[0][1]==0x40000000 && b.ddr_data[0][2]==0x40000000);
    assert(b.ddr_data[1][0]==0x200003e8 && b.ddr_data[1][1]==0x60000008 && b.ddr_data[1][2]==0x600003e8);
    assert(b.ddr_data[2][0]==(0x20000000u|(500u<<14)|3000u));
    assert(b.cnoc_data[1][0]==0x20000064 && b.cnoc_data[1][1]==0x60000064 && b.cnoc_data[1][2]==0x60000064);
    uint32_t wire[160];assert(ceph_hfi_bandwidth(wire,160,&b)==160 && wire[5]==6);
    struct ceph_hfi_bw saved=b;
    d.bcm_count=UINT32_MAX;assert(ceph_gpu_bw_table(&c,cl,&d,dl,3,&b)<0 && !memcmp(&saved,&b,sizeof(b)));d.bcm_count=3;
    d.rsc_id=8001;assert(ceph_gpu_bw_table(&c,cl,&d,dl,3,&b)<0 && !memcmp(&saved,&b,sizeof(b)));d.rsc_id=8000;
    d.acv_index=3;assert(ceph_gpu_bw_table(&c,cl,&d,dl,3,&b)<0);d.acv_index=2;
    struct ceph_bw_level bad[]={{0,0},{0,1000},{0,500}};
    assert(ceph_gpu_bw_table(&c,cl,&d,bad,3,&b)<0);
    struct ceph_bw_node partial[]={{0,16,4},{1,4,4}};d.nodes=partial;d.node_count=2;
    assert(ceph_gpu_bw_table(&c,cl,&d,dl,3,&b)<0);
    struct ceph_bcm_resource bit_ddr[3];memcpy(bit_ddr,ddr,sizeof(ddr));bit_ddr[2].unit=bit_ddr[2].width=0;
    d.nodes=dnodes;d.node_count=3;d.bcms=bit_ddr;
    assert(!ceph_gpu_bw_table(&c,cl,&d,dl,3,&b) && b.ddr_data[1][1]==0x60000008);
    d.acv_index=-1;assert(ceph_gpu_bw_table(&c,cl,&d,dl,3,&b)<0);
    puts("GPU BW tables: route widths, apps-ACV bit, VCD-stable HFI rows and malformed topology PASS (synthetic metadata)");
}
