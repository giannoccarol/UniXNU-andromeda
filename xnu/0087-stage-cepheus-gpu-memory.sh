#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -f bsd/conf/files
mkdir -p iokit/Kernel/arm
install -m 0644 "$here/cepheus_gpu_memory.cpp" "$here/cepheus_gpu_memory.h" "$here/cepheus_gpu_memory_state.h" "$here/cepheus_gpu_firmware.h" iokit/Kernel/arm/
install -m 0644 "$here/cepheus_gpu_retire.h" "$here/cepheus_gpu_memory_private.h" iokit/Kernel/arm/
install -m 0644 "$here/cepheus_gpu_memory.h" "$here/cepheus_gpu_compile.c" "$here/cepheus_gpu_compile.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_workspace.c" "$here/cepheus_gmu_workspace.h" bsd/dev/
install -m 0644 "$here/cepheus_gpu_retire.c" "$here/cepheus_gpu_retire.h" "$here/cepheus_gpu_memory_private.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_hfi_link.c" "$here/cepheus_gmu_hfi_link.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_mmio.h" "$here/cepheus_gpu_config.h" bsd/dev/
install -m 0644 "$here/cepheus_qmp_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_retire_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_pdc_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_gpu_fault_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_upload_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_qmp_connect.h" bsd/dev/
install -m 0644 "$here/cepheus_gpu_tsens.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_ring_config.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_protection.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_snapshot_mmio.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_workspace.c" "$here/cepheus_cp_workspace.h" bsd/dev/
install -m 0644 "$here/cepheus_gpu_vm39.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_core.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_perfcounter.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_gbif.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_llc.h" bsd/dev/
install -m 0644 "$here/cepheus_scm_discovery.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_va_layout.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_hwcg.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_unsecure_gate.h" bsd/dev/
install -m 0644 "$here/cepheus_cp_preinit.h" bsd/dev/
install -m 0644 "$here/cepheus_gx_smmu_plan.h" bsd/dev/
install -m 0644 "$here/cepheus_gmu_bringup.c" "$here/cepheus_gmu_bringup.h" bsd/dev/
for header in cepheus_hfi.h cepheus_hfi_boot.h cepheus_hfi_messages.h cepheus_hfi_session.h \
    cepheus_arc.h cepheus_gmu_firmware.h cepheus_gmu_upload.h cepheus_smmu_plan.h \
    cepheus_smmu_install.h cepheus_gpu_power.h cepheus_gpu_power_txn.h cepheus_gpu_submit.h cepheus_gmu_start.h \
    cepheus_gpu_ring.h cepheus_a640_pm4.h cepheus_sqe.h cepheus_gpu_bw.h cepheus_bcm.h cepheus_gpu_firmware.h cepheus_gpu_fault.h cepheus_gpu_vm.h cepheus_gpu_va.h cepheus_gpu_pdc.h cepheus_gpu_bin.h cepheus_gpu_thermal.h cepheus_cp_init.h cepheus_smmu_detach.h cepheus_gmu_park.h cepheus_gmu_slumber.h cepheus_cp_protect.h cepheus_qmp_acd.h cepheus_smmu_geometry.h cepheus_smmu_mmio.h cepheus_gmu_oob.h; do
    install -m 0644 "$here/$header" bsd/dev/
done
