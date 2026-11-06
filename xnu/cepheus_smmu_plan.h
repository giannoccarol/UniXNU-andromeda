/* GMU SMMUv2 context plan: pure computation, NO global reset or MMIO backend.
 * A platform owner must reserve the chosen CB/SMR/ASID, stop the GMU, clean
 * tables, install with stream FAULT, synchronize scoped TLB, verify registers,
 * and only then enable translation. qcom,skip-init forbids wholesale reset.
 * Geometry/ACTLR, TBU clocks, secure ownership and lease remain platform work.
 */
#ifndef CEPHEUS_SMMU_PLAN_H
#define CEPHEUS_SMMU_PLAN_H
#include <stddef.h>
#include <stdint.h>
struct ceph_smmu_stream { uint32_t smr,s2cr; };
struct ceph_smmu_plan {
    uint32_t smr,s2cr_fault,s2cr_translate,cbar,cba2r,tcr,tcr2,mair0,mair1,sctlr;
    uint64_t ttbr0,ttbr1;
    unsigned cb,slot,asid;
};
static inline int ceph_smmu_overlap(uint32_t smr,unsigned sid,unsigned mask)
{
    return (smr&(1u<<31)) && !(((smr&0x7fffu)^sid)&~(((smr>>16)&0x7fffu)|mask)&0x7fffu);
}
static inline int ceph_smmu_gmu_plan(const struct ceph_smmu_stream *streams,unsigned slots,
    unsigned chosen,unsigned cb,unsigned numcb,unsigned sid,unsigned asid,
    uint64_t root_pa,uint32_t current_sctlr,struct ceph_smmu_plan *out)
{
    if(!out) return -1;
#ifdef __cplusplus
    *out={};
#else
    *out=(struct ceph_smmu_plan){0};
#endif
    if(!streams || !slots || slots>128 || chosen>=slots || !numcb || numcb>128 ||
        cb>=numcb || (sid!=4 && sid!=5) || !asid || asid>255 ||
        !root_pa || root_pa>=(1ULL<<40) || (root_pa&4095u) ||
        current_sctlr==UINT32_MAX || (current_sctlr&1)) return -1;
    const unsigned mask=0x400;
    for(unsigned i=0;i<slots;++i) {
        if(streams[i].smr==UINT32_MAX || streams[i].s2cr==UINT32_MAX) return -1;
        if(ceph_smmu_overlap(streams[i].smr,sid,mask)) return -2;
        /* Do not attach a second stream to a foreign context bank. */
        if((streams[i].smr&(1u<<31)) && ((streams[i].s2cr>>16)&3u)==0 &&
            (streams[i].s2cr&255u)==cb) return -2;
    }
    if((streams[chosen].smr&(1u<<31)) || ((streams[chosen].s2cr>>16)&3u)!=2) return -2;
    *out=(struct ceph_smmu_plan){
        .smr=(1u<<31)|(mask<<16)|sid,.s2cr_fault=2u<<16,.s2cr_translate=cb,
        .cbar=(1u<<16)|(3u<<8)|(15u<<12),.cba2r=1,
        /* 32-bit VA, TTBR1 disabled, 4K granule, normal NC table walks,
         * outer shareable. 40-bit PA in TTBCR2; 8-bit ASID (AS=0). */
        .tcr=(1u<<23)|(2u<<12)|32u,.tcr2=(7u<<15)|2u,
        .mair0=0x44ff,.mair1=0,
        /* abort-on-fault, no stall; IRQ disabled until platform handler exists */
        .sctlr=(3u<<22)|(1u<<12)|(1u<<8)|(1u<<5)|(1u<<2)|(1u<<1)|1u,
        .ttbr0=root_pa|((uint64_t)asid<<48),.ttbr1=(uint64_t)asid<<48,
        .cb=cb,.slot=chosen,.asid=asid,
    };
    return 0;
}
#endif
