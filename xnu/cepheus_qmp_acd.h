/* Dedicated GPU ACD QMP message on an ALREADY CONNECTED, exclusively leased
 * AOP mailbox. Does not negotiate/steal a shared channel. Only fixed ACD text,
 * no arbitrary remote command. Length=bytes excluding NUL (KGSL scnprintf).
 * Consumption ACK (length clears) is not an application-level ACD proof.
 */
#ifndef CEPHEUS_QMP_ACD_H
#define CEPHEUS_QMP_ACD_H
#include <stdint.h>
#include <stddef.h>
struct ceph_qmp_layout {uint32_t tx_offset,tx_bytes;};
struct ceph_qmp_ops {
    void *ctx;
    int (*begin)(void *,struct ceph_qmp_layout *); /* owns shared region/IPCC; may not mutate on failure */
    int (*validate)(void *); /* still connected + same descriptor/generation/ownership */
    int (*read32)(void *,uint32_t offset,uint32_t *);
    int (*write32)(void *,uint32_t offset,uint32_t);
    int (*barrier)(void *);
    int (*notify)(void *);
    int (*now_ns)(void *,uint64_t *);
    int (*pause)(void *);
    void (*end)(void *,int uncertain);
};
struct ceph_qmp_tx {int terminal,consumed,uncertain;unsigned writes;};
static inline int ceph_qmp_acd_text(char out[33],unsigned enabled)
{
    if(!out || enabled>1)return -1;
    static const char message[]="{class: gpu, res: acd, value: 0}";
    for(unsigned i=0;i<sizeof(message);++i)out[i]=message[i];
    out[30]=(char)('0'+enabled);
    return (int)sizeof(message)-1;
}
static inline int ceph_qmp_send_acd(struct ceph_qmp_tx *s,struct ceph_qmp_ops *b,unsigned enabled)
{
    if(!s || s->terminal || !b || !b->begin || !b->validate || !b->read32 || !b->write32 ||
        !b->barrier || !b->notify || !b->now_ns || !b->pause || !b->end)return -1;
    char text[33]={0};int length=ceph_qmp_acd_text(text,enabled);if(length<0)return -1;
    unsigned padded=((unsigned)length+3)&~3u;
    struct ceph_qmp_layout l={0};s->terminal=1;
    int e=b->begin(b->ctx,&l);if(e)return e;
    if((l.tx_offset&3u) || l.tx_offset<64 || l.tx_bytes<padded+4 || l.tx_bytes>4096 ||
        l.tx_offset>65536-l.tx_bytes){e=-1;goto out;}
    uint32_t current;
    if(b->validate(b->ctx) || b->read32(b->ctx,l.tx_offset,&current) || current){e=-1;goto out;}
    for(unsigned i=0;i<padded;i+=4) {
        uint32_t word=0;for(unsigned j=0;j<4;++j)word|=(uint32_t)(uint8_t)text[i+j]<<(8*j);
        if(b->validate(b->ctx)){e=-1;goto out;}
        ++s->writes;e=b->write32(b->ctx,l.tx_offset+4+i,word);if(e)goto out;
    }
    e=b->barrier(b->ctx);if(e)goto out;
    if(b->validate(b->ctx)){e=-1;goto out;}
    ++s->writes;e=b->write32(b->ctx,l.tx_offset,(unsigned)length);if(e)goto out;
    e=b->barrier(b->ctx);if(e)goto out;
    if(b->read32(b->ctx,l.tx_offset,&current) || current!=(unsigned)length){e=-1;goto out;}
    e=b->notify(b->ctx);if(e)goto out;
    uint64_t start,last,now;
    e=b->now_ns(b->ctx,&start);if(e)goto out;last=start;
    for(unsigned i=0;i<10000;++i) {
        if(b->validate(b->ctx) || b->now_ns(b->ctx,&now)){e=-1;goto out;}
        if(now<last || now-start>=1000000000ULL){e=-2;goto out;}last=now;
        e=b->read32(b->ctx,l.tx_offset,&current);if(e)goto out;
        if(!current){s->consumed=1;e=0;goto out;}
        if(current!=(unsigned)length){e=-1;goto out;}
        e=b->pause(b->ctx);if(e)goto out;
    }
    e=-2;
out:
    if(e && s->writes)s->uncertain=1;
    b->end(b->ctx,s->uncertain);return e;
}
#endif
