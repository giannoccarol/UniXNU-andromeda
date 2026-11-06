/*
 * Phase F4 on andromeda (Xiaomi Mi Mix 3 5G, SM8150, Adreno 640): the GPU
 * copies each presented frame onto the panel. Off unless andrgpupresent=1,
 * and only after andrgpu=4 has passed its CP_MEM_WRITE test in this boot
 * (see andromeda_gpu_present.h for the boot-args).
 *
 * What the frame copy is. Patch 0094 (sweetfb_fit=1) shows iOS's 1125x2436
 * display on the 1080x2340 panel scaled by 1080/1125 with two nearest-
 * neighbour tables: panel column x shows logical column fit_col[x], fitted
 * row y shows logical row fit_row[y]. 1125/1080 = 25/24, so of every 25
 * logical columns (rows) 24 consecutive ones are kept. The GPU reproduces
 * those tables exactly with unscaled 2D-engine copies (CP_BLIT, nearest, no
 * filtering), so its frame is meant to be bit-identical to the CPU's:
 *   pass 1  one copy per run of consecutive columns (45 for 1125 -> 1080):
 *           surface -> a stage image in the GPU pool (all logical rows,
 *           fitted columns, 64-byte aligned rows);
 *   pass 2  one copy per row class (24 for 25 -> 24): fitted rows y with the
 *           same y mod 24 are 24 panel rows apart and their logical rows 25
 *           apart, so each class is one 2D copy whose destination pitch is
 *           24 panel rows and whose source pitch is 25 stage rows.
 * The 2D engine wants 64-byte aligned destination bases and pitches; the
 * panel's rows are 4320 bytes (ABL's stride, not a multiple of 64). Instead
 * of reprogramming the display pipe's stride as ios-flame does (MDP VIG0/VIG1
 * YSTRIDE0 = 4352), the panel keeps its native stride: a class whose first
 * row starts 32 bytes into a 64-byte line is written from the aligned
 * address below it, 8 pixels to the right - the trick Mesa's turnip uses for
 * copies to unaligned buffers (tu_clear_blit.cc, copy_image_to_buffer and
 * copy_buffer: "dst_va & ~63", x = (dst_va & 63) / cpp). Nothing in the MDP
 * is touched.
 *
 * Memory rules (the driver's, andromeda_gpu.c):
 *   - the GPU can write only the pool (RAM XNU does not manage, checked
 *     there) and the panel memory, which is checked here page by page with
 *     pmap_valid_address() before it is mapped (refused if XNU manages any
 *     of it);
 *   - surface pages are XNU's: they are mapped READ-ONLY (AP = 11) in the
 *     GPU page tables, must be pmap_valid_address() RAM inside DRAM, are
 *     prepare()d and retained by the framebuffer while mapped, and are
 *     unmapped (PTEs cleared, TLB invalidated and synced) before they are
 *     released; if the sync cannot be confirmed the framebuffer keeps them;
 *   - the CPU renders those surfaces through the cache: each frame's bytes
 *     are cleaned to the point of coherency before the GPU reads them (the
 *     GPU's mappings are normal non-cacheable, MAIR attr 0x44);
 *   - every wait is bounded; on a fence timeout, a check mismatch, a
 *     thermal stop or any setup error the GPU path turns itself off for the
 *     rest of the boot and the CPU copy carries on.
 * One thread uses all of this (the framebuffer's work loop); the bring-up
 * thread hands the GPU over once (andromeda_gpu_present_attach) and stops.
 * Submissions are synchronous: the frame's fence lands before the function
 * returns, so the CPU fallback, the display-off clear and surface unmaps
 * never race with GPU writes.
 *
 * Sources. The present design (per-surface GPU mappings in slots at
 * 4 GB + n * 16 MB from prepare()d pages, cache clean, blit IB + WAIT_FOR_IDLE
 * + CP_MEM_WRITE fence, timeout -> back to the CPU copy, fallback reasons
 * logged once) and the 2D blit register stream (A640 init registers +
 * RB/GRAS/TPL1 A2D registers + CP_BLIT, rebuilt by ios-flame from a turnip
 * capture), the 4K page-table walk and the SMMU TLB flush are ios-flame's
 * (Nick Wylynko / NWylynko, https://github.com/NWylynko/ios-flame, patches
 * 0095-flame-gpu-present: sweet_a6xx.c a6_blit_ib/map_4k/a6_tlb_flush and
 * sweet_gpu.c sweet_gpu_present; 0111-flame-gpu-display-mask: a stage image
 * in the pool and several 2D passes in one IB; 0119-flame-gpu-async-present;
 * MIT, the patched XNU files APSL 2.0). Register names and fields: Mesa's
 * a6xx.xml (MIT); the unaligned-destination trick: Mesa turnip (MIT). The
 * IOSurface and IOMobileFramebuffer it feeds are ios-sweet's (dipa-studio).
 * New here: the exact two-pass decomposition of the 0094 fit tables, the
 * native-stride panel writes, the read-only surface mappings with the
 * pmap checks, and the CRC check against the CPU result. See CREDITS.md.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#if defined(KERNEL)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#elif !defined(ANDR_PRESENT_HOST_TEST)	/* host syntax check: stubinc */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
typedef unsigned int boolean_t;
#endif	/* (the host test, test_andromeda_gpu_present.c, declares the environment itself) */
#include "andromeda_gpu.h"
#include "andromeda_gpu_present.h"

/* Same declarations as andromeda_gpu.c (pmap) and osfmk/arm/caches_internal.h. */
extern boolean_t pmap_valid_address(uint64_t pa);
extern void CleanPoC_DcacheRegion(vm_offset_t va, size_t length);

/* ---- layout ---- */

/* GPU virtual addresses (39-bit, 4K pages): the pool stays at 0x10000000 in
 * 2 MB blocks (andromeda_gpu_core.c); the panel at 0x20000000; surface slot
 * n at 4 GB + n * 16 MB (flame's SLOT_IOVA). */
#define AP_GVA_PANEL		0x20000000ull
#define AP_GVA_SLOTS		0x100000000ull
#define AP_GVA_SLOT(n)		(AP_GVA_SLOTS + ANDR_PRESENT_SLOT_BYTES * (n))
#define AP_L3_PER_SLOT		((uint32_t)(ANDR_PRESENT_SLOT_BYTES >> 21))	/* 2 MB per L3 table */
#define AP_PANEL_L3_MAX		8u						/* panel up to 16 MB */

/* Pool offsets used here (the rest of the layout is andromeda_gpu.h's). */
#define AP_OFF_FENCE		(ANDR_OFF_SCRATCH + 0x200ull)	/* present fence dword */
#define AP_OFF_TS		(ANDR_OFF_SCRATCH + 0x300ull)	/* CP_EVENT_WRITE timestamps */
#define AP_OFF_BORDER		(ANDR_OFF_SCRATCH + 0x800ull)	/* border colours (zero, unused) */
#define AP_OFF_IB		0x220000ull			/* present IB, 64 KB */
#define AP_IB_DW		0x4000u
#define AP_OFF_STAGE		0x400000ull			/* stage image, up to the pool's end */

/* Stage-1 descriptors: AF, inner shareable, AttrIndx 0 (normal NC), page. */
#define AP_PTE_RW		((1ull << 10) | (3ull << 8) | (1ull << 6) | 3ull)	/* AP=01 */
#define AP_PTE_RO		((1ull << 10) | (3ull << 8) | (3ull << 6) | 3ull)	/* AP=11 */
#define AP_DESC_TABLE		3ull
#define AP_DESC_ADDR(e)		((e) & 0x000000fffffff000ull)

/* SMMU context bank and GPU registers (same as andromeda_gpu_core.c). */
#define AP_CB_FSR		0x58u
#define AP_CB_FAR		0x60u
#define AP_CB_FSYNR0		0x68u
#define AP_CB_TLBIALL		0x618u
#define AP_CB_TLBSYNC		0x7f0u
#define AP_CB_TLBSTATUS		0x7f4u
#define AP_RBBM_STATUS		0x210u
#define AP_RBBM_INT_STATUS	0x201u
#define AP_CP_RB_RPTR		0x806u
#define AP_CP_RB_WPTR		0x807u
#define AP_CP_HW_FAULT		0x821u
#define AP_CP_STATUS_1		0x825u

/* PM4 opcodes and CP events (a6xx.xml / adreno_pm4.xml). */
#define AP_CP_BLIT		0x2cu
#define AP_CP_INDIRECT_BUFFER	0x3fu
#define AP_CP_EVENT_WRITE	0x46u
#define AP_BLIT_OP_SCALE	3u
#define AP_EV_CACHE_FLUSH_TS	0x04u
#define AP_EV_CCU_INVALIDATE_COLOR 0x19u
#define AP_EV_CCU_FLUSH_DEPTH_TS 0x1cu
#define AP_EV_CCU_FLUSH_COLOR_TS 0x1du
#define AP_EV_CACHE_INVALIDATE	0x31u

/*
 * DRAM on the SM8150: 0x80000000..0x200000000 for 6 GiB. The boot
 * framebuffer (cont_splash_region 0x9c000000, docs/research/gpu.md) lies in
 * the first bank, below the 0xc0000000.. range andromeda_gpu.c accepts for
 * the pool; both checks only keep MMIO out, the ownership checks are
 * pmap_valid_address().
 */
#define AP_DRAM_BASE		0x80000000ull
#define AP_DRAM_END		0x200000000ull

/* Limits of the fit decomposition (0094's tables are 1536 and 3072 long). */
#define AP_MAX_W		1536u
#define AP_MAX_H		3072u
#define AP_MAX_RUNS		128u
#define AP_MAX_CLASSES		64u
#define AP_MAX_PERIOD		64u
#define AP_MAX_COORD		16383u		/* GRAS_A2D_DEST_TL/BR fields */
#define AP_MAX_TEX		32767u		/* TPL1_A2D_SRC_TEXTURE_SIZE fields */
#define AP_MAX_SRC_PITCH	(32767u * 64u)	/* TPL1_A2D_SRC_TEXTURE_PITCH: 15 bits of 64 B */
#define AP_MAX_DST_PITCH	(65535u * 64u)	/* RB_A2D_DEST_BUFFER_PITCH: 16 bits of 64 B */

/* Waits. */
#define AP_FENCE_TIMEOUT_US	100000u		/* one frame's two passes take a few ms */
#define AP_TLB_POLLS		1000u		/* x 10 us */
#define AP_STATS_FRAMES		300u		/* periodic timing line */
#define AP_THERMAL_SECS		10u

/* ---- state ---- */

enum { AP_UNPARSED = 0, AP_OFF, AP_WAITING, AP_READY, AP_FAILED };

static int ap_state;
static uint32_t ap_arg_checks, ap_arg_every, ap_arg_hot;
static bool ap_waiting_logged;
static struct andr_gpu *ap_g;
static uint64_t ap_pool_gpu;		/* bytes of the pool the GPU sees */
static uint32_t ap_fence;

struct ap_run {
	uint16_t x0, len, c0;		/* panel columns x0.. <- logical columns c0.. */
};
struct ap_class {
	uint32_t y0, rows, l0;		/* fitted rows y0, y0 + q*m, ... <- logical rows l0, l0 + p*m, ... */
};

static struct {
	bool ok;
	uint64_t panel_pa, panel_gva;
	uintptr_t panel_kva;
	uint32_t pw, ph, rb, lw, lh, fw, fh, fy;
	uint32_t q, p, m;		/* row period: q fitted rows <- p logical rows; m: classes per residue */
	uint32_t stage_pitch, dst_pitch, src_pitch;
	uint32_t nruns, nclasses;
	uint32_t panel_pages;		/* 4K pages mapped for the panel */
	struct ap_run run[AP_MAX_RUNS];
	struct ap_class cls[AP_MAX_CLASSES];
	uint16_t col[AP_MAX_W], row[AP_MAX_H];
} ap_plan;

static uint64_t ap_l2_slots;			/* L2 table for 4 GB..5 GB */
static uint64_t ap_slot_l3[ANDR_PRESENT_SLOTS][AP_L3_PER_SLOT];
static uint64_t ap_slot_bytes[ANDR_PRESENT_SLOTS];	/* 0: slot empty */
static uint64_t ap_pte[512];
static const uint64_t ap_zero_table[512];

static struct {
	uint64_t frames, total;			/* GPU frames: in this period, ever */
	uint64_t clean_abs, gpu_abs, all_abs, gpu_max_abs;
	uint32_t checks_done, checks_same, checks_unsure;
	uint64_t refused;			/* frames the GPU side declined */
	uint32_t refused_why;			/* reasons already logged */
	uint64_t thermal_at;
} ap_st;

static uint32_t ap_crc_tab[256];

/* ---- helpers ---- */

static void
ap_args(void)
{
	if (__atomic_load_n(&ap_state, __ATOMIC_ACQUIRE) != AP_UNPARSED) {
		return;
	}
	uint32_t on = 0, checks = 8, every = 1000, hot = 80;
	(void)PE_parse_boot_argn("andrgpupresent", &on, sizeof(on));
	(void)PE_parse_boot_argn("andrgpupresentcheck", &checks, sizeof(checks));
	(void)PE_parse_boot_argn("andrgpupresentevery", &every, sizeof(every));
	(void)PE_parse_boot_argn("andrgpuhot", &hot, sizeof(hot));
	if (checks > 64) {
		checks = 64;
	}
	if (hot == 0 || hot > 95) {
		hot = 80;
	}
	/* Both threads may get here; they store the same values. */
	__atomic_store_n(&ap_arg_checks, checks, __ATOMIC_RELAXED);
	__atomic_store_n(&ap_arg_every, every, __ATOMIC_RELAXED);
	__atomic_store_n(&ap_arg_hot, hot, __ATOMIC_RELAXED);
	int expected = AP_UNPARSED;
	(void)__atomic_compare_exchange_n(&ap_state, &expected, on ? AP_WAITING : AP_OFF, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static bool
ap_move(int from, int to)
{
	return __atomic_compare_exchange_n(&ap_state, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Off for the rest of the boot; logged once. */
static void
ap_fail(const char *why)
{
	if (ap_move(AP_READY, AP_FAILED)) {
		andr_log("present: DISABLED for this boot (%s); the CPU copies the frames from now on\n", why);
	}
}

static uint64_t
ap_us(uint64_t abs)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

static uint32_t
ap_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
	    (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t
ap_pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (ap_parity(cnt) << 15) | ((op & 0x7fu) << 16) | (ap_parity(op) << 23);
}

static uint32_t
ap_pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (ap_parity(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (ap_parity(reg) << 27);
}

static uint64_t
ap_rd64(uint64_t pa)
{
	uint64_t v = 0;
	(void)andr_read(pa, &v, 8);
	return v;
}

static void
ap_wr64(uint64_t pa, uint64_t v)
{
	(void)andr_write(pa, &v, 8);
}

/* A zeroed 4K table from the pool's page-table arena (flame pt_alloc). */
static uint64_t
ap_pt_alloc(void)
{
	uint64_t off = ANDR_OFF_PT_ARENA + 0x1000ull * ap_g->pt_next;
	if (off + 0x1000 > ANDR_OFF_SQE) {
		return 0;
	}
	ap_g->pt_next++;
	uint64_t pa = andr_pool_pa(ap_g, off);
	(void)andr_write(pa, ap_zero_table, sizeof(ap_zero_table));
	return pa;
}

/* The next-level table under table[idx], made if the entry is empty; 0 if it is a block. */
static uint64_t
ap_table(uint64_t table, unsigned idx)
{
	uint64_t e = ap_rd64(table + 8ull * idx);
	if (e & 1) {
		return (e & 3) == 3 ? AP_DESC_ADDR(e) : 0;
	}
	uint64_t t = ap_pt_alloc();
	if (t) {
		ap_wr64(table + 8ull * idx, t | AP_DESC_TABLE);
	}
	return t;
}

static int
ap_tlb_sync(void)
{
	uint64_t c = ANDR_SMMU_CB(ap_g->gpu_cb);
	andr_wr(c + AP_CB_TLBIALL, 0);
	andr_wr(c + AP_CB_TLBSYNC, 0);
	for (uint32_t i = 0; i < AP_TLB_POLLS; i++) {
		if (!(andr_rd(c + AP_CB_TLBSTATUS) & 1u)) {	/* 0xffffffff (unmapped) counts as busy */
			return 0;
		}
		delay(10);
	}
	return -1;
}

static void
ap_diag(const char *what)
{
	uint64_t c = ANDR_SMMU_CB(ap_g->gpu_cb);
	uint32_t fsr = andr_rd(c + AP_CB_FSR);
	uint64_t far = andr_rd(c + AP_CB_FAR) | ((uint64_t)andr_rd(c + AP_CB_FAR + 4) << 32);
	andr_log("present: %s: fence %u memory %u, rptr %x wptr %x, rbbm %x int %x hwfault %x cp_status1 %x, "
	    "smmu cb%d fsr %x far %llx fsynr0 %x\n", what, ap_fence,
	    andr_rd(andr_pool_pa(ap_g, AP_OFF_FENCE)), andr_rd(ANDR_GREG(AP_CP_RB_RPTR)), ap_g->wptr,
	    andr_rd(ANDR_GREG(AP_RBBM_STATUS)), andr_rd(ANDR_GREG(AP_RBBM_INT_STATUS)),
	    andr_rd(ANDR_GREG(AP_CP_HW_FAULT)), andr_rd(ANDR_GREG(AP_CP_STATUS_1)), ap_g->gpu_cb, fsr,
	    (unsigned long long)far, andr_rd(c + AP_CB_FSYNR0));
	if (fsr & ANDR_FSR_FAULT) {
		andr_wr(c + AP_CB_FSR, fsr);	/* a fault blocks later writes until cleared (flame) */
	}
}

/* ---- CRC-32 (IEEE) of the fitted image, for the check ---- */

static void
ap_crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (unsigned k = 0; k < 8; k++) {
			c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		}
		ap_crc_tab[i] = c;
	}
}

static inline uint32_t
ap_crc_px(uint32_t crc, uint32_t px)
{
	for (unsigned k = 0; k < 4; k++) {
		crc = ap_crc_tab[(crc ^ px) & 0xffu] ^ (crc >> 8);
		px >>= 8;
	}
	return crc;
}

struct ap_sums {
	uint32_t cpu, gpu;		/* CRC-32 of the expected (CPU) and panel (GPU) fitted image */
	uint64_t diff;			/* pixels that differ */
	uint32_t dx, dy;		/* first difference, panel coordinates */
	uint32_t want, got;
};

/* Expected pixel (fit tables applied to the source) vs the panel; panel = false: expected only. */
static void
ap_sums(const struct andr_present_src *s, bool panel, struct ap_sums *r)
{
	uint32_t cpu = 0xffffffffu, gpu = 0xffffffffu;
	memset(r, 0, sizeof(*r));
	for (uint32_t y = 0; y < ap_plan.fh; y++) {
		const uint32_t *srow = (const uint32_t *)(s->kva + s->offset + (uint64_t)ap_plan.row[y] * s->bytes_per_row);
		const volatile uint32_t *prow = (const volatile uint32_t *)(ap_plan.panel_kva +
		    (uint64_t)(y + ap_plan.fy) * ap_plan.rb);
		for (uint32_t x = 0; x < ap_plan.fw; x++) {
			uint32_t want = srow[ap_plan.col[x]];
			cpu = ap_crc_px(cpu, want);
			if (!panel) {
				continue;
			}
			uint32_t got = prow[x];
			gpu = ap_crc_px(gpu, got);
			if (got != want && r->diff++ == 0) {
				r->dx = x;
				r->dy = y + ap_plan.fy;
				r->want = want;
				r->got = got;
			}
		}
	}
	r->cpu = ~cpu;
	r->gpu = ~gpu;
}

/* ---- attach / ready ---- */

void
andromeda_gpu_present_attach(struct andr_gpu *g, uint64_t pool_size)
{
	ap_args();
	int st = __atomic_load_n(&ap_state, __ATOMIC_ACQUIRE);
	if (st == AP_OFF) {
		andr_log("present: off (andrgpupresent=1 lets the GPU copy the frames)\n");
		return;
	}
	if (st != AP_WAITING) {
		return;
	}
	if (g == NULL || g->gpu_cb < 0 || !g->zap_authenticated || pool_size < ANDR_POOL_SIZE || !andr_gx_up()) {
		andr_log("present: GPU not usable (cb %d, zap %d, pool 0x%llx, gx %d)\n", g ? g->gpu_cb : -1,
		    g ? g->zap_authenticated : 0, (unsigned long long)pool_size, andr_gx_up());
		if (ap_move(AP_WAITING, AP_FAILED)) {
			andr_log("present: DISABLED for this boot (GPU not usable); CPU copy\n");
		}
		return;
	}
	ap_g = g;
	ap_pool_gpu = ANDR_POOL_SIZE;	/* andromeda_gpu_core.c maps 16 MiB of the pool to the GPU */
	ap_crc_init();
	if (ap_move(AP_WAITING, AP_READY)) {
		andr_log("present: GPU attached after step 4 (checks: first %u frames, then 1 in %u); "
		    "the framebuffer hands it the next frame\n", ap_arg_checks, ap_arg_every);
	}
}

int
andromeda_gpu_present_ready(void)
{
	ap_args();
	int st = __atomic_load_n(&ap_state, __ATOMIC_ACQUIRE);
	if (st == AP_WAITING && !ap_waiting_logged) {
		ap_waiting_logged = true;
		andr_log("present: requested (andrgpupresent=1); CPU copy until andrgpu step 4 passes\n");
	}
	return st == AP_READY;
}

/* ---- setup: the fit as GPU copies, the panel mapping ---- */

static int
ap_plan_make(const struct andr_present_geom *gm)
{
	if (gm->fit_col == NULL || gm->fit_row == NULL || gm->panel_kva == 0 || gm->panel_w == 0 ||
	    gm->panel_h == 0 || gm->fit_w == 0 || gm->fit_h == 0 || gm->fit_w > AP_MAX_W ||
	    gm->fit_h > AP_MAX_H || gm->fit_w > gm->panel_w || gm->fit_y + gm->fit_h > gm->panel_h ||
	    gm->panel_rowbytes < gm->panel_w * 4 || (gm->panel_rowbytes & 3) || (gm->panel_pa & 3) ||
	    gm->logical_w == 0 || gm->logical_h == 0 || gm->logical_w > AP_MAX_TEX ||
	    gm->logical_h > AP_MAX_COORD + 1) {
		andr_log("present: geometry refused (panel %ux%u stride %u, logical %ux%u, fit %ux%u at y %u)\n",
		    gm->panel_w, gm->panel_h, gm->panel_rowbytes, gm->logical_w, gm->logical_h, gm->fit_w,
		    gm->fit_h, gm->fit_y);
		return -1;
	}
	ap_plan.panel_pa = gm->panel_pa;
	ap_plan.panel_kva = gm->panel_kva;
	ap_plan.pw = gm->panel_w;
	ap_plan.ph = gm->panel_h;
	ap_plan.rb = gm->panel_rowbytes;
	ap_plan.lw = gm->logical_w;
	ap_plan.lh = gm->logical_h;
	ap_plan.fw = gm->fit_w;
	ap_plan.fh = gm->fit_h;
	ap_plan.fy = gm->fit_y;
	/* Our own copy of the tables, checked: strictly increasing, inside the display. */
	for (uint32_t x = 0; x < ap_plan.fw; x++) {
		ap_plan.col[x] = gm->fit_col[x];
		if (ap_plan.col[x] >= ap_plan.lw || (x && ap_plan.col[x] <= ap_plan.col[x - 1])) {
			andr_log("present: column table not increasing at %u\n", x);
			return -1;
		}
	}
	for (uint32_t y = 0; y < ap_plan.fh; y++) {
		ap_plan.row[y] = gm->fit_row[y];
		if (ap_plan.row[y] >= ap_plan.lh || (y && ap_plan.row[y] <= ap_plan.row[y - 1])) {
			andr_log("present: row table not increasing at %u\n", y);
			return -1;
		}
	}

	/* Pass 1: runs of consecutive logical columns. */
	ap_plan.nruns = 0;
	for (uint32_t x = 0; x < ap_plan.fw;) {
		uint32_t len = 1;
		while (x + len < ap_plan.fw && ap_plan.col[x + len] == ap_plan.col[x] + len) {
			len++;
		}
		if (ap_plan.nruns >= AP_MAX_RUNS) {
			andr_log("present: more than %u column runs\n", AP_MAX_RUNS);
			return -1;
		}
		ap_plan.run[ap_plan.nruns++] = (struct ap_run){ (uint16_t)x, (uint16_t)len, ap_plan.col[x] };
		x += len;
	}

	/*
	 * Pass 2: the row table's period q (row[y + q] = row[y] + p for every y;
	 * 24 and 25 for 0094's 1080/1125). The fitted rows with the same y mod q
	 * are one copy: q panel rows apart on the panel, p rows apart in the stage.
	 */
	ap_plan.q = 0;
	for (uint32_t q = 1; q <= AP_MAX_PERIOD && q < ap_plan.fh && !ap_plan.q; q++) {
		uint32_t p = (uint32_t)ap_plan.row[q] - ap_plan.row[0];
		uint32_t y = 0;
		while (y + q < ap_plan.fh && ap_plan.row[y + q] == ap_plan.row[y] + p) {
			y++;
		}
		if (y + q >= ap_plan.fh) {
			ap_plan.q = q;
			ap_plan.p = p;
		}
	}
	if (!ap_plan.q) {
		andr_log("present: row table has no period up to %u rows\n", AP_MAX_PERIOD);
		return -1;
	}
	/* Classes are q*m panel rows apart: m (a power of two) makes that a multiple of 64 bytes. */
	ap_plan.stage_pitch = (ap_plan.fw * 4 + 63) & ~63u;
	ap_plan.m = 1;
	while (((uint64_t)ap_plan.m * ap_plan.q * ap_plan.rb) & 63) {
		ap_plan.m *= 2;			/* rb is a multiple of 4: m <= 16 */
	}
	uint64_t dst_pitch = (uint64_t)ap_plan.m * ap_plan.q * ap_plan.rb;
	uint64_t src_pitch = (uint64_t)ap_plan.m * ap_plan.p * ap_plan.stage_pitch;
	uint64_t step = (uint64_t)ap_plan.m * ap_plan.q;
	if (dst_pitch > AP_MAX_DST_PITCH || src_pitch > AP_MAX_SRC_PITCH ||
	    dst_pitch < (uint64_t)(ap_plan.fw + 15) * 4) {
		andr_log("present: row copies need strides %llu/%llu (q %u p %u m %u)\n",
		    (unsigned long long)dst_pitch, (unsigned long long)src_pitch, ap_plan.q, ap_plan.p, ap_plan.m);
		return -1;
	}
	ap_plan.dst_pitch = (uint32_t)dst_pitch;
	ap_plan.src_pitch = (uint32_t)src_pitch;
	ap_plan.nclasses = 0;
	for (uint32_t r = 0; r < ap_plan.q; r++) {
		for (uint32_t j = 0; j < ap_plan.m; j++) {
			uint64_t y0 = r + (uint64_t)ap_plan.q * j;
			if (y0 >= ap_plan.fh) {
				continue;
			}
			if (ap_plan.nclasses >= AP_MAX_CLASSES) {
				andr_log("present: rows do not split into <= %u copies (q %u m %u)\n", AP_MAX_CLASSES,
				    ap_plan.q, ap_plan.m);
				return -1;
			}
			uint64_t rows = (ap_plan.fh - y0 + step - 1) / step;
			ap_plan.cls[ap_plan.nclasses++] = (struct ap_class){ (uint32_t)y0, (uint32_t)rows,
				                                               ap_plan.row[y0] };
		}
	}
	if (AP_OFF_STAGE + (uint64_t)ap_plan.stage_pitch * ap_plan.lh > ap_pool_gpu) {
		andr_log("present: stage image %u x %u rows does not fit in the pool\n", ap_plan.stage_pitch,
		    ap_plan.lh);
		return -1;
	}
	return 0;
}

/* The panel memory at AP_GVA_PANEL, read/write, after checking that XNU manages none of it. */
static int
ap_panel_map(void)
{
	uint64_t base = ap_plan.panel_pa & ~0xfffull;
	uint64_t end = (ap_plan.panel_pa + (uint64_t)ap_plan.rb * ap_plan.ph + 0xfff) & ~0xfffull;
	uint64_t pool_lo = ap_g->pool_pa, pool_hi = ap_g->pool_pa + ap_pool_gpu;
	uint32_t pages = (uint32_t)((end - base) >> 12);
	if (base < AP_DRAM_BASE || end > AP_DRAM_END || pages > AP_PANEL_L3_MAX * 512u ||
	    (base < pool_hi && end > pool_lo)) {
		andr_log("present: panel 0x%llx..0x%llx outside DRAM, too large or over the pool\n",
		    (unsigned long long)base, (unsigned long long)end);
		return -1;
	}
	for (uint64_t pa = base; pa < end; pa += 0x1000) {
		if (pmap_valid_address(pa)) {
			andr_log("present: panel page 0x%llx is XNU-managed: not giving it to the GPU\n",
			    (unsigned long long)pa);
			return -1;
		}
	}
	uint64_t l2 = ap_table(andr_pool_pa(ap_g, ANDR_OFF_GT_L1), (unsigned)((AP_GVA_PANEL >> 30) & 0x1ff));
	if (!l2) {
		return -1;
	}
	for (uint32_t t = 0; t * 512u < pages; t++) {
		uint64_t l3 = ap_table(l2, (unsigned)(((AP_GVA_PANEL >> 21) + t) & 0x1ff));
		if (!l3) {
			andr_log("present: no page table for the panel (arena full or a block there)\n");
			return -1;
		}
		for (uint32_t k = 0; k < 512; k++) {
			uint32_t i = t * 512u + k;
			ap_pte[k] = i < pages ? (base + 0x1000ull * i) | AP_PTE_RW : 0;
		}
		(void)andr_write(l3, ap_pte, sizeof(ap_pte));
	}
	if (ap_tlb_sync()) {
		andr_log("present: TLB sync timeout after mapping the panel\n");
		return -1;
	}
	ap_plan.panel_gva = AP_GVA_PANEL + (ap_plan.panel_pa & 0xfff);
	ap_plan.panel_pages = pages;
	return 0;
}

int
andromeda_gpu_present_setup(const struct andr_present_geom *gm)
{
	if (!andromeda_gpu_present_ready() || gm == NULL) {
		return -1;
	}
	if (ap_plan.ok) {
		return 0;
	}
	if (ap_plan_make(gm)) {
		ap_fail("the fit does not split into GPU copies");
		return -1;
	}
	if (ap_panel_map()) {
		ap_fail("panel memory not mappable for the GPU");
		return -1;
	}
	ap_plan.ok = true;
	andr_log("present: panel %ux%u stride %u at pa 0x%llx (not XNU-managed, %u GPU pages, native stride kept); "
	    "fit %ux%u -> %ux%u at y %u\n", ap_plan.pw, ap_plan.ph, ap_plan.rb, (unsigned long long)ap_plan.panel_pa,
	    ap_plan.panel_pages, ap_plan.lw, ap_plan.lh, ap_plan.fw, ap_plan.fh, ap_plan.fy);
	andr_log("present: plan: %u column copies into the pool stage (stride %u), %u row copies onto the panel "
	    "(%u fitted rows <- %u logical, dst stride %u, src stride %u): %u blits per frame\n", ap_plan.nruns,
	    ap_plan.stage_pitch, ap_plan.nclasses, ap_plan.q, ap_plan.p, ap_plan.dst_pitch, ap_plan.src_pitch,
	    ap_plan.nruns + ap_plan.nclasses);
	return 0;
}

/* ---- surfaces: read-only GPU mappings ---- */

static int
ap_slot_tables(uint32_t slot)
{
	if (!ap_l2_slots) {
		ap_l2_slots = ap_table(andr_pool_pa(ap_g, ANDR_OFF_GT_L1), (unsigned)((AP_GVA_SLOTS >> 30) & 0x1ff));
		if (!ap_l2_slots) {
			return -1;
		}
	}
	for (uint32_t t = 0; t < AP_L3_PER_SLOT; t++) {
		if (!ap_slot_l3[slot][t]) {
			ap_slot_l3[slot][t] = ap_table(ap_l2_slots,
			    (unsigned)(((AP_GVA_SLOT(slot) >> 21) + t) & 0x1ff));
			if (!ap_slot_l3[slot][t]) {
				return -1;
			}
		}
	}
	return 0;
}

/* Write the slot's 4096 PTEs: pages (each page_size bytes, 4K steps) read-only, the rest invalid. */
static void
ap_slot_ptes(uint32_t slot, const uint64_t *pages, uint32_t npages, uint32_t sub)
{
	uint64_t n4k = (uint64_t)npages * sub;
	for (uint32_t t = 0; t < AP_L3_PER_SLOT; t++) {
		for (uint32_t k = 0; k < 512; k++) {
			uint64_t i = (uint64_t)t * 512u + k;
			ap_pte[k] = (pages && i < n4k) ? (pages[i / sub] + 0x1000ull * (i % sub)) | AP_PTE_RO : 0;
		}
		(void)andr_write(ap_slot_l3[slot][t], ap_pte, sizeof(ap_pte));
	}
}

int
andromeda_gpu_present_map(uint32_t slot, const uint64_t *pages, uint32_t npages, uint32_t page_size)
{
	if (!andromeda_gpu_present_ready() || slot >= ANDR_PRESENT_SLOTS || ap_slot_bytes[slot] || pages == NULL ||
	    npages == 0 || page_size < 0x1000 || page_size > 0x10000 || (page_size & (page_size - 1)) ||
	    (uint64_t)npages * page_size > ANDR_PRESENT_SLOT_BYTES) {
		return -1;
	}
	uint64_t pool_lo = ap_g->pool_pa, pool_hi = ap_g->pool_pa + ap_pool_gpu;
	uint64_t panel_lo = ap_plan.panel_pa & ~0xfffull, panel_hi = panel_lo + 0x1000ull * ap_plan.panel_pages;
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t pa = pages[i];
		if ((pa & (page_size - 1)) || pa < AP_DRAM_BASE || pa + page_size > AP_DRAM_END ||
		    (pa < pool_hi && pa + page_size > pool_lo) || (pa < panel_hi && pa + page_size > panel_lo) ||
		    !pmap_valid_address(pa)) {
			andr_log("present: surface page %u at 0x%llx is not ordinary XNU RAM; not mapped\n", i,
			    (unsigned long long)pa);
			return -1;
		}
	}
	if (ap_slot_tables(slot)) {
		ap_fail("no page tables for the surface slots");
		return -1;
	}
	ap_slot_ptes(slot, pages, npages, page_size / 0x1000);
	if (ap_tlb_sync()) {
		/* Nothing was submitted with these PTEs: clearing them in memory is enough. */
		ap_slot_ptes(slot, NULL, 0, 1);
		ap_fail("SMMU TLB sync timeout");
		return -1;
	}
	ap_slot_bytes[slot] = (uint64_t)npages * page_size;
	andr_log("present: surface -> slot %u (%u pages of %u B, read-only for the GPU)\n", slot, npages, page_size);
	return 0;
}

int
andromeda_gpu_present_unmap(uint32_t slot)
{
	if (slot >= ANDR_PRESENT_SLOTS) {
		return -1;
	}
	if (!ap_slot_bytes[slot]) {
		return 0;
	}
	ap_slot_ptes(slot, NULL, 0, 1);
	if (ap_tlb_sync()) {
		andr_log("present: TLB sync timeout unmapping slot %u: its surface stays wired\n", slot);
		ap_fail("SMMU TLB sync timeout");
		return -1;
	}
	ap_slot_bytes[slot] = 0;
	return 0;
}

/* ---- the IB ---- */

/* The A640 state turnip writes before any work (flame's blit_init, from a turnip capture). */
static const uint32_t ap_blit_init[][2] = {
	{ 0x9804, 0x1f }, { 0x9805, 1 }, { 0xa0f8, 1 }, { 0xb600, 0x8000 }, { 0x8600, 0 },
	{ 0xae03, 0x420 }, { 0xae00, 0 }, { 0xbe04, 0 }, { 0x9600, 0x2000000 }, { 0x0e12, 1 },
	{ 0x8e01, 1 }, { 0x8e04, 0x4100000 }, { 0xae04, 0 }, { 0xae0f, 0x3f }, { 0xb605, 0x44 },
	{ 0xbe00, 0x80 }, { 0xbe01, 0 }, { 0xab20, 0 }, { 0xb182, 0 }, { 0xbb11, 0 }, { 0xa9a8, 0 },
	{ 0xab00, 5 }, { 0xa009, 3 }, { 0x8811, 0x10 }, { 0x8110, 0 }, { 0x8818, 0 }, { 0x8819, 0 },
	{ 0x881a, 0 }, { 0x881b, 0 }, { 0x881c, 0 }, { 0x881d, 0 }, { 0x881e, 0 }, { 0x88f0, 0 },
	{ 0x9236, 0 }, { 0x9300, 0 }, { 0x9306, 1 }, { 0xb183, 0 }, { 0x80af, 0 }, { 0x8099, 0 },
	{ 0x980a, 0 }, { 0x9210, 0 }, { 0x9211, 0 }, { 0x9602, 0 }, { 0x9e72, 0 }, { 0xb309, 0xa2 },
	{ 0xb986, 0xfc }, { 0xa007, 0 }, { 0x8864, 0 }, { 0xb800, 0x102 },
	{ 0x8e07, 0x10000000 },		/* RB_CCU_CNTL: sysmem, full CCU */
};

static uint32_t ap_ib[AP_IB_DW];
static uint32_t ap_n;
static bool ap_overflow;

static void
ap_emit(uint32_t v)
{
	if (ap_n < AP_IB_DW) {
		ap_ib[ap_n++] = v;
	} else {
		ap_overflow = true;
	}
}

static void
ap_reg(uint32_t reg, uint32_t v)
{
	ap_emit(ap_pkt4(reg, 1));
	ap_emit(v);
}

static void
ap_event(uint32_t ev, bool ts)
{
	uint64_t va = andr_pool_va(AP_OFF_TS);
	ap_emit(ap_pkt7(AP_CP_EVENT_WRITE, ts ? 4 : 1));
	ap_emit(ev);
	if (ts) {
		ap_emit((uint32_t)va);
		ap_emit((uint32_t)(va >> 32));
		ap_emit(0);
	}
}

/* flame a6_blit_ib's head: caches, A640 init, 2D control for BGRA8 (any 32-bit pixels) copies. */
static void
ap_prologue(void)
{
	uint64_t border = andr_pool_va(AP_OFF_BORDER);
	ap_event(AP_EV_CACHE_INVALIDATE, false);
	ap_reg(0xbb08, 0xfffff);		/* SP_UPDATE_CNTL */
	ap_emit(ap_pkt7(ANDR_CP_WAIT_FOR_IDLE, 0));
	for (unsigned i = 0; i < sizeof(ap_blit_init) / sizeof(ap_blit_init[0]); i++) {
		ap_reg(ap_blit_init[i][0], ap_blit_init[i][1]);
	}
	ap_emit(ap_pkt4(0xb302, 2));		/* TPL1_GFX_BORDER_COLOR_BASE */
	ap_emit((uint32_t)border);
	ap_emit((uint32_t)(border >> 32));
	ap_emit(ap_pkt4(0xb180, 2));		/* TPL1_CS_BORDER_COLOR_BASE */
	ap_emit((uint32_t)border);
	ap_emit((uint32_t)(border >> 32));
	ap_event(AP_EV_CCU_FLUSH_COLOR_TS, true);
	ap_event(AP_EV_CCU_INVALIDATE_COLOR, false);
	ap_event(AP_EV_CACHE_INVALIDATE, false);
	ap_emit(ap_pkt7(ANDR_CP_WAIT_FOR_IDLE, 0));
	ap_reg(0x8c01, 0);			/* RB_A2D_PIXEL_CNTL: no blending */
	ap_reg(0x8c00, 0x00f03000);		/* RB_A2D_BLT_CNTL: FMT6_8_8_8_8_UNORM, mask f */
	ap_reg(0x8400, 0x00f03000);		/* GRAS_A2D_BLT_CNTL */
	ap_reg(0xacc0, 0xf180);			/* SP_A2D_OUTPUT_INFO */
}

/* flame a6_blit_ib's tail: flush the colour cache and UCHE to memory. */
static void
ap_epilogue(void)
{
	ap_event(AP_EV_CCU_FLUSH_COLOR_TS, true);
	ap_event(AP_EV_CCU_FLUSH_DEPTH_TS, true);
	ap_event(AP_EV_CACHE_FLUSH_TS, true);
}

/*
 * One unscaled, unfiltered copy of a w x h rectangle: texture (src, src_w x
 * src_h, src_pitch) at (sx, sy) -> destination (dst, dst_pitch) at (dx, dy).
 * Same registers and values as flame's a6_blit_ib, filter bit clear.
 */
static void
ap_blit(uint64_t src, uint32_t src_w, uint32_t src_h, uint32_t src_pitch, uint32_t sx, uint32_t sy,
    uint64_t dst, uint32_t dst_pitch, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
	ap_emit(ap_pkt4(0x8405, 2));		/* GRAS_A2D_DEST_TL, _BR (inclusive) */
	ap_emit(dx | (dy << 16));
	ap_emit((dx + w - 1) | ((dy + h - 1) << 16));
	ap_emit(ap_pkt4(0x8401, 4));		/* GRAS_A2D_SRC_XMIN..YMAX, 24.8, inclusive */
	ap_emit(sx << 8);
	ap_emit((sx + w - 1) << 8);
	ap_emit(sy << 8);
	ap_emit((sy + h - 1) << 8);
	ap_emit(ap_pkt4(0xb4c0, 5));		/* TPL1_A2D_SRC_TEXTURE_INFO, SIZE, BASE, PITCH */
	ap_emit(0x00500430);			/* 8888 UNORM, linear, swap WZYX, UNK20|UNK22, nearest */
	ap_emit(src_w | (src_h << 15));
	ap_emit((uint32_t)src);
	ap_emit((uint32_t)(src >> 32));
	ap_emit(src_pitch << 3);		/* PITCH field: bits 9..23, 64-byte units */
	ap_emit(ap_pkt4(0xb4ca, 3));		/* TPL1_A2D_SRC_TEXTURE_FLAG_BASE, _PITCH (unused) */
	ap_emit((uint32_t)src);
	ap_emit((uint32_t)(src >> 32));
	ap_emit(0);
	ap_emit(ap_pkt4(0x8c17, 4));		/* RB_A2D_DEST_BUFFER_INFO, BASE, PITCH (64-byte units) */
	ap_emit(0x430);
	ap_emit((uint32_t)dst);
	ap_emit((uint32_t)(dst >> 32));
	ap_emit(dst_pitch >> 6);
	ap_emit(ap_pkt4(0x8c20, 3));		/* RB_A2D_DEST_FLAG_BUFFER_BASE, _PITCH (unused) */
	ap_emit((uint32_t)dst);
	ap_emit((uint32_t)(dst >> 32));
	ap_emit(0);
	ap_emit(ap_pkt7(AP_CP_BLIT, 1));
	ap_emit(AP_BLIT_OP_SCALE);
}

static uint32_t
ap_build(uint32_t slot, const struct andr_present_src *s)
{
	uint64_t surf = AP_GVA_SLOT(slot) + s->offset;
	uint64_t stage = andr_pool_va(AP_OFF_STAGE);
	ap_n = 0;
	ap_overflow = false;

	/* Pass 1: fitted columns of every logical row, surface -> stage. */
	ap_prologue();
	for (uint32_t i = 0; i < ap_plan.nruns; i++) {
		const struct ap_run *r = &ap_plan.run[i];
		ap_blit(surf, ap_plan.lw, ap_plan.lh, s->bytes_per_row, r->c0, 0, stage, ap_plan.stage_pitch,
		    r->x0, 0, r->len, ap_plan.lh);
	}
	ap_epilogue();

	/* Pass 2: fitted rows, stage -> panel, one copy per class; the stage is read back through the
	 * texture cache, so the head invalidates it again (as flame's chained a6_blit_ib streams). */
	ap_prologue();
	for (uint32_t i = 0; i < ap_plan.nclasses; i++) {
		const struct ap_class *c = &ap_plan.cls[i];
		uint64_t dst = ap_plan.panel_gva + (uint64_t)(c->y0 + ap_plan.fy) * ap_plan.rb;
		uint32_t dxo = (uint32_t)(dst & 63) / 4;	/* native 4320-byte rows: 0 or 8 pixels */
		ap_blit(stage + (uint64_t)c->l0 * ap_plan.stage_pitch, ap_plan.fw, c->rows, ap_plan.src_pitch, 0, 0,
		    dst & ~63ull, ap_plan.dst_pitch, dxo, 0, ap_plan.fw, c->rows);
	}
	ap_epilogue();
	return ap_overflow ? 0 : ap_n;
}

/*
 * Ring: IB + WAIT_FOR_IDLE + CP_MEM_WRITE fence (flame a6_submit_ib); wait
 * for the fence, bounded. 0 = landed, -1 = timeout, -2 = the ring was busy.
 */
static int
ap_submit(uint32_t ndw)
{
	struct andr_gpu *g = ap_g;
	if (andr_rd(ANDR_GREG(AP_CP_RB_RPTR)) != g->wptr) {
		return -2;
	}
	uint64_t ib = andr_pool_va(AP_OFF_IB), fva = andr_pool_va(AP_OFF_FENCE);
	uint32_t fence = ++ap_fence;
	if (fence == 0) {
		fence = ++ap_fence;
	}
	const uint32_t dw[] = {
		ap_pkt7(AP_CP_INDIRECT_BUFFER, 3), (uint32_t)ib, (uint32_t)(ib >> 32), ndw,
		ap_pkt7(ANDR_CP_WAIT_FOR_IDLE, 0),
		ap_pkt7(ANDR_CP_MEM_WRITE, 3), (uint32_t)fva, (uint32_t)(fva >> 32), fence,
	};
	const uint32_t n = sizeof(dw) / 4;
	uint32_t w = g->wptr, first = n < ANDR_RING_DW - w ? n : ANDR_RING_DW - w;
	(void)andr_write(andr_pool_pa(g, ANDR_OFF_RING) + 4ull * w, dw, 4ull * first);
	if (first < n) {
		(void)andr_write(andr_pool_pa(g, ANDR_OFF_RING), dw + first, 4ull * (n - first));
	}
	g->wptr = (w + n) % ANDR_RING_DW;
	andr_wr(ANDR_GREG(AP_CP_RB_WPTR), g->wptr);

	uint64_t start = mach_absolute_time(), limit = 0;
	nanoseconds_to_absolutetime((uint64_t)AP_FENCE_TIMEOUT_US * 1000, &limit);
	for (uint32_t i = 0; i < 200 + AP_FENCE_TIMEOUT_US / 100; i++) {
		if (andr_rd(andr_pool_pa(g, AP_OFF_FENCE)) == fence) {
			return 0;
		}
		if (mach_absolute_time() - start > limit) {
			break;
		}
		delay(i < 200 ? 5 : 100);
	}
	return andr_rd(andr_pool_pa(g, AP_OFF_FENCE)) == fence ? 0 : -1;
}

/* ---- the frame ---- */

static int
ap_refuse(uint32_t why, const char *what)
{
	ap_st.refused++;
	if (why < 32 && !(ap_st.refused_why & (1u << why))) {
		ap_st.refused_why |= 1u << why;
		andr_log("present: frame left to the CPU: %s (logged once)\n", what);
	}
	return -1;
}

static bool
ap_thermal_ok(uint64_t now)
{
	uint64_t period = 0;
	nanoseconds_to_absolutetime((uint64_t)AP_THERMAL_SECS * 1000000000ull, &period);
	if (ap_st.thermal_at && now - ap_st.thermal_at < period) {
		return true;
	}
	ap_st.thermal_at = now;
	int mc = 0;
	bool valid = andr_gpu_temperature(&mc);
	if (valid && mc < (int)ap_arg_hot * 1000) {
		return true;
	}
	andr_log("present: GPU temperature %s %d mC (limit %u C)\n", valid ? "at" : "UNREADABLE", mc, ap_arg_hot);
	ap_fail(valid ? "GPU too hot" : "GPU temperature unreadable");
	return false;
}

static void
ap_stats(uint64_t clean, uint64_t gpu, uint64_t all)
{
	ap_st.frames++;
	ap_st.total++;
	ap_st.clean_abs += clean;
	ap_st.gpu_abs += gpu;
	ap_st.all_abs += all;
	if (gpu > ap_st.gpu_max_abs) {
		ap_st.gpu_max_abs = gpu;
	}
	if (ap_st.total == 1) {
		andr_log("present: first GPU frame on the panel (clean %llu us, GPU %llu us, total %llu us)\n",
		    (unsigned long long)ap_us(clean), (unsigned long long)ap_us(gpu), (unsigned long long)ap_us(all));
	}
	if (ap_st.frames < AP_STATS_FRAMES) {
		return;
	}
	uint64_t n = ap_st.frames;
	andr_log("present: %llu GPU frames (%llu so far): cache clean avg %llu us, GPU avg %llu us max %llu us, "
	    "total avg %llu us; checks %u (%u identical, %u inconclusive); %llu frames left to the CPU\n",
	    (unsigned long long)n, (unsigned long long)ap_st.total, (unsigned long long)(ap_us(ap_st.clean_abs) / n),
	    (unsigned long long)(ap_us(ap_st.gpu_abs) / n), (unsigned long long)ap_us(ap_st.gpu_max_abs),
	    (unsigned long long)(ap_us(ap_st.all_abs) / n), ap_st.checks_done, ap_st.checks_same,
	    ap_st.checks_unsure, (unsigned long long)ap_st.refused);
	ap_st.frames = ap_st.clean_abs = ap_st.gpu_abs = ap_st.all_abs = ap_st.gpu_max_abs = 0;
}

/* After the fence: the panel against the CPU result. 0 = keep going, -1 = GPU off. */
static int
ap_check(const struct andr_present_src *s, uint32_t before, bool initial)
{
	struct ap_sums r;
	uint64_t t0 = mach_absolute_time();
	ap_sums(s, true, &r);
	uint64_t us = ap_us(mach_absolute_time() - t0);
	uint64_t px = (uint64_t)ap_plan.fw * ap_plan.fh;
	bool stable = r.cpu == before;
	const char *verdict;
	int rc = 0;
	ap_st.checks_done++;
	if (r.diff == 0 && r.gpu == r.cpu) {
		ap_st.checks_same++;
		verdict = "identical";
	} else if (!stable) {
		ap_st.checks_unsure++;
		verdict = "inconclusive: the surface changed while it was copied";
	} else if (!s->console_quiet && r.diff * 4 <= px) {
		ap_st.checks_unsure++;
		verdict = "inconclusive: the kernel console draws on the panel (sweetfb_noconsole=1 for a strict check)";
	} else {
		verdict = "DIFFERENT";
		rc = -1;
	}
	if (r.diff) {
		andr_log("present: check %u: GPU crc %08x, CPU crc %08x, %llu of %llu px differ (first at panel %u,%u: "
		    "%08x, want %08x), %llu us: %s\n", ap_st.checks_done, r.gpu, r.cpu, (unsigned long long)r.diff,
		    (unsigned long long)px, r.dx, r.dy, r.got, r.want, (unsigned long long)us, verdict);
	} else if (initial || !stable) {
		andr_log("present: check %u: GPU crc %08x, CPU crc %08x over %llu px, %llu us: %s\n", ap_st.checks_done,
		    r.gpu, r.cpu, (unsigned long long)px, (unsigned long long)us, verdict);
	}
	if (initial && ap_st.checks_done == ap_arg_checks && !rc) {
		andr_log("present: first %u GPU frames checked: %u identical to the CPU copy, %u inconclusive, 0 different\n",
		    ap_st.checks_done, ap_st.checks_same, ap_st.checks_unsure);
	}
	if (rc) {
		ap_fail("a GPU frame differs from the CPU copy");
	}
	return rc;
}

int
andromeda_gpu_present_frame(uint32_t slot, const struct andr_present_src *s)
{
	if (!andromeda_gpu_present_ready() || !ap_plan.ok || s == NULL) {
		return -1;
	}
	if (slot >= ANDR_PRESENT_SLOTS || !ap_slot_bytes[slot]) {
		return ap_refuse(0, "surface not mapped");
	}
	uint64_t used = (uint64_t)s->bytes_per_row * (ap_plan.lh - 1) + (uint64_t)ap_plan.lw * 4;
	if (s->kva == 0 || s->width < ap_plan.lw || s->height < ap_plan.lh || (s->bytes_per_row & 63) ||
	    s->bytes_per_row > AP_MAX_SRC_PITCH || s->bytes_per_row < ap_plan.lw * 4 || (s->offset & 15) ||
	    s->offset + used > s->len || s->offset + used > ap_slot_bytes[slot]) {
		return ap_refuse(1, "surface size, offset or stride (the 2D engine wants 64-byte strides)");
	}
	uint64_t t0 = mach_absolute_time();
	if (!ap_thermal_ok(t0)) {
		return -1;
	}
	/* The CPU drew the frame through the cache; the GPU reads memory. */
	CleanPoC_DcacheRegion((vm_offset_t)(s->kva + s->offset), (size_t)used);
	uint64_t t1 = mach_absolute_time();

	/* A check frame first sums what the CPU copy would show (not timed). */
	bool initial = ap_st.checks_done < ap_arg_checks;
	bool check = initial || (ap_arg_every && ap_st.total && (ap_st.total % ap_arg_every) == 0);
	struct ap_sums before = { 0 };
	if (check) {
		ap_sums(s, false, &before);
	}
	uint64_t t1c = mach_absolute_time();

	uint32_t n = ap_build(slot, s);
	if (n == 0) {
		ap_fail("IB overflow");
		return -1;
	}
	(void)andr_write(andr_pool_pa(ap_g, AP_OFF_IB), ap_ib, 4ull * n);
	uint64_t t2 = mach_absolute_time();
	int rc = ap_submit(n);
	if (rc) {
		ap_diag(rc == -2 ? "ring busy before the frame" : "GPU frame timeout");
		ap_fail(rc == -2 ? "ring not idle" : "GPU fence timeout");
		return -1;
	}
	uint64_t t3 = mach_absolute_time();
	if (check && ap_check(s, before.cpu, initial)) {
		return -1;
	}
	ap_stats(t1 - t0, t3 - t2, (t3 - t0) - (t1c - t1));
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
