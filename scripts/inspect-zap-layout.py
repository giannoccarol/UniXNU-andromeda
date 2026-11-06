#!/usr/bin/env python3
"""Validate A640 ZAP ELF/MDT/split payload layout. Offline, NO PAS/SCM calls.

This is container metadata only, not DSP decompilation or signature validation.
Exact local files are identified by digest; output has no proprietary payload.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def parse(elf):
    if len(elf)<52 or len(elf)>2*1024*1024 or elf[:7]!=b'\x7fELF\x01\x01\x01':
        raise ValueError('expected bounded ELF32 little-endian file')
    kind,machine,version,entry,phoff,_,flags,ehsize,entsize,count,_,_,_=struct.unpack_from('<HHIIIIIHHHHHH',elf,16)
    if kind!=2 or machine!=164 or version!=1 or ehsize!=52 or entsize!=32 or phoff!=52 or not 1<=count<=32:
        raise ValueError('unsupported PIL ELF geometry or architecture')
    metadata_bytes=phoff+count*entsize
    if metadata_bytes>len(elf):raise ValueError('truncated program headers')
    segments=[];loads=[];hashes=[];min_pa=1<<32;max_pa=0;alignment=0
    for i in range(count):
        typ,off,va,pa,filesz,memsz,flg,align=struct.unpack_from('<8I',elf,phoff+i*entsize)
        if off>len(elf) or filesz>len(elf)-off or pa+memsz>1<<32 or (align and align&(align-1)):
            raise ValueError('invalid segment range/alignment')
        hashed=((flg>>24)&7)==2;reloc=bool(flg&(1<<27))
        item={'index':i,'type':typ,'offset':off,'va':va,'pa':pa,'filesz':filesz,'memsz':memsz,'flags':flg,'alignment':align,
              'hash_segment':hashed,'relocatable':reloc,'sha256':hashlib.sha256(elf[off:off+filesz]).hexdigest()}
        segments.append(item)
        if hashed:hashes.append(item)
        elif typ==1 and memsz:
            if not reloc or filesz>memsz:raise ValueError('fixed-address or malformed load refused')
            for prev in loads:
                if pa<prev['pa']+prev['memsz'] and prev['pa']<pa+memsz:raise ValueError('overlapping load segments')
            loads.append(item)
            if pa<min_pa:min_pa=pa;alignment=align
            max_pa=max(max_pa,pa+memsz)
    if not loads or len(hashes)!=1 or not hashes[0]['filesz']:
        raise ValueError('missing load/hash metadata')
    if not any(s['pa']<=entry<s['pa']+s['memsz'] for s in loads):raise ValueError('entry outside loaded image')
    logical=((max_pa+4095)&~4095)-min_pa
    unit=4*1024*1024 if alignment>=4*1024*1024 else (1024*1024 if alignment>=1024*1024 else 4096)
    allocated=(logical+unit-1)&~(unit-1)
    if not logical or allocated>4*1024*1024:raise ValueError('PIL allocation outside diagnostic bound')
    return {'machine':machine,'entry':entry,'flags':flags,'metadata_header_bytes':metadata_bytes,'segments':segments,
            'relocation_base':min_pa,'logical_mem_setup_bytes':logical,'allocation_bytes':allocated,
            'entry_offset':entry-min_pa,'pas_id':13,
            'limitations':['no signature authentication','no executable decompilation','no secure-memory allocation or SCM calls',
                           'PIL allocation policy derived from local peripheral-loader.c; XNU backend absent']}


def inspect(path):
    elf=path.read_bytes();r=parse(elf)
    r['elf_sha256']=hashlib.sha256(elf).hexdigest();r['elf_bytes']=len(elf)
    for s in r['segments']:
        split=path.with_suffix(f".b{s['index']:02d}").read_bytes()
        if split!=elf[s['offset']:s['offset']+s['filesz']]:raise ValueError('split segment differs from ELF')
    h=next(s for s in r['segments'] if s['hash_segment'])
    expected=elf[:r['metadata_header_bytes']]+elf[h['offset']:h['offset']+h['filesz']]
    mdt=path.with_suffix('.mdt').read_bytes()
    if mdt!=expected:raise ValueError('MDT not exact ELF headers + hash segment')
    r['mdt_sha256']=hashlib.sha256(mdt).hexdigest();r['mdt_bytes']=len(mdt);r['split_files_match']=True
    return r


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);a=p.parse_args()
    print(json.dumps(inspect(a.elf),indent=2))


if __name__=='__main__':main()
