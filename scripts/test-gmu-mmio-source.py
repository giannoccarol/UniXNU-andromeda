#!/usr/bin/env python3
"""Compare compiled MMIO adapters with explicit downstream A6xx definitions."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('a6xx_reg',type=Path);a=p.parse_args()
    text=a.a6xx_reg.read_text()
    defs={k:int(v,0) for k,v in re.findall(r'^#define\s+(A6XX_\w+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)\s*$',text,re.M)}
    startup=['GMU_CM3_SYSRESET','GMU_CM3_FW_INIT_RESULT','GMU_CM3_BOOT_CONFIG','GMU_CM3_CFG',
        'GMU_HFI_QTBL_ADDR','GMU_HFI_QTBL_INFO','GMU_AHB_FENCE_RANGE_0','GMU_HFI_SFR_ADDR',
        'GPU_GMU_CX_GMU_PWR_COL_CP_MSG','GMU_SYS_BUS_CONFIG','GMU_ICACHE_CONFIG','GMU_DCACHE_CONFIG',
        'GMU_PWR_COL_INTER_FRAME_CTRL','GMU_PWR_COL_INTER_FRAME_HYST','GMU_PWR_COL_SPTPRAC_HYST',
        'GMU_RPMH_CTRL','GMU_CX_GMU_WFI_CONFIG','GMU_HFI_CTRL_INIT','GMU_HFI_CTRL_STATUS',
        'GMU_HFI_VERSION_INFO','GMU_AO_HOST_INTERRUPT_STATUS','GMU_GMU2HOST_INTR_INFO','GMU_GENERAL_7']
    slumber=['GMU_CX_GMU_POWER_COUNTER_ENABLE','GPU_GMU_CX_GMU_RPMH_POWER_STATE','GMU_SPTPRAC_PWR_CLK_STATUS']
    park=['GMU_SPTPRAC_PWR_CLK_STATUS','RSCC_TCS0_DRV0_STATUS','RSCC_TCS1_DRV0_STATUS',
          'RSCC_TCS2_DRV0_STATUS','RSCC_TCS3_DRV0_STATUS','GMU_CM3_SYSRESET','GBIF_HALT','GBIF_HALT_ACK']
    def array(name,registers):
        return 'static const uint32_t '+name+'[]={'+','.join(hex(defs['A6XX_'+r]) for r in registers)+'};\n'
    snippet='''#include <assert.h>
#include "cepheus_gmu_retire_mmio.h"
#include "cepheus_gmu_upload_mmio.h"
static uint32_t gmu[0x30000/4],gpu[0x40000/4];
static int lease(void *p){(void)p;return 0;}
static void finish(void *p,int uncertain){(void)p;(void)uncertain;}
'''+array('startup',startup)+array('slumber',slumber)+array('park',park)+array('tcm',['GMU_CM3_ITCM_START','GMU_CM3_DTCM_START'])+'''
_Static_assert(sizeof(startup)/sizeof(startup[0])==CE_GMU_REG_COUNT,"startup count");
_Static_assert(sizeof(park)/sizeof(park[0])==CE_PARK_REG_COUNT,"park count");
int main(void) {
 struct ceph_gmu_mmio m={(volatile uint8_t *)gmu,sizeof(gmu),{0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,0,lease};
 for(unsigned r=0;r<CE_GMU_REG_COUNT;++r) {
  uint32_t off,value;assert(!ceph_gmu_register_offset((enum ceph_gmu_reg)r,&off));assert(off==startup[r]*4-0x6a000);
  gmu[off/4]=0xabc00000+r;assert(!ceph_gmu_mmio_start_read(&m,(enum ceph_gmu_reg)r,&value) && value==0xabc00000+r);
 }
 for(unsigned r=0;r<3;++r) {
  gmu[slumber[r]-0x1a800]=0xdef00000+r;uint32_t v;
  assert(!ceph_gmu_mmio_slumber_read(&m,(enum ceph_slumber_reg)r,&v) && v==0xdef00000+r);
 }
 gmu[slumber[1]-0x1a800]=15;
 struct ceph_gmu_park_mmio pm={&m,gpu,sizeof(gpu),0,lease};
 for(unsigned r=0;r<CE_PARK_REG_COUNT;++r) {
  if(r<CE_PARK_GBIF_HALT)gmu[park[r]-0x1a800]=0xf000+r;else gpu[park[r]]=0xf000+r;
  uint32_t v;assert(!ceph_gmu_mmio_park_read(&pm,(enum ceph_gmu_park_reg)r,&v) && v==0xf000+r);
 }
 struct ceph_gmu_upload_mmio um={&m,0,finish,0,0};struct ceph_gmu_upload u;
 assert(!ceph_gmu_upload_mmio_bind(&um,&u));assert(u.itcm==gmu+tcm[0]-0x1a800 && u.dtcm==gmu+tcm[1]-0x1a800);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(snippet);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('Compiled GMU startup/slumber/park/TCM offsets match supplied A6xx register header: PASS')


if __name__=='__main__':main()
