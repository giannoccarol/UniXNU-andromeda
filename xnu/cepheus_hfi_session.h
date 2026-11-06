/* Single-owner HFI v2 exchange using real shared rings. No exchange-success
 * callback; tests exercise write/read/index/ACK paths end to end. Platform
 * callbacks handle time, acquire/clean/barriers/doorbell and GPU fault status.
 * Caller serializes; one outstanding command; sequences never wrap in session.
 */
#ifndef CEPHEUS_HFI_SESSION_H
#define CEPHEUS_HFI_SESSION_H
#include "cepheus_hfi.h"
enum { CE_HFI_TIMEOUT=-5, CE_HFI_FW_ERROR=-6, CE_HFI_PROTOCOL=-7 };
struct ceph_hfi_session {
    struct ceph_hfi_queue *tx,*rx;
    void *ctx;
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *);
    int (*fault)(void *); /* poll actual CM3/SMMU errors, 0 means no error */
    unsigned sequence;
    int poisoned,busy;
    uint32_t active_header;
};
static inline int ceph_hfi_session_poison(struct ceph_hfi_session *s,int error)
{
    s->poisoned=1;s->busy=0;s->tx->poisoned=s->rx->poisoned=1;return error;
}
static inline int ceph_hfi_exchange(struct ceph_hfi_session *s,const uint32_t *cmd,unsigned words,
    uint32_t *reply,unsigned capacity,unsigned *reply_words)
{
    if(!reply_words)return CE_HFI_INVALID;
    *reply_words=0;
    if(!s || !s->tx || !s->rx || s->tx==s->rx || !s->now_ns || !s->pause || !s->fault ||
        !cmd || !words || words>255 || !reply || capacity<3 ||
        ((cmd[0]>>8)&255u)!=words || (cmd[0]&0xffff0000u) ||
        s->tx->major!=2 || s->rx->major!=2 || s->busy) return CE_HFI_INVALID;
    if(s->poisoned || s->tx->poisoned || s->rx->poisoned)return CE_HFI_POISONED;
    if(s->sequence>=4095)return CE_HFI_INVALID;
    uint32_t wire[255],response[255];
    for(unsigned i=0;i<words;++i)wire[i]=cmd[i];
    wire[0]|=(++s->sequence)<<20;s->busy=1;s->active_header=wire[0];
    uint64_t start,last,now;
    if(s->now_ns(s->ctx,&start))return ceph_hfi_session_poison(s,CE_HFI_IO);
    last=start;int sent=0;
    for(unsigned loops=0;loops<10000;++loops) {
        if(s->fault(s->ctx) || s->now_ns(s->ctx,&now))return ceph_hfi_session_poison(s,CE_HFI_IO);
        if(now<last || now-start>=100000000ULL)return ceph_hfi_session_poison(s,CE_HFI_TIMEOUT);
        last=now;
        if(!sent) {
            int e=ceph_hfi_write(s->tx,wire,words);
            if(e==CE_HFI_FULL) { if(s->pause(s->ctx))return ceph_hfi_session_poison(s,CE_HFI_IO);continue; }
            if(e)return ceph_hfi_session_poison(s,e);
            sent=1;
        }
        unsigned count=0;
        int e=ceph_hfi_read(s->rx,response,255,&count);
        if(e==CE_HFI_EMPTY) { if(s->pause(s->ctx))return ceph_hfi_session_poison(s,CE_HFI_IO);continue; }
        if(e)return ceph_hfi_session_poison(s,e);
        /* Dedicated response queue: unmatched ACK/asynchronous allocation/error
         * must not be swallowed and mistaken for success. Debug has its own queue. */
        if((response[0]&255u)==100)return ceph_hfi_session_poison(s,CE_HFI_FW_ERROR);
        /* kgsl_hfi.c binds replies through req_hdr, not the outer ACK ID.
         * Do not invent an undocumented requirement that those IDs match. */
        if(count<3 || ((response[0]>>16)&15u)!=1 || response[1]!=wire[0])
            return ceph_hfi_session_poison(s,CE_HFI_PROTOCOL);
        if(response[2])return ceph_hfi_session_poison(s,CE_HFI_FW_ERROR);
        if(count>capacity)return ceph_hfi_session_poison(s,CE_HFI_SMALL);
        /* An ACK and an asynchronous error can arrive together. Recheck the
         * hardware/debug fault path after receipt, before reporting success. */
        if(s->fault(s->ctx))return ceph_hfi_session_poison(s,CE_HFI_IO);
        for(unsigned i=0;i<count;++i)reply[i]=response[i];
        *reply_words=count;s->busy=0;return 0;
    }
    return ceph_hfi_session_poison(s,CE_HFI_TIMEOUT);
}
#endif
