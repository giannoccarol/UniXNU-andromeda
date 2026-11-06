#!/usr/bin/env python3
"""A640 LLC register/SCID comparisons against explicit original source paths."""
from pathlib import Path
import re
import sys

kernel = Path(sys.argv[1])
kgsl = kernel / 'drivers/gpu/msm'
regs = (kgsl / 'a6xx_reg.h').read_text()
assert re.search(r'#define\s+A6XX_GBIF_SCACHE_CNTL1\s+0x3c02\b', regs)
assert re.search(r'#define\s+A6XX_LLC_NUM_GPU_SCIDS\s+5\b', regs)
assert re.search(r'#define\s+A6XX_GPU_LLC_SCID_NUM_BITS\s+5\b', regs)
body = (kgsl / 'adreno_a6xx.c').read_text()
gpu = body.split('static void a6xx_llc_configure_gpu_scid', 1)[1].split('static void a6xx_llc_configure_gpuhtw_scid', 1)[0]
assert 'adreno_is_a640' in gpu and 'A6XX_GBIF_SCACHE_CNTL1' in gpu and 'kgsl_regrmw' in gpu
htw = body.split('static void a6xx_llc_configure_gpuhtw_scid', 1)[1].split('static void a6xx_llc_enable_overrides', 1)[0]
assert htw.index('adreno_is_a640') < htw.index('return;') < htw.index('adreno_cx_misc_regrmw')
overrides = body.split('static void a6xx_llc_enable_overrides', 1)[1].split('static const char', 1)[0]
assert 'MMU-500' in overrides
assert overrides.index('adreno_is_a640') < overrides.index('return;') < overrides.index('adreno_cx_misc_regwrite')
llcc = (kernel / 'drivers/soc/qcom/llcc-sm8150.c').read_text()
assert re.search(r'SCT_ENTRY\("gpu"\s*,\s*12\s*,\s*12\s*,', llcc)
assert re.search(r'SCT_ENTRY\("gpuhtw"\s*,\s*11\s*,\s*11\s*,', llcc)
setup = (kgsl / 'adreno_llc.h').read_text()
assert setup.index('llcc_slice_activate(adreno_dev->gpu_llc_slice)') < setup.index('gpudev->llc_configure_gpu_scid(adreno_dev)')
print('A640 LLC source: GBIF RMW, SCID12/HTW11, activation prerequisite, no MMU-500 overrides PASS (source only)')
