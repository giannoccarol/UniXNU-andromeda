/* Exact A640 register protection + address-mode setup while CP remains halted.
 * No trust-control writes, CP execution or security firmware bypass. Requires
 * already initialized/powered GX, fence/OOB and exclusive platform ownership.
 */
#ifndef CEPHEUS_CP_PROTECTION_H
#define CEPHEUS_CP_PROTECTION_H
#include "cepheus_cp_protect.h"
#include "cepheus_gmu_mmio.h"
static const uint32_t ceph_cp_address_modes[]={0x842,0xc01,0x8601,0x8e05,0x9e01,0xbe05,
    0xa601,0x9601,0xe00,0xae01,0xb601,0xf810};
static inline int ceph_cp_halted_window(struct ceph_gmu_mmio *gmu,volatile uint32_t *gpu,
    size_t bytes,void *owner,int (*lease)(void *))
{
    if(!gpu || ((uintptr_t)gpu&3u) || bytes!=0x40000 || !lease || lease(owner) ||
        ceph_gmu_mmio_ready(gmu) || ceph_gmu_mmio_fault(gmu))return -1;
    if(ceph_gmu_mmio_gx_access(gmu))return -1;
    uint32_t halt=gpu[0x808],fault=gpu[0x823];ceph_gmu_mmio_barrier();
    return halt==UINT32_MAX || !(halt&2) || fault?-1:0;
}
struct ceph_cp_protection {
    struct ceph_gmu_mmio *gmu;volatile uint32_t *gpu;size_t bytes;
    void *owner;int (*lease)(void *);void (*finish)(void *,int keep);
    unsigned writes;int terminal,configured,error;
};
static inline int ceph_cp_protection_word(unsigned step,uint32_t *reg,uint32_t *value)
{
    if(!reg || !value || step>=45)return -1;
    if(step<12){*reg=ceph_cp_address_modes[step];*value=1;}
    else if(step==12){*reg=0x84f;*value=11;}
    else {*reg=0x850+step-13;if(ceph_cp_protect_word(step-13,value))return -1;}
    return 0;
}
static inline int ceph_cp_protection_check(struct ceph_gmu_mmio *gmu,volatile uint32_t *gpu,
    size_t bytes,void *owner,int (*lease)(void *))
{
    for(unsigned step=0;step<45;++step) {
        uint32_t reg,value;
        if(ceph_cp_protection_word(step,&reg,&value) || ceph_cp_halted_window(gmu,gpu,bytes,owner,lease))return -1;
        uint32_t actual=gpu[reg];ceph_gmu_mmio_barrier();if(actual!=value)return -1;
    }
    return 0;
}
static inline int ceph_cp_protection_install(struct ceph_cp_protection *s)
{
    if(!s || s->terminal || s->writes || !s->finish ||
        ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease))return -1;
    s->terminal=1;int e=0;
    for(unsigned step=0;step<45;++step) {
        uint32_t reg,value;
        if(ceph_cp_protection_word(step,&reg,&value) || ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease)) {e=-1;goto done;}
        ++s->writes;ceph_gmu_mmio_barrier();s->gpu[reg]=value;ceph_gmu_mmio_barrier();
        if(ceph_cp_halted_window(s->gmu,s->gpu,s->bytes,s->owner,s->lease) || s->gpu[reg]!=value){e=-1;goto done;}
    }
    e=ceph_cp_protection_check(s->gmu,s->gpu,s->bytes,s->owner,s->lease);
    if(!e)s->configured=1;
done:
    s->error=e;s->finish(s->owner,1);return e;
}
#endif
