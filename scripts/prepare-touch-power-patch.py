#!/usr/bin/env python3
"""Add ONLY an ldoc1 XOB enable vote to pinned SWEET RPMh transport.

The Mi 9 live FDT uses the same RSC/DRV layout, but ldoc1 is XOB: never
write its voltage or mode. Keep idle-slot validation and timeout quarantine.
"""
import difflib
from pathlib import Path
import subprocess
import sys
import tempfile

TARGET = 'bsd/dev/sweet_rpmh.c'


def replace(s, old, new, count=1):
    if s.count(old) != count:
        raise ValueError(f'power patch anchor changed: {old!r}')
    return s.replace(old, new)


def transform(s):
    s = replace(s, 'int sweet_rpmh_vreg_enable(const char *res, unsigned mv);',
                'int sweet_rpmh_vreg_enable(const char *res, unsigned mv);\nint cepheus_touch_power_enable(void);')
    s = replace(s, 'rh_send(uint32_t address, unsigned mv)',
                'rh_send(uint32_t address, unsigned mv, bool xob)')
    s = replace(s, '\t/* HPM (raw 7) is supported by this PMIC5 LDO; enable comes last. */',
                '\t/* XOB only accepts enable at base+4. VRM keeps its original sequence. */')
    s = replace(s, '\tconst uint32_t offsets[3] = {0, 8, 4};\n\tconst uint32_t values[3] = {mv, 7, 1};',
                '\tconst uint32_t offsets[3] = {xob ? 4u : 0u, 8, 4};\n'
                '\tconst uint32_t values[3] = {xob ? 1u : mv, 7, 1};\n'
                '\tconst unsigned ncmd = xob ? 1u : 3u;\n'
                '\tconst uint32_t command_mask = (1u << ncmd) - 1u;\n'
                '\tif (xob) { printf("CEPHEUS POWER: ldoc1 XOB enable only address=%#x data=1\\n", address + 4); }')
    s = replace(s, 'c < 3;', 'c < ncmd;', count=2)
    s = replace(s, 'rh_sync(b + RH_WAIT, 7) || !rh_sync(b + RH_ENABLE, 7)',
                'rh_sync(b + RH_WAIT, command_mask) || !rh_sync(b + RH_ENABLE, command_mask)')
    s = replace(s, 'rh_send(address, mv);', 'rh_send(address, mv, false);')
    s = replace(s, '#endif /* ARM64_BOARD_CONFIG_SWEET */', '''
/* Mi 9 fixed 1.8V L1C is an XOB resource, not programmable VRM voltage.
 * Only ACTIVE enable=1 is sent. No direct SPMI, sleep or wake writes.
 * Successful submission is not proof that the physical rail reached 1.8V.
 */
int
cepheus_touch_power_enable(void)
{
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) { return EBUSY; }
	int error = EIO;
	uint32_t address = 0;
	if (!rh_poisoned) {
		error = rh_map();
		if (!error) { error = rh_lookup("ldoc1", &address); }
		if (!error) { error = rh_send(address, 0, true); }
	}
	printf("CEPHEUS POWER: ldoc1 enable request result=%d\\n", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */''')
    s = s.replace('conflicting inherited L11C request', 'conflicting inherited rail request')
    s = s.replace('L11C ACTIVE vote complete', 'ACTIVE vote complete')
    s = replace(s, '\tprintf("SWEET RPMH: TCS%u ACTIVE %u mV mode=7 enable=1 irq-enable=%#x\\n", t, mv, irq_en);',
                '\tif (!xob) { printf("SWEET RPMH: TCS%u ACTIVE %u mV mode=7 enable=1 irq-enable=%#x\\n", t, mv, irq_en); }')
    return s


def main():
    patches, output = map(Path, sys.argv[1:])
    if output.exists(): sys.exit('refusing to overwrite patch')
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        for p in sorted(patches.glob('*.patch')):
            # This generator emits 0084. A later GPU-only include must not
            # become context required by the earlier patch on a clean build.
            if p.name == '0086-cepheus-gpu-arc.patch':
                continue
            subprocess.run(['git', '-C', str(root), 'apply', '--whitespace=nowarn',
                            f'--include={TARGET}', str(p.resolve())], check=True)
        before = (root / TARGET).read_text()
        after = transform(before)
        output.write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                                      fromfile='a/' + TARGET, tofile='b/' + TARGET)))
        subprocess.run(['git', '-C', str(root), 'apply', '--check', str(output.resolve())], check=True)
    print('ldoc1 enable-only patch apply-checked:', output)


if __name__ == '__main__': main()
