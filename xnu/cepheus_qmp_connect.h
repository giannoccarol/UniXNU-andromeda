/* Cold, externally-owned AOP QMP link/channel negotiation. Only admits an
 * AOP-published MAIL v1 layout with ucore link UP, both channels DOWN and
 * all local states/ACKs DOWN. Already connected/partially inherited channels
 * are NOT reset or taken over. No fallback sizes: remote publishes TX/RX.
 * Writes only host-owned ucore ACKs and mcore link/channel state. Peer-owned
 * ACK fields need no initialization because cold preflight proved them DOWN.
 * Success retains external lease for the fixed ACD adapter, not a new lease.
 */
#ifndef CEPHEUS_QMP_CONNECT_H
#define CEPHEUS_QMP_CONNECT_H
#include "cepheus_qmp_mmio.h"
enum {CE_QMP_DOWN=0xffff0000u,CE_QMP_UP=0xffffu};
struct ceph_qmp_connect {
    struct ceph_qmp_descriptor expected;
    struct ceph_qmp_connection layout;
    unsigned writes,notifies;
    int terminal,connected,error;
    uint64_t start,last;
};
static inline int ceph_qmp_connect_snapshot(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m)
{
    struct ceph_qmp_descriptor d;
    if(ceph_qmp_mmio_snapshot(m,&d))return -1;
    for(unsigned i=0;i<16;++i) {
        if(i==5 || i==10 || i==12) {
            /* Remote channel state/local link ACK/local channel ACK may
             * advance only DOWN->UP. Drop/reset during negotiation is fatal. */
            if(d.words[i]!=CE_QMP_DOWN && d.words[i]!=CE_QMP_UP)return -1;
            if(s->expected.words[i]==CE_QMP_UP && d.words[i]!=CE_QMP_UP)return -1;
        } else if(d.words[i]!=s->expected.words[i])return -1;
    }
    if(m->ram[s->layout.tx.tx_offset/4] || m->ram[s->layout.rx.tx_offset/4])return -1;
    s->expected=d;return 0;
}
static inline int ceph_qmp_connect_write(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m,unsigned index)
{
    if(index!=4 && index!=9 && index!=11 && index!=6)return -1;
    if(ceph_qmp_connect_snapshot(s,m) || s->expected.words[index]!=CE_QMP_DOWN)return -1;
    ++s->writes;ceph_qmp_mmio_fence();m->ram[index]=CE_QMP_UP;ceph_qmp_mmio_fence();
    s->expected.words[index]=CE_QMP_UP;return ceph_qmp_connect_snapshot(s,m);
}
static inline int ceph_qmp_connect_notify(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m)
{
    if(ceph_qmp_connect_snapshot(s,m))return -1;
    ++s->notifies;ceph_qmp_mmio_fence();*m->irq=1;ceph_qmp_mmio_fence();return 0;
}
static inline int ceph_qmp_connect_wait(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m,unsigned index)
{
    if(index!=10 && index!=12)return -1;
    for(unsigned loops=0;loops<10000;++loops) {
        uint64_t now;
        if(m->now_ns(m->owner,&now) || now<s->last || now-s->start>=5000000000ULL)return -2;
        s->last=now;
        if(ceph_qmp_connect_snapshot(s,m))return -1;
        /* Remote open and local-open ACK can arrive in either order. ACK the
         * remote channel immediately, not only after our own ACK (deadlock). */
        if(index==12 && s->expected.words[5]==CE_QMP_UP && s->expected.words[6]==CE_QMP_DOWN) {
            if(ceph_qmp_connect_write(s,m,6) || ceph_qmp_connect_notify(s,m))return -1;
        }
        if(s->expected.words[index]==CE_QMP_UP && (index==10 || s->expected.words[6]==CE_QMP_UP))return 0;
        if(m->pause(m->owner))return -1;
    }
    return -2;
}
static inline int ceph_qmp_connect_cold(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m)
{
    if(!s || s->terminal || s->writes || !m || m->bound || m->active || m->terminal ||
        m->poisoned || m->writes || m->notified || ceph_qmp_mmio_resources(m))return -1;
    s->terminal=1;int e=ceph_qmp_mmio_snapshot(m,&s->expected);if(e)goto failed;
    e=ceph_qmp_descriptor_layout(&s->expected,&s->layout);if(e)goto failed;
    if(s->expected.words[3]!=CE_QMP_UP){e=-1;goto failed;}
    const unsigned down[]={4,5,6,9,10,11,12};
    for(unsigned i=0;i<7;++i)if(s->expected.words[down[i]]!=CE_QMP_DOWN){e=-1;goto failed;}
    e=ceph_qmp_connect_snapshot(s,m);if(e)goto failed;
    if(m->now_ns(m->owner,&s->start)){e=-1;goto failed;}s->last=s->start;
    /* Accept remote link, announce local link. No overwrite of ACKs. */
    e=ceph_qmp_connect_write(s,m,4);if(e)goto failed;
    e=ceph_qmp_connect_write(s,m,9);if(e)goto failed;
    e=ceph_qmp_connect_notify(s,m);if(e)goto failed;
    e=ceph_qmp_connect_wait(s,m,10);if(e)goto failed;
    /* Request local channel, wait for remote ACK and remote channel open. */
    e=ceph_qmp_connect_write(s,m,11);if(e)goto failed;
    e=ceph_qmp_connect_notify(s,m);if(e)goto failed;
    e=ceph_qmp_connect_wait(s,m,12);if(e)goto failed;
    e=ceph_qmp_connect_snapshot(s,m);if(e)goto failed;
    struct ceph_qmp_connection connected;
    e=ceph_qmp_descriptor_decode(&s->expected,&connected);if(e)goto failed;
    s->connected=1;return 0;
failed:
    s->error=e;
    if(s->writes || s->notifies) {
        m->terminal=1;m->poisoned=1;m->release(m->owner,1);
    }
    /* Preflight rejection leaves ownership cleanup to acquiring caller, same
     * as bind refusal; no attempts to disconnect/clear uncertain live state. */
    return e;
}
#endif
