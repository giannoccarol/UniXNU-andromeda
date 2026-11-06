/* A640v2 UCHE/GMEM/ROQ/UBWC subset of a6xx_start, NOT complete GX initialization.
 * Does not configure HWCG/GBIF/LLCC/QoS/secure mode, release CP or touch clocks.
 * Platform must hold powered GX/SPTP/fence/OOB and reserved GMEM address range.
 * Explicit1MiB GMEM comes from supplied device core identity, not PA allocation.
 */
#ifndef CEPHEUS_GX_CORE_H
#define CEPHEUS_GX_CORE_H
#include "cepheus_cp_protection.h"
struct ceph_gx_core_input {uint32_t chipid,gmem_bytes,gmem_base,highest_bank_bit,min_access_length,ubwc_mode;};
struct ceph_gx_write {uint32_t word,value;};
struct ceph_gx_core_plan {struct ceph_gx_write writes[23];};
static inline int ceph_gx_core_plan(const struct ceph_gx_core_input *i,struct ceph_gx_core_plan *out)
{
    if(!i || !out || i->chipid!=0x06040001 || i->gmem_bytes!=0x100000 || i->gmem_base!=0x100000 ||
        i->highest_bank_bit!=15 || i->min_access_length!=32 || i->ubwc_mode!=3)return -1;
    /* FDT UBWC3 => amsbc=1/mode=0, HBB15=>2, MAL32=>0. */
    *out=(struct ceph_gx_core_plan){{
        {0xe05,0xffffffc0},{0xe06,0x0001ffff},{0xe09,0xfffff000},{0xe0a,0x0001ffff},
        {0xe07,0xfffff000},{0xe08,0x0001ffff},{0xe0b,0x100000},{0xe0c,0},
        {0xe0d,0x1fffff},{0xe0e,0},{0xe18,0x804},{0xe17,4},
        {0x8c2,0x02000140},{0x8c1,0x8040362c},{0x8c3,128},
        {0x9e00,0x200000},{0x98d,1},{0x8e08,0x14},{0xb604,4},{0xae02,4},
        {0xe01,0x00400000},{0x1f,0x40cfffff},{0xe19,1}}};return 0;
}
struct ceph_gx_core {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    struct ceph_gx_core_input input;
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_gx_core_configure(struct ceph_gx_core *s)
{
    struct ceph_gx_core_plan p;
    if(!s || s->terminal || s->writes || !s->finish || ceph_gx_core_plan(&s->input,&p) ||
        ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease))return -1;
    s->terminal=1;int e=0;
    for(unsigned n=0;n<23;++n) {
        if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease)){e=-1;goto done;}
        ++s->writes;ceph_gmu_mmio_barrier();s->gpu[p.writes[n].word]=p.writes[n].value;ceph_gmu_mmio_barrier();
        if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease) ||
            s->gpu[p.writes[n].word]!=p.writes[n].value){e=-1;goto done;}
    }
    s->configured=1;
done:s->error=e;s->finish(s->owner,1);return e;
}
#endif
