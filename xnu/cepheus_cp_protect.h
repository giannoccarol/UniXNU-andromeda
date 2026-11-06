/* A640 register protection spans from KGSL a630_protected_regs, which is also
 * selected by a6xx_protect_init for A640. Ordered spans can overlap on purpose;
 * the last span protects from its start to the end of the register space.
 * This only encodes/validates protection, never disables it or enters secure mode.
 */
#ifndef CEPHEUS_CP_PROTECT_H
#define CEPHEUS_CP_PROTECT_H
#include <stdint.h>
struct ceph_cp_span {uint32_t first,last;unsigned noaccess;};
static const struct ceph_cp_span ceph_cp_spans[32]={
    {0x00000,0x004ff,0},{0x00501,0x00506,0},{0x0050b,0x007ff,0},{0x0050e,0x0050e,1},
    {0x00510,0x00510,1},{0x00534,0x00534,1},{0x00800,0x00882,1},{0x008a0,0x008a8,1},
    {0x008ab,0x008cf,1},{0x008d0,0x0098c,0},{0x00900,0x0094d,1},{0x0098d,0x00bff,1},
    {0x00e00,0x00e01,1},{0x00e03,0x00e0f,1},{0x03c00,0x03cc3,1},{0x03cc4,0x05cc3,0},
    {0x08630,0x087ff,1},{0x08e00,0x08e00,1},{0x08e08,0x08e08,1},{0x08e50,0x08e6f,1},
    {0x09624,0x097ff,1},{0x09e70,0x09e71,1},{0x09e78,0x09fff,1},{0x0a630,0x0a7ff,1},
    {0x0ae02,0x0ae02,1},{0x0ae50,0x0b17f,1},{0x0b604,0x0b604,1},{0x0be02,0x0be03,1},
    {0x0be20,0x0d5ff,1},{0x0f000,0x0fbff,1},{0x0fc00,0x11bff,0},{0x11c00,0x11c00,1}
};
static inline int ceph_cp_protect_word(unsigned slot,uint32_t *out)
{
    if(!out || slot>=32)return -1;
    const struct ceph_cp_span *s=&ceph_cp_spans[slot];
    if(s->last<s->first || s->first>0x3ffff || s->last-s->first>0x1fff || s->noaccess>1)return -1;
    *out=s->first|((s->last-s->first)<<18)|(s->noaccess<<31);return 0;
}
#endif
