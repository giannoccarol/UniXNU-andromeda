#!/usr/bin/env python3
"""Fixed discovery ABI checks against an explicit SM8150 source reference."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
header = (root / 'include/soc/qcom/scm.h').read_text()
source = (root / 'drivers/soc/qcom/scm.c').read_text()
for macro, value in (('SCM_SVC_INFO', '0x6'), ('SCM_SVC_PIL', '0x2')):
    assert re.search(r'#define\s+' + macro + r'\s+' + value + r'\b', header)
for macro, value in (('SMC64_MASK', '0x40000000'), ('SMC_ATOMIC_MASK', '0x80000000'),
                     ('IS_CALL_AVAIL_CMD', '1'), ('SCM_INTERRUPTED', '1'), ('SCM_V2_EBUSY', '-12')):
    assert re.search(r'#define\s+' + macro + r'\s+' + value + r'\b', source)
discovery = source.split('bool is_scm_armv8(void)', 1)[1].split('EXPORT_SYMBOL(is_scm_armv8)', 1)[0]
assert 'SCM_SIP_FNID(SCM_SVC_INFO, IS_CALL_AVAIL_CMD) | SMC_ATOMIC_MASK' in discovery
assert '__scm_call_armv8_64(x0 | SMC64_MASK, SCM_ARGS(1), x0' in discovery
assert '__scm_call_armv8_32(x0, SCM_ARGS(1), x0' in discovery
pas = (root / 'drivers/firmware/qcom_scm-64.c').read_text()
assert 'QCOM_SCM_ARGS(1)' in pas.split('bool __qcom_scm_pas_supported', 1)[1].split('int __qcom_scm_pas_init_image', 1)[0]
constants = (root / 'drivers/firmware/qcom_scm.h').read_text()
assert re.search(r'#define\s+QCOM_SCM_PAS_IS_SUPPORTED_CMD\s+0x7\b', constants)
print('SCM source: fixed INFO6/PIL2, 64->32 convention query, resume/busy constants, PAS supported7 PASS (no SMC)')
