#!/usr/bin/env python3
"""Check actual staged GPU source and linked symbols; no claim of execution."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ('cepheus_hfi.h', 'cepheus_hfi_boot.h', 'cepheus_hfi_messages.h', 'cepheus_hfi_session.h',
           'cepheus_arc.h', 'cepheus_gmu_firmware.h', 'cepheus_gmu_upload.h', 'cepheus_smmu_plan.h',
           'cepheus_smmu_install.h', 'cepheus_gpu_power.h', 'cepheus_gpu_power_txn.h', 'cepheus_gpu_submit.h',
           'cepheus_gpu_compile.c', 'cepheus_gpu_compile.h', 'cepheus_gpu_memory.h')
HEADERS += ('cepheus_gmu_start.h','cepheus_bcm.h')
HEADERS += ('cepheus_gpu_ring.h','cepheus_a640_pm4.h','cepheus_sqe.h')
HEADERS += ('cepheus_gpu_bw.h','cepheus_gpu_firmware.h','cepheus_gpu_fault.h')
HEADERS += ('cepheus_gpu_config.h',)
HEADERS += ('cepheus_qmp_mmio.h',)
HEADERS += ('cepheus_gmu_retire_mmio.h',)
HEADERS += ('cepheus_pdc_mmio.h',)
HEADERS += ('cepheus_gpu_fault_mmio.h',)
HEADERS += ('cepheus_gmu_upload_mmio.h',)
HEADERS += ('cepheus_qmp_connect.h',)
HEADERS += ('cepheus_gpu_tsens.h',)
HEADERS += ('cepheus_cp_ring_config.h',)
HEADERS += ('cepheus_cp_protection.h',)
HEADERS += ('cepheus_cp_snapshot_mmio.h',)
HEADERS += ('cepheus_cp_workspace.c','cepheus_cp_workspace.h')
HEADERS += ('cepheus_gpu_vm39.h',)
HEADERS += ('cepheus_gx_core.h',)
HEADERS += ('cepheus_gx_perfcounter.h',)
HEADERS += ('cepheus_gx_llc.h',)
HEADERS += ('cepheus_scm_discovery.h',)
HEADERS += ('cepheus_cp_preinit.h',)
HEADERS += ('cepheus_gx_va_layout.h','cepheus_gx_gbif.h','cepheus_gx_hwcg.h','cepheus_cp_unsecure_gate.h')
HEADERS += ('cepheus_gx_smmu_plan.h',)
HEADERS += ('cepheus_gpu_vm.h',)
HEADERS += ('cepheus_gpu_va.h',)
HEADERS += ('cepheus_gpu_pdc.h',)
HEADERS += ('cepheus_gpu_bin.h',)
HEADERS += ('cepheus_gpu_thermal.h',)
HEADERS += ('cepheus_cp_init.h',)
HEADERS += ('cepheus_smmu_detach.h',)
HEADERS += ('cepheus_gmu_park.h',)
HEADERS += ('cepheus_gmu_slumber.h',)
HEADERS += ('cepheus_cp_protect.h',)
HEADERS += ('cepheus_qmp_acd.h',)
HEADERS += ('cepheus_smmu_geometry.h',)
HEADERS += ('cepheus_smmu_mmio.h',)
HEADERS += ('cepheus_gmu_oob.h',)
HEADERS += ('cepheus_gpu_retire.c','cepheus_gpu_retire.h','cepheus_gpu_memory_private.h')
HEADERS += ('cepheus_gmu_hfi_link.c','cepheus_gmu_hfi_link.h')
HEADERS += ('cepheus_gmu_mmio.h',)
HEADERS += ('cepheus_gmu_bringup.c','cepheus_gmu_bringup.h')
HEADERS += ('cepheus_gmu_workspace.c','cepheus_gmu_workspace.h')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xnu', type=Path)
    parser.add_argument('kernel', type=Path)
    args = parser.parse_args()
    for name in HEADERS:
        staged = args.xnu / 'bsd/dev' / name
        if not staged.exists():
            raise ValueError(f'header declared in HEADERS but never staged: {name}')
        if staged.read_bytes() != (ROOT / 'xnu' / name).read_bytes():
            raise ValueError(f'staged BSD source mismatch: {name}')
    for name in ('cepheus_gpu_memory.cpp', 'cepheus_gpu_memory.h', 'cepheus_gpu_memory_state.h','cepheus_gpu_firmware.h',
                 'cepheus_gpu_retire.h','cepheus_gpu_memory_private.h'):
        if (args.xnu / 'iokit/Kernel/arm' / name).read_bytes() != (ROOT / 'xnu' / name).read_bytes():
            raise ValueError(f'staged IOKit source mismatch: {name}')
    data = args.kernel.read_bytes()
    if data[:8] != bytes.fromhex('cffaedfe0c000001') or b'CEPHEUS GPU MEM:' not in data:
        raise ValueError('not the expected arm64 GPU kernel')
    if b'SWEET FASTBOOT:' in data or b'sweetfastboot' in data:
        raise ValueError('cancelled USB control code unexpectedly present')
    nm = subprocess.check_output(['xcrun', '--find', 'nm'], text=True).strip() if sys.platform == 'darwin' else shutil.which('llvm-nm')
    if not nm:
        raise ValueError('Mach-O nm is required')
    output = subprocess.check_output([nm, str(args.kernel)], text=True)
    symbols = {line.split()[-1] for line in output.splitlines() if len(line.split()) == 3 and line.split()[-2].upper() != 'U'}
    required = {'_cepheus_gpu_hfi_boot', '_cepheus_gpu_upload', '_cepheus_gpu_smmu_install',
                '_cepheus_gpu_power_start', '_cepheus_gpu_submit', '_cepheus_gpu_memory_selftest', '_cepheus_gpu_gmu_start',
                '_cepheus_gpu_ring_submit', '_cepheus_gpu_sqe_copy','_cepheus_gpu_bandwidth_table','_cepheus_gpu_fault_read',
                '_cepheus_gpu_upload_snapshot','_cepheus_gpu_sqe_snapshot_copy',
                '_cepheus_gpu_vm_build','_cepheus_gpu_vm_lookup','_cepheus_gpu_va_build',
                '_cepheus_gmu_workspace_prepare','_cepheus_gmu_workspace_release_cpu','_cepheus_gmu_workspace_selftest',
                '_cepheus_gpu_pdc_install','_cepheus_gpu_thermal_guard','_cepheus_gpu_cp_init_packet','_cepheus_gpu_cp_reglist',
                '_cepheus_gpu_smmu_detach','_cepheus_gpu_gmu_park','_cepheus_gpu_gmu_slumber','_cepheus_gpu_acd_message',
                '_cepheus_gpu_smmu_geometry','_cepheus_gpu_smmu_mmio_read','_cepheus_gpu_smmu_mmio_write',
                '_cepheus_gpu_oob_acquire','_cepheus_gpu_oob_release',
                '_cepheus_gmu_hfi_bind','_cepheus_gmu_hfi_invalidate',
                '_cepheus_gpu_gmu_read','_cepheus_gpu_gmu_write','_cepheus_gpu_hfi_notify','_cepheus_gpu_gmu_fault',
                '_cepheus_gmu_bringup_prepare','_cepheus_gmu_bringup_start'}
    required |= {'_cepheus_gpu_config_build'}
    required |= {'_cepheus_gpu_qmp_bind'}
    required |= {'_cepheus_gpu_pdc_mmio_bind'}
    required |= {'_cepheus_gpu_fault_mmio_read'}
    required |= {'_cepheus_gpu_upload_mmio_bind'}
    required |= {'_cepheus_gmu_bringup_retire'}
    required |= {'_cepheus_gmu_bringup_thermal_check'}
    required |= {'_cepheus_gpu_qmp_connect'}
    required |= {'_cepheus_gpu_temperature_sample'}
    required |= {'_cepheus_gpu_cp_ring_configure'}
    required |= {'_cepheus_gpu_cp_protect_configure'}
    required |= {'_cepheus_gpu_cp_snapshot_mmio'}
    required |= {'_cepheus_cp_workspace_prepare','_cepheus_cp_workspace_release'}
    required |= {'_cepheus_gpu_vm39_build'}
    required |= {'_cepheus_gpu_gx_core_configure'}
    required |= {'_cepheus_gpu_gx_perfcounter_configure'}
    required |= {'_cepheus_gpu_gx_gbif_configure','_cepheus_gpu_gx_va_partition'}
    required |= {'_cepheus_gpu_gx_llc_configure'}
    required |= {'_cepheus_gpu_scm_discover','_cepheus_gpu_scm_probe_once'}
    required |= {'_cepheus_gpu_gx_hwcg_configure'}
    required |= {'_cepheus_gpu_cp_unhalt_check'}
    required |= {'_cepheus_gpu_gx_smmu_plan'}
    required |= {'_cepheus_cp_workspace_table'}
    required |= {'_cepheus_cp_workspace_validate'}
    required |= {'_cepheus_cp_workspace_restore'}
    required |= {'_cepheus_gpu_config_probe','_cepheus_gpu_fuse_sample'}
    required |= {'_cepheus_gmu_hfi_poll_debug'}
    required |= {'_cepheus_gpu_park_read','_cepheus_gpu_park_write','_cepheus_gpu_slumber_read','_cepheus_gpu_slumber_write'}
    if required - symbols:
        raise ValueError(f'missing linked GPU symbols: {sorted(required - symbols)}')
    memory_api={'_cepheus_gpu_memory_alloc','_cepheus_gpu_memory_view_get','_cepheus_gpu_memory_view_put',
                '_cepheus_gpu_memory_publish','_cepheus_gpu_memory_release_cpu',
                '_cepheus_gpu_firmware_snapshot','_cepheus_gpu_firmware_get'}
    memory_api |= {'_cepheus_gpu_memory_share_workspace','_cepheus_gpu_memory_retire_workspace',
                   '_cepheus_gmu_workspace_share','_cepheus_gmu_workspace_retire','_cepheus_gpu_memory_group_status'}
    memory_api |= {'_cepheus_gpu_memory_abandon_workspace'}
    if memory_api - symbols:
        raise ValueError(f'missing linked memory lifecycle APIs: {sorted(memory_api - symbols)}')
    if subprocess.check_output([nm, '-u', str(args.kernel)], text=True).strip():
        raise ValueError('kernel has unresolved symbols')
    print(f'GPU staged sources, {len(required)} core entrypoints and {len(memory_api)} memory/firmware APIs: PASS (not hardware readiness)')


if __name__ == '__main__':
    main()
