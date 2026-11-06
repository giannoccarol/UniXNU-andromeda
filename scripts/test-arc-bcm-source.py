#!/usr/bin/env python3
"""Compare ARC rounding and ACV ABI facts against explicit SM8150 sources.

Source assertions only: no hardware calls, ownership or ACV byte defaults.
The host suite exercises the production encoders with separate RAM fixtures.
"""
from pathlib import Path
import re
import sys

kernel = Path(sys.argv[1])
root = Path(__file__).resolve().parents[1]
kgsl = (kernel / 'drivers/gpu/msm/kgsl_gmu.c').read_text()
body = kgsl.split('static int setup_volt_dependency_tbl(', 1)[1].split(
    'static int rpmh_arc_votes_init(', 1)[0]
assert 'pri_rail->val[k] >= vlvl[i]' in body
assert 'cur_vlvl = pri_rail->val[k]' in body
assert 'sec_rail->val[j] >= cur_vlvl' in body
assert 'votes[i] = ARC_VOTE_SET(k, j, cur_vlvl)' in body
# KGSL saturates secondary at the final level. Our stricter missing-coverage
# rejection is intentional; do not claim byte-identical behavior for that case.
assert 'j + 1 == sec_rail->num' in body
gpucc = (kernel / 'drivers/clk/qcom/gpucc-sm8150.c').read_text()
assert re.search(r'\[VDD_MIN\]\s*=\s*200000000', gpucc)
bus = kernel / 'drivers/soc/qcom/msm_bus'
fabric = (bus / 'msm_bus_fabric_rpmh.c').read_text()
aux = fabric.split('struct bcm_db {', 1)[1].split('};', 1)[0]
for field in ('uint32_t unit_size;', 'uint16_t width;', 'uint8_t clk_domain;', 'uint8_t reserved;'):
    assert field in aux
init = fabric.split('static int msm_bus_bcm_init(', 1)[1].split(
    'static int msm_bus_rsc_init(', 1)[0]
for field in ('aux_data.width', 'aux_data.clk_domain', 'aux_data.unit_size'):
    assert field in init
arb = (bus / 'msm_bus_arb_rpmh.c').read_text()
assert re.search(r'#define\s+BCM_TCS_CMD_ACV_APPS\s+0x8\b', arb)
acv = arb.split('static void bcm_update_acv_req(', 1)[1].split(
    'static void bcm_update_bus_req(', 1)[0]
for fact in ('MSM_BUS_RSC_APPS', 'max_ab == 0 && max_ib == 0', '*vec_a = 0;', '*vec_b = *acv;'):
    assert fact in acv
arc = (root / 'xnu/cepheus_arc.h').read_text()
assert 'ceph_arc_select(mx,cx->levels[c],ceiling,&m)' in arc
power = (root / 'xnu/cepheus_gpu_power_txn.h').read_text()
assert 'ceph_arc_pair(&in->cx,&in->mx,48,UINT16_MAX,&c,&m)' in power
bcm = (root / 'xnu/cepheus_bcm.h').read_text()
assert 'key[0]!=0x00564341u || key[1]' in bcm
assert 'size!=8 || (off&3) || !ceph_arc_range(db,off,8)' in bcm
assert '(!acv && (!found.unit || !found.width))' in bcm
assert 'ceph_bcm_lookup_kind(db,"ACV",out,1)' in bcm
print('ARC/ACV source: primary ceil, rounded-primary MX dependency, BCM ABI/apps bit; stricter secondary coverage PASS (source only)')
