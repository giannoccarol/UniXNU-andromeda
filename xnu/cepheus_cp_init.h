/* A640 CP_ME_INIT and owned powerup register snapshot. Read callbacks take
 * GPU-relative DWORD offsets; no writes/protection toggles occur here.
 * The backend must own powered GX/fence, configure protection and 64-bit modes
 * BEFORE taking this snapshot. This is NOT a zero-filled substitute reglist.
 */
#ifndef CEPHEUS_CP_INIT_H
#define CEPHEUS_CP_INIT_H
#include "cepheus_a640_pm4.h"
#include "cepheus_cp_protect.h"
enum {CE_CP_IFPC_REGS=38,CE_CP_PREEMPT_REGS=28,CE_CP_REGLIST_WORDS=136};
static const uint32_t ceph_cp_restore_offsets[66]={
    0x10,0x11,0x841,0x843,0x84f,0x850,0x851,0x852,0x853,0x854,0x855,0x856,
    0x857,0x858,0x859,0x85a,0x85b,0x85c,0x85d,0x85e,0x85f,0x860,0x861,0x862,
    0x863,0x864,0x865,0x866,0x867,0x868,0x869,0x86a,0x86b,0x86c,0x86d,0x86e,0x86f,0x98d,
    0xc01,0x8601,0x8e05,0x9e01,0xbe05,0xa601,0x9601,0xe00,0xae01,0xb601,
    0xe05,0xe06,0xe09,0xe0a,0xe07,0xe08,0xe0b,0xe0c,0xe0d,0xe0e,0xe18,0xe17,0xe01,
    0x8e08,0xb604,0xae02,0x9e00,0x8e50
};
struct ceph_cp_snapshot_ops {
    void *ctx;
    int (*begin)(void *); /* lock/ownership/GX fence+power stable */
    int (*read)(void *,uint32_t word_offset,uint32_t *value);
    int (*validate)(void *); /* all configured fields stable through snapshot */
    void (*end)(void *);
};
static inline int ceph_cp_snapshot(struct ceph_cp_snapshot_ops *b,uint32_t *out,unsigned words)
{
    if(!b || !b->begin || !b->read || !b->validate || !b->end || !out || words<CE_CP_REGLIST_WORDS)return -1;
    if(b->begin(b->ctx))return -1;
    uint32_t data[CE_CP_REGLIST_WORDS]={0};int e=-1;
    /* cpu_gpu_lock: 3 zero flags/turn, then little-endian u16 len+offset. */
    data[3]=132u|(76u<<16);
    for(unsigned i=0;i<66;++i) {
        uint32_t value;
        if(b->validate(b->ctx) || b->read(b->ctx,ceph_cp_restore_offsets[i],&value))goto done;
        /* Some protection register values legitimately contain all ones.
         * Transport validity must be signaled by callback, not guessed by data. */
        if(i==4 && (value&11u)!=11u)goto done;
        if(i>=5 && i<37) {
            uint32_t expected;
            if(ceph_cp_protect_word(i-5,&expected) || value!=expected)goto done;
        }
        if(i>=38 && i<48 && !(value&1u))goto done; /* all address modes64 */
        data[4+2*i]=ceph_cp_restore_offsets[i];data[5+2*i]=value;
    }
    if(b->validate(b->ctx))goto done;
    for(unsigned i=0;i<CE_CP_REGLIST_WORDS;++i)out[i]=data[i];
    e=0;
done:b->end(b->ctx);return e;
}
static inline int ceph_cp_me_init(uint32_t *out,unsigned capacity,uint64_t reglist_va,size_t mapped_bytes)
{
    if(!out || capacity<12 || !reglist_va || (reglist_va&4095u) || mapped_bytes<4096 ||
        mapped_bytes>16384 || (mapped_bytes&4095u) || reglist_va>=(1ULL<<40) ||
        mapped_bytes>(1ULL<<40)-reglist_va)return -1;
    /* CP_INIT_MASK=bits0,1,2,3,5,6,8. default-reset bit3 has no payload.
     * 11 payload DWORDs, including final zero pad. No unsecure/protection packets. */
    const uint32_t packet[12]={ceph_pm4_type7(0x48,11),0x16f,3,0x20000000,0,0,0,2,
        (uint32_t)reglist_va,(uint32_t)(reglist_va>>32),0,0};
    for(unsigned i=0;i<12;++i)out[i]=packet[i];
    return 12;
}
#endif
