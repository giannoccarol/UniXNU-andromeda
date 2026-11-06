#include "cepheus_gmu_bringup.h"
#include "cepheus_gpu_compile.h"
#include "cepheus_smmu_detach.h"
#include "cepheus_gmu_slumber.h"
#include "cepheus_gmu_park.h"
#if defined(KERNEL)
#include <sys/systm.h>
#else
#include <stdio.h>
#endif

int cepheus_gmu_bringup_thermal_check(struct ceph_gmu_bringup *s)
{
    if(!s || !s->thermal.active || !s->backend.temperature || !s->backend.now_ns)return CE_THERMAL_INVALID;
    if(s->thermal.failure)return CE_THERMAL_LATCHED;
    struct ceph_gpu_temperature sample;uint64_t now;
    if(s->backend.temperature(s->backend.owner,&sample) || s->backend.now_ns(s->backend.owner,&now)) {
        s->thermal.failure=CE_THERMAL_INVALID;return CE_THERMAL_INVALID;
    }
    return ceph_gpu_thermal_check(&s->thermal,&sample,now);
}
static int hfi_validate(void *p)
{
    struct ceph_gmu_bringup *s=p;
    /* A latched thermal stop must not prevent the bounded PREPARE_SLUMBER
     * needed to stop hardware. Link separately checks RETIRING group and id33.
     * Only formal retirement or the failure-path stop enters this state; no
     * ordinary requests are admitted. Return the thermal code, not -1, so the
     * caller can tell a thermal refusal from a transport fault. */
    if(s->stage!=CE_BR_RETIRING && !s->stopping) {
        int t=cepheus_gmu_bringup_thermal_check(s);
        if(t)return t;
    }
    return s->backend.transport.validate(s->backend.transport.ctx);
}
static int hfi_doorbell(void *p){struct ceph_gmu_bringup *s=p;return s->backend.transport.doorbell(s->backend.transport.ctx);}
static int hfi_now(void *p,uint64_t *n){struct ceph_gmu_bringup *s=p;return s->backend.transport.now_ns(s->backend.transport.ctx,n);}
static int hfi_pause(void *p){struct ceph_gmu_bringup *s=p;return s->backend.transport.pause(s->backend.transport.ctx);}
static int hfi_fault(void *p){struct ceph_gmu_bringup *s=p;return s->backend.transport.fault(s->backend.transport.ctx);}

static void revoke_upload_aliases(struct ceph_gmu_bringup *s)
{
    for(unsigned i=0;i<3;++i)s->upload_maps[i]=(struct ceph_gmu_mapping){0};
    struct ceph_gmu_upload *u=&s->backend.upload;
    u->maps=NULL;u->map_count=0;u->itcm=u->dtcm=NULL;u->itcm_words=u->dtcm_words=0;
}

/* Compare serialized policy, not pointer identity or struct padding. bind may
 * fill MMIO adapters, but must not change what preflight validated before power
 * side effects. Buffers are bounded scratch reused one at a time (XNU stack). */
static int policy_unchanged(const struct ceph_gmu_bringup_backends *b,const uint32_t perf[59],
    const uint32_t bw[160],const uint32_t acd[17],const struct ceph_pdc_plan *pdc,
    const struct ceph_gpu_bin_policy *policy,unsigned min_minor)
{
    const struct ceph_hfi_boot_config *h=&b->hfi;
    if(!ceph_gpu_bin_policy_valid(h->policy) || h->policy->bin!=policy->bin ||
        b->initial_freq!=1 || b->initial_bw!=3 || !h->gx || h->ng<2 || h->gx[1].hz!=257000000 ||
        h->ng!=policy->gx_levels || h->throttle_mask!=policy->throttle_mask ||
        (!!h->acd)!=policy->acd || h->min_fw_minor!=min_minor)return -1;
    uint32_t words[160];
    if(ceph_hfi_perf(words,160,h->gx,h->ng,h->cx,h->nc,h->gfx,h->cxl,h->mx)!=59)return -1;
    for(unsigned i=0;i<59;++i)if(words[i]!=perf[i])return -1;
    if(ceph_hfi_bandwidth(words,160,h->bw)!=160)return -1;
    for(unsigned i=0;i<160;++i)if(words[i]!=bw[i])return -1;
    if(h->acd) {
        if(ceph_hfi_acd_table(words,160,h->acd)!=17)return -1;
        for(unsigned i=0;i<17;++i)if(words[i]!=acd[i])return -1;
    }
    struct ceph_pdc_plan plan;if(ceph_pdc_plan_a640v2(&b->pdc_input,&plan) || plan.count!=pdc->count)return -1;
    for(unsigned i=0;i<plan.count;++i)
        if(plan.writes[i].window!=pdc->writes[i].window || plan.writes[i].offset!=pdc->writes[i].offset ||
            plan.writes[i].value!=pdc->writes[i].value)return -1;
    return 0;
}

int cepheus_gmu_bringup_prepare(struct ceph_gmu_bringup *s,const uint8_t *firmware,size_t bytes)
{
    if(!s || s->stage!=CE_BR_NEW || s->firmware || s->workspace.state!=CE_WS_NEW)return -1;
    if(cepheus_gpu_firmware_snapshot(0,firmware,bytes,&s->firmware))return -1;
    if(cepheus_gmu_workspace_prepare(&s->workspace)) {
        /* complete() can fail even before publication. Keep the only handle
         * for diagnostics if backing was retained; never report it released. */
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)if(s->workspace.handles[i])++s->unpublished_retained;
        if(!cepheus_gpu_memory_release_cpu(s->firmware))s->firmware=0;
        s->stage=CE_BR_FAILED;s->error=-1;
        /* Never published, so no device ever saw these buffers; the pool has
         * already quarantined any slot whose IOKit completion failed. */
        if(s->unpublished_retained)
            printf("CEPHEUS GMU BRINGUP: prepare failed; %u unpublished buffers retained (never device-visible)\n",
                s->unpublished_retained);
        return -1;
    }
    s->stage=CE_BR_MEMORY;return 0;
}

int cepheus_gmu_bringup_start(struct ceph_gmu_bringup *s)
{
    if(!s || s->stage!=CE_BR_MEMORY || !s->firmware || !s->backend.bind || !s->backend.failed ||
        !s->backend.temperature || !s->backend.now_ns || !s->backend.trial_ns || s->backend.trial_ns>30000000000ULL ||
        !s->backend.transport.validate || !s->backend.transport.doorbell || !s->backend.transport.now_ns ||
        !s->backend.transport.pause || !s->backend.transport.fault)return -1;
    struct ceph_gmu_bringup_backends *b=&s->backend;
    int e;
    /* Reject incomplete immutable policy before any power side effects. bind
     * supplies mappings, not an opportunity to invent OPP/BW/thermal policy. */
    uint32_t perf[59],bw[160],acd[17],dcvs[4];struct ceph_pdc_plan pdc_plan;
    if(!ceph_gpu_bin_policy_valid(b->hfi.policy) || b->hfi.ng!=b->hfi.policy->gx_levels ||
        b->hfi.throttle_mask!=b->hfi.policy->throttle_mask || (!!b->hfi.acd)!=b->hfi.policy->acd ||
        ceph_hfi_perf(perf,59,b->hfi.gx,b->hfi.ng,b->hfi.cx,b->hfi.nc,b->hfi.gfx,b->hfi.cxl,b->hfi.mx)!=59 ||
        ceph_hfi_bandwidth(bw,160,b->hfi.bw)!=160 ||
        (b->hfi.acd && ceph_hfi_acd_table(acd,17,b->hfi.acd)!=17) ||
        ceph_pdc_plan_a640v2(&b->pdc_input,&pdc_plan))return -1;
    if(b->initial_freq!=1 || b->initial_bw!=3 || b->hfi.gx[1].hz!=257000000 ||
        ceph_hfi_dcvs(dcvs,4,b->initial_freq,CE_HFI_DCVS_INVALID)!=4)return -1;
    /* A640 GX bring-up requires a held oob_gpu keepalive; refuse before any
     * power side effect if no real OOB backend was supplied. */
    if(!b->oob.begin || !b->oob.validate || !b->oob.read || !b->oob.write ||
        !b->oob.barrier || !b->oob.now || !b->oob.pause || !b->oob.drained || !b->oob.end)return -1;
    if(b->hfi.policy->acd && (!b->acd.begin || !b->acd.validate || !b->acd.read32 ||
        !b->acd.write32 || !b->acd.barrier || !b->acd.notify || !b->acd.now_ns || !b->acd.pause || !b->acd.end))return -1;
    const struct ceph_gpu_bin_policy policy=*b->hfi.policy;unsigned min_minor=b->hfi.min_fw_minor;
    s->stage=CE_BR_THERMAL;uint64_t now;
    e=b->now_ns(b->owner,&now);if(e)goto failed;
    e=ceph_gpu_thermal_start(&s->thermal,now,b->trial_ns);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    s->stage=CE_BR_POWER;e=ceph_power_start(&s->power,&b->power);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    s->stage=CE_BR_BOUND;e=b->bind(b->owner,s);if(e)goto failed;
    e=policy_unchanged(b,perf,bw,acd,&pdc_plan,&policy,min_minor);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    /* Bind cannot swap/forge the prepared root; exact resource identities are
     * checked again by the publication pool and by each concrete engine. */
    if(!ceph_smmu_plan_canonical(&b->smmu_plan) || (b->smmu_plan.smr&0x7fffu)!=5 ||
        b->smmu_plan.ttbr0!=(s->workspace.table.pa|((uint64_t)b->smmu_plan.asid<<48)) ||
        b->pdc_input.chipid!=0x06040001 || !b->upload.itcm || !b->upload.dtcm ||
        !ceph_gpu_bin_policy_valid(b->hfi.policy)) {e=-1;goto failed;}
    s->upload_maps[0]=(struct ceph_gmu_mapping){0x4000,0x3c000,s->workspace.views[0].cpu,CE_GMU_ICACHE};
    s->upload_maps[1]=(struct ceph_gmu_mapping){0x44000,0x3c000,s->workspace.views[1].cpu,CE_GMU_DCACHE};
    /* Persist is not part of image payload; it is zero-initialized and mapped
     * at0x60000000 for firmware's write-buffer dummy access. */
    s->upload_maps[2]=(struct ceph_gmu_mapping){0x60000000,4096,s->workspace.views[4].cpu,CE_GMU_KERNEL};
    b->upload.maps=s->upload_maps;b->upload.map_count=3;
    /* GENERAL_7 enables TCM retention before PDC scripts and firmware upload,
     * as required by a6xx_gmu_fw_start cold boot. Do not leave it as an implicit
     * undocumented obligation inside CM3 begin(). */
    uint32_t reset;
    if(!b->cm3.validate || !b->cm3.read || !b->cm3.write || !b->cm3.barrier ||
        b->cm3.validate(b->cm3.ctx) || b->cm3.read(b->cm3.ctx,CE_GMU_SYSRESET,&reset) || reset!=1) {e=-1;goto failed;}
    struct ceph_gmu_start retention={0};
    e=ceph_gmu_start_write(&retention,&b->cm3,CE_GMU_TCM_RETENTION,1,1);
    s->retention_writes=retention.effects;if(e)goto failed;
    if(b->hfi.policy->acd) {e=ceph_qmp_send_acd(&s->acd,&b->acd,1);if(e)goto failed;}
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    s->stage=CE_BR_SHARED;e=cepheus_gmu_workspace_share(&s->workspace,&b->smmu_plan);if(e)goto failed;
    s->device_visible=1;
    s->stage=CE_BR_PDC;e=ceph_pdc_install_a640v2(&s->pdc,&b->pdc,&b->pdc_input);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    s->stage=CE_BR_SMMU;e=ceph_smmu_install_context(&s->smmu,&b->smmu,&b->smmu_plan,0x303);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    s->stage=CE_BR_UPLOAD;e=cepheus_gpu_upload_snapshot(&b->upload,s->firmware,s->blocks,32);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    /* Bind pristine HFI table before CM3 starts modifying it. */
    struct ceph_gmu_hfi_transport transport={s,hfi_validate,hfi_doorbell,hfi_now,hfi_pause,hfi_fault};
    e=cepheus_gmu_hfi_bind(&s->link,&s->workspace,&transport);if(e)goto failed;
    s->stage=CE_BR_CM3;e=ceph_gmu_start_cold(&s->cm3,&b->cm3,&s->workspace.layout.start);if(e)goto failed;
    b->hfi.hfi_version=s->cm3.hfi_version;
    s->stage=CE_BR_HFI;e=ceph_hfi_boot_a640(&s->link.session,&b->hfi,&s->fw_version);if(e)goto failed;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    /* KGSL requests the default GPU level after HFI boot with the BW field left
     * at INVALID_DCVS_IDX, so the bootstrap DDR bandwidth lease is still the
     * only thing holding bandwidth. BW_TBL never held bandwidth, and this ACK
     * proves neither physical frequency nor release of apps votes. */
    s->stage=CE_BR_DCVS;uint32_t reply[16];unsigned reply_words;
    e=ceph_hfi_exchange(&s->link.session,dcvs,4,reply,16,&reply_words);if(e)goto failed;
    s->dcvs_acked=1;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    /* Order from _adreno_start(): hold oob_gpu keepalive, then H2F_MSG_START
     * for A640/A680, and only then touch GX registers. Sending id10 without a
     * held keepalive would let the GMU power GX down underneath the caller. */
    s->stage=CE_BR_KEEPALIVE;
    e=ceph_gmu_oob_acquire(&s->keepalive,&b->oob,s->cm3.hfi_version);if(e)goto failed;
    s->stage=CE_BR_START;
    uint32_t start_cmd[1];
    if(ceph_hfi_start(start_cmd,1)!=1) {e=-1;goto failed;}
    e=ceph_hfi_exchange(&s->link.session,start_cmd,1,reply,16,&reply_words);if(e)goto failed;
    s->start_sent=1;
    e=cepheus_gmu_bringup_thermal_check(s);if(e)goto failed;
    /* Image is host-only sealed copy, never DMA-mapped; release after upload.
     * Workspace remains shared and pinned until protocol retirement. */
    e=cepheus_gpu_memory_release_cpu(s->firmware);
    if(e)goto failed;
    s->firmware=0;s->error=0;s->stage=CE_BR_HFI;return 0;
failed:
    s->failed_stage=s->stage;s->stage=CE_BR_FAILED;s->error=e;
    /* A transport failure is reported as CE_HFI_IO even when the underlying
     * cause was a thermal refusal. Keep both so the platform can choose a
     * thermal stop vs a firmware/SMMU dump. */
    s->io_error=e;
    /* A latched thermal refusal surfaces through the HFI transport as a generic
     * IO-class error, and CE_THERMAL_HOT/CE_HFI_IO share the value -3. An
     * overwritten error code therefore cannot distinguish them; keep the raw
     * error and set an explicit flag so the platform can pick a thermal stop
     * instead of dumping firmware/SMMU state. */
    s->thermal_refused=(e && s->thermal.failure)?1u:0u;
    /* CM3 may already be out of reset with firmware DMA live. Attempt the same
     * bounded stop used by retirement BEFORE quarantining the backing. A
     * poisoned/unusable session cannot stop the device; record that instead of
     * pretending the hardware is quiet. */
    /* HFI is only reachable once CM3 runs. Before that there is no bounded stop
     * primitive: CM3 has not been released (CE_BR_CM3 failures happen inside
     * ceph_gmu_start_cold, which quarantines its own state) so nothing is live. */
    if(ceph_bringup_stage_cm3_maybe_live(s->failed_stage) && s->workspace.state==CE_WS_SHARED && s->link.bound && b->stop &&
        b->stop->slumber && b->stop->park && b->stop->detach && b->stop->hfi==&s->link.session &&
        b->stop->plan==&b->smmu_plan) {
        struct ceph_gmu_slumber sl={0};struct ceph_gmu_park pk={0};struct ceph_smmu_detach dt={0};
        s->stop_attempted=1;s->stopping=1;
        int se=ceph_gmu_slumber_request(&sl,b->stop->slumber,&s->link.session,
            b->initial_bw,b->initial_freq,b->hfi.bw->levels,b->hfi.ng);
        if(!se)se=ceph_gmu_park_slumbered(&pk,b->stop->park);
        if(!se)se=ceph_smmu_detach_context(&dt,b->stop->detach,&b->smmu_plan);
        s->stopping=0;s->stop_error=se;
        if(se)printf("CEPHEUS GMU BRINGUP: failure-path stop refused/failed (%d); retaining all backing\n",se);
        /* Only a completed stop justifies dropping the GX keepalive; a refused
         * stop keeps it held so the device cannot power down unexpectedly. */
        if(!se && s->keepalive.state==CE_OOB_HELD && b->oob_release)
            (void)ceph_gmu_oob_release(&s->keepalive,b->oob_release);
    }
    if(s->link.bound)cepheus_gmu_hfi_invalidate(&s->link);
    if(s->workspace.state==CE_WS_SHARED) {
        /* Do not claim RETAINED unless the pool actually moved the group out of
         * SHARED; otherwise the group stays device-visible and must be reported. */
        int ae=cepheus_gpu_memory_abandon_workspace(&s->workspace.publication);
        s->abandon_error=ae;
        if(!ae)s->workspace.state=CE_WS_RETAINED;
        /* Record handles/group, but never leave accessible aliases advertised
         * as a ready workspace after a failed device-visible transition. */
        s->workspace.table=(struct ceph_vm_pool){0};
        s->workspace.layout=(struct ceph_gpu_va_plan){0};s->workspace.leased=0;
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)s->workspace.views[i]=(struct ceph_gpu_memory_view){0};
        if(ae)printf("CEPHEUS GMU BRINGUP: abandon refused (%d); group still device-visible\n",ae);
    }
    /* The uploader has copies of workspace CPU VAs as well as TCM aliases.
     * Quarantine retains backing, not permission for a second upload attempt.
     * Leave accounting/error state intact but revoke all advertised aliases. */
    revoke_upload_aliases(s);
    /* Only claim quarantine of device-visible memory when a device could have
     * touched it. A pre-power refusal must not look like a DMA-era failure. */
    b->upload.quarantined=b->upload.writes||s->device_visible;
    b->failed(b->owner,s->failed_stage,e);
    return e;
}

int cepheus_gmu_bringup_retire(struct ceph_gmu_bringup *s,struct ceph_gpu_retire_ops *b)
{
    if(!s || s->stage!=CE_BR_HFI || !s->dcvs_acked || s->firmware || s->workspace.state!=CE_WS_SHARED ||
        !s->link.bound || !b || b->hfi!=&s->link.session || b->plan!=&s->backend.smmu_plan ||
        !s->backend.hfi.bw || b->bw_levels!=s->backend.hfi.bw->levels || b->gx_levels!=s->backend.hfi.ng ||
        /* Firmware only ever entered the fixed startup pair; preparing slumber
         * for any other levels would be a state the GMU never reached. */
        b->bw!=s->backend.initial_bw || b->freq!=s->backend.initial_freq ||
        /* A chain that holds the GX keepalive must be able to drop it after the
         * stop; otherwise retirement would leave GX pinned awake. */
        (s->keepalive.state==CE_OOB_HELD && !s->backend.oob_release))return -1;
    s->stage=CE_BR_RETIRING;
    int e=cepheus_gmu_workspace_retire(&s->workspace,b);
    /* Invalid retirement input refused before admission leaves the session
     * untouched. An attempted protocol failure instead quarantines the group. */
    if(s->workspace.state==CE_WS_SHARED){s->stage=CE_BR_HFI;return e?e:-1;}
    /* slumber->park->detach finished: only now may the GX keepalive be dropped.
     * A release failure leaves it held and is reported, never silently ignored. */
    if(!e && s->keepalive.state==CE_OOB_HELD && s->backend.oob_release) {
        int oe=ceph_gmu_oob_release(&s->keepalive,s->backend.oob_release);
        if(oe){s->error=oe;e=oe;}
    }
    cepheus_gmu_hfi_invalidate(&s->link);revoke_upload_aliases(s);
    s->error=e;
    if(e) {
        s->failed_stage=CE_BR_HFI;s->stage=CE_BR_FAILED;s->backend.upload.quarantined=1;
        s->backend.failed(s->backend.owner,s->failed_stage,e);
    } else s->stage=CE_BR_RETIRED;
    return e;
}
