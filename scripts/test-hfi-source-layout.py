#!/usr/bin/env python3
"""Compare encoders with structs extracted from a caller-supplied KGSL header.

Private host fixture only; neither modifies Android nor copies its code into the
repository. Explicit source input avoids relying on a machine-specific path.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STRUCTS = ("hfi_bwtable_cmd", "opp_gx_desc", "opp_desc", "hfi_dcvstable_cmd",
           "hfi_acd_table_cmd", "hfi_fw_version_cmd", "hfi_feature_ctrl_cmd", "hfi_core_fw_start_cmd", "hfi_prep_slumber_cmd",
           "hfi_err_cmd", "hfi_debug_cmd", "hfi_gx_bw_perf_vote_cmd", "hfi_start_cmd")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kgsl_hfi_header", type=Path)
    args = parser.parse_args()
    text = args.kgsl_hfi_header.read_text()
    structs = []
    for name in STRUCTS:
        matches = re.findall(rf"struct {name}\s*\{{[^{{}}]*\}};", text)
        if len(matches) != 1:
            raise ValueError(f"expected one source definition for {name}")
        structs.append(matches[0])
    # Independent source limits from kgsl_gmu_core.h.
    core = args.kgsl_hfi_header.with_name("kgsl_gmu_core.h").read_text()
    constants = []
    for name in ("MAX_GX_LEVELS", "MAX_CX_LEVELS", "MAX_CNOC_LEVELS", "MAX_CNOC_CMDS", "MAX_BW_CMDS"):
        values = re.findall(rf"^#define\s+{name}\s+(\d+)\s*$", core, flags=re.M)
        if len(values) != 1:
            raise ValueError(f"missing numeric source constant {name}")
        constants.append(f"#define {name} {values[0]}")
    for name in ("MAX_ACD_STRIDE", "MAX_ACD_NUM_LEVELS"):
        values = re.findall(rf"^#define\s+{name}\s+(\d+)\s*$", text, flags=re.M)
        if len(values) != 1:
            raise ValueError(f"missing source constant {name}")
        constants.append(f"#define {name} {values[0]}")
    preamble = '#include <stdint.h>\n#include <stddef.h>\n#include <assert.h>\n#include <string.h>\n#include "cepheus_hfi_messages.h"\n'
    checks = r'''
_Static_assert(sizeof(struct hfi_dcvstable_cmd)==CE_HFI_PERF_WORDS*4, "PERF size");
_Static_assert(offsetof(struct hfi_dcvstable_cmd,cx_votes)==51*4, "CX offset");
_Static_assert(sizeof(struct hfi_bwtable_cmd)==CE_HFI_BW_WORDS*4, "BW size");
_Static_assert(offsetof(struct hfi_bwtable_cmd,cnoc_cmd_data)==12*4, "CNOC data offset");
_Static_assert(offsetof(struct hfi_bwtable_cmd,ddr_cmd_addrs)==24*4, "DDR address offset");
_Static_assert(offsetof(struct hfi_bwtable_cmd,ddr_cmd_data)==32*4, "DDR data offset");
_Static_assert(sizeof(struct hfi_acd_table_cmd)==CE_HFI_ACD_WORDS*4, "ACD size");
_Static_assert(sizeof(struct hfi_err_cmd)==18*4 && offsetof(struct hfi_err_cmd,error_code)==4, "ERR ABI");
_Static_assert(sizeof(struct hfi_debug_cmd)==4*4 && offsetof(struct hfi_debug_cmd,data)==12, "DEBUG ABI");
int main(void) {
    struct ceph_arc_resource r={.levels={0,16,48,64},.count=4};
    struct ceph_hfi_opp gx[]={{0,0},{215000000,64}},cx[]={{0,0},{200000000,48}};
    uint32_t encoded[160];
    assert(ceph_hfi_perf(encoded,160,gx,2,cx,2,&r,&r,&r)==59);
    struct hfi_dcvstable_cmd p={0};p.hdr=0x3b04;p.gpu_level_num=p.gmu_level_num=2;
    p.gx_votes[0].acd=p.gx_votes[1].acd=0xffffffff;
    p.gx_votes[1].vote=0x400303;p.gx_votes[1].freq=215000;
    p.cx_votes[1].vote=0x300202;p.cx_votes[1].freq=200000;
    assert(!memcmp(encoded,&p,sizeof(p)));
    struct ceph_hfi_bw input={.levels=2,.cnoc_count=1,.ddr_count=1,.cnoc_wait=1,.ddr_wait=1,
        .cnoc_addr={0x50000},.ddr_addr={0x50004},.cnoc_data={{123},{456}},.ddr_data={{789},{987}}};
    assert(ceph_hfi_bandwidth(encoded,160,&input)==160);
    struct hfi_bwtable_cmd b={0};b.hdr=0xa003;b.bw_level_num=2;b.cnoc_cmds_num=b.ddr_cmds_num=1;
    b.cnoc_wait_bitmask=b.ddr_wait_bitmask=1;b.cnoc_cmd_addrs[0]=0x50000;b.ddr_cmd_addrs[0]=0x50004;
    b.cnoc_cmd_data[0][0]=123;b.cnoc_cmd_data[1][0]=456;b.ddr_cmd_data[0][0]=789;b.ddr_cmd_data[1][0]=987;
    assert(!memcmp(encoded,&b,sizeof(b)));
    struct hfi_fw_version_cmd v={0x201,0x20080000};ceph_hfi_fw_version(encoded,160,2,8);
    assert(!memcmp(encoded,&v,sizeof(v)));
    struct hfi_feature_ctrl_cmd f={0x40b,7,1,3};ceph_hfi_feature(encoded,160,7,1,3);
    assert(!memcmp(encoded,&f,sizeof(f)));
    struct hfi_core_fw_start_cmd s={0x20e,0};ceph_hfi_core_start(encoded,160);
    assert(!memcmp(encoded,&s,sizeof(s)));
    struct hfi_prep_slumber_cmd sleep={0x321,3,1};ceph_hfi_slumber(encoded,160,3,1,12,8);
    assert(!memcmp(encoded,&sleep,sizeof(sleep)));
    struct hfi_gx_bw_perf_vote_cmd dcvs={0x41e,1,1,0xff};ceph_hfi_dcvs(encoded,160,1,0xff);
    assert(!memcmp(encoded,&dcvs,sizeof(dcvs)));
    struct hfi_start_cmd start={0x10a};assert(ceph_hfi_start(encoded,160)==1);
    assert(!memcmp(encoded,&start,sizeof(start)));
}
'''
    with tempfile.TemporaryDirectory(prefix="ceph-hfi-source-") as tmp:
        fixture = Path(tmp) / "layout.c"
        fixture.write_text(preamble + '\n'.join(constants + structs) + checks)
        binary = Path(tmp) / "test"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I", str(ROOT / "xnu"), str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("HFI encoder bytes and fixed layouts match supplied KGSL source structs: PASS")


if __name__ == "__main__":
    main()
