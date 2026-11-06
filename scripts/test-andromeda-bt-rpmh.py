#!/usr/bin/env python3
"""Apply-check patch 0092's sweet_rpmh.c part on the file as the build has it.

sweet_rpmh.c is created by ios-sweet 0062, rewritten by the generated 0084
(prepare-touch-power-patch.py) and extended by 0086. 0092-andromeda-bt-rpmh.patch
must apply after all three, so it is NOT installed into ios-sweet/xnu/patches
(the 0084 generator and test-touch-power.py apply everything there to the
pre-0084 file); the workflow copies it into the build's patch directory.

Usage: test-andromeda-bt-rpmh.py IOS_SWEET_PATCH_DIR
"""
import importlib.util
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'bsd/dev/sweet_rpmh.c'
spec = importlib.util.spec_from_file_location('touch_power', ROOT / 'scripts/prepare-touch-power-patch.py')
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)
PATCHES = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
RPMH = ROOT / 'xnu/0092-andromeda-bt-rpmh.patch'


def build_tree(tree):
    subprocess.run(['git', 'init', '-q', str(tree)], check=True)
    for patch in sorted(PATCHES.glob('*.patch')):
        if patch.name.startswith(('0086-', '0092-', '0093-')):
            continue
        subprocess.run(['git', '-C', str(tree), 'apply', '--whitespace=nowarn',
                        f'--include={TARGET}', str(patch)], check=True)
    file = tree / TARGET
    file.write_text(power.transform(file.read_text()))
    subprocess.run(['git', '-C', str(tree), 'apply', str(ROOT / 'xnu/0086-cepheus-gpu-arc.patch')], check=True)
    return file


class BtRpmh(unittest.TestCase):
    def test_not_in_upstream_patch_dir(self):
        self.assertIsNotNone(PATCHES, 'supply ios-sweet/xnu/patches')
        self.assertFalse((PATCHES / RPMH.name).exists(),
                         'the 0084 generator would apply it to the pre-0084 file')

    def test_applies_after_0084_and_0086(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            file = build_tree(tree)
            before = file.read_text()
            subprocess.run(['git', '-C', str(tree), 'apply', str(RPMH)], check=True)
            after = file.read_text()
            self.assertNotEqual(before, after)
            # Mi 9 and ios-sweet paths keep their requests.
            self.assertIn('rh_send(address, 0, true)', after)
            self.assertIn('rh_send(address, mv, false)', after)
            self.assertIn('const uint32_t values[3] = {xob ? 1u : mv, 7, 1};', after)
            self.assertIn('return rh_send_cmds(address, xob ? 1u : 3u, offsets, values);', after)
            self.assertIn('if (ncmd == 0 || ncmd > 3 || !offsets || !values) {', after)
            # Exactly the three DT voltages of bt_wcn3990, nothing else.
            body = after.split('andromeda_rpmh_bt_rail(const char *res, bool on)\n{', 1)[1]
            body = body.split('\n}\n', 1)[0]
            for rail in ('{"ldoa7", 1800}', '{"ldoc2", 1304}', '{"ldoc11", 3312}'):
                self.assertIn(rail, body)
            self.assertEqual(len(re.findall(r'\{"[a-z0-9]+", [0-9]+\}', body)), 3)
            self.assertIn('on_values[3] = {mv, 7, 1}', body)
            self.assertIn('off_offsets[1] = {4}, off_values[1] = {0}', body)
            self.assertIn('__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)', body)
            self.assertTrue(after.rstrip().endswith(
                '#include "cepheus_arc_probe.h"\n#endif /* ARM64_BOARD_CONFIG_SWEET */'))

    def test_stage_installs_only_the_driver(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            (tree / 'bsd/conf').mkdir(parents=True)
            (tree / 'bsd/dev').mkdir()
            (tree / 'bsd/conf/files').write_text('base\n')
            subprocess.run(['bash', str(ROOT / 'xnu/0092-stage-andromeda-bt.sh')], cwd=tree, check=True)
            self.assertEqual((tree / 'bsd/dev/andromeda_bt.c').read_bytes(),
                             (ROOT / 'xnu/andromeda_bt.c').read_bytes())
            self.assertEqual(sorted(p.name for p in (tree / 'bsd/dev').iterdir()), ['andromeda_bt.c'])


if __name__ == '__main__':
    unittest.main()
