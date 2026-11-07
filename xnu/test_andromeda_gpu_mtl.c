/*
 * Host test of andromeda_gpu_mtl.c (phases F5/F6, patch 0200, andrmtl=N):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined xnu/test_andromeda_gpu_mtl.c -o /tmp/mtl-test
 *   /tmp/mtl-test
 *
 * The kernel environment is replaced by a small model: andromeda_gpu.c's
 * 16 MiB pool, the BO pool, "XNU-managed" RAM with scattered 16K task pages,
 * the GPU page tables walked like the SMMU's stage 1 (4K granule, 39-bit VA,
 * 2 MB blocks for the low pool mapping, AP bits enforced), the TLB registers,
 * and a command processor that runs the ring: CP_EVENT_WRITE (CCU
 * invalidates, CACHE_FLUSH_TS writing the fence), CP_INDIRECT_BUFFER, and in
 * the IBs CP_MEM_WRITE and CP_NOP. The OS layer (descriptors, pinning,
 * zombies, publication) is a fake with reference counts.
 *
 * What it shows: the boot-args and the "off by default" behaviour, the BO
 * pool checks, publication in either order, the ABI methods with their
 * checks (sizes, windows, handles, permissions, IB ranges), the PTEs each
 * mapping leaves (read-only where it must), the ring wrapper and its fences
 * (also across the ring's end), flame's v2test sequence, imports (writable,
 * read-only, refused), busy ring, hang, thermal and TLB-sync stops, zombie
 * memory and the release of everything at close. The record layouts are
 * checked against NativeAdrenoABI.h by scripts/test-mtl-abi.py and at compile
 * time by SweetAccelerator.cpp.
 * What it cannot show: IOKit, the real SMMU and A640, Turnip. That is for the
 * phone (v2test, vktest: docs/research/gpu.md F5).
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef unsigned int boolean_t;
#define VM_WIMG_WCOMB 6
#define ARM64_BOARD_CONFIG_SWEET 1
#define ANDR_MTL_HOST_TEST 1

uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs);
void delay(int usec);
int PE_parse_boot_argn(const char *name, void *arg, int size);
boolean_t pmap_valid_address(uint64_t pa);
vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
void andr_mtl_host_lock(void);
void andr_mtl_host_unlock(void);

#include "andromeda_gpu_mtl.c"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ---- time, boot-args ---- */

static uint64_t now_ns;
uint64_t mach_absolute_time(void) { return now_ns += 1000; }
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns) { *ns = abs; }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
void delay(int usec) { now_ns += (uint64_t)usec * 1000; }

static struct {
	bool mtl, pool, size, hot;
	uint32_t mtl_v, hot_v;
	uint64_t pool_v, size_v;
} args;

int
PE_parse_boot_argn(const char *name, void *arg, int size)
{
	if (!strcmp(name, "andrmtl") && args.mtl) {
		CHECK(size == 4);
		memcpy(arg, &args.mtl_v, 4);
		return 1;
	}
	if (!strcmp(name, "andrgpuhot") && args.hot) {
		CHECK(size == 4);
		memcpy(arg, &args.hot_v, 4);
		return 1;
	}
	if (!strcmp(name, "andrmtlpool") && args.pool) {
		CHECK(size == 8);
		memcpy(arg, &args.pool_v, 8);
		return 1;
	}
	if (!strcmp(name, "andrmtlpoolsize") && args.size) {
		CHECK(size == 8);
		memcpy(arg, &args.size_v, 8);
		return 1;
	}
	return 0;
}

/* ---- memory model ---- */

#define GPOOL_PA	0x1ff000000ull			/* andrgpupool default */
#define MANAGED_LO	0x181004000ull			/* the stock 1.5 GiB kernel window */
#define MANAGED_HI	0x1e1004000ull
#define TASK_PA		0x190000000ull			/* task pages: managed RAM */
#define TASK_PAGES	1024u
#define PG		0x4000u
#define MMIO_LO		0x02c00000ull
#define MMIO_WORDS	0x40000u			/* 0x2c00000..0x2d00000 */

static uint8_t *gpool, *mpool, *task_ram;
static uint64_t mpool_pa, mpool_size;
static uint32_t mmio[MMIO_WORDS];
static int lock_depth;

static struct {
	uint32_t rptr;
	bool stall, hang, tlb_stuck, gx, temp_valid;
	int temp_mc;
	uint64_t tlbi, faults, ro_faults, events, ibs, mem_writes, fences;
} emu;

static struct andr_gpu gpu;

boolean_t
pmap_valid_address(uint64_t pa)
{
	return pa >= MANAGED_LO && pa < MANAGED_HI;
}

vm_offset_t
ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags)
{
	CHECK(flags == VM_WIMG_WCOMB);
	CHECK(mpool == NULL);
	mpool = calloc(1, size);
	CHECK(mpool != NULL);
	mpool_pa = phys_addr;
	mpool_size = size;
	return (vm_offset_t)mpool;
}

void
andr_mtl_host_lock(void)
{
	CHECK(lock_depth == 0);		/* never taken twice: lck_mtx is not recursive */
	lock_depth++;
}

void
andr_mtl_host_unlock(void)
{
	CHECK(lock_depth == 1);
	lock_depth--;
}

static uint8_t *
host(uint64_t pa, uint64_t len)
{
	if (pa >= GPOOL_PA && pa + len <= GPOOL_PA + ANDR_POOL_SIZE) {
		return gpool + (pa - GPOOL_PA);
	}
	if (mpool && pa >= mpool_pa && pa + len <= mpool_pa + mpool_size) {
		return mpool + (pa - mpool_pa);
	}
	if (pa >= TASK_PA && pa + len <= TASK_PA + (uint64_t)TASK_PAGES * PG) {
		return task_ram + (pa - TASK_PA);
	}
	return NULL;
}

static uint64_t
rd64(uint64_t pa)
{
	uint8_t *p = host(pa, 8);
	CHECK(p != NULL);
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* andromeda_gpu.h environment: pool = memory, everything else MMIO words. */
static void cp_run(void);

uint32_t
andr_rd(uint64_t pa)
{
	if (pa >= GPOOL_PA && pa + 4 <= GPOOL_PA + ANDR_POOL_SIZE) {
		uint32_t v;
		memcpy(&v, gpool + (pa - GPOOL_PA), 4);
		return v;
	}
	CHECK(pa >= MMIO_LO && pa < MMIO_LO + 4ull * MMIO_WORDS && !(pa & 3));
	if (pa == ANDR_SMMU_CB(1) + MT_CB_TLBSTATUS) {
		return emu.tlb_stuck ? 1u : 0u;
	}
	if (pa == ANDR_GREG(MT_CP_RB_RPTR)) {
		return emu.rptr;
	}
	if (pa == ANDR_GREG(MT_ALWAYS_ON_LO)) {
		return (uint32_t)(now_ns / 52);
	}
	if (pa == ANDR_GREG(MT_ALWAYS_ON_HI)) {
		return (uint32_t)((now_ns / 52) >> 32);
	}
	return mmio[(pa - MMIO_LO) / 4];
}

void
andr_wr(uint64_t pa, uint32_t v)
{
	if (pa >= GPOOL_PA && pa + 4 <= GPOOL_PA + ANDR_POOL_SIZE) {
		memcpy(gpool + (pa - GPOOL_PA), &v, 4);
		return;
	}
	CHECK(pa >= MMIO_LO && pa < MMIO_LO + 4ull * MMIO_WORDS && !(pa & 3));
	mmio[(pa - MMIO_LO) / 4] = v;
	if (pa == ANDR_SMMU_CB(1) + MT_CB_TLBIALL) {
		emu.tlbi++;
	}
	if (pa == ANDR_GREG(MT_CP_RB_WPTR)) {
		CHECK(v < ANDR_RING_DW);
		cp_run();
	}
}

int
andr_write(uint64_t pa, const void *buf, size_t len)
{
	/* the BO pool is written through its own mapping, never through here */
	CHECK(!(mpool && pa >= mpool_pa && pa < mpool_pa + mpool_size));
	if (pa >= GPOOL_PA && pa + len <= GPOOL_PA + ANDR_POOL_SIZE) {
		memcpy(gpool + (pa - GPOOL_PA), buf, len);
		return 0;
	}
	const uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		andr_wr(pa + 4 * i, w[i]);
	}
	return 0;
}

int
andr_read(uint64_t pa, void *buf, size_t len)
{
	CHECK(!(mpool && pa >= mpool_pa && pa < mpool_pa + mpool_size));
	CHECK(pa >= GPOOL_PA && pa + len <= GPOOL_PA + ANDR_POOL_SIZE);
	memcpy(buf, gpool + (pa - GPOOL_PA), len);
	return 0;
}

void andr_delay_ms(unsigned ms) { now_ns += (uint64_t)ms * 1000000; }
void andr_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void andr_smc(uint64_t x[8]) { (void)x; CHECK(0); }
const uint8_t *andr_firmware(enum andr_fw fw, uint32_t *len) { (void)fw; (void)len; CHECK(0); return NULL; }
bool andr_gx_up(void) { return emu.gx; }

bool
andr_gpu_temperature(int *max_mc)
{
	*max_mc = emu.temp_mc;
	return emu.temp_valid;
}

/* ---- SMMU stage 1 and the command processor ---- */

#define ADDR(e) ((e) & 0x000000fffffff000ull)

/* VA -> PA as the SMMU walks it; 0 = fault (counted). */
static uint64_t
walk(uint64_t va, bool write)
{
	CHECK(va < (1ull << 39));
	uint64_t e = rd64(GPOOL_PA + ANDR_OFF_GT_L1 + 8 * ((va >> 30) & 0x1ff));
	if ((e & 3) != 3) {
		emu.faults++;
		return 0;
	}
	e = rd64(ADDR(e) + 8 * ((va >> 21) & 0x1ff));
	uint64_t pa, ap;
	if ((e & 3) == 1) {		/* 2 MB block (the low pool mapping) */
		pa = (e & 0x000000ffffe00000ull) + (va & 0x1fffff);
		ap = (e >> 6) & 3;
	} else if ((e & 3) == 3) {
		e = rd64(ADDR(e) + 8 * ((va >> 12) & 0x1ff));
		if ((e & 3) != 3) {
			emu.faults++;
			return 0;
		}
		CHECK((e & (1ull << 10)) && ((e >> 8) & 3) == 3 && ((e >> 2) & 7) == 0);	/* AF, inner shareable, NC */
		pa = ADDR(e) + (va & 0xfff);
		ap = (e >> 6) & 3;
	} else {
		emu.faults++;
		return 0;
	}
	CHECK(ap == 1 || ap == 3);
	if (write && ap != 1) {
		emu.ro_faults++;
		emu.faults++;
		return 0;
	}
	return pa;
}

static bool
gpu_rd32(uint64_t va, uint32_t *v)
{
	uint64_t pa = walk(va, false);
	uint8_t *p = pa ? host(pa, 4) : NULL;
	if (!p) {
		return false;
	}
	memcpy(v, p, 4);
	return true;
}

static void
gpu_wr32(uint64_t va, uint32_t v)
{
	uint64_t pa = walk(va, true);
	uint8_t *p = pa ? host(pa, 4) : NULL;
	if (p) {
		memcpy(p, &v, 4);
	}
}

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	    (v >> 28)))) & 1u;
}

static void
check_pkt7(uint32_t h, uint32_t *op, uint32_t *cnt)
{
	CHECK((h >> 28) == 7);
	*cnt = h & 0x3fffu;
	*op = (h >> 16) & 0x7fu;
	CHECK(((h >> 15) & 1u) == parity(*cnt) && ((h >> 23) & 1u) == parity(*op));
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (parity(cnt) << 15) | ((op & 0x7fu) << 16) | (parity(op) << 23);
}

static void
run_ib(uint64_t va, uint32_t dwords)
{
	emu.ibs++;
	for (uint32_t i = 0; i < dwords;) {
		uint32_t h = 0, op, cnt;
		CHECK(gpu_rd32(va + 4ull * i, &h));
		check_pkt7(h, &op, &cnt);
		CHECK(i + 1 + cnt <= dwords);
		if (op == 0x3d) {		/* CP_MEM_WRITE */
			uint32_t lo = 0, hi = 0;
			CHECK(cnt >= 3 && gpu_rd32(va + 4ull * (i + 1), &lo) && gpu_rd32(va + 4ull * (i + 2), &hi));
			for (uint32_t k = 2; k < cnt; k++) {
				uint32_t d = 0;
				CHECK(gpu_rd32(va + 4ull * (i + 1 + k), &d));
				gpu_wr32((((uint64_t)hi << 32) | lo) + 4ull * (k - 2), d);
				emu.mem_writes++;
			}
		} else {
			CHECK(op == 0x10);	/* CP_NOP */
		}
		i += 1 + cnt;
	}
}

static uint32_t *
ring(void)
{
	return (uint32_t *)(void *)(gpool + ANDR_OFF_RING);
}

static void
cp_run(void)
{
	uint32_t wptr = mmio[(ANDR_GREG(MT_CP_RB_WPTR) - MMIO_LO) / 4];
	while (!emu.stall && emu.rptr != wptr) {
		uint32_t *rb = ring(), r = emu.rptr, op, cnt;
		check_pkt7(rb[r], &op, &cnt);
#define RB(k) rb[(r + 1 + (k)) % ANDR_RING_DW]
		if (op == MT_CP_EVENT_WRITE) {
			emu.events++;
			if (cnt == 1) {
				CHECK(RB(0) == MT_EV_CCU_INVALIDATE_DEPTH || RB(0) == MT_EV_CCU_INVALIDATE_COLOR);
			} else {
				CHECK(cnt == 4 && RB(0) == MT_EV_CACHE_FLUSH_TS);
				gpu_wr32(((uint64_t)RB(2) << 32) | RB(1), RB(3));
				emu.fences++;
			}
		} else {
			CHECK(op == MT_CP_INDIRECT_BUFFER && cnt == 3);
			if (emu.hang) {
				return;		/* the CP sits in this IB: rptr stays */
			}
			run_ib(((uint64_t)RB(1) << 32) | RB(0), RB(2));
		}
#undef RB
		emu.rptr = (r + 1 + cnt) % ANDR_RING_DW;
	}
}

/* ---- fake OS layer (SweetAccelerator.cpp) ---- */

enum { MD_PHYS = 1, MD_IMPORT };

struct fmd {
	int kind, refs;
	uint64_t pa, len, addr;
	const struct region *rg;
};

struct region {
	uint64_t addr;
	uint32_t npages;
	uint64_t pa[64];
	bool writable;
};

#define FAKE_TASK ((void *)0x7a5c)
static struct region regions[8];
static unsigned nregions;
static int live_phys, live_import, publishes;
static unsigned published_level;
static struct fmd *zombies[64];
static unsigned nzombies;
static struct fmd *live_md[1024];	/* every descriptor not yet released (kept ones included) */

static void
md_track(struct fmd *m, bool add)
{
	for (unsigned i = 0; i < 1024; i++) {
		if (add ? live_md[i] == NULL : live_md[i] == m) {
			live_md[i] = add ? m : NULL;
			return;
		}
	}
	CHECK(0);
}

void *
andr_mtl_os_phys(uint64_t pa, uint64_t bytes)
{
	CHECK(lock_depth == 0 && mt_in_pool(pa, bytes));
	struct fmd *m = calloc(1, sizeof(*m));
	CHECK(m != NULL);
	m->kind = MD_PHYS;
	m->refs = 1;
	m->pa = pa;
	m->len = bytes;
	live_phys++;
	md_track(m, true);
	return m;
}

void *
andr_mtl_os_import(void *task, uint64_t address, uint64_t bytes, bool *writable)
{
	CHECK(lock_depth == 0 && task == FAKE_TASK);
	for (unsigned i = 0; i < nregions; i++) {
		const struct region *rg = &regions[i];
		if (address >= rg->addr && address + bytes <= rg->addr + (uint64_t)rg->npages * PG) {
			struct fmd *m = calloc(1, sizeof(*m));
			CHECK(m != NULL);
			m->kind = MD_IMPORT;
			m->refs = 1;
			m->addr = address;
			m->len = bytes;
			m->rg = rg;
			*writable = rg->writable;
			live_import++;
			md_track(m, true);
			return m;
		}
	}
	return NULL;
}

uint64_t
andr_mtl_os_import_pa(void *md, uint64_t offset)
{
	const struct fmd *m = md;
	CHECK(m->kind == MD_IMPORT && offset < m->len);
	uint64_t va = m->addr + offset;
	return m->rg->pa[(va - m->rg->addr) / PG] + (va & (PG - 1));
}

void
andr_mtl_os_import_release(void *md)
{
	struct fmd *m = md;
	CHECK(lock_depth == 0 && m->kind == MD_IMPORT && m->refs == 1);
	live_import--;
	md_track(m, false);
	free(m);
}

void
andr_mtl_os_phys_release(void *md, uint64_t pa, uint64_t bytes)
{
	struct fmd *m = md;
	CHECK(lock_depth == 0 && m->kind == MD_PHYS && m->pa == pa && m->len == bytes);
	if (m->refs <= 1) {
		live_phys--;
		md_track(m, false);
		free(m);
		andr_mtl_pool_free(pa, bytes);
		return;
	}
	CHECK(nzombies < 64);
	zombies[nzombies++] = m;
}

void
andr_mtl_os_sweep(void)
{
	CHECK(lock_depth == 0);
	for (unsigned i = 0; i < nzombies;) {
		struct fmd *m = zombies[i];
		if (m->refs <= 1) {
			uint64_t pa = m->pa, len = m->len;
			live_phys--;
			md_track(m, false);
			free(m);
			zombies[i] = zombies[--nzombies];
			andr_mtl_pool_free(pa, len);
			continue;
		}
		i++;
	}
}

void
andr_mtl_os_retain(void *md)
{
	((struct fmd *)md)->refs++;
}

struct andr_mtl_bo *
andr_mtl_os_bo_table(uint32_t n)
{
	return calloc(n, sizeof(struct andr_mtl_bo));
}

void
andr_mtl_os_bo_table_free(struct andr_mtl_bo *t, uint32_t n)
{
	(void)n;
	free(t);
}

void
andr_mtl_os_publish(unsigned level)
{
	publishes++;
	published_level = level;
}

/* ---- harness ---- */

static void
reset(void)
{
	CHECK(lock_depth == 0);
	free(mpool);
	mpool = NULL;
	mpool_pa = mpool_size = 0;
	memset(gpool, 0, ANDR_POOL_SIZE);
	memset(task_ram, 0, (size_t)TASK_PAGES * PG);
	memset(mmio, 0, sizeof(mmio));
	memset(&emu, 0, sizeof(emu));
	memset(&args, 0, sizeof(args));
	emu.gx = emu.temp_valid = true;
	emu.temp_mc = 40000;
	nregions = 0;
	nzombies = 0;
	for (unsigned i = 0; i < 1024; i++) {	/* what the last "boot" kept for good */
		free(live_md[i]);
		live_md[i] = NULL;
	}
	live_phys = live_import = publishes = 0;
	published_level = 0;
	/* the module's statics: a new boot */
	mt_state = MT_UNPARSED;
	mt_level = 0;
	mt_hot = 80;
	mt_pool_arg = ANDR_MTL_POOL_DEFAULT;
	mt_pool_size_arg = ANDR_MTL_POOL_SIZE_DEFAULT;
	mt_pub = mt_logged = 0;
	mt_g = NULL;
	mt_gpool_lo = mt_gpool_hi = 0;
	mt_kva = 0;
	mt_pa = mt_size = 0;
	mt_pt_next = mt_pt_max = 0;
	mt_nfree = 0;
	mt_used = mt_peak = 0;
	mt_win_used = 0;
	mt_progress_fence = 0;
	mt_progress_abs = mt_check_abs = mt_thermal_abs = 0;
	mt_submits = 0;
	/* andromeda_gpu.c after step 4: pool, GPU bank 1, ZAP done, the low 2 MB block mapping (map_gpu_low) */
	memset(&gpu, 0, sizeof(gpu));
	gpu.pool_pa = GPOOL_PA;
	gpu.gmu_cb = 0;
	gpu.gpu_cb = 1;
	gpu.zap_authenticated = true;
	gpu.wptr = 40;			/* the bring-up left some packets */
	emu.rptr = 40;
	mmio[(ANDR_GREG(MT_CP_RB_WPTR) - MMIO_LO) / 4] = 40;
	uint64_t l1 = GPOOL_PA + ANDR_OFF_GT_L1, l2 = GPOOL_PA + ANDR_OFF_GT_L2, d = l2 | 3;
	memcpy(host(l1, 8), &d, 8);
	for (uint64_t o = 0; o < ANDR_POOL_SIZE; o += 0x200000) {
		d = (GPOOL_PA + o) | (1u << 10) | (3u << 8) | (1u << 6) | 1u;
		memcpy(host(l2 + 8 * (((ANDR_GVA_POOL + o) >> 21) & 0x1ff), 8), &d, 8);
	}
}

static void
boot(uint32_t level, uint64_t pool, uint64_t size)
{
	reset();
	if (level != 0xffffffffu) {
		args.mtl = true;
		args.mtl_v = level;
	}
	if (pool) {
		args.pool = true;
		args.pool_v = pool;
	}
	if (size) {
		args.size = true;
		args.size_v = size;
	}
}

#define SMALL_POOL	0x4000000ull	/* 64 MiB: 8 MiB of tables + 56 MiB of BOs */

/* andrmtl=level with a small pool, service started, GPU attached. */
static void
ready(uint32_t level)
{
	boot(level, 0, SMALL_POOL);
	andromeda_mtl_service_started();
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
	CHECK(andromeda_mtl_usable() && publishes == 1 && published_level == level);
}

static int
call(struct andr_mtl_client *c, uint32_t method, const void *in, uint32_t isz, void *out, uint32_t osz)
{
	uint8_t buf[8 + sizeof(andr_mtl_submit_t)];
	uint32_t hdr[2] = { ANDR_MTL_TUNNEL, method };
	CHECK(isz <= sizeof(andr_mtl_submit_t));
	memcpy(buf, hdr, 8);
	if (isz) {
		memcpy(buf + 8, in, isz);
	}
	uint32_t o = osz;
	int rc = andr_mtl_external(c, 0, buf, 8 + isz, osz ? out : NULL, osz ? &o : NULL);
	CHECK(lock_depth == 0);
	CHECK(rc != ANDR_IOR_SUCCESS || !osz || o == osz);
	return rc;
}

static int
alloc_bo(struct andr_mtl_client *c, uint64_t bytes, uint64_t *h)
{
	andr_mtl_bo_alloc_t a = { bytes, 12, 0 };
	andr_mtl_handle_t r = { 0 };
	int rc = call(c, ANDR_MTL_M_BO_ALLOC, &a, sizeof(a), &r, sizeof(r));
	*h = r.handle;
	return rc;
}

static int
map_bo(struct andr_mtl_client *c, uint64_t h, uint64_t iova, uint32_t perm)
{
	andr_mtl_bo_map_t m = { h, iova, perm, 0, 2, 0 };
	andr_mtl_bo_map_reply_t r = { 0 };
	int rc = call(c, ANDR_MTL_M_BO_MAP, &m, sizeof(m), &r, sizeof(r));
	CHECK(rc != ANDR_IOR_SUCCESS || r.iova == iova);
	return rc;
}

static int
free_bo(struct andr_mtl_client *c, uint64_t h)
{
	andr_mtl_handle_t f = { h };
	return call(c, ANDR_MTL_M_BO_FREE, &f, sizeof(f), NULL, 0);
}

/* CPU view of a BO through the descriptor clientMemoryForType hands out. */
static uint32_t *
cpu_view(struct andr_mtl_client *c, uint64_t h, uint64_t *len)
{
	andr_mtl_handle_t q = { h };
	andr_mtl_cpu_map_reply_t r = { 0, 0, 0 };
	CHECK(call(c, ANDR_MTL_M_BO_CPU_MAP, &q, sizeof(q), &r, sizeof(r)) == ANDR_IOR_SUCCESS && r.token == h);
	struct fmd *m = andr_mtl_cpu_memory(c, r.token);
	CHECK(m != NULL && m->kind == MD_PHYS && m->len == r.bytes);
	m->refs--;			/* the test does not keep a mapping */
	*len = r.bytes;
	return (uint32_t *)(void *)host(m->pa, m->len);
}

static int
submit(struct andr_mtl_client *c, const andr_mtl_submit_buffer_t *b, uint32_t n, uint64_t *fence)
{
	andr_mtl_submit_t s;
	memset(&s, 0, sizeof(s));
	s.buffer_count = n;
	memcpy(s.buffers, b, sizeof(*b) * n);
	andr_mtl_u64_t r = { 0 };
	int rc = call(c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r));
	*fence = r.value;
	return rc;
}

static int
wait_fence(struct andr_mtl_client *c, uint64_t fence, uint64_t timeout_ns)
{
	andr_mtl_wait_fence_t w = { fence, timeout_ns };
	return call(c, ANDR_MTL_M_WAIT_FENCE, &w, sizeof(w), NULL, 0);
}

/* The PTE the SMMU would use for va (0 if none). */
static uint64_t
pte(uint64_t va)
{
	uint64_t e = rd64(GPOOL_PA + ANDR_OFF_GT_L1 + 8 * ((va >> 30) & 0x1ff));
	if ((e & 3) != 3) {
		return 0;
	}
	e = rd64(ADDR(e) + 8 * ((va >> 21) & 0x1ff));
	if ((e & 3) != 3) {
		return 0;
	}
	return rd64(ADDR(e) + 8 * ((va >> 12) & 0x1ff));
}

static uint64_t
free_bytes(void)
{
	uint64_t t = 0;
	for (uint32_t i = 0; i < mt_nfree; i++) {
		t += mt_free[i].size;
	}
	return t;
}

/* An IB in BO ib that CP_MEM_WRITEs 4 words (0xcafe0001..4) to dst (flame v2test gpu_write). */
static uint64_t
ib_mem_write(struct andr_mtl_client *c, uint64_t ib, uint64_t ib_iova, uint64_t dst)
{
	uint64_t len;
	uint32_t *w = cpu_view(c, ib, &len);
	w[0] = pkt7(0x3d, 6);
	w[1] = (uint32_t)dst;
	w[2] = (uint32_t)(dst >> 32);
	w[3] = 0xcafe0001;
	w[4] = 0xcafe0002;
	w[5] = 0xcafe0003;
	w[6] = 0xcafe0004;
	andr_mtl_submit_buffer_t b = { ib, ib_iova, 7 * 4 };
	uint64_t fence = 0;
	CHECK(submit(c, &b, 1, &fence) == ANDR_IOR_SUCCESS && fence != 0);
	return fence;
}

/* ---- tests ---- */

static void
test_layout(void)
{
	CHECK(sizeof(andr_mtl_submit_t) == 416 && offsetof(andr_mtl_submit_t, buffers[15].bytes) == 32 + 15 * 24 + 16);
	CHECK(sizeof(andr_mtl_device_info_t) == 64 && offsetof(andr_mtl_device_info_t, highest_bank_bit) == 40);
	CHECK(offsetof(andr_mtl_metadata_t, text) == 16 && offsetof(andr_mtl_cpu_map_reply_t, bytes) == 8);
	CHECK(ANDR_MTL_WIN_BASE >> 30 == 64 && ((ANDR_MTL_WIN_BASE + ANDR_MTL_WIN_COUNT * ANDR_MTL_WIN_SIZE - 1) >> 30) < 512);
	/* the windows stay out of the L1 entries andromeda_gpu_core.c (0) and the F4 slots (4) use */
	CHECK((ANDR_MTL_WIN_BASE >> 30) > ((0x100000000ull + 4 * 0x1000000ull) >> 30));
	printf("PASS layout: records as NativeAdrenoABI.h, windows 64..319 GB of the 39-bit space\n");
}

static void
test_off(void)
{
	boot(0xffffffffu, 0, 0);		/* andrmtl absent */
	CHECK(andromeda_mtl_level() == 0);
	andromeda_mtl_service_started();
	CHECK(!andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
	CHECK(publishes == 0 && mpool == NULL && !andromeda_mtl_usable());
	CHECK(emu.tlbi == 0 && gpu.wptr == 40 && emu.events == 0);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_NOT_READY && !c.open);

	boot(3, 0, 0);				/* not a level */
	CHECK(andromeda_mtl_level() == 0 && !andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && mpool == NULL);
	boot(0, 0, 0);
	CHECK(andromeda_mtl_level() == 0 && !andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
	printf("PASS off by default: no pool, nothing published, GPU untouched, open refused\n");
}

static void
test_pool_checks(void)
{
	static const struct {
		uint64_t pool, size;
		const char *what;
	} bad[] = {
		{ 0x1e2100000ull, SMALL_POOL, "not 2 MiB aligned" },
		{ 0x1e2000000ull, 0x1000000ull, "smaller than 32 MiB" },
		{ 0x1e2000000ull, 0x2002000ull, "not a 16 KiB multiple" },
		{ 0xa0000000ull, SMALL_POOL, "below DRAM" },
		{ 0x1fe000000ull, SMALL_POOL, "past the end of DRAM" },
		{ 0xc0000000ull, SMALL_POOL, "over MPSS_EFS" },
		{ 0x17e000000ull, SMALL_POOL, "over PStore" },
		{ 0x1fe000000ull, 0x2000000ull, "over the GPU pool" },
		{ 0x1e0000000ull, SMALL_POOL, "XNU-managed pages" },
		{ 0x180000000ull, SMALL_POOL, "XNU-managed pages at the top" },
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		boot(2, bad[i].pool, bad[i].size);
		andromeda_mtl_service_started();
		CHECK(!andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
		CHECK(mpool == NULL && publishes == 0 && !andromeda_mtl_usable());
		CHECK(!andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));	/* not retried */
	}
	/* the defaults: 464 MiB below andrgpupool, above the stock window */
	boot(2, 0, 0);
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
	CHECK(mpool_pa == ANDR_MTL_POOL_DEFAULT && mpool_size == ANDR_MTL_POOL_SIZE_DEFAULT);
	CHECK(mt_nfree == 1 && free_bytes() == ANDR_MTL_POOL_SIZE_DEFAULT - ANDR_MTL_PT_BYTES && mt_pt_max == 2048);
	CHECK(publishes == 0);			/* no service yet */
	/* a GPU that did not reach the CP state is refused */
	boot(2, 0, SMALL_POOL);
	gpu.zap_authenticated = false;
	CHECK(!andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && mpool == NULL);
	boot(2, 0, SMALL_POOL);
	emu.gx = false;
	CHECK(!andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && mpool == NULL);
	printf("PASS BO pool: %u bad placements refused, defaults accepted, GPU state checked\n",
	    (unsigned)(sizeof(bad) / sizeof(bad[0])));
}

static void
test_publish_order(void)
{
	boot(2, 0, SMALL_POOL);
	andromeda_mtl_service_started();
	CHECK(publishes == 0);
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && publishes == 1 && published_level == 2);
	andromeda_mtl_service_started();
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && publishes == 1);
	boot(1, 0, SMALL_POOL);
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE) && publishes == 0);
	andromeda_mtl_service_started();
	CHECK(publishes == 1 && published_level == 1);
	/* the fence word starts at the bring-up's counter */
	CHECK(andr_rd(GPOOL_PA + ANDR_OFF_SCRATCH) == gpu.fence);
	printf("PASS publication: once, after both the service and step 4, in either order\n");
}

static void
test_open_windows(void)
{
	ready(2);
	static struct andr_mtl_client cl[ANDR_MTL_WIN_COUNT + 1];
	CHECK(andr_mtl_open(&cl[0], FAKE_TASK, 0, "x") == ANDR_IOR_UNSUPPORTED);
	CHECK(andr_mtl_open(&cl[0], FAKE_TASK, 0x41470001u, "x") == ANDR_IOR_UNSUPPORTED);
	for (unsigned i = 0; i < ANDR_MTL_WIN_COUNT; i++) {
		CHECK(andr_mtl_open(&cl[i], FAKE_TASK, i & 1 ? ANDR_MTL_OPEN_DIRECT : ANDR_MTL_OPEN_TYPE,
		    "a-very-long-process-name-to-cut") == ANDR_IOR_SUCCESS);
		CHECK(cl[i].win_base == ANDR_MTL_WIN_BASE + i * ANDR_MTL_WIN_SIZE && cl[i].name[31] == 0);
	}
	CHECK(andr_mtl_open(&cl[ANDR_MTL_WIN_COUNT], FAKE_TASK, ANDR_MTL_OPEN_TYPE, "late") == ANDR_IOR_NO_RESOURCES);
	andr_mtl_close(&cl[5]);
	andr_mtl_close(&cl[5]);			/* twice: no effect */
	CHECK(andr_mtl_open(&cl[ANDR_MTL_WIN_COUNT], FAKE_TASK, ANDR_MTL_OPEN_TYPE, "late") == ANDR_IOR_SUCCESS);
	CHECK(cl[ANDR_MTL_WIN_COUNT].win_base == ANDR_MTL_WIN_BASE + 5 * ANDR_MTL_WIN_SIZE);
	for (unsigned i = 0; i <= ANDR_MTL_WIN_COUNT; i++) {
		andr_mtl_close(&cl[i]);
	}
	CHECK(mt_win_used == 0);
	printf("PASS connections: open types 0x4147 and 2 only, %u windows, reused after close\n", ANDR_MTL_WIN_COUNT);
}

static void
test_query(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	/* the tunnel probe Turnip uses to find an admitted selector */
	uint32_t probe[2] = { ANDR_MTL_TUNNEL, ANDR_MTL_PROBE }, osz = 0;
	CHECK(andr_mtl_external(&c, 77, probe, 8, NULL, NULL) == ANDR_IOR_SUCCESS);
	andr_mtl_params_t p;
	CHECK(call(&c, ANDR_MTL_M_QUERY, NULL, 0, &p, sizeof(p)) == ANDR_IOR_SUCCESS);
	CHECK(p.abi_version == 1 && p.input_bits == 39 && p.output_bits == 36 && p.flags == ANDR_MTL_PARAMS_HOST_IMPORT &&
	    p.max_bo_bytes == (1ull << 28));
	andr_mtl_device_info_t d, d2;
	CHECK(call(&c, ANDR_MTL_M_GET_DEVICE_INFO, NULL, 0, &d, sizeof(d)) == ANDR_IOR_SUCCESS);
	CHECK(d.chip_id == 0x06040001u && d.gmem_bytes == 0x100000u && d.gmem_base == 0x100000u &&
	    d.uche_trap_base == 0x1fffffffff000ull && d.va_start == c.win_base && d.va_size == 0x100000000ull &&
	    d.highest_bank_bit == 15 && d.ubwc_swizzle == 6 && d.macrotile_mode == 0 && d.ubwc_version == 3 &&
	    d.flags == ANDR_MTL_DEVICE_CLIENT_IOVA && d.reserved == 0);
	/* selector = method (direct form) gives the same */
	osz = sizeof(d2);
	CHECK(andr_mtl_external(&c, ANDR_MTL_M_GET_DEVICE_INFO, NULL, 0, &d2, &osz) == ANDR_IOR_SUCCESS &&
	    osz == sizeof(d2) && !memcmp(&d, &d2, sizeof(d)));
	/* too small an output, unknown methods, queues, metadata, status, timestamp */
	osz = sizeof(d) - 1;
	CHECK(andr_mtl_external(&c, ANDR_MTL_M_GET_DEVICE_INFO, NULL, 0, &d2, &osz) == ANDR_IOR_BAD_ARGUMENT);
	CHECK(call(&c, 0, NULL, 0, NULL, 0) == ANDR_IOR_UNSUPPORTED && call(&c, 17, NULL, 0, NULL, 0) == ANDR_IOR_UNSUPPORTED);
	andr_mtl_u64_t q;
	CHECK(call(&c, ANDR_MTL_M_CREATE_QUEUE, NULL, 0, &q, sizeof(q)) == ANDR_IOR_SUCCESS && q.value == 1);
	CHECK(call(&c, ANDR_MTL_M_CLOSE_QUEUE, &q, sizeof(q), NULL, 0) == ANDR_IOR_SUCCESS);
	andr_mtl_metadata_t md;
	memset(&md, 0x5a, sizeof(md));
	CHECK(call(&c, ANDR_MTL_M_SET_METADATA, &md, sizeof(md), NULL, 0) == ANDR_IOR_SUCCESS);
	CHECK(call(&c, ANDR_MTL_M_GET_METADATA, &md, sizeof(md), &md, sizeof(md)) == ANDR_IOR_SUCCESS && md.bytes == 0);
	andr_mtl_status_t st;
	CHECK(call(&c, ANDR_MTL_M_GET_STATUS, NULL, 0, &st, sizeof(st)) == ANDR_IOR_SUCCESS && st.faults == 0);
	andr_mtl_u64_t t1, t2;
	CHECK(call(&c, ANDR_MTL_M_GET_TIMESTAMP, NULL, 0, &t1, sizeof(t1)) == ANDR_IOR_SUCCESS);
	now_ns += 1000000;
	CHECK(call(&c, ANDR_MTL_M_GET_TIMESTAMP, NULL, 0, &t2, sizeof(t2)) == ANDR_IOR_SUCCESS && t2.value > t1.value);
	andr_mtl_close(&c);
	CHECK(andr_mtl_external(&c, 77, probe, 8, NULL, NULL) == ANDR_IOR_NOT_READY);
	printf("PASS query: tunnel probe, ABI v1 params, A640 device info as hw_init programs it, both call forms\n");
}

static void
test_alloc_free(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "SpringBoard") == ANDR_IOR_SUCCESS);
	uint64_t h = 0, all = free_bytes();
	andr_mtl_bo_alloc_t bad[] = {
		{ 0, 12, 0 }, { 4096 + 1, 12, 0 }, { (1ull << 28) + 4096, 12, 0 }, { 4096, 11, 0 }, { 4096, 25, 0 },
		{ 4096, 12, 2 },
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		andr_mtl_handle_t r;
		CHECK(call(&c, ANDR_MTL_M_BO_ALLOC, &bad[i], sizeof(bad[i]), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);
	}
	CHECK(call(&c, ANDR_MTL_M_BO_ALLOC, &bad[0], 8, &h, 8) == ANDR_IOR_BAD_ARGUMENT);	/* wrong size */
	/* zeroing: dirty the whole BO area first */
	memset(mpool + ANDR_MTL_PT_BYTES, 0xa5, (size_t)(mpool_size - ANDR_MTL_PT_BYTES));
	uint64_t hs[5], sizes[5] = { 4096, 0x10000, 0x3000, 0x200000, 0x1000000 };
	for (unsigned i = 0; i < 5; i++) {
		CHECK(alloc_bo(&c, sizes[i], &hs[i]) == ANDR_IOR_SUCCESS && hs[i] == i + 1);
		uint64_t len;
		uint32_t *w = cpu_view(&c, hs[i], &len);
		CHECK(len == ((sizes[i] + 0x3fff) & ~0x3fffull));
		for (uint64_t k = 0; k < len / 4; k++) {
			CHECK(w[k] == 0);
		}
	}
	CHECK(c.bo_live == 5 && c.pool_bytes == all - free_bytes() && live_phys == 5);
	/* free in a scattered order: the free list merges back to one range */
	unsigned order[5] = { 1, 3, 0, 4, 2 };
	for (unsigned i = 0; i < 5; i++) {
		CHECK(free_bo(&c, hs[order[i]]) == ANDR_IOR_SUCCESS);
		CHECK(free_bo(&c, hs[order[i]]) == ANDR_IOR_BAD_ARGUMENT);
	}
	CHECK(mt_nfree == 1 && free_bytes() == all && c.pool_bytes == 0 && live_phys == 0 && mt_used == 0);
	/* the table grows past 256 handles; exhaustion is NO_MEMORY, not a crash */
	for (unsigned i = 0; i < 300; i++) {
		CHECK(alloc_bo(&c, 4096, &h) == ANDR_IOR_SUCCESS && h == i + 1);
	}
	CHECK(c.bo_cap == 512);
	uint64_t big = 0;
	CHECK(alloc_bo(&c, 1ull << 28, &big) == ANDR_IOR_NO_MEMORY);	/* 256 MiB > the 56 MiB small pool */
	andr_mtl_close(&c);
	CHECK(mt_nfree == 1 && free_bytes() == all && live_phys == 0);
	printf("PASS BO alloc/free: argument checks, zeroed 16K-rounded memory, merging free list, growing table\n");
}

static void
test_map(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t a = 0, b = 0, w = c.win_base;
	CHECK(alloc_bo(&c, 0x6000, &a) == ANDR_IOR_SUCCESS && alloc_bo(&c, 0x400000, &b) == ANDR_IOR_SUCCESS);
	CHECK(map_bo(&c, a, w - 0x1000, 3) == ANDR_IOR_BAD_ARGUMENT);		/* below the window */
	CHECK(map_bo(&c, a, w + c.win_size - 0x2000, 3) == ANDR_IOR_BAD_ARGUMENT);	/* runs past it */
	CHECK(map_bo(&c, a, w + 0x800, 3) == ANDR_IOR_BAD_ARGUMENT);		/* not 4K aligned */
	CHECK(map_bo(&c, a, w, 0) == ANDR_IOR_BAD_ARGUMENT && map_bo(&c, a, w, 2) == ANDR_IOR_BAD_ARGUMENT &&
	    map_bo(&c, a, w, 7) == ANDR_IOR_BAD_ARGUMENT);
	CHECK(map_bo(&c, 99, w, 3) == ANDR_IOR_BAD_ARGUMENT);
	andr_mtl_bo_map_t m = { a, w, 3, 1, 2, 0 };
	andr_mtl_bo_map_reply_t r;
	CHECK(call(&c, ANDR_MTL_M_BO_MAP, &m, sizeof(m), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);	/* writeback */
	m.writeback = 0;
	m.reserved = 1;
	CHECK(call(&c, ANDR_MTL_M_BO_MAP, &m, sizeof(m), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);
	uint64_t t0 = emu.tlbi;
	CHECK(map_bo(&c, a, w + c.win_size - 0x6000, 3) == ANDR_IOR_SUCCESS);	/* the window's last pages */
	CHECK(map_bo(&c, a, w, 3) == ANDR_IOR_BUSY && emu.tlbi == t0 + 1);
	const struct andr_mtl_bo *bo = &c.bo[a - 1];
	for (uint64_t o = 0; o < 0x6000; o += 0x1000) {
		CHECK(pte(w + c.win_size - 0x6000 + o) == ((bo->pool_pa + o) | MT_PTE_RW));
	}
	/* GPU read-only BOs (TU_BO_ALLOC_GPU_READ_ONLY: permissions 1) get AP=11 */
	CHECK(map_bo(&c, b, w + 0x200000 - 0x1000, 1) == ANDR_IOR_SUCCESS);	/* crosses two L3 tables */
	bo = &c.bo[b - 1];
	for (uint64_t o = 0; o < 0x400000; o += 0x1000) {
		CHECK(pte(w + 0x1ff000 + o) == ((bo->pool_pa + o) | MT_PTE_RO));
	}
	CHECK(pte(w + 0x1fe000) == 0 && pte(w + 0x5ff000) == 0);		/* nothing around it */
	/* GPU walk: in the window, inside the BO only */
	CHECK(walk(w + 0x1ff000, false) == bo->pool_pa && walk(w + 0x1ff000, true) == 0 && emu.ro_faults == 1);
	/* unmapped on free, tables kept */
	uint32_t tables = mt_pt_next;
	CHECK(free_bo(&c, b) == ANDR_IOR_SUCCESS);
	for (uint64_t o = 0; o < 0x400000; o += 0x1000) {
		CHECK(pte(w + 0x1ff000 + o) == 0);
	}
	uint64_t b2;
	CHECK(alloc_bo(&c, 0x400000, &b2) == ANDR_IOR_SUCCESS && map_bo(&c, b2, w + 0x1ff000, 3) == ANDR_IOR_SUCCESS);
	CHECK(mt_pt_next == tables);
	andr_mtl_close(&c);
	CHECK(live_phys == 0);
	/* the page tables run out: refused, nothing left mapped */
	boot(2, 0, SMALL_POOL);
	andromeda_mtl_service_started();
	CHECK(andromeda_gpu_mtl_attach(&gpu, ANDR_POOL_SIZE));
	mt_pt_max = 3;		/* L2 + two L3 */
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "t") == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 0x600000, &a) == ANDR_IOR_SUCCESS);
	CHECK(map_bo(&c, a, c.win_base, 3) == ANDR_IOR_NO_RESOURCES && c.bo[a - 1].iova == 0);
	for (uint64_t o = 0; o < 0x600000; o += 0x1000) {
		CHECK(pte(c.win_base + o) == 0);
	}
	CHECK(map_bo(&c, a, c.win_base, 3) == ANDR_IOR_NO_RESOURCES);	/* not BUSY: it is not mapped */
	andr_mtl_close(&c);
	printf("PASS BO map: window and argument checks, PTEs per 4K page, read-only for GPU-read-only BOs, "
	    "unmapped on free, table exhaustion refused\n");
}

static void
test_v2test(void)
{
	/* flame's phone/turnip/v2test.cpp, step by step */
	ready(1);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "v2test") == ANDR_IOR_SUCCESS);
	andr_mtl_device_info_t info;
	CHECK(call(&c, ANDR_MTL_M_GET_DEVICE_INFO, NULL, 0, &info, sizeof(info)) == ANDR_IOR_SUCCESS);
	uint64_t a, b, len;
	CHECK(alloc_bo(&c, 65536, &a) == ANDR_IOR_SUCCESS && map_bo(&c, a, info.va_start, 3) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 65536, &b) == ANDR_IOR_SUCCESS && map_bo(&c, b, info.va_start + 0x100000, 3) == ANDR_IOR_SUCCESS);
	uint32_t *am = cpu_view(&c, a, &len), *bm = cpu_view(&c, b, &len);
	for (uint32_t i = 0; i < 16384; i++) {
		am[i] = 0x10000000 + i;
	}
	uint64_t f1 = ib_mem_write(&c, a, info.va_start, info.va_start + 0x100000);
	CHECK(wait_fence(&c, f1, 1000000000ull) == ANDR_IOR_SUCCESS);
	CHECK(bm[0] == 0xcafe0001 && bm[3] == 0xcafe0004 && bm[4] == 0);
	CHECK(andr_rd(GPOOL_PA + ANDR_OFF_SCRATCH) == f1 && f1 == 1 && emu.fences == 1 && emu.events == 3);
	/* an import of the caller's own (cacheable) pages: scattered 16K pages of managed RAM */
	regions[0].addr = 0x100c000000ull;
	regions[0].npages = 4;
	regions[0].writable = true;
	for (unsigned i = 0; i < 4; i++) {
		regions[0].pa[i] = TASK_PA + (uint64_t)(7 * i + 3) * PG;
	}
	nregions = 1;
	andr_mtl_bo_import_t im = { regions[0].addr, 65536 };
	andr_mtl_handle_t ir;
	CHECK(call(&c, ANDR_MTL_M_BO_IMPORT, &im, sizeof(im), &ir, sizeof(ir)) == ANDR_IOR_SUCCESS && live_import == 1);
	CHECK(map_bo(&c, ir.handle, info.va_start + 0x200000, 3) == ANDR_IOR_SUCCESS);
	for (unsigned i = 0; i < 16; i++) {
		CHECK(pte(info.va_start + 0x200000 + 0x1000ull * i) ==
		    ((regions[0].pa[i / 4] + 0x1000ull * (i % 4)) | MT_PTE_RW));
	}
	uint64_t f2 = ib_mem_write(&c, a, info.va_start, info.va_start + 0x200000);
	CHECK(wait_fence(&c, f2, 1000000000ull) == ANDR_IOR_SUCCESS && f2 == 2);
	uint32_t *hostp = (uint32_t *)(void *)host(regions[0].pa[0], 16);
	CHECK(hostp[0] == 0xcafe0001 && hostp[3] == 0xcafe0004);
	/* an import has no CPU-map token */
	andr_mtl_cpu_map_reply_t cr;
	CHECK(call(&c, ANDR_MTL_M_BO_CPU_MAP, &ir, sizeof(ir), &cr, sizeof(cr)) == ANDR_IOR_BAD_ARGUMENT);
	CHECK(andr_mtl_cpu_memory(&c, (uint32_t)ir.handle) == NULL && andr_mtl_cpu_memory(&c, 99) == NULL);
	andr_mtl_status_t st;
	CHECK(call(&c, ANDR_MTL_M_GET_STATUS, NULL, 0, &st, sizeof(st)) == ANDR_IOR_SUCCESS && st.faults == 0);
	CHECK(emu.faults == 0);
	andr_mtl_close(&c);
	CHECK(live_import == 0 && live_phys == 0 && pte(info.va_start + 0x200000) == 0 && pte(info.va_start) == 0);
	printf("PASS v2test sequence: CP_MEM_WRITE into a BO and into imported task pages, fences 1 and 2\n");
}

static void
test_submit_checks(void)
{
	ready(2);
	struct andr_mtl_client c, other;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	CHECK(andr_mtl_open(&other, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "SpringBoard") == ANDR_IOR_SUCCESS);
	uint64_t ib, dst, unmapped, oth, w = c.win_base, fence = 0, len;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, w, 1) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 0x4000, &dst) == ANDR_IOR_SUCCESS && map_bo(&c, dst, w + 0x10000, 3) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 0x4000, &unmapped) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&other, 0x4000, &oth) == ANDR_IOR_SUCCESS && map_bo(&other, oth, other.win_base, 3) == ANDR_IOR_SUCCESS);
	andr_mtl_submit_buffer_t badb[] = {
		{ ib, w, 0 }, { ib, w, 6 }, { ib, w + 0x4000 - 4, 8 }, { ib, w - 4, 8 }, { unmapped, w, 8 },
		{ 77, w, 8 }, { dst, w, 8 }, { oth, other.win_base, 8 }, { ib, w + 4, 0x4000 },
	};
	for (unsigned i = 0; i < sizeof(badb) / sizeof(badb[0]); i++) {
		CHECK(submit(&c, &badb[i], 1, &fence) == ANDR_IOR_BAD_ARGUMENT);
	}
	andr_mtl_submit_t s;
	andr_mtl_u64_t r;
	memset(&s, 0, sizeof(s));
	CHECK(call(&c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);	/* 0 IBs */
	s.buffer_count = 17;
	CHECK(call(&c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);
	s.buffer_count = 1;
	s.buffers[0] = (andr_mtl_submit_buffer_t){ ib, w, 4 };
	s.reserved = 1;
	CHECK(call(&c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);
	s.reserved = 0;
	s.wait_fence = 5;			/* never submitted */
	CHECK(call(&c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r)) == ANDR_IOR_BAD_ARGUMENT);
	CHECK(gpu.wptr == 40 && emu.events == 0);	/* nothing reached the ring */
	CHECK(wait_fence(&c, 0, 0) == ANDR_IOR_SUCCESS && wait_fence(&c, 1, 0) == ANDR_IOR_BAD_ARGUMENT &&
	    wait_fence(&c, 1ull << 32, 0) == ANDR_IOR_BAD_ARGUMENT);
	/* 16 IBs in one submit, each a NOP + 4-word write at its own offset */
	uint32_t *iw = cpu_view(&c, ib, &len);
	andr_mtl_submit_buffer_t bs[16];
	for (unsigned i = 0; i < 16; i++) {
		uint32_t *p = iw + 16 * i;
		uint64_t to = w + 0x10000 + 16ull * i;
		p[0] = pkt7(0x10, 1);
		p[1] = 0;
		p[2] = pkt7(0x3d, 3);
		p[3] = (uint32_t)to;
		p[4] = (uint32_t)(to >> 32);
		p[5] = 0xb0b00000u + i;
		bs[i] = (andr_mtl_submit_buffer_t){ ib, w + 64ull * i, 6 * 4 };
	}
	CHECK(submit(&c, bs, 16, &fence) == ANDR_IOR_SUCCESS && fence == 1 && emu.ibs == 16);
	uint32_t *dm = cpu_view(&c, dst, &len);
	for (unsigned i = 0; i < 16; i++) {
		CHECK(dm[4 * i] == 0xb0b00000u + i);
	}
	/* across the ring's end: 300 submits of 1..3 IBs, every fence lands in order */
	uint64_t last = fence;
	for (unsigned k = 0; k < 300; k++) {
		CHECK(submit(&c, bs, 1 + k % 3, &fence) == ANDR_IOR_SUCCESS && fence == last + 1);
		last = fence;
		CHECK(andr_rd(GPOOL_PA + ANDR_OFF_SCRATCH) == fence);
	}
	CHECK(gpu.wptr == emu.rptr && emu.fences == 301 && c.last_fence == last);
	/* with a wait on an earlier fence */
	s = (andr_mtl_submit_t){ 0 };
	s.buffer_count = 1;
	s.buffers[0] = bs[0];
	s.wait_fence = last;
	s.wait_timeout_ns = 1000000;
	CHECK(call(&c, ANDR_MTL_M_SUBMIT, &s, sizeof(s), &r, sizeof(r)) == ANDR_IOR_SUCCESS && r.value == last + 1);
	CHECK(emu.faults == 0);
	andr_mtl_close(&other);
	andr_mtl_close(&c);
	printf("PASS submit: IB must sit in a mapped BO of the caller, 16 IBs per submit, 300 fences across the ring end\n");
}

static void
test_imports(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "SpringBoard") == ANDR_IOR_SUCCESS);
	uint64_t w = c.win_base;
	/* 0: read-only pages, 1: one page not XNU RAM (MMIO), 2: a page of the GPU pool, 3: of the BO pool */
	regions[0] = (struct region){ .addr = 0x2000000000ull, .npages = 2, .pa = { TASK_PA, TASK_PA + PG }, .writable = false };
	regions[1] = (struct region){ .addr = 0x2100000000ull, .npages = 2, .pa = { TASK_PA + 2 * PG, 0x0c000000ull }, .writable = true };
	regions[2] = (struct region){ .addr = 0x2200000000ull, .npages = 1, .pa = { GPOOL_PA + 0x210000 }, .writable = true };
	regions[3] = (struct region){ .addr = 0x2300000000ull, .npages = 1, .pa = { mpool_pa + ANDR_MTL_PT_BYTES }, .writable = true };
	nregions = 4;
	andr_mtl_handle_t h[4];
	andr_mtl_bo_import_t bad[] = { { 0, 4096 }, { regions[0].addr + 1, 4096 }, { regions[0].addr, 4097 },
		{ regions[0].addr, 0 }, { regions[0].addr, (1ull << 28) + 4096 }, { 0x3000000000ull, 4096 } };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(call(&c, ANDR_MTL_M_BO_IMPORT, &bad[i], sizeof(bad[i]), &h[0], sizeof(h[0])) == ANDR_IOR_BAD_ARGUMENT);
	}
	for (unsigned i = 0; i < 4; i++) {
		andr_mtl_bo_import_t im = { regions[i].addr, (uint64_t)regions[i].npages * PG };
		CHECK(call(&c, ANDR_MTL_M_BO_IMPORT, &im, sizeof(im), &h[i], sizeof(h[i])) == ANDR_IOR_SUCCESS);
	}
	CHECK(live_import == 4);
	/* read-only pinned: mapped read-only although RW was asked */
	CHECK(map_bo(&c, h[0].handle, w, 3) == ANDR_IOR_SUCCESS && pte(w) == (TASK_PA | MT_PTE_RO) &&
	    pte(w + 0x7000) == ((TASK_PA + PG + 0x3000) | MT_PTE_RO));
	/* the others are refused, and leave no PTE behind */
	CHECK(map_bo(&c, h[1].handle, w + 0x100000, 3) == ANDR_IOR_VM_ERROR && pte(w + 0x100000) == 0 &&
	    c.bo[h[1].handle - 1].iova == 0);
	CHECK(map_bo(&c, h[2].handle, w + 0x200000, 3) == ANDR_IOR_VM_ERROR && pte(w + 0x200000) == 0);
	CHECK(map_bo(&c, h[3].handle, w + 0x300000, 3) == ANDR_IOR_VM_ERROR && pte(w + 0x300000) == 0);
	/* a GPU write into the read-only import faults instead of landing */
	uint64_t ib, len, fence;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, w + 0x400000, 1) == ANDR_IOR_SUCCESS);
	(void)cpu_view(&c, ib, &len);
	fence = ib_mem_write(&c, ib, w + 0x400000, w);
	CHECK(wait_fence(&c, fence, 1000000) == ANDR_IOR_SUCCESS && emu.ro_faults == 4);
	const uint32_t *t = (const uint32_t *)(const void *)host(TASK_PA, 16);
	CHECK(t[0] == 0 && t[3] == 0);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(free_bo(&c, h[i].handle) == ANDR_IOR_SUCCESS);
	}
	CHECK(live_import == 0);
	andr_mtl_close(&c);
	printf("PASS imports: read-only pins mapped read-only, MMIO and GPU-pool pages refused with no PTE left\n");
}

static void
test_busy(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t ib, len, fence = 0;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, c.win_base, 1) == ANDR_IOR_SUCCESS);
	uint32_t *iw = cpu_view(&c, ib, &len);
	iw[0] = pkt7(0x10, 1);
	andr_mtl_submit_buffer_t b[16];
	for (unsigned i = 0; i < 16; i++) {
		b[i] = (andr_mtl_submit_buffer_t){ ib, c.win_base, 8 };
	}
	emu.stall = true;			/* the CP reads nothing */
	unsigned n = 0;
	int rc;
	while ((rc = submit(&c, b, 16, &fence)) == ANDR_IOR_SUCCESS) {
		n++;
		CHECK(n < 200);
	}
	CHECK(rc == ANDR_IOR_BUSY && n == (ANDR_RING_DW - 1 - 16 - 40) / 73);
	/* the ring content the CP has not read is intact: it runs everything once unstalled */
	emu.stall = false;
	cp_run();
	CHECK(emu.fences == n && andr_rd(GPOOL_PA + ANDR_OFF_SCRATCH) == n && emu.ibs == 16ull * n);
	CHECK(submit(&c, b, 16, &fence) == ANDR_IOR_SUCCESS && fence == n + 1);
	andr_mtl_close(&c);
	printf("PASS busy ring: %u submits of 16 IBs fill it, then BUSY; nothing unread overwritten\n", n);
}

static void
test_hang(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t ib, len, dst;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, c.win_base, 1) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 0x4000, &dst) == ANDR_IOR_SUCCESS && map_bo(&c, dst, c.win_base + 0x4000, 3) == ANDR_IOR_SUCCESS);
	(void)cpu_view(&c, ib, &len);
	uint64_t f1 = ib_mem_write(&c, ib, c.win_base, c.win_base + 0x4000);
	CHECK(wait_fence(&c, f1, 1000000) == ANDR_IOR_SUCCESS);
	emu.hang = true;
	uint64_t f2 = ib_mem_write(&c, ib, c.win_base, c.win_base + 0x4000);
	unsigned slices = 0;
	int rc;
	while ((rc = wait_fence(&c, f2, 1000000)) == ANDR_IOR_TIMEOUT) {	/* Turnip's 1 ms slices */
		slices++;
		CHECK(slices < 2000);
	}
	CHECK(rc == ANDR_IOR_NOT_READY && slices >= 900 && !andromeda_mtl_usable());
	andr_mtl_status_t st;
	CHECK(call(&c, ANDR_MTL_M_GET_STATUS, NULL, 0, &st, sizeof(st)) == ANDR_IOR_SUCCESS && st.faults == 1);
	uint64_t fence;
	andr_mtl_submit_buffer_t b = { ib, c.win_base, 28 };
	CHECK(submit(&c, &b, 1, &fence) == ANDR_IOR_NOT_READY);
	CHECK(wait_fence(&c, f1, 0) == ANDR_IOR_SUCCESS);	/* landed before: still done */
	uint64_t h;
	CHECK(alloc_bo(&c, 4096, &h) == ANDR_IOR_NOT_READY && map_bo(&c, dst, c.win_base + 0x100000, 3) != ANDR_IOR_SUCCESS);
	struct andr_mtl_client late;
	CHECK(andr_mtl_open(&late, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "late") == ANDR_IOR_NOT_READY);
	/* close: unmapped (the TLB sync is the SMMU's, it still answers), memory back */
	andr_mtl_close(&c);
	CHECK(live_phys == 0 && pte(c.win_base) == 0 && free_bytes() == SMALL_POOL - ANDR_MTL_PT_BYTES);
	printf("PASS hang: no progress for 1 s stops Metal (faults 1, NOT_READY), nothing reset, memory unmapped then freed\n");
}

static void
test_thermal_tlb(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t ib, len, fence;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, c.win_base, 1) == ANDR_IOR_SUCCESS);
	uint32_t *iw = cpu_view(&c, ib, &len);
	iw[0] = pkt7(0x10, 1);
	andr_mtl_submit_buffer_t b = { ib, c.win_base, 8 };
	CHECK(submit(&c, &b, 1, &fence) == ANDR_IOR_SUCCESS);
	emu.temp_mc = 85000;
	CHECK(submit(&c, &b, 1, &fence) == ANDR_IOR_SUCCESS);	/* checked every 10 s only */
	now_ns += 11000000000ull;
	CHECK(submit(&c, &b, 1, &fence) == ANDR_IOR_NOT_READY && !andromeda_mtl_usable());
	andr_mtl_close(&c);

	ready(2);
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	emu.temp_valid = false;
	CHECK(alloc_bo(&c, 0x4000, &ib) == ANDR_IOR_SUCCESS && map_bo(&c, ib, c.win_base, 1) == ANDR_IOR_SUCCESS);
	b.handle = ib;
	CHECK(submit(&c, &b, 1, &fence) == ANDR_IOR_NOT_READY && !andromeda_mtl_usable());
	andr_mtl_close(&c);

	/* a TLB sync that never completes: stopped, and the memory is kept (never reused under a live PTE) */
	ready(2);
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t a, a2, all = free_bytes();
	CHECK(alloc_bo(&c, 0x8000, &a) == ANDR_IOR_SUCCESS && map_bo(&c, a, c.win_base, 3) == ANDR_IOR_SUCCESS);
	CHECK(alloc_bo(&c, 0x8000, &a2) == ANDR_IOR_SUCCESS);
	emu.tlb_stuck = true;
	CHECK(map_bo(&c, a2, c.win_base + 0x100000, 3) == ANDR_IOR_NOT_READY && !andromeda_mtl_usable());
	CHECK(free_bo(&c, a) == ANDR_IOR_SUCCESS && live_phys == 2 && free_bytes() == all - 0x10000);
	andr_mtl_close(&c);
	CHECK(live_phys == 2 && free_bytes() == all - 0x10000);
	printf("PASS stops: GPU over andrgpuhot or unreadable, SMMU TLB sync timeout (memory kept, not reused)\n");
}

static void
test_zombies(void)
{
	ready(2);
	struct andr_mtl_client c;
	CHECK(andr_mtl_open(&c, FAKE_TASK, ANDR_MTL_OPEN_TYPE, "backboardd") == ANDR_IOR_SUCCESS);
	uint64_t a, b, all = free_bytes();
	CHECK(alloc_bo(&c, 0x100000, &a) == ANDR_IOR_SUCCESS);
	struct fmd *m = andr_mtl_cpu_memory(&c, (uint32_t)a);	/* a task mapping holds it */
	CHECK(m != NULL && m->refs == 2);
	CHECK(free_bo(&c, a) == ANDR_IOR_SUCCESS && nzombies == 1 && free_bytes() == all - 0x100000);
	CHECK(alloc_bo(&c, 0x100000, &b) == ANDR_IOR_SUCCESS && nzombies == 1);	/* still mapped: kept */
	CHECK(c.bo[b - 1].pool_pa != m->pa);
	m->refs--;				/* the task unmapped it */
	CHECK(alloc_bo(&c, 4096, &a) == ANDR_IOR_SUCCESS && nzombies == 0);	/* swept */
	andr_mtl_close(&c);
	CHECK(free_bytes() == all && mt_nfree == 1 && live_phys == 0);
	printf("PASS zombies: pool memory still mapped by a task is reused only after its last mapping\n");
}

int
main(void)
{
	gpool = calloc(1, ANDR_POOL_SIZE);
	task_ram = calloc(TASK_PAGES, PG);
	CHECK(gpool != NULL && task_ram != NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_layout();
	test_off();
	test_pool_checks();
	test_publish_order();
	test_open_windows();
	test_query();
	test_alloc_free();
	test_map();
	test_v2test();
	test_submit_checks();
	test_imports();
	test_busy();
	test_hang();
	test_thermal_tlb();
	test_zombies();
	reset();
	free(gpool);
	free(task_ram);
	printf("andromeda_gpu_mtl host test: ALL PASS (model only: no IOKit, SMMU or A640 executed)\n");
	return 0;
}
