#!/usr/bin/env bash
# Copy the cepheus XNU patches into ios-sweet's patch directory.
#
# ios-sweet applies xnu/patches/*.patch in filename order with `git apply`.
# Stage the cepheus source before the patch loop; 0082 changes only the
# compiled file in bsd/conf/files after upstream reference-driver patches.
# Do not replace the reference source in place: later 0062/0063 patches target
# it, and a malformed/stale patch can be silently skipped by build.sh.
#
# Usage: build-kernel-patches.sh /path/to/ios-sweet
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:?usage: build-kernel-patches.sh /path/to/ios-sweet}"
[ -d "$DEST/xnu/patches" ] || { echo "ERROR: $DEST/xnu/patches not found" >&2; exit 1; }

# Official git endpoint intermittently returns 502. Use the exact same Git
# object from a verified public mirror, keeping both commit and tree checks.
python3 "$ROOT/scripts/test-bearssl-source.py"
python3 "$ROOT/scripts/prepare-bearssl-source.py" "$DEST/xnu/patches"

install -m 644 "$ROOT/xnu/0082-cepheus-touch.patch" "$DEST/xnu/patches/0082-cepheus-touch.patch"
install -m 644 "$ROOT/xnu/0082-stage-cepheus-touch.sh" "$DEST/xnu/patches/0082-stage-cepheus-touch.sh"
install -m 644 "$ROOT/xnu/cepheus_touch.c" "$ROOT/xnu/cepheus_fts_events.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_clock_status.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_gpi_layout.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_i2c_bitbang.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/0085-stage-cepheus-soc.sh" "$ROOT/xnu/0085-cepheus-soc.patch" "$ROOT/xnu/cepheus_soc.c" "$ROOT/xnu/cepheus_soc.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_cpu_policy.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_gpu_power.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/0086-cepheus-gpu-arc.patch" "$ROOT/xnu/0086-stage-cepheus-gpu-arc.sh" "$ROOT/xnu/cepheus_arc.h" "$ROOT/xnu/cepheus_arc_probe.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/cepheus_bcm.h" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/0087-cepheus-gpu-memory.patch" "$ROOT/xnu/0087-stage-cepheus-gpu-memory.sh" "$DEST/xnu/patches/"
install -m 644 "$ROOT/xnu/0088-cepheus-pmap-cache-pa.patch" "$DEST/xnu/patches/"
for file in cepheus_gpu_memory.cpp cepheus_gpu_memory.h cepheus_gpu_compile.c cepheus_gpu_compile.h \
    cepheus_gpu_memory_state.h \
    cepheus_gmu_upload_mmio.h \
    cepheus_qmp_connect.h \
    cepheus_gpu_tsens.h \
    cepheus_cp_ring_config.h \
    cepheus_cp_protection.h \
    cepheus_cp_snapshot_mmio.h \
    cepheus_cp_workspace.c cepheus_cp_workspace.h \
    cepheus_gpu_vm39.h \
    cepheus_gx_core.h \
    cepheus_gx_perfcounter.h cepheus_gx_gbif.h cepheus_gx_llc.h cepheus_scm_discovery.h cepheus_gx_va_layout.h cepheus_gx_hwcg.h cepheus_cp_unsecure_gate.h cepheus_cp_preinit.h \
    cepheus_gx_smmu_plan.h \
    cepheus_gmu_workspace.c cepheus_gmu_workspace.h \
    cepheus_gpu_retire.c cepheus_gpu_retire.h cepheus_gpu_memory_private.h \
    cepheus_gmu_hfi_link.c cepheus_gmu_hfi_link.h \
    cepheus_gmu_mmio.h cepheus_gpu_config.h cepheus_qmp_mmio.h cepheus_gmu_retire_mmio.h cepheus_pdc_mmio.h cepheus_gpu_fault_mmio.h \
    cepheus_gmu_bringup.c cepheus_gmu_bringup.h \
    cepheus_hfi.h cepheus_hfi_boot.h cepheus_hfi_messages.h cepheus_hfi_session.h \
    cepheus_gmu_firmware.h cepheus_gmu_upload.h cepheus_smmu_plan.h cepheus_smmu_install.h \
    cepheus_gpu_power_txn.h cepheus_gpu_submit.h cepheus_gmu_start.h \
    cepheus_gpu_ring.h cepheus_a640_pm4.h cepheus_sqe.h cepheus_gpu_bw.h cepheus_gpu_firmware.h cepheus_gpu_fault.h cepheus_gpu_vm.h cepheus_gpu_va.h cepheus_gpu_pdc.h cepheus_gpu_bin.h cepheus_gpu_thermal.h cepheus_cp_init.h cepheus_smmu_detach.h cepheus_gmu_park.h cepheus_gmu_slumber.h cepheus_cp_protect.h cepheus_qmp_acd.h cepheus_smmu_geometry.h cepheus_smmu_mmio.h cepheus_gmu_oob.h; do
    install -m 644 "$ROOT/xnu/$file" "$DEST/xnu/patches/"
done
echo "installed 0082-cepheus-touch.patch into $DEST/xnu/patches"
ls -l "$DEST/xnu/patches/" | tail -3
