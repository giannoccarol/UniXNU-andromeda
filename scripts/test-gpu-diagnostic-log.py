#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('diag',Path(__file__).with_name('check-gpu-diagnostic-log.py'))
diag=importlib.util.module_from_spec(spec);spec.loader.exec_module(diag)
SAMPLE='''CEPHEUS GPU: probe only; no GPU submission, firmware upload or Metal support
CEPHEUS GPU: CX_HW_CTRL=0x5ff4 gmu_blockers=0x7a memory_blockers=0x37a
CEPHEUS GPU BIN: bin=1 gx_levels=7 lm=1 acd=1 throttle=0x40
CEPHEUS ARC: read-only result=0; current ARC vote UNKNOWN, no power writes
CEPHEUS GPU CONFIG: dry-run result=0; no HFI, MMIO writes or ownership grant
CEPHEUS GMU MEM: CPU workspace table_pages=6 HFI=0x60100000 log=0x60200000; no SMMU/DMA
CEPHEUS GMU MEM: CPU workspace release=0 state=3
CEPHEUS GPU TSENS: valid_mask=3 gpuss0_mc=40000 gpuss1_mc=41000 (TSENS0:15 TSENS1:11, sample only)
SWEET TOUCH (cepheus): selected ST-FTS, 1080x2340
SWEET HID: one-shot Home complete
'''


class LogTests(unittest.TestCase):
    def test_complete_not_acceleration(self):
        r=diag.report(SAMPLE)
        self.assertEqual(r['result'],'READONLY_EVIDENCE_COMPLETE')
        self.assertEqual(r['gpu_acceleration'],'UNVERIFIED')
        self.assertEqual(r['arc_vote_ownership'],'UNKNOWN')

    def test_absence_not_success(self):
        self.assertEqual(diag.report('')['result'],'INCOMPLETE')
        for line in SAMPLE.splitlines():
            self.assertEqual(diag.report(SAMPLE.replace(line,'',1))['result'],'INCOMPLETE')

    def test_fault_conflict_and_host_rejected(self):
        # GX probe: an explicit request must yield either a read or a documented
        # refusal; a refusal alone is a safe outcome and stays acceptable.
        refused=SAMPLE+'cephgx=1\nCEPHEUS GPU GX: AHB_FENCE=0x1 (not ALLOW); GX not read\n'
        self.assertEqual(diag.report(refused)['result'],'READONLY_EVIDENCE_COMPLETE')
        read=SAMPLE+'cephgx=1\nCEPHEUS GPU GX: CP_HALT=0x3 CP_FAULT=0x0 PROTECT_CNTL=0xb RING=0xfc000000/0x0 SQE=0xfc020000/0x0\n'
        self.assertEqual(diag.report(read)['result'],'READONLY_EVIDENCE_COMPLETE')
        # Requested but silent, a faulted CP, and a read+refusal contradiction
        # must never be accepted.
        self.assertEqual(diag.report(SAMPLE+'cephgx=1\n')['result'],'INCOMPLETE')
        faulted=SAMPLE+'cephgx=1\nCEPHEUS GPU GX: CP_HALT=0x3 CP_FAULT=0x1 PROTECT_CNTL=0xb RING=0x0/0x0 SQE=0x0/0x0\n'
        self.assertEqual(diag.report(faulted)['result'],'REJECTED')
        both=refused+'CEPHEUS GPU GX: CP_HALT=0x3 CP_FAULT=0x0 PROTECT_CNTL=0xb RING=0x0/0x0 SQE=0x0/0x0\n'
        self.assertEqual(diag.report(both)['result'],'REJECTED')
        for bad in (SAMPLE.replace('release=0 state=3','release=-1 state=4'),
                    SAMPLE.replace('gpuss1_mc=41000','gpuss1_mc=60000'),
                    SAMPLE+'CEPHEUS GPU BIN: bin=0 gx_levels=8 lm=0 acd=0 throttle=0x80\n',
                    SAMPLE+'panic(cpu 0 caller 0x0)\n',SAMPLE+'PASS (emulated hardware)',
                    SAMPLE.replace('dry-run result=0','dry-run result=-1')):
            self.assertEqual(diag.report(bad)['result'],'REJECTED')


if __name__=='__main__':unittest.main()
