#!/usr/bin/env python3
"""Summarize a SAVED CPU-only GPU diagnostic boot log, never contact a phone.

Evidence completeness is not device acceptance or GPU acceleration. Missing or
conflicting records cannot turn into PASS. Old preflight logs remain useful but
lack later fuse/ARC/workspace/thermal evidence. Input must be original logs, not
test-fixture output. Host/RAM/synthetic records are explicitly rejected.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def report(text):
    checks=[]
    def check(name,pattern,valid):
        records=re.findall(pattern,text,re.M)
        if not records:status='MISSING'
        elif len(set(records))!=1:status='CONFLICT'
        else:status='OBSERVED' if valid(records[0]) else 'FAILED'
        checks.append({'name':name,'status':status,'records':records})
    host=bool(re.search(r'emulated hardware|RAM hardware|IOKit double|IOKit test double|synthetic metadata|host IOKit|host test programs',text,re.I))
    checks.append({'name':'no-host-fixture-marker','status':'FAILED' if host else 'OBSERVED',
                   'note':'absence of marker cannot authenticate log origin'})
    check('probe-only',r'(CEPHEUS GPU: probe only; no GPU submission, firmware upload or Metal support)',lambda _:True)
    check('power-observation',r'CEPHEUS GPU: CX_HW_CTRL=(0x[0-9a-f]+) gmu_blockers=(0x[0-9a-f]+) memory_blockers=(0x[0-9a-f]+)',lambda _:True)
    check('fuse-bin',r'CEPHEUS GPU BIN: bin=(\d+) gx_levels=(\d+) lm=(\d+) acd=(\d+) throttle=(0x[0-9a-f]+)',
          lambda r:tuple(map(int,r[:4])) in ((0,8,0,0),(1,7,1,1)) and int(r[4],16)==(0x80 if r[0]=='0' else 0x40))
    check('arc-readonly',r'CEPHEUS ARC: read-only result=(-?\d+); current ARC vote UNKNOWN, no power writes',lambda r:r=='0')
    check('configuration-dryrun',r'CEPHEUS GPU CONFIG: dry-run result=(-?\d+); no HFI, MMIO writes or ownership grant',lambda r:r=='0')
    check('workspace-built',r'CEPHEUS GMU MEM: CPU workspace table_pages=(\d+) HFI=(0x[0-9a-f]+) log=(0x[0-9a-f]+); no SMMU/DMA',
          lambda r:int(r[0])==6 and int(r[1],16)==0x60100000 and int(r[2],16)==0x60200000)
    check('workspace-released',r'CEPHEUS GMU MEM: CPU workspace release=(-?\d+) state=(\d+)',lambda r:r==('0','3'))
    check('gpu-thermal-zones',r'CEPHEUS GPU TSENS: valid_mask=(\d+) gpuss0_mc=(-?\d+) gpuss1_mc=(-?\d+) \(TSENS0:15 TSENS1:11, sample only\)',
          lambda r:r[0]=='3' and all(-40000<=int(v)<60000 for v in r[1:]))
    # Optional: only present when the GX read-only probe was requested. A
    # powered-down or unfenced GX legitimately reports "not read", which is
    # evidence of a safe refusal, not of GX content.
    gx_refusal=r'CEPHEUS GPU GX: (?:GMU not powered/clocked|SPTPRAC_PWR_CLK=0x[0-9a-f]+ \(GX not powered/clocked\)|AHB_FENCE=0x[0-9a-f]+ \(not ALLOW\)|CFG_AHB unavailable|GMU aperture unreadable); GX not read'
    gx_read=r'CEPHEUS GPU GX: CP_HALT=(0x[0-9a-f]+) CP_FAULT=(0x[0-9a-f]+) PROTECT_CNTL=(0x[0-9a-f]+) RING=(0x[0-9a-f]+)/(0x[0-9a-f]+) SQE=(0x[0-9a-f]+)/(0x[0-9a-f]+)'
    refused=re.findall(gx_refusal,text,re.M);read=re.findall(gx_read,text,re.M)
    # Absent entirely: the probe was not requested in that boot, which is not a
    # gap in this log. Only an explicit request that produced neither a read nor
    # a documented refusal would be MISSING.
    requested=bool(re.search(r'cephgx=1',text))
    if not refused and not read:status='MISSING' if requested else 'NOT_REQUESTED'
    elif refused and read:status='CONFLICT'
    elif read:
        r=read[0]
        # A read is only credible when the CP fault register is clear; the halt
        # bit and 64-bit address modes are recorded, not asserted as healthy.
        status='OBSERVED' if int(r[1],16)==0 else 'FAILED'
    else:status='OBSERVED'
    checks.append({'name':'gx-halted-window','status':status,'records':read or refused,
                   'note':'refusal is a safe outcome; GX content is not authorization to write'})
    check('touch-controller',r'(SWEET TOUCH \(cepheus\): selected ST-FTS, 1080x2340)',lambda _:True)
    check('home-injection',r'(SWEET HID: one-shot Home complete)',lambda _:True)
    hard=[]
    for line in text.splitlines():
        if re.search(r'panic\(cpu|pmap cache mismatch|startup abandoned group=|terminal GPU session|workspace preparation failed|allocation retained',line):hard.append(line)
    checks.append({'name':'no-known-fatal-markers','status':'FAILED' if hard else 'OBSERVED','records':hard})
    failed=any(c['status'] in ('FAILED','CONFLICT') for c in checks)
    missing=any(c['status']=='MISSING' for c in checks)
    return {'result':'REJECTED' if failed else 'INCOMPLETE' if missing else 'READONLY_EVIDENCE_COMPLETE',
            'checks':checks,'gpu_acceleration':'UNVERIFIED','arc_vote_ownership':'UNKNOWN',
            'acceptance':'User must verify homescreen/touch/power; log presence is not acceptance',
            'limitations':['not authenticated log origin','no GMU execution/PAS/CP/Metal proof',
                           'temperature sample is not ongoing conversion/thermal-stop proof',
                           'power blockers are observations, never authorization to write']}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);a=p.parse_args()
    data=a.log.read_bytes()
    if len(data)>32*1024*1024:p.error('log exceeds32MiB bound')
    r=report(data.decode('utf-8',errors='replace'));r['log_sha256']=hashlib.sha256(data).hexdigest();r['log_bytes']=len(data)
    print(json.dumps(r,indent=2));return 1 if r['result']=='REJECTED' else 2 if r['result']=='INCOMPLETE' else 0


if __name__=='__main__':raise SystemExit(main())
