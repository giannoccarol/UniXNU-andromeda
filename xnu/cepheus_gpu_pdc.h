/* A640-v2 PDC/RSC cold sequence from adreno_a6xx_gmu.c, corroborated by Linux
 * v6.12 a6xx_gmu.c. Pure planner plus bounded installer; no live backend supplied.
 * The three ePCB addresses are EXACT source values, not claimed resource names.
 * Runtime cmd-db must resolve them and admit all required indices. begin must
 * validate ownership of GPU PDC/RSC + quiescent CM3/GX and freeze those leases.
 * Nothing here touches the apps DRV TCS bank, so it cannot fix apps vote ownership.
 */
#ifndef CEPHEUS_GPU_PDC_H
#define CEPHEUS_GPU_PDC_H
#include "cepheus_arc.h"
enum ceph_pdc_window {CE_PDC_RSC,CE_PDC_CFG,CE_PDC_SEQ};
struct ceph_pdc_write {enum ceph_pdc_window window;uint32_t offset,value;};
struct ceph_pdc_input {
    uint32_t chipid;uint64_t cfg_pa,seq_pa;
    struct ceph_arc_resource resources[3]; /* exact order 30010,30000,30090 */
};
struct ceph_pdc_plan {struct ceph_pdc_write writes[48];unsigned count;};
static inline int ceph_pdc_append(struct ceph_pdc_plan *p,enum ceph_pdc_window window,uint32_t word,uint32_t value)
{
    if(p->count>=48 || word>UINT32_MAX/4)return -1;
    uint32_t off=word*4;
    if(window==CE_PDC_RSC){if(off<0x6a000)return -1;off-=0x6a000;}
    if(off>=(window==CE_PDC_RSC?0x30000u:0x10000u))return -1;
    p->writes[p->count++]=(struct ceph_pdc_write){window,off,value};return 0;
}
static inline int ceph_pdc_plan_a640v2(const struct ceph_pdc_input *in,struct ceph_pdc_plan *out)
{
    if(!in || !out || in->chipid!=0x06040001 || in->cfg_pa!=0xb290000 || in->seq_pa!=0xb490000)return -1;
    const uint32_t expected[]={0x30010,0x30000,0x30090};
    const unsigned needed[]={2,3,3};
    for(unsigned i=0;i<3;++i) {
        const struct ceph_arc_resource *r=&in->resources[i];
        if(r->address!=expected[i] || r->count<=needed[i] || r->count>16)return -1;
        for(unsigned j=1;j<r->count;++j)if(r->levels[j]<=r->levels[j-1])return -1;
    }
    struct ceph_pdc_plan p={0};
#define PDC_ADD(window,word,value) do {if(ceph_pdc_append(&p,window,word,value))return -1;} while(0)
    PDC_ADD(CE_PDC_RSC,0x23404,1u<<24);
    PDC_ADD(CE_PDC_RSC,0x2340b,1);
    for(unsigned i=0;i<3;++i) {
        PDC_ADD(CE_PDC_RSC,0x2340eu+2*i,i==2?0x80000000u:0);
        PDC_ADD(CE_PDC_RSC,0x2340d+2*i,0);
    }
    PDC_ADD(CE_PDC_RSC,0x23500,0);
    PDC_ADD(CE_PDC_RSC,0x23408,0x4520);
    PDC_ADD(CE_PDC_RSC,0x23409,0x4510);
    PDC_ADD(CE_PDC_RSC,0x2340a,0x4514);
    const uint32_t rsc[]={0xa7a506a0,0xa1e6a6e7,0xa2e081e1,0xe9a982e2,0x0020e8a8};
    const uint32_t seq[]={0xfebea1e1,0xa5a4a3a2,0x8382a6e0,0xbce3e284,0x002081fc};
    for(unsigned i=0;i<5;++i)PDC_ADD(CE_PDC_RSC,0x23580+i,rsc[i]);
    for(unsigned i=0;i<5;++i)PDC_ADD(CE_PDC_SEQ,i,seq[i]);
    for(unsigned t=0;t<2;++t) {
        uint32_t base=t?0x15d6:0x1572;
        PDC_ADD(CE_PDC_CFG,base,0);
        PDC_ADD(CE_PDC_CFG,base+2,0);
        for(unsigned c=0;c<3;++c) {
            PDC_ADD(CE_PDC_CFG,base+3+c*4,0x10108);
            PDC_ADD(CE_PDC_CFG,base+4+c*4,expected[c]);
            PDC_ADD(CE_PDC_CFG,base+5+c*4,c==0?(t?2:1):(t?3:0));
        }
    }
    /* Unlike reference's initialization code, keep bank enable off until both
     * payloads are complete. PDC itself remains disabled until the last write. */
    PDC_ADD(CE_PDC_CFG,0x1573,7);
    PDC_ADD(CE_PDC_CFG,0x15d7,7);
    PDC_ADD(CE_PDC_CFG,0x1148,0);
    PDC_ADD(CE_PDC_CFG,0x1140,0x80000001u);
#undef PDC_ADD
    *out=p;return 0;
}
struct ceph_pdc_ops {
    void *ctx;
    int (*begin)(void *,const struct ceph_pdc_input *);
    int (*validate)(void *);
    int (*read)(void *,enum ceph_pdc_window,uint32_t,uint32_t *);
    int (*write)(void *,enum ceph_pdc_window,uint32_t,uint32_t);
    int (*barrier)(void *);
    void (*end)(void *,int keep);
};
struct ceph_pdc_install {unsigned attempted;int terminal,quarantined;};
static inline int ceph_pdc_install_a640v2(struct ceph_pdc_install *s,struct ceph_pdc_ops *b,const struct ceph_pdc_input *in)
{
    if(!s || s->terminal || s->attempted || !b || !b->begin || !b->validate || !b->read || !b->write || !b->barrier || !b->end)return -1;
    struct ceph_pdc_plan p;if(ceph_pdc_plan_a640v2(in,&p))return -1;
    s->terminal=1;int e=b->begin(b->ctx,in);if(e)return e;
    const uint32_t checks[]={0x1140*4,0x1573*4,0x15d7*4};
    for(unsigned i=0;i<3;++i) {
        uint32_t v;if(b->validate(b->ctx) || b->read(b->ctx,CE_PDC_CFG,checks[i],&v) || v) {e=-1;goto done;}
    }
    for(unsigned i=0;i<p.count;++i) {
        struct ceph_pdc_write *w=&p.writes[i];
        if(b->validate(b->ctx)){e=-1;goto done;}
        ++s->attempted;e=b->write(b->ctx,w->window,w->offset,w->value);if(e)goto done;
        e=b->barrier(b->ctx);if(e)goto done;
        /* RSC_STATUS0 is a control/status mixed register; do not compare all
         * returned status bits to a write. Check stable configuration elsewhere. */
        if(i) {
            uint32_t v;
            if(b->read(b->ctx,w->window,w->offset,&v) || v!=w->value){e=-1;goto done;}
        }
    }
done:
    if(e && s->attempted)s->quarantined=1;
    b->end(b->ctx,s->attempted!=0);return e;
}
#endif
