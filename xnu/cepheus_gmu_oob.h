/* A640 GPU keepalive OOB0 only. No generic OOB ids6/7 (reserved legacy ABI).
 * A validated running GMU/HFI and thermal/resource lease are mandatory. The
 * request remains asserted while HELD; clearing the host ACK does not drop it.
 * Only release after caller has closed admission and drained CP transactions.
 * Release request is not proof of GX-off; slumber/park must establish that later.
 */
#ifndef CEPHEUS_GMU_OOB_H
#define CEPHEUS_GMU_OOB_H
#include <stdint.h>
enum ceph_oob_reg {CE_OOB_H2G_SET,CE_OOB_G2H_INFO,CE_OOB_G2H_CLEAR,CE_OOB_GX_STATUS};
struct ceph_oob_bits {uint32_t request,ack,release;};
static inline int ceph_gmu_oob_gpu_bits(uint32_t hfi_version,struct ceph_oob_bits *out)
{
    if(!out || hfi_version==UINT32_MAX || (hfi_version>>28)!=2)return -1;
    if((hfi_version&31u)==0)*out=(struct ceph_oob_bits){1u<<16,1u<<16,1u<<24};
    else *out=(struct ceph_oob_bits){1u<<30,1u<<31,1u<<31};
    return 0;
}
enum ceph_oob_state {CE_OOB_NEW,CE_OOB_REQUESTED,CE_OOB_HELD,CE_OOB_RELEASED,CE_OOB_FAILED};
struct ceph_gmu_oob {enum ceph_oob_state state;struct ceph_oob_bits bits;unsigned writes;int error;};
struct ceph_oob_ops {
    void *ctx;
    int (*begin)(void *); /* GMU/HFI valid, sole OOB0 owner, thermal guard */
    int (*validate)(void *);
    int (*read)(void *,enum ceph_oob_reg,uint32_t *);
    int (*write)(void *,enum ceph_oob_reg,uint32_t);
    int (*barrier)(void *);
    int (*now)(void *,uint64_t *);
    int (*pause)(void *);
    int (*drained)(void *); /* admission closed + CP/fences idle before release */
    void (*end)(void *,int keep);
};
static inline int ceph_oob_read(struct ceph_oob_ops *b,enum ceph_oob_reg reg,uint32_t *v)
{return b->validate(b->ctx) || b->read(b->ctx,reg,v) || *v==UINT32_MAX?-1:0;}
static inline int ceph_oob_write(struct ceph_gmu_oob *s,struct ceph_oob_ops *b,enum ceph_oob_reg reg,uint32_t v)
{if(b->validate(b->ctx))return -1;++s->writes;return b->write(b->ctx,reg,v) || b->barrier(b->ctx)?-1:0;}
static inline int ceph_gmu_oob_acquire(struct ceph_gmu_oob *s,struct ceph_oob_ops *b,uint32_t hfi_version)
{
    if(!s || s->state!=CE_OOB_NEW || s->writes || !b || !b->begin || !b->validate || !b->read ||
        !b->write || !b->barrier || !b->now || !b->pause || !b->drained || !b->end ||
        ceph_gmu_oob_gpu_bits(hfi_version,&s->bits))return -1;
    int e=b->begin(b->ctx);if(e){s->state=CE_OOB_FAILED;s->error=e;return e;}
    uint32_t info;
    if(ceph_oob_read(b,CE_OOB_G2H_INFO,&info) || (info&s->bits.ack)) {e=-1;goto failed;}
    s->state=CE_OOB_REQUESTED;
    e=ceph_oob_write(s,b,CE_OOB_H2G_SET,s->bits.request);if(e)goto failed;
    uint64_t start,last,now;e=b->now(b->ctx,&start);if(e)goto failed;last=start;
    for(unsigned i=0;i<10000;++i) {
        if(b->now(b->ctx,&now) || ceph_oob_read(b,CE_OOB_G2H_INFO,&info)){e=-1;goto failed;}
        if(now<last || now-start>=100000000ULL){e=-2;goto failed;}last=now;
        if(info&s->bits.ack) {
            e=ceph_oob_write(s,b,CE_OOB_G2H_CLEAR,s->bits.ack);if(e)goto failed;
            /* Do not use arbitrary nonzero status as GX ready. Both power-off
             * bit6 and clock-off bit7 must be clear while request is held. */
            uint32_t gx;if(ceph_oob_read(b,CE_OOB_GX_STATUS,&gx) || (gx&0xc0u)){e=-1;goto failed;}
            s->state=CE_OOB_HELD;s->error=0;b->end(b->ctx,1);return 0;
        }
        e=b->pause(b->ctx);if(e)goto failed;
    }
    e=-2;
failed:s->state=CE_OOB_FAILED;s->error=e;b->end(b->ctx,s->writes!=0);return e;
}
static inline int ceph_gmu_oob_release(struct ceph_gmu_oob *s,struct ceph_oob_ops *b)
{
    if(!s || s->state!=CE_OOB_HELD || !b || !b->validate || !b->write || !b->barrier || !b->drained || !b->end)return -1;
    if(b->drained(b->ctx) || b->validate(b->ctx))return -1; /* retain held request */
    int e=ceph_oob_write(s,b,CE_OOB_H2G_SET,s->bits.release);
    s->error=e;s->state=e?CE_OOB_FAILED:CE_OOB_RELEASED;b->end(b->ctx,1);return e;
}
#endif
