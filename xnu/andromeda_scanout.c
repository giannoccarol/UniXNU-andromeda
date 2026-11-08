/*
 * andromeda: the display reads iOS's frames itself (patch 0207, andrscanout).
 * What, why and whose design: andromeda_scanout.h.
 *
 * The hardware as the ABL leaves it and as andctl dpu read it on 2026-11-08:
 *
 *   MDP 0x0ae01000 (DPU 5.0): VIG0 (+0x4000) fetches the panel memory
 *   0x9c000000 (1080x2340, stride 4320, ARGB8888 = iOS's BGRA, no scaling)
 *   for CTL0 -> LM0 -> INTF1 (DSI0), command mode; INTF1's tear check is on
 *   (+0x284 = 1) with autorefresh every frame (+0x2b4 = 0x80000001), so the
 *   panel is refreshed without a CTL_START and a CTL flush is taken at the
 *   next frame. Register offsets: Linux dpu1 (andromeda_scanout.h).
 *
 *   apps SMMU 0x15000000 (MMU-500, 4 KiB pages, 128 global pages, 56 banks):
 *   SMR 3 = 0x84200800 (SID 0x800 mask 0x420, the DT's display stream),
 *   S2CR 3 = 0x00000002 (translate, bank 2), bank 2 with SCTLR.M = 0: the
 *   pipe reads physical addresses, and only below 4 GiB. iOS's surfaces are
 *   wired kernel memory anywhere in RAM.
 *
 * Bank 2 gets 4 KiB stage-1 tables with a 32-bit input space (start level 1):
 * the panel memory at its own address (the ABL's frame, the console, the
 * blank of display off), surfaces in 16 MiB slots from 0x80000000. Leaves
 * are read-only, non-cacheable (MAIR attr 0 = 0x44), non-global (ASID = the
 * bank, ASIDPNE), the walk is non-cacheable, so every table write is cleaned
 * to memory. The register values are Linux arm-smmu's for a stage-1 bank
 * (as cepheus_smmu_plan.h's GMU bank): CBAR type 1 with BPSHCFG/MEMATTR,
 * CBA2R AArch64, TCR T0SZ 32 / SH0 outer / TG0 4K / EPD1, TCR2 PASize 40 bits.
 * If the bank faults or a flush is not taken within 100 ms of turning
 * translation on, everything goes back as it was (SCTLR.M = 0) and the
 * framebuffer keeps its CPU copy for the boot.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ANDR_SCANOUT_HOST)
#if defined(KERNEL)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

extern vm_offset_t andromeda_io_map(vm_offset_t, vm_size_t); /* andromeda_probe.c: shared, outside io_submap */
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern void IOSleep(unsigned int milliseconds);
#endif	/* (the host test, test_andromeda_scanout.c, declares the environment itself) */
#include "andromeda_scanout.h"

#define SC_TAG            "ANDROMEDA SCANOUT:"

/* MDP (DT mdss_mdp reg "mdp"); block offsets are Linux's catalog dpu_5_0_sm8150.h */
#define SC_MDP_PA         0x0ae01000u
#define SC_MDP_LEN        0x00090000u
#define SC_VIG0           0x4000u
#define SC_QSEED          (SC_VIG0 + 0xa00u)
#define SC_CTL0           0x1000u
#define SC_INTF1          0x6a800u
/* SSPP (dpu_hw_sspp.c) */
#define SS_SRC_SIZE       0x00u
#define SS_SRC_XY         0x08u
#define SS_OUT_SIZE       0x0cu
#define SS_OUT_XY         0x10u
#define SS_SRC0_ADDR      0x14u
#define SS_YSTRIDE0       0x24u
#define SS_SRC_FORMAT     0x30u
#define SS_OP_MODE        0x38u
#define SS_PE_C0_LR       0x100u
#define SS_PE_C0_TB       0x104u
#define SS_PE_C0_REQ      0x108u
#define SS_PE_C12_LR      0x110u
#define SS_PE_C12_TB      0x114u
#define SS_PE_C12_REQ     0x118u
#define SS_PE_C3_LR       0x120u
#define SS_PE_C3_TB       0x124u
#define SS_PE_C3_REQ      0x128u
/* QSEED3 (dpu_hw_util.c) */
#define QS_HW_VERSION     0x00u
#define QS_OP_MODE        0x04u
#define QS_PHASE_INIT     0x0cu
#define QS_STEP_Y_H       0x10u
#define QS_STEP_Y_V       0x14u
#define QS_STEP_UV_H      0x18u
#define QS_STEP_UV_V      0x1cu
#define QS_PRELOAD        0x20u
#define QS_SRC_SIZE_Y     0x40u
#define QS_SRC_SIZE_UV    0x44u
#define QS_DST_SIZE       0x48u
#define QS_INIT_Y_H       0x90u
#define QS_INIT_Y_V       0x94u
#define QS_INIT_UV_H      0x98u
#define QS_INIT_UV_V      0x9cu
/* CTL (dpu_hw_ctl.c): flush VIG0 (bit 0) and the CTL itself (bit 17) */
#define CT_FLUSH          0x18u
#define CT_FLUSH_BITS     ((1u << 0) | (1u << 17))
/* INTF tear check, DPU >= 5 (dpu_hw_intf.c) */
#define IF_TEAR_CHECK_EN  0x284u
#define IF_TEAR_LINE      0x2b0u
#define IF_AUTOREFRESH    0x2b4u

/* apps SMMU (DT apps_smmu reg) and the display's stream (DT iommus <&apps_smmu 0x800 0x420>) */
#define SC_SMMU_PA        0x15000000u
#define SC_SMMU_LEN       0x00100000u
#define SC_SID            0x800u
#define SM_IDR0           0x20u
#define SM_IDR1           0x24u
#define SM_IDR2           0x28u
#define SM_SMR(n)         (0x800u + 4u * (n))
#define SM_S2CR(n)        (0xc00u + 4u * (n))
#define SM_CBAR(n)        (4u * (n))             /* in global space 1 */
#define SM_CBA2R(n)       (0x800u + 4u * (n))    /* in global space 1 */
#define CB_SCTLR          0x00u
#define CB_ACTLR          0x04u
#define CB_TCR2           0x10u
#define CB_TTBR0          0x20u
#define CB_TTBR1          0x28u
#define CB_TCR            0x30u
#define CB_MAIR0          0x38u
#define CB_MAIR1          0x3cu
#define CB_FSR            0x58u
#define CB_FAR            0x60u
#define CB_FSYNR0         0x68u
#define CB_TLBIASID       0x610u
#define CB_TLBSYNC        0x7f0u
#define CB_TLBSTATUS      0x7f4u
/* Fault bits of CB_FSR as Linux arm-smmu reads them (TF..UUT, SS, MULTI); bits 10:9 are FORMAT. */
#define CB_FSR_FAULT      (0x1feu | (1u << 30) | (1u << 31))

/* What the bank is set to (see the top of the file). */
#define SC_CBAR           0x0001f300u     /* type 1 (S1, S2 bypass), BPSHCFG non-shareable, MEMATTR WB */
#define SC_CBA2R          0x1u            /* AArch64 descriptors */
#define SC_TCR            0x00802020u     /* T0SZ 32, NC walk, SH0 outer, TG0 4K, EPD1 */
#define SC_TCR2_SEP       (7u << 15)
#define SC_MAIR0          0x0000ff44u     /* attr 0 normal non-cacheable, attr 1 write-back */
#define SC_SCTLR          0x00001027u     /* M, TRE, AFE, CFRE, ASIDPNE; no fault interrupt */
/* Leaf: page, AttrIndx 0, AP read-only, SH outer, AF, nG, PXN, UXN */
#define SC_LEAF_ATTR      ((1ull << 11) | (1ull << 10) | (2ull << 8) | (1ull << 7) | (1ull << 6) | 3ull | (3ull << 53))

/* Tables and IOVA slots */
#define SC_POOL_PAGES     96u             /* 4 KiB tables: 1 + 1 + panel (6) + 6 slots x 6, with room */
#define SC_SLOT_BASE      0x80000000u
#define SC_SLOT_BYTES     (16u << 20)
#define SC_SLOTS          8u

struct sc_tables {
	uint64_t *va;                   /* pool, SC_POOL_PAGES x 4 KiB, contiguous */
	uint64_t pa;
	unsigned used;                  /* pages taken (page 0 is the level-1 table) */
};

/* Physical address of the 4 KiB table holding entry E. */
static inline uint64_t
sc_pt_pa(const struct sc_tables *t, const uint64_t *e)
{
	return t->pa + (uint64_t)((const uint8_t *)e - (const uint8_t *)t->va);
}

/* The level-3 entry for IOVA, its tables made when MAKE; NULL when the pool is full or (no MAKE) not mapped. */
static uint64_t *
sc_pt_leaf(struct sc_tables *t, uint32_t iova, bool make, void (*clean)(uint64_t pa, unsigned len))
{
	uint64_t *table = t->va;
	for (unsigned level = 1; level < 3; level++) {
		const unsigned shift = level == 1 ? 30 : 21;
		uint64_t *e = &table[(iova >> shift) & (level == 1 ? 3u : 511u)];
		if ((*e & 3) != 3) {
			if (!make || t->used >= SC_POOL_PAGES) {
				return NULL;
			}
			*e = (t->pa + 0x1000ull * t->used++) | 3;
			clean(sc_pt_pa(t, e), 8);
		}
		table = (uint64_t *)((uint8_t *)t->va + ((*e & 0xfffffffff000ull) - t->pa));
	}
	return &table[(iova >> 12) & 511];
}

/* Map NPAGES at IOVA: PAGES[i] or (PAGES NULL) PA + i x 4 KiB. False when the pool ran out. */
static bool
sc_pt_map(struct sc_tables *t, uint32_t iova, const uint64_t *pages, uint64_t pa, uint32_t npages,
    void (*clean)(uint64_t pa, unsigned len))
{
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t *leaf = sc_pt_leaf(t, iova + 0x1000u * i, true, clean);
		if (leaf == NULL) {
			return false;
		}
		const uint64_t page = pages ? pages[i] : pa + 0x1000ull * i;
		*leaf = (page & 0xfffffffff000ull) | SC_LEAF_ATTR;
		clean(sc_pt_pa(t, leaf), 8);
	}
	return true;
}

static void
sc_pt_unmap(struct sc_tables *t, uint32_t iova, uint32_t npages, void (*clean)(uint64_t pa, unsigned len))
{
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t *leaf = sc_pt_leaf(t, iova + 0x1000u * i, false, clean);
		if (leaf != NULL) {
			*leaf = 0;
			clean(sc_pt_pa(t, leaf), 8);
		}
	}
}

/* Pipe and scaler values for a frame (pure, host-tested). */
struct sc_regs {
	uint32_t src_size, src_xy, out_size, out_xy, addr, stride, req;
	bool scaled;
	uint32_t step_h, step_v, preload, qsrc, qdst, qop;
};

static bool
sc_frame_regs(const struct andr_scan_frame *f, uint32_t qseed_version, struct sc_regs *r)
{
	if (f->src_w < 2 || f->src_h < 2 || f->dst_w < 2 || f->dst_h < 2 || f->src_w > 4096 || f->src_h > 4096 ||
	    f->dst_w > 4096 || f->dst_h > 4096 || f->stride == 0 || f->stride > 0xffff || (f->iova & 0xf) != 0 ||
	    f->src_x > 0xffff || f->src_y > 0xffff || f->dst_x > 0xffff || f->dst_y > 0xffff ||
	    /* QSEED3 limits as Linux's catalog: up to 4x down, 20x up */
	    f->src_w > 4 * f->dst_w || f->src_h > 4 * f->dst_h || f->dst_w > 20 * f->src_w || f->dst_h > 20 * f->src_h) {
		return false;
	}
	r->src_size = (f->src_h << 16) | f->src_w;
	r->src_xy = (f->src_y << 16) | f->src_x;
	r->out_size = (f->dst_h << 16) | f->dst_w;
	r->out_xy = (f->dst_y << 16) | f->dst_x;
	r->addr = f->iova;
	r->stride = f->stride;
	/* SW pixel extension (SRC_OP_MODE bit 31, set by the ABL): no extra pixels, as dpu_plane with QSEED3 */
	r->req = r->src_size;
	r->scaled = f->src_w != f->dst_w || f->src_h != f->dst_h;
	/* dpu_plane: phase step = (1 << 21) * src / dst; preload 4 / 3; bilinear (3), blend, 8-bit, alpha bilinear */
	r->step_h = (uint32_t)(((uint64_t)f->src_w << 21) / f->dst_w) & 0xffffff;
	r->step_v = (uint32_t)(((uint64_t)f->src_h << 21) / f->dst_h) & 0xffffff;
	r->preload = 4u | (3u << 8) | (4u << 16) | (3u << 24);
	r->qsrc = (f->src_w & 0x1ffff) | ((f->src_h & 0x1ffff) << 16);
	r->qdst = (f->dst_w & 0x1ffff) | ((f->dst_h & 0x1ffff) << 16);
	r->qop = 1u | (3u << 16) | (1u << 31) | (1u << 14) | (1u << 10) |
	    (qseed_version == 0x1002 ? (1u << 30) : (1u << 29));
	return true;
}

#if defined(KERNEL)
#define SC_SAVED_SSPP 16u
static const uint32_t sc_sspp_saved[SC_SAVED_SSPP] = {
	SS_SRC_SIZE, SS_SRC_XY, SS_OUT_SIZE, SS_OUT_XY, SS_SRC0_ADDR, SS_YSTRIDE0, SS_OP_MODE,
	SS_PE_C0_LR, SS_PE_C0_TB, SS_PE_C0_REQ, SS_PE_C12_LR, SS_PE_C12_TB, SS_PE_C12_REQ,
	SS_PE_C3_LR, SS_PE_C3_TB, SS_PE_C3_REQ,
};
/* OP_MODE last: restored after the values it turns on. */
#define SC_SAVED_QSEED 14u
static const uint32_t sc_qseed_saved[SC_SAVED_QSEED] = {
	QS_PHASE_INIT, QS_STEP_Y_H, QS_STEP_Y_V, QS_STEP_UV_H, QS_STEP_UV_V, QS_PRELOAD, QS_SRC_SIZE_Y,
	QS_SRC_SIZE_UV, QS_DST_SIZE, QS_INIT_Y_H, QS_INIT_Y_V, QS_INIT_UV_H, QS_INIT_UV_V, QS_OP_MODE,
};

static struct {
	lck_grp_t *grp;
	lck_mtx_t lock;                 /* everything below and the registers */
	bool tried, ready, on_panel;
	int error;
	vm_offset_t mdp, smmu;
	uint32_t cb, gr1;               /* offsets of the bank and of global space 1 */
	unsigned bank, smr, asid;
	uint32_t qseed_version;
	uint64_t panel_pa;
	uint32_t panel_bytes;
	struct sc_tables t;
	bool slot_used[SC_SLOTS];
	uint32_t sspp[SC_SAVED_SSPP], qseed[SC_SAVED_QSEED];
	uint64_t shows, waits, faults;
} sc;

static unsigned sc_mode, sc_mode_parsed, sc_copy_req;

static inline uint32_t
mdp_rd(uint32_t off)
{
	return *(volatile uint32_t *)(sc.mdp + off);
}

static inline void
mdp_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sc.mdp + off) = v;
}

static inline uint32_t
sm_rd(uint32_t off)
{
	return *(volatile uint32_t *)(sc.smmu + off);
}

static inline void
sm_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sc.smmu + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline void
sm_wr64(uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(sc.smmu + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
sc_clean(uint64_t pa, unsigned len)
{
	flush_dcache64((addr64_t)pa, len, TRUE);
}

/* Sleep about US microseconds. */
static void
sc_nap_us(unsigned us)
{
	static int chan;
	(void)assert_wait_timeout((event_t)&chan, THREAD_UNINT, us, NSEC_PER_USEC);
	(void)thread_block(THREAD_CONTINUE_NULL);
}

static void
sc_mode_args(void)
{
	if (__atomic_exchange_n(&sc_mode_parsed, 1u, __ATOMIC_ACQ_REL) == 0) {
		unsigned v = 0;
		(void)PE_parse_boot_argn("andrscanout", &v, sizeof(v));
		__atomic_store_n(&sc_mode, v > 2 ? 0u : v, __ATOMIC_RELAXED);
		if (v) {
			printf(SC_TAG " andrscanout=%u: the display reads Metal frames itself from the first one\n", v);
		}
	}
}

unsigned
andromeda_scanout_mode(void)
{
	sc_mode_args();
	return __atomic_load_n(&sc_mode, __ATOMIC_RELAXED);
}

/* TLB of the bank's ASID, then a sync (at most 10 ms). */
static bool
sc_tlb_flush(void)
{
	sm_wr(sc.cb + CB_TLBIASID, sc.asid);
	sm_wr(sc.cb + CB_TLBSYNC, 0);
	for (unsigned i = 0; i < 10000; i++) {
		if ((sm_rd(sc.cb + CB_TLBSTATUS) & 1) == 0) {
			return true;
		}
		delay(1);
	}
	printf(SC_TAG " TLB sync of bank %u not done after 10 ms\n", sc.bank);
	return false;
}

/* Until CTL0 has taken the last flush, at most 25 ms (one 60 Hz frame and a half). */
static bool
sc_flush_idle(void)
{
	for (unsigned i = 0; i < 70; i++) {
		if ((mdp_rd(SC_CTL0 + CT_FLUSH) & CT_FLUSH_BITS) == 0) {
			if (i != 0) {
				sc.waits++;
			}
			return true;
		}
		if (i < 20) {
			delay(25);
		} else {
			sc_nap_us(500);
		}
	}
	return false;
}

/* Fault status of the bank: counted, logged (first 8), cleared. True when there was a fault. */
static bool
sc_fault_check(void)
{
	const uint32_t fsr = sm_rd(sc.cb + CB_FSR);
	if ((fsr & CB_FSR_FAULT) == 0) {
		return false;
	}
	if (++sc.faults <= 8) {
		printf(SC_TAG " bank %u fault fsr %08x far %08x%08x fsynr0 %08x\n", sc.bank, fsr,
		    sm_rd(sc.cb + CB_FAR + 4), sm_rd(sc.cb + CB_FAR), sm_rd(sc.cb + CB_FSYNR0));
	}
	sm_wr(sc.cb + CB_FSR, fsr);
	return true;
}

static void
sc_lock_init(void)
{
	static unsigned once;
	if (__atomic_exchange_n(&once, 1u, __ATOMIC_ACQ_REL) == 0) {
		sc.grp = lck_grp_alloc_init("andromeda_scanout", LCK_GRP_ATTR_NULL);
		lck_mtx_init(&sc.lock, sc.grp, LCK_ATTR_NULL);
	}
}

static int
sc_init_locked(uint64_t panel_pa, uint32_t rowbytes, uint32_t w, uint32_t h)
{
	sc.mdp = andromeda_io_map(SC_MDP_PA, SC_MDP_LEN);
	sc.smmu = andromeda_io_map(SC_SMMU_PA, SC_SMMU_LEN);
	if (!sc.mdp || !sc.smmu) {
		printf(SC_TAG " MDP or SMMU not mapped: CPU copy\n");
		return ENOMEM;
	}
	/* The pipe: VIG0 reads the panel memory unscaled; INTF1 refreshes it by itself. */
	const uint32_t addr = mdp_rd(SC_VIG0 + SS_SRC0_ADDR), stride = mdp_rd(SC_VIG0 + SS_YSTRIDE0) & 0xffff;
	const uint32_t size = mdp_rd(SC_VIG0 + SS_SRC_SIZE), out = mdp_rd(SC_VIG0 + SS_OUT_SIZE);
	const uint32_t te = mdp_rd(SC_INTF1 + IF_TEAR_CHECK_EN), ar = mdp_rd(SC_INTF1 + IF_AUTOREFRESH);
	sc.qseed_version = mdp_rd(SC_QSEED + QS_HW_VERSION);
	printf(SC_TAG " vig0 %08x stride %u size %08x out %08x format %08x op %08x; qseed3 version %08x op %08x; "
	    "intf1 tear check %08x autorefresh %08x\n", addr, stride, size, out, mdp_rd(SC_VIG0 + SS_SRC_FORMAT),
	    mdp_rd(SC_VIG0 + SS_OP_MODE), sc.qseed_version, mdp_rd(SC_QSEED + QS_OP_MODE), te, ar);
	if (addr != (uint32_t)panel_pa || panel_pa > 0xffffffffull || stride != rowbytes || size != ((h << 16) | w) ||
	    out != size || !(ar & (1u << 31)) || (te & 1) == 0) {
		printf(SC_TAG " the pipe is not the ABL's panel setup (panel %#llx %ux%u stride %u): CPU copy\n",
		    (unsigned long long)panel_pa, w, h, rowbytes);
		return ENODEV;
	}
	/* The stream: one valid stream match for SID 0x800, translating context, its bank off. */
	const uint32_t idr0 = sm_rd(SM_IDR0), idr1 = sm_rd(SM_IDR1), idr2 = sm_rd(SM_IDR2);
	const unsigned nsmr = idr0 & 0xffu, ncb = idr1 & 0xffu;
	const uint32_t page = (idr1 & (1u << 31)) ? 0x10000u : 0x1000u;
	const uint32_t numpage = 1u << (((idr1 >> 28) & 7u) + 1);
	unsigned hits = 0;
	uint32_t s2cr = 0;
	for (unsigned i = 0; i < nsmr && i < 128; i++) {
		const uint32_t smr = sm_rd(SM_SMR(i)), id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
		if ((smr & (1u << 31)) && ((id ^ SC_SID) & ~mask & 0x7fffu) == 0) {
			sc.smr = i;
			s2cr = sm_rd(SM_S2CR(i));
			hits++;
		}
	}
	sc.bank = s2cr & 0xffu;
	sc.cb = numpage * page + sc.bank * page;
	sc.gr1 = page;
	if (hits != 1 || ((s2cr >> 16) & 3u) != 0 || sc.bank >= ncb || sc.cb + page > SC_SMMU_LEN) {
		printf(SC_TAG " stream %#x: %u matches, s2cr %08x (idr0 %08x idr1 %08x): CPU copy\n", SC_SID, hits, s2cr,
		    idr0, idr1);
		return ENODEV;
	}
	const uint32_t was_sctlr = sm_rd(sc.cb + CB_SCTLR), was_actlr = sm_rd(sc.cb + CB_ACTLR);
	const uint32_t was_cbar = sm_rd(sc.gr1 + SM_CBAR(sc.bank)), was_cba2r = sm_rd(sc.gr1 + SM_CBA2R(sc.bank));
	const uint32_t was_tcr = sm_rd(sc.cb + CB_TCR), was_tcr2 = sm_rd(sc.cb + CB_TCR2);
	const uint32_t was_mair0 = sm_rd(sc.cb + CB_MAIR0), was_mair1 = sm_rd(sc.cb + CB_MAIR1);
	printf(SC_TAG " stream match %u s2cr %08x -> bank %u: sctlr %08x actlr %08x cbar %08x cba2r %08x tcr %08x "
	    "tcr2 %08x mair %08x/%08x; idr2 %08x\n", sc.smr, s2cr, sc.bank, was_sctlr, was_actlr, was_cbar, was_cba2r,
	    was_tcr, was_tcr2, was_mair0, was_mair1, idr2);
	if (was_sctlr & 1) {
		printf(SC_TAG " bank %u already translates: CPU copy\n", sc.bank);
		return EBUSY;
	}
	/* Output size: the SMMU's, at most 40 bits (encodings 0..5 = 32, 36, 40, 42, 44, 48). */
	unsigned oas = (idr2 >> 4) & 15u;
	if (oas > 5) {
		return ENODEV;
	}
	if (oas > 2) {
		oas = 2;
	}

	/* Tables: the panel memory at its own address. */
	vm_offset_t tva = 0;
	if (kmem_alloc_contig(kernel_map, &tva, SC_POOL_PAGES * 0x1000, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	sc.t.va = (uint64_t *)tva;
	sc.t.pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)tva));
	sc.t.used = 1;
	const uint64_t lo = panel_pa & ~0xfffull;
	const uint64_t hi = (panel_pa + (uint64_t)rowbytes * h + 0xfff) & ~0xfffull;
	sc.panel_pa = panel_pa;
	sc.panel_bytes = (uint32_t)(hi - lo);
	if (sc.t.pa == 0 || (sc.t.pa >> (32 + 4 * oas)) != 0 ||
	    (lo < SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES && hi > SC_SLOT_BASE) ||
	    !sc_pt_map(&sc.t, (uint32_t)lo, NULL, lo, (uint32_t)((hi - lo) >> 12), sc_clean)) {
		printf(SC_TAG " tables at %#llx: panel %#llx-%#llx not mapped: CPU copy\n", (unsigned long long)sc.t.pa,
		    (unsigned long long)lo, (unsigned long long)hi);
		kmem_free(kernel_map, tva, SC_POOL_PAGES * 0x1000);
		sc.t.va = NULL;
		return ENOMEM;
	}
	flush_dcache64((addr64_t)sc.t.pa, SC_POOL_PAGES * 0x1000, TRUE);

	/* The bank, translation still off; then TLB, then on. */
	sc.asid = sc.bank;
	const uint64_t ttbr0 = ((uint64_t)sc.asid << 48) | sc.t.pa;
	const uint32_t tcr2 = SC_TCR2_SEP | (oas == 0 ? 0u : oas == 1 ? 1u : 2u);
	const uint32_t f0 = mdp_rd(SC_INTF1 + IF_TEAR_LINE);
	sm_wr(sc.cb + CB_SCTLR, 0);
	sm_wr(sc.gr1 + SM_CBA2R(sc.bank), SC_CBA2R);
	sm_wr(sc.gr1 + SM_CBAR(sc.bank), (was_cbar & 0xff000000u) | SC_CBAR);
	sm_wr(sc.cb + CB_TCR2, tcr2);
	sm_wr(sc.cb + CB_TCR, SC_TCR);
	sm_wr64(sc.cb + CB_TTBR0, ttbr0);
	sm_wr64(sc.cb + CB_TTBR1, (uint64_t)sc.asid << 48);
	sm_wr(sc.cb + CB_MAIR0, SC_MAIR0);
	sm_wr(sc.cb + CB_MAIR1, 0);
	sm_wr(sc.cb + CB_FSR, 0xffffffffu);
	const bool tlb = sc_tlb_flush();
	const uint32_t rb_tcr = sm_rd(sc.cb + CB_TCR), rb_cbar = sm_rd(sc.gr1 + SM_CBAR(sc.bank));
	const uint64_t rb_ttbr0 = *(volatile uint64_t *)(sc.smmu + sc.cb + CB_TTBR0);
	bool ok = tlb && rb_tcr == SC_TCR && (rb_cbar & 0x00ffffffu) == SC_CBAR && rb_ttbr0 == ttbr0;
	if (ok) {
		sm_wr(sc.cb + CB_SCTLR, SC_SCTLR);
		/* A flush of the unchanged pipe must be taken (frames run) and the bank must not fault. */
		mdp_wr(SC_CTL0 + CT_FLUSH, CT_FLUSH_BITS);
		IOSleep(100);
		ok = (mdp_rd(SC_CTL0 + CT_FLUSH) & CT_FLUSH_BITS) == 0 && !sc_fault_check();
	}
	const uint32_t f1 = mdp_rd(SC_INTF1 + IF_TEAR_LINE);
	printf(SC_TAG " bank %u: tables %u pages at %#llx, tlb %s, readback tcr %08x cbar %08x ttbr0 %#llx; "
	    "sctlr %08x fsr %08x flush %08x, tear line %u -> %u: %s\n", sc.bank, sc.t.used,
	    (unsigned long long)sc.t.pa, tlb ? "ok" : "TIMEOUT", rb_tcr, rb_cbar, (unsigned long long)rb_ttbr0,
	    sm_rd(sc.cb + CB_SCTLR), sm_rd(sc.cb + CB_FSR), mdp_rd(SC_CTL0 + CT_FLUSH), f0, f1,
	    ok ? "the display reads through its own tables" : "back as the ABL left it, CPU copy");
	if (!ok) {
		sm_wr(sc.cb + CB_SCTLR, was_sctlr);
		sm_wr(sc.gr1 + SM_CBAR(sc.bank), was_cbar);
		sm_wr(sc.gr1 + SM_CBA2R(sc.bank), was_cba2r);
		sm_wr(sc.cb + CB_TCR, was_tcr);
		sm_wr(sc.cb + CB_TCR2, was_tcr2);
		sm_wr(sc.cb + CB_MAIR0, was_mair0);
		sm_wr(sc.cb + CB_MAIR1, was_mair1);
		sm_wr(sc.cb + CB_FSR, 0xffffffffu);
		/* The tables stay allocated: the SMMU may still hold walks of them. */
		return EIO;
	}
	for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
		sc.sspp[i] = mdp_rd(SC_VIG0 + sc_sspp_saved[i]);
	}
	for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
		sc.qseed[i] = mdp_rd(SC_QSEED + sc_qseed_saved[i]);
	}
	sc.on_panel = true;
	return 0;
}

int
andromeda_scanout_init(uint64_t panel_pa, uint32_t rowbytes, uint32_t w, uint32_t h)
{
	sc_lock_init();
	lck_mtx_lock(&sc.lock);
	if (!sc.tried) {
		sc.tried = true;
		sc.error = sc_init_locked(panel_pa, rowbytes, w, h);
		sc.ready = sc.error == 0;
	}
	const int error = sc.error;
	lck_mtx_unlock(&sc.lock);
	return error;
}

bool
andromeda_scanout_ready(void)
{
	return __atomic_load_n(&sc.ready, __ATOMIC_ACQUIRE);
}

uint32_t
andromeda_scanout_map(const uint64_t *pages, uint32_t npages)
{
	if (!andromeda_scanout_ready() || pages == NULL || npages == 0 || npages > SC_SLOT_BYTES / 0x1000) {
		return 0;
	}
	for (uint32_t i = 0; i < npages; i++) {
		if (pages[i] == 0 || (pages[i] & 0xfff) != 0 || (pages[i] >> 40) != 0) {
			return 0;
		}
	}
	uint32_t iova = 0;
	lck_mtx_lock(&sc.lock);
	for (unsigned i = 0; i < SC_SLOTS; i++) {
		if (!sc.slot_used[i]) {
			const uint32_t at = SC_SLOT_BASE + i * SC_SLOT_BYTES;
			if (sc_pt_map(&sc.t, at, pages, 0, npages, sc_clean)) {
				sc.slot_used[i] = true;
				iova = at;
			} else {
				sc_pt_unmap(&sc.t, at, npages, sc_clean);
				printf(SC_TAG " table pool full (%u pages)\n", sc.t.used);
			}
			(void)sc_tlb_flush();
			break;
		}
	}
	lck_mtx_unlock(&sc.lock);
	return iova;
}

void
andromeda_scanout_unmap(uint32_t iova, uint32_t npages)
{
	if (!andromeda_scanout_ready() || iova < SC_SLOT_BASE || iova >= SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES ||
	    (iova - SC_SLOT_BASE) % SC_SLOT_BYTES != 0 || npages > SC_SLOT_BYTES / 0x1000) {
		return;
	}
	lck_mtx_lock(&sc.lock);
	sc_pt_unmap(&sc.t, iova, npages, sc_clean);
	(void)sc_tlb_flush();
	sc.slot_used[(iova - SC_SLOT_BASE) / SC_SLOT_BYTES] = false;
	lck_mtx_unlock(&sc.lock);
}

int
andromeda_scanout_show(const struct andr_scan_frame *f)
{
	struct sc_regs r;
	if (!andromeda_scanout_ready()) {
		return ENODEV;
	}
	if (f == NULL || !sc_frame_regs(f, sc.qseed_version, &r)) {
		return EINVAL;
	}
	lck_mtx_lock(&sc.lock);
	if (!sc_flush_idle()) {
		lck_mtx_unlock(&sc.lock);
		return EBUSY;
	}
	const uint32_t p = SC_VIG0, q = SC_QSEED;
	mdp_wr(p + SS_SRC0_ADDR, r.addr);
	mdp_wr(p + SS_YSTRIDE0, r.stride);
	mdp_wr(p + SS_SRC_SIZE, r.src_size);
	mdp_wr(p + SS_SRC_XY, r.src_xy);
	mdp_wr(p + SS_OUT_SIZE, r.out_size);
	mdp_wr(p + SS_OUT_XY, r.out_xy);
	mdp_wr(p + SS_PE_C0_LR, 0);
	mdp_wr(p + SS_PE_C0_TB, 0);
	mdp_wr(p + SS_PE_C0_REQ, r.req);
	mdp_wr(p + SS_PE_C12_LR, 0);
	mdp_wr(p + SS_PE_C12_TB, 0);
	mdp_wr(p + SS_PE_C12_REQ, r.req);
	mdp_wr(p + SS_PE_C3_LR, 0);
	mdp_wr(p + SS_PE_C3_TB, 0);
	mdp_wr(p + SS_PE_C3_REQ, r.req);
	if (r.scaled) {
		if (sc.qseed_version == 0x1002) {
			mdp_wr(q + QS_PHASE_INIT, 0);
		} else {
			mdp_wr(q + QS_INIT_Y_H, 0);
			mdp_wr(q + QS_INIT_Y_V, 0);
			mdp_wr(q + QS_INIT_UV_H, 0);
			mdp_wr(q + QS_INIT_UV_V, 0);
		}
		mdp_wr(q + QS_STEP_Y_H, r.step_h);
		mdp_wr(q + QS_STEP_Y_V, r.step_v);
		mdp_wr(q + QS_STEP_UV_H, r.step_h);
		mdp_wr(q + QS_STEP_UV_V, r.step_v);
		mdp_wr(q + QS_PRELOAD, r.preload);
		mdp_wr(q + QS_SRC_SIZE_Y, r.qsrc);
		mdp_wr(q + QS_SRC_SIZE_UV, r.qsrc);
		mdp_wr(q + QS_DST_SIZE, r.qdst);
		mdp_wr(q + QS_OP_MODE, r.qop);
	} else {
		mdp_wr(q + QS_OP_MODE, sc.qseed[SC_SAVED_QSEED - 1]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	mdp_wr(SC_CTL0 + CT_FLUSH, CT_FLUSH_BITS);
	sc.on_panel = false;
	sc.shows++;
	lck_mtx_unlock(&sc.lock);
	return 0;
}

int
andromeda_scanout_panel(void)
{
	if (!andromeda_scanout_ready()) {
		return 0;
	}
	lck_mtx_lock(&sc.lock);
	if (sc.on_panel) {
		lck_mtx_unlock(&sc.lock);
		return 0;
	}
	if (!sc_flush_idle()) {
		lck_mtx_unlock(&sc.lock);
		return EBUSY;
	}
	for (unsigned i = 0; i < SC_SAVED_SSPP; i++) {
		mdp_wr(SC_VIG0 + sc_sspp_saved[i], sc.sspp[i]);
	}
	for (unsigned i = 0; i < SC_SAVED_QSEED; i++) {
		mdp_wr(SC_QSEED + sc_qseed_saved[i], sc.qseed[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	mdp_wr(SC_CTL0 + CT_FLUSH, CT_FLUSH_BITS);
	sc.on_panel = true;
	lck_mtx_unlock(&sc.lock);
	return 0;
}

bool
andromeda_scanout_latched(void)
{
	if (!andromeda_scanout_ready()) {
		return true;
	}
	if (sc_fault_check() && sc.faults >= 3) {
		/* Something the tables do not map: back to the CPU copy (the framebuffer takes the panel back). */
		if (__atomic_exchange_n(&sc_mode, 0u, __ATOMIC_RELAXED) != 0) {
			printf(SC_TAG " %llu SMMU faults: scanout off (andrscanout=0), CPU copy\n", (unsigned long long)sc.faults);
		}
	}
	return (mdp_rd(SC_CTL0 + CT_FLUSH) & CT_FLUSH_BITS) == 0;
}

void
andromeda_scanout_counts(uint64_t *shows, uint64_t *waits, uint64_t *faults)
{
	*shows = sc.shows;
	*waits = sc.waits;
	*faults = sc.faults;
}

int
andromeda_scanout_ctl(unsigned mode, char *why, size_t len)
{
	sc_mode_args();
	if (mode > 2) {
		snprintf(why, len, "scanout: mode 0, 1 or 2");
		return EINVAL;
	}
	__atomic_store_n(&sc_mode, mode, __ATOMIC_RELAXED);
	snprintf(why, len, "scanout %u (%s; hardware %s, %llu pipe updates, %llu waited for a frame, %llu SMMU faults)",
	    mode, mode == 0 ? "CPU copy" : mode == 1 ? "the display reads Metal frames" : "scanout with cache clean",
	    sc.ready ? "set up" : sc.tried ? "refused, CPU copy" : "not set up yet", (unsigned long long)sc.shows,
	    (unsigned long long)sc.waits, (unsigned long long)sc.faults);
	printf(SC_TAG " %s\n", why);
	return 0;
}

void
andromeda_scanout_copy_frames(unsigned n)
{
	__atomic_store_n(&sc_copy_req, n, __ATOMIC_RELEASE);
}

bool
andromeda_scanout_copy_pending(void)
{
	return __atomic_load_n(&sc_copy_req, __ATOMIC_ACQUIRE) != 0;
}

bool
andromeda_scanout_take_copy(void)
{
	unsigned n = __atomic_load_n(&sc_copy_req, __ATOMIC_ACQUIRE);
	while (n != 0) {
		if (__atomic_compare_exchange_n(&sc_copy_req, &n, n - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
	return false;
}
#endif /* KERNEL */
#endif /* ARM64_BOARD_CONFIG_SWEET || ANDR_SCANOUT_HOST */
