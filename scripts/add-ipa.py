#!/usr/bin/env python3
"""Stage the owner's own unencrypted iOS apps for the andromeda root build.

  scripts/add-ipa.py [--dest DIR] [--resign | --keep-signature]
                     [--drop-encrypted-plugins] IPA_OR_APP...
  scripts/add-ipa.py [--dest DIR] --list
  scripts/add-ipa.py [--dest DIR] --remove BUNDLE_ID_OR_NAME...
  scripts/add-ipa.py --verify APP...

Each .ipa (Payload/NAME.app inside a zip) or .app directory (an Xcode device
build) is checked, prepared and staged as DIR/NAME.app. DIR defaults to
<repo>/inputs/<board>/apps (git-ignored); phone/andromeda-apps/install-root.sh
copies the staged bundles into /Applications of the iOS root that
scripts/rebuild-root.sh builds. Nothing is uploaded anywhere: the apps stay on
this machine and in the local root image.

Refused (nothing staged):
  - FairPlay-encrypted code: any arm64 Mach-O with LC_ENCRYPTION_INFO(_64)
    cryptid != 0. The kernel only loads it through Apple's FairPlay decrypter,
    which does not exist on this phone (docs/research/app-store.md, section 3).
    With --drop-encrypted-plugins, encrypted app extensions (PlugIns/,
    Extensions/, AppClips/) are removed instead and the app is staged without
    them; an encrypted main executable or framework is always refused.
  - code without an arm64 slice (armv7, arm64e only: the iPhone X userland on
    the SM8150 runs arm64 without pointer authentication), simulator or
    macOS builds, iPad-only apps, apps that need a newer iOS than 16.7.16.

Prepared:
  - fat Mach-O files are thinned to their arm64 slice;
  - FairPlay leftovers (SC_Info/) and the watchOS app (Watch/) are removed;
  - signatures: our kernel accepts any code directory hash (patch 0096, the
    AMFI stand-in from ios-flame), but every page must still match its hash,
    and decrypted binaries no longer do. By default (auto) a Mach-O is
    re-signed ad hoc when its signature is missing or does not match, and a
    bundle's main executable also when its identifier is not the bundle id
    (LaunchServices then ignores its entitlements, ios-flame HANDOFF.md).
    The original entitlements (XML and DER blobs) are kept as they are; DER
    is generated from the XML when the original had none. --resign re-signs
    every Mach-O, --keep-signature never re-signs (and refuses broken ones).
    The ad-hoc code directory follows tools/mkinit.py of ios-sweet
    (dipa-studio, MIT): version 0x20400, SHA-256, 4 KiB pages, CS_ADHOC.

Warnings (staged anyway): libraries loaded through @rpath/@executable_path/
@loader_path (dyld needs an AMFI dyld policy our kernel does not have yet:
ios-flame patch 0114), Metal (no GPU driver yet), Apple services in the
entitlements (push, iCloud: not available).

--verify checks staged bundles without changing them (used by
phone/andromeda-apps/install-root.sh) and exits 1 if one would not run.
"""
import argparse
import hashlib
import os
import plistlib
import shutil
import stat
import struct
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

REPO = Path(__file__).resolve().parent.parent
IOS_VERSION = (16, 7, 16)                 # the 20H392 userland
MAX_APP_BYTES = 8 << 30                   # the root image is 16 GiB
TAG = 'add-ipa'

# Mach-O (mach-o/loader.h, mach-o/fat.h, mach/machine.h)
MH_MAGIC, MH_MAGIC_64 = 0xfeedface, 0xfeedfacf
FAT_MAGIC, FAT_MAGIC_64 = 0xcafebabe, 0xcafebabf
CPU_TYPE_ARM64 = 0x0100000c
CPU_SUBTYPE_MASK = 0xff000000
CPU_SUBTYPE_ARM64E = 2
ARM64_SUBTYPES = (0, 1)                   # ARM64_ALL, ARM64_V8
MH_EXECUTE, MH_DYLIB, MH_BUNDLE = 2, 6, 8
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1d
LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64 = 0x21, 0x2c
LC_VERSION_MIN_IPHONEOS = 0x25
LC_BUILD_VERSION = 0x32
DYLIB_LOADS = (0xc, 0x20, 0x80000018, 0x8000001f, 0x80000023)  # load, lazy, weak, reexport, upward
PLATFORM_IOS = 2
PLATFORMS = {1: 'macOS', 2: 'iOS', 3: 'tvOS', 4: 'watchOS', 5: 'bridgeOS', 6: 'Mac Catalyst',
             7: 'iOS simulator', 8: 'tvOS simulator', 9: 'watchOS simulator', 10: 'DriverKit',
             11: 'visionOS', 12: 'visionOS simulator'}
SEGMENT_ALIGN = 0x4000                    # arm64 iOS segments are 16 KiB aligned

# Code signing (xnu bsd/sys/codesign.h, osfmk/kern/cs_blobs.h)
CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02
CSMAGIC_REQUIREMENTS = 0xfade0c01
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xfade7171
CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xfade7172
CSSLOT_CODEDIRECTORY, CSSLOT_INFOSLOT, CSSLOT_REQUIREMENTS, CSSLOT_RESOURCEDIR = 0, 1, 2, 3
CSSLOT_ENTITLEMENTS, CSSLOT_DER_ENTITLEMENTS = 5, 7
CSSLOT_ALTERNATE_CODEDIRECTORIES = 0x1000
CS_ADHOC = 0x2
CS_EXECSEG_MAIN_BINARY = 0x1
CD_VERSION = 0x20400
CD_HEADER = 88                            # CodeDirectory fields through execSegFlags
CS_PAGE_SHIFT = 12
HASHES = {                                # hashType: (function, size); xnu ranks 4 > 2 > 3 > 1
    1: (hashlib.sha1, 20),
    2: (hashlib.sha256, 32),
    3: (hashlib.sha256, 20),              # SHA-256 truncated
    4: (hashlib.sha384, 48),
}
HASH_RANK = {1: 0, 3: 1, 2: 2, 4: 3}

NESTED_PLUGIN_DIRS = ('PlugIns', 'Extensions', 'AppClips')
BUNDLE_SUFFIXES = ('.app', '.appex', '.framework', '.xpc', '.bundle')
METAL_LIBS = ('/System/Library/Frameworks/Metal.framework/Metal',
              '/System/Library/Frameworks/MetalKit.framework/MetalKit')
SERVICE_ENTITLEMENTS = {
    'aps-environment': 'push notifications',
    'com.apple.developer.icloud-services': 'iCloud',
    'com.apple.developer.ubiquity-kvstore-identifier': 'iCloud key-value store',
    'com.apple.developer.in-app-payments': 'Apple Pay',
    'com.apple.developer.game-center': 'Game Center',
    'com.apple.developer.applesignin': 'Sign in with Apple',
}


class Refused(Exception):
    """This app cannot run on andromeda or is unsafe to stage."""


class BadSignature(Exception):
    """The code signature is missing or would not pass xnu's checks."""


def say(msg):
    print(f'{TAG}: {msg}', flush=True)


def version_tuple(text):
    parts = []
    for p in str(text).split('.')[:3]:
        digits = ''.join(ch for ch in p if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts + [0] * (3 - len(parts)))


def version_text(v):
    parts = list(v)
    while len(parts) > 2 and parts[-1] == 0:
        parts.pop()
    return '.'.join(str(x) for x in parts)


def align(n, a):
    return (n + a - 1) // a * a


def human(n):
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if n < 1024 or unit == 'GiB':
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024
    return str(n)


# ---------------------------------------------------------------------------
# Mach-O


class MachO:
    """A thin 64-bit little-endian Mach-O image."""

    def __init__(self, data, name):
        self.data = data
        self.name = name
        if len(data) < 32:
            raise Refused(f'{name}: truncated Mach-O header')
        (magic, self.cputype, self.cpusubtype, self.filetype, self.ncmds, self.sizeofcmds,
         self.flags, _) = struct.unpack_from('<8I', data, 0)
        if magic != MH_MAGIC_64:
            raise Refused(f'{name}: not a 64-bit Mach-O')
        end = 32 + self.sizeofcmds
        if end > len(data):
            raise Refused(f'{name}: load commands run past the end of the file')
        self.cmds = []
        off = 32
        for _ in range(self.ncmds):
            if off + 8 > end:
                raise Refused(f'{name}: load commands overrun sizeofcmds')
            cmd, size = struct.unpack_from('<II', data, off)
            if size < 8 or off + size > end:
                raise Refused(f'{name}: bad load command size at {off:#x}')
            self.cmds.append((cmd, off, size))
            off += size
        self.segments = []
        for cmd, off, size in self.cmds:
            if cmd != LC_SEGMENT_64:
                continue
            if size < 72:
                raise Refused(f'{name}: short LC_SEGMENT_64')
            segname = data[off + 8:off + 24].split(b'\0')[0].decode('ascii', 'replace')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, off + 24)
            nsects, = struct.unpack_from('<I', data, off + 64)
            if 72 + 80 * nsects > size:
                raise Refused(f'{name}: {segname} sections overrun the load command')
            sect_offsets = [struct.unpack_from('<I', data, off + 72 + 80 * i + 48)[0] for i in range(nsects)]
            self.segments.append(dict(name=segname, cmd_off=off, vmaddr=vmaddr, vmsize=vmsize,
                                      fileoff=fileoff, filesize=filesize, sect_offsets=sect_offsets))

    def segment(self, name):
        return next((s for s in self.segments if s['name'] == name), None)

    def code_signature(self):
        """(dataoff, datasize, command offset) of LC_CODE_SIGNATURE, or None."""
        for cmd, off, size in self.cmds:
            if cmd == LC_CODE_SIGNATURE and size >= 16:
                dataoff, datasize = struct.unpack_from('<II', self.data, off + 8)
                return dataoff, datasize, off
        return None

    def encryption(self):
        """[(cryptoff, cryptsize, cryptid)] of every LC_ENCRYPTION_INFO(_64)."""
        out = []
        for cmd, off, size in self.cmds:
            if cmd in (LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64) and size >= 20:
                out.append(struct.unpack_from('<III', self.data, off + 8))
        return out

    def platform(self):
        """(platform, minimum OS (x, y, z)) or (None, None)."""
        for cmd, off, size in self.cmds:
            if cmd == LC_BUILD_VERSION and size >= 24:
                plat, minos = struct.unpack_from('<II', self.data, off + 8)
                return plat, (minos >> 16, (minos >> 8) & 0xff, minos & 0xff)
            if cmd == LC_VERSION_MIN_IPHONEOS and size >= 16:
                v, = struct.unpack_from('<I', self.data, off + 8)
                return PLATFORM_IOS, (v >> 16, (v >> 8) & 0xff, v & 0xff)
        return None, None

    def dylibs(self):
        out = []
        for cmd, off, size in self.cmds:
            if cmd in DYLIB_LOADS and size >= 24:
                name_off, = struct.unpack_from('<I', self.data, off + 8)
                if 24 <= name_off < size:
                    out.append(self.data[off + name_off:off + size].split(b'\0')[0].decode('utf-8', 'replace'))
        return out


def arm64_slice(data, name):
    """The arm64 image of a thin or fat Mach-O file: (bytes, was_fat). Refuses other architectures."""
    if len(data) < 8:
        raise Refused(f'{name}: truncated Mach-O')
    magic_le, = struct.unpack_from('<I', data, 0)
    magic_be, = struct.unpack_from('>I', data, 0)
    if magic_le == MH_MAGIC_64:
        cputype, sub = struct.unpack_from('<II', data, 4)
        check_arch(cputype, sub, name)
        return data, False
    if magic_le == MH_MAGIC:
        raise Refused(f'{name}: 32-bit code only (armv7): iOS 16 runs 64-bit code')
    if magic_be in (FAT_MAGIC, FAT_MAGIC_64):
        arches = fat_arches(data, name)
        found = []
        for cputype, sub, off, size in arches:
            found.append(arch_name(cputype, sub))
            if cputype == CPU_TYPE_ARM64 and (sub & ~CPU_SUBTYPE_MASK) in ARM64_SUBTYPES:
                return data[off:off + size], True
        raise Refused(f'{name}: no arm64 slice (has {", ".join(found)}); '
                      'the iPhone X userland on this phone needs plain arm64 (no arm64e)')
    raise Refused(f'{name}: not a Mach-O file')


def fat_arches(data, name):
    magic, n = struct.unpack_from('>II', data, 0)
    wide = magic == FAT_MAGIC_64
    entry = 32 if wide else 20
    if 8 + n * entry > len(data):
        raise Refused(f'{name}: truncated fat header')
    out = []
    for i in range(n):
        if wide:
            cputype, sub, off, size, _, _ = struct.unpack_from('>IIQQII', data, 8 + i * entry)
        else:
            cputype, sub, off, size, _ = struct.unpack_from('>5I', data, 8 + i * entry)
        if off + size > len(data):
            raise Refused(f'{name}: fat slice {i} runs past the end of the file')
        out.append((cputype, sub, off, size))
    return out


def arch_name(cputype, sub):
    sub &= ~CPU_SUBTYPE_MASK
    if cputype == CPU_TYPE_ARM64:
        return 'arm64e' if sub == CPU_SUBTYPE_ARM64E else 'arm64'
    return {12: 'armv7', 7: 'i386', 0x01000007: 'x86_64', 0x0200000c: 'arm64_32'}.get(cputype, f'cpu {cputype:#x}')


def check_arch(cputype, sub, name):
    if cputype != CPU_TYPE_ARM64:
        raise Refused(f'{name}: built for {arch_name(cputype, sub)}, not arm64 (simulator or Mac build?)')
    if (sub & ~CPU_SUBTYPE_MASK) not in ARM64_SUBTYPES:
        raise Refused(f'{name}: arm64e only; the iPhone X userland on this phone needs plain arm64')


def is_macho_file(path):
    """True for thin or fat Mach-O files (fat with a plausible arch count: not a Java class)."""
    try:
        with open(path, 'rb') as f:
            head = f.read(8)
    except OSError:
        return False
    if len(head) < 8:
        return False
    le, = struct.unpack_from('<I', head, 0)
    be, n = struct.unpack_from('>II', head, 0)
    if le in (MH_MAGIC, MH_MAGIC_64):
        return True
    return be in (FAT_MAGIC, FAT_MAGIC_64) and 0 < n <= 16


def macho_filetype(data):
    """File type (MH_EXECUTE, MH_DYLIB, ...) of a thin Mach-O or of a fat file's first slice."""
    try:
        le, = struct.unpack_from('<I', data, 0)
        if le in (MH_MAGIC, MH_MAGIC_64):
            return struct.unpack_from('<I', data, 12)[0]
        arches = fat_arches(data, '')
        return struct.unpack_from('<I', data, arches[0][2] + 12)[0] if arches else None
    except (struct.error, Refused):
        return None


# ---------------------------------------------------------------------------
# Code signatures


def signature_blobs(m):
    """{slot type: blob bytes} of the embedded signature; None when unsigned."""
    cs = m.code_signature()
    if cs is None:
        return None
    dataoff, datasize, _ = cs
    if datasize < 12 or dataoff + datasize > len(m.data):
        raise BadSignature('the signature lies outside the file')
    sb = m.data[dataoff:dataoff + datasize]
    magic, length, count = struct.unpack_from('>III', sb, 0)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE or length > datasize or 12 + 8 * count > length:
        raise BadSignature('not an embedded signature superblob')
    blobs = {}
    for i in range(count):
        typ, off = struct.unpack_from('>II', sb, 12 + 8 * i)
        if off + 8 > length:
            raise BadSignature(f'blob {typ:#x} outside the superblob')
        _, blen = struct.unpack_from('>II', sb, off)
        if blen < 8 or off + blen > length:
            raise BadSignature(f'blob {typ:#x} overruns the superblob')
        blobs[typ] = sb[off:off + blen]
    return blobs


def parse_code_directory(blob):
    if len(blob) < 44:
        raise BadSignature('short code directory')
    (magic, length, version, flags, hash_off, ident_off, n_special, n_code, code_limit,
     hash_size, hash_type, _platform, page_shift, _) = struct.unpack_from('>9I4BI', blob, 0)
    if magic != CSMAGIC_CODEDIRECTORY or length > len(blob):
        raise BadSignature('bad code directory header')
    cd = dict(blob=blob, version=version, flags=flags, hash_off=hash_off, n_special=n_special,
              n_code=n_code, code_limit=code_limit, hash_size=hash_size, hash_type=hash_type,
              page_shift=page_shift, scatter=0, code_limit64=0, exec_seg=None)
    if version >= 0x20100 and length >= 48:
        cd['scatter'], = struct.unpack_from('>I', blob, 44)
    if version >= 0x20300 and length >= 64:
        cd['code_limit64'], = struct.unpack_from('>Q', blob, 56)
    if version >= 0x20400 and length >= CD_HEADER:
        cd['exec_seg'] = struct.unpack_from('>QQQ', blob, 64)
    end = blob.find(b'\0', ident_off) if ident_off < length else -1
    if end < 0:
        raise BadSignature('identifier outside the code directory')
    cd['identifier'] = blob[ident_off:end].decode('utf-8', 'replace')
    return cd


def check_signature(m):
    """(problem or None, info). info: identifier, ent and der blobs, exec_seg_flags."""
    info = dict(identifier=None, ent=None, der=None, exec_seg_flags=None)
    try:
        blobs = signature_blobs(m)
        if blobs is None:
            return 'not signed', info
        info['ent'] = blobs.get(CSSLOT_ENTITLEMENTS)
        info['der'] = blobs.get(CSSLOT_DER_ENTITLEMENTS)
        cds = [parse_code_directory(b) for t, b in blobs.items()
               if t == CSSLOT_CODEDIRECTORY or CSSLOT_ALTERNATE_CODEDIRECTORIES <= t < CSSLOT_ALTERNATE_CODEDIRECTORIES + 5]
        usable = [cd for cd in cds if cd['hash_type'] in HASHES]
        if not usable:
            return 'no code directory with a known hash type', info
        cd = max(usable, key=lambda c: HASH_RANK[c['hash_type']])   # the one xnu validates
        info['identifier'] = cd['identifier']
        if cd['exec_seg']:
            info['exec_seg_flags'] = cd['exec_seg'][2]
        return verify_code_directory(m, cd, blobs), info
    except BadSignature as e:
        return str(e), info


def verify_code_directory(m, cd, blobs):
    """None if xnu would accept the pages and entitlements this code directory covers."""
    fn, size = HASHES[cd['hash_type']]
    blob = cd['blob']
    if cd['hash_size'] != size:
        return f'hash size {cd["hash_size"]} for hash type {cd["hash_type"]}'
    if cd['page_shift'] not in (12, 14):
        return f'page size 2^{cd["page_shift"]} (xnu takes 4 or 16 KiB)'
    if cd['scatter']:
        return 'scatter code directory (not checked here)'
    limit = cd['code_limit64'] or cd['code_limit']
    dataoff = m.code_signature()[0]
    if limit > dataoff:
        return 'the hashed range covers the signature itself'
    page = 1 << cd['page_shift']
    n = (limit + page - 1) // page
    if n != cd['n_code']:
        return f'{cd["n_code"]} page hashes for {n} pages'
    hash_off = cd['hash_off']
    if hash_off < cd['n_special'] * size or hash_off + n * size > len(blob):
        return 'hash table outside the code directory'
    data = memoryview(m.data)
    for i in range(n):
        start = i * page
        digest = fn(data[start:min(start + page, limit)]).digest()[:size]
        if digest != blob[hash_off + i * size:hash_off + (i + 1) * size]:
            return f'page {i} (file offset {start:#x}) does not match its hash'

    def special(slot):
        if slot > cd['n_special']:
            return None
        return blob[hash_off - slot * size:hash_off - (slot - 1) * size]

    for slot, what in ((CSSLOT_ENTITLEMENTS, 'XML entitlements'), (CSSLOT_DER_ENTITLEMENTS, 'DER entitlements')):
        embedded = special(slot)
        present = blobs.get(slot)
        if present is None:
            if embedded is not None and any(embedded):
                return f'{what} hashed but missing'
        elif embedded is None or fn(present).digest()[:size] != embedded:
            return f'{what} do not match their hash'
    return None


def der_length(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def der(tag, content):
    return bytes([tag]) + der_length(len(content)) + content


def der_value(v):
    if isinstance(v, bool):
        return der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        return der(0x02, v.to_bytes((v + (v < 0)).bit_length() // 8 + 1, 'big', signed=True))
    if isinstance(v, str):
        return der(0x0c, v.encode('utf-8'))
    if isinstance(v, list):
        return der(0x30, b''.join(der_value(x) for x in v))
    if isinstance(v, dict):
        keys = sorted(v, key=lambda k: k.encode('utf-8'))
        return der(0x31, b''.join(der(0x30, der(0x0c, k.encode('utf-8')) + der_value(v[k])) for k in keys))
    raise ValueError(f'{type(v).__name__} values have no DER entitlement form')


def der_entitlements_blob(xml_blob):
    """DER entitlements blob (as codesign writes it) from an XML entitlements blob, or None."""
    try:
        ents = plistlib.loads(xml_blob[8:])
        if not isinstance(ents, dict):
            return None
        body = der(0x70, der(0x02, b'\x01') + der(0xb0, der_value(ents)))   # [APPLICATION 16] { 1, [16] dict }
    except (ValueError, plistlib.InvalidFileException):
        return None
    return struct.pack('>II', CSMAGIC_EMBEDDED_DER_ENTITLEMENTS, 8 + len(body)) + body


def sign_adhoc(m, identifier, info_plist=None, resources=None, ent=None, der_blob=None, exec_seg_flags=None):
    """The Mach-O image re-signed ad hoc: one SHA-256 code directory, empty requirements,
    the given entitlement blobs. Code directory layout as in ios-sweet tools/mkinit.py."""
    data = bytearray(m.data)
    text, linkedit = m.segment('__TEXT'), m.segment('__LINKEDIT')
    if text is None or linkedit is None:
        raise Refused(f'{m.name}: no __TEXT or __LINKEDIT segment to sign')
    le_end = linkedit['fileoff'] + linkedit['filesize']
    for s in m.segments:
        if s is not linkedit and s['filesize'] and s['fileoff'] + s['filesize'] > linkedit['fileoff']:
            raise Refused(f'{m.name}: __LINKEDIT is not the last segment in the file')
    cs = m.code_signature()
    if cs is not None:
        dataoff, _, lc_off = cs
        if not linkedit['fileoff'] <= dataoff <= len(data) or dataoff % 16:
            raise Refused(f'{m.name}: LC_CODE_SIGNATURE does not point into __LINKEDIT')
        del data[dataoff:]
    else:
        cmds_end = 32 + m.sizeofcmds
        first = min([o for s in m.segments for o in s['sect_offsets'] if o] +
                    [s['fileoff'] for s in m.segments if s['fileoff']] + [len(data)])
        if cmds_end + 16 > first or any(data[cmds_end:cmds_end + 16]):
            raise Refused(f'{m.name}: unsigned and no room for LC_CODE_SIGNATURE (relink with -headerpad)')
        if le_end != len(data):
            raise Refused(f'{m.name}: unsigned, and __LINKEDIT does not end the file')
        lc_off = cmds_end
        struct.pack_into('<4I', data, lc_off, LC_CODE_SIGNATURE, 16, 0, 0)
        struct.pack_into('<II', data, 16, m.ncmds + 1, m.sizeofcmds + 16)
        dataoff = align(len(data), 16)
        data += bytes(dataoff - len(data))
    if dataoff >= 1 << 32:
        raise Refused(f'{m.name}: larger than 4 GiB')

    page = 1 << CS_PAGE_SHIFT
    n_code = (dataoff + page - 1) // page
    ident = identifier.encode('utf-8') + b'\0'
    requirements = struct.pack('>III', CSMAGIC_REQUIREMENTS, 12, 0)
    special = {CSSLOT_REQUIREMENTS: hashlib.sha256(requirements).digest()}
    if info_plist is not None:
        special[CSSLOT_INFOSLOT] = hashlib.sha256(info_plist).digest()
    if resources is not None:
        special[CSSLOT_RESOURCEDIR] = hashlib.sha256(resources).digest()
    blobs = [(CSSLOT_REQUIREMENTS, requirements)]
    if ent is not None:
        special[CSSLOT_ENTITLEMENTS] = hashlib.sha256(ent).digest()
        blobs.append((CSSLOT_ENTITLEMENTS, ent))
    if der_blob is not None:
        special[CSSLOT_DER_ENTITLEMENTS] = hashlib.sha256(der_blob).digest()
        blobs.append((CSSLOT_DER_ENTITLEMENTS, der_blob))
    n_special = max(special)
    hash_off = CD_HEADER + len(ident) + 32 * n_special
    cd_len = hash_off + 32 * n_code
    sb_len = 12 + 8 * (1 + len(blobs)) + cd_len + sum(len(b) for _, b in blobs)
    datasize = align(sb_len, 16)

    # The header is hashed too: final LC_CODE_SIGNATURE and __LINKEDIT sizes first.
    struct.pack_into('<II', data, lc_off + 8, dataoff, datasize)
    filesize = dataoff + datasize - linkedit['fileoff']
    vmsize = align(filesize, SEGMENT_ALIGN)
    for s in m.segments:
        if s['vmaddr'] > linkedit['vmaddr'] and linkedit['vmaddr'] + vmsize > s['vmaddr']:
            raise Refused(f'{m.name}: {s["name"]} follows __LINKEDIT in memory; cannot grow it')
    struct.pack_into('<Q', data, linkedit['cmd_off'] + 32, vmsize)
    struct.pack_into('<Q', data, linkedit['cmd_off'] + 48, filesize)

    if exec_seg_flags is None:
        exec_seg_flags = CS_EXECSEG_MAIN_BINARY if m.filetype == MH_EXECUTE else 0
    image = memoryview(data)
    code_hashes = b''.join(hashlib.sha256(image[o:min(o + page, dataoff)]).digest()
                           for o in range(0, dataoff, page))
    del image
    cd = struct.pack('>9I4BI', CSMAGIC_CODEDIRECTORY, cd_len, CD_VERSION, CS_ADHOC,
                     hash_off, CD_HEADER, n_special, n_code, dataoff,
                     32, 2, 0, CS_PAGE_SHIFT, 0)
    cd += struct.pack('>III', 0, 0, 0)                     # scatter, team id, spare3
    cd += struct.pack('>QQQQ', 0, text['fileoff'], text['filesize'], exec_seg_flags)
    assert len(cd) == CD_HEADER
    cd += ident + b''.join(special.get(k, bytes(32)) for k in range(n_special, 0, -1)) + code_hashes
    assert len(cd) == cd_len
    entries = [(CSSLOT_CODEDIRECTORY, cd)] + blobs
    index, payload, off = b'', b'', 12 + 8 * len(entries)
    for typ, blob in entries:
        index += struct.pack('>II', typ, off)
        payload += blob
        off += len(blob)
    sb = struct.pack('>III', CSMAGIC_EMBEDDED_SIGNATURE, sb_len, len(entries)) + index + payload
    assert len(sb) == sb_len
    data += sb + bytes(datasize - sb_len)
    return bytes(data)


# ---------------------------------------------------------------------------
# Bundles


def read_plist(path):
    try:
        with open(path, 'rb') as f:
            value = plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        raise Refused(f'{path}: unreadable property list ({e})') from None
    if not isinstance(value, dict):
        raise Refused(f'{path}: not a dictionary')
    return value


def bundles_in(app):
    """{main executable path: bundle dir} for the app and every nested bundle with an executable."""
    out = {}
    dirs = [app] + [Path(d) for d, _, _ in os.walk(app) if Path(d) != app and d.endswith(BUNDLE_SUFFIXES)]
    for d in dirs:
        info = d / 'Info.plist'
        if not info.is_file() or info.is_symlink():
            continue
        try:
            exe = plistlib.loads(info.read_bytes()).get('CFBundleExecutable')
        except (plistlib.InvalidFileException, ValueError, AttributeError):
            continue
        if isinstance(exe, str) and exe and '/' not in exe and (d / exe).is_file():
            out[d / exe] = d
    return out


def check_links(app):
    """Refuse symlinks that leave the bundle and anything that is not a file, directory or link."""
    root = os.path.realpath(app)
    for d, dirnames, filenames in os.walk(app):
        for name in dirnames + filenames:
            p = os.path.join(d, name)
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                target = os.readlink(p)
                resolved = os.path.normpath(os.path.join(d, target))
                if os.path.isabs(target) or os.path.commonpath([root, os.path.realpath(resolved)]) != root:
                    raise Refused(f'{os.path.relpath(p, app)}: symlink to {target} leaves the bundle')
            elif not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
                raise Refused(f'{os.path.relpath(p, app)}: not a regular file or directory')


def nested_plugin_of(rel):
    """The nested extension/app-clip bundle (relative path) that REL lies in, or None."""
    parts = rel.parts
    for i in range(len(parts) - 1):
        if parts[i] in NESTED_PLUGIN_DIRS and parts[i + 1].endswith(('.appex', '.app')):
            return PurePosixPath(*parts[:i + 2])
    return None


def check_info(app, info):
    for key in ('CFBundleIdentifier', 'CFBundleExecutable'):
        if not isinstance(info.get(key), str) or not info[key]:
            raise Refused(f'{app.name}: Info.plist has no {key}')
    platforms = info.get('CFBundleSupportedPlatforms')
    if isinstance(platforms, list) and platforms and 'iPhoneOS' not in platforms:
        raise Refused(f'{app.name}: built for {", ".join(map(str, platforms))}, not iPhoneOS')
    families = info.get('UIDeviceFamily')
    if isinstance(families, list) and families and 1 not in [int(f) for f in families if str(f).isdigit()]:
        raise Refused(f'{app.name}: iPad-only (UIDeviceFamily {families}); iOS on an iPhone X does not list it')
    minimum = info.get('MinimumOSVersion')
    if minimum and version_tuple(minimum) > IOS_VERSION:
        raise Refused(f'{app.name}: needs iOS {minimum}; this phone runs iOS {version_text(IOS_VERSION)}')


def process_app(app, mode='auto', drop_encrypted=False, fix=True):
    """Check the .app directory APP (and with fix=True prepare it in place: drop FairPlay
    leftovers, thin, re-sign). Returns a report dict; raises Refused."""
    app = Path(app)
    rep = dict(app=app.name, removed=[], dropped=[], resigned=[], kept=0, warnings=[], machos=0)
    info_path = app / 'Info.plist'
    if not info_path.is_file():
        raise Refused(f'{app.name}: no Info.plist')
    info = read_plist(info_path)
    check_info(app, info)
    rep.update(bundle_id=info['CFBundleIdentifier'], executable=info['CFBundleExecutable'],
               version=str(info.get('CFBundleShortVersionString') or info.get('CFBundleVersion') or '?'),
               name=str(info.get('CFBundleDisplayName') or info.get('CFBundleName') or app.stem),
               minimum=str(info.get('MinimumOSVersion') or '?'))
    if fix:
        for d in [p for p in app.rglob('SC_Info') if p.is_dir() and not p.is_symlink()]:
            shutil.rmtree(d)
            rep['removed'].append(f'{d.relative_to(app)} (FairPlay data)')
        if (app / 'Watch').is_dir() and not (app / 'Watch').is_symlink():
            shutil.rmtree(app / 'Watch')
            rep['removed'].append('Watch (watchOS app)')
    check_links(app)
    main = app / rep['executable']
    if not main.is_file() or main.is_symlink() or not is_macho_file(main):
        raise Refused(f'{app.name}: main executable {rep["executable"]} missing or not a Mach-O')

    # Encryption first, over every loadable Mach-O, so nothing is changed for a refused app.
    # Object files and debug symbols (MH_OBJECT, MH_DSYM) are not loaded: left alone.
    machos = sorted((p for p in app.rglob('*') if p.is_file() and not p.is_symlink() and is_macho_file(p)),
                    key=lambda p: (-len(p.parts), str(p)))
    encrypted = {}
    images = {}
    for p in machos:
        rel = PurePosixPath(p.relative_to(app).as_posix())
        raw = p.read_bytes()
        if p != main and macho_filetype(raw) not in (MH_EXECUTE, MH_DYLIB, MH_BUNDLE):
            continue
        image, was_fat = arm64_slice(raw, str(rel))
        m = MachO(image, str(rel))
        if any(cryptid for _, _, cryptid in m.encryption()):
            encrypted[rel] = m.encryption()
        images[p] = (m, was_fat)
    for rel, enc in encrypted.items():
        plugin = nested_plugin_of(rel)
        ids = ', '.join(str(c) for _, _, c in enc if c)
        if plugin is None or not drop_encrypted:
            hint = (' (an encrypted extension: --drop-encrypted-plugins stages the app without it)'
                    if plugin is not None else '')
            raise Refused(f'{rel}: encrypted with FairPlay (cryptid {ids}){hint}; '
                          'only unencrypted code runs on this phone')
    for plugin in sorted({nested_plugin_of(rel) for rel in encrypted}):
        if fix:
            shutil.rmtree(app / plugin)
        rep['dropped'].append(str(plugin))
        for p in [p for p in images if p.is_relative_to(app / plugin)]:
            del images[p]

    plat, minos = images[main][0].platform()
    if images[main][0].filetype != MH_EXECUTE:
        raise Refused(f'{rep["executable"]}: not an executable (file type {images[main][0].filetype})')
    if plat is not None and plat != PLATFORM_IOS:
        raise Refused(f'{rep["executable"]}: built for {PLATFORMS.get(plat, plat)}, not iOS')
    if minos and minos > IOS_VERSION:
        rep['warnings'].append(f'{rep["executable"]} was built for iOS {version_text(minos)} or later: '
                               'it may stop on missing symbols')

    main_of = bundles_in(app)
    rpath_users, metal = [], False
    for p, (m, was_fat) in images.items():
        rel = str(p.relative_to(app))
        rep['machos'] += 1
        for lib in m.dylibs():
            if lib.startswith(('@rpath/', '@executable_path/', '@loader_path/')):
                rpath_users.append(rel)
                break
        metal = metal or any(lib in METAL_LIBS for lib in m.dylibs())
        problem, sig = check_signature(m)
        bundle = main_of.get(p)
        bundle_id = None
        if bundle is not None:
            try:
                bundle_id = plistlib.loads((bundle / 'Info.plist').read_bytes()).get('CFBundleIdentifier')
            except (plistlib.InvalidFileException, ValueError, AttributeError):
                bundle_id = None
        mismatch = None
        if problem is None and isinstance(bundle_id, str) and sig['identifier'] != bundle_id:
            mismatch = f'identifier {sig["identifier"]!r} is not the bundle id {bundle_id}'
        if not fix or mode == 'never':
            if problem is not None:
                hint = '--keep-signature' if fix else 'stage the original with scripts/add-ipa.py'
                raise Refused(f'{rel}: signature {problem}; the kernel would refuse it ({hint})')
            if mismatch:
                rep['warnings'].append(f'{rel}: {mismatch} (LaunchServices ignores its entitlements)')
            rep['kept'] += 1
            if was_fat and fix:
                p.write_bytes(m.data)
            continue
        reason = problem or mismatch or ('--resign' if mode == 'always' else None)
        if reason is None:
            rep['kept'] += 1
            if was_fat:
                p.write_bytes(m.data)
            continue
        ident = bundle_id if isinstance(bundle_id, str) and bundle_id else sig['identifier'] or p.stem
        ent, der_blob = sig['ent'], sig['der']
        if ent is not None and (len(ent) < 8 or struct.unpack_from('>I', ent, 0)[0] != CSMAGIC_EMBEDDED_ENTITLEMENTS):
            ent = None
        if der_blob is not None and struct.unpack_from('>I', der_blob, 0)[0] != CSMAGIC_EMBEDDED_DER_ENTITLEMENTS:
            der_blob = None
        if ent is not None and der_blob is None:
            der_blob = der_entitlements_blob(ent)
            if der_blob is None:
                rep['warnings'].append(f'{rel}: entitlements kept as XML only (no DER form for them)')
        info_plist = resources = None
        if bundle is not None:
            info_plist = (bundle / 'Info.plist').read_bytes()
            res = bundle / '_CodeSignature' / 'CodeResources'
            if res.is_file() and not res.is_symlink():
                resources = res.read_bytes()
        signed = sign_adhoc(m, ident, info_plist, resources, ent, der_blob, sig['exec_seg_flags'])
        again, _ = check_signature(MachO(signed, rel))
        if again is not None:
            raise Refused(f'{rel}: re-signing failed its own check: {again}')
        tmp = p.with_name(p.name + '.add-ipa-tmp')
        tmp.write_bytes(signed)
        os.chmod(tmp, 0o755)
        os.replace(tmp, p)
        rep['resigned'].append((rel, reason))

    if fix:
        for p in images:
            os.chmod(p, 0o755)
    if rpath_users:
        rep['warnings'].append(
            f'{len(rpath_users)} Mach-O file(s) load libraries through @rpath/@executable_path/@loader_path '
            f'(e.g. {rpath_users[0]}): dyld refuses @ paths until our kernel answers its AMFI dyld policy '
            'query (ios-flame patch 0114, not ported yet), so this app will not start before that')
    if metal:
        rep['warnings'].append('links Metal: it draws nothing until the GPU driver works')
    ents = entitlements_of(images[main][0])
    services = sorted({v for k, v in SERVICE_ENTITLEMENTS.items() if k in ents})
    if services:
        rep['warnings'].append(f'uses {", ".join(services)}: not available on this phone')
    rep['size'] = sum(p.stat().st_size for p in app.rglob('*') if p.is_file() and not p.is_symlink())
    return rep


def entitlements_of(m):
    try:
        blobs = signature_blobs(m) or {}
        ent = blobs.get(CSSLOT_ENTITLEMENTS)
        value = plistlib.loads(ent[8:]) if ent else {}
        return value if isinstance(value, dict) else {}
    except (BadSignature, plistlib.InvalidFileException, ValueError):
        return {}


# ---------------------------------------------------------------------------
# Unpacking


def zip_name(info):
    """Member name as UTF-8 even when the archiver did not set the UTF-8 flag (macOS zips)."""
    name = info.filename
    if not info.flag_bits & 0x800:
        try:
            name = name.encode('cp437').decode('utf-8')
        except (UnicodeEncodeError, UnicodeDecodeError):
            pass
    return name


def unpack_ipa(ipa, work):
    """Extract the single Payload/NAME.app of IPA into WORK; return its path."""
    try:
        zf = zipfile.ZipFile(ipa)
    except (zipfile.BadZipFile, OSError) as e:
        raise Refused(f'{ipa}: not a readable .ipa ({e})') from None
    with zf:
        members = []
        apps = set()
        for info in zf.infolist():
            name = zip_name(info)
            if '\0' in name or '\\' in name:
                raise Refused(f'{ipa}: bad member name {name!r}')
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts:
                raise Refused(f'{ipa}: member {name!r} escapes the archive')
            if len(path.parts) < 2 or path.parts[0] != 'Payload':
                continue                  # iTunesMetadata.plist, META-INF, SwiftSupport: not needed
            if not path.parts[1].endswith('.app'):
                raise Refused(f'{ipa}: Payload/{path.parts[1]} is not an .app bundle')
            apps.add(path.parts[1])
            members.append((info, path))
        if len(apps) != 1:
            raise Refused(f'{ipa}: expected one Payload/NAME.app, found {len(apps)}')
        total = sum(i.file_size for i, _ in members)
        if total > MAX_APP_BYTES:
            raise Refused(f'{ipa}: {human(total)} unpacked; the root image cannot hold it')
        free = shutil.disk_usage(work).free
        if total + (64 << 20) > free:
            raise Refused(f'{ipa}: needs {human(total)}, {human(free)} free in {work}')
        links = []
        try:
            for info, path in members:
                dest = work.joinpath(*path.parts[1:])
                mode = info.external_attr >> 16
                if info.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                    continue
                if mode and stat.S_ISLNK(mode):
                    links.append((dest, zf.read(info).decode('utf-8')))
                    continue
                if mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                    raise Refused(f'{ipa}: {path} is not a regular file')
                dest.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, open(dest, 'wb') as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                os.chmod(dest, 0o755 if mode & 0o111 else 0o644)
            # Links last, so no member is ever written through one; check_links() vets their targets.
            for dest, target in links:
                dest.parent.mkdir(parents=True, exist_ok=True)
                os.symlink(target, dest)
        except (OSError, UnicodeDecodeError, zipfile.BadZipFile, NotImplementedError) as e:
            raise Refused(f'{ipa}: cannot unpack ({e})') from None
    return work / next(iter(apps))


def copy_app_dir(src, work):
    """Copy an .app directory (an Xcode build) into WORK, keeping symlinks as symlinks."""
    dest = work / src.name
    shutil.copytree(src, dest, symlinks=True)
    return dest


# ---------------------------------------------------------------------------
# Staging directory


def default_dest():
    name = 'andromeda'
    try:
        import tomllib
        with open(REPO / 'board.toml', 'rb') as f:
            name = tomllib.load(f)['board']['name']
    except (OSError, KeyError, ImportError, ValueError):
        pass
    return REPO / 'inputs' / name / 'apps'


def staged_apps(dest):
    """[(path, Info.plist dict or None)] of the bundles staged in DEST."""
    out = []
    if dest.is_dir():
        for p in sorted(dest.glob('*.app')):
            if p.is_dir() and not p.is_symlink() and not p.name.startswith('.'):
                try:
                    out.append((p, read_plist(p / 'Info.plist')))
                except Refused:
                    out.append((p, None))
    return out


def stage(src, dest, args):
    src = Path(src)
    if not src.exists():
        raise Refused(f'{src}: no such file')
    dest.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='.add-ipa-', dir=dest))
    try:
        if src.is_dir():
            if src.suffix != '.app':
                raise Refused(f'{src}: a directory must be an .app bundle')
            app = copy_app_dir(src, work)
        else:
            app = unpack_ipa(src, work)
        mode = 'always' if args.resign else 'never' if args.keep_signature else 'auto'
        rep = process_app(app, mode, args.drop_encrypted_plugins, fix=True)
        target = dest / app.name
        for p, info in staged_apps(dest):
            same_id = info is not None and info.get('CFBundleIdentifier') == rep['bundle_id']
            if p == target and not same_id:
                raise Refused(f'{target} already holds another app '
                              f'({info.get("CFBundleIdentifier") if info else "unreadable"}); '
                              'remove it first with --remove')
            if same_id:
                shutil.rmtree(p)
                say(f'replacing the staged {p.name}')
        os.chmod(app, 0o755)
        os.rename(app, target)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report(src, target, rep)


def report(src, target, rep):
    say(f'{src} -> {target}')
    print(f'  {rep["bundle_id"]} {rep["version"]} ("{rep["name"]}"), minimum iOS {rep["minimum"]}, '
          f'{human(rep["size"])}, {rep["machos"]} Mach-O file(s)')
    print(f'  signature: {rep["kept"]} kept, {len(rep["resigned"])} re-signed ad hoc')
    for rel, why in rep['resigned'][:8]:
        print(f'    {rel}: {why}')
    if len(rep['resigned']) > 8:
        print(f'    ... and {len(rep["resigned"]) - 8} more')
    for what in rep['removed']:
        print(f'  removed: {what}')
    for what in rep['dropped']:
        print(f'  dropped (encrypted extension): {what}')
    for w in rep['warnings']:
        print(f'  warning: {w}')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Stage unencrypted iOS apps for the andromeda root build.')
    ap.add_argument('inputs', nargs='*', metavar='IPA_OR_APP')
    ap.add_argument('--dest', type=Path, help='staging directory (default: inputs/<board>/apps)')
    sign = ap.add_mutually_exclusive_group()
    sign.add_argument('--resign', action='store_true', help='re-sign every Mach-O ad hoc')
    sign.add_argument('--keep-signature', action='store_true', help='never re-sign; refuse broken signatures')
    ap.add_argument('--drop-encrypted-plugins', action='store_true',
                    help='remove encrypted app extensions instead of refusing the app')
    action = ap.add_mutually_exclusive_group()
    action.add_argument('--list', action='store_true', help='list the staged apps')
    action.add_argument('--remove', action='store_true', help='unstage the apps named (bundle id or NAME.app)')
    action.add_argument('--verify', action='store_true', help='check .app bundles without changing them')
    args = ap.parse_args(argv)
    dest = (args.dest or default_dest()).resolve()

    if args.list:
        apps = staged_apps(dest)
        if not apps:
            say(f'nothing staged in {dest}')
        for p, info in apps:
            info = info or {}
            size = sum(f.stat().st_size for f in p.rglob('*') if f.is_file() and not f.is_symlink())
            print(f'{p.name:32} {info.get("CFBundleIdentifier", "?"):40} '
                  f'{info.get("CFBundleShortVersionString", "?"):12} {human(size)}')
        return 0
    if not args.inputs:
        ap.error('no IPA_OR_APP given')
    if args.remove:
        rc = 0
        for name in args.inputs:
            hits = [p for p, info in staged_apps(dest)
                    if name in (p.name, p.stem) or (info or {}).get('CFBundleIdentifier') == name]
            if not hits:
                say(f'{name}: not staged in {dest}')
                rc = 1
            for p in hits:
                shutil.rmtree(p)
                say(f'removed {p}')
        return rc
    if args.verify:
        rc = 0
        for name in args.inputs:
            try:
                rep = process_app(Path(name), 'never', fix=False)
                say(f'{name}: ok ({rep["bundle_id"]} {rep["version"]}, {rep["machos"]} Mach-O file(s))')
                for w in rep['warnings']:
                    print(f'  warning: {w}')
            except Refused as e:
                say(f'{name}: refused: {e}')
                rc = 1
        return rc
    rc = 0
    for name in args.inputs:
        try:
            stage(name, dest, args)
        except Refused as e:
            say(f'{name}: refused, nothing staged: {e}')
            rc = 1
    if rc == 0:
        say(f'next: build the root (./build-root.sh); phone/andromeda-apps/install-root.sh copies '
            f'{dest} into /Applications')
    return rc


if __name__ == '__main__':
    sys.exit(main())
