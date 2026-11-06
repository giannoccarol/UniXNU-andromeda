#!/usr/bin/env python3
"""Run offline GPU checks and write an evidence receipt (never touches devices).

Optional proprietary fixtures/reference trees are explicit. Missing inputs are
SKIP entries, not passed checks. Logs and SHA256 identities stay in requested
local output directory; no upload, authentication, boot or MMIO operations.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def identity(path):
    if path is None:return None
    return {'path':str(path),'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True,type=Path)
    for name in ('gmu','sqe','fdt','zap','kernel-reference','xnu-reference','patches'):
        p.add_argument('--'+name,type=Path)
    a=p.parse_args();out=a.output.resolve()
    if out.exists():p.error('output already exists; keep earlier evidence intact')
    if bool(a.gmu)!=bool(a.sqe):p.error('supply both --gmu and --sqe, or neither')
    for name in ('gmu','sqe','fdt','zap','kernel_reference','xnu_reference','patches'):
        value=getattr(a,name)
        if value:
            value=value.resolve();setattr(a,name,value)
            if not value.exists():p.error('missing input '+str(value))
    out.mkdir(parents=True)
    receipt={'time_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
             'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
             'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),
             'scope':'OFFLINE ONLY: no actual IOKit, GPU execution, PAS or device acceptance',
             'fixtures':{name:identity(getattr(a,name)) for name in ('gmu','sqe','fdt','zap')},'checks':[]}
    checks=[('host',['bash','scripts/test-gpu-stack.sh'],{}),
            ('pmap-cache-pa',['python3','scripts/test-pmap-cache-pa.py'] +
             (['--xnu',str(a.xnu_reference)] if a.xnu_reference else []),{}),
            ('pmap-cache-verifier',['python3','scripts/test-pmap-cache-verifier.py'],{}),
            ('scm-codegen',['python3','scripts/test-scm-codegen.py'],{}),
            ('diagnostic-log-tests',['python3','scripts/test-gpu-diagnostic-log.py'],{}),
            ('sanitizers',['bash','scripts/test-gpu-sanitizers.sh'],{}),
            ('static',['python3','scripts/analyze-gpu-host.py'],{}),
            ('integration',['python3','scripts/test-gpu-integration.py'],
             {'CEPHEUS_XNU_REFERENCE':str(a.xnu_reference)} if a.xnu_reference else {}),
            ('fdt-parser',['python3','scripts/test-gpu-fdt.py'],{'CEPHEUS_LIVE_FDT':str(a.fdt)} if a.fdt else {}),
            ('zap-layout',['python3','scripts/test-zap-layout.py'],{'CEPHEUS_ZAP_ELF':str(a.zap)} if a.zap else {}),
            ('memory-host',['bash','scripts/test-gpu-memory-host.sh'],{})]
    if a.gmu:
        checks.append(('memory-real-fixtures',['bash','scripts/test-gpu-memory-host.sh',str(a.gmu),str(a.sqe)],
                       {'CEPHEUS_GPU_HOST_ASAN':'1','ASAN_OPTIONS':'detect_leaks=0'}))
    else:receipt['checks'].append({'name':'memory-real-fixtures','status':'SKIP','reason':'no proprietary GMU/SQE input'})
    if a.patches:checks.append(('arc',['python3','scripts/test-gpu-arc.py',str(a.patches)],{}))
    else:receipt['checks'].append({'name':'arc','status':'SKIP','reason':'no pinned ios-sweet patches'})
    if a.kernel_reference:
        kgsl=a.kernel_reference/'drivers/gpu/msm';iommu=a.kernel_reference/'drivers/iommu'
        for name,script,args in (
            ('hfi-source','test-hfi-source-layout.py',[kgsl/'kgsl_hfi.h']),
            ('cp-source','test-cp-source-layout.py',[kgsl]),
            ('llc-source','test-llc-source.py',[a.kernel_reference]),
            ('arc-bcm-source','test-arc-bcm-source.py',[a.kernel_reference]),
            ('scm-source','test-scm-source.py',[a.kernel_reference]),
            ('cp-permissions-source','test-cp-permissions-source.py',[a.kernel_reference]),
            ('gmu-mmio-source','test-gmu-mmio-source.py',[kgsl/'a6xx_reg.h']),
            ('gmu-permissions-source','test-gmu-permissions-source.py',[a.kernel_reference]),
            ('smmu-source','test-smmu-source-layout.py',[iommu]),
            ('qmp-source','test-qmp-source-layout.py',[a.kernel_reference/'drivers/mailbox/msm_qmp.c'])):
            checks.append((name,['python3','scripts/'+script,*map(str,args)],{}))
        if a.fdt:
            checks.extend([('gx-source',['python3','scripts/test-gx-source-layout.py',str(kgsl),str(a.fdt)],{}),
                           ('tsens-source',['python3','scripts/test-gpu-tsens-source.py',str(a.kernel_reference/'drivers/thermal'),str(a.fdt)],{})])
        else:receipt['checks'].append({'name':'gx/tsens-source','status':'SKIP','reason':'no captured FDT'})
    else:receipt['checks'].append({'name':'downstream-source-comparison','status':'SKIP','reason':'no explicit kernel reference'})
    for name,present in (('recorded-FDT',a.fdt),('real-ZAP',a.zap),('real-XNU-conf',a.xnu_reference)):
        if not present:receipt['checks'].append({'name':name,'status':'SKIP','reason':'input omitted'})
    # Prevent ambient fixture variables silently turning an omitted input into
    # a different private run. Other normal compiler environment is preserved.
    env=os.environ.copy()
    for key in ('CEPHEUS_XNU_REFERENCE','CEPHEUS_LIVE_FDT','CEPHEUS_ZAP_ELF',
                'CEPHEUS_GPU_HOST_ASAN','CEPHEUS_GPU_SANITIZERS','ASAN_OPTIONS'):
        env.pop(key,None)
    failed=False
    for name,command,extra in checks:
        run_env=env|extra
        with (out/(name+'.log')).open('wb') as log:
            try:
                result=subprocess.run(command,cwd=ROOT,env=run_env,stdout=log,stderr=subprocess.STDOUT,timeout=240)
                code=result.returncode
            except subprocess.TimeoutExpired:code=124
        status='PASS' if code==0 else 'FAIL';failed|=code!=0
        receipt['checks'].append({'name':name,'status':status,'exit_code':code,'log':name+'.log',
                                  'command':command,'environment':extra})
        print(name,status,flush=True)
        # Persist after every completed check, including failures/timeouts.
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    receipt['result']='FAIL' if failed else 'PASS_WITH_SKIPS' if any(c['status']=='SKIP' for c in receipt['checks']) else 'PASS'
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(receipt['result'],out/'receipt.json')
    return 1 if failed else 0


if __name__=='__main__':sys.exit(main())
