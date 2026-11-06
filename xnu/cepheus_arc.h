/* Bounded command-db ARC decoder, no hardware writes or inferred vote ownership.
 * cmd-db.c: 144-byte header, 8 slave headers, 24-byte entries; ARC aux u16 LE.
 * Offset callback reads aligned words only; no packed/unaligned Device loads.
 */
#ifndef CEPHEUS_ARC_H
#define CEPHEUS_ARC_H
#include <stddef.h>
#include <stdint.h>

struct ceph_arc_db {
    void *ctx;
    uint32_t bytes;
    uint32_t (*read32)(void *, uint32_t);
};
struct ceph_arc_resource {
    uint32_t address, priority[2];
    uint16_t aux_version, levels[16];
    unsigned count;
};
enum { CE_ARC_OK=0, CE_ARC_INVALID=-1, CE_ARC_MISSING=-2, CE_ARC_RANGE=-3 };

static inline int ceph_arc_range(const struct ceph_arc_db *db, uint32_t off, uint32_t size)
{
    return off <= db->bytes && size <= db->bytes - off;
}
/* Call only after the enclosing byte range has been checked. */
static inline uint16_t ceph_arc_u16(const struct ceph_arc_db *db, uint32_t off)
{
    return (uint16_t)(db->read32(db->ctx, off & ~3u) >> (8u * (off & 3u)));
}
static inline int ceph_arc_lookup(const struct ceph_arc_db *db, const char *name,
    struct ceph_arc_resource *out)
{
    if (!out) return CE_ARC_INVALID;
    *out = (struct ceph_arc_resource){0};
    if (!db || !db->read32 || !name || !name[0] || db->bytes < 144 ||
        db->bytes > 0x100000 || (db->bytes & 3u)) return CE_ARC_INVALID;
    uint32_t key[2]={0};
    unsigned length=0;
    for (; length<8 && name[length]; ++length)
        key[length/4] |= (uint32_t)(uint8_t)name[length] << (8u*(length%4));
    if (length==8 && name[8]) return CE_ARC_INVALID;
    if (db->read32(db->ctx,0)!=1 || db->read32(db->ctx,4)!=0x0c0330dbu ||
        (db->read32(db->ctx,140)&1u)) return CE_ARC_INVALID;
    struct ceph_arc_resource found={0};
    unsigned matches=0;
    for (unsigned i=0; i<8; ++i) {
        uint32_t h=8+16*i;
        unsigned slave=ceph_arc_u16(db,h);
        if (!slave) break;
        unsigned count=ceph_arc_u16(db,h+6);
        uint32_t entries=144+ceph_arc_u16(db,h+2), aux=144+ceph_arc_u16(db,h+4);
        if (slave<3 || slave>5 || count>256 || (entries&3u) ||
            !ceph_arc_range(db,entries,count*24u) || !ceph_arc_range(db,aux,0))
            return CE_ARC_INVALID;
        for (unsigned j=0; j<count; ++j) {
            uint32_t e=entries+j*24;
            if (db->read32(db->ctx,e)!=key[0] || db->read32(db->ctx,e+4)!=key[1]) continue;
            if (++matches!=1 || slave!=3) return CE_ARC_INVALID;
            found.address=db->read32(db->ctx,e+16);
            found.priority[0]=db->read32(db->ctx,e+8);
            found.priority[1]=db->read32(db->ctx,e+12);
            found.aux_version=ceph_arc_u16(db,h+8);
            unsigned size=ceph_arc_u16(db,e+20);
            uint32_t offset=aux+ceph_arc_u16(db,e+22);
            if ((found.address>>16)!=3 || (found.address&3u) || !size ||
                size>32 || (size&1u) || (offset&1u) || !ceph_arc_range(db,offset,size))
                return CE_ARC_INVALID;
            int padding=0;
            for (unsigned k=0; k<size/2; ++k) {
                uint16_t level=ceph_arc_u16(db,offset+2*k);
                if (k && !level) padding=1;
                if (padding) { if (level) return CE_ARC_INVALID; continue; }
                if (found.count && level<=found.levels[found.count-1]) return CE_ARC_INVALID;
                found.levels[found.count++]=level;
            }
            if (!found.count) return CE_ARC_INVALID;
        }
    }
    if (!matches) return CE_ARC_MISSING;
    *out=found;
    return CE_ARC_OK;
}
/* Reverse lookup for fixed PDC ePCB addresses. Name/aux are still validated by
 * the normal parser, duplicates refused; no resource name inferred from address.
 */
static inline int ceph_arc_lookup_address(const struct ceph_arc_db *db,uint32_t address,char name[9],
    struct ceph_arc_resource *out)
{
    if(!name || !out)return CE_ARC_INVALID;
    for(unsigned i=0;i<9;++i)name[i]=0;
    *out=(struct ceph_arc_resource){0};
    if(!db || !db->read32 || db->bytes<144 || db->bytes>0x100000 || (db->bytes&3) ||
        (address>>16)!=3 || (address&3))return CE_ARC_INVALID;
    if(db->read32(db->ctx,0)!=1 || db->read32(db->ctx,4)!=0x0c0330db || (db->read32(db->ctx,140)&1))return CE_ARC_INVALID;
    unsigned matches=0;char found[9]={0};
    for(unsigned i=0;i<8;++i) {
        uint32_t h=8+16*i;unsigned slave=ceph_arc_u16(db,h);if(!slave)break;
        unsigned count=ceph_arc_u16(db,h+6);uint32_t entries=144+ceph_arc_u16(db,h+2);
        if(slave<3 || slave>5 || count>256 || (entries&3) || !ceph_arc_range(db,entries,count*24))return CE_ARC_INVALID;
        for(unsigned j=0;j<count;++j) {
            uint32_t e=entries+24*j;if(db->read32(db->ctx,e+16)!=address)continue;
            if(slave!=3 || ++matches!=1)return CE_ARC_INVALID;
            uint32_t words[2]={db->read32(db->ctx,e),db->read32(db->ctx,e+4)};
            int end=0;
            for(unsigned c=0;c<8;++c) {
                unsigned v=(words[c/4]>>(8*(c%4)))&255;
                if(!v)end=1;
                else if(end || v<0x21 || v>0x7e)return CE_ARC_INVALID;
                found[c]=(char)v;
            }
            if(!found[0])return CE_ARC_INVALID;
        }
    }
    if(!matches)return CE_ARC_MISSING;
    int e=ceph_arc_lookup(db,found,out);if(e)return e;
    if(out->address!=address)return CE_ARC_INVALID;
    for(unsigned i=0;i<9;++i)name[i]=found[i];
    return 0;
}
/* Raw abstract VLVL (MIN_SVS=48), not millivolts or Linux's +1 offset.
 * The ARC data word is the selected table INDEX. Upper bound is explicit.
 */
static inline int ceph_arc_select(const struct ceph_arc_resource *r,
    unsigned minimum, unsigned maximum, unsigned *index)
{
    if (!r || !index || !r->count || r->count>16 || minimum>maximum) return CE_ARC_INVALID;
    for (unsigned i=0; i<r->count; ++i) {
        if (i && r->levels[i]<=r->levels[i-1]) return CE_ARC_INVALID;
    }
    for (unsigned i=0; i<r->count; ++i) {
        if (r->levels[i]>=minimum) {
            if (r->levels[i]>maximum) break;
            *index=i; return CE_ARC_OK;
        }
    }
    return CE_ARC_RANGE;
}
/* MX must cover the chosen CX level; never silently select an insufficient MX. */
static inline int ceph_arc_pair(const struct ceph_arc_resource *cx,
    const struct ceph_arc_resource *mx, unsigned minimum, unsigned ceiling,
    unsigned *cx_index, unsigned *mx_index)
{
    if (!cx_index || !mx_index) return CE_ARC_INVALID;
    unsigned c,m;
    int err=ceph_arc_select(cx,minimum,ceiling,&c);
    if (err) return err;
    err=ceph_arc_select(mx,cx->levels[c],ceiling,&m);
    if (err) return err;
    *cx_index=c; *mx_index=m; return CE_ARC_OK;
}
#endif
