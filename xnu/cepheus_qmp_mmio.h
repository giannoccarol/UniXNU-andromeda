/* AOP QMP descriptor decoder + restricted volatile ACD transport. No ioremap,
 * negotiation, descriptor writes, remote-RX consumption or channel takeover.
 * A real exclusive owner must hold mapping/transport lifetime across bind/send
 * and serialize ALL other QMP clients; connected descriptor alone isn't a lease.
 */
#ifndef CEPHEUS_QMP_MMIO_H
#define CEPHEUS_QMP_MMIO_H
#include "cepheus_qmp_acd.h"
struct ceph_qmp_descriptor {uint32_t words[16];};
struct ceph_qmp_connection {struct ceph_qmp_layout tx,rx;};
static inline int ceph_qmp_descriptor_layout(const struct ceph_qmp_descriptor *d,
    struct ceph_qmp_connection *out)
{
    if(!d || !out || d->words[0]!=0x4d41494c || d->words[1]!=1 || d->words[2] || d->words[15])return -1;
    struct ceph_qmp_connection c={{d->words[14],d->words[13]},{d->words[8],d->words[7]}};
    const struct ceph_qmp_layout *layouts[]={&c.tx,&c.rx};
    for(unsigned i=0;i<2;++i) {
        const struct ceph_qmp_layout *l=layouts[i];
        if((l->tx_offset|l->tx_bytes)&3u || l->tx_offset<64 || l->tx_bytes<4 ||
            l->tx_bytes>4096 || l->tx_offset>4096-l->tx_bytes)return -1;
    }
    if(c.tx.tx_bytes<36 || (c.tx.tx_offset<c.rx.tx_offset+c.rx.tx_bytes &&
        c.rx.tx_offset<c.tx.tx_offset+c.tx.tx_bytes))return -1;
    *out=c;return 0;
}
static inline int ceph_qmp_descriptor_decode(const struct ceph_qmp_descriptor *d,
    struct ceph_qmp_connection *out)
{
    if(!d)return -1;
    /* ucore and mcore link_state/ack, ch_state/ack must ALL be connected. */
    for(unsigned core=0;core<2;++core)for(unsigned i=0;i<4;++i)
        if(d->words[3+6*core+i]!=0xffff)return -1;
    return ceph_qmp_descriptor_layout(d,out);
}
struct ceph_qmp_mmio {
    volatile uint32_t *ram,*irq;
    uint64_t ram_pa,irq_pa;
    size_t bytes;
    void *owner;
    int (*lease)(void *);
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *);
    void (*release)(void *,int uncertain);
    struct ceph_qmp_descriptor identity;
    struct ceph_qmp_connection connection;
    uint32_t payload[8];
    unsigned bound,active,terminal,poisoned,writes,notified;
};
static inline void ceph_qmp_mmio_fence(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error "QMP MMIO requires arm64"
#endif
}
static inline int ceph_qmp_mmio_resources(struct ceph_qmp_mmio *m)
{
    return !m || !m->ram || !m->irq || (((uintptr_t)m->ram|(uintptr_t)m->irq)&3u) ||
        m->ram_pa!=0xc300000 || m->irq_pa!=0x17c0000c || m->bytes!=4096 ||
        !m->lease || !m->now_ns || !m->pause || !m->release || m->lease(m->owner)?-1:0;
}
static inline int ceph_qmp_mmio_snapshot(struct ceph_qmp_mmio *m,struct ceph_qmp_descriptor *d)
{
    if(!d || ceph_qmp_mmio_resources(m))return -1;
    for(unsigned i=0;i<16;++i)d->words[i]=m->ram[i];
    ceph_qmp_mmio_fence();
    /* Reject a visibly changing descriptor; ABA cannot be excluded without
     * the external lifetime/firmware-generation lease. Do not infer one here. */
    for(unsigned i=0;i<16;++i)if(d->words[i]!=m->ram[i])return -1;
    return m->lease(m->owner);
}
static inline int ceph_qmp_mmio_bind(struct ceph_qmp_mmio *m,unsigned enabled)
{
    if(!m || m->bound || m->terminal || m->active || m->writes || m->notified || m->poisoned)return -1;
    struct ceph_qmp_descriptor d;struct ceph_qmp_connection c;char text[33];
    if(ceph_qmp_acd_text(text,enabled)!=32 || ceph_qmp_mmio_snapshot(m,&d) ||
        ceph_qmp_descriptor_decode(&d,&c))return -1;
    if(m->ram[c.tx.tx_offset/4] || m->ram[c.rx.tx_offset/4])return -1;
    m->identity=d;m->connection=c;
    for(unsigned i=0;i<8;++i) {
        uint32_t word=0;for(unsigned j=0;j<4;++j)word|=(uint32_t)(uint8_t)text[4*i+j]<<(8*j);
        m->payload[i]=word;
    }
    m->bound=1;return 0;
}
static inline int ceph_qmp_mmio_validate(void *ctx)
{
    struct ceph_qmp_mmio *m=ctx;struct ceph_qmp_descriptor d;
    if(!m || !m->bound || !m->active || m->terminal || m->poisoned)return -1;
    if(ceph_qmp_mmio_snapshot(m,&d))goto fail;
    for(unsigned i=0;i<16;++i)if(d.words[i]!=m->identity.words[i])goto fail;
    if(m->ram[m->connection.rx.tx_offset/4])goto fail;
    return 0;
fail:
    m->poisoned=1;return -1;
}
static inline int ceph_qmp_mmio_begin(void *ctx,struct ceph_qmp_layout *out)
{
    struct ceph_qmp_mmio *m=ctx;
    if(!out || !m || !m->bound || m->active || m->terminal || m->poisoned)return -1;
    m->active=1;
    if(ceph_qmp_mmio_validate(m) || m->ram[m->connection.tx.tx_offset/4]) {
        m->active=0;m->bound=0;m->terminal=1;m->release(m->owner,0);return -1;
    }
    *out=m->connection.tx;return 0;
}
static inline int ceph_qmp_mmio_read(void *ctx,uint32_t off,uint32_t *v)
{
    struct ceph_qmp_mmio *m=ctx;
    if(!v || ceph_qmp_mmio_validate(m) || off!=m->connection.tx.tx_offset)return -1;
    *v=m->ram[off/4];ceph_qmp_mmio_fence();return 0;
}
static inline int ceph_qmp_mmio_write(void *ctx,uint32_t off,uint32_t v)
{
    struct ceph_qmp_mmio *m=ctx;
    if(ceph_qmp_mmio_validate(m) || m->writes>=9 || m->notified)return -1;
    uint32_t tx=m->connection.tx.tx_offset;
    if(m->ram[tx/4] || (m->writes<8?(off!=tx+4+4*m->writes || v!=m->payload[m->writes]):(off!=tx || v!=32)))return -1;
    ++m->writes;ceph_qmp_mmio_fence();m->ram[off/4]=v;ceph_qmp_mmio_fence();return 0;
}
static inline int ceph_qmp_mmio_barrier(void *ctx)
{if(ceph_qmp_mmio_validate(ctx))return -1;ceph_qmp_mmio_fence();return 0;}
static inline int ceph_qmp_mmio_notify(void *ctx)
{
    struct ceph_qmp_mmio *m=ctx;
    if(ceph_qmp_mmio_validate(m) || m->writes!=9 || m->notified || m->ram[m->connection.tx.tx_offset/4]!=32)return -1;
    for(unsigned i=0;i<8;++i)if(m->ram[m->connection.tx.tx_offset/4+1+i]!=m->payload[i])return -1;
    m->notified=1;ceph_qmp_mmio_fence();*m->irq=1;ceph_qmp_mmio_fence();return 0;
}
static inline int ceph_qmp_mmio_now(void *ctx,uint64_t *now)
{struct ceph_qmp_mmio *m=ctx;return !now || ceph_qmp_mmio_validate(m)?-1:m->now_ns(m->owner,now);}
static inline int ceph_qmp_mmio_pause(void *ctx)
{struct ceph_qmp_mmio *m=ctx;return ceph_qmp_mmio_validate(m)?-1:m->pause(m->owner);}
static inline void ceph_qmp_mmio_end(void *ctx,int uncertain)
{
    struct ceph_qmp_mmio *m=ctx;if(!m || !m->bound || !m->active)return;
    m->active=0;m->bound=0;m->terminal=1;
    m->release(m->owner,uncertain || (m->poisoned && m->writes));
}
static inline int ceph_qmp_mmio_ops(struct ceph_qmp_mmio *m,struct ceph_qmp_ops *out)
{
    if(!m || !m->bound || m->terminal || !out)return -1;
    *out=(struct ceph_qmp_ops){m,ceph_qmp_mmio_begin,ceph_qmp_mmio_validate,ceph_qmp_mmio_read,
        ceph_qmp_mmio_write,ceph_qmp_mmio_barrier,ceph_qmp_mmio_notify,ceph_qmp_mmio_now,
        ceph_qmp_mmio_pause,ceph_qmp_mmio_end};return 0;
}
#endif
