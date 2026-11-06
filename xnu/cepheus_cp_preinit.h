/* Ordered CP-side halted pre-configuration: GX core subset -> GX perfcounter
 * block -> register protection/address modes -> ring/SQE addresses -> restore
 * snapshot + ME_INIT packet. This is the order a6xx_start() and the later
 * adreno_ringbuffer setup impose; each engine re-validates the halted window
 * itself, so this sequencer only fixes ordering and stops on first failure.
 *
 * NOT included and deliberately still absent: ZAP/PAS authentication, secure
 * CP aperture ownership, GX SMMU context installation, OOB keepalive, CP
 * unhalt, WPTR doorbell and any GPU command submission. Nothing here powers,
 * clocks, resets or votes. Caller must already own powered GX/SPTP/fence.
 */
#ifndef CEPHEUS_CP_PREINIT_H
#define CEPHEUS_CP_PREINIT_H
#include "cepheus_cp_workspace.h"
#include "cepheus_gx_perfcounter.h"
#include "cepheus_cp_ring_config.h"
#include "cepheus_cp_snapshot_mmio.h"
enum ceph_cp_preinit_stage {CE_CPI_NEW,CE_CPI_GX_CORE,CE_CPI_GX_PERF,CE_CPI_PROTECT,
    CE_CPI_RING,CE_CPI_RESTORE,CE_CPI_READY,CE_CPI_FAILED};
struct ceph_cp_preinit {
    enum ceph_cp_preinit_stage stage,failed_stage;
    int error;
    unsigned core_writes,perf_writes,protect_writes,ring_writes;
    uint32_t me_init[12];
    struct ceph_gx_core core;
    struct ceph_gx_perfcounter perf;
    struct ceph_cp_protection protect;
    struct ceph_cp_ring_config ring;
    struct ceph_cp_snapshot_mmio *snapshot;
};
/* The workspace must already be TABLED so ring/SQE/restore VAs are real page
 * table entries, not caller-supplied numbers. */
static inline int ceph_cp_preinit_ready(const struct ceph_cp_preinit *s,const struct ceph_cp_workspace *w)
{
    /* restore_ready must still be false: the snapshot is taken here and only
     * then sealed, so a pre-set flag would mean a foreign/duplicate capture. */
    if(!s || !w || s->stage!=CE_CPI_NEW || w->state!=CE_CP_WS_TABLED || w->restore_ready)return -1;
    if(!s->core.gmu || s->core.gmu!=s->perf.gmu || s->core.gmu!=s->protect.gmu ||
        s->core.gmu!=s->ring.gmu || s->core.gmu!=s->snapshot->gmu)return -1;
    if(s->core.gpu!=s->perf.gpu || s->core.gpu!=s->protect.gpu || s->core.gpu!=s->ring.gpu ||
        s->core.gpu!=s->snapshot->gpu || s->core.bytes!=0x40000 || s->ring.bytes!=0x40000)return -1;
    if(s->core.owner!=s->perf.owner || s->core.owner!=s->protect.owner ||
        s->core.owner!=s->ring.owner || s->core.owner!=s->snapshot->owner)return -1;
    if(s->core.lease!=s->perf.lease || s->core.lease!=s->protect.lease || s->core.lease!=s->ring.lease)return -1;
    /* Ring/SQE/RPTR VAs must be exactly the tabled regions, never re-typed. */
    const struct ceph_vm_region *r=w->regions;
    if(s->ring.input.ring_va!=r[CE_CP_RING].va || s->ring.input.ring_bytes!=r[CE_CP_RING].bytes ||
        s->ring.input.rptr_va!=r[CE_CP_RPTR].va || s->ring.input.rptr_bytes!=r[CE_CP_RPTR].bytes ||
        s->ring.input.sqe_va!=r[CE_CP_SQE].va || s->ring.input.sqe_bytes!=r[CE_CP_SQE].bytes)return -1;
    if(s->core.input.chipid!=0x06040001 || s->perf.core.chipid!=s->core.input.chipid)return -1;
    return 0;
}
static inline int ceph_cp_preinit_run(struct ceph_cp_preinit *s,struct ceph_cp_workspace *w)
{
    if(ceph_cp_preinit_ready(s,w))return -1;
    int e;
    s->stage=CE_CPI_GX_CORE;e=ceph_gx_core_configure(&s->core);
    s->core_writes=s->core.writes;if(e)goto failed;
    s->stage=CE_CPI_GX_PERF;e=ceph_gx_perf_configure(&s->perf);
    s->perf_writes=s->perf.writes;if(e)goto failed;
    s->stage=CE_CPI_PROTECT;e=ceph_cp_protection_install(&s->protect);
    s->protect_writes=s->protect.writes;if(e)goto failed;
    /* Ring configuration re-checks the full protection table before touching
     * any address, and refuses a cold context that is not empty. */
    s->stage=CE_CPI_RING;e=ceph_cp_ring_configure_halted(&s->ring);
    s->ring_writes=s->ring.writes;if(e)goto failed;
    /* Restore snapshot + ME_INIT are CPU-owned data here; nothing is published
     * to the CP and no doorbell is rung. */
    s->stage=CE_CPI_RESTORE;e=cepheus_cp_workspace_restore(w,s->snapshot,s->me_init);
    if(e)goto failed;
    s->stage=CE_CPI_READY;s->error=0;return 0;
failed:
    s->failed_stage=s->stage;s->stage=CE_CPI_FAILED;s->error=e;return e;
}
#endif
