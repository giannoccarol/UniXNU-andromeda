/*
 * Adreno 640 core on the Xiaomi Mi Mix 3 5G (andromeda, SM8150), shared by
 * the kernel environment (bsd/dev/andromeda_gpu.c) and the register bring-up
 * sequence (bsd/dev/andromeda_gpu_core.c).
 *
 * The bring-up is a port of ios-flame's sweet_a6xx.c / sweet_gpu.c
 * (Nick Wylynko / NWylynko, https://github.com/NWylynko/ios-flame; MIT for the
 * repository, the patched XNU files are APSL 2.0). flame runs this exact GPU
 * on a Pixel 4 (same SM8150, same Adreno 640, same ios-sweet xnu-8796 SWEET
 * base, same iOS 16.7.16) with Metal; its register values follow mainline
 * Linux drm/msm (a6xx_gmu.c, a6xx_hfi.c, a6xx_gpu.c), a6xx.xml, gpucc-sm8150.c
 * and sm8150.dtsi. The ios-sweet platform is dipa-studio's. The read-only
 * probe, the TSENS thermal guard and the "verify, then act in gated steps"
 * discipline follow the Mi 9 port (YangQi0408, UniXNU-cepheus feat/adreno640:
 * cepheus_gpu_tsens.h, cepheus_gpu_firmware.h, cepheus_soc.c). See CREDITS.md.
 *
 * Every address below is andromeda's own, read from the phone's live device
 * tree (phone/<serial>/live-fdt.dts). Because it is the same SM8150 revision
 * as the Pixel 4 and the Mi 9, the register map is identical; the one value
 * that differs is the reserved ZAP region (ANDR_ZAP_REGION), which is
 * per-device.
 *
 * Nothing here touches the GPU unless the boot-arg andrgpu=N (N = highest
 * step to run) is set; andrgpu=0 or absent leaves every register untouched.
 *   1  read-only probe: GPUCC/GMU/SMMU IDs, cmd-db, firmware found in the ADT
 *      and SHA-256 verified against docs/research/gpu.md 6.3b
 *   2  CX power + clocks, then GMU firmware boot (expect 0xBABEFACE, HFI ready)
 *   3  HFI perf/bw tables + OOB GPU_SET (GX up), GPU SMMU + hw_init + SQE + CP
 *      ME_INIT + ZAP shader via TrustZone PAS, secure mode off
 *   4  CP_MEM_WRITE test: write 0xcafef00d to a pool buffer and read it back
 * No present / IOSurface / Metal here: those are later phases.
 */
#ifndef ANDROMEDA_GPU_H
#define ANDROMEDA_GPU_H
#ifdef KERNEL
#include <sys/types.h>
#include <stdarg.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#endif

/* ---- andromeda register blocks (live-fdt.dts) ---- */
#define ANDR_GCC		0x00100000ull	/* gcc clock controller */
#define ANDR_GPU_BASE		0x02c00000ull	/* qcom,kgsl-3d0@2C00000 reg[0] */
#define ANDR_GMU_BASE		0x02c6a000ull	/* qcom,gmu@0x2C6A000 reg[0] */
#define ANDR_GPUCC		0x02c90000ull	/* qcom,gpucc reg cc_base */
#define ANDR_SMMU		0x02ca0000ull	/* kgsl-smmu@0x02ca0000 reg base */
#define ANDR_RSCC		(ANDR_GMU_BASE + 0x23000ull)
#define ANDR_PDC		0x0b290000ull	/* kgsl_gmu_pdc_cfg */
#define ANDR_PDC_SEQ		0x0b490000ull	/* kgsl_gmu_pdc_seq */
#define ANDR_CMDDB_LOCATOR	0x0c3f000cull	/* qcom,cmd-db@c3f000c */

#define ANDR_GREG(o)		(ANDR_GPU_BASE + 4ull * (o))
#define ANDR_GMU_REG(o)		(ANDR_GMU_BASE + 4ull * (o))
#define ANDR_SMMU_CB(n)		(ANDR_SMMU + 0x8000 + 0x1000ull * (n))

/* SMMU stream ids and masks: live-fdt.dts iommus. gfx3d_user <0x1b8 0 0x401>,
 * gmu <0x1b8 4/5 0x400>. Same as flame/vayu on the SM8150. */
#define ANDR_GMU_SID		5u
#define ANDR_GMU_SID_MASK	0x400u
#define ANDR_GPU_SID		0u
#define ANDR_GPU_SID_MASK	0x401u

/* ZAP shader: reserved-memory pil_gpu_region (removed-dma-pool, no-map);
 * qcom,kgsl-hyp pas-id 13, firmware-name "a640_zap". flame's Pixel 4 value was
 * 0x9a915000; andromeda's is 0x98f15000 (live-fdt.dts), size 0x2000. */
#define ANDR_ZAP_REGION		0x98f15000ull
#define ANDR_ZAP_SIZE		0x2000u
#define ANDR_ZAP_PAS		13u

#define ANDR_CHIP_ID		0x06040001ull	/* qcom,chipid = <0x6040001> */
#define ANDR_GPU_ID		640u

/* GPU TSENS: gpuss-0-usr = tsens0 sensor 15, gpuss-1-usr = tsens1 sensor 11
 * (live-fdt.dts thermal-zones <0xbe 0x0f> / <0xbf 0x0b>; tsens0 TM 0x0c263000,
 * tsens1 TM 0x0c265000). Same decode as andromeda_cpufreq.c. */
#define ANDR_TSENS0_TM_PA	0x0c263000ull
#define ANDR_TSENS1_TM_PA	0x0c265000ull
#define ANDR_TSENS0_GPU_SENSOR	15u
#define ANDR_TSENS1_GPU_SENSOR	11u

/* ---- GPU pool: contiguous DRAM the GPU and GMU use directly ----
 *
 * One 16 MB region (2 MB aligned) with a fixed internal layout (flame):
 *   + 0x000000  GMU page tables, icache/dcache windows, log, HFI queues
 *   + 0x0a0000  ZAP shader metadata handed to TrustZone
 *   + 0x100000  GPU page tables (L1, L2 of 2 MB blocks, then a 4K-table arena)
 *   + 0x200000  SQE microcode
 *   + 0x210000  ring buffer (32 KB)
 *   + 0x218000  scratch: fence (+0), CP_MEM_WRITE test target (+0x100)
 */
#define ANDR_POOL_SIZE		0x1000000ull
#define ANDR_POOL_ALIGN		0x200000ull
#define ANDR_OFF_ZAP_META	0x0a0000ull
#define ANDR_OFF_GT_L1		0x100000ull
#define ANDR_OFF_GT_L2		0x101000ull
#define ANDR_OFF_PT_ARENA	0x102000ull
#define ANDR_OFF_SQE		0x200000ull
#define ANDR_OFF_RING		0x210000ull
#define ANDR_OFF_SCRATCH	0x218000ull
#define ANDR_OFF_MEMTEST	(ANDR_OFF_SCRATCH + 0x100ull)	/* step 4 target */

#define ANDR_GVA_POOL		0x10000000ull	/* the pool's GPU virtual address */
#define ANDR_RING_SIZE		0x8000u
#define ANDR_RING_DW		(ANDR_RING_SIZE / 4)

/* GMU internal windows (flame): icache/dcache/log/HFI VAs and GMEM offsets. */
#define ANDR_GMEM_LOG_OFF	0x91000ull
#define ANDR_GMU_LOG_VA		0x60001000u
#define ANDR_GMU_LOG_SIZE	0x4000u

#define ANDR_FSR_FAULT		0xc00001feu

/* CP (PM4) opcodes used here. */
#define ANDR_CP_WAIT_FOR_IDLE	0x26u
#define ANDR_CP_MEM_WRITE	0x3du
#define ANDR_CP_ME_INIT		0x48u
#define ANDR_CP_SET_SECURE_MODE	0x66u

#define ANDR_MEMTEST_MAGIC	0xcafef00du
#define ANDR_GMU_BOOTED_MAGIC	0xbabefaceu

/* The firmware the driver loads, with the expected size and SHA-256 from
 * docs/research/gpu.md 6.3b (extracted from this phone, 2026-11-06; the files
 * live in inputs/andromeda/gpufw/, outside git). Only the identity is here,
 * never any firmware byte. */
enum andr_fw {
	ANDR_FW_GMU = 0,	/* a640_gmu.bin  37680 B */
	ANDR_FW_SQE,		/* a630_sqe.fw   32136 B */
	ANDR_FW_ZAP_MDT,	/* a640_zap.mdt   6860 B */
	ANDR_FW_ZAP_B02,	/* a640_zap.b02   1968 B */
	ANDR_FW_COUNT
};

/* Boot steps (highest value requested by andrgpu=N). */
enum andr_step {
	ANDR_STEP_NONE = 0,
	ANDR_STEP_PROBE = 1,
	ANDR_STEP_GMU = 2,
	ANDR_STEP_CP = 3,
	ANDR_STEP_MEMTEST = 4
};

struct andr_gpu {
	uint64_t pool_pa;
	int32_t gmu_smr, gmu_cb, gpu_smr, gpu_cb;
	uint32_t wptr;			/* ring write pointer, in dwords */
	uint32_t fence;			/* last fence value submitted */
	uint32_t hfi_seq;
	uint32_t pt_next;		/* next free 4K table in the arena */
	bool zap_authenticated;
};

/* ---- environment supplied by andromeda_gpu.c ---- */
uint32_t andr_rd(uint64_t pa);			/* 32-bit MMIO / pool read */
void andr_wr(uint64_t pa, uint32_t v);		/* 32-bit MMIO / pool write */
int andr_write(uint64_t pa, const void *buf, size_t len);	/* pool: bytes; MMIO: words */
int andr_read(uint64_t pa, void *buf, size_t len);
void andr_delay_ms(unsigned ms);
void andr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* TrustZone SMC, SMCCC: x0..x7 in, x0..x3 back. 0 result = success. */
void andr_smc(uint64_t x[8]);
/* Firmware from the ADT, SHA-256 verified; NULL if missing or the digest or
 * size does not match. Never returns an unverified image. */
const uint8_t *andr_firmware(enum andr_fw fw, uint32_t *len);
/* Max GPU-island temperature in m degC; false if nothing readable. */
bool andr_gpu_temperature(int *max_mc);

/* ---- core helpers (andromeda_gpu_core.c) ---- */
static inline uint64_t andr_pool_pa(const struct andr_gpu *g, uint64_t off) { return g->pool_pa + off; }
static inline uint64_t andr_pool_va(uint64_t off) { return ANDR_GVA_POOL + off; }

void andr_gpu_state_init(struct andr_gpu *g, uint64_t pool_pa);
bool andr_cx_up(void);
bool andr_gx_up(void);

/* Each returns 0 on success, -1 otherwise, and logs "ANDROMEDA GPU:" lines for
 * every register it touches. They run in order; a failing step stops the rest. */
int andr_gpu_probe(struct andr_gpu *g);		/* step 1: read-only */
int andr_power_cx(struct andr_gpu *g);		/* step 2a */
int andr_gmu_boot(struct andr_gpu *g);		/* step 2b: expect 0xBABEFACE */
int andr_gmu_hfi(struct andr_gpu *g);		/* step 3a: HFI tables + GX via OOB */
int andr_gpu_cp(struct andr_gpu *g);		/* step 3b: SMMU + hw_init + SQE + CP + ZAP */
int andr_cp_memtest(struct andr_gpu *g);	/* step 4: CP_MEM_WRITE 0xcafef00d */

#endif /* ANDROMEDA_GPU_H */
