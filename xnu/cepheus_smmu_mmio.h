/* Named SMMUv2 MMIO adapter for the install/detach engines. Caller supplies an
 * already-mapped aperture and live capability-decoded geometry; no mapping or
 * hardware discovery is performed here. lease() verifies the exact power/TBU,
 * CB/SMR/ASID reservation and GMU park, not just a caller readiness flag.
 * No raw register offset API, no global control/reset/TLBI or secure CB writes.
 */
#ifndef CEPHEUS_SMMU_MMIO_H
#define CEPHEUS_SMMU_MMIO_H
#include "cepheus_smmu_install.h"
#include "cepheus_smmu_geometry.h"
struct ceph_smmu_mmio {
    volatile uint8_t *base;
    size_t mapped_bytes;
    struct ceph_smmu_geometry geometry;
    struct ceph_smmu_plan plan;
    uint32_t actlr;
    void *owner;
    int (*lease)(void *);
};
static inline void ceph_smmu_mmio_order(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb osh" ::: "memory");
#elif !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error "SMMU MMIO adapter requires arm64"
#endif
}
static inline int ceph_smmu_mmio_offset(const struct ceph_smmu_mmio *m,enum ceph_smmu_reg reg,uint32_t *offset,unsigned *width)
{
    if(!m || !m->base || ((uintptr_t)m->base&7u) || !offset || !width ||
        !ceph_smmu_plan_canonical(&m->plan) || m->actlr!=0x303 ||
        m->geometry.bytes!=m->mapped_bytes || m->geometry.bytes!=0x10000 ||
        m->geometry.page_shift!=12 || m->geometry.cb_base!=0x8000 || m->geometry.gr1!=0x1000 ||
        m->geometry.va_bits<32 || m->geometry.ipa_bits<40 || m->geometry.pa_bits<40 ||
        m->plan.slot>=m->geometry.groups || m->plan.cb<m->geometry.s2_contexts ||
        m->plan.cb>=m->geometry.contexts || !m->lease)return -1;
    uint32_t off=0;*width=4;
    switch(reg) {
    case CE_SM_SMR: off=0x800+4*m->plan.slot;break;
    case CE_SM_S2CR: off=0xc00+4*m->plan.slot;break;
    case CE_SM_CBAR: off=m->geometry.gr1+4*m->plan.cb;break;
    case CE_SM_CBA2R: off=m->geometry.gr1+0x800+4*m->plan.cb;break;
    default: {
        uint32_t cbreg;
        switch(reg) {
        case CE_SM_SCTLR:cbreg=0;break;
        case CE_SM_ACTLR:cbreg=4;break;
        case CE_SM_TCR2:cbreg=0x10;break;
        case CE_SM_TTBR0:cbreg=0x20;*width=8;break;
        case CE_SM_TTBR1:cbreg=0x28;*width=8;break;
        case CE_SM_TCR:cbreg=0x30;break;
        case CE_SM_MAIR0:cbreg=0x38;break;
        case CE_SM_MAIR1:cbreg=0x3c;break;
        case CE_SM_FSR:cbreg=0x58;break;
        case CE_SM_TLBIASID:cbreg=0x610;break;
        case CE_SM_TLBSYNC:cbreg=0x7f0;break;
        case CE_SM_TLBSTATUS:cbreg=0x7f4;break;
        default:return -1;
        }
        if(ceph_smmu_cb_offset(&m->geometry,m->plan.cb,cbreg,&off))return -1;
    }}
    if(off>m->mapped_bytes || *width>m->mapped_bytes-off || off&(*width-1))return -1;
    *offset=off;return 0;
}
static inline int ceph_smmu_mmio_read(void *ctx,enum ceph_smmu_reg reg,uint64_t *value)
{
    struct ceph_smmu_mmio *m=ctx;uint32_t offset;unsigned width;
    if(!value || ceph_smmu_mmio_offset(m,reg,&offset,&width) ||
        reg==CE_SM_TLBIASID || reg==CE_SM_TLBSYNC || m->lease(m->owner))return -1;
    ceph_smmu_mmio_order();
    *value=width==8?*(volatile uint64_t *)(m->base+offset):*(volatile uint32_t *)(m->base+offset);
    ceph_smmu_mmio_order();return 0;
}
static inline int ceph_smmu_mmio_write(void *ctx,enum ceph_smmu_reg reg,uint64_t value)
{
    struct ceph_smmu_mmio *m=ctx;uint32_t offset;unsigned width;
    if(ceph_smmu_mmio_offset(m,reg,&offset,&width))return -1;
    const struct ceph_smmu_plan *p=&m->plan;int allowed=0;
    switch(reg) {
    case CE_SM_SMR:allowed=value==p->smr || value==(p->smr&~(1u<<31));break;
    case CE_SM_S2CR:allowed=value==p->s2cr_fault || value==p->s2cr_translate;break;
    case CE_SM_SCTLR:allowed=value==p->sctlr || value==(p->sctlr&~1u);break;
    case CE_SM_CBAR:allowed=value==p->cbar;break;
    case CE_SM_CBA2R:allowed=value==p->cba2r;break;
    case CE_SM_TCR2:allowed=value==p->tcr2;break;
    case CE_SM_TCR:allowed=value==p->tcr;break;
    case CE_SM_TTBR0:allowed=value==p->ttbr0;break;
    case CE_SM_TTBR1:allowed=value==p->ttbr1;break;
    case CE_SM_MAIR0:allowed=value==p->mair0;break;
    case CE_SM_MAIR1:allowed=value==p->mair1;break;
    case CE_SM_ACTLR:allowed=value==m->actlr;break;
    case CE_SM_TLBIASID:allowed=value==p->asid;break;
    case CE_SM_TLBSYNC:allowed=!value;break;
    default:return -1; /* status/FSR clearing deliberately unavailable */
    }
    if(!allowed || m->lease(m->owner))return -1;
    ceph_smmu_mmio_order();
    if(width==8)*(volatile uint64_t *)(m->base+offset)=value;
    else *(volatile uint32_t *)(m->base+offset)=(uint32_t)value;
    ceph_smmu_mmio_order();return 0;
}
#endif
