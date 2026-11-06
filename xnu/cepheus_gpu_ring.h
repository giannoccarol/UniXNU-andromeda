/* Kernel-generated scratch-only A6xx ring submission. Caller owns live SQE,
 * ring+scratch mappings and serialization. Never accepts raw user PM4. Ring
 * capacity is exactly the 32KiB KGSL A6xx configuration (8192 DWORDs).
 * Queuing is NOT execution/completion: follow with independently verified fence
 * and quiescence before releasing memory. Any publication failure poisons ring.
 */
#ifndef CEPHEUS_GPU_RING_H
#define CEPHEUS_GPU_RING_H
#include "cepheus_a640_pm4.h"
struct ceph_ring_span {unsigned start,next,pad_start,pad_words;};
static inline int ceph_ring_reserve(unsigned r,unsigned w,unsigned words,struct ceph_ring_span *span)
{
    if(!span || r>=8192 || w>8190 || !words || words>8190)return -1;
    struct ceph_ring_span p={0};
    if(r<=w) {
        if(words<=8190-w) {p.start=w;p.next=w+words;}
        else if(words<r) {p.start=0;p.next=words;p.pad_start=w;p.pad_words=8192-w;}
        else return 1; /* FULL; nothing touched */
    } else {
        if(words>=r-w)return 1;
        p.start=w;p.next=w+words;
    }
    *span=p;return 0;
}
struct ceph_gpu_ring {
    volatile uint32_t *cpu;uint64_t gpu_va;unsigned wptr;
    void *ctx;
    int (*begin)(void *); /* validate lease, exact ring/SQE/NS state */
    int (*rptr)(void *,unsigned *); /* acquire coherent shadow or live register */
    int (*publish)(void *,unsigned start,unsigned words); /* clean exact CPU ring range */
    int (*doorbell)(void *,unsigned next); /* release barrier then WPTR */
    void (*end)(void *,int uncertain);
    int poisoned,inflight;uint32_t sequence;
};
static inline int ceph_gpu_ring_scratch(struct ceph_gpu_ring *r,uint64_t scratch,uint64_t bytes,
    uint64_t address,uint32_t value,uint64_t fence,uint32_t sequence)
{
    if(!r || !r->cpu || ((uintptr_t)r->cpu&3u) || (r->gpu_va&4095u) || !r->gpu_va ||
        r->gpu_va>=(1ULL<<40) || 32768>(1ULL<<40)-r->gpu_va || !r->begin || !r->rptr ||
        !r->publish || !r->doorbell || !r->end || r->poisoned || r->inflight ||
        scratch>UINT64_MAX-bytes || !bytes || scratch>=(1ULL<<40) || bytes>(1ULL<<40)-scratch ||
        (scratch<r->gpu_va+32768 && r->gpu_va<scratch+bytes))return -1;
    uint32_t packet[10];
    if(ceph_a640_fenced_write_test(packet,10,scratch,bytes,address,value,fence,sequence)!=10)return -1;
    int e=r->begin(r->ctx);if(e)return e;
    unsigned read;struct ceph_ring_span span;
    e=r->rptr(r->ctx,&read);
    if(!e)e=ceph_ring_reserve(read,r->wptr,10,&span);
    if(e) {r->end(r->ctx,0);return e;}
    /* Writing CPU memory is reversible before publication, but no paths below
     * attempt rollback once peer visibility could occur. */
    if(span.pad_words) {
        r->cpu[span.pad_start]=ceph_pm4_type7(0x10,span.pad_words-1);
        for(unsigned i=1;i<span.pad_words;++i)r->cpu[span.pad_start+i]=0;
    }
    for(unsigned i=0;i<10;++i)r->cpu[span.start+i]=packet[i];
    r->inflight=1;r->sequence=sequence;
    if(span.pad_words) {e=r->publish(r->ctx,span.pad_start,span.pad_words);if(e)goto failed;}
    e=r->publish(r->ctx,span.start,10);if(e)goto failed;
    r->wptr=span.next;
    e=r->doorbell(r->ctx,span.next);if(e)goto failed;
    r->end(r->ctx,0);return 0;
failed:
    r->poisoned=1;r->end(r->ctx,1);return e;
}
#endif
