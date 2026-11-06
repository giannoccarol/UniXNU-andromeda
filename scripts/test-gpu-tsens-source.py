#!/usr/bin/env python3
"""Cross-check compiled GPU sampler against saved FDT and downstream TSENS ABI."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('thermal_dir',type=Path);p.add_argument('fdt',type=Path);a=p.parse_args()
    spec=importlib.util.spec_from_file_location('gpu_fdt',ROOT/'scripts/gpu-fdt-config.py')
    fdt=importlib.util.module_from_spec(spec);spec.loader.exec_module(fdt)
    zones=fdt.report(a.fdt.read_bytes())['thermal']
    source=(a.thermal_dir/'tsens2xxx.c').read_text();header=(a.thermal_dir/'tsens.h').read_text()
    registration=(a.thermal_dir/'msm-tsens.c').read_text()
    if 'tmdev->sensor[i].hw_id = i;' not in registration:raise ValueError('sensor ID registration changed')
    # Compile original simple register macros, not another set of hand-coded
    # numbers. Refuse absent/changed shapes rather than silently using defaults.
    names=('TSENS_TM_TRDY','TSENS_TM_TRDY_FIRST_ROUND_COMPLETE','TSENS_TM_SN_STATUS_VALID_BIT',
           'TSENS_TM_CODE_BIT_MASK','TSENS_TM_CODE_SIGN_BIT','TSENS_TM_SCALE_DECI_MILLIDEG',
           'TSENS_TM_CRITICAL_INT_STATUS','TSENS_TM_CRITICAL_WD_BARK','TSENS_EN','TSENS_CTRL_SENSOR_EN_MASK')
    macros=['#define BIT(n) (1u << (n))']
    for name in names:
        m=re.search(r'^#define\s+'+name+r'(?:\([^\n]*?\))?\s+[^\n]+$',source,re.M)
        if not m:raise ValueError('missing macro '+name)
        macros.append(m[0])
    m=re.search(r'^#define\s+TSENS_TM_SN_STATUS\(n\)\s+[^\n]+$',header,re.M)
    if not m:raise ValueError('status macro absent')
    macros.append(m[0])
    if 'TSENS_CTRL_ADDR(tmdev->tsens_srot_addr + 0x4)' not in source:
        raise ValueError('SROT control offset changed')
    if [z['zone'] for z in zones]!=['gpuss-0-usr','gpuss-1-usr']:raise ValueError('zone order changed')
    meta='static const unsigned sensors[]={'+','.join(str(z['sensor']) for z in zones)+'};\n'
    meta+='static const uint64_t tm_pa[]={'+','.join(hex(z['tm']['address']) for z in zones)+'};\n'
    meta+='static const uint64_t srot_pa[]={'+','.join(hex(z['srot']['address']) for z in zones)+'};\n'
    code='''#include <assert.h>
#include "cepheus_gpu_tsens.h"
'''+ '\n'.join(macros)+'\n'+meta+'''
static uint32_t tm[2][1024],srot[2][1024];
static int lease(void *p){(void)p;return 0;}
static int now(void *p,uint64_t *v){(void)p;*v=123;return 0;}
int main(void) {
 struct ceph_gpu_tsens m={{tm[0],tm[1]},{4096,4096},{tm_pa[0],tm_pa[1]},0,lease,now,
                         {srot[0],srot[1]},{4096,4096},{srot_pa[0],srot_pa[1]}};
 for(unsigned i=0;i<2;++i) {
  srot[i][1]=TSENS_EN|(1u<<(sensors[i]+3));assert(TSENS_CTRL_SENSOR_EN_MASK(srot[i][1])&(1u<<sensors[i]));
  tm[i][TSENS_TM_TRDY(0)/4]=TSENS_TM_TRDY_FIRST_ROUND_COMPLETE;
  tm[i][TSENS_TM_SN_STATUS(0)/4+sensors[i]]=TSENS_TM_SN_STATUS_VALID_BIT|(i?425:400);
 }
 struct ceph_gpu_temperature sample;assert(!ceph_gpu_tsens_sample(&m,&sample));
 assert(sample.gpuss0_mc==400*TSENS_TM_SCALE_DECI_MILLIDEG && sample.gpuss1_mc==425*TSENS_TM_SCALE_DECI_MILLIDEG);
 tm[1][TSENS_TM_CRITICAL_INT_STATUS(0)/4]=TSENS_TM_CRITICAL_WD_BARK;
 assert(ceph_gpu_tsens_sample(&m,&sample)<0);
 assert(tm[1][TSENS_TM_CRITICAL_INT_STATUS(0)/4]==TSENS_TM_CRITICAL_WD_BARK);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('GPU TSENS sampler agrees with supplied FDT thermal-zone IDs/controllers and downstream macros: PASS (RAM)')


if __name__=='__main__':main()
