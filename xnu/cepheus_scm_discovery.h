/* Restricted ARMv8 SCM discovery, not PAS authentication or a general SMC API.
 * Source: downstream scm.c:is_scm_armv8/scm_is_call_available, PAS ID13 from
 * Cepheus FDT. No pointers, shared memory, arbitrary FNIDs or PAS mutations.
 * A callback may issue one real SMC; host tests supply a deterministic double.
 * A secure call that never returns is NOT bounded by this local loop budget.
 */
#ifndef CEPHEUS_SCM_DISCOVERY_H
#define CEPHEUS_SCM_DISCOVERY_H
#include <stdint.h>
enum ceph_scm_convention {CE_SCM_UNKNOWN,CE_SCM_ARMV8_32,CE_SCM_ARMV8_64};
enum ceph_scm_query {CE_SCM_SELF,CE_SCM_PAS_INIT_AVAILABLE,CE_SCM_PAS_MEM_AVAILABLE,
    CE_SCM_PAS_AUTH_AVAILABLE,CE_SCM_PAS_STOP_AVAILABLE,CE_SCM_PAS_SUPPORTED};
enum {CE_SCM_INTERRUPTED=1,CE_SCM_BUSY=-12,CE_SCM_QUERY_LIMIT=16};
struct ceph_scm_regs {uint64_t x[8];};
struct ceph_scm_discovery_ops {
    void *ctx;
    int (*lease)(void *); /* serialization, allowed context, trusted SMC conduit */
    int (*invoke)(void *,struct ceph_scm_regs *); /* exactly one SMC; preserve returned x6 */
};
struct ceph_scm_discovery {
    enum ceph_scm_convention convention;
    unsigned available_mask,supported,calls,terminal;
    int error;uint64_t transport,secure;
};
static inline int ceph_scm_query_regs(enum ceph_scm_convention c,enum ceph_scm_query q,
    struct ceph_scm_regs *out)
{
    if(!out || (c!=CE_SCM_ARMV8_32 && c!=CE_SCM_ARMV8_64) || q<CE_SCM_SELF || q>CE_SCM_PAS_SUPPORTED)return -1;
    uint32_t info=0x02000601u,query=info;
    switch(q) {
    case CE_SCM_SELF: query|=0x80000000u;break;
    case CE_SCM_PAS_INIT_AVAILABLE:query=0x02000201u;break;
    case CE_SCM_PAS_MEM_AVAILABLE:query=0x02000202u;break;
    case CE_SCM_PAS_AUTH_AVAILABLE:query=0x02000205u;break;
    case CE_SCM_PAS_STOP_AVAILABLE:query=0x02000206u;break;
    case CE_SCM_PAS_SUPPORTED:info=0x02000207u;query=13;break;
    }
    *out=(struct ceph_scm_regs){{info|(q==CE_SCM_SELF?0x80000000u:0u)|
        (c==CE_SCM_ARMV8_64?0x40000000u:0u),1,query,0,0,0,0,0}};
    return 0;
}
static inline int ceph_scm_query_run(struct ceph_scm_discovery *s,struct ceph_scm_discovery_ops *b,
    enum ceph_scm_convention c,enum ceph_scm_query q,uint64_t *value)
{
    struct ceph_scm_regs r;
    if(ceph_scm_query_regs(c,q,&r))return -1;
    for(;;) {
        if(s->calls>=CE_SCM_QUERY_LIMIT || b->lease(b->ctx))return -1;
        ++s->calls;
        if(b->invoke(b->ctx,&r))return -1; /* ambiguous transport: stop, do not switch ABI */
        s->transport=r.x[0];s->secure=r.x[1];
        int32_t status=(int32_t)r.x[0];
        if(status==CE_SCM_INTERRUPTED)continue; /* preserve returned x0..x6 for resume */
        if(status==0){*value=r.x[1];return 0;}
        /* Only convention discovery may fall back on a clean not-supported.
         * BUSY, positive unknown, and other errors are not convention evidence. */
        return status==-1 && q==CE_SCM_SELF?1:-1;
    }
}
static inline int ceph_scm_discover_pas13(struct ceph_scm_discovery *s,struct ceph_scm_discovery_ops *b)
{
    if(!s || s->terminal || s->calls || s->available_mask || s->supported ||
        s->convention!=CE_SCM_UNKNOWN || !b || !b->lease || !b->invoke)return -1;
    s->terminal=1;uint64_t v=0;
    int e=ceph_scm_query_run(s,b,CE_SCM_ARMV8_64,CE_SCM_SELF,&v);
    if(e<0 || (!e && v>1))goto failed;
    s->convention=CE_SCM_ARMV8_64;
    if(e || !v) {
        s->convention=CE_SCM_UNKNOWN;
        e=ceph_scm_query_run(s,b,CE_SCM_ARMV8_32,CE_SCM_SELF,&v);
        if(e || v!=1)goto failed;
        s->convention=CE_SCM_ARMV8_32;
    }
    for(unsigned q=CE_SCM_PAS_INIT_AVAILABLE;q<=CE_SCM_PAS_STOP_AVAILABLE;++q) {
        if(ceph_scm_query_run(s,b,s->convention,(enum ceph_scm_query)q,&v) || v!=1)goto failed;
        s->available_mask|=1u<<(q-CE_SCM_PAS_INIT_AVAILABLE);
    }
    if(ceph_scm_query_run(s,b,s->convention,CE_SCM_PAS_SUPPORTED,&v) || v!=1)goto failed;
    s->supported=1;s->error=0;return 0;
failed:s->error=-1;return -1;
}
#if defined(KERNEL)
/* XNU one-shot thread-context info probe; NOT wired into boot/startup and not
 * a PAS loader. Calls only the fixed discovery queries above, never PAS1/2/5/6
 * directly. Actual secure-world return time is unknown until hardware tested. */
int cepheus_gpu_scm_probe_once(struct ceph_scm_discovery *);
#endif
#endif
