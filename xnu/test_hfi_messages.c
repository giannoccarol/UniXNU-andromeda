#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_hfi_messages.h"
int main(void)
{
    struct ceph_arc_resource rail={.levels={0,16,48,64,128},.count=5};
    struct ceph_hfi_opp gx[]={{0,0},{215000000,64},{309110000,128}},cx[]={{0,0},{200000000,48}};
    uint32_t out[161];for(unsigned i=0;i<161;++i)out[i]=0xdeadbeef;
    assert(ceph_hfi_perf(out,59,gx,3,cx,2,&rail,&rail,&rail)==59);
    assert(out[0]==0x3b04 && out[1]==3 && out[2]==2 && out[3]==0 && out[4]==UINT32_MAX);
    assert(out[6]==0x00400303 && out[7]==UINT32_MAX && out[8]==215000);
    assert(out[9]==0x00800404 && out[11]==309110 && out[12]==0);
    assert(out[51]==0 && out[52]==0 && out[53]==0x00300202 && out[54]==200000);
    assert(out[55]==0 && out[58]==0 && out[59]==0xdeadbeef);
    struct ceph_arc_resource actual_cx={.levels={0,16,64,128,192,256,384,416},.count=8};
    struct ceph_arc_resource actual_mx={.levels={0,16,128,192,256,320,384,416},.count=8};
    uint32_t rounded=0;assert(!ceph_hfi_vote(&actual_cx,&actual_mx,48,&rounded) && rounded==0x00400202);
    actual_mx.count=2;rounded=0xdeadbeef;
    assert(ceph_hfi_vote(&actual_cx,&actual_mx,48,&rounded)==CE_HFI_INVALID && rounded==0xdeadbeef);
    uint32_t old[59];memcpy(old,out,sizeof(old));
    gx[2].hz=215000000;assert(ceph_hfi_perf(out,59,gx,3,cx,2,&rail,&rail,&rail)==CE_HFI_INVALID);
    assert(!memcmp(old,out,sizeof(old)));gx[2].hz=309110000;
    struct ceph_arc_resource shortmx={.levels={0,16,48},.count=3};
    assert(ceph_hfi_perf(out,59,gx,3,cx,2,&rail,&rail,&shortmx)==CE_HFI_INVALID);
    assert(ceph_hfi_perf(out,58,gx,3,cx,2,&rail,&rail,&rail)==CE_HFI_INVALID);
    struct ceph_hfi_bw bw={.levels=2,.cnoc_count=2,.ddr_count=2,.cnoc_wait=2,.ddr_wait=3,
        .cnoc_addr={0x50000,0x50004},.ddr_addr={0x50008,0x5000c},
        .cnoc_data={{1,2},{3,4}},.ddr_data={{5,6},{7,8}}};
    assert(ceph_hfi_bandwidth(out,160,&bw)==160 && out[0]==0xa003 && out[160]==0xdeadbeef);
    assert(out[4]==2 && out[5]==3 && out[6]==0x50000 && out[7]==0x50004 && out[8]==0);
    assert(out[12]==1 && out[13]==2 && out[18]==3 && out[19]==4);
    assert(out[24]==0x50008 && out[25]==0x5000c && out[32]==5 && out[40]==7 && out[159]==0);
    bw.cnoc_wait=4;assert(ceph_hfi_bandwidth(out,160,&bw)==CE_HFI_INVALID);bw.cnoc_wait=2;
    bw.ddr_addr[1]=bw.ddr_addr[0];assert(ceph_hfi_bandwidth(out,160,&bw)==CE_HFI_INVALID);
    bw.ddr_addr[1]=0x40000;assert(ceph_hfi_bandwidth(out,160,&bw)==CE_HFI_INVALID);
    struct ceph_hfi_acd acd={.enable_mask=5,.stride=2,.levels=2,.data={11,22,33,44}};
    assert(ceph_hfi_acd_table(out,17,&acd)==17 && out[0]==0x1107 && out[1]==1 && out[8]==44 && out[16]==0);
    acd.enable_mask=7;assert(ceph_hfi_acd_table(out,17,&acd)==CE_HFI_INVALID);
    assert(ceph_hfi_fw_version(out,2,2,8)==2 && out[0]==0x201 && out[1]==0x20080000);
    assert(ceph_hfi_fw_version(out,2,1,8)==CE_HFI_INVALID);
    assert(ceph_hfi_feature(out,4,7,1,0x3)==4 && out[0]==0x40b && out[3]==3);
    assert(ceph_hfi_feature(out,4,31,0,0)==CE_HFI_INVALID);
    assert(ceph_hfi_core_start(out,2)==2 && out[0]==0x20e && !out[1]);
    /* Startup vote names only a GPU level; bw stays the 0xFF sentinel. */
    assert(ceph_hfi_dcvs(out,4,1,CE_HFI_DCVS_INVALID)==4 && out[0]==0x41e && out[1]==1 &&
        out[2]==1 && out[3]==0xff);
    assert(ceph_hfi_dcvs(out,4,16,0)==CE_HFI_INVALID && ceph_hfi_dcvs(out,4,1,16)==CE_HFI_INVALID);
    assert(ceph_hfi_start(out,1)==1 && out[0]==0x10a && ceph_hfi_start(out,0)==CE_HFI_INVALID);
    assert(ceph_hfi_slumber(out,3,3,1,12,8)==3 && out[0]==0x321 && out[1]==3 && out[2]==1);
    assert(ceph_hfi_slumber(out,3,12,1,12,8)==CE_HFI_INVALID);
    assert(ceph_hfi_slumber(out,3,3,8,12,8)==CE_HFI_INVALID);
    puts("HFI messages: A640 fixed layouts, ARC index packing, real-table bounds and zero tails PASS");
}
