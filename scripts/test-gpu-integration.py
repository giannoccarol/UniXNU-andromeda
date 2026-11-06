#!/usr/bin/env python3
"""Portable GPU source/staging integration checks, not XNU ABI or runtime tests."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Integration(unittest.TestCase):
    def test_real_config_sources_when_supplied(self):
        # Optional explicit reference; synthetic coverage is not presented as
        # real source application. CI verifies the actual applied build tree.
        import os
        source = os.environ.get('CEPHEUS_XNU_REFERENCE')
        if not source:
            self.skipTest('set CEPHEUS_XNU_REFERENCE for actual pre-GPU conf files')
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            subprocess.run(['git', 'init', '-q', str(tree)], check=True)
            for name in ('bsd/conf/files', 'iokit/conf/files.arm64'):
                (tree / name).parent.mkdir(parents=True, exist_ok=True)
                (tree / name).write_bytes((Path(source) / name).read_bytes())
            conf = (tree / 'bsd/conf/files').read_text()
            patches = []
            if 'bsd/dev/cepheus_touch.c' not in conf: patches.append('0082-cepheus-touch.patch')
            if 'bsd/dev/cepheus_soc.c' not in conf: patches.append('0085-cepheus-soc.patch')
            for patch in patches + ['0087-cepheus-gpu-memory.patch']:
                subprocess.run(['git', '-C', str(tree), 'apply', str(ROOT / 'xnu' / patch)], check=True)

    def test_headers_stage_before_patch_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            (tree / 'bsd/conf').mkdir(parents=True)
            (tree / 'bsd/dev').mkdir()
            (tree / 'bsd/conf/files').write_text('base\n')
            subprocess.run(['bash', str(ROOT / 'xnu/0087-stage-cepheus-gpu-memory.sh')], cwd=tree, check=True)
            for where, name in [('bsd/dev', 'cepheus_gpu_compile.c'), ('bsd/dev', 'cepheus_hfi_boot.h'),
                                ('bsd/dev', 'cepheus_smmu_install.h'), ('iokit/Kernel/arm', 'cepheus_gpu_memory.cpp')]:
                self.assertEqual((tree / where / name).read_bytes(), (ROOT / 'xnu' / name).read_bytes())
            self.assertFalse((tree / 'bsd/dev/sweet_rpmh.c').exists())

    def test_patch_places_each_source_once(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            subprocess.run(['git', 'init', '-q', str(tree)], check=True)
            for name, text in {
                'bsd/conf/files': 'bsd/dev/sweet_sample.c\t\t\tstandard\nbsd/dev/sweet_keys.c\t\t\tstandard\n'
                    'bsd/dev/cepheus_touch.c\t\t\tstandard\nbsd/dev/cepheus_soc.c\t\t\tstandard\n'
                    'bsd/dev/sweet_rpmh.c\t\t\tstandard\nbsd/dev/sweet_spmi.c\t\t\tstandard\n',
                'iokit/conf/files.arm64': 'iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
                    'iokit/Kernel/arm/SweetSurface.cpp                                optional iokitcpp\n'
                    'iokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp\n'
                    'iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n'
                    'iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp\n',
            }.items():
                path = tree / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            subprocess.run(['git', '-C', str(tree), 'apply', str(ROOT / 'xnu/0087-cepheus-gpu-memory.patch')], check=True)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_gpu_compile.c'), 1)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_gmu_workspace.c'), 1)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_gpu_retire.c'), 1)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_gmu_hfi_link.c'), 1)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_gmu_bringup.c'), 1)
            self.assertEqual((tree / 'bsd/conf/files').read_text().count('cepheus_cp_workspace.c'), 1)
            self.assertEqual((tree / 'iokit/conf/files.arm64').read_text().count('cepheus_gpu_memory.cpp'), 1)

    def test_memory_uses_explicit_nc_mapping(self):
        source = (ROOT / 'xnu/cepheus_gpu_memory.cpp').read_text()
        self.assertIn('createMappingInTask(kernel_task,0,', source)
        self.assertIn('kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache', source)
        self.assertIn('b->mapping->getVirtualAddress()', source)
        self.assertNotIn('b->memory->getBytesNoCopy()', source)
        self.assertIn('IODefaultCacheBits(b->pa+offset)&VM_WIMG_MASK', source)
        self.assertIn('if(attrs!=VM_WIMG_WCOMB)', source)

    def test_pa_validation_precedes_cache_transition(self):
        source = (ROOT / 'xnu/cepheus_gpu_memory.cpp').read_text()
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
        allocation = source.split('b->memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask', 1)[1].split(';', 1)[0]
        self.assertNotIn('kIOMapWriteCombineCache', allocation)
        self.assertNotIn('kIOMemoryPersistent', allocation)
        self.assertLess(source.index('cepheus_gpu_pa_window_mask('), source.index('inTaskWithPhysicalMask('))
        self.assertLess(source.index('cepheus_gpu_pa_range_managed('), source.index('createMappingInTask('))
        self.assertNotIn('0x000000fffffff000', source)

    def test_wrappers_compile_without_runtime_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-c',
                            str(ROOT / 'xnu/cepheus_gpu_compile.c'), '-o', str(Path(directory) / 'gpu.o')], check=True)

    def test_boot_sources_do_not_invoke_gpu_engines(self):
        # A build can accidentally retain all symbols AND introduce a startup
        # call. Keep this separate check on actual boot-facing source bodies.
        dangerous=('cepheus_gpu_scm_probe_once','cepheus_gmu_bringup_start','ceph_power_start','cepheus_gpu_power_start',
                   'ceph_qmp_connect_cold','ceph_qmp_send_acd','ceph_gx_core_configure',
                   'cepheus_gpu_gx_core_configure','ceph_cp_ring_configure_halted',
                   'cepheus_gpu_submit','ceph_smmu_install_context')
        for name in ('cepheus_soc.c','cepheus_touch.c','cepheus_arc_probe.h'):
            text=(ROOT/'xnu'/name).read_text()
            text=re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
            for call in dangerous:self.assertNotRegex(text,rf'\b{call}\s*\(',name)
        source=(ROOT/'xnu/cepheus_gpu_compile.c').read_text()
        body=source.split('int cepheus_gpu_config_probe',1)[1].split('int cepheus_gpu_qmp_connect',1)[0]
        for call in dangerous:self.assertNotRegex(body,rf'\b{call}\s*\(')

    def test_no_security_bypass_in_concrete_cp_adapters(self):
        for name in ('cepheus_cp_ring_config.h','cepheus_cp_protection.h',
                     'cepheus_cp_snapshot_mmio.h','cepheus_gx_core.h'):
            source=(ROOT/'xnu'/name).read_text()
            # Comments explicitly name forbidden registers; check executable
            # code, not the presence of those cautionary words.
            source=re.sub(r'/\*.*?\*/|//[^\n]*','',source,flags=re.S)
            self.assertNotRegex(source,r'gpu\[0x(?:808|807)\]\s*=(?!=)',name)
            self.assertNotIn('TRUST_CONTROL',source)


if __name__ == '__main__':
    unittest.main()
