/* GMU staged loader core. Hardware resource acquisition deliberately belongs
 * to an explicit platform backend. There is NO default/success stub backend.
 * The supplied image must already be digest-checked against the chosen local
 * artifact. No caller-provided 'ready=true' flag substitutes live validation.
 */
#ifndef CEPHEUS_GMU_LOADER_H
#define CEPHEUS_GMU_LOADER_H
#include "cepheus_gmu_firmware.h"
enum ceph_gmu_stage { CE_GMU_NEW, CE_GMU_VALIDATED, CE_GMU_POWER, CE_GMU_MAPPED,
    CE_GMU_RESET, CE_GMU_LOADED, CE_GMU_STARTED, CE_GMU_HFI, CE_GMU_FAILED };
struct ceph_gmu_backend {
	void *ctx;
	int (*power_acquire)(void *); /* validated CX rail/clock ownership */
	int (*maps_acquire)(void *, const struct ceph_gmu_block *, unsigned);
	int (*reset_assert)(void *); /* confirm CM3 reset before any upload */
	int (*prepare_pdc_rsc)(void *); /* platform/cmd-db validated sequences */
	int (*write_block)(void *, uint32_t, enum ceph_gmu_region, const uint8_t *, uint32_t);
	int (*sync_upload)(void *); /* clean memory + MMIO write barriers */
	int (*prepare_boot)(void *); /* HFI/log buffers, chipid, fence, power config */
	int (*release_reset)(void *);
	int (*wait_init)(void *, uint32_t, unsigned); /* expected BABEFACE, 100ms */
	int (*start_hfi)(void *, unsigned); /* initiate and wait, 100ms */
	int (*cleanup)(void *, enum ceph_gmu_stage); /* stop/reset BEFORE free/unmap */
};
struct ceph_gmu_loader { enum ceph_gmu_stage stage; int error; int quarantined; };
static int ceph_gmu_boot(struct ceph_gmu_loader *l, struct ceph_gmu_backend *b,
    const uint8_t *file, size_t size, struct ceph_gmu_block *plan, unsigned capacity)
{
	if (!l || l->stage != CE_GMU_NEW || l->quarantined || !b || !b->power_acquire ||
	    !b->maps_acquire || !b->reset_assert || !b->prepare_pdc_rsc || !b->write_block ||
	    !b->sync_upload || !b->prepare_boot || !b->release_reset || !b->wait_init ||
	    !b->start_hfi || !b->cleanup) { return -1; }
	unsigned count = 0;
	int error = ceph_gmu_plan(file,size,plan,capacity,&count);
	if (error) { l->error=error; return error; } // preflight: zero hardware effects
	int has_itcm=0;
	for(unsigned i=0;i<count;++i)
		if(plan[i].size && plan[i].region==CE_GMU_ITCM) has_itcm=1;
	if(!has_itcm) { l->error=-1;return -1; } // metadata alone cannot boot CM3
	l->stage=CE_GMU_VALIDATED;
#define CE_STAGE(stage_name, call) do { l->stage=(stage_name); error=(call); if (error) goto failed; } while (0)
	CE_STAGE(CE_GMU_POWER,b->power_acquire(b->ctx));
	CE_STAGE(CE_GMU_MAPPED,b->maps_acquire(b->ctx,plan,count));
	CE_STAGE(CE_GMU_RESET,b->reset_assert(b->ctx));
	error=b->prepare_pdc_rsc(b->ctx); if (error) goto failed;
	for (unsigned i=0;i<count;++i) {
		if (!plan[i].size) { continue; } // metadata interpreted by maps_acquire
		error=b->write_block(b->ctx,plan[i].address,plan[i].region,file+plan[i].file_offset,plan[i].size);
		if (error) goto failed;
	}
	CE_STAGE(CE_GMU_LOADED,b->sync_upload(b->ctx));
	error=b->prepare_boot(b->ctx); if (error) goto failed;
	CE_STAGE(CE_GMU_STARTED,b->release_reset(b->ctx));
	error=b->wait_init(b->ctx,0xbabefaceu,100); if (error) goto failed;
	CE_STAGE(CE_GMU_HFI,b->start_hfi(b->ctx,100));
#undef CE_STAGE
	l->error=0;
	return 0;
failed:
	l->error=error;
	/* Backend must quarantine all potentially GPU-owned buffers on uncertain
	 * reset/stop; never release them merely because a handshake timed out. */
	l->quarantined=b->cleanup(b->ctx,l->stage)!=0;
	l->stage=CE_GMU_FAILED;
	return error;
}
#endif
