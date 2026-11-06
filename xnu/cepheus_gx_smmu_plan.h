/* Pure GX single-context stage1 plan, NOT GMU SID5 and NOT an install API.
 * Exact saved FDT gfx3d_user SID0/mask0x401, AArch6439VA/40PA, NC walks/leaves.
 * All global/private CP objects would share this reserved context; no per-app
 * TTBR switching/CP aperture SCM call is implemented or bypassed here.
 * Secure SID2/mask0x400 is never claimed. Real secure CB/ASID/TBU ownership and
 * DMA retirement must be established separately before any hardware write.
 */
#ifndef CEPHEUS_GX_SMMU_PLAN_H
#define CEPHEUS_GX_SMMU_PLAN_H
#include "cepheus_smmu_plan.h"
#include "cepheus_smmu_geometry.h"
struct ceph_gx_smmu_plan {struct ceph_smmu_plan context;uint32_t actlr;};
static inline int ceph_gx_smmu_plan(const struct ceph_smmu_stream *streams,unsigned count,
    const struct ceph_smmu_geometry *g,unsigned slot,unsigned cb,unsigned asid,uint64_t root,
    uint32_t current_sctlr,struct ceph_gx_smmu_plan *out)
{
    if(!out || !streams || !g || g->bytes!=0x10000 || g->page_shift!=12 || g->cb_base!=0x8000 ||
        g->gr1!=0x1000 || count!=g->groups || !count || count>128 || slot>=count ||
        g->va_bits<39 || g->ipa_bits<40 || g->pa_bits<40 || g->sid_bits<11 || g->sid_bits>15 ||
        cb<g->s2_contexts || cb>=g->contexts || !g->contexts || g->contexts>8 ||
        !asid || asid>255 || !root || (root&4095u) || root>=(1ULL<<40) ||
        current_sctlr==UINT32_MAX || (current_sctlr&1))return -1;
    for(unsigned i=0;i<count;++i) {
        if(streams[i].smr==UINT32_MAX || streams[i].s2cr==UINT32_MAX)return -1;
        if(ceph_smmu_overlap(streams[i].smr,0,0x401))return -2;
        if((streams[i].smr&(1u<<31)) && ((streams[i].s2cr>>16)&3)==0 && (streams[i].s2cr&255)==cb)return -2;
    }
    if((streams[slot].smr&(1u<<31)) || streams[slot].s2cr!=(2u<<16))return -2;
    /* Same architecture fields as GMU except SID/mask and T0SZ25 for39bit.
     * Abort-on-fault diagnostic policy, no IRQ/stall until handler exists.
     * This NC baseline deliberately doesn't request optional LLC walk hints. */
    struct ceph_smmu_plan p={.smr=0x84010000,.s2cr_fault=2u<<16,.s2cr_translate=cb,
        .cbar=0x1f300,.cba2r=1,.tcr=0x802019,.tcr2=0x38002,.mair0=0x44ff,.mair1=0,
        .sctlr=0xc01127,.ttbr0=root|((uint64_t)asid<<48),.ttbr1=(uint64_t)asid<<48,
        .cb=cb,.slot=slot,.asid=asid};
    *out=(struct ceph_gx_smmu_plan){p,0x303};return 0;
}
#endif
