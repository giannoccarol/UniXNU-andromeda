/* Integrate concrete GPU engines; no platform discovery/ownership fallback.
 * No startup hook invokes this until actual ARC/clock/TBU/SMMU/PDC ownership
 * adapters have been verified. Device writes only occur inside supplied engines.
 * The bundle is address-stable/single-owner throughout this one-shot attempt.
 */
#ifndef CEPHEUS_GMU_BRINGUP_H
#define CEPHEUS_GMU_BRINGUP_H
#include "cepheus_gpu_power_txn.h"
#include "cepheus_gpu_pdc.h"
#include "cepheus_smmu_install.h"
#include "cepheus_gmu_upload.h"
#include "cepheus_gmu_hfi_link.h"
#include "cepheus_hfi_boot.h"
#include "cepheus_qmp_acd.h"
#include "cepheus_gpu_thermal.h"
#include "cepheus_gmu_oob.h"
enum ceph_bringup_stage {CE_BR_NEW,CE_BR_MEMORY,CE_BR_POWER,CE_BR_BOUND,CE_BR_SHARED,
    CE_BR_PDC,CE_BR_SMMU,CE_BR_UPLOAD,CE_BR_CM3,CE_BR_HFI,CE_BR_FAILED,CE_BR_RETIRED,
    CE_BR_THERMAL,CE_BR_RETIRING,CE_BR_DCVS,CE_BR_KEEPALIVE,CE_BR_START};
/* Enum order is not boot order (THERMAL/DCVS are appended), so never compare
 * stages numerically. These are the stages where CM3 may already be running:
 * the firmware could be executing and DMA-visible memory live. */
static inline int ceph_bringup_stage_cm3_maybe_live(enum ceph_bringup_stage s)
{return s==CE_BR_CM3 || s==CE_BR_HFI || s==CE_BR_DCVS || s==CE_BR_KEEPALIVE ||
    s==CE_BR_START || s==CE_BR_RETIRING;}
struct ceph_gmu_bringup;
struct ceph_gmu_bringup_backends {
    void *owner;
    /* Resolve exact mapped windows/reservations/plan/config from successful
     * power+CPU workspace. Must make no hardware writes and invent no metadata. */
    int (*bind)(void *,struct ceph_gmu_bringup *);
    /* Required actual gpuss-0/1 thermal-zone samples and monotonic time. Every hardware
     * lease validator must also call bringup_thermal_check during long stages.
     * Platform still supplies independent deadline/stop supervision at idle. */
    int (*temperature)(void *,struct ceph_gpu_temperature *);
    int (*now_ns)(void *,uint64_t *);
    uint64_t trial_ns; /*1..30seconds; not extendable after thermal_start */
    struct ceph_power_ops power;
    struct ceph_pdc_ops pdc;
    struct ceph_smmu_ops smmu;
    struct ceph_gmu_start_ops cm3;
    struct ceph_qmp_ops acd; /* bin1 must notify AOP before GMU startup */
    /* A640 needs the oob_gpu keepalive held across GX bring-up, exactly as
     * _adreno_start() does before hfi_start_msg(). Without a real OOB backend
     * the chain must refuse rather than send id10 into an unkept GX. */
    struct ceph_oob_ops oob;
    struct ceph_gmu_hfi_transport transport;
    struct ceph_hfi_boot_config hfi;
    unsigned initial_freq,initial_bw; /* fixed lowest Cepheus startup pair1/3 */
    struct ceph_pdc_input pdc_input;
    struct ceph_smmu_plan smmu_plan;
    struct ceph_gmu_upload upload;
    /* Optional bounded stop used ONLY on the startup-failure path, after CM3
     * has been released from reset. Without it a post-CM3 failure must retain
     * and report, never assume the device is quiescent. Same session/plan
     * identity rules as retirement apply. */
    struct ceph_gpu_retire_ops *stop;
    /* Released only after the bounded stop drained the device, mirroring
     * _adreno_stop()'s oob_clear(oob_gpu) ordering. NULL refuses retirement
     * of a chain that actually acquired the keepalive. */
    struct ceph_oob_ops *oob_release;
    void (*failed)(void *,enum ceph_bringup_stage,int); /* retain/dump, NO assumed safe rollback */
};
struct ceph_gmu_bringup {
    enum ceph_bringup_stage stage,failed_stage;
    int error;
    int io_error,stop_error,abandon_error; /* raw transport vs thermal vs stop vs pool */
    unsigned stop_attempted,device_visible,unpublished_retained,stopping,thermal_refused;
    unsigned retention_writes;
    uint64_t firmware;
    struct ceph_gmu_workspace workspace;
    struct ceph_power_txn power;
    struct ceph_pdc_install pdc;
    struct ceph_smmu_install smmu;
    struct ceph_gmu_start cm3;
    struct ceph_qmp_tx acd;
    struct ceph_gpu_thermal thermal;
    struct ceph_gmu_hfi_link link;
    struct ceph_gmu_mapping upload_maps[3];
    struct ceph_gmu_block blocks[32];
    struct ceph_gmu_bringup_backends backend;
    uint32_t fw_version;
    unsigned dcvs_acked;
    struct ceph_gmu_oob keepalive;
    unsigned start_sent;
};
#ifdef __cplusplus
extern "C" {
#endif
int cepheus_gmu_bringup_prepare(struct ceph_gmu_bringup *,const uint8_t *firmware,size_t bytes);
int cepheus_gmu_bringup_start(struct ceph_gmu_bringup *);
int cepheus_gmu_bringup_thermal_check(struct ceph_gmu_bringup *);
/* Exact same HFI session/plan; successful return means workspace detached and
 * released, NOT power votes dropped, PDC reset or platform leases surrendered. */
int cepheus_gmu_bringup_retire(struct ceph_gmu_bringup *,struct ceph_gpu_retire_ops *);
#ifdef __cplusplus
}
#endif
#endif
