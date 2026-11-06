#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import struct
import unittest

spec=importlib.util.spec_from_file_location('zap',Path(__file__).with_name('inspect-zap-layout.py'))
zap=importlib.util.module_from_spec(spec);spec.loader.exec_module(zap)


def fixture():
    data=bytearray(0x3040);data[:7]=b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH',data,16,2,164,1,0x5000,52,0,3,52,32,3,0,0,0)
    struct.pack_into('<8I',data,52,0,0,0,0,148,0,0x7000000,0)
    struct.pack_into('<8I',data,84,0,0x1000,0x6000,0x6000,16,4096,0x2200000,4096)
    struct.pack_into('<8I',data,116,1,0x3000,0x5000,0x5000,64,64,0x8000007,0x100000)
    return data


class LayoutTests(unittest.TestCase):
    def test_relocatable_layout(self):
        r=zap.parse(fixture());self.assertEqual(r['allocation_bytes'],0x100000)
        self.assertEqual(r['logical_mem_setup_bytes'],4096);self.assertEqual(r['entry_offset'],0)
        self.assertEqual(r['metadata_header_bytes'],148)

    def test_malformed_refused(self):
        for offset,fmt,value in [(18,'H',183),(28,'I',0),(42,'H',16),(44,'H',33),(24,'I',0x9000),
                                 (116+24,'I',7),(116+16,'I',65),(116+28,'I',3),(84+24,'I',0)]:
            with self.subTest(offset=offset),self.assertRaises(ValueError):
                data=fixture();struct.pack_into('<'+fmt,data,offset,value);zap.parse(data)
        for length in (0,51,147,0x303f):
            with self.assertRaises(ValueError):zap.parse(fixture()[:length])

    def test_local_artifact_when_supplied(self):
        p=os.environ.get('CEPHEUS_ZAP_ELF')
        if not p:self.skipTest('supply CEPHEUS_ZAP_ELF for actual split-file equality')
        r=zap.inspect(Path(p))
        self.assertEqual(r['elf_sha256'],'74a948e84f0aac830b314ed05d26006b3571c9a3f90600c708ca76c25612660d')
        self.assertEqual(r['allocation_bytes'],0x100000);self.assertEqual(r['logical_mem_setup_bytes'],0x1000)
        self.assertEqual(r['segments'][2]['filesz'],1968);self.assertEqual(r['mdt_bytes'],6860)


if __name__=='__main__':unittest.main()
