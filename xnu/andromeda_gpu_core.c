/*
 * Adreno 640 bring-up sequence for andromeda (see andromeda_gpu.h).
 *
 * Ported from ios-flame's bsd/dev/sweet_a6xx.c (Nick Wylynko / NWylynko,
 * https://github.com/NWylynko/ios-flame, patch 0095-flame-gpu-present; MIT,
 * the patched XNU file is APSL 2.0). The register values and the order of
 * operations are flame's, which follow mainline Linux drm/msm (a6xx_gmu.c,
 * a6xx_hfi.c, a6xx_gpu.c), the a6xx.xml register database, gpucc-sm8150.c and
 * sm8150.dtsi. flame drives this same GPU on a Pixel 4 (same SM8150 + Adreno
 * 640 + ios-sweet xnu-8796 SWEET + iOS 16.7.16). ios-sweet is dipa-studio's.
 *
 * Adapted for andromeda: addresses come from andromeda_gpu.h (its own live
 * device tree), the one-shot power-up is split into the gated steps the
 * boot-arg andrgpu=N selects, and the TrustZone PAS path is preceded by an
 * is-call-available query and never calls PAS_SHUTDOWN (the "fail-closed"
 * caution of the Mi 9 port, YangQi0408 / UniXNU-cepheus, layered over flame's
 * proven sequence). Every loop is bounded; nothing spins forever. See
 * CREDITS.md.
 *
 * This file is pure register logic: it only calls the andr_* environment
 * hooks, so it builds both in-kernel and for the host syntax check.
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#ifdef KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
#include "andromeda_gpu.h"

#define rd32		andr_rd
#define wr32		andr_wr

static void wr64(uint64_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

/* Poll reg for (reg & mask) == (set ? mask-nonzero : 0) up to ms times, 1 ms
 * apart. Returns 0 if the condition was seen, -1 on timeout. Bounded. */
static int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (!!(rd32(pa) & mask) == set)
			return 0;
		if (i < ms)
			andr_delay_ms(1);
	}
	return -1;
}

static void zero_pa(uint64_t pa, uint64_t size)
{
	static const uint8_t z[0x4000];
	for (uint64_t o = 0; o < size; o += sizeof(z))
		andr_write(pa + o, z, (size_t)(size - o < sizeof(z) ? size - o : sizeof(z)));
}

/* ---- register offsets (words into their block; same SM8150 as flame) ---- */

#define GCC		ANDR_GCC
#define GPUCC		ANDR_GPUCC
#define GPU		ANDR_GPU_BASE
#define GREG(o)		ANDR_GREG(o)
#define GMU_REG(o)	ANDR_GMU_REG(o)
#define SMMU		ANDR_SMMU

#define GPUCC_CX_GDSCR	0x106c
#define GPUCC_GX_GDSCR	0x100c
#define GPUCC_GMU_RCG	0x1120
#define GPUCC_AHB	0x1078
#define GPUCC_CXO	0x109c
#define GPUCC_CX_GMU	0x1098

#define GMU_ITCM	0x1b400
#define GMU_DTCM	0x1c400
#define GMU_FW_VERSION	0x1d3f8
#define GMU_INIT_RESULT	0x1f81c
#define GMU_RPMH_STATE	0x1f8ec
#define GMU_GENERAL_7	0x1f9cc
#define GMU_SYSRESET	0x1f800
#define GMU_BOOT_CFG	0x1f801
#define GMU_CM3_CFG	0x1f82d
#define GMU_PWRCOL_CTRL	0x1f8c0
#define GMU_RPMH_CTRL	0x1f8e8
#define GMU_CP_MSG	0x1f900
#define GMU_CP_RESP	0x1f901
#define GMU_HFI_CTRL	0x1f980
#define GMU_SFR_ADDR	0x1f982
#define GMU_QTBL_INFO	0x1f984
#define GMU_QTBL_ADDR	0x1f985
#define GMU_SYS_BUS	0x1f40f
#define GMU_ICACHE_CFG	0x1f400
#define GMU_DCACHE_CFG	0x1f401
#define GMU_RSCC_REQ	0x23b07
#define GMU_RSCC_ACK	0x23b08
#define GMU_FENCE0	0x23b11
#define GMU_G2H_CLR	0x1f991
#define GMU_G2H_INFO	0x1f992
#define GMU_H2G_SET	0x1f994

#define SMMU_GR1	(SMMU + 0x1000)
#define SMMU_CB(n)	ANDR_SMMU_CB(n)
#define SMR(n)		(SMMU + 0x800 + 4 * (n))
#define S2CR(n)		(SMMU + 0xc00 + 4 * (n))
#define CBAR(n)		(SMMU_GR1 + 4 * (n))
#define CBA2R(n)	(SMMU_GR1 + 0x800 + 4 * (n))
#define CB_SCTLR	0x00
#define CB_TCR2		0x10
#define CB_TTBR0	0x20
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_FSR		0x58
#define CB_FAR		0x60
#define CB_FSYNR0	0x68
#define CB_TLBIALL	0x618
#define CB_TLBSYNC	0x7f0
#define CB_TLBSTATUS	0x7f4
/* TCR2: 40-bit PA, SEP = 7 (upstream bus size). SEP 0 (reset) makes VA bit 31
 * select TTBR1, which is off: every GPU VA >= 2 GB would fault at level 0. */
#define TCR2_VAL	((7u << 15) | 2u)

#define RBBM_STATUS		0x210
#define RBBM_INT_STATUS		0x201
#define CP_RB_BASE		0x800
#define CP_RB_CNTL		0x802
#define CP_RB_RPTR		0x806
#define CP_RB_WPTR		0x807
#define CP_SQE_CNTL		0x808
#define CP_HW_FAULT		0x821
#define CP_STATUS_1		0x825
#define CP_SQE_INSTR_BASE	0x830

/* GMEM (pool) windows the GMU reaches through its own SMMU. */
#define GMEM_L1(g)	andr_pool_pa(g, 0x0000)
#define GMEM_L2_0(g)	andr_pool_pa(g, 0x1000)
#define GMEM_L2_1(g)	andr_pool_pa(g, 0x2000)
#define GMEM_L3_LOW(g)	andr_pool_pa(g, 0x3000)
#define GMEM_L3_6(g)	andr_pool_pa(g, 0x4000)
#define GMEM_ICACHE(g)	andr_pool_pa(g, 0x10000)
#define GMEM_DCACHE(g)	andr_pool_pa(g, 0x50000)
#define GMEM_DUMMY(g)	andr_pool_pa(g, 0x90000)
#define GMEM_LOG(g)	andr_pool_pa(g, ANDR_GMEM_LOG_OFF)
#define GMEM_HFI(g)	andr_pool_pa(g, 0x95000)

#define VA_ICACHE	0x4000u
#define VA_DCACHE	0x44000u
#define CACHE_SIZE	0x3c000u
#define VA_DUMMY	0x60000000u
#define VA_LOG		ANDR_GMU_LOG_VA
#define VA_HFI		0x60005000u

#define RSCC_REG(o)	(ANDR_RSCC + 4ull * (o))
#define PDC_REG(o)	(ANDR_PDC + 4ull * (o))

static bool is_on(uint64_t gdscr)
{
	return (rd32(gdscr) >> 31) & 1;
}

static bool clk_on(uint64_t cbcr)
{
	return !(rd32(cbcr) >> 31);	/* bit 31 = CLK_OFF */
}

bool andr_cx_up(void)
{
	return is_on(GPUCC + GPUCC_CX_GDSCR) && clk_on(GPUCC + GPUCC_AHB) && clk_on(GPUCC + GPUCC_CX_GMU);
}

bool andr_gx_up(void)
{
	return andr_cx_up() && is_on(GPUCC + GPUCC_GX_GDSCR);
}

void andr_gpu_state_init(struct andr_gpu *g, uint64_t pool_pa)
{
	memset(g, 0, sizeof(*g));
	g->pool_pa = pool_pa;
	g->gmu_smr = g->gmu_cb = g->gpu_smr = g->gpu_cb = -1;
}

/* ---- step 1: read-only probe ---- */

int andr_gpu_probe(struct andr_gpu *g)
{
	(void)g;
	/*
	 * An unclocked register block can stall the bus, so read GPUCC only with
	 * GCC gpu_cfg_ahb running, and SMMU/GMU only with the CX GDSC on (the
	 * Mi 9 port's gpu_probe order, cepheus_soc.c / cepheus_gpu_power.h).
	 */
	uint32_t cfg_ahb = rd32(GCC + 0x71004);
	uint32_t ahb_state = cfg_ahb & 0xf0000000u;
	int ahb_on = cfg_ahb != 0xffffffffu && (!(ahb_state & 0x80000000u) || ahb_state == 0x20000000u);
	andr_log("probe: GCC gpu_cfg_ahb %x (%s)\n", cfg_ahb, ahb_on ? "running" : "off");
	uint32_t cx = 0, gx = 0;
	if (ahb_on) {
		cx = rd32(GPUCC + GPUCC_CX_GDSCR);
		gx = rd32(GPUCC + GPUCC_GX_GDSCR);
		andr_log("probe: GPUCC cx_gdsc %x gx_gdsc %x gmu_rcg %x ahb %x cxo %x cx_gmu %x\n",
		    cx, gx, rd32(GPUCC + GPUCC_GMU_RCG), rd32(GPUCC + GPUCC_AHB), rd32(GPUCC + GPUCC_CXO),
		    rd32(GPUCC + GPUCC_CX_GMU));
		andr_log("probe: CX %s, GX %s (bit31 of each GDSCR)\n",
		    (cx >> 31) & 1 ? "ON" : "off", (gx >> 31) & 1 ? "ON" : "off");
	} else {
		andr_log("probe: GPUCC not read (its AHB clock is off)\n");
	}
	int cx_on = ahb_on && ((cx >> 31) & 1);
	if (cx_on) {
		andr_log("probe: SMMU idr0 %x idr1 %x; CBAR0 %x\n", rd32(SMMU + 0x20), rd32(SMMU + 0x24),
		    rd32(CBAR(0)));
	}
	/* cmd-db locator: a valid table starts with magic 0x0c0330db at +4. */
	uint64_t db = rd32(ANDR_CMDDB_LOCATOR);
	uint32_t magic = rd32(db + 4);
	andr_log("probe: cmd-db locator -> %llx magic %x (%s)\n", (unsigned long long)db, magic,
	    magic == 0x0c0330db ? "ok" : "unexpected");
	/* The GMU CM3 is off before boot; read only the identity words, CX on. */
	if (cx_on) {
		andr_log("probe: GMU fw_version %x init_result %x rpmh_state %x\n",
		    rd32(GMU_REG(GMU_FW_VERSION)), rd32(GMU_REG(GMU_INIT_RESULT)), rd32(GMU_REG(GMU_RPMH_STATE)));
	} else {
		andr_log("probe: SMMU and GMU not read (CX GDSC off)\n");
	}

	int ok = 0;
	static const struct { enum andr_fw fw; const char *name; } fws[] = {
		{ ANDR_FW_GMU, "a640_gmu.bin" }, { ANDR_FW_SQE, "a630_sqe.fw" },
		{ ANDR_FW_ZAP_MDT, "a640_zap.mdt" }, { ANDR_FW_ZAP_B02, "a640_zap.b02" },
	};
	for (unsigned i = 0; i < sizeof(fws) / sizeof(fws[0]); i++) {
		uint32_t len = 0;
		const uint8_t *p = andr_firmware(fws[i].fw, &len);
		andr_log("probe: firmware %s %s (%u bytes)\n", fws[i].name,
		    p ? "found + SHA-256 OK" : "MISSING or digest mismatch", len);
		if (!p)
			ok = -1;
	}
	return ok;
}

/* ---- step 2a: clocks and CX power ---- */

static void branch_on(const char *name, uint64_t reg)
{
	wr32(reg, rd32(reg) | 1);
	int rc = wait_bit(reg, 1u << 31, 0, 10);
	andr_log("%s = %x%s\n", name, rd32(reg), rc ? " (still halted)" : "");
}

int andr_power_cx(struct andr_gpu *g)
{
	(void)g;
	andr_log("power_cx: before cx %x gx %x rcg %x/%x ahb %x cxo %x cx_gmu %x\n",
	    rd32(GPUCC + GPUCC_CX_GDSCR), rd32(GPUCC + GPUCC_GX_GDSCR), rd32(GPUCC + GPUCC_GMU_RCG),
	    rd32(GPUCC + GPUCC_GMU_RCG + 4), rd32(GPUCC + GPUCC_AHB), rd32(GPUCC + GPUCC_CXO),
	    rd32(GPUCC + GPUCC_CX_GMU));
	branch_on("gcc gpu_cfg_ahb", GCC + 0x71004);
	branch_on("gcc ddrss_gpu_axi", GCC + 0x71154);
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm_gfx", GCC + 0x71018);

	uint64_t gdsc = GPUCC + GPUCC_CX_GDSCR;
	wr32(gdsc, rd32(gdsc) & ~1u);			/* clear SW_COLLAPSE */
	int rc = wait_bit(gdsc, 1u << 31, 1, 100);	/* wait PWR_ON */
	andr_log("power_cx: cx gdsc -> %x %s\n", rd32(gdsc), rc ? "TIMEOUT" : "on");
	if (rc)
		return -1;

	uint64_t rcg = GPUCC + GPUCC_GMU_RCG;		/* GMU clock on XO */
	wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | 1u);
	wr32(rcg, rd32(rcg) | 1u);
	rc = wait_bit(rcg, 1, 0, 10);
	andr_log("power_cx: gmu rcg %x cfg %x %s\n", rd32(rcg), rd32(rcg + 4), rc ? "UPDATE TIMEOUT" : "");

	branch_on("gpucc ahb", GPUCC + GPUCC_AHB);
	branch_on("gpucc cxo", GPUCC + GPUCC_CXO);
	branch_on("gpucc cx_gmu", GPUCC + GPUCC_CX_GMU);
	if (!andr_cx_up()) {
		andr_log("power_cx: CX did not come up\n");
		return -1;
	}
	andr_log("power_cx: CX up; smmu idr0 %x\n", rd32(SMMU + 0x20));
	return 0;
}

/* ---- SMMU ---- */

/* 4K pages for the GMU windows; priv: AP=00 (GMU code), else AP=01. */
static void map_gmu_pages(uint64_t l3, uint32_t va, uint64_t pa, uint32_t size, int priv)
{
	for (uint32_t o = 0; o < size; o += 0x1000) {
		uint32_t idx = ((va + o) >> 12) & 0x1ff;
		wr64(l3 + 8 * idx, (pa + o) | (1u << 10) | (3u << 8) | (priv ? 0 : (1u << 6)) | 3u);
	}
}

/* T0SZ: 32 (4 GB) for the GMU, 25 (39-bit) for the GPU; walks start at level 1. */
static void smmu_bank(int smr, int cb, uint64_t ttbr0, uint32_t sid, uint32_t mask)
{
	uint32_t t0sz = sid == ANDR_GPU_SID ? 25 : 32;
	uint64_t c = SMMU_CB(cb);
	wr32(c + CB_SCTLR, 0);
	wr32(CBA2R(cb), 1);					/* AArch64 descriptors */
	wr32(CBAR(cb), (1u << 16) | (0xfu << 12) | (3u << 8) | (uint32_t)(cb + 1));
	wr32(c + CB_TCR2, TCR2_VAL);
	wr64(c + CB_TTBR0, ttbr0);
	wr32(c + CB_TCR, t0sz | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
	wr32(c + CB_MAIR0, 0x0000ff44);
	wr32(c + CB_FSR, 0xffffffff);
	wr32(c + CB_SCTLR, 1 | 2 | 4 | 0x20 | 0x40);		/* M TRE AFE CFRE CFIE */
	wr32(SMMU + 0x68, 0);					/* TLBIALLNSNH */
	wr32(SMMU + 0x70, 0);					/* TLBGSYNC */
	wait_bit(SMMU + 0x74, 1, 0, 10);
	wr32(S2CR(smr), (uint32_t)cb);
	wr32(SMR(smr), (1u << 31) | (mask << 16) | sid);
	andr_log("smmu: smr%d %x -> cb%d cbar %x sctlr %x tcr %x tcr2 %x\n", smr, rd32(SMR(smr)), cb,
	    rd32(CBAR(cb)), rd32(c + CB_SCTLR), rd32(c + CB_TCR), rd32(c + CB_TCR2));
}

/* Pick a free stream-match register and context bank at runtime (flame): never
 * reuse one the bootloader left translating. */
static int smmu_pick(int32_t *smr_out, int32_t *cb_out, int cb_min)
{
	int smr = -1, cb = -1;
	uint32_t used_cb = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t m = rd32(SMR(i)), c = rd32(S2CR(i));
		if ((m >> 31) && ((c >> 16) & 3) == 0)
			used_cb |= 1u << (c & 0xff);
		if (smr < 0 && !(m >> 31))
			smr = i;
	}
	for (int i = cb_min; i < 5; i++)
		if (cb < 0 && !(used_cb & (1u << i)) && !(rd32(SMMU_CB(i) + CB_SCTLR) & 1))
			cb = i;
	*smr_out = smr;
	*cb_out = cb;
	return smr < 0 || cb < 0 ? -1 : 0;
}

/* ---- step 2b: GMU boot ---- */

static int gmu_smmu(struct andr_gpu *g)
{
	if (smmu_pick(&g->gmu_smr, &g->gmu_cb, 0)) {
		andr_log("smmu: no free smr/cb for the GMU\n");
		return -1;
	}
	zero_pa(g->pool_pa, 0x100000);
	wr64(GMEM_L1(g) + 0, GMEM_L2_0(g) | 3);
	wr64(GMEM_L1(g) + 8, GMEM_L2_1(g) | 3);
	wr64(GMEM_L2_0(g) + 0, GMEM_L3_LOW(g) | 3);
	wr64(GMEM_L2_1(g) + 8 * ((VA_DUMMY >> 21) & 0x1ff), GMEM_L3_6(g) | 3);
	map_gmu_pages(GMEM_L3_LOW(g), VA_ICACHE, GMEM_ICACHE(g), CACHE_SIZE, 1);
	map_gmu_pages(GMEM_L3_LOW(g), VA_DCACHE, GMEM_DCACHE(g), CACHE_SIZE, 1);
	map_gmu_pages(GMEM_L3_6(g), VA_DUMMY, GMEM_DUMMY(g), 0x1000, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_LOG, GMEM_LOG(g), ANDR_GMU_LOG_SIZE, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_HFI, GMEM_HFI(g), 0x4000, 0);
	smmu_bank(g->gmu_smr, g->gmu_cb, GMEM_L1(g), ANDR_GMU_SID, ANDR_GMU_SID_MASK);
	return 0;
}

/* a6xx_gmu_rpmh_init for the A640 (pdc_address_offset 0x30090). */
static void rpmh_init(void)
{
	static const uint32_t rscc[][2] = {
		{ 0x0004, 1u << 24 }, { 0x000b, 1 }, { 0x000e, 0 }, { 0x000d, 0 }, { 0x0010, 0 }, { 0x000f, 0 },
		{ 0x0012, 0x80000000 }, { 0x0011, 0 }, { 0x0100, 0 }, { 0x0008, 0x4520 }, { 0x0009, 0x4510 },
		{ 0x000a, 0x4514 }, { 0x0180, 0xa7a506a0 }, { 0x0181, 0xa1e6a6e7 }, { 0x0182, 0xa2e081e1 },
		{ 0x0183, 0xe9a982e2 }, { 0x0184, 0x0020e8a8 },
	};
	static const uint32_t seq[] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	static const uint32_t pdc[][2] = {
		{ 0x1573, 7 }, { 0x1574, 0 }, { 0x1572, 0 }, { 0x1575, 0x10108 }, { 0x1576, 0x30010 },
		{ 0x1577, 1 }, { 0x1579, 0x10108 }, { 0x157a, 0x30000 }, { 0x157b, 0 }, { 0x157d, 0x10108 },
		{ 0x157e, 0x30090 }, { 0x157f, 0 },
		{ 0x15d7, 7 }, { 0x15d8, 0 }, { 0x15d6, 0 }, { 0x15d9, 0x10108 }, { 0x15da, 0x30010 },
		{ 0x15db, 2 }, { 0x15dd, 0x10108 }, { 0x15de, 0x30000 }, { 0x15df, 3 }, { 0x15e1, 0x10108 },
		{ 0x15e2, 0x30090 }, { 0x15e3, 3 },
		{ 0x1148, 0 }, { 0x1140, 0x80000001 },
	};
	/* andromeda 2026-11-07: step 2 hung the CPU between the GMU SMMU bank and
	 * the firmware load (no further log, that CPU took no more interrupts):
	 * name each register block before touching it. */
	andr_log("gmu: rscc %llx: %u writes\n", (unsigned long long)RSCC_REG(0),
	    (unsigned)(sizeof(rscc) / sizeof(rscc[0])));
	for (unsigned i = 0; i < sizeof(rscc) / sizeof(rscc[0]); i++)
		wr32(RSCC_REG(rscc[i][0]), rscc[i][1]);
	andr_log("gmu: pdc seq %llx: %u writes\n", (unsigned long long)ANDR_PDC_SEQ,
	    (unsigned)(sizeof(seq) / sizeof(seq[0])));
	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
		wr32(ANDR_PDC_SEQ + 4 * i, seq[i]);
	andr_log("gmu: pdc cfg %llx: %u writes\n", (unsigned long long)PDC_REG(0),
	    (unsigned)(sizeof(pdc) / sizeof(pdc[0])));
	for (unsigned i = 0; i < sizeof(pdc) / sizeof(pdc[0]); i++)
		wr32(PDC_REG(pdc[i][0]), pdc[i][1]);
	andr_log("gmu: rpmh init written\n");
}

static void hfi_table(struct andr_gpu *g)
{
	uint64_t t = GMEM_HFI(g);
	wr32(t + 0, 0);
	wr32(t + 4, 6 * 4 + 3 * 12 * 4);
	wr32(t + 8, 6);
	wr32(t + 12, 12);
	wr32(t + 16, 3);
	wr32(t + 20, 3);
	for (uint32_t q = 0; q < 3; q++) {
		uint64_t h = t + 24 + 48 * q;
		wr32(h + 0, 1);
		wr32(h + 4, VA_HFI + 0x1000 * (q + 1));
		wr32(h + 8, (10u << 8) | q);
		wr32(h + 12, 0x1000 >> 2);
		wr32(h + 24, 1);
		wr32(h + 28, 1);
		wr32(h + 32, 1);
	}
}

static int fw_load(struct andr_gpu *g)
{
	uint32_t len;
	const uint8_t *fw = andr_firmware(ANDR_FW_GMU, &len);
	if (!fw)
		return -1;
	const uint8_t *p = fw, *end = fw + len;
	int n = 0;
	while (p + 16 <= end) {
		uint32_t addr, size;
		memcpy(&addr, p, 4);
		memcpy(&size, p + 4, 4);
		const uint8_t *data = p + 16;
		uint64_t dst = 0;
		if (addr < 0x4000)
			dst = GMU_REG(GMU_ITCM) + addr;
		else if (addr >= 0x40000 && addr < 0x44000)
			dst = GMU_REG(GMU_DTCM) + (addr - 0x40000);
		else if (addr >= VA_ICACHE && addr + size <= VA_ICACHE + CACHE_SIZE)
			dst = GMEM_ICACHE(g) + (addr - VA_ICACHE);
		else if (addr >= VA_DCACHE && addr + size <= VA_DCACHE + CACHE_SIZE)
			dst = GMEM_DCACHE(g) + (addr - VA_DCACHE);
		if (!dst || data + size > end) {
			andr_log("gmu fw: bad block %x size %x\n", addr, size);
			return -1;
		}
		andr_log("gmu fw: block %d addr %x size %x -> %llx\n", n, addr, size, (unsigned long long)dst);
		andr_write(dst, data, (size + 3) & ~3u);
		p = data + size;
		n++;
	}
	return n;
}

int andr_gmu_boot(struct andr_gpu *g)
{
	if (gmu_smmu(g))
		return -1;
	andr_log("gmu: smmu bank set; rpmh init next\n");
	rpmh_init();
	hfi_table(g);
	andr_log("gmu: hfi table written; general_7 %llx\n", (unsigned long long)GMU_REG(GMU_GENERAL_7));

	wr32(GMU_REG(GMU_GENERAL_7), 1);
	andr_log("gmu: rscc req %llx\n", (unsigned long long)GMU_REG(GMU_RSCC_REQ));
	wr32(GMU_REG(GMU_RSCC_REQ), 1u << 1);
	int ack = wait_bit(GMU_REG(GMU_RSCC_ACK), 1u << 1, 1, 200);	/* never acks; harmless */
	int seq = wait_bit(RSCC_REG(0x0101), 0xffffffff, 0, 10);
	wr32(GMU_REG(GMU_RSCC_REQ), 0);
	andr_log("gmu: rscc ack %s, seq busy %s; firmware load next (itcm %llx)\n",
	    ack ? "timeout" : "ok", seq ? "timeout" : "idle", (unsigned long long)GMU_REG(GMU_ITCM));

	int nblk = fw_load(g);
	andr_log("gmu: %d firmware blocks loaded\n", nblk);
	if (nblk <= 0)
		return -1;

	wr32(GMU_REG(GMU_INIT_RESULT), 0);
	wr32(GMU_REG(GMU_BOOT_CFG), 2);
	wr32(GMU_REG(GMU_QTBL_ADDR), VA_HFI);
	wr32(GMU_REG(GMU_QTBL_INFO), 1);
	wr32(GMU_REG(GMU_FENCE0), (1u << 31) | (0xau << 18) | 0xa0);
	wr32(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr32(GMU_REG(GMU_SFR_ADDR), 0x06040100);
	for (uint32_t i = 0; i < 4; i++)
		wr32(GPU + 4 * (0x3c03 + i), 0x00071620);
	wr32(GMU_REG(GMU_CP_MSG), VA_LOG | (ANDR_GMU_LOG_SIZE / 0x1000 - 1));
	wr32(GMU_REG(GMU_SYS_BUS), 1);
	wr32(GMU_REG(GMU_ICACHE_CFG), 1);
	wr32(GMU_REG(GMU_DCACHE_CFG), 1);
	wr32(GMU_REG(GMU_PWRCOL_CTRL), 0x9c40400);
	wr32(GMU_REG(GMU_RPMH_CTRL), rd32(GMU_REG(GMU_RPMH_CTRL)) |
	    1 | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));

	uint32_t v = rd32(GMU_REG(GMU_DTCM) + 0xff8 * 4);
	uint32_t mask = v <= 0x20010004 ? 0xffffffff : 0x1ff;
	uint32_t want = v <= 0x20010004 ? ANDR_GMU_BOOTED_MAGIC : 0x100;
	wr32(GMU_REG(GMU_SYSRESET), 1);
	wr32(GMU_REG(GMU_CP_RESP), 0);
	wr32(GMU_REG(GMU_SYSRESET), 0);
	uint32_t r = 0;
	bool ok = false;
	for (int i = 0; i < 2000 && !ok; i++) {
		r = rd32(GMU_REG(GMU_INIT_RESULT));
		ok = (r & mask) == want;
		if (!ok)
			andr_delay_ms(1);
	}
	andr_log("gmu: init result %x %s (want %x), hfi_ctrl %x\n", r, ok ? "BOOTED" : "timeout", want,
	    rd32(GMU_REG(GMU_HFI_CTRL)));
	return ok ? 0 : -1;
}

/* ---- step 3a: HFI, GX power ---- */

#define HFI_Q_CMD	0
#define HFI_Q_RESP	1
#define QHDR(g, q)	(GMEM_HFI(g) + 24 + 48 * (q))
#define QDATA(g, q)	(GMEM_HFI(g) + 0x1000 * ((q) + 1))
#define QSIZE		1024

#define LVL_MIN_SVS	48
#define LVL_LOW_SVS	64
#define LVL_SVS		128
#define LVL_SVS_L1	192
#define LVL_SVS_L2	224
#define LVL_NOM		256

/* A640 GPU levels 257..585 MHz (live-fdt.dts qcom,gpu-pwrlevels-0), GMU 200. */
static const uint32_t gpu_khz[] = { 0, 257000, 345000, 427000, 499200, 585000 };
static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_SVS_L2, LVL_NOM };
static const uint32_t gmu_khz[] = { 0, 200000 };
static const uint32_t gmu_lvl[] = { 0, LVL_MIN_SVS };

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(ANDR_CMDDB_LOCATOR);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

static int arc_votes(uint32_t *votes, const uint32_t *lvl, int n, const char *pri_id, const char *sec_id)
{
	uint64_t pri, sec;
	uint32_t plen, slen;
	if (cmddb_aux(pri_id, &pri, &plen) || cmddb_aux(sec_id, &sec, &slen))
		return -1;
	plen /= 2;
	slen /= 2;
	for (int i = 0; i < n; i++) {
		uint32_t pindex = 0, sindex = 0, j;
		for (j = 0; j < plen; j++)
			if (aux16(pri, j) >= lvl[i]) {
				pindex = j;
				break;
			}
		if (j == plen)
			return -1;
		for (j = 0; j < slen; j++) {
			if (aux16(sec, j) >= lvl[i]) {
				sindex = j;
				break;
			} else if (aux16(sec, j)) {
				sindex = j;
			}
		}
		votes[i] = (aux16(pri, pindex) << 16) | (sindex << 8) | pindex;
	}
	return 0;
}

static int hfi_send(struct andr_gpu *g, uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t seq = ++g->hfi_seq;
	msg[0] = (seq << 20) | (dwords << 8) | id;
	uint64_t h = QHDR(g, HFI_Q_CMD);
	uint32_t w = rd32(h + 44);
	for (uint32_t i = 0; i < dwords; i++) {
		wr32(QDATA(g, HFI_Q_CMD) + 4 * w, msg[i]);
		w = (w + 1) % QSIZE;
	}
	for (; w % 4; w = (w + 1) % QSIZE)
		wr32(QDATA(g, HFI_Q_CMD) + 4 * w, 0xfafafafa);
	wr32(h + 44, w);
	wr32(GMU_REG(GMU_H2G_SET), 1);

	for (int t = 0; t < 1000; t++) {
		if (rd32(GMU_REG(GMU_G2H_INFO)) & 1) {
			wr32(GMU_REG(GMU_G2H_CLR), 1);
			uint64_t rh = QHDR(g, HFI_Q_RESP);
			while (rd32(rh + 40) != rd32(rh + 44)) {
				uint32_t r = rd32(rh + 40), hdr = rd32(QDATA(g, HFI_Q_RESP) + 4 * r);
				uint32_t nn = (hdr >> 8) & 0xff, buf[24] = { 0 };
				for (uint32_t i = 0; i < nn && i < 24; i++) {
					buf[i] = rd32(QDATA(g, HFI_Q_RESP) + 4 * r);
					r = (r + 1) % QSIZE;
				}
				r = ((r + 3) & ~3u) % QSIZE;
				wr32(rh + 40, r);
				if ((hdr & 0xff) != 100 && (buf[1] >> 20) == seq)
					return (int)buf[2];
			}
		}
		andr_delay_ms(1);
	}
	andr_log("hfi: msg %u seq %u: no ack (info %x)\n", id, seq, rd32(GMU_REG(GMU_G2H_INFO)));
	return -1;
}

int andr_gmu_hfi(struct andr_gpu *g)
{
	static uint32_t msg[256];
	uint32_t gx[6], cx[2];
	if (arc_votes(gx, gpu_lvl, 6, "gfx.lvl", "mx.lvl") || arc_votes(cx, gmu_lvl, 2, "cx.lvl", "mx.lvl")) {
		andr_log("hfi: cmd-db levels not found\n");
		return -1;
	}
	memset(msg, 0, sizeof(msg));
	msg[1] = 6;
	msg[2] = 2;
	for (int i = 0; i < 6; i++) {
		msg[3 + 3 * i] = gx[i];
		msg[4 + 3 * i] = 0xffffffff;
		msg[5 + 3 * i] = gpu_khz[i];
	}
	for (int i = 0; i < 2; i++) {
		msg[3 + 48 + 2 * i] = cx[i];
		msg[4 + 48 + 2 * i] = gmu_khz[i];
	}
	int rc = hfi_send(g, 4, msg, 3 + 48 + 8);

	memset(msg, 0, sizeof(msg));
	static const uint32_t bw[][2] = {
		{ 1, 1 }, { 2, 3 }, { 3, 3 }, { 4, 1 }, { 5, 1 }, { 6, 0x50034 }, { 7, 0x5007c }, { 8, 0x5004c },
		{ 12, 0x40000000 }, { 14, 0x40000000 }, { 18, 0x60000001 }, { 19, 0x20000001 }, { 20, 0x60000001 },
		{ 24, 0x50000 }, { 25, 0x5003c }, { 26, 0x5000c }, { 32, 0x40000000 }, { 33, 0x40000000 },
		{ 34, 0x40000000 },
	};
	for (unsigned i = 0; i < sizeof(bw) / sizeof(bw[0]); i++)
		msg[bw[i][0]] = bw[i][1];
	rc |= hfi_send(g, 3, msg, 32 + 128);
	msg[1] = 0;
	rc |= hfi_send(g, 14, msg, 2);
	rc |= hfi_send(g, 10, msg, 1);
	uint32_t lvl = g->perf_level;
	if (lvl < 1 || lvl >= sizeof(gpu_khz) / sizeof(gpu_khz[0]))
		lvl = 1;		/* 257 MHz */
	msg[1] = 1;
	msg[2] = lvl;
	msg[3] = 0;
	rc |= hfi_send(g, 30, msg, 4);
	andr_log("hfi: perf/bw tables, core fw start, start, perf vote %u MHz (level %u) -> %d\n",
	    gpu_khz[lvl] / 1000, lvl, rc);

	wr32(GMU_REG(GMU_H2G_SET), 1u << 30);	/* OOB GPU_SET: the GMU raises GX */
	int ack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << 31, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << 31);
	andr_log("hfi: oob gpu_set %s, gx gdsc %x\n", ack ? "TIMEOUT" : "acked", rd32(GPUCC + GPUCC_GX_GDSCR));
	if (!andr_gx_up()) {
		andr_log("hfi: GX not powered; not touching GPU registers\n");
		return -1;
	}
	andr_log("hfi: GX up, RBBM_STATUS %x\n", rd32(GREG(RBBM_STATUS)));
	return 0;
}

/* ---- ZAP shader through TrustZone PAS ----
 *
 * SMCCC SIP calls (flame): the PIL service is a yielding SMC64 call that can be
 * interrupted (andr_smc resumes it). Before any of them, andromeda first asks
 * whether the PIL auth call is implemented at all (the Mi 9 safeguard), and
 * never calls PAS_SHUTDOWN (it hung vayu). If auth is not available, the step
 * stops without touching the secure world further. */
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))	/* SIP, PIL svc 2, yielding */
#define SCM_IS_AVAIL	0xc2000601ull				/* SIP info: is-call-available (fast) */
#define PIL_INIT_IMAGE	1
#define PIL_MEM_SETUP	2
#define PIL_AUTH_RESET	5

static int64_t scm(uint64_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { fn, arginfo, a, b, c, 0, 0, 0 };
	andr_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static bool pas_auth_available(void)
{
	uint64_t r1 = 0;
	int64_t rc = scm(SCM_IS_AVAIL, 1, SCM_PIL(PIL_AUTH_RESET), 0, 0, &r1);
	andr_log("zap: is-call-available(auth) rc %lld avail %llu\n", (long long)rc, (unsigned long long)r1);
	/* Definite "not implemented": the query returned cleanly and said 0. Any
	 * other outcome is treated as "proceed, but each PAS call still checks". */
	return !(rc == 0 && r1 == 0);
}

static int zap_load(struct andr_gpu *g)
{
	if (!pas_auth_available()) {
		andr_log("zap: PIL auth not available; leaving the CP in secure mode\n");
		return -1;
	}
	uint64_t r1 = 0;
	uint32_t mlen, slen;
	const uint8_t *mdt = andr_firmware(ANDR_FW_ZAP_MDT, &mlen);
	const uint8_t *b02 = andr_firmware(ANDR_FW_ZAP_B02, &slen);
	if (!mdt || !b02 || slen > ANDR_ZAP_SIZE)
		return -1;
	andr_write(andr_pool_pa(g, ANDR_OFF_ZAP_META), mdt, mlen);
	int64_t rc = scm(SCM_PIL(PIL_INIT_IMAGE), 2 | (2u << 6), ANDR_ZAP_PAS,
	    andr_pool_pa(g, ANDR_OFF_ZAP_META), 0, &r1);
	if (rc || r1) {
		andr_log("zap: init image rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	rc = scm(SCM_PIL(PIL_MEM_SETUP), 3, ANDR_ZAP_PAS, ANDR_ZAP_REGION, ANDR_ZAP_SIZE, &r1);
	if (rc || r1) {
		andr_log("zap: mem setup rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	static uint8_t seg[ANDR_ZAP_SIZE];
	memset(seg, 0, sizeof(seg));
	memcpy(seg, b02, slen);
	andr_write(ANDR_ZAP_REGION, seg, ANDR_ZAP_SIZE);
	rc = scm(SCM_PIL(PIL_AUTH_RESET), 1, ANDR_ZAP_PAS, 0, 0, &r1);
	andr_log("zap: auth and reset rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	g->zap_authenticated = true;
	return 0;
}

/* ---- ring ---- */

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
	    (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000 | cnt | (pm4_parity(cnt) << 15) | ((op & 0x7f) << 16) | (pm4_parity(op) << 23);
}

static int ring(struct andr_gpu *g, const uint32_t *dw, uint32_t n, const char *what, unsigned timeout_ms)
{
	if (n >= ANDR_RING_DW)
		return -1;
	uint32_t w = g->wptr, first = n < ANDR_RING_DW - w ? n : ANDR_RING_DW - w;
	andr_write(andr_pool_pa(g, ANDR_OFF_RING) + 4ull * w, dw, 4ull * first);
	if (first < n)
		andr_write(andr_pool_pa(g, ANDR_OFF_RING), dw + first, 4ull * (n - first));
	g->wptr = (w + n) % ANDR_RING_DW;
	wr32(GREG(CP_RB_WPTR), g->wptr);
	unsigned i;
	for (i = 0; i < timeout_ms * 10; i++) {
		if (rd32(GREG(CP_RB_RPTR)) == g->wptr && !(rd32(GREG(RBBM_STATUS)) & (1u << 23)))
			break;
		if ((i & 7) == 7)
			andr_delay_ms(1);
	}
	bool ok = i < timeout_ms * 10;
	if (what || !ok)
		andr_log("ring %s: rptr %x wptr %x status %x int %x hwfault %x cp_status1 %x %s\n",
		    what ? what : "", rd32(GREG(CP_RB_RPTR)), g->wptr, rd32(GREG(RBBM_STATUS)),
		    rd32(GREG(RBBM_INT_STATUS)), rd32(GREG(CP_HW_FAULT)), rd32(GREG(CP_STATUS_1)),
		    ok ? "idle" : "TIMEOUT");
	return ok ? 0 : -1;
}

/* ---- step 3b: GPU SMMU bank, hw_init, SQE, CP, ZAP ---- */

static void map_gpu_low(struct andr_gpu *g)
{
	uint64_t l1 = andr_pool_pa(g, ANDR_OFF_GT_L1), l2 = andr_pool_pa(g, ANDR_OFF_GT_L2);
	zero_pa(l1, 0x2000);
	wr64(l1 + 0, l2 | 3);		/* VA 0-1 GB */
	for (uint64_t o = 0; o < ANDR_POOL_SIZE; o += 0x200000)		/* AP=01, NC, 2 MB blocks */
		wr64(l2 + 8 * (((ANDR_GVA_POOL + o) >> 21) & 0x1ff),
		    (g->pool_pa + o) | (1u << 10) | (3u << 8) | (1u << 6) | 1u);
}

int andr_gpu_cp(struct andr_gpu *g)
{
	if (smmu_pick(&g->gpu_smr, &g->gpu_cb, g->gmu_cb + 1)) {
		andr_log("smmu: no free smr/cb for the GPU\n");
		return -1;
	}
	map_gpu_low(g);
	smmu_bank(g->gpu_smr, g->gpu_cb, andr_pool_pa(g, ANDR_OFF_GT_L1), ANDR_GPU_SID, ANDR_GPU_SID_MASK);

	/* a6xx hw_init for the A640 (a6xx_gpu.c), minus clock gating, CP protection
	 * and interrupts (flame). highest bank bit 15, UBWC 3 (live-fdt.dts). */
	wr32(GREG(0x3c45), 0);			/* GBIF_HALT */
	(void)rd32(GREG(0x3c45));
	wr32(GREG(0x16), 0);			/* RBBM_GBIF_HALT */
	(void)rd32(GREG(0x16));
	static const uint32_t init[][2] = {
		{ 0xf803, 0 }, { 0xf800, 0 }, { 0xf801, 0 }, { 0xf802, 0 },	/* SECVID TSB */
		{ 0x0842, 1 }, { 0x0c01, 1 }, { 0x8601, 1 }, { 0x8e05, 1 }, { 0x9e01, 1 }, { 0xbe05, 1 },
		{ 0xa601, 1 }, { 0x9601, 1 }, { 0x0e00, 1 }, { 0xae01, 1 }, { 0xb601, 1 }, { 0xf810, 1 },
		{ 0x0011, 3 },				/* RBBM_GBIF_CLIENT_QOS_CNTL */
		{ 0x050b, 0xffffffff },			/* RBBM_PERFCTR_GPU_BUSY_MASKED */
		{ 0x0e05, 0xffffffc0 }, { 0x0e06, 0x1ffff }, { 0x0e09, 0xfffff000 }, { 0x0e0a, 0x1ffff },
		{ 0x0e07, 0xfffff000 }, { 0x0e08, 0x1ffff },
		{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x1fffff }, { 0x0e0e, 0 },	/* GMEM range */
		{ 0x0e18, 0x804 }, { 0x0e17, 0x4 },	/* UCHE_FILTER_CNTL, CACHE_WAYS */
		{ 0x08c2, 0x02000140 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },	/* ROQ, mem pool */
		{ 0x098d, 1 }, { 0x0500, 1 },		/* CP_AHB_CNTL, RBBM_PERFCTR_CNTL */
		{ 0x001f, (1u << 30) | 0x1fffff },	/* RBBM_INTERFACE_HANG_INT_CNTL */
		{ 0x0e19, (1u << 7) | 1 },		/* UCHE_CLIENT_PF */
		{ 0x9e00, 0x00180000 },			/* PC_DBG_ECO_CNTL: prim_fifo_threshold */
		{ 0x08d0, 0 },				/* CP_PERFCTR_CP_SEL[0] */
		{ 0x8e08, (1u << 4) | (2u << 1) }, { 0xb604, 2u << 1 }, { 0xae02, 2u << 1 },	/* UBWC hbb 15 */
		{ 0x0e01, 2u << 21 }, { 0x0534, 0 },
	};
	for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++)
		wr32(GREG(init[i][0]), init[i][1]);

	uint32_t len;
	const uint8_t *sqe = andr_firmware(ANDR_FW_SQE, &len);
	if (!sqe || len < 8 || len > 0x10000)
		return -1;
	andr_write(andr_pool_pa(g, ANDR_OFF_SQE), sqe + 4, (len - 4 + 3) & ~3u);	/* drop first dword */
	wr32(GREG(CP_SQE_INSTR_BASE), (uint32_t)andr_pool_va(ANDR_OFF_SQE));
	wr32(GREG(CP_SQE_INSTR_BASE + 1), 0);
	wr32(GREG(CP_RB_BASE), (uint32_t)andr_pool_va(ANDR_OFF_RING));
	wr32(GREG(CP_RB_BASE + 1), 0);
	wr32(GREG(CP_RB_CNTL), 0x0800020c);	/* 32K ring, 32-byte blocks, no rptr update */
	g->wptr = 0;
	wr32(GREG(CP_SQE_CNTL), 1);

	uint32_t me_init[] = { pkt7(ANDR_CP_ME_INIT, 8), 0x2f, 3, 0x20000000, 0, 0, 0, 0, 0 };
	if (ring(g, me_init, 9, "me_init", 1000))
		return -1;
	if (zap_load(g))
		return -1;
	uint32_t secure[] = { pkt7(ANDR_CP_SET_SECURE_MODE, 1), 0 };
	int rc = ring(g, secure, 2, "secure mode off", 1000);
	andr_log("cp: %s (sqe base %x, zap %s)\n", rc ? "FAILED" : "running",
	    rd32(GREG(CP_SQE_INSTR_BASE)), g->zap_authenticated ? "authenticated" : "not loaded");
	return rc;
}

/* ---- step 4: CP_MEM_WRITE test ---- */

int andr_cp_memtest(struct andr_gpu *g)
{
	if (!g->zap_authenticated || !andr_gx_up()) {
		andr_log("memtest: CP not ready (gx %d, zap %d)\n", andr_gx_up(), g->zap_authenticated);
		return -1;
	}
	uint32_t zero = 0;
	andr_write(andr_pool_pa(g, ANDR_OFF_MEMTEST), &zero, 4);
	uint64_t va = andr_pool_va(ANDR_OFF_MEMTEST);
	uint32_t dw[] = {
		pkt7(ANDR_CP_MEM_WRITE, 3), (uint32_t)va, (uint32_t)(va >> 32), ANDR_MEMTEST_MAGIC,
		pkt7(ANDR_CP_WAIT_FOR_IDLE, 0),
	};
	int rc = ring(g, dw, sizeof(dw) / 4, "cp_mem_write", 1000);
	uint32_t got = 0;
	andr_read(andr_pool_pa(g, ANDR_OFF_MEMTEST), &got, 4);
	andr_log("memtest: wrote %x via CP_MEM_WRITE, read back %x (%s)\n", ANDR_MEMTEST_MAGIC, got,
	    (!rc && got == ANDR_MEMTEST_MAGIC) ? "PASS" : "FAIL");
	return (!rc && got == ANDR_MEMTEST_MAGIC) ? 0 : -1;
}
#endif /* !KERNEL || ARM64_BOARD_CONFIG_SWEET */
