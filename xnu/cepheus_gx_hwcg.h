/* A640 hardware clock-gating block from a6xx_hwcg_set(adreno_dev, true).
 *
 * Order in the source:
 *   1. GMU AO CGC mode/delay/hyst (GMU aperture, 0x23b09..0x23b0b)
 *   2. read RBBM_CLOCK_CNTL; if it already equals the A640 "on" value, stop
 *   3. drop the SPTPRAC GX clock (GMU 0x1a880, rmw bit0 -> 0)
 *   4. program the 49-entry a640_hwcg_regs table
 *   5. restore the SPTPRAC GX clock (rmw bit0 -> 1)
 *   6. write top-level RBBM_CLOCK_CNTL = 0x8aa8aa82
 *
 * The early-out at step 2 is preserved: re-programming an already-gated block
 * is not equivalent (it would needlessly ungate SPTPRAC again).
 *
 * Registers span two apertures (GMU words vs GPU-relative words), CP must stay
 * halted, and the caller owns powered GX/SPTP/fence. No reset, no unhalt, no
 * clock/vote programming beyond the SPTPRAC gate bits the source itself toggles.
 */
#ifndef CEPHEUS_GX_HWCG_H
#define CEPHEUS_GX_HWCG_H
#include "cepheus_gx_core.h"
enum {CE_HWCG_REGS=49};
struct ceph_hwcg_write {uint32_t word,value;};
struct ceph_gx_hwcg_plan {
    struct ceph_hwcg_write regs[CE_HWCG_REGS];
    uint32_t gmu_cgc_mode,gmu_cgc_delay,gmu_cgc_hyst,sptprac_clock,top_clock_cntl;
};
/* __get_gmu_ao_cgc_*_cntl() and __get_rbbm_clock_cntl_on() for the default
 * (non-a612/a615) branch, which is what A640 takes. */
static inline int ceph_gx_hwcg_plan(const struct ceph_gx_core_input *i,struct ceph_gx_hwcg_plan *out)
{
    if(!i || !out || i->chipid!=0x06040001)return -1;
    static const struct ceph_hwcg_write table[CE_HWCG_REGS]={
        {0x000b0,0x02222222},{0x000b4,0x02222220},{0x000b8,0x00000080},{0x000bc,0x0000f3cf},
        {0x000c0,0x02222222},{0x000c4,0x22222222},{0x000c8,0x22222222},{0x000cc,0x00022222},
        {0x000d0,0x11111111},{0x000d4,0x11111111},{0x000d8,0x11111111},{0x000dc,0x00011111},
        {0x000e0,0x77777777},{0x000e4,0x77777777},{0x000e8,0x77777777},{0x000ec,0x00077777},
        {0x000f0,0x22222222},{0x000f4,0x01002222},{0x000f8,0x00002220},{0x00100,0x00040f00},
        {0x00104,0x05222022},{0x00105,0x00005555},{0x00106,0x00000011},{0x00107,0x00445044},
        {0x00108,0x04222222},{0x00111,0x00002222},{0x00114,0x00222222},{0x00117,0x00000002},
        {0x0011b,0x00002222},{0x00109,0x00004000},{0x00112,0x00002222},{0x00115,0x00000200},
        {0x0011c,0x00000000},{0x0010a,0x00000000},{0x00113,0x00000000},{0x00116,0x04104004},
        {0x0011d,0x00000000},{0x00120,0x00000222},{0x00121,0x00000111},{0x00122,0x00000777},
        {0x0010b,0x22222222},{0x00110,0x00000004},{0x0010f,0x00000002},{0x000533,0x00000182},
        {0x000044,0x00000000},{0x000042,0x00000000},{0x00118,0x00000222},{0x00119,0x00000111},
        {0x0011a,0x00000555}};
    for(unsigned n=0;n<CE_HWCG_REGS;++n)out->regs[n]=table[n];
    out->gmu_cgc_mode=0x00020202;out->gmu_cgc_delay=0x00010111;out->gmu_cgc_hyst=0x00005555;
    out->sptprac_clock=0x1a880;out->top_clock_cntl=0x8aa8aa82;
    return 0;
}
struct ceph_gx_hwcg {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    struct ceph_gx_core_input input;
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,skipped,error;
};
static inline int ceph_gx_hwcg_ready(struct ceph_gx_hwcg *s)
{return ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease);}
static inline int ceph_gx_hwcg_gmu_store(struct ceph_gx_hwcg *s,uint32_t word,uint32_t value)
{
    uint32_t actual;
    return ceph_gmu_mmio_store(s->gmu,word,value) ||
        ceph_gmu_mmio_load(s->gmu,word,&actual) || actual!=value ? -1 : 0;
}
static inline int ceph_gx_hwcg_sptprac(struct ceph_gx_hwcg *s,uint32_t bit)
{
    uint32_t actual;
    if(ceph_gmu_mmio_load(s->gmu,0x1a880,&actual))return -1;
    uint32_t v=bit?(actual|1u):(actual&~1u);
    return ceph_gx_hwcg_gmu_store(s,0x1a880,v);
}
static inline int ceph_gx_hwcg_configure(struct ceph_gx_hwcg *s)
{
    struct ceph_gx_hwcg_plan p;
    if(!s || s->terminal || s->writes || !s->finish || ceph_gx_hwcg_plan(&s->input,&p) ||
        /* The 49-entry table spans GPU words up to 0x533, so a short aperture is
         * refused before any write instead of faulting part-way through. */
        !s->gpu || s->bytes<0x40000 || ((uintptr_t)s->gpu&3u) || ceph_gx_hwcg_ready(s))return -1;
    s->terminal=1;int e=0;uint32_t top;
    if(ceph_gx_hwcg_gmu_store(s,0x23b09,p.gmu_cgc_mode) ||
        ceph_gx_hwcg_gmu_store(s,0x23b0a,p.gmu_cgc_delay) ||
        ceph_gx_hwcg_gmu_store(s,0x23b0b,p.gmu_cgc_hyst)) {e=-1;goto done;}
    s->writes=3;
    /* Source early-out: already-on HWCG needs no re-programming. */
    if(ceph_gx_hwcg_ready(s)){e=-1;goto done;}
    top=s->gpu[0xae];ceph_gmu_mmio_barrier();
    if(top==p.top_clock_cntl) {s->skipped=1;s->configured=1;goto done;}
    if(ceph_gx_hwcg_sptprac(s,0)){e=-1;goto done;}
    ++s->writes;
    for(unsigned n=0;n<CE_HWCG_REGS;++n) {
        if(ceph_gx_hwcg_ready(s)){e=-1;goto done;}
        ++s->writes;ceph_gmu_mmio_barrier();s->gpu[p.regs[n].word]=p.regs[n].value;ceph_gmu_mmio_barrier();
        if(ceph_gx_hwcg_ready(s) || s->gpu[p.regs[n].word]!=p.regs[n].value){e=-1;goto done;}
    }
    if(ceph_gx_hwcg_sptprac(s,1)){e=-1;goto done;}
    ++s->writes;
    if(ceph_gx_hwcg_ready(s)){e=-1;goto done;}
    ++s->writes;ceph_gmu_mmio_barrier();s->gpu[0xae]=p.top_clock_cntl;ceph_gmu_mmio_barrier();
    if(ceph_gx_hwcg_ready(s) || s->gpu[0xae]!=p.top_clock_cntl){e=-1;goto done;}
    s->configured=1;
done:s->error=e;s->finish(s->owner,1);return e;
}
#endif
