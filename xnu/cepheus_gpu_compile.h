#ifndef CEPHEUS_GPU_COMPILE_H
#define CEPHEUS_GPU_COMPILE_H
#include "cepheus_hfi_boot.h"
#include "cepheus_gmu_upload.h"
#include "cepheus_smmu_install.h"
#include "cepheus_gpu_power_txn.h"
#include "cepheus_gpu_submit.h"
#include "cepheus_gmu_start.h"
#include "cepheus_gpu_ring.h"
#include "cepheus_sqe.h"
#include "cepheus_gpu_bw.h"
#include "cepheus_gpu_config.h"
#include "cepheus_gpu_fault.h"
#include "cepheus_gpu_memory.h"
#include "cepheus_gpu_vm.h"
#include "cepheus_gpu_va.h"
#include "cepheus_gpu_pdc.h"
#include "cepheus_gpu_thermal.h"
#include "cepheus_cp_init.h"
#include "cepheus_gx_perfcounter.h"
#include "cepheus_gx_gbif.h"
#include "cepheus_gx_llc.h"
#include "cepheus_gx_va_layout.h"
#include "cepheus_gx_hwcg.h"
#include "cepheus_cp_unsecure_gate.h"
#include "cepheus_scm_discovery.h"
#include "cepheus_smmu_detach.h"
#include "cepheus_gmu_park.h"
#include "cepheus_gmu_slumber.h"
#include "cepheus_qmp_acd.h"
#include "cepheus_qmp_mmio.h"
#include "cepheus_smmu_geometry.h"
#include "cepheus_smmu_mmio.h"
#include "cepheus_gmu_oob.h"
#include "cepheus_gmu_mmio.h"
#include "cepheus_gmu_retire_mmio.h"
#include "cepheus_pdc_mmio.h"
#include "cepheus_gpu_fault_mmio.h"
#include "cepheus_gmu_upload_mmio.h"
#include "cepheus_qmp_connect.h"
#include "cepheus_gpu_tsens.h"
#include "cepheus_cp_ring_config.h"
#include "cepheus_cp_protection.h"
#include "cepheus_cp_snapshot_mmio.h"
#include "cepheus_gpu_vm39.h"
#include "cepheus_gx_core.h"
#include "cepheus_gx_smmu_plan.h"
int cepheus_gpu_config_build(const struct ceph_arc_db *,uint32_t,uint32_t,struct ceph_gpu_config *);
int cepheus_gpu_config_probe(const struct ceph_arc_db *,uint32_t,uint32_t);
int cepheus_gpu_qmp_bind(struct ceph_qmp_mmio *,unsigned,struct ceph_qmp_ops *);
int cepheus_gpu_pdc_mmio_bind(struct ceph_pdc_mmio *,struct ceph_pdc_ops *);
int cepheus_gpu_fault_mmio_read(struct ceph_gpu_fault_mmio *,struct ceph_gpu_fault_record *);
int cepheus_gpu_upload_mmio_bind(struct ceph_gmu_upload_mmio *,struct ceph_gmu_upload *);
int cepheus_gpu_qmp_connect(struct ceph_qmp_connect *,struct ceph_qmp_mmio *);
int cepheus_gpu_temperature_sample(struct ceph_gpu_tsens *,struct ceph_gpu_temperature *);
int cepheus_gpu_cp_ring_configure(struct ceph_cp_ring_config *);
int cepheus_gpu_cp_protect_configure(struct ceph_cp_protection *);
int cepheus_gpu_cp_snapshot_mmio(struct ceph_cp_snapshot_mmio *,uint32_t *,unsigned);
int cepheus_gpu_vm39_build(struct ceph_vm_pool *,const struct ceph_vm_region *,unsigned);
int cepheus_gpu_gx_core_configure(struct ceph_gx_core *);
int cepheus_gpu_gx_llc_configure(struct ceph_gx_llc *);
int cepheus_gpu_scm_discover(struct ceph_scm_discovery *,struct ceph_scm_discovery_ops *);
int cepheus_gpu_gx_smmu_plan(const struct ceph_smmu_stream *,unsigned,const struct ceph_smmu_geometry *,unsigned,unsigned,unsigned,uint64_t,uint32_t,struct ceph_gx_smmu_plan *);
int cepheus_gpu_park_read(void *,enum ceph_gmu_park_reg,uint32_t *);
int cepheus_gpu_park_write(void *,enum ceph_gmu_park_reg,uint32_t);
int cepheus_gpu_slumber_read(void *,enum ceph_slumber_reg,uint32_t *);
int cepheus_gpu_slumber_write(void *,enum ceph_slumber_reg,uint32_t);
int cepheus_gpu_hfi_boot(struct ceph_hfi_session *,const struct ceph_hfi_boot_config *,uint32_t *);
int cepheus_gpu_upload(struct ceph_gmu_upload *,const uint8_t *,size_t,struct ceph_gmu_block *,unsigned);
int cepheus_gpu_smmu_install(struct ceph_smmu_install *,struct ceph_smmu_ops *,const struct ceph_smmu_plan *,uint32_t);
int cepheus_gpu_power_start(struct ceph_power_txn *,struct ceph_power_ops *);
int cepheus_gpu_submit(struct ceph_submit *,struct ceph_submit_ops *,uint32_t);
int cepheus_gpu_gmu_start(struct ceph_gmu_start *,struct ceph_gmu_start_ops *,const struct ceph_gmu_start_config *);
int cepheus_gpu_ring_submit(struct ceph_gpu_ring *,uint64_t,uint64_t,uint64_t,uint32_t,uint64_t,uint32_t);
int cepheus_gpu_sqe_copy(const uint8_t *,size_t,uint8_t *,size_t,struct ceph_sqe_image *);
int cepheus_gpu_bandwidth_table(const struct ceph_bw_route *,const struct ceph_bw_level *,const struct ceph_bw_route *,const struct ceph_bw_level *,unsigned,struct ceph_hfi_bw *);
int cepheus_gpu_fault_read(struct ceph_gpu_fault_ops *,struct ceph_gpu_fault_record *);
/* Preferred sealed-snapshot entrypoints: hold immutable view through use. */
int cepheus_gpu_upload_snapshot(struct ceph_gmu_upload *,uint64_t,struct ceph_gmu_block *,unsigned);
int cepheus_gpu_sqe_snapshot_copy(uint64_t,uint8_t *,size_t,struct ceph_sqe_image *);
int cepheus_gpu_vm_build(struct ceph_vm_pool *,const struct ceph_vm_region *,unsigned);
int cepheus_gpu_vm_lookup(struct ceph_vm_pool *,uint64_t,uint64_t *,uint64_t *);
int cepheus_gpu_va_build(const struct ceph_gpu_backing *,struct ceph_gpu_va_plan *);
int cepheus_gpu_pdc_install(struct ceph_pdc_install *,struct ceph_pdc_ops *,const struct ceph_pdc_input *);
int cepheus_gpu_thermal_guard(struct ceph_gpu_thermal *,const struct ceph_gpu_temperature *,uint64_t);
int cepheus_gpu_cp_init_packet(uint32_t *,unsigned,uint64_t,size_t);
int cepheus_gpu_cp_reglist(struct ceph_cp_snapshot_ops *,uint32_t *,unsigned);
int cepheus_gpu_smmu_detach(struct ceph_smmu_detach *,struct ceph_smmu_detach_ops *,const struct ceph_smmu_plan *);
int cepheus_gpu_gmu_park(struct ceph_gmu_park *,struct ceph_gmu_park_ops *);
int cepheus_gpu_gmu_slumber(struct ceph_gmu_slumber *,struct ceph_gmu_slumber_ops *,struct ceph_hfi_session *,unsigned,unsigned,unsigned,unsigned);
int cepheus_gpu_acd_message(struct ceph_qmp_tx *,struct ceph_qmp_ops *,unsigned);
int cepheus_gpu_smmu_geometry(uint32_t,uint32_t,uint32_t,uint32_t,struct ceph_smmu_geometry *);
int cepheus_gpu_smmu_mmio_read(void *,enum ceph_smmu_reg,uint64_t *);
int cepheus_gpu_smmu_mmio_write(void *,enum ceph_smmu_reg,uint64_t);
int cepheus_gpu_oob_acquire(struct ceph_gmu_oob *,struct ceph_oob_ops *,uint32_t);
int cepheus_gpu_oob_release(struct ceph_gmu_oob *,struct ceph_oob_ops *);
int cepheus_gpu_gmu_read(void *,enum ceph_gmu_reg,uint32_t *);
int cepheus_gpu_gmu_write(void *,enum ceph_gmu_reg,uint32_t);
int cepheus_gpu_hfi_notify(void *);
int cepheus_gpu_gmu_fault(void *);
#endif
