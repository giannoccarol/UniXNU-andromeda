#!/usr/bin/env python3
"""Regression tests for scripts/add-ipa.py, on Mach-O files laid out here (no Apple
tools, no real apps): staging, FairPlay refusal, ad-hoc re-signing, thinning, DER
entitlements, archive escapes."""
import contextlib
import importlib.util
import io
import os
import plistlib
import stat
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('add_ipa', Path(__file__).with_name('add-ipa.py'))
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)

NOP = b'\x1f\x20\x03\xd5'


def segment(name, vmaddr, vmsize, fileoff, filesize, prot, sections=()):
    return struct.pack('<II16sQQQQIIII', a.LC_SEGMENT_64, 72 + 80 * len(sections), name.encode(),
                       vmaddr, vmsize, fileoff, filesize, prot, prot, len(sections), 0) + b''.join(sections)


def macho(filetype=a.MH_EXECUTE, cpusubtype=0, platform=a.PLATFORM_IOS, dylibs=(), cryptid=None):
    """A minimal arm64 image: __TEXT 0..16K (code at 4K), __LINKEDIT 256 bytes, unsigned."""
    base = 1 << 32 if filetype == a.MH_EXECUTE else 0
    text = NOP * 64
    sect = struct.pack('<16s16sQQIIIIIIII', b'__text', b'__TEXT', base + 0x1000, len(text), 0x1000,
                       2, 0, 0, 0x80000400, 0, 0, 0)
    cmds = [segment('__PAGEZERO', 0, base, 0, 0, 0)] if filetype == a.MH_EXECUTE else []
    cmds += [segment('__TEXT', base, 0x4000, 0, 0x4000, 5, [sect]),
             segment('__LINKEDIT', base + 0x4000, 0x4000, 0x4000, 0x100, 1),
             struct.pack('<6I', a.LC_BUILD_VERSION, 24, platform, 0x100000, 0x100000, 0)]
    for lib in dylibs:
        name = lib.encode() + b'\0'
        size = a.align(24 + len(name), 8)
        cmds.append(struct.pack('<6I', 0xc, size, 24, 2, 0x10000, 0x10000) + name.ljust(size - 24, b'\0'))
    if cryptid is not None:
        cmds.append(struct.pack('<6I', a.LC_ENCRYPTION_INFO_64, 24, 0x1000, 0x1000, cryptid, 0))
    lc = b''.join(cmds)
    img = bytearray(0x4100)
    img[:32 + len(lc)] = struct.pack('<8I', a.MH_MAGIC_64, a.CPU_TYPE_ARM64, cpusubtype, filetype,
                                     len(cmds), len(lc), 0x200085, 0) + lc
    img[0x1000:0x1000 + len(text)] = text
    img[0x4000:] = b'\x01' * 0x100
    return bytes(img)


def signed(image, ident, ents=None):
    ent = None
    if ents is not None:
        xml = plistlib.dumps(ents)
        ent = struct.pack('>II', a.CSMAGIC_EMBEDDED_ENTITLEMENTS, 8 + len(xml)) + xml
    return a.sign_adhoc(a.MachO(image, 'test'), ident, ent=ent)


def fat(arm64):
    """A fat file: a fake x86_64 slice, then the arm64 image at 16K alignment."""
    x86 = struct.pack('<8I', a.MH_MAGIC_64, 0x01000007, 3, a.MH_EXECUTE, 0, 0, 0, 0)
    off_x86, off_arm = 0x4000, 0x8000
    head = struct.pack('>II', a.FAT_MAGIC, 2) + struct.pack('>5I', 0x01000007, 3, off_x86, len(x86), 14) + \
        struct.pack('>5I', a.CPU_TYPE_ARM64, 0, off_arm, len(arm64), 14)
    return head.ljust(off_x86, b'\0') + x86.ljust(off_arm - off_x86, b'\0') + arm64


def info(bid, exe, **extra):
    d = dict(CFBundleIdentifier=bid, CFBundleExecutable=exe, CFBundleShortVersionString='1.0',
             MinimumOSVersion='16.0', UIDeviceFamily=[1], CFBundleSupportedPlatforms=['iPhoneOS'])
    d.update(extra)
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY)


def ipa(path, files, links=()):
    """files: {path inside Payload/Test.app: bytes}; links: [(path, target)]."""
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('iTunesMetadata.plist', b'personal purchase data')
        for name, data in files.items():
            zi = zipfile.ZipInfo(f'Payload/Test.app/{name}' if not name.startswith('/') else name[1:])
            zi.external_attr = (0o100755 if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe') else 0o100644) << 16
            z.writestr(zi, data)
        for name, target in links:
            zi = zipfile.ZipInfo(f'Payload/Test.app/{name}')
            zi.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(zi, target)


def app_files(main, **extra):
    files = {'Info.plist': info('org.test.app', 'Test', **extra), 'Test': main}
    return files


class AddIpa(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.dest = self.dir / 'apps'

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = a.main(['--dest', str(self.dest), *map(str, args)])
        return rc, out.getvalue()

    def stage(self, files, *flags, links=()):
        path = self.dir / 'test.ipa'
        ipa(path, files, links)
        return self.run_main(*flags, path)

    def code_directory(self, path):
        m = a.MachO(Path(path).read_bytes(), str(path))
        problem, sig = a.check_signature(m)
        return problem, sig, a.signature_blobs(m)

    def test_unsigned_app_is_signed_with_its_bundle_id(self):
        files = app_files(macho())
        files['SC_Info/Test.sinf'] = b'fairplay'
        files['Watch/W.app/W'] = b'\xcf\xfa\xed\xfe' + bytes(28)
        rc, out = self.stage(files)
        self.assertEqual(rc, 0, out)
        app = self.dest / 'Test.app'
        problem, sig, blobs = self.code_directory(app / 'Test')
        self.assertIsNone(problem)
        self.assertEqual(sig['identifier'], 'org.test.app')
        self.assertFalse((app / 'SC_Info').exists())
        self.assertFalse((app / 'Watch').exists())
        self.assertFalse(any(p.name == 'iTunesMetadata.plist' for p in self.dest.rglob('*')))
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()), ['Test.app'])   # no work dir left
        self.assertTrue(os.stat(app / 'Test').st_mode & 0o111)
        rc, out = self.run_main('--verify', app)
        self.assertEqual(rc, 0, out)

    def test_valid_signature_is_kept(self):
        main = signed(macho(), 'org.test.app')
        rc, out = self.stage(app_files(main))
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.dest / 'Test.app/Test').read_bytes(), main)

    def test_tampered_page_is_resigned_or_refused(self):
        main = bytearray(signed(macho(), 'org.test.app'))
        main[0x1000] ^= 0xff
        rc, out = self.stage(app_files(bytes(main)), '--keep-signature')
        self.assertEqual(rc, 1)
        self.assertIn('does not match its hash', out)
        self.assertFalse((self.dest / 'Test.app').exists())
        rc, out = self.stage(app_files(bytes(main)))
        self.assertEqual(rc, 0, out)
        self.assertIsNone(self.code_directory(self.dest / 'Test.app/Test')[0])
        rc, out = self.run_main('--verify', self.dest / 'Test.app')
        self.assertEqual(rc, 0, out)

    def test_fairplay_encrypted_code_is_refused(self):
        rc, out = self.stage(app_files(macho(cryptid=1)))
        self.assertEqual(rc, 1)
        self.assertIn('encrypted with FairPlay (cryptid 1)', out)
        self.assertFalse(self.dest.exists() and any(self.dest.iterdir()))
        files = app_files(macho())
        files['Frameworks/F.framework/F'] = macho(a.MH_DYLIB, cryptid=1)
        files['Frameworks/F.framework/Info.plist'] = info('org.test.F', 'F')
        rc, out = self.stage(files, '--drop-encrypted-plugins')
        self.assertEqual(rc, 1, 'an encrypted framework is never dropped')
        rc, out = self.stage(app_files(macho(cryptid=0)))
        self.assertEqual(rc, 0, out)

    def test_encrypted_extension_dropped_on_request(self):
        files = app_files(macho())
        files['PlugIns/E.appex/E'] = macho(cryptid=1)
        files['PlugIns/E.appex/Info.plist'] = info('org.test.app.e', 'E')
        rc, out = self.stage(files)
        self.assertEqual(rc, 1)
        self.assertIn('--drop-encrypted-plugins', out)
        rc, out = self.stage(files, '--drop-encrypted-plugins')
        self.assertEqual(rc, 0, out)
        self.assertFalse((self.dest / 'Test.app/PlugIns/E.appex').exists())

    def test_fat_file_is_thinned_to_arm64(self):
        rc, out = self.stage(app_files(fat(signed(macho(), 'org.test.app'))))
        self.assertEqual(rc, 0, out)
        data = (self.dest / 'Test.app/Test').read_bytes()
        self.assertEqual(struct.unpack_from('<II', data, 0), (a.MH_MAGIC_64, a.CPU_TYPE_ARM64))
        self.assertIsNone(self.code_directory(self.dest / 'Test.app/Test')[0])

    def test_wrong_architecture_or_platform_is_refused(self):
        for image, why in ((macho(cpusubtype=a.CPU_SUBTYPE_ARM64E), 'arm64e only'),
                           (macho(platform=7), 'iOS simulator')):
            rc, out = self.stage(app_files(image))
            self.assertEqual(rc, 1)
            self.assertIn(why, out)
        rc, out = self.stage(app_files(macho(), UIDeviceFamily=[2]))
        self.assertIn('iPad-only', out)
        rc, out = self.stage(app_files(macho(), MinimumOSVersion='17.0'))
        self.assertIn('needs iOS 17.0', out)

    def test_entitlements_kept_and_der_added(self):
        ents = {'application-identifier': 'T.org.test.app', 'get-task-allow': True,
                'keychain-access-groups': ['T.org.test.app'], 'n': 300}
        rc, out = self.stage(app_files(signed(macho(), 'Test', ents)))   # wrong identifier: re-signed
        self.assertEqual(rc, 0, out)
        problem, sig, blobs = self.code_directory(self.dest / 'Test.app/Test')
        self.assertIsNone(problem)
        self.assertEqual(sig['identifier'], 'org.test.app')
        self.assertEqual(plistlib.loads(blobs[a.CSSLOT_ENTITLEMENTS][8:]), ents)
        der = blobs[a.CSSLOT_DER_ENTITLEMENTS]
        self.assertEqual(der[8:9], b'\x70')                               # [APPLICATION 16]
        self.assertIn(a.der(0x0c, b'application-identifier'), der)

    def test_rpath_libraries_warn(self):
        rc, out = self.stage(app_files(macho(dylibs=['@rpath/F.framework/F'])))
        self.assertEqual(rc, 0, out)
        self.assertIn('ios-flame patch 0114', out)

    def test_archive_escapes_are_refused(self):
        rc, out = self.stage({**app_files(macho()), '/Payload/Test.app/../../evil': b'x'})
        self.assertEqual(rc, 1)
        self.assertIn('escapes the archive', out)
        for target in ('../../../../etc/passwd', '/etc/passwd'):
            rc, out = self.stage(app_files(macho()), links=[('out', target)])
            self.assertEqual(rc, 1)
            self.assertIn('leaves the bundle', out)
        rc, out = self.stage(app_files(macho()), links=[('ok', 'Info.plist')])
        self.assertEqual(rc, 0, out)
        self.assertTrue((self.dest / 'Test.app/ok').is_symlink())

    def test_list_and_remove(self):
        self.stage(app_files(macho()))
        rc, out = self.run_main('--list')
        self.assertIn('org.test.app', out)
        rc, out = self.run_main('--remove', 'org.test.app')
        self.assertEqual(rc, 0)
        self.assertFalse((self.dest / 'Test.app').exists())


if __name__ == '__main__':
    unittest.main()
