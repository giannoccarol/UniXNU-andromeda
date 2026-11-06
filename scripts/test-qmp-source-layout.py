#!/usr/bin/env python3
"""Compile real downstream QMP descriptor definitions against our decoder."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('qmp_source',type=Path);a=p.parse_args()
    source=a.qmp_source.read_text();definitions=[]
    for name in ('channel_desc','mbox_desc'):
        m=re.search(r'struct '+name+r'\s*\{[^{}]*\};',source)
        if not m:raise ValueError('missing '+name)
        definitions.append(m[0])
    constants=dict(re.findall(r'^#define\s+(QMP_\w+)\s+(0[xX][0-9a-fA-F]+)',source,re.M))
    for k,v in {'QMP_MAGIC':0x4d41494c,'QMP_VERSION':1,'QMP_FEATURES':0,
                'QMP_MBOX_LINK_UP':0xffff,'QMP_MBOX_CH_CONNECTED':0xffff}.items():
        if int(constants[k],0)!=v:raise ValueError('source constant changed '+k)
    snippet='''#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include "cepheus_qmp_mmio.h"
typedef uint32_t u32;
'''+ '\n'.join(definitions)+'''
_Static_assert(sizeof(struct channel_desc)==24,"core descriptor");
_Static_assert(sizeof(struct mbox_desc)==64,"full descriptor");
_Static_assert(offsetof(struct mbox_desc,ucore)==12,"ucore");
_Static_assert(offsetof(struct mbox_desc,mcore)==36,"mcore");
_Static_assert(offsetof(struct mbox_desc,reserved)==60,"reserved");
_Static_assert(offsetof(struct mbox_desc,ucore)+offsetof(struct channel_desc,link_state_ack)==16,"host ucore link ACK");
_Static_assert(offsetof(struct mbox_desc,ucore)+offsetof(struct channel_desc,ch_state_ack)==24,"host ucore channel ACK");
_Static_assert(offsetof(struct mbox_desc,mcore)+offsetof(struct channel_desc,link_state)==36,"host mcore link");
_Static_assert(offsetof(struct mbox_desc,mcore)+offsetof(struct channel_desc,ch_state)==44,"host mcore channel");
int main(void) {
 struct mbox_desc d={.magic=0x4d41494c,.version=1,
  .ucore={0xffff,0xffff,0xffff,0xffff,64,64},.mcore={0xffff,0xffff,0xffff,0xffff,128,128}};
 struct ceph_qmp_descriptor words;memcpy(&words,&d,64);struct ceph_qmp_connection c;
 assert(!ceph_qmp_descriptor_decode(&words,&c));assert(c.tx.tx_offset==128 && c.tx.tx_bytes==128);
 assert(c.rx.tx_offset==64 && c.rx.tx_bytes==64);
}
'''
    with tempfile.TemporaryDirectory() as directory:
        c=Path(directory)/'test.c';c.write_text(snippet);exe=Path(directory)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                        '-I',str(ROOT/'xnu'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('QMP descriptor size/offsets/magic/states match supplied downstream source: PASS')


if __name__=='__main__':main()
