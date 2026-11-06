/* Fixed Cepheus A640v2 profile, source-bound to saved FDT SHA below. No I/O,
 * guessed BCM addresses/units, inferred eFuse, or platform ownership grant.
 * The future platform must verify its live FDT/profile and supply a stable
 * cmd-db snapshot plus the actual QFPROM word; this module does not discover
 * them. GMU 200MHz VLVL48 is from gpucc-sm8150.c/VDD_MIN, not GPU OPP phandle.
 */
#ifndef CEPHEUS_GPU_CONFIG_H
#define CEPHEUS_GPU_CONFIG_H
#include "cepheus_gpu_bw.h"
#include "cepheus_gpu_pdc.h"
#include "cepheus_hfi_boot.h"
#define CE_GPU_CONFIG_FDT_SHA256 "bdd364a16ceabe9960e128103162e5280b150303624a123dfd71e871f9a43274"
static const struct ceph_hfi_opp ceph_config_gx[]={
    {0,0},{257000000,64},{345000000,128},{427000000,192},
    {499200000,224},{585000000,256},{675000000,320},{770000000,320}};
static const struct ceph_hfi_opp ceph_config_cx[]={{0,0},{200000000,48}};
/* msm_bus_of.c KBTOB is *1000ULL, NOT *1024 or a 32-bit multiplication. */
static const struct ceph_bw_level ceph_config_ddr[]={
    {0,0},{0,795000000ULL},{0,1195000000ULL},{0,1799000000ULL},
    {0,2183000000ULL},{0,2719000000ULL},{0,3067000000ULL},{0,4063000000ULL},
    {0,5407000000ULL},{0,6215000000ULL},{0,7211000000ULL},{0,8363000000ULL}};
static const struct ceph_bw_level ceph_config_cnoc[]={{0,0},{0,100000}};
/* Nodes with no BCM contribute no command. Preserve FDT route order for
 * stable VCD sorting; ACV is a bit vote at index2, not a normalized bandwidth. */
static const char *const ceph_config_ddr_names[]={"SH0","MC0","ACV"};
static const char *const ceph_config_cnoc_names[]={"SH2","SN15","SN3"};
static const struct ceph_bw_node ceph_config_ddr_nodes[]={{0,16,4},{1,4,4},{2,4,4}};
static const struct ceph_bw_node ceph_config_cnoc_nodes[]={{0,8,1},{1,8,1},{2,8,1}};
static const struct ceph_hfi_acd ceph_config_acd={0x7e,2,6,
    {2687328253u,30225,2687328253u,26897,2687328253u,24849,
     2687328253u,24593,2150457341u,21521,2150457341u,21521}};
struct ceph_gpu_config {
    struct ceph_gpu_bin_policy policy;
    struct ceph_arc_resource gfx,cx,mx;
    struct ceph_hfi_bw bw;
    struct ceph_pdc_input pdc;
    unsigned initial_freq,initial_bw; /* ascending HFI indices, NOT a vote */
};
enum {CE_CONFIG_RAILS=-20,CE_CONFIG_DDR_BCM=-21,CE_CONFIG_CNOC_BCM=-22,
    CE_CONFIG_ROUTES=-23,CE_CONFIG_PDC_RESOURCE=-24,CE_CONFIG_WIRE=-25};
static inline int ceph_gpu_config_build(const struct ceph_arc_db *db,uint32_t chipid,
    uint32_t qfprom,struct ceph_gpu_config *out)
{
    if(!out)return -1;
    struct ceph_gpu_config c={0};
    if(ceph_gpu_bin_policy(chipid,qfprom,&c.policy) ||
        ceph_arc_lookup(db,"gfx.lvl",&c.gfx) || ceph_arc_lookup(db,"cx.lvl",&c.cx) ||
        ceph_arc_lookup(db,"mx.lvl",&c.mx) || c.gfx.address==c.cx.address ||
        c.gfx.address==c.mx.address || c.cx.address==c.mx.address)return CE_CONFIG_RAILS;
    struct ceph_bcm_resource ddr[3],cnoc[3];
    for(unsigned i=0;i<3;++i) {
        if(i==2?ceph_bcm_lookup_acv(db,&ddr[i]):ceph_bcm_lookup(db,ceph_config_ddr_names[i],&ddr[i]))return CE_CONFIG_DDR_BCM;
        if(ceph_bcm_lookup(db,ceph_config_cnoc_names[i],&cnoc[i]))return CE_CONFIG_CNOC_BCM;
    }
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
        if(ddr[i].address==cnoc[j].address)return CE_CONFIG_ROUTES;
    struct ceph_bw_route dr={ddr,3,ceph_config_ddr_nodes,3,2,8000};
    struct ceph_bw_route cr={cnoc,3,ceph_config_cnoc_nodes,3,-1,8000};
    if(ceph_gpu_bw_table(&cr,ceph_config_cnoc,&dr,ceph_config_ddr,12,&c.bw))return CE_CONFIG_ROUTES;
    c.pdc.chipid=chipid;c.pdc.cfg_pa=0xb290000;c.pdc.seq_pa=0xb490000;
    static const uint32_t addresses[]={0x30010,0x30000,0x30090};
    for(unsigned i=0;i<3;++i) {
        char name[9];
        if(ceph_arc_lookup_address(db,addresses[i],name,&c.pdc.resources[i]))return CE_CONFIG_PDC_RESOURCE;
    }
    struct ceph_pdc_plan pdc;
    uint32_t perf[59],acd[17];
    if(ceph_pdc_plan_a640v2(&c.pdc,&pdc) ||
        ceph_hfi_perf(perf,59,ceph_config_gx,c.policy.gx_levels,ceph_config_cx,2,&c.gfx,&c.cx,&c.mx)!=59 ||
        (c.policy.acd && ceph_hfi_acd_table(acd,17,&ceph_config_acd)!=17))return CE_CONFIG_WIRE;
    /* Both FDT bins start at257MHz (ascending index1), BW index3. */
    c.initial_freq=1;c.initial_bw=3;*out=c;return 0;
}
/* c must remain immutable and address-stable throughout the HFI session. No
 * self-pointers in the value returned by build(): copying it is safe pre-bind. */
static inline int ceph_gpu_config_hfi(const struct ceph_gpu_config *c,uint32_t version,
    struct ceph_hfi_boot_config *out)
{
    if(!c || !out || !ceph_gpu_bin_policy_valid(&c->policy) || (version>>28)!=2)return -1;
    *out=(struct ceph_hfi_boot_config){version,0,c->policy.throttle_mask,
        ceph_config_gx,ceph_config_cx,c->policy.gx_levels,2,&c->gfx,&c->cx,&c->mx,
        &c->bw,c->policy.acd?&ceph_config_acd:NULL,&c->policy};return 0;
}
#endif
