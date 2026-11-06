/* A640 cold-start power transaction, platform backend NOT provided/enabled.
 * prepare() must acquire shared ARC/clock/GDSC ownership and return CURRENT
 * apps-DRV votes (not empty TCS contents), decoded cmd-db tables and a measured
 * 300MHz GPLL0-div parent AND live CXO. Verify under the same lease; no bare ready flags.
 * Reads/writes are narrowly named resources, not arbitrary caller MMIO.
 * No GX writes, firmware upload, reset release, voltage lowering or global reset.
 */
#ifndef CEPHEUS_GPU_POWER_TXN_H
#define CEPHEUS_GPU_POWER_TXN_H
#include "cepheus_arc.h"
#include "cepheus_gpu_power.h"

enum ceph_power_reg { CE_PWR_CX, CE_PWR_CX_STATUS, CE_PWR_GX, CE_PWR_CFG_AHB,
    CE_PWR_RCG, CE_PWR_RCG_CFG, CE_PWR_GMU, CE_PWR_CXO, CE_PWR_DDR,
    CE_PWR_MEMNOC, CE_PWR_AHB, CE_PWR_DVM, CE_PWR_CM3_RESET, CE_PWR_REG_COUNT };
enum ceph_power_state { CE_PWR_NEW, CE_PWR_OWNED, CE_PWR_VOTES, CE_PWR_DOMAIN,
    CE_PWR_CLOCKS, CE_PWR_READY, CE_PWR_REFUSED, CE_PWR_QUARANTINED };
enum { CE_PWR_INVALID=-1, CE_PWR_BUSY=-2, CE_PWR_TIMEOUT=-3, CE_PWR_IO=-4 };
struct ceph_power_input {
    struct ceph_arc_resource cx, mx;
    unsigned current_cx, current_mx;
    uint32_t parent_hz;
};
struct ceph_power_plan { unsigned cx_index, mx_index; uint32_t rcg_cfg; };
struct ceph_power_ops {
    void *ctx;
    /* Failure must release any partial acquisition, with no hardware writes. */
    int (*prepare)(void *, struct ceph_power_input *);
    int (*validate_lease)(void *); /* includes foreign writers and inherited votes */
    int (*read)(void *, enum ceph_power_reg, uint32_t *);
    int (*write)(void *, enum ceph_power_reg, uint32_t);
    int (*vote)(void *, uint32_t, unsigned); /* ARC addr/index; bounded ACK */
    int (*delay_us)(void *, unsigned); /* real monotonic delay, not success stub */
    /* keep=1 retains rail/clock ownership+buffers for explicit future recovery.
     * Releases the serialization lock only. No automatic vote-off/free/reset. */
    void (*finish)(void *, int keep);
};
struct ceph_power_txn { enum ceph_power_state state; int error; unsigned effects; };

static inline int ceph_power_plan(const struct ceph_power_input *in, struct ceph_power_plan *out)
{
    if (!in || !out || in->parent_hz!=300000000 || !in->cx.count || !in->mx.count ||
        in->cx.count>16 || in->mx.count>16 || in->current_cx>=in->cx.count ||
        in->current_mx>=in->mx.count || (in->cx.address>>16)!=3 || (in->mx.address>>16)!=3 ||
        ((in->cx.address|in->mx.address)&3u) || in->cx.address==in->mx.address)
        return CE_PWR_INVALID;
    unsigned c,m;
    /* KGSL setup_volt_dependency_tbl takes the first primary level >=48,
     * then the first MX level >= that ACTUAL rounded primary, not >=48.
     * No exact-48 assumption, and no jump to a highest-table overvote. */
    int e=ceph_arc_pair(&in->cx,&in->mx,48,UINT16_MAX,&c,&m);
    if (e) return CE_PWR_INVALID;
    /* Never lower a pre-existing apps-DRV request. Increase MX first so it
     * covers the actual preserved CX request, not merely our 48 minimum. */
    if (in->current_cx>c) c=in->current_cx;
    if (ceph_arc_select(&in->mx,in->cx.levels[c],UINT16_MAX,&m)) return CE_PWR_INVALID;
    if (in->current_mx>m) m=in->current_mx;
    *out=(struct ceph_power_plan){c,m,(6u<<8)|2u}; /* 300MHz / 1.5 */
    return 0;
}
static inline int ceph_power_read(struct ceph_power_ops *b, enum ceph_power_reg reg, uint32_t *v)
{
    if (b->validate_lease(b->ctx) || b->read(b->ctx,reg,v) || *v==UINT32_MAX) return CE_PWR_IO;
    return 0;
}
static inline int ceph_power_poll(struct ceph_power_ops *b, enum ceph_power_reg reg,
    uint32_t mask,uint32_t wanted,unsigned us)
{
    for (unsigned i=0;i<us;++i) {
        uint32_t v;
        if (ceph_power_read(b,reg,&v)) return CE_PWR_IO;
        if ((v&mask)==wanted) return 0;
        if (b->delay_us(b->ctx,1)) return CE_PWR_IO;
    }
    return CE_PWR_TIMEOUT;
}
static inline int ceph_power_update(struct ceph_power_txn *t,struct ceph_power_ops *b,
    enum ceph_power_reg reg,uint32_t mask,uint32_t wanted)
{
    uint32_t before,after;
    if (ceph_power_read(b,reg,&before)) return CE_PWR_IO;
    uint32_t next=(before&~mask)|(wanted&mask);
    if (next==before) return 0;
    if (b->validate_lease(b->ctx)) return CE_PWR_BUSY;
    ++t->effects; /* count attempted writes too: a failed write may have landed */
    if (b->write(b->ctx,reg,next) || ceph_power_read(b,reg,&after) ||
        (after&mask)!=(next&mask)) return CE_PWR_IO;
    return 0;
}
static inline int ceph_power_start(struct ceph_power_txn *t,struct ceph_power_ops *b)
{
    if (!t || t->state!=CE_PWR_NEW || t->effects || !b || !b->prepare ||
        !b->validate_lease || !b->read || !b->write || !b->vote || !b->delay_us || !b->finish)
        return CE_PWR_INVALID;
    struct ceph_power_input in={0};struct ceph_power_plan plan;
    int e=b->prepare(b->ctx,&in);
    if (e) { t->error=e;t->state=CE_PWR_REFUSED;return e; }
    t->state=CE_PWR_OWNED;
    e=ceph_power_plan(&in,&plan);if(e) goto failed;
    uint32_t cx,hw,gx,cfg,gmu,rcg,reset;
    if (ceph_power_read(b,CE_PWR_CX,&cx) || ceph_power_read(b,CE_PWR_CX_STATUS,&hw) ||
        ceph_power_read(b,CE_PWR_GX,&gx) || ceph_power_read(b,CE_PWR_CFG_AHB,&cfg) ||
        ceph_power_read(b,CE_PWR_GMU,&gmu) || ceph_power_read(b,CE_PWR_RCG,&rcg) ||
        ceph_power_read(b,CE_PWR_CM3_RESET,&reset)) {
        e=CE_PWR_IO;goto failed;
    }
    if ((cx&7u)!=1u || (hw&(1u<<31)) || (gx&(1u<<31)) || !(gx&1u) ||
        !ceph_gpu_branch_active(cfg) || !(gmu&(1u<<31)) || (gmu&1u) ||
        !(rcg&(1u<<31)) || (rcg&3u) || reset!=1) { e=CE_PWR_BUSY;goto failed; }
    t->state=CE_PWR_VOTES;
    const unsigned wanted[]={plan.mx_index,plan.cx_index};
    const unsigned current[]={in.current_mx,in.current_cx};
    const uint32_t address[]={in.mx.address,in.cx.address};
    for(unsigned i=0;i<2;++i) {
        if(wanted[i]==current[i]) continue;
        if(b->validate_lease(b->ctx)) { e=CE_PWR_BUSY;goto failed; }
        ++t->effects;e=b->vote(b->ctx,address[i],wanted[i]);if(e) goto failed;
    }
    t->state=CE_PWR_DOMAIN;
    /* Rest=2/few=8/clock-disable=8, then clear SW_COLLAPSE. */
    e=ceph_power_update(t,b,CE_PWR_CX,0x00fff000u,0x00288000u);if(e) goto failed;
    e=ceph_power_update(t,b,CE_PWR_CX,1u,0);if(e) goto failed;
    if(b->delay_us(b->ctx,1)) {e=CE_PWR_IO;goto failed;}
    e=ceph_power_poll(b,CE_PWR_CX_STATUS,1u<<31,1u<<31,500);if(e) goto failed;
    if(b->delay_us(b->ctx,2)) {e=CE_PWR_IO;goto failed;}
    t->state=CE_PWR_CLOCKS;
    /* GPUCC enables safe_config: clk_rcg2_enable forces the root during mux
     * switching, then relinquishes the force vote. Both parent sources are
     * already held by prepare(). No PLL programming. Disable dual-edge mode
     * (bits13:12) and HW clock control(bit20) per clk-rcg2.c before UPDATE. */
    e=ceph_power_update(t,b,CE_PWR_RCG,2u,2u);if(e) goto failed;
    e=ceph_power_poll(b,CE_PWR_RCG,1u<<31,0,500);if(e) goto failed;
    e=ceph_power_update(t,b,CE_PWR_RCG_CFG,0x0010371fu,plan.rcg_cfg);if(e) goto failed;
    uint32_t cmd;if(ceph_power_read(b,CE_PWR_RCG,&cmd)) {e=CE_PWR_IO;goto failed;}
    /* read() may sleep or observe lease expiry. Revalidate immediately before
     * the non-RMW UPDATE store too; all other writes already use update(). */
    if(b->validate_lease(b->ctx)) {e=CE_PWR_BUSY;goto failed;}
    ++t->effects;e=b->write(b->ctx,CE_PWR_RCG,cmd|1u);if(e) goto failed;
    e=ceph_power_poll(b,CE_PWR_RCG,1u,0,500);if(e) goto failed;
    e=ceph_power_update(t,b,CE_PWR_RCG,2u,0);if(e) goto failed;
    const enum ceph_power_reg clocks[]={CE_PWR_GMU,CE_PWR_CXO,CE_PWR_DDR,
        CE_PWR_MEMNOC,CE_PWR_AHB,CE_PWR_DVM};
    for(unsigned i=0;i<sizeof(clocks)/sizeof(clocks[0]);++i) {
        e=ceph_power_update(t,b,clocks[i],1,1);if(e) goto failed;
        e=ceph_power_poll(b,clocks[i],1u<<31,0,500);if(e) goto failed;
    }
    e=ceph_power_poll(b,CE_PWR_RCG,(1u<<31)|1u,0,500);if(e) goto failed;
    if(ceph_power_read(b,CE_PWR_CM3_RESET,&reset) || reset!=1) {
        e=CE_PWR_BUSY;goto failed; /* no blind reset of an inherited running GMU */
    }
    uint32_t selected;
    if (ceph_power_read(b,CE_PWR_RCG_CFG,&selected) ||
        (selected&0x0010371fu)!=plan.rcg_cfg) {e=CE_PWR_BUSY;goto failed;}
    t->state=CE_PWR_READY;t->error=0;b->finish(b->ctx,1);return 0;
failed:
    t->error=e;t->state=t->effects ? CE_PWR_QUARANTINED : CE_PWR_REFUSED;
    b->finish(b->ctx,t->effects!=0);return e;
}
#endif
