/*
 * Host test of andromeda_gpu_present.c (phase F4, patch 0120):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined xnu/test_andromeda_gpu_present.c -o /tmp/present-test
 *   /tmp/present-test
 *
 * The real environment (andromeda_gpu.c: pool, MMIO, SMC) is replaced by a
 * small model: the pool, the panel memory and scattered 16K surface pages as
 * host buffers, the GPU page tables walked like the SMMU's stage 1 (4K
 * granule, 39-bit VA, AP bits enforced), and a command processor that runs
 * the ring and the IB: PKT4 register writes, CP_INDIRECT_BUFFER,
 * CP_WAIT_FOR_IDLE, CP_EVENT_WRITE, CP_MEM_WRITE, and CP_BLIT as an
 * *unscaled* copy (the model asserts that every blit is unscaled, unfiltered,
 * 64-byte aligned where the registers say so, and inside its texture).
 *
 * What it shows: the plan built from 0094's fit tables, the GVA layout, the
 * native-stride trick and the register packing produce exactly the CPU copy
 * of 0094 (whole panel compared byte for byte, every fitted pixel written
 * once, nothing else written, no GPU write outside the pool and the panel),
 * for andromeda's geometry and a few others; and the fallbacks (check
 * mismatch, console, source change, fence timeout, bad pages, managed panel).
 * What it cannot show: how the real A640 executes these packets. That is
 * for the phone (the CRC check of the first frames, andrgpupresentcheck).
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef unsigned int boolean_t;
#define ARM64_BOARD_CONFIG_SWEET 1
#define ANDR_PRESENT_HOST_TEST 1

/* ---- kernel environment ---- */
static uint64_t now_ns;
static uint32_t arg_present = 1, arg_check = 8, arg_every = 1000;

uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs);
void delay(int usec);
int PE_parse_boot_argn(const char *name, void *arg, int size);

uint64_t mach_absolute_time(void) { return now_ns += 1000; }
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns) { *ns = abs; }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
void delay(int usec) { now_ns += (uint64_t)usec * 1000; }

int
PE_parse_boot_argn(const char *name, void *arg, int size)
{
	assert(size == 4);
	uint32_t v;
	if (!strcmp(name, "andrgpupresent")) {
		v = arg_present;
	} else if (!strcmp(name, "andrgpupresentcheck")) {
		v = arg_check;
	} else if (!strcmp(name, "andrgpupresentevery")) {
		v = arg_every;
	} else {
		return 0;
	}
	memcpy(arg, &v, 4);
	return 1;
}

#include "andromeda_gpu_present.c"

/* ---- memory model ---- */

#define POOL_PA		0x1ff000000ull
#define PANEL_PA	0x9c000000ull
#define RAM_PA		0x140000000ull	/* "XNU-managed" RAM: surface pages come from here */
#define RAM_SIZE	0x40000000ull
#define PG		16384u
#define MMIO_BASE	0x02c00000ull
#define MMIO_WORDS	0x40000u

static uint8_t *pool;
static uint8_t *panel, *panel_ref;
static uint64_t panel_bytes;
static uint8_t *surf;
static uint32_t surf_pages;
static uint32_t page_slot[1024];	/* page k of the surface sits at RAM_PA + page_slot[k] * PG */
static int32_t slot_page[0x10000];	/* inverse (RAM page index -> surface page), -1 none */
static uint8_t *coverage;		/* times each panel pixel was written by the GPU */
static uint32_t mmio[MMIO_WORDS];

static struct {
	uint32_t rptr;
	bool hang, corrupt, mutate_src, smmu_stuck;
	uint64_t blits, panel_px, tlbi, gpu_writes_outside, ro_faults;
	uint32_t regs[0x40000];
} emu;

static struct andr_gpu gpu;
static uint32_t cur_pw, cur_ph, cur_rb;
static bool clean_seen;

static uint8_t *
host(uint64_t pa, uint64_t len, int *kind)
{
	if (pa >= POOL_PA && pa + len <= POOL_PA + ANDR_POOL_SIZE) {
		*kind = 0;
		return pool + (pa - POOL_PA);
	}
	if (pa >= PANEL_PA && pa + len <= PANEL_PA + panel_bytes) {
		*kind = 1;
		return panel + (pa - PANEL_PA);
	}
	if (pa >= RAM_PA && pa + len <= RAM_PA + RAM_SIZE) {
		uint64_t idx = (pa - RAM_PA) / PG;
		if (idx < 0x10000 && slot_page[idx] >= 0 && (pa % PG) + len <= PG) {
			*kind = 2;
			return surf + (uint64_t)slot_page[idx] * PG + (pa % PG);
		}
	}
	*kind = -1;
	return NULL;
}

boolean_t
pmap_valid_address(uint64_t pa)
{
	return pa >= RAM_PA && pa < RAM_PA + RAM_SIZE;
}

void
CleanPoC_DcacheRegion(vm_offset_t va, size_t length)
{
	assert(va >= (vm_offset_t)surf && va + length <= (vm_offset_t)surf + (uint64_t)surf_pages * PG);
	clean_seen = true;
}

/* andromeda_gpu.h environment */
uint32_t
andr_rd(uint64_t pa)
{
	int kind;
	uint8_t *p = host(pa, 4, &kind);
	if (p && kind == 0) {
		uint32_t v;
		memcpy(&v, p, 4);
		return v;
	}
	assert(pa >= MMIO_BASE && pa < MMIO_BASE + 4ull * MMIO_WORDS);
	if (pa == ANDR_GREG(AP_CP_RB_RPTR)) {
		return emu.rptr;
	}
	if (pa == ANDR_SMMU_CB(gpu.gpu_cb) + AP_CB_TLBSTATUS) {
		return emu.smmu_stuck ? 1 : 0;
	}
	return mmio[(pa - MMIO_BASE) / 4];
}

static void emu_run(uint32_t wptr);

void
andr_wr(uint64_t pa, uint32_t v)
{
	assert(pa >= MMIO_BASE && pa < MMIO_BASE + 4ull * MMIO_WORDS);	/* the module never pokes RAM this way */
	mmio[(pa - MMIO_BASE) / 4] = v;
	if (pa == ANDR_GREG(AP_CP_RB_WPTR)) {
		emu_run(v);
	} else if (pa == ANDR_SMMU_CB(gpu.gpu_cb) + AP_CB_TLBIALL) {
		emu.tlbi++;
	}
}

int
andr_write(uint64_t pa, const void *buf, size_t len)
{
	int kind;
	uint8_t *p = host(pa, len, &kind);
	assert(p && kind == 0);		/* CPU writes through andr_write: pool only */
	memcpy(p, buf, len);
	return 0;
}

int
andr_read(uint64_t pa, void *buf, size_t len)
{
	int kind;
	uint8_t *p = host(pa, len, &kind);
	assert(p && kind == 0);
	memcpy(buf, p, len);
	return 0;
}

void
andr_log(const char *fmt, ...)
{
	va_list ap;
	printf("ANDROMEDA GPU: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

bool andr_gx_up(void) { return true; }
bool andr_gpu_temperature(int *max_mc) { *max_mc = 41000; return true; }

/* ---- GPU model ---- */

static uint64_t
pool64(uint64_t pa)
{
	int kind;
	uint8_t *p = host(pa, 8, &kind);
	assert(p && kind == 0);		/* page tables live in the pool */
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* SMMU stage 1, 4K granule, T0SZ 25: L1 (1 GB) -> L2 (2 MB block or table) -> L3 (4K page). */
static bool
translate(uint64_t va, bool write, uint64_t *pa)
{
	if (va >> 39) {
		return false;
	}
	uint64_t e = pool64(POOL_PA + ANDR_OFF_GT_L1 + 8 * ((va >> 30) & 0x1ff));
	if ((e & 3) != 3) {
		return false;
	}
	e = pool64((e & 0xfffffff000ull) + 8 * ((va >> 21) & 0x1ff));
	uint64_t out;
	if ((e & 3) == 1) {
		out = (e & 0xffffe00000ull) + (va & 0x1fffff);
	} else if ((e & 3) == 3) {
		e = pool64((e & 0xfffffff000ull) + 8 * ((va >> 12) & 0x1ff));
		if ((e & 3) != 3) {
			return false;
		}
		out = (e & 0xfffffff000ull) + (va & 0xfff);
	} else {
		return false;
	}
	unsigned ap = (unsigned)(e >> 6) & 3;
	if (!(e & (1u << 10)) || !(ap & 1)) {
		return false;
	}
	if (write && (ap & 2)) {
		emu.ro_faults++;
		return false;
	}
	*pa = out;
	return true;
}

static uint32_t
gva_rd(uint64_t va)
{
	uint64_t pa;
	int kind;
	assert(translate(va, false, &pa));
	uint8_t *p = host(pa, 4, &kind);
	assert(p);
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static void
gva_wr(uint64_t va, uint32_t v)
{
	uint64_t pa;
	int kind;
	assert(translate(va, true, &pa));
	uint8_t *p = host(pa, 4, &kind);
	assert(p);
	if (kind != 0 && kind != 1) {
		emu.gpu_writes_outside++;
		return;
	}
	if (kind == 1) {
		uint64_t off = pa - PANEL_PA;
		if (off < (uint64_t)cur_rb * cur_ph && (off % cur_rb) < (uint64_t)cur_pw * 4) {
			coverage[(off / cur_rb) * cur_pw + (off % cur_rb) / 4]++;
		}
		emu.panel_px++;
	}
	memcpy(p, &v, 4);
}

static uint32_t
odd_parity(uint32_t v)
{
	return (uint32_t)(~__builtin_popcount(v)) & 1u;
}

static void
emu_blit(void)
{
	uint32_t *r = emu.regs;
	assert(r[0x8c00] == 0x00f03000 && r[0x8400] == 0x00f03000 && r[0x8c01] == 0 && r[0xacc0] == 0xf180);
	assert(r[0xb4c0] == 0x00500430);	/* BGRA8 linear, FILTER (bit 16) clear */
	assert(r[0x8c17] == 0x430);
	uint64_t src = r[0xb4c2] | ((uint64_t)r[0xb4c3] << 32);
	uint64_t dst = r[0x8c18] | ((uint64_t)r[0x8c19] << 32);
	assert(!(src & 15) && !(dst & 63));		/* a6xx.xml align="16" / align="64" */
	assert(!(r[0xb4c4] & 0x1ff) && !(r[0xb4c4] >> 24));
	uint32_t spitch = (r[0xb4c4] >> 9) * 64, dpitch = (r[0x8c1a] & 0xffff) * 64;
	uint32_t tw = r[0xb4c1] & 0x7fff, th = (r[0xb4c1] >> 15) & 0x7fff;
	for (unsigned i = 0; i < 4; i++) {
		assert(!(r[0x8401 + i] & 0xff));	/* integer source coordinates */
	}
	uint32_t sx0 = r[0x8401] >> 8, sx1 = r[0x8402] >> 8, sy0 = r[0x8403] >> 8, sy1 = r[0x8404] >> 8;
	uint32_t dx0 = r[0x8405] & 0x3fff, dy0 = (r[0x8405] >> 16) & 0x3fff;
	uint32_t dx1 = r[0x8406] & 0x3fff, dy1 = (r[0x8406] >> 16) & 0x3fff;
	assert(sx1 >= sx0 && sy1 >= sy0 && dx1 >= dx0 && dy1 >= dy0);
	assert(sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0);	/* unscaled */
	assert(sx1 < tw && sy1 < th);
	assert(spitch >= tw * 4 && dpitch >= (dx1 + 1) * 4);	/* rows never overlap */
	for (uint32_t y = 0; y <= dy1 - dy0; y++) {
		for (uint32_t x = 0; x <= dx1 - dx0; x++) {
			uint32_t v = gva_rd(src + (uint64_t)(sy0 + y) * spitch + (uint64_t)(sx0 + x) * 4);
			gva_wr(dst + (uint64_t)(dy0 + y) * dpitch + (uint64_t)(dx0 + x) * 4, v);
		}
	}
	if (emu.corrupt && dst >= AP_GVA_PANEL && dst < AP_GVA_PANEL + 0x10000000ull) {
		emu.corrupt = false;
		uint64_t va = dst + (uint64_t)dy0 * dpitch + (uint64_t)dx0 * 4;
		gva_wr(va, gva_rd(va) ^ 0x00010000u);
	}
	if (emu.mutate_src && dst >= AP_GVA_PANEL) {
		emu.mutate_src = false;
		surf[gpu.pt_next % 64] ^= 0x5a;		/* the CPU draws into the surface meanwhile */
	}
	emu.blits++;
}

static void emu_exec(bool ib, uint64_t base, uint32_t ndw);

static uint32_t
fetch(bool ib, uint64_t base, uint32_t i)
{
	if (ib) {
		return gva_rd(base + 4ull * i);
	}
	uint32_t v;
	memcpy(&v, pool + ANDR_OFF_RING + 4ull * ((base + i) % ANDR_RING_DW), 4);
	return v;
}

static void
emu_exec(bool ib, uint64_t base, uint32_t ndw)
{
	for (uint32_t i = 0; i < ndw;) {
		uint32_t h = fetch(ib, base, i++);
		if ((h >> 28) == 4) {
			uint32_t cnt = h & 0x7f, reg = (h >> 8) & 0x3ffff;
			assert(((h >> 7) & 1) == odd_parity(cnt) && ((h >> 27) & 1) == odd_parity(reg));
			for (uint32_t k = 0; k < cnt; k++) {
				emu.regs[reg + k] = fetch(ib, base, i++);
			}
			continue;
		}
		assert((h >> 28) == 7);
		uint32_t cnt = h & 0x3fff, op = (h >> 16) & 0x7f, p[8] = { 0 };
		assert(((h >> 15) & 1) == odd_parity(cnt) && ((h >> 23) & 1) == odd_parity(op) && cnt <= 8);
		for (uint32_t k = 0; k < cnt; k++) {
			p[k] = fetch(ib, base, i++);
		}
		switch (op) {
		case AP_CP_INDIRECT_BUFFER:
			assert(!ib && cnt == 3);
			emu_exec(true, p[0] | ((uint64_t)p[1] << 32), p[2]);
			break;
		case ANDR_CP_WAIT_FOR_IDLE:
			assert(cnt == 0);
			break;
		case ANDR_CP_MEM_WRITE:
			assert(cnt == 3);
			gva_wr(p[0] | ((uint64_t)p[1] << 32), p[2]);
			break;
		case AP_CP_EVENT_WRITE:
			assert(cnt == 1 || cnt == 4);
			if (cnt == 4) {
				gva_wr(p[1] | ((uint64_t)p[2] << 32), p[3]);
			}
			break;
		case AP_CP_BLIT:
			assert(ib && cnt == 1 && p[0] == AP_BLIT_OP_SCALE);
			emu_blit();
			break;
		default:
			fprintf(stderr, "unexpected opcode %x\n", op);
			assert(0);
		}
	}
}

static void
emu_run(uint32_t wptr)
{
	if (emu.hang) {
		return;
	}
	uint32_t n = (wptr + ANDR_RING_DW - emu.rptr) % ANDR_RING_DW;
	emu_exec(false, emu.rptr, n);
	emu.rptr = wptr;
}

/* ---- 0094 on the CPU (start() tables, presentFit for one full-display layer) ---- */

static uint16_t fit_col[1536], fit_row[3072];
static int32_t fit_w, fit_h, fit_y;

static bool
fit_tables(uint32_t pw, uint32_t ph, uint32_t lw, uint32_t lh)
{
	if (!(lw > pw && pw <= 1536 && lw < 65535)) {
		return false;
	}
	fit_w = (int32_t)pw;
	fit_h = (int32_t)((uint64_t)lh * pw / lw);
	if (!(fit_h > 0 && fit_h <= 3072 && fit_h <= (int32_t)ph)) {
		return false;
	}
	fit_y = ((int32_t)ph - fit_h) / 2;
	for (int32_t x = 0; x < fit_w; x++) {
		fit_col[x] = (uint16_t)((uint64_t)x * lw / pw);
	}
	for (int32_t y = 0; y < fit_h; y++) {
		fit_row[y] = (uint16_t)((uint64_t)y * lw / pw);
	}
	return true;
}

static void
cpu_fit(uint8_t *pixels, uint32_t pw, uint32_t rb, uint32_t lw, uint32_t lh, const uint8_t *src, uint32_t bpr)
{
	int32_t lx = 0, ly = 0, sx = 0, sy = 0, w = (int32_t)lw, h = (int32_t)lh;
	int32_t x0 = (int32_t)(((uint64_t)lx * pw + lw - 1) / lw);
	int32_t x1 = (int32_t)(((uint64_t)(lx + w) * pw + lw - 1) / lw);
	int32_t y0 = (int32_t)(((uint64_t)ly * pw + lw - 1) / lw);
	int32_t y1 = (int32_t)(((uint64_t)(ly + h) * pw + lw - 1) / lw);
	x1 = x1 > fit_w ? fit_w : x1;
	y1 = y1 > fit_h ? fit_h : y1;
	for (int32_t y = y0; y < y1; y++) {
		int32_t srow = (int32_t)fit_row[y] - ly;
		int32_t py = y + fit_y;
		if (srow < 0 || srow >= h || py < 0 || py >= (int32_t)cur_ph) {
			continue;
		}
		uint32_t *dst = (uint32_t *)(pixels + (uint64_t)py * rb);
		const uint32_t *row = (const uint32_t *)(src + (uint64_t)(sy + srow) * bpr) + sx;
		for (int32_t x = x0; x < x1;) {
			int32_t scol = (int32_t)fit_col[x] - lx;
			int32_t run = 1;
			while (x + run < x1 && fit_col[x + run] == fit_col[x] + run) {
				run++;
			}
			if (scol >= 0 && scol + run <= w) {
				memcpy(dst + x, row + scol, (size_t)run * 4);
			}
			x += run;
		}
	}
}

/* ---- scenarios ---- */

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint32_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)rng;
}

static void
reset(uint32_t pw, uint32_t ph, uint32_t rb, uint32_t lw, uint32_t lh, uint32_t bpr)
{
	/* the module's statics (this file includes it) */
	ap_state = AP_UNPARSED;
	ap_waiting_logged = false;
	ap_g = NULL;
	ap_fence = 0;
	memset(&ap_plan, 0, sizeof(ap_plan));
	ap_l2_slots = 0;
	memset(ap_slot_l3, 0, sizeof(ap_slot_l3));
	memset(ap_slot_bytes, 0, sizeof(ap_slot_bytes));
	memset(&ap_st, 0, sizeof(ap_st));

	memset(&emu, 0, sizeof(emu));
	memset(mmio, 0, sizeof(mmio));
	free(pool);
	free(panel);
	free(panel_ref);
	free(surf);
	free(coverage);
	pool = calloc(1, ANDR_POOL_SIZE);
	cur_pw = pw;
	cur_ph = ph;
	cur_rb = rb;
	panel_bytes = (((uint64_t)rb * ph + 0xfff) & ~0xfffull) + 0x1000;
	panel = malloc(panel_bytes);
	panel_ref = malloc(panel_bytes);
	for (uint64_t i = 0; i < panel_bytes; i++) {
		panel[i] = panel_ref[i] = (uint8_t)(0xa5 ^ (i * 7));	/* console text / boot logo */
	}
	coverage = calloc((size_t)pw * ph, 1);
	surf_pages = (uint32_t)(((uint64_t)bpr * lh + PG - 1) / PG);
	assert(surf_pages <= 1024);
	surf = malloc((size_t)surf_pages * PG);
	memset(slot_page, 0xff, sizeof(slot_page));
	for (uint32_t k = 0; k < surf_pages; k++) {
		page_slot[k] = k * 37 + 3;	/* scattered, in no particular order */
	}
	for (uint32_t k = surf_pages; k > 1; k--) {
		uint32_t j = rnd() % k, t = page_slot[k - 1];
		page_slot[k - 1] = page_slot[j];
		page_slot[j] = t;
	}
	for (uint32_t k = 0; k < surf_pages; k++) {
		assert(page_slot[k] < 0x10000);
		slot_page[page_slot[k]] = (int32_t)k;
	}

	/* What andromeda_gpu_core.c leaves after step 4: the pool at GVA 0x10000000 in 2 MB blocks. */
	memset(&gpu, 0, sizeof(gpu));
	gpu.pool_pa = POOL_PA;
	gpu.gmu_cb = 0;
	gpu.gpu_cb = 1;
	gpu.zap_authenticated = true;
	gpu.wptr = ANDR_RING_DW - 4;	/* the next submit wraps the ring */
	emu.rptr = gpu.wptr;
	uint64_t l1 = ANDR_OFF_GT_L1, l2 = ANDR_OFF_GT_L2, e = (POOL_PA + l2) | 3;
	memcpy(pool + l1, &e, 8);
	for (uint64_t o = 0; o < ANDR_POOL_SIZE; o += 0x200000) {
		e = (POOL_PA + o) | (1u << 10) | (3u << 8) | (1u << 6) | 1u;
		memcpy(pool + l2 + 8 * (((ANDR_GVA_POOL + o) >> 21) & 0x1ff), &e, 8);
	}
	assert(fit_tables(pw, ph, lw, lh));
}

static struct andr_present_geom
geom(uint32_t lw, uint32_t lh)
{
	struct andr_present_geom g = {
		.panel_pa = PANEL_PA, .panel_kva = (uintptr_t)panel, .panel_w = cur_pw, .panel_h = cur_ph,
		.panel_rowbytes = cur_rb, .logical_w = lw, .logical_h = lh, .fit_w = (uint32_t)fit_w,
		.fit_h = (uint32_t)fit_h, .fit_y = (uint32_t)fit_y, .fit_col = fit_col, .fit_row = fit_row,
	};
	return g;
}

static int
map_slot(uint32_t slot)
{
	uint64_t pages[1024];
	for (uint32_t k = 0; k < surf_pages; k++) {
		pages[k] = RAM_PA + (uint64_t)page_slot[k] * PG;
	}
	return andromeda_gpu_present_map(slot, pages, surf_pages, PG);
}

static struct andr_present_src
source(uint32_t lw, uint32_t lh, uint32_t bpr, int quiet)
{
	struct andr_present_src s = {
		.kva = (uintptr_t)surf, .len = (uint64_t)surf_pages * PG, .offset = 0, .width = lw, .height = lh,
		.bytes_per_row = bpr, .console_quiet = quiet,
	};
	return s;
}

static void
new_frame(void)
{
	for (uint64_t i = 0; i < (uint64_t)surf_pages * PG / 4; i++) {
		uint32_t v = rnd();
		memcpy(surf + 4 * i, &v, 4);
	}
}

/* The whole panel (fit area, untouched rows, padding past each row, slack) equals the CPU copy. */
static void
expect_same_as_cpu(uint32_t lw, uint32_t lh, uint32_t bpr)
{
	cpu_fit(panel_ref, cur_pw, cur_rb, lw, lh, surf, bpr);
	assert(memcmp(panel, panel_ref, panel_bytes) == 0);
}

static void
scenario(const char *name, uint32_t pw, uint32_t ph, uint32_t rb, uint32_t lw, uint32_t lh, uint32_t bpr,
    unsigned frames)
{
	printf("--- %s: panel %ux%u stride %u, display %ux%u bpr %u\n", name, pw, ph, rb, lw, lh, bpr);
	reset(pw, ph, rb, lw, lh, bpr);
	struct andr_present_geom g = geom(lw, lh);
	assert(!andromeda_gpu_present_ready());		/* requested, not attached yet */
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	assert(andromeda_gpu_present_ready());
	assert(andromeda_gpu_present_setup(&g) == 0);
	assert(map_slot(1) == 0);
	assert(map_slot(1) != 0);			/* a slot is unmapped before reuse */
	struct andr_present_src s = source(lw, lh, bpr, 1);
	for (unsigned f = 0; f < frames; f++) {
		new_frame();
		memset(coverage, 0, (size_t)pw * ph);
		uint64_t before = emu.panel_px;
		clean_seen = false;
		assert(andromeda_gpu_present_frame(1, &s) == 0);
		assert(clean_seen);
		expect_same_as_cpu(lw, lh, bpr);
		assert(emu.panel_px - before == (uint64_t)fit_w * fit_h);
		for (uint32_t y = 0; y < ph; y++) {
			for (uint32_t x = 0; x < pw; x++) {
				bool fitted = y >= (uint32_t)fit_y && y < (uint32_t)(fit_y + fit_h) && x < (uint32_t)fit_w;
				assert(coverage[y * pw + x] == (fitted ? 1 : 0));
			}
		}
	}
	assert(emu.gpu_writes_outside == 0 && emu.ro_faults == 0);
	assert(ap_st.checks_same == ap_st.checks_done && ap_st.checks_unsure == 0);
	assert(ap_st.checks_done == (frames < arg_check ? frames : arg_check) + (arg_every ? (frames - 1) / arg_every : 0));
	/* Remap the slot with the pages in another order: the GPU follows the new mapping. */
	assert(andromeda_gpu_present_unmap(1) == 0);
	for (uint32_t k = 0; k < 16; k++) {
		uint64_t pte = pool64(ap_slot_l3[1][k / 512 % AP_L3_PER_SLOT] + 8 * (k % 512));
		assert(pte == 0);
	}
	uint32_t t = page_slot[0];
	page_slot[0] = page_slot[surf_pages - 1];
	page_slot[surf_pages - 1] = t;
	slot_page[page_slot[0]] = 0;
	slot_page[page_slot[surf_pages - 1]] = (int32_t)(surf_pages - 1);
	assert(map_slot(1) == 0);
	new_frame();
	assert(andromeda_gpu_present_frame(1, &s) == 0);
	expect_same_as_cpu(lw, lh, bpr);
	printf("    %llu blits, %u column copies + %u row copies per frame, %u page tables, %llu TLB invalidations\n",
	    (unsigned long long)emu.blits, ap_plan.nruns, ap_plan.nclasses, gpu.pt_next,
	    (unsigned long long)emu.tlbi);
}

static void
fallbacks(void)
{
	const uint32_t pw = 1080, ph = 2340, rb = 4320, lw = 1125, lh = 2436, bpr = 4608;
	struct andr_present_geom g;
	struct andr_present_src s;

	printf("--- off by default\n");
	reset(pw, ph, rb, lw, lh, bpr);
	arg_present = 0;
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	assert(!andromeda_gpu_present_ready());
	g = geom(lw, lh);
	assert(andromeda_gpu_present_setup(&g) != 0);
	assert(map_slot(0) != 0);
	andromeda_gpu_present_decline("test");		/* nothing to decline */
	assert(ap_state == AP_OFF);
	arg_present = 1;

	printf("--- declined (Metal has the ring, andrmtl=N): off for the boot, a later attach is ignored\n");
	reset(pw, ph, rb, lw, lh, bpr);
	andromeda_gpu_present_decline("Metal (andrmtl=1) has the GPU ring");
	assert(!andromeda_gpu_present_ready() && ap_state == AP_OFF);
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	assert(!andromeda_gpu_present_ready());
	g = geom(lw, lh);
	assert(andromeda_gpu_present_setup(&g) != 0 && map_slot(0) != 0);
	assert(gpu.wptr == ANDR_RING_DW - 4 && emu.rptr == gpu.wptr);	/* the ring untouched */

	printf("--- a wrong GPU pixel with a quiet console: GPU off, CPU copy\n");
	reset(pw, ph, rb, lw, lh, bpr);
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	g = geom(lw, lh);
	assert(andromeda_gpu_present_setup(&g) == 0 && map_slot(0) == 0);
	s = source(lw, lh, bpr, 1);
	new_frame();
	emu.corrupt = true;
	assert(andromeda_gpu_present_frame(0, &s) != 0);
	assert(!andromeda_gpu_present_ready() && ap_state == AP_FAILED);
	assert(andromeda_gpu_present_frame(0, &s) != 0);
	assert(andromeda_gpu_present_unmap(0) == 0);	/* surfaces can still be given back */

	printf("--- console drawing on the panel: inconclusive, GPU stays on\n");
	reset(pw, ph, rb, lw, lh, bpr);
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	g = geom(lw, lh);
	assert(andromeda_gpu_present_setup(&g) == 0 && map_slot(0) == 0);
	s = source(lw, lh, bpr, 0);
	new_frame();
	emu.corrupt = true;
	assert(andromeda_gpu_present_frame(0, &s) == 0 && andromeda_gpu_present_ready());
	assert(ap_st.checks_unsure == 1);

	printf("--- the surface changes during the copy: inconclusive, GPU stays on\n");
	s.console_quiet = 1;
	new_frame();
	emu.mutate_src = true;
	assert(andromeda_gpu_present_frame(0, &s) == 0 && andromeda_gpu_present_ready());
	assert(ap_st.checks_unsure == 2);

	printf("--- fence timeout: GPU off\n");
	emu.hang = true;
	new_frame();
	assert(andromeda_gpu_present_frame(0, &s) != 0 && !andromeda_gpu_present_ready());

	printf("--- surface pages that are not XNU RAM, or the panel's: refused\n");
	reset(pw, ph, rb, lw, lh, bpr);
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	g = geom(lw, lh);
	assert(andromeda_gpu_present_setup(&g) == 0);
	uint64_t bad[2] = { RAM_PA, PANEL_PA };
	assert(andromeda_gpu_present_map(0, &bad[1], 1, PG) != 0);
	uint64_t pool_page = POOL_PA;
	assert(andromeda_gpu_present_map(0, &pool_page, 1, PG) != 0);
	uint64_t odd = RAM_PA + 0x1000;
	assert(andromeda_gpu_present_map(0, &odd, 1, PG) != 0);
	assert(andromeda_gpu_present_map(0, bad, 1, PG) == 0 && andromeda_gpu_present_unmap(0) == 0);
	assert(andromeda_gpu_present_ready());

	printf("--- a surface with a stride the 2D engine cannot use: frame left to the CPU, GPU stays on\n");
	assert(map_slot(0) == 0);
	s = source(lw, lh, 4520, 1);
	assert(andromeda_gpu_present_frame(0, &s) != 0 && andromeda_gpu_present_ready());

	printf("--- TLB sync that never completes: surface stays wired\n");
	emu.smmu_stuck = true;
	assert(andromeda_gpu_present_unmap(0) != 0 && !andromeda_gpu_present_ready());

	printf("--- panel memory XNU manages: never given to the GPU\n");
	reset(pw, ph, rb, lw, lh, bpr);
	andromeda_gpu_present_attach(&gpu, ANDR_POOL_SIZE);
	g = geom(lw, lh);
	g.panel_pa = RAM_PA;
	assert(andromeda_gpu_present_setup(&g) != 0 && !andromeda_gpu_present_ready());
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	scenario("andromeda (0094: 1125x2436 -> 1080x2338 at y 1)", 1080, 2340, 4320, 1125, 2436, 4608, 10);
	scenario("even fit offset (1080x2342 panel)", 1080, 2342, 4320, 1125, 2436, 4608, 2);
	scenario("ios-sweet panel 1080x2400", 1080, 2400, 4320, 1125, 2436, 4608, 2);
	scenario("odd period: 1200 -> 1080 (9 rows <- 10, two classes per residue)", 1080, 2340, 4320, 1200,
	    2436, 4800, 2);
	scenario("64-byte panel stride 4352", 1080, 2340, 4352, 1125, 2436, 4608, 2);
	/* Small display, many frames: periodic checks and the timing line; a 392-byte stride puts rows at
	 * 8-byte steps inside their 64-byte lines (offsets of 0, 2, 4 and 6 pixels). */
	arg_every = 100;
	scenario("small 96x200 panel, stride 392, 305 frames", 96, 200, 392, 100, 208, 448, 305);
	arg_every = 1000;
	fallbacks();
	printf("test_andromeda_gpu_present: all passed (model of the CP and 2D engine; the A640 itself is not exercised)\n");
	return 0;
}
