#!/usr/bin/env python3
"""Per-device hardware profiles for the SWEET XNU port.

One entry per phone: everything that changes when the same kernel/shim is
brought up on a different Qualcomm board. The values are the ones the early
boot path reads (see NOTES.md); they come from the live FDT / stock DTB of
the device (reserved-memory, /memory, /soc MMIO nodes) unless noted.

`guacamole` is the OnePlus 7 Pro (SM8150) port and `cepheus` is the Redmi K20
Pro / Mi 9T Pro (SM8150) -- the SAME silicon. They share GIC/UFS/CPU MMIO
ranges; they differ in the board compatible string and the panel resolution
(cepheus's panel is 1080x2340). `sweet` is the original SM7150 bring-up and is
kept for reference.

Used by tools/mkadt.py (ADT) and mirrored by shim/boards/*.h (framebuffer,
GIC, probe addresses). Keep the two in sync when changing a value.
"""

DEVICES = {
    'sweet': {
        # Redmi Note 10 Pro (SM7150 / Snapdragon 732G)
        'name': 'Redmi Note 10 Pro (sweet, SM7150)',
        'board_compat': 'xiaomi,sweet',
        'soc_compat': 'qcom,sm7150',
        'phys_base': 0xb0000000,
        'mem_size': 0x4d000000,
        'high_phys_base': 0x100000000,
        'high_mem_size': 0xfcc00000,
        'gicd': 0x17a00000,
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,
        'gicr_size': 0x100000,
        'timebase': 19200000,
        'vtimer_intid': 19,
        'fb': 0x9c000000,
        'fb_size': 0x1700000,
        'fb_w': 1080,
        'fb_h': 2400,
        'fb_stride': 1080 * 4,
        'ufs': 0x01d84000,
        'ufs_size': 0x3000,
        'cpu_compat': 'qcom,kryo470',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1804800000] * 6 + [2304000000] * 2,
        'memory_frequency': 1866000000,
        'bus_frequency': 100000000,
    },
    'guacamole': {
        # OnePlus 7 Pro (guacamole, SM8150). Board facts from the LineageOS
        # oneplus_sm8150 DTS and the mainline sm8150-oneplus-guacamole.dts;
        # audited against the GM1910's own live FDT.
        'name': 'OnePlus 7 Pro (guacamole, SM8150)',
        'board_compat': 'oneplus,guacamole',
        'soc_compat': 'qcom,sm8150',
        'phys_base': 0xc0000000,
        'mem_size': 0xc0000000,
        'high_phys_base': 0x180000000,
        'high_mem_size': 0x100000000,
        'gicd': 0x17a00000,          # confirmed on the live FDT
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,          # confirmed
        'gicr_size': 0x100000,
        'timebase': 19200000,        # confirmed (clock-frequency)
        'vtimer_intid': 19,          # confirmed (GIC_PPI 3 numbering)
        # reserved-memory cont_splash_region: 0x9C000000, 0x02400000 (36 MiB).
        'fb': 0x9c000000,
        'fb_size': 0x2400000,
        'fb_w': 1440,
        'fb_h': 3120,
        'fb_stride': 1440 * 4,
        'ufs': 0x01d84000,           # confirmed (ICE at 0x1d90000 too)
        'ufs_size': 0x3000,
        'cpu_compat': 'qcom,kryo485',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1785600000] * 4 + [2419200000] * 3 + [2841600000],
        'memory_frequency': 2133000000,
        'bus_frequency': 100000000,
    },
    'andromeda': {
        # Xiaomi Mi Mix 3 5G (andromeda, SM8150) -- same SoC as cepheus/raphael/
        # guacamole, same Samsung EA8076 command-mode panel family as cepheus.
        # Values read from THIS phone's live device tree on 2026-11-06
        # (adb root, /sys/firmware/fdt, 651331 bytes;
        # hwversion 3.19.0, board-id 0x2d, SM8150 v2.2, 6 GiB, EvolutionX 16).
        'name': 'Xiaomi Mi Mix 3 5G (andromeda, SM8150)',
        'board_compat': 'xiaomi,andromeda',
        # MACHINE_CONFIG=SWEET kernel: the platform expert matches 'xiaomi,sweet'.
        'platform_match': 'xiaomi,sweet',
        'soc_compat': 'qcom,sm8150',
        # Live FDT /memory (6 GiB unit):
        #   0x080000000 + 0x3CC00000  (low bank, ends 0x0BCC00000)
        #   0x0C0000000 + 0x80000000  (mid bank, ends 0x140000000)
        #   0x140000000 + 0xC0000000  (high bank, ends 0x200000000)
        # The stock Aloha UEFI map splits that run at 0x180000000 (RamPartitionDxe
        # adds the high RAM with other capabilities; there is no PStore in this
        # build); patches/aloha-andromeda-ram-merge.patch merges it into one
        # 3 GiB entry. DarwinBoot, not this table, places the kernel window
        # (board.toml owned_memory_size).
        'phys_base': 0xc0000000,
        'mem_size': 0xc0000000,
        'high_phys_base': 0x180000000,
        'high_mem_size': 0x80000000,
        'gicd': 0x17a00000,          # SM8150, same as cepheus
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,
        'gicr_size': 0x100000,
        'timebase': 19200000,
        'vtimer_intid': 19,
        # Live FDT reserved-memory cont_splash_region: 0x9C000000 + 0x02400000
        # (36 MiB), panel dsi_samsung_fhd_ea8076_cmd_display, 1080x2340.
        # The kernel prefers boot_args->Video; this is only the ADT fallback.
        'fb': 0x9c000000,
        'fb_size': 0x2400000,
        'fb_w': 1080,
        'fb_h': 2340,
        'fb_stride': 1080 * 4,
        'ufs': 0x01d84000,           # SM8150 UFS host, same as cepheus
        'ufs_size': 0x3000,
        # Kryo 485: 4x A55 + 3x A76 + 1x A76 prime (SM8150, not -AC).
        'cpu_compat': 'qcom,kryo485',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1785600000] * 4 + [2419200000] * 3 + [2841600000],
        'memory_frequency': 2133000000,
        'bus_frequency': 100000000,
    },
    'cepheus': {
        # Xiaomi Mi 9 (cepheus, SM8150) -- SAME SoC as guacamole and raphael.
        # Every value below was read out of THIS phone's live device tree on
        # 2026-10-31 (adb root, `adb pull /sys/firmware/fdt`, 646932 bytes;
        # serial 67eea163, hwversion 1.9.2, CN, MP, 8 GiB / 256 GB).
        # The SM8150 MMIO ranges (GIC/UFS/timer) are identical to guacamole.
        'name': 'Xiaomi Mi 9 (cepheus, SM8150)',
        'board_compat': 'xiaomi,cepheus',
        # The kernel is built with MACHINE_CONFIG=SWEET, whose built-in
        # platform expert only matches IONameMatch 'xiaomi,sweet'. The tree has
        # to carry that string or the platform expert never binds.
        'platform_match': 'xiaomi,sweet',
        'soc_compat': 'qcom,sm8150',
        # Live FDT /memory (8 GiB unit):
        #   0x080000000 + 0x3BB00000  (low bank, ends 0x0BBC00000)
        #   0x100000000 + 0x100000000 (high bank)
        #   0x0C0000000 + 0x0C0000000 (mid bank, carve-out free)
        # dram-base is the mid bank; the 52 MiB hole 0xBBC00000..0xC0000000
        # between low and mid is why base is NOT 0x80000000.
        'phys_base': 0xc0000000,
        'mem_size': 0xc0000000,
        'high_phys_base': 0x180000000,
        'high_mem_size': 0x100000000,
        'gicd': 0x17a00000,          # confirmed on the live FDT
        'gicd_size': 0x10000,
        'gicr': 0x17a60000,          # confirmed
        'gicr_size': 0x100000,
        'timebase': 19200000,        # confirmed (clock-frequency)
        'vtimer_intid': 19,          # confirmed (GIC_PPI 3 numbering)
        # Live FDT reserved-memory cont_splash_region@9c000000:
        #   reg = <0x0 0x9C000000 0x0 0x02400000>   (36 MiB)
        # 1080x2340x4 = 10,108,800 B, so 36 MiB covers it with room to spare.
        # NOTE: the Mu-Silicium handoff has been seen reporting video pa
        # 0x90000000 on this SoC family (raphael/cepheus measurements). The
        # kernel must prefer boot_args->Video at runtime; this value is only
        # the ADT fallback when UEFI hands over no Video.
        'fb': 0x9c000000,
        'fb_size': 0x2400000,
        'fb_w': 1080,
        'fb_h': 2340,
        'fb_stride': 1080 * 4,
        'ufs': 0x01d84000,           # confirmed (ICE at 0x1d90000 too)
        'ufs_size': 0x3000,
        # Kryo 485: 4x A55 (silver) + 3x A76 (gold) + 1x A76 (prime) -- same as guacamole.
        'cpu_compat': 'qcom,kryo485',
        'cpus': 8,
        'cpu_regs': [0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700],
        'cpu_clocks': [1785600000] * 4 + [2419200000] * 3 + [2841600000],
        'memory_frequency': 2133000000,
        'bus_frequency': 100000000,
    },
}


def get(name):
    try:
        return DEVICES[name]
    except KeyError:
        raise SystemExit(f'unknown device "{name}"; known: {", ".join(sorted(DEVICES))}')
