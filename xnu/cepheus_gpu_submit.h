/* One-shot submission contract. No register backend or raw user command API.
 * begin() must own a running non-secure SQE/ring, mapped command+scratch+fence
 * memory, powered domains and idle contexts, and reject a stale fence value.
 * Fence visibility is not permission to free buffers: quiesce must also pass.
 */
#ifndef CEPHEUS_GPU_SUBMIT_H
#define CEPHEUS_GPU_SUBMIT_H
#include <stdint.h>
enum ceph_submit_state { CE_SUB_NEW,CE_SUB_PREPARED,CE_SUB_PUBLISHED,CE_SUB_SENT,
    CE_SUB_COMPLETE,CE_SUB_REFUSED,CE_SUB_QUARANTINED };
struct ceph_submit { enum ceph_submit_state state;int error;uint32_t sequence; };
struct ceph_submit_ops {
    void *ctx;
    int (*begin)(void *,uint32_t); /* failure releases partial CPU-only claims */
    int (*publish)(void *); /* clean commands/sentinel, barrier before WPTR */
    int (*doorbell)(void *); /* failure may still have reached GPU */
    int (*observe)(void *,uint32_t *); /* invalidate/acquire fence, check GPU faults */
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *); /* bounded yield, not busy infinite spin */
    int (*quiesce)(void *); /* bounded idle proof while memory is still pinned */
    void (*finish)(void *,int); /* keep=1 quarantines all GPU-owned allocations */
};
static inline int ceph_submit_once(struct ceph_submit *s,struct ceph_submit_ops *b,uint32_t sequence)
{
    if(!s || s->state!=CE_SUB_NEW || !sequence || !b || !b->begin || !b->publish ||
        !b->doorbell || !b->observe || !b->now_ns || !b->pause || !b->quiesce || !b->finish) return -1;
    int e=b->begin(b->ctx,sequence);
    if(e) {s->error=e;s->state=CE_SUB_REFUSED;return e;}
    s->state=CE_SUB_PREPARED;s->sequence=sequence;
    /* Once cache publication starts, stale WPTR/peer fetch makes rollback
     * unsafe unless a real platform stop proves otherwise. Keep on any error. */
    s->state=CE_SUB_PUBLISHED;e=b->publish(b->ctx);if(e) goto failed;
    uint64_t start,last,now;
    e=b->now_ns(b->ctx,&start);if(e) goto failed;last=start;
    s->state=CE_SUB_SENT;e=b->doorbell(b->ctx);if(e) goto failed;
    for(unsigned n=0;n<10000;++n) {
        e=b->now_ns(b->ctx,&now);if(e) goto failed;
        if(now<last || now-start>=100000000ULL) {e=-2;goto failed;}last=now;
        uint32_t fence=0;e=b->observe(b->ctx,&fence);if(e) goto failed;
        if(fence==sequence) {
            e=b->quiesce(b->ctx);if(e) goto failed;
            s->state=CE_SUB_COMPLETE;s->error=0;b->finish(b->ctx,0);return 0;
        }
        e=b->pause(b->ctx);if(e) goto failed;
    }
    e=-2;
failed:
    s->state=CE_SUB_QUARANTINED;s->error=e;b->finish(b->ctx,1);return e;
}
#endif
