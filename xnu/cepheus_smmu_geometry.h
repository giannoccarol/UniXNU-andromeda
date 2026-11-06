/* Decode actual SMMUv2 ID registers before deriving CB offsets. Supports only
 * the legacy15-bit SMR format used by our installer; EXIDS explicitly refused.
 * Region length from live FDT must agree, not guessed from an example SoC.
 * This plans offsets/capabilities, not ownership or permission to access MMIO.
 */
#ifndef CEPHEUS_SMMU_GEOMETRY_H
#define CEPHEUS_SMMU_GEOMETRY_H
#include <stdint.h>
struct ceph_smmu_geometry {unsigned page_shift,groups,contexts,s2_contexts,pa_bits,sid_bits;uint32_t gr1,cb_base,bytes;unsigned va_bits,ipa_bits;};
static inline unsigned ceph_smmu_address_bits(unsigned encoding)
{
    static const unsigned bits[]={32,36,40,42,44,48};
    return encoding<6?bits[encoding]:0;
}
static inline int ceph_smmu_geometry(uint32_t id0,uint32_t id1,uint32_t id2,uint32_t bytes,struct ceph_smmu_geometry *out)
{
    if(!out || id0==UINT32_MAX || id1==UINT32_MAX || id2==UINT32_MAX ||
        !(id0&(1u<<30)) || !(id0&(1u<<27)) || (id0&(1u<<8)) || !(id2&(1u<<12)))return -1;
    struct ceph_smmu_geometry g={0};
    g.page_shift=(id1&(1u<<31))?16:12;
    g.groups=id0&255;g.contexts=id1&255;g.s2_contexts=(id1>>16)&255;
    g.sid_bits=(id0>>9)&15;g.pa_bits=ceph_smmu_address_bits((id2>>4)&15);
    g.va_bits=ceph_smmu_address_bits((id2>>8)&15);g.ipa_bits=ceph_smmu_address_bits(id2&15);
    if(!g.groups || g.groups>128 || !g.contexts || g.contexts>128 || g.s2_contexts>=g.contexts ||
        g.sid_bits<11 || g.sid_bits>15 || g.pa_bits<40 || g.ipa_bits<40 || g.va_bits<32)return -1;
    unsigned half_shift=g.page_shift+((id1>>28)&7)+1;
    if(half_shift>=31)return -1;
    g.cb_base=1u<<half_shift;g.gr1=1u<<g.page_shift;g.bytes=bytes;
    if(bytes!=g.cb_base*2 || ((uint64_t)g.contexts<<g.page_shift)>g.cb_base ||
        g.gr1+0x800+4*g.contexts>g.cb_base || 0xc00+4*g.groups>g.gr1)return -1;
    *out=g;return 0;
}
static inline int ceph_smmu_cb_offset(const struct ceph_smmu_geometry *g,unsigned cb,uint32_t reg,uint32_t *offset)
{
    if(!g || !offset || cb<g->s2_contexts || cb>=g->contexts || (reg&3u) ||
        (g->page_shift!=12 && g->page_shift!=16) || reg>=(1u<<g->page_shift))return -1;
    uint64_t off=g->cb_base+((uint64_t)cb<<g->page_shift)+reg;
    if(off+4>g->bytes)return -1;
    *offset=(uint32_t)off;return 0;
}
#endif
