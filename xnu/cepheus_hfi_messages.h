/* A640 HFI v2 wire encoders; offsets/counts follow kgsl_hfi.h, not packed host
 * structs. Callers supply source-validated OPP/BCM tables and actual cmd-db ARC
 * data. No hardcoded rail indices, fictitious BW votes or firmware execution.
 */
#ifndef CEPHEUS_HFI_MESSAGES_H
#define CEPHEUS_HFI_MESSAGES_H
#include "cepheus_hfi.h"
#include "cepheus_arc.h"
enum { CE_HFI_PERF_WORDS=59, CE_HFI_BW_WORDS=160, CE_HFI_ACD_WORDS=17 };
struct ceph_hfi_opp { uint32_t hz, raw_vlvl; };
struct ceph_hfi_bw {
    unsigned levels, cnoc_count, ddr_count;
    uint32_t cnoc_wait, ddr_wait;
    uint32_t cnoc_addr[6], cnoc_data[2][6], ddr_addr[8], ddr_data[16][8];
};
struct ceph_hfi_acd { unsigned enable_mask, stride, levels; uint32_t data[12]; };
static inline int ceph_hfi_vote(const struct ceph_arc_resource *rail,
    const struct ceph_arc_resource *mx, unsigned vlvl, uint32_t *vote)
{
    unsigned p,s;
    if (!vote || ceph_arc_select(rail,vlvl,UINT16_MAX,&p) ||
        ceph_arc_select(mx,rail->levels[p],UINT16_MAX,&s)) return CE_HFI_INVALID;
    *vote=((uint32_t)rail->levels[p]<<16)|(s<<8)|p;
    return 0;
}
static inline int ceph_hfi_opps_valid(const struct ceph_hfi_opp *opp,unsigned count,unsigned max)
{
    if (!opp || count<2 || count>max || opp[0].hz || opp[0].raw_vlvl) return 0;
    for(unsigned i=1;i<count;++i)
        if (!opp[i].hz || opp[i].hz%1000 || opp[i].hz<=opp[i-1].hz ||
            !opp[i].raw_vlvl || opp[i].raw_vlvl>UINT16_MAX ||
            opp[i].raw_vlvl<opp[i-1].raw_vlvl) return 0;
    return 1;
}
static inline int ceph_hfi_perf(uint32_t *out,unsigned capacity,
    const struct ceph_hfi_opp *gx,unsigned ng,const struct ceph_hfi_opp *cx,unsigned nc,
    const struct ceph_arc_resource *gfx,const struct ceph_arc_resource *cxl,
    const struct ceph_arc_resource *mx)
{
    if (!out || capacity<CE_HFI_PERF_WORDS || !ceph_hfi_opps_valid(gx,ng,16) ||
        !ceph_hfi_opps_valid(cx,nc,4) || !gfx || !cxl || !mx ||
        !gfx->count || !cxl->count || !mx->count || gfx->levels[0] ||
        cxl->levels[0] || mx->levels[0]) return CE_HFI_INVALID;
    uint32_t message[CE_HFI_PERF_WORDS]={0};
    message[0]=ceph_hfi_header(4,CE_HFI_PERF_WORDS,0,0);message[1]=ng;message[2]=nc;
    for(unsigned i=0;i<ng;++i) {
        if(ceph_hfi_vote(gfx,mx,gx[i].raw_vlvl,&message[3+3*i])) return CE_HFI_INVALID;
        message[4+3*i]=UINT32_MAX;message[5+3*i]=gx[i].hz/1000;
    }
    for(unsigned i=0;i<nc;++i) {
        if(ceph_hfi_vote(cxl,mx,cx[i].raw_vlvl,&message[51+2*i])) return CE_HFI_INVALID;
        message[52+2*i]=cx[i].hz/1000;
    }
    for(unsigned i=0;i<CE_HFI_PERF_WORDS;++i)out[i]=message[i];
    return CE_HFI_PERF_WORDS;
}
static inline int ceph_hfi_bcm_addrs(const uint32_t *addr,unsigned count,unsigned max,uint32_t wait)
{
    if (!count || count>max || (wait&~((1u<<count)-1))) return 0;
    for(unsigned i=0;i<count;++i) {
        if((addr[i]>>16)!=5 || (addr[i]&3u)) return 0;
        for(unsigned j=0;j<i;++j)if(addr[i]==addr[j])return 0;
    }
    return 1;
}
static inline int ceph_hfi_bandwidth(uint32_t *out,unsigned capacity,const struct ceph_hfi_bw *b)
{
    if(!out || capacity<CE_HFI_BW_WORDS || !b || b->levels<2 || b->levels>16 ||
        !ceph_hfi_bcm_addrs(b->cnoc_addr,b->cnoc_count,6,b->cnoc_wait) ||
        !ceph_hfi_bcm_addrs(b->ddr_addr,b->ddr_count,8,b->ddr_wait)) return CE_HFI_INVALID;
    uint32_t message[CE_HFI_BW_WORDS]={0};
    message[0]=ceph_hfi_header(3,CE_HFI_BW_WORDS,0,0);message[1]=b->levels;
    message[2]=b->cnoc_count;message[3]=b->ddr_count;
    message[4]=b->cnoc_wait;message[5]=b->ddr_wait;
    for(unsigned i=0;i<b->cnoc_count;++i) {
        message[6+i]=b->cnoc_addr[i];
        for(unsigned j=0;j<2;++j)message[12+j*6+i]=b->cnoc_data[j][i];
    }
    for(unsigned i=0;i<b->ddr_count;++i) {
        message[24+i]=b->ddr_addr[i];
        for(unsigned j=0;j<b->levels;++j)message[32+j*8+i]=b->ddr_data[j][i];
    }
    for(unsigned i=0;i<CE_HFI_BW_WORDS;++i)out[i]=message[i];
    return CE_HFI_BW_WORDS;
}
static inline int ceph_hfi_acd_table(uint32_t *out,unsigned capacity,const struct ceph_hfi_acd *a)
{
    if(!out || capacity<CE_HFI_ACD_WORDS || !a || !a->levels || a->levels>6 ||
        !a->stride || a->stride>2 || (a->enable_mask&~0xffffu)) return CE_HFI_INVALID;
    unsigned bits=0;for(unsigned v=a->enable_mask;v;v>>=1)bits+=v&1u;
    if(bits!=a->levels)return CE_HFI_INVALID;
    uint32_t message[CE_HFI_ACD_WORDS]={0};
    message[0]=ceph_hfi_header(7,CE_HFI_ACD_WORDS,0,0);message[1]=1;
    message[2]=a->enable_mask;message[3]=a->stride;message[4]=a->levels;
    for(unsigned i=0;i<a->stride*a->levels;++i)message[5+i]=a->data[i];
    for(unsigned i=0;i<CE_HFI_ACD_WORDS;++i)out[i]=message[i];
    return CE_HFI_ACD_WORDS;
}
static inline int ceph_hfi_fw_version(uint32_t *out,unsigned capacity,unsigned major,unsigned minor)
{
    if(!out || capacity<2 || major!=2 || minor>4095)return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(1,2,0,0);out[1]=(major<<28)|(minor<<16);return 2;
}
static inline int ceph_hfi_feature(uint32_t *out,unsigned capacity,unsigned feature,unsigned enable,uint32_t data)
{
    if(!out || capacity<4 || (feature!=1 && feature!=7 && feature!=12) || enable>1)
        return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(11,4,0,0);out[1]=feature;out[2]=enable;out[3]=data;return 4;
}
static inline int ceph_hfi_core_start(uint32_t *out,unsigned capacity)
{
    if(!out || capacity<2)return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(14,2,0,0);out[1]=0;return 2;
}
/* H2F_MSG_GX_BW_PERF_VOTE after CORE_START. Uploading the PERF/BW tables is
 * NOT a vote; this message is what actually selects a level.
 * KGSL's startup path (kgsl_pwrctrl_set_default_gpu_pwrlevel ->
 * kgsl_clk_set_rate -> gmu_core_dcvs_set(level, INVALID_DCVS_IDX)) only names
 * a GPU level: bus_level stays INVALID_DCVS_IDX, and gmu_dcvs_set() then skips
 * the bw assignment (`bus_level < num_bwlevels && bus_level > 0`), leaving the
 * 0xFF sentinel in the wire field. Emitting a real bandwidth index here would
 * be a vote KGSL never makes at startup. Both fields are bounded by
 * MAX_GX_LEVELS on the source side, not by the BW table height. */
enum {CE_HFI_DCVS_INVALID=0xff};
/* H2F_MSG_START: A640/A680-only GX bring-up kick, sent by
 * a6xx_gmu_hfi_start_msg() AFTER the OOB keepalive is held and BEFORE any GX
 * register programming. Header-only payload (sizeof(hfi_start_cmd)==4). */
static inline int ceph_hfi_start(uint32_t *out,unsigned capacity)
{
    if(!out || capacity<1)return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(10,1,0,0);return 1;
}
static inline int ceph_hfi_dcvs(uint32_t *out,unsigned capacity,unsigned freq,unsigned bw)
{
    if(!out || capacity<4 || freq>=16 || (bw>=16 && bw!=CE_HFI_DCVS_INVALID))return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(30,4,0,0);out[1]=1; /* DCVS_ACK_BLOCK */
    out[2]=freq;out[3]=bw;return 4;
}
/* HFI ACK is only acceptance, not proof that GX has entered slumber. */
static inline int ceph_hfi_slumber(uint32_t *out,unsigned capacity,unsigned bw,unsigned freq,
    unsigned bw_levels,unsigned gx_levels)
{
    if(!out || capacity<3 || bw_levels<2 || bw_levels>16 || gx_levels<2 || gx_levels>16 ||
        bw>=bw_levels || freq>=gx_levels)return CE_HFI_INVALID;
    out[0]=ceph_hfi_header(33,3,0,0);out[1]=bw;out[2]=freq;return 3;
}
#endif
