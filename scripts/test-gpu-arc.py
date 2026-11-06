#!/usr/bin/env python3
"""Apply-check ARC include after the real touch-power transformation, privately."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("touch_power", ROOT / "scripts/prepare-touch-power-patch.py")
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)
PATCHES = Path(sys.argv[1]) if len(sys.argv) > 1 else None


class ArcIntegrationTests(unittest.TestCase):
    def test_stage_before_source_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            (tree / "bsd/conf").mkdir(parents=True)
            (tree / "bsd/dev").mkdir()
            (tree / "bsd/conf/files").write_text("base XNU inventory\n")
            subprocess.run(["bash", str(ROOT / "xnu/0086-stage-cepheus-gpu-arc.sh")],
                           cwd=tree, check=True)
            for name in ("cepheus_arc.h", "cepheus_arc_probe.h"):
                self.assertEqual((tree / "bsd/dev" / name).read_bytes(), (ROOT / "xnu" / name).read_bytes())
            self.assertFalse((tree / "bsd/dev/sweet_rpmh.c").exists())

    def test_real_patch_after_touch_enable_only(self):
        self.assertIsNotNone(PATCHES, "supply pinned ios-sweet/xnu/patches")
        self.assertTrue(PATCHES.is_dir())
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            subprocess.run(["git", "init", "-q", str(tree)], check=True)
            target = "bsd/dev/sweet_rpmh.c"
            for patch in sorted(PATCHES.glob("*.patch")):
                if patch.name == "0086-cepheus-gpu-arc.patch":
                    continue
                subprocess.run(["git", "-C", str(tree), "apply", "--whitespace=nowarn",
                                f"--include={target}", str(patch.resolve())], check=True)
            file = tree / target
            before = power.transform(file.read_text())
            file.write_text(before)
            subprocess.run(["git", "-C", str(tree), "apply",
                            str(ROOT / "xnu/0086-cepheus-gpu-arc.patch")], check=True)
            after = file.read_text()
            self.assertEqual(after.count('#include "cepheus_arc_probe.h"'), 1)
            self.assertEqual(after.replace('#include "cepheus_arc_probe.h"\n', ''), before)
            self.assertTrue(after.rstrip().endswith('#include "cepheus_arc_probe.h"\n#endif /* ARM64_BOARD_CONFIG_SWEET */'))

    def test_probe_has_no_transport_write_or_send_calls(self):
        probe = (ROOT / "xnu/cepheus_arc_probe.h").read_text()
        for name in ("rh_write", "rh_sync", "rh_send"):
            self.assertNotRegex(probe, rf"\b{name}\s*\(")
        self.assertIn("__atomic_exchange_n(&rh_lock", probe)
        self.assertIn("__atomic_store_n(&rh_lock, 0", probe)
        self.assertIn("current ARC vote UNKNOWN", probe)
        self.assertIn("ceph_arc_lookup_address", probe)
        self.assertIn("cepheus_gpu_fuse_sample(&fuse)", probe)
        self.assertIn("cepheus_gpu_config_probe(&db", probe)
        dryrun=(ROOT / 'xnu/cepheus_gpu_compile.c').read_text().split('int cepheus_gpu_config_probe',1)[1].split('int cepheus_gpu_qmp_connect',1)[0]
        for name in ('ceph_hfi_exchange','ceph_power_start','ceph_gmu_start_cold','ceph_qmp_send_acd'):
            self.assertNotIn(name+'(',dryrun)
        self.assertIn('NOT SENT',dryrun)

    def test_speed_bin_probe_is_optin_read_only(self):
        source = (ROOT / "xnu/cepheus_soc.c").read_text()
        body = source.split('static void gpu_fuse_probe(void)', 1)[1].split('static void soc_thread', 1)[0]
        self.assertIn('ml_io_map(0x00784000,0x1000)', body)
        self.assertIn('first=sr(efuse,0x130),second=sr(efuse,0x130)', body)
        self.assertNotIn('osm_request(', body)
        self.assertNotRegex(body, r'\*\s*\(volatile[^\n]+\)\s*=')
        self.assertIn('if(fuse==1) {gpu_fuse_probe();}', source)
        sample=source.split('int cepheus_gpu_fuse_sample(uint32_t *value)',1)[1].split('void cepheus_cpu_activity',1)[0]
        self.assertIn('__atomic_load_n(&gpu_fuse_valid',sample)
        self.assertNotIn('ml_io_map(',sample)
        self.assertNotIn('sr(',sample)
        self.assertIn('TSENS0:15 TSENS1:11',source)
        self.assertIn('ml_io_map(0x0c265000,0x1000)',source)

    def test_generated_0084_does_not_depend_on_later_arc_include(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            patches = base / "patches"
            patches.mkdir()
            for patch in PATCHES.glob("*.patch"):
                if patch.name != "0086-cepheus-gpu-arc.patch":
                    shutil.copyfile(patch, patches / patch.name)
            shutil.copyfile(ROOT / "xnu/0086-cepheus-gpu-arc.patch", patches / "0086-cepheus-gpu-arc.patch")
            generated = base / "0084.patch"
            subprocess.run([sys.executable, str(ROOT / "scripts/prepare-touch-power-patch.py"),
                            str(patches), str(generated)], check=True)
            self.assertNotIn("cepheus_arc_probe.h", generated.read_text())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
