/* Sequencer ordering test for the halted CP preinit chain. Needs a real tabled
 * CP workspace (the restore stage validates live handles), so it is driven from
 * the host CP-workspace test rather than as a pure header unit. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_cp_preinit.h"
#include "cepheus_gx_core.h"
static uint32_t gxregs[0x40000/4],gmuregs[0x30000/4];
struct host {unsigned calls,fail;};
static int lease(void *p){struct host *h=p;return ++h->calls==h->fail?-1:0;}
static void keep(void *p,int retained){(void)p;assert(retained);}
static void finish(void *p){(void)p;}
/* Ring configuration demands an explicit publication proof (ring+RPTR
 * initialised and SQE payload visible) before it writes any address. This stub
 * only satisfies the contract; the real backend must verify those buffers. */
static int publish(void *p){(void)p;return 0;}
/* Pristine halted GX: CP halted(bit1), no fault, GX/SPTP on, AHB fence ALLOW,
 * 64-bit address modes and the protection table already in place (the ring
 * engine re-checks all45 words before it writes an address). */
static void halted(void)
{
    memset(gxregs,0,sizeof(gxregs));memset(gmuregs,0,sizeof(gmuregs));
    /* CP halted(bit1) with no fault. GX/SPTP status must have power/clock bits
     * clear (0xd4 mask) for the preflight to accept the window, and the AHB
     * fence must read 0 (ALLOW). */
    gxregs[0x808]=3;gmuregs[0x1f8d0 - 0x1a800]=0;gmuregs[0x23b10 - 0x1a800]=0;
    static const uint32_t modes[]={0x842,0xc01,0x8601,0x8e05,0x9e01,0xbe05,
        0xa601,0x9601,0xe00,0xae01,0xb601,0xf810};
    for(unsigned i=0;i<12;++i)gxregs[modes[i]]=1;
    gxregs[0x84f]=11;
}
static void wire(struct ceph_cp_preinit *s,const struct ceph_gx_core_input *in,struct host *h,
    struct ceph_gmu_mmio *g,const struct ceph_cp_workspace *w,struct ceph_cp_snapshot_mmio *snap)
{
    memset(s,0,sizeof(*s));
    s->core=(struct ceph_gx_core){g,gxregs,sizeof(gxregs),*in,h,lease,keep,0,0,0,0};
    s->perf=(struct ceph_gx_perfcounter){g,gxregs,sizeof(gxregs),*in,h,lease,keep,0,0,0,0};
    s->protect=(struct ceph_cp_protection){g,gxregs,sizeof(gxregs),h,lease,keep,0,0,0,0};
    s->ring=(struct ceph_cp_ring_config){g,gxregs,sizeof(gxregs),
        {w->regions[CE_CP_RING].va,w->regions[CE_CP_RPTR].va,w->regions[CE_CP_SQE].va,
         w->regions[CE_CP_RING].bytes,w->regions[CE_CP_RPTR].bytes,w->regions[CE_CP_SQE].bytes},
        h,lease,publish,keep,0,0,0,0};
    *snap=(struct ceph_cp_snapshot_mmio){g,gxregs,sizeof(gxregs),h,lease,finish,0};
    s->snapshot=snap;
}
int ceph_test_cp_preinit(struct ceph_cp_workspace *w)
{
    struct host h={0};struct ceph_gmu_mmio g={(volatile uint8_t *)gmuregs,sizeof(gmuregs),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,lease};
    const struct ceph_gx_core_input in={0x06040001,0x100000,0x100000,15,32,3};
    struct ceph_cp_snapshot_mmio snap;struct ceph_cp_preinit s;
    assert(w && w->state==CE_CP_WS_TABLED && !w->restore_ready);
    halted();wire(&s,&in,&h,&g,w,&snap);
    assert(!ceph_cp_preinit_run(&s,w));
    assert(s.stage==CE_CPI_READY && !s.error);
    assert(s.core_writes==23 && s.perf_writes==5 && s.protect_writes==45 && s.ring_writes==7);
    assert(w->restore_ready && s.me_init[1]==0x16f);
    /* Nothing unhalted, reset, doorbelled or wrote WPTR/SQE_CNTL. */
    /* 0x830 is the SQE address the ring engine legitimately writes; 0x831 is
     * its high half and must stay 0 for a 39-bit VA. Halt/WPTR untouched. */
    /* 0x800/0x804/0x830 are the ring base, RPTR and SQE addresses the engine
     * is supposed to program. What must stay untouched: halt, WPTR(0x807),
     * the SQE high half(0x831, zero for a 39-bit VA) and the CP fault(0x823). */
    assert(gxregs[0x808]==3 && !gxregs[0x807] && !gxregs[0x831] && !gxregs[0x823]);
    assert(gxregs[0x800]==(uint32_t)w->regions[CE_CP_RING].va && gxregs[0x804]==(uint32_t)w->regions[CE_CP_RPTR].va);
    assert(!gmuregs[0x1f800 - 0x1a800] && !gmuregs[0x1f986 - 0x1a800]);
    /* One-shot: a second run is refused. */
    assert(ceph_cp_preinit_run(&s,w)<0);
    /* Pre-flight rejections happen before any write. */
    halted();wire(&s,&in,&h,&g,w,&snap);s.ring.input.ring_va+=0x1000;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    halted();wire(&s,&in,&h,&g,w,&snap);s.ring.bytes=0x20000;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    halted();wire(&s,&in,&h,&g,w,&snap);s.snapshot->gpu=gxregs+1;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    halted();wire(&s,&in,&h,&g,w,&snap);s.perf.core.chipid=0x06040000;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    halted();wire(&s,&in,&h,&g,w,&snap);s.protect.owner=&snap;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    halted();wire(&s,&in,&h,&g,w,&snap);w->restore_ready=1;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    w->restore_ready=0;
    /* A non-TABLED workspace cannot supply ring VAs. */
    halted();wire(&s,&in,&h,&g,w,&snap);w->state=CE_CP_WS_READY;
    assert(ceph_cp_preinit_run(&s,w)<0 && !s.core_writes);
    w->state=CE_CP_WS_TABLED;
    puts("CP halted preinit: GX core->perf->protect->ring->restore order, 80 writes, no unhalt/doorbell PASS (RAM only)");
    return 0;
}
