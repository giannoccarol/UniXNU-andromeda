/*
 * Phases F5/F6 on andromeda (Xiaomi Mi Mix 3 5G, SM8150, Adreno 640): the
 * C side of SweetAccelerator and its NativeAdreno user client. Off unless
 * andrmtl=N; see andromeda_gpu_mtl.h for the boot-args, the sources and how
 * this differs from ios-flame.
 *
 * What is here (ported from ios-flame's sweet_gpu.c and SweetAccelerator.cpp,
 * patch 0115-flame-gpu-v2-client-turnip, Nick Wylynko / NWylynko,
 * https://github.com/NWylynko/ios-flame commit 745c4341; MIT, patched XNU
 * files APSL 2.0; the ABI is UniXNU's NativeAdreno, MIT):
 *   - the BO pool: RAM XNU does not manage (SWEET has no PPL, so the physmap
 *     maps managed RAM cacheable and a write-combined alias of it loses
 *     writes: flame's "big trap"), first fit in 16 KiB units, sorted and
 *     merged free list (flame gpool); here a single range from andrmtlpool,
 *     checked page by page and mapped write-combining once;
 *   - the GPU page tables of the client windows: 4K pages under the shared
 *     L1 of andromeda_gpu_core.c, tables from the pool's first 8 MiB (flame
 *     a6_map_pages / a6_unmap / map_4k / pt_walk, 0106's extra tables);
 *   - one 4 GB GPU address window per connection from 64 GB up;
 *   - the ring submit of up to 16 IBs wrapped as Linux's a6xx_submit: CCU
 *     depth/colour invalidate, the IBs, CACHE_FLUSH_TS writing the fence
 *     (flame a6_submit_ibs_async), with the room check of 0109;
 *   - fences, waits in short slices and the no-progress hang check (flame
 *     gpu_note_submit_locked / gpu_hung_locked), without flame's reset: a
 *     hang stops Metal for the boot;
 *   - the ABI methods (flame v2Method), with stricter checks (header).
 * The GPU itself (power, GMU, CP, SMMU banks) is andromeda_gpu.c's and
 * andromeda_gpu_core.c's; this file only uses the ring, the fence word, the
 * SMMU context bank's TLB registers and the L1 table they set up, after the
 * bring-up thread has handed them over (andromeda_gpu_mtl_attach).
 *
 * Locking: one mutex for the GPU side (ring, fence counter, page tables,
 * pool, windows) and every client's BO table; fence waits poll without it.
 * The OS-layer calls that may call back (descriptor release -> pool free)
 * are made without it.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#if defined(KERNEL)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <vm/pmap.h>		/* VM_WIMG_WCOMB */
#elif !defined(ANDR_MTL_HOST_TEST)	/* host syntax check: stubinc */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
typedef unsigned int boolean_t;
#define VM_WIMG_WCOMB 6
#endif	/* (the host test, test_andromeda_gpu_mtl.c, declares the environment itself) */
#include "andromeda_gpu.h"
#include "andromeda_gpu_mtl.h"

#define MT_TAG "ANDROMEDA MTL:"

/* Same declarations as andromeda_gpu.c. */
extern boolean_t pmap_valid_address(uint64_t pa);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
#if defined(KERNEL)
extern int vprintf(const char *, va_list);
#endif

#if defined(KERNEL)
static LCK_GRP_DECLARE(mt_lck_grp, "andromeda_mtl");
static LCK_MTX_DECLARE(mt_lck, &mt_lck_grp);
#define MT_LOCK()	lck_mtx_lock(&mt_lck)
#define MT_UNLOCK()	lck_mtx_unlock(&mt_lck)
#else
void andr_mtl_host_lock(void);
void andr_mtl_host_unlock(void);
#define MT_LOCK()	andr_mtl_host_lock()
#define MT_UNLOCK()	andr_mtl_host_unlock()
#endif

/* ---- layout and registers ---- */

/* DRAM and the regions the BO pool must stay out of (as andromeda_gpu.c). */
#define MT_DRAM_BASE		0xc0000000ull
#define MT_DRAM_END		0x200000000ull
#define MT_MPSS_EFS_END		0xc0300000ull	/* Aloha "MPSS_EFS", reserved */
#define MT_PSTORE_BASE		0x17fe00000ull
#define MT_PSTORE_END		0x180000000ull

#define MT_POOL_ALIGN		0x200000ull
#define MT_MAX_BO		(1ull << ANDR_MTL_MAX_BO_SHIFT)

/* Stage-1 4K page descriptors: AF, inner shareable, AttrIndx 0 (normal NC). */
#define MT_PTE_RW		((1ull << 10) | (3ull << 8) | (1ull << 6) | 3ull)	/* AP=01 */
#define MT_PTE_RO		((1ull << 10) | (3ull << 8) | (3ull << 6) | 3ull)	/* AP=11 */
#define MT_DESC_ADDR(e)		((e) & 0x000000fffffff000ull)

/* SMMU context bank and GPU registers (as andromeda_gpu_core.c / _present.c). */
#define MT_CB_FSR		0x58u
#define MT_CB_FAR		0x60u
#define MT_CB_FSYNR0		0x68u
#define MT_CB_TLBIALL		0x618u
#define MT_CB_TLBSYNC		0x7f0u
#define MT_CB_TLBSTATUS		0x7f4u
#define MT_RBBM_STATUS		0x210u
#define MT_CP_RB_RPTR		0x806u
#define MT_CP_RB_WPTR		0x807u
#define MT_CP_HW_FAULT		0x821u
#define MT_CP_STATUS_1		0x825u
#define MT_CP_IB1_BASE		0x928u		/* lo, hi, then IB1_REM_SIZE */
#define MT_CP_IB2_BASE		0x92bu
#define MT_ALWAYS_ON_LO		0x980u		/* CP_ALWAYS_ON_COUNTER, 19.2 MHz */
#define MT_ALWAYS_ON_HI		0x981u

/* PM4 opcodes and events (adreno_pm4.xml). */
#define MT_CP_INDIRECT_BUFFER	0x3fu
#define MT_CP_EVENT_WRITE	0x46u
#define MT_EV_CACHE_FLUSH_TS	0x04u
#define MT_EV_CCU_INVALIDATE_DEPTH 0x18u
#define MT_EV_CCU_INVALIDATE_COLOR 0x19u
#define MT_RING_SLACK		16u		/* dwords kept free in the ring (flame) */

#define MT_TLB_POLLS		1000u		/* x 10 us */
#define MT_HANG_MS		1000u		/* no fence progress with work queued */
#define MT_WAIT_MAX_NS		10000000000ull	/* one wait call at most (callers loop) */
#define MT_CLOSE_WAIT_NS	2000000000ull	/* a closing client's last fence (flame) */
#define MT_THERMAL_SECS		10u

/* ---- state ---- */

enum { MT_UNPARSED = 0, MT_OFF, MT_WAITING, MT_READY, MT_STOPPED };

static int mt_state;
static unsigned mt_level, mt_hot = 80;
static uint64_t mt_pool_arg = ANDR_MTL_POOL_DEFAULT, mt_pool_size_arg = ANDR_MTL_POOL_SIZE_DEFAULT;
static uint32_t mt_pub;			/* bit 0 service started, 1 GPU attached, 2 published */
static uint32_t mt_logged;		/* methods logged once */

static struct andr_gpu *mt_g;
static uint64_t mt_gpool_lo, mt_gpool_hi;	/* andromeda_gpu.c's pool */
static uint64_t mt_fence_pa, mt_fence_va;

static uintptr_t mt_kva;		/* the BO pool, write-combining */
static uint64_t mt_pa, mt_size;
static uint32_t mt_pt_next, mt_pt_max;
static struct {
	uint64_t pa, size;
} mt_free[ANDR_MTL_FREE_MAX];
static uint32_t mt_nfree;
static uint64_t mt_used, mt_peak;
static uint64_t mt_win_used;		/* bit n: window n taken */

static uint32_t mt_progress_fence;
static uint64_t mt_progress_abs, mt_check_abs, mt_thermal_abs;
static uint64_t mt_submits;

static uint64_t mt_pte[512];
static const uint64_t mt_zero_pte[512];

/* ---- helpers ---- */

static void mt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
mt_log(const char *fmt, ...)
{
	va_list ap;
	printf(MT_TAG " ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static inline void
mt_dsb(void)
{
#if defined(KERNEL)
	__asm__ volatile ("dsb osh" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static int
mt_get_state(void)
{
	return __atomic_load_n(&mt_state, __ATOMIC_ACQUIRE);
}

static bool
mt_move(int from, int to)
{
	return __atomic_compare_exchange_n(&mt_state, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint32_t
mt_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
	    (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t
mt_pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (mt_parity(cnt) << 15) | ((op & 0x7fu) << 16) | (mt_parity(op) << 23);
}

static uint64_t
mt_ns_to_abs(uint64_t ns)
{
	uint64_t abs = 0;
	nanoseconds_to_absolutetime(ns, &abs);
	return abs;
}

static bool
mt_in_pool(uint64_t pa, uint64_t len)
{
	return mt_kva && pa >= mt_pa && len <= mt_size && pa - mt_pa <= mt_size - len;
}

/* Overlap of [a, a + al) and [b, b + bl). */
static bool
mt_overlap(uint64_t a, uint64_t al, uint64_t b, uint64_t bl)
{
	return a < b + bl && b < a + al;
}

/* Page tables: the L1 lives in andromeda_gpu.c's pool, the rest in ours. */
static uint64_t
mt_rd64(uint64_t pa)
{
	uint64_t v = 0;
	if (mt_in_pool(pa, 8)) {
		memcpy(&v, (const void *)(mt_kva + (uintptr_t)(pa - mt_pa)), 8);
	} else {
		(void)andr_read(pa, &v, 8);
	}
	return v;
}

static void
mt_wr(uint64_t pa, const void *buf, size_t len)
{
	if (mt_in_pool(pa, len)) {
		memcpy((void *)(mt_kva + (uintptr_t)(pa - mt_pa)), buf, len);
		mt_dsb();
	} else {
		(void)andr_write(pa, buf, len);
	}
}

static uint32_t
mt_fence_read(void)
{
	return andr_rd(mt_fence_pa);
}

static bool
mt_fence_done(uint32_t fence)
{
	return (int32_t)(mt_fence_read() - fence) >= 0;
}

/* ---- boot-args, publication ---- */

static void
mt_args(void)
{
	if (mt_get_state() != MT_UNPARSED) {
		return;
	}
	uint32_t level = 0, hot = 80;
	uint64_t pool = ANDR_MTL_POOL_DEFAULT, size = ANDR_MTL_POOL_SIZE_DEFAULT;
	(void)PE_parse_boot_argn("andrmtl", &level, sizeof(level));
	(void)PE_parse_boot_argn("andrmtlpool", &pool, sizeof(pool));
	(void)PE_parse_boot_argn("andrmtlpoolsize", &size, sizeof(size));
	(void)PE_parse_boot_argn("andrgpuhot", &hot, sizeof(hot));
	if (level > ANDR_MTL_METAL) {
		printf(MT_TAG " andrmtl=%u is not a level (0, 1, 2): off\n", level);
		level = 0;
	}
	if (hot == 0 || hot > 95) {
		hot = 80;
	}
	/* Several threads may get here; they store the same values. */
	__atomic_store_n(&mt_level, level, __ATOMIC_RELAXED);
	__atomic_store_n(&mt_hot, hot, __ATOMIC_RELAXED);
	__atomic_store_n(&mt_pool_arg, pool, __ATOMIC_RELAXED);
	__atomic_store_n(&mt_pool_size_arg, size, __ATOMIC_RELAXED);
	int expected = MT_UNPARSED;
	(void)__atomic_compare_exchange_n(&mt_state, &expected, level ? MT_WAITING : MT_OFF, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

unsigned
andromeda_mtl_level(void)
{
	mt_args();
	return mt_get_state() == MT_OFF ? 0u : __atomic_load_n(&mt_level, __ATOMIC_RELAXED);
}

bool
andromeda_mtl_usable(void)
{
	return mt_get_state() == MT_READY;
}

/* Publish once both the IOKit service exists and the GPU is attached, in either order. */
static void
mt_try_publish(void)
{
	uint32_t f = __atomic_load_n(&mt_pub, __ATOMIC_ACQUIRE);
	while ((f & 3u) == 3u && !(f & 4u)) {
		if (__atomic_compare_exchange_n(&mt_pub, &f, f | 4u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			andr_mtl_os_publish(andromeda_mtl_level());
			return;
		}
	}
}

void
andromeda_mtl_service_started(void)
{
	(void)__atomic_fetch_or(&mt_pub, 1u, __ATOMIC_ACQ_REL);
	mt_try_publish();
}

/* ---- the BO pool ---- */

static uint64_t
mt_round(uint64_t bytes)
{
	return (bytes + ANDR_MTL_UNIT - 1) & ~(ANDR_MTL_UNIT - 1);
}

/* First fit (flame sweet_gpu_pool_alloc). With mt_lck. 0 = ok. */
static int
mt_pool_alloc_locked(uint64_t size, uint64_t *pa)
{
	for (uint32_t i = 0; i < mt_nfree; i++) {
		if (mt_free[i].size < size) {
			continue;
		}
		*pa = mt_free[i].pa;
		mt_free[i].pa += size;
		mt_free[i].size -= size;
		if (mt_free[i].size == 0) {
			memmove(&mt_free[i], &mt_free[i + 1], (mt_nfree - i - 1) * sizeof(mt_free[0]));
			mt_nfree--;
		}
		mt_used += size;
		if (mt_used > mt_peak) {
			mt_peak = mt_used;
		}
		if ((mt_used >> 25) != ((mt_used - size) >> 25)) {
			mt_log("pool: %llu MiB in use (peak %llu)\n", (unsigned long long)(mt_used >> 20),
			    (unsigned long long)(mt_peak >> 20));
		}
		return 0;
	}
	return -1;
}

/* Back into the sorted free list, merged with its neighbours (flame sweet_gpu_pool_free). With mt_lck. */
static void
mt_pool_free_locked(uint64_t pa, uint64_t size)
{
	if (size == 0 || !mt_in_pool(pa, size) || pa - mt_pa < ANDR_MTL_PT_BYTES) {
		mt_log("pool: refusing to free 0x%llx + 0x%llx (not BO memory)\n", (unsigned long long)pa,
		    (unsigned long long)size);
		return;
	}
	mt_used -= size;
	uint32_t i = 0;
	while (i < mt_nfree && mt_free[i].pa < pa) {
		i++;
	}
	if (i > 0 && mt_free[i - 1].pa + mt_free[i - 1].size == pa) {
		mt_free[i - 1].size += size;
		if (i < mt_nfree && pa + size == mt_free[i].pa) {
			mt_free[i - 1].size += mt_free[i].size;
			memmove(&mt_free[i], &mt_free[i + 1], (mt_nfree - i - 1) * sizeof(mt_free[0]));
			mt_nfree--;
		}
	} else if (i < mt_nfree && pa + size == mt_free[i].pa) {
		mt_free[i].pa = pa;
		mt_free[i].size += size;
	} else if (mt_nfree < ANDR_MTL_FREE_MAX) {
		memmove(&mt_free[i + 1], &mt_free[i], (mt_nfree - i) * sizeof(mt_free[0]));
		mt_free[i].pa = pa;
		mt_free[i].size = size;
		mt_nfree++;
	} else {
		mt_log("pool: free list full, 0x%llx bytes at 0x%llx leak\n", (unsigned long long)size,
		    (unsigned long long)pa);
	}
}

void
andr_mtl_pool_free(uint64_t pa, uint64_t bytes)
{
	MT_LOCK();
	mt_pool_free_locked(pa, mt_round(bytes));
	MT_UNLOCK();
}

/* Check and map the pool (andrmtlpool, andrmtlpoolsize). 0 = ok. */
static int
mt_pool_setup(void)
{
	uint64_t base = __atomic_load_n(&mt_pool_arg, __ATOMIC_RELAXED);
	uint64_t size = __atomic_load_n(&mt_pool_size_arg, __ATOMIC_RELAXED);
	const char *why = NULL;
	if ((base & (MT_POOL_ALIGN - 1)) || (size & (ANDR_MTL_UNIT - 1)) || size < ANDR_MTL_POOL_MIN) {
		why = "base must be 2 MiB aligned, size a 16 KiB multiple of at least 32 MiB";
	} else if (base < MT_DRAM_BASE || size > MT_DRAM_END - MT_DRAM_BASE || base > MT_DRAM_END - size) {
		why = "outside DRAM 0xc0000000..0x200000000";
	} else if (mt_overlap(base, size, MT_DRAM_BASE, MT_MPSS_EFS_END - MT_DRAM_BASE)) {
		why = "overlaps MPSS_EFS 0xc0000000..0xc0300000";
	} else if (mt_overlap(base, size, MT_PSTORE_BASE, MT_PSTORE_END - MT_PSTORE_BASE)) {
		why = "overlaps PStore 0x17fe00000..0x180000000";
	} else if (mt_overlap(base, size, mt_gpool_lo, mt_gpool_hi - mt_gpool_lo)) {
		why = "overlaps the GPU pool (andrgpupool)";
	}
	if (why != NULL) {
		mt_log("pool 0x%llx size 0x%llx refused: %s\n", (unsigned long long)base, (unsigned long long)size, why);
		return -1;
	}
	/* Every page: refuse if any is XNU-managed (fail-safe, as andromeda_gpu.c's pool). */
	for (uint64_t o = 0; o < size; o += ANDR_MTL_UNIT) {
		if (pmap_valid_address(base + o)) {
			mt_log("pool 0x%llx: page 0x%llx is XNU-managed, refusing\n", (unsigned long long)base,
			    (unsigned long long)(base + o));
			return -1;
		}
	}
	vm_offset_t va = ml_io_map_unmappable((vm_offset_t)base, (vm_size_t)size, VM_WIMG_WCOMB);
	if (!va) {
		mt_log("pool 0x%llx: write-combining map failed\n", (unsigned long long)base);
		return -1;
	}
	mt_kva = (uintptr_t)va;
	mt_pa = base;
	mt_size = size;
	/* Tables are taken from here and never freed: zeroed once now. BOs are zeroed when allocated. */
	memset((void *)mt_kva, 0, (size_t)ANDR_MTL_PT_BYTES);
	mt_dsb();
	mt_pt_next = 0;
	mt_pt_max = (uint32_t)(ANDR_MTL_PT_BYTES / 0x1000u);
	mt_free[0].pa = base + ANDR_MTL_PT_BYTES;
	mt_free[0].size = size - ANDR_MTL_PT_BYTES;
	mt_nfree = 1;
	mt_used = mt_peak = 0;
	mt_log("pool at pa 0x%llx size 0x%llx (not XNU-managed, write-combining): %llu MiB for buffers, %u page tables\n",
	    (unsigned long long)base, (unsigned long long)size, (unsigned long long)((size - ANDR_MTL_PT_BYTES) >> 20),
	    mt_pt_max);
	return 0;
}

/* ---- stopping (no reset here: Metal ends for the boot) ---- */

static void
mt_stop_locked(const char *why)
{
	if (!mt_move(MT_READY, MT_STOPPED)) {
		return;
	}
	uint64_t c = ANDR_SMMU_CB((uint32_t)mt_g->gpu_cb);
	uint64_t far = andr_rd(c + MT_CB_FAR) | ((uint64_t)andr_rd(c + MT_CB_FAR + 4) << 32);
	mt_log("GPU stopped for this boot (%s): fence %u, done %u; rb rptr %x wptr %x; ib1 %08x%08x rem %x, "
	    "ib2 %08x%08x rem %x; rbbm %x hwfault %x cp_status1 %x; smmu cb%d fsr %x far %llx fsynr0 %x. "
	    "Clients get device lost; the display keeps the CPU copy\n", why, mt_g->fence, mt_fence_read(),
	    andr_rd(ANDR_GREG(MT_CP_RB_RPTR)), mt_g->wptr, andr_rd(ANDR_GREG(MT_CP_IB1_BASE + 1)),
	    andr_rd(ANDR_GREG(MT_CP_IB1_BASE)), andr_rd(ANDR_GREG(MT_CP_IB1_BASE + 2)),
	    andr_rd(ANDR_GREG(MT_CP_IB2_BASE + 1)), andr_rd(ANDR_GREG(MT_CP_IB2_BASE)),
	    andr_rd(ANDR_GREG(MT_CP_IB2_BASE + 2)), andr_rd(ANDR_GREG(MT_RBBM_STATUS)),
	    andr_rd(ANDR_GREG(MT_CP_HW_FAULT)), andr_rd(ANDR_GREG(MT_CP_STATUS_1)), mt_g->gpu_cb,
	    andr_rd(c + MT_CB_FSR), (unsigned long long)far, andr_rd(c + MT_CB_FSYNR0));
}

/* Work queued on an idle GPU restarts the progress clock (flame gpu_note_submit_locked). */
static void
mt_note_submit_locked(void)
{
	if (mt_fence_read() == mt_g->fence) {
		mt_progress_fence = mt_g->fence;
		mt_progress_abs = mach_absolute_time();
	}
}

/* No fence progress for ms although work is queued (flame gpu_hung_locked). */
static bool
mt_hung_locked(uint32_t ms)
{
	uint32_t done = mt_fence_read();
	uint64_t now = mach_absolute_time();
	if (done != mt_progress_fence || (int32_t)(mt_g->fence - done) <= 0) {
		mt_progress_fence = done;
		mt_progress_abs = now;
		return false;
	}
	return now - mt_progress_abs > mt_ns_to_abs((uint64_t)ms * 1000000ull);
}

static bool
mt_thermal_ok_locked(void)
{
	uint64_t now = mach_absolute_time();
	if (mt_thermal_abs && now - mt_thermal_abs < mt_ns_to_abs((uint64_t)MT_THERMAL_SECS * 1000000000ull)) {
		return true;
	}
	mt_thermal_abs = now;
	int mc = 0;
	bool valid = andr_gpu_temperature(&mc);
	unsigned hot = __atomic_load_n(&mt_hot, __ATOMIC_RELAXED);
	if (valid && mc < (int)hot * 1000) {
		return true;
	}
	mt_log("GPU temperature %s %d mC (limit %u C)\n", valid ? "at" : "UNREADABLE", mc, hot);
	mt_stop_locked(valid ? "GPU too hot" : "GPU temperature unreadable");
	return false;
}

/*
 * Wait up to timeout_ns for a fence (flame sweet_gpu_wait_fence_ns, which
 * Turnip calls in 1 ms slices). 0 done, 1 timeout, -1 stopped. The hang
 * check runs by time, under the lock, every 5 ms of waiting.
 */
static int
mt_wait(uint32_t fence, uint64_t timeout_ns)
{
	if (timeout_ns > MT_WAIT_MAX_NS) {
		timeout_ns = MT_WAIT_MAX_NS;
	}
	uint64_t t0 = mach_absolute_time(), limit = mt_ns_to_abs(timeout_ns), check = mt_ns_to_abs(5000000ull);
	for (uint32_t i = 0;; i++) {
		if (mt_fence_done(fence)) {
			return 0;
		}
		if (mt_get_state() != MT_READY) {
			return -1;
		}
		uint64_t now = mach_absolute_time();
		if (now - __atomic_load_n(&mt_check_abs, __ATOMIC_RELAXED) > check) {
			__atomic_store_n(&mt_check_abs, now, __ATOMIC_RELAXED);
			MT_LOCK();
			if (!mt_fence_done(fence) && mt_hung_locked(MT_HANG_MS)) {
				mt_stop_locked("submitted work made no progress for 1 s (GPU hang?)");
			}
			MT_UNLOCK();
		}
		if (now - t0 >= limit) {
			return mt_fence_done(fence) ? 0 : 1;
		}
		delay(i < 50 ? 20 : 100);
	}
}

/* ---- GPU page tables ---- */

/* A zeroed 4K table from the pool's table area (never freed). With mt_lck. */
static uint64_t
mt_pt_alloc(void)
{
	if (mt_pt_next >= mt_pt_max) {
		return 0;
	}
	return mt_pa + 0x1000ull * mt_pt_next++;
}

/* The next-level table under table[idx], made if empty; 0 if a block is there or no table is left. */
static uint64_t
mt_pt_walk(uint64_t table, unsigned idx)
{
	uint64_t e = mt_rd64(table + 8ull * idx);
	if (e & 1) {
		return (e & 3) == 3 ? MT_DESC_ADDR(e) : 0;
	}
	uint64_t t = mt_pt_alloc();
	if (t) {
		uint64_t d = t | 3ull;
		mt_wr(table + 8ull * idx, &d, 8);
	}
	return t;
}

typedef uint64_t (*mt_pa_fn)(void *arg, uint64_t offset);

/*
 * 4K entries for [iova, iova + n4k * 4K), the physical address of the piece at
 * offset o from pa_of (flame map_4k). With mt_lck; the caller syncs the TLB.
 * Fails (-1) without having written a PTE for a piece whose pa_of returned 0.
 */
static int
mt_map(uint64_t iova, uint64_t n4k, mt_pa_fn pa_of, void *arg, uint64_t attrs)
{
	uint64_t l1 = andr_pool_pa(mt_g, ANDR_OFF_GT_L1);
	for (uint64_t i = 0; i < n4k;) {
		uint64_t va = iova + 0x1000ull * i;
		uint64_t l2 = mt_pt_walk(l1, (unsigned)((va >> 30) & 0x1ff));
		uint64_t l3 = l2 ? mt_pt_walk(l2, (unsigned)((va >> 21) & 0x1ff)) : 0;
		if (!l3) {
			mt_log("map: no page table for 0x%llx (%u of %u tables used)\n", (unsigned long long)va,
			    mt_pt_next, mt_pt_max);
			return -1;
		}
		unsigned first = (unsigned)((va >> 12) & 0x1ff), k = 0;
		while (first + k < 512u && i < n4k) {
			uint64_t pa = pa_of(arg, 0x1000ull * i);
			if (!pa) {
				if (k) {
					mt_wr(l3 + 8ull * first, mt_pte, 8ull * k);
				}
				return -1;
			}
			mt_pte[k++] = pa | attrs;
			i++;
		}
		mt_wr(l3 + 8ull * first, mt_pte, 8ull * k);
	}
	return 0;
}

/* Clear the 4K entries of [iova, iova + size) (tables stay, flame a6_unmap). With mt_lck. */
static void
mt_unmap(uint64_t iova, uint64_t size)
{
	uint64_t l1 = andr_pool_pa(mt_g, ANDR_OFF_GT_L1);
	uint64_t end = iova + ((size + 0xfff) & ~0xfffull);
	for (uint64_t va = iova & ~0xfffull; va < end;) {
		uint64_t e = mt_rd64(l1 + 8 * ((va >> 30) & 0x1ff)), l2 = (e & 3) == 3 ? MT_DESC_ADDR(e) : 0, l3 = 0;
		if (l2) {
			e = mt_rd64(l2 + 8 * ((va >> 21) & 0x1ff));
			l3 = (e & 3) == 3 ? MT_DESC_ADDR(e) : 0;
		}
		uint64_t next = (va | 0x1fffffull) + 1;
		if (next > end) {
			next = end;
		}
		if (l3) {
			mt_wr(l3 + 8 * ((va >> 12) & 0x1ff), mt_zero_pte, (size_t)((next - va) >> 12) * 8);
		}
		va = next;
	}
}

/* TLBIALL + sync on the GPU's context bank; 0 = confirmed. With mt_lck. */
static int
mt_tlb_sync_locked(void)
{
	uint64_t c = ANDR_SMMU_CB((uint32_t)mt_g->gpu_cb);
	andr_wr(c + MT_CB_TLBIALL, 0);
	andr_wr(c + MT_CB_TLBSYNC, 0);
	for (uint32_t i = 0; i < MT_TLB_POLLS; i++) {
		if (!(andr_rd(c + MT_CB_TLBSTATUS) & 1u)) {	/* 0xffffffff (unmapped) counts as busy */
			return 0;
		}
		delay(10);
	}
	mt_stop_locked("SMMU TLB sync timeout");
	return -1;
}

/* ---- the ring ---- */

/*
 * Up to ANDR_MTL_MAX_BUFFERS IBs as Linux's a6xx_submit wraps them (flame
 * a6_submit_ibs_async, 0115): CCU depth and colour invalidate, the IBs, then
 * CACHE_FLUSH_TS writes the fence once the caches are clean. Returns the
 * fence; 0 with *busy when the ring has no room now. With mt_lck.
 */
static uint32_t
mt_submit_locked(const uint64_t *iova, const uint32_t *dwords, unsigned nib, bool *busy)
{
	struct andr_gpu *g = mt_g;
	uint32_t dw[4 + 4 * ANDR_MTL_MAX_BUFFERS + 5], n = 0;
	uint32_t fence = g->fence + 1;
	*busy = false;
	if (nib == 0 || nib > ANDR_MTL_MAX_BUFFERS) {
		return 0;
	}
	if (fence == 0) {
		fence = 1;	/* 0 means "no fence" to the clients */
	}
	dw[n++] = mt_pkt7(MT_CP_EVENT_WRITE, 1);
	dw[n++] = MT_EV_CCU_INVALIDATE_DEPTH;
	dw[n++] = mt_pkt7(MT_CP_EVENT_WRITE, 1);
	dw[n++] = MT_EV_CCU_INVALIDATE_COLOR;
	for (unsigned i = 0; i < nib; i++) {
		dw[n++] = mt_pkt7(MT_CP_INDIRECT_BUFFER, 3);
		dw[n++] = (uint32_t)iova[i];
		dw[n++] = (uint32_t)(iova[i] >> 32);
		dw[n++] = dwords[i] & 0xfffffu;
	}
	dw[n++] = mt_pkt7(MT_CP_EVENT_WRITE, 4);
	dw[n++] = MT_EV_CACHE_FLUSH_TS;
	dw[n++] = (uint32_t)mt_fence_va;
	dw[n++] = (uint32_t)(mt_fence_va >> 32);
	dw[n++] = fence;
	/* Room in the ring: what the CP has not read yet stays intact. */
	uint32_t r = andr_rd(ANDR_GREG(MT_CP_RB_RPTR));
	if (r >= ANDR_RING_DW) {
		mt_stop_locked("ring read pointer out of range");
		return 0;
	}
	uint32_t used = (g->wptr + ANDR_RING_DW - r) % ANDR_RING_DW;
	if (used + n + MT_RING_SLACK >= ANDR_RING_DW) {
		*busy = true;
		return 0;
	}
	mt_note_submit_locked();
	uint32_t w = g->wptr, first = n < ANDR_RING_DW - w ? n : ANDR_RING_DW - w;
	(void)andr_write(andr_pool_pa(g, ANDR_OFF_RING) + 4ull * w, dw, 4ull * first);
	if (first < n) {
		(void)andr_write(andr_pool_pa(g, ANDR_OFF_RING), dw + first, 4ull * (n - first));
	}
	g->wptr = (w + n) % ANDR_RING_DW;
	__atomic_store_n(&g->fence, fence, __ATOMIC_RELEASE);
	andr_wr(ANDR_GREG(MT_CP_RB_WPTR), g->wptr);
	mt_submits++;
	return fence;
}

/* ---- attach ---- */

bool
andromeda_gpu_mtl_attach(struct andr_gpu *g, uint64_t gpu_pool_size)
{
	mt_args();
	int st = mt_get_state();
	if (st == MT_OFF) {
		mt_log("off (andrmtl=1: user client for v2test/vktest, andrmtl=2: also the Metal plug-in)\n");
		return false;
	}
	if (st != MT_WAITING) {
		return st == MT_READY;
	}
	if (g == NULL || g->gpu_cb < 0 || !g->zap_authenticated || gpu_pool_size < ANDR_POOL_SIZE || !andr_gx_up()) {
		mt_log("GPU not usable (cb %d, zap %d, pool 0x%llx, gx %d): nothing published\n", g ? g->gpu_cb : -1,
		    g ? g->zap_authenticated : 0, (unsigned long long)gpu_pool_size, andr_gx_up());
		(void)mt_move(MT_WAITING, MT_STOPPED);
		return false;
	}
	mt_gpool_lo = g->pool_pa;
	mt_gpool_hi = g->pool_pa + gpu_pool_size;
	if (mt_pool_setup()) {
		mt_log("no BO pool: nothing published; the GPU stays free for the present path\n");
		(void)mt_move(MT_WAITING, MT_STOPPED);
		return false;
	}
	mt_g = g;
	/* The fence word (scratch + 0, unused by the bring-up and the present path) and its counter. */
	mt_fence_pa = andr_pool_pa(g, ANDR_OFF_SCRATCH);
	mt_fence_va = andr_pool_va(ANDR_OFF_SCRATCH);
	andr_wr(mt_fence_pa, g->fence);
	mt_progress_fence = g->fence;
	mt_progress_abs = mach_absolute_time();
	if (!mt_move(MT_WAITING, MT_READY)) {
		return false;
	}
	mt_log("andrmtl=%u: GPU attached after step 4 (ring at wptr %u, fence %u, GPU windows 0x%llx + %u x 4 GB)%s\n",
	    andromeda_mtl_level(), g->wptr, g->fence, (unsigned long long)ANDR_MTL_WIN_BASE, ANDR_MTL_WIN_COUNT,
	    andromeda_mtl_level() == ANDR_MTL_METAL ? "; Metal plug-in SweetMetal2 named" : "; no Metal plug-in named");
	(void)__atomic_fetch_or(&mt_pub, 2u, __ATOMIC_ACQ_REL);
	mt_try_publish();
	return true;
}

/* ---- connections ---- */

static struct andr_mtl_bo *
mt_bo(struct andr_mtl_client *c, uint64_t handle)
{
	return handle >= 1 && handle <= c->bo_cap && c->bo[handle - 1].md != NULL ? &c->bo[handle - 1] : NULL;
}

/* A free slot (the table doubles when full); its handle. With mt_lck. */
static int
mt_bo_slot(struct andr_mtl_client *c, uint64_t *handle)
{
	uint32_t i = 0;
	while (i < c->bo_cap && c->bo[i].md != NULL) {
		i++;
	}
	if (i == c->bo_cap) {
		if (c->bo_cap >= ANDR_MTL_MAX_BOS) {
			return ANDR_IOR_NO_RESOURCES;
		}
		struct andr_mtl_bo *n = andr_mtl_os_bo_table(c->bo_cap * 2u);
		if (n == NULL) {
			return ANDR_IOR_NO_MEMORY;
		}
		memcpy(n, c->bo, sizeof(*n) * c->bo_cap);
		andr_mtl_os_bo_table_free(c->bo, c->bo_cap);
		c->bo = n;
		c->bo_cap *= 2u;
	}
	*handle = (uint64_t)i + 1;
	return ANDR_IOR_SUCCESS;
}

int
andr_mtl_open(struct andr_mtl_client *c, void *task, uint32_t type, const char *name)
{
	memset(c, 0, sizeof(*c));
	for (size_t i = 0; name != NULL && name[i] && i + 1 < sizeof(c->name); i++) {
		c->name[i] = name[i];
	}
	if (type != ANDR_MTL_OPEN_TYPE && type != ANDR_MTL_OPEN_DIRECT) {
		mt_log("%s: open type 0x%x refused (0x%x or %u)\n", c->name, type, ANDR_MTL_OPEN_TYPE, ANDR_MTL_OPEN_DIRECT);
		return ANDR_IOR_UNSUPPORTED;
	}
	if (!andromeda_mtl_usable()) {
		mt_log("%s: open refused: the GPU is %s\n", c->name, mt_get_state() == MT_STOPPED ? "stopped" : "not attached");
		return ANDR_IOR_NOT_READY;
	}
	struct andr_mtl_bo *table = andr_mtl_os_bo_table(256);
	if (table == NULL) {
		return ANDR_IOR_NO_MEMORY;
	}
	MT_LOCK();
	unsigned w = 0;
	while (w < ANDR_MTL_WIN_COUNT && (mt_win_used & (1ull << w))) {
		w++;
	}
	if (w < ANDR_MTL_WIN_COUNT) {
		mt_win_used |= 1ull << w;
	}
	MT_UNLOCK();
	if (w == ANDR_MTL_WIN_COUNT) {
		andr_mtl_os_bo_table_free(table, 256);
		mt_log("%s: open refused: all %u GPU windows taken\n", c->name, ANDR_MTL_WIN_COUNT);
		return ANDR_IOR_NO_RESOURCES;
	}
	c->task = task;
	c->win_base = ANDR_MTL_WIN_BASE + (uint64_t)w * ANDR_MTL_WIN_SIZE;
	c->win_size = ANDR_MTL_WIN_SIZE;
	c->bo = table;
	c->bo_cap = 256;
	c->open = 1;
	mt_log("%s: client open (type 0x%x), GPU window 0x%llx\n", c->name, type, (unsigned long long)c->win_base);
	return ANDR_IOR_SUCCESS;
}

/* Release a BO record taken out of a table (no lock: may call back into the pool). */
static void
mt_bo_release(struct andr_mtl_bo *bo)
{
	if (bo->imported) {
		andr_mtl_os_import_release(bo->md);
	} else {
		andr_mtl_os_phys_release(bo->md, bo->pool_pa, bo->len);
	}
}

void
andr_mtl_close(struct andr_mtl_client *c)
{
	if (!c->open) {
		return;
	}
	/* Its work may still run: wait for its last fence before unmapping (flame). */
	if (c->last_fence != 0 && mt_wait(c->last_fence, MT_CLOSE_WAIT_NS) != 0) {
		mt_log("%s: closed with fence %u still pending\n", c->name, c->last_fence);
		MT_LOCK();
		mt_stop_locked("a closing client's work did not finish in 2 s");
		MT_UNLOCK();
	}
	MT_LOCK();
	uint32_t mapped = 0, live = 0;
	for (uint32_t i = 0; i < c->bo_cap; i++) {
		if (c->bo[i].md != NULL) {
			live++;
			if (c->bo[i].iova) {
				mt_unmap(c->bo[i].iova, c->bo[i].bytes);
				mapped++;
			}
		}
	}
	int sync = mapped ? mt_tlb_sync_locked() : 0;
	mt_win_used &= ~(1ull << ((c->win_base - ANDR_MTL_WIN_BASE) / ANDR_MTL_WIN_SIZE));
	MT_UNLOCK();
	uint32_t kept = 0;
	for (uint32_t i = 0; i < c->bo_cap; i++) {
		if (c->bo[i].md == NULL) {
			continue;
		}
		if (sync == 0 && !c->bo[i].kept) {
			mt_bo_release(&c->bo[i]);	/* unreachable for the GPU now: the pages go back */
		} else {
			kept++;
		}
	}
	if (kept) {
		mt_log("%s: TLB sync not confirmed: %u buffers stay wired and their memory is not reused\n", c->name, kept);
	}
	mt_log("%s: client closed (%u buffers; GPU pool: %llu MiB held, peak %llu MiB)\n", c->name, live,
	    (unsigned long long)(c->pool_bytes >> 20), (unsigned long long)(c->pool_peak >> 20));
	andr_mtl_os_bo_table_free(c->bo, c->bo_cap);
	c->bo = NULL;
	c->bo_cap = 0;
	c->open = 0;
}

void *
andr_mtl_cpu_memory(struct andr_mtl_client *c, uint32_t token)
{
	void *md = NULL;
	MT_LOCK();
	struct andr_mtl_bo *bo = c->open ? mt_bo(c, token) : NULL;
	if (bo != NULL && !bo->imported) {
		md = bo->md;
		andr_mtl_os_retain(md);
	}
	MT_UNLOCK();
	return md;
}

/* ---- the ABI methods (flame v2Method) ---- */

struct mt_bo_pa {
	const struct andr_mtl_bo *bo;
	bool bad;
};

static uint64_t
mt_alloc_pa(void *arg, uint64_t offset)
{
	return ((const struct mt_bo_pa *)arg)->bo->pool_pa + offset;
}

/* An import's 4K piece: ordinary XNU RAM only, never the GPU pools (or MMIO). */
static uint64_t
mt_import_pa(void *arg, uint64_t offset)
{
	struct mt_bo_pa *p = (struct mt_bo_pa *)arg;
	uint64_t pa = andr_mtl_os_import_pa(p->bo->md, offset);
	if (!pa || (pa & 0xfff) || !pmap_valid_address(pa) || mt_in_pool(pa, 0x1000) ||
	    mt_overlap(pa, 0x1000, mt_gpool_lo, mt_gpool_hi - mt_gpool_lo)) {
		if (!p->bad) {
			mt_log("map: imported page at offset 0x%llx is 0x%llx: not ordinary XNU RAM, refused\n",
			    (unsigned long long)offset, (unsigned long long)pa);
		}
		p->bad = true;
		return 0;
	}
	return pa;
}

#define MT_IN(T, v)							\
	T v;								\
	if (in == NULL || in_size != sizeof(T)) {			\
		return ANDR_IOR_BAD_ARGUMENT;				\
	}								\
	memcpy(&v, in, sizeof(T))
#define MT_OUT(T, v)							\
	T *v = (T *)out;						\
	if (out == NULL || *out_size < sizeof(T)) {			\
		return ANDR_IOR_BAD_ARGUMENT;				\
	}								\
	memset(v, 0, sizeof(T));					\
	*out_size = (uint32_t)sizeof(T)

static int
mt_bo_alloc(struct andr_mtl_client *c, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	MT_IN(andr_mtl_bo_alloc_t, rq);
	MT_OUT(andr_mtl_handle_t, rp);
	if (rq.bytes == 0 || (rq.bytes & 4095) || rq.bytes > MT_MAX_BO || rq.align_log2 < 12 || rq.align_log2 > 24 ||
	    rq.contiguous > 1) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	/* Memory XNU does not manage (no cacheable alias), zeroed through the pool's WC view; always contiguous. */
	andr_mtl_os_sweep();
	uint64_t len = mt_round(rq.bytes), pa = 0;
	MT_LOCK();
	int rc = mt_get_state() != MT_READY ? ANDR_IOR_NOT_READY :
	    mt_pool_alloc_locked(len, &pa) ? ANDR_IOR_NO_MEMORY : ANDR_IOR_SUCCESS;
	MT_UNLOCK();
	if (rc) {
		if (rc == ANDR_IOR_NO_MEMORY) {
			mt_log("%s: pool: no %llu KiB free (%llu MiB in use)\n", c->name, (unsigned long long)(len >> 10),
			    (unsigned long long)(mt_used >> 20));
		}
		return rc;
	}
	memset((void *)(mt_kva + (uintptr_t)(pa - mt_pa)), 0, (size_t)len);
	mt_dsb();
	void *md = andr_mtl_os_phys(pa, len);
	if (md == NULL) {
		andr_mtl_pool_free(pa, len);
		return ANDR_IOR_NO_MEMORY;
	}
	uint64_t h = 0;
	MT_LOCK();
	rc = mt_bo_slot(c, &h);
	if (rc == ANDR_IOR_SUCCESS) {
		struct andr_mtl_bo *bo = &c->bo[h - 1];
		memset(bo, 0, sizeof(*bo));
		bo->md = md;
		bo->bytes = rq.bytes;
		bo->len = len;
		bo->pool_pa = pa;
		c->bo_live++;
		c->pool_bytes += len;
		if (c->pool_bytes > c->pool_peak) {
			c->pool_peak = c->pool_bytes;
		}
	}
	MT_UNLOCK();
	if (rc != ANDR_IOR_SUCCESS) {
		andr_mtl_os_phys_release(md, pa, len);
		return rc;
	}
	rp->handle = h;
	return ANDR_IOR_SUCCESS;
}

static int
mt_bo_import(struct andr_mtl_client *c, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	MT_IN(andr_mtl_bo_import_t, rq);
	MT_OUT(andr_mtl_handle_t, rp);
	if (rq.address == 0 || ((rq.address | rq.bytes) & 4095) || rq.bytes == 0 || rq.bytes > MT_MAX_BO ||
	    rq.address + rq.bytes < rq.address) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	if (!andromeda_mtl_usable()) {
		return ANDR_IOR_NOT_READY;
	}
	bool writable = false;
	void *md = andr_mtl_os_import(c->task, rq.address, rq.bytes, &writable);
	if (md == NULL) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	uint64_t h = 0;
	MT_LOCK();
	int rc = mt_bo_slot(c, &h);
	if (rc == ANDR_IOR_SUCCESS) {
		struct andr_mtl_bo *bo = &c->bo[h - 1];
		memset(bo, 0, sizeof(*bo));
		bo->md = md;
		bo->bytes = rq.bytes;
		bo->len = rq.bytes;
		bo->imported = 1;
		bo->writable = writable;
		c->bo_live++;
	}
	MT_UNLOCK();
	if (rc != ANDR_IOR_SUCCESS) {
		andr_mtl_os_import_release(md);
		return rc;
	}
	rp->handle = h;
	return ANDR_IOR_SUCCESS;
}

static int
mt_bo_map(struct andr_mtl_client *c, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	MT_IN(andr_mtl_bo_map_t, rq);
	MT_OUT(andr_mtl_bo_map_reply_t, rp);
	/* writeback 1 (cached coherent) is not offered: GetDeviceInfo has no NativeAdrenoDeviceCachedCoherent. */
	if (!(rq.permissions & 1u) || (rq.permissions & ~3u) || (rq.iova & 4095) || rq.reserved || rq.writeback) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	MT_LOCK();
	struct andr_mtl_bo *bo = mt_bo(c, rq.handle);
	int rc = ANDR_IOR_SUCCESS;
	if (mt_get_state() != MT_READY) {
		rc = ANDR_IOR_NOT_READY;
	} else if (bo == NULL) {
		rc = ANDR_IOR_BAD_ARGUMENT;
	} else if (bo->iova != 0) {
		rc = ANDR_IOR_BUSY;
	} else if (rq.iova < c->win_base || bo->bytes > c->win_size || rq.iova - c->win_base > c->win_size - bo->bytes) {
		rc = ANDR_IOR_BAD_ARGUMENT;
	}
	if (rc == ANDR_IOR_SUCCESS) {
		struct mt_bo_pa arg = { bo, false };
		bool rw = (rq.permissions & 2u) && (!bo->imported || bo->writable);
		if (mt_map(rq.iova, bo->bytes >> 12, bo->imported ? mt_import_pa : mt_alloc_pa, &arg,
		    rw ? MT_PTE_RW : MT_PTE_RO)) {
			mt_unmap(rq.iova, bo->bytes);
			rc = arg.bad ? ANDR_IOR_VM_ERROR : ANDR_IOR_NO_RESOURCES;
		}
		if (mt_tlb_sync_locked() && rc == ANDR_IOR_SUCCESS) {
			/* the GPU may hold these translations: unmapped, and the memory is never reused */
			mt_unmap(rq.iova, bo->bytes);
			bo->kept = 1;
			rc = ANDR_IOR_NOT_READY;
		}
		if (rc == ANDR_IOR_SUCCESS) {
			bo->iova = rq.iova;
			bo->gpu_rw = rw;
			rp->iova = rq.iova;
		}
	}
	MT_UNLOCK();
	return rc;
}

static int
mt_bo_free(struct andr_mtl_client *c, const void *in, uint32_t in_size)
{
	MT_IN(andr_mtl_handle_t, rq);
	struct andr_mtl_bo copy;
	memset(&copy, 0, sizeof(copy));
	MT_LOCK();
	struct andr_mtl_bo *bo = mt_bo(c, rq.handle);
	int sync = 0;
	if (bo != NULL) {
		copy = *bo;
		if (bo->iova) {
			mt_unmap(bo->iova, bo->bytes);
			sync = mt_tlb_sync_locked();
		}
		memset(bo, 0, sizeof(*bo));
		c->bo_live--;
		if (!copy.imported) {
			c->pool_bytes -= copy.len;
		}
	}
	MT_UNLOCK();
	if (bo == NULL) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	if (sync == 0 && !copy.kept) {
		mt_bo_release(&copy);
	} else {
		mt_log("%s: buffer %llu stays wired (TLB sync not confirmed)\n", c->name, (unsigned long long)rq.handle);
	}
	return ANDR_IOR_SUCCESS;
}

static int
mt_bo_cpu_map(struct andr_mtl_client *c, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	MT_IN(andr_mtl_handle_t, rq);
	MT_OUT(andr_mtl_cpu_map_reply_t, rp);
	MT_LOCK();
	struct andr_mtl_bo *bo = mt_bo(c, rq.handle);
	int rc = bo == NULL || bo->imported ? ANDR_IOR_BAD_ARGUMENT : ANDR_IOR_SUCCESS;
	if (rc == ANDR_IOR_SUCCESS) {
		rp->token = (uint32_t)rq.handle;	/* clientMemoryForType type */
		rp->bytes = bo->len;
	}
	MT_UNLOCK();
	return rc;
}

static int
mt_submit(struct andr_mtl_client *c, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	MT_IN(andr_mtl_submit_t, rq);
	MT_OUT(andr_mtl_u64_t, rp);
	if (rq.buffer_count == 0 || rq.buffer_count > ANDR_MTL_MAX_BUFFERS || rq.reserved) {
		return ANDR_IOR_BAD_ARGUMENT;
	}
	if (rq.wait_fence != 0) {
		uint32_t wf = (uint32_t)rq.wait_fence;
		if (rq.wait_fence > 0xffffffffull || (int32_t)(wf - __atomic_load_n(&mt_g->fence, __ATOMIC_ACQUIRE)) > 0) {
			return ANDR_IOR_BAD_ARGUMENT;
		}
		int w = mt_wait(wf, rq.wait_timeout_ns);
		if (w) {
			return w > 0 ? ANDR_IOR_TIMEOUT : ANDR_IOR_NOT_READY;
		}
	}
	uint64_t iova[ANDR_MTL_MAX_BUFFERS];
	uint32_t dw[ANDR_MTL_MAX_BUFFERS];
	MT_LOCK();
	int rc = ANDR_IOR_SUCCESS;
	/* Each IB inside a BO of this client, mapped at that address (flame: inside the window). */
	for (uint32_t i = 0; i < rq.buffer_count && rc == ANDR_IOR_SUCCESS; i++) {
		const andr_mtl_submit_buffer_t *b = &rq.buffers[i];
		const struct andr_mtl_bo *bo = mt_bo(c, b->handle);
		if ((b->bytes & 3) || b->bytes == 0 || b->bytes / 4 > 0xfffff || bo == NULL || bo->iova == 0 ||
		    b->iova < bo->iova || b->bytes > bo->bytes || b->iova - bo->iova > bo->bytes - b->bytes) {
			rc = ANDR_IOR_BAD_ARGUMENT;
			break;
		}
		iova[i] = b->iova;
		dw[i] = (uint32_t)(b->bytes / 4);
	}
	uint32_t fence = 0;
	if (rc == ANDR_IOR_SUCCESS && (mt_get_state() != MT_READY || !mt_thermal_ok_locked())) {
		rc = ANDR_IOR_NOT_READY;
	}
	if (rc == ANDR_IOR_SUCCESS) {
		bool busy = false;
		fence = mt_submit_locked(iova, dw, rq.buffer_count, &busy);
		if (fence == 0) {
			rc = busy ? ANDR_IOR_BUSY : ANDR_IOR_NOT_READY;
		} else {
			c->last_fence = fence;
		}
	}
	MT_UNLOCK();
	if (rc != ANDR_IOR_SUCCESS) {
		return rc;
	}
	rp->value = fence;
	return ANDR_IOR_SUCCESS;
}

static int
mt_wait_fence(const void *in, uint32_t in_size)
{
	MT_IN(andr_mtl_wait_fence_t, rq);
	if (rq.fence == 0) {
		return ANDR_IOR_SUCCESS;
	}
	uint32_t f = (uint32_t)rq.fence;
	if (rq.fence > 0xffffffffull || mt_g == NULL ||
	    (int32_t)(f - __atomic_load_n(&mt_g->fence, __ATOMIC_ACQUIRE)) > 0) {
		return ANDR_IOR_BAD_ARGUMENT;	/* never submitted: it would never land */
	}
	int w = mt_wait(f, rq.timeout_ns);
	return w == 0 ? ANDR_IOR_SUCCESS : w > 0 ? ANDR_IOR_TIMEOUT : ANDR_IOR_NOT_READY;
}

static uint64_t
mt_timestamp(void)
{
	if (mt_g == NULL || !andr_gx_up()) {
		return 0;
	}
	uint32_t hi = andr_rd(ANDR_GREG(MT_ALWAYS_ON_HI)), lo = andr_rd(ANDR_GREG(MT_ALWAYS_ON_LO));
	uint32_t hi2 = andr_rd(ANDR_GREG(MT_ALWAYS_ON_HI));
	if (hi2 != hi) {
		lo = andr_rd(ANDR_GREG(MT_ALWAYS_ON_LO));
	}
	return ((uint64_t)hi2 << 32) | lo;
}

static int
mt_method(struct andr_mtl_client *c, uint32_t method, const void *in, uint32_t in_size, void *out, uint32_t *out_size)
{
	if (method < 32 && !(__atomic_fetch_or(&mt_logged, 1u << method, __ATOMIC_RELAXED) & (1u << method))) {
		mt_log("method %u first used (by %s, in %u bytes)\n", method, c->name, in_size);
	}
	switch (method) {
	case ANDR_MTL_M_QUERY: {
		MT_OUT(andr_mtl_params_t, rp);
		rp->abi_version = ANDR_MTL_ABI_VERSION;
		rp->input_bits = ANDR_MTL_INPUT_BITS;
		rp->output_bits = ANDR_MTL_OUTPUT_BITS;
		rp->flags = ANDR_MTL_PARAMS_HOST_IMPORT;
		rp->max_bo_bytes = MT_MAX_BO;
		return ANDR_IOR_SUCCESS;
	}
	case ANDR_MTL_M_GET_DEVICE_INFO: {
		MT_OUT(andr_mtl_device_info_t, rp);
		/* What andromeda_gpu_core.c's hw_init programs (as flame's): GMEM 1 MiB at 0x100000, UBWC 3, hbb 15. */
		rp->chip_id = (uint32_t)ANDR_CHIP_ID;
		rp->gmem_bytes = ANDR_MTL_GMEM_BYTES;
		rp->gmem_base = ANDR_MTL_GMEM_BASE;
		rp->uche_trap_base = ANDR_MTL_UCHE_TRAP;
		rp->va_start = c->win_base;
		rp->va_size = c->win_size;
		rp->highest_bank_bit = ANDR_MTL_HBB;
		rp->ubwc_swizzle = ANDR_MTL_UBWC_SWIZZLE;
		rp->macrotile_mode = 0;
		rp->ubwc_version = ANDR_MTL_UBWC_VERSION;
		rp->flags = ANDR_MTL_DEVICE_CLIENT_IOVA;
		return ANDR_IOR_SUCCESS;
	}
	case ANDR_MTL_M_BO_ALLOC:
		return mt_bo_alloc(c, in, in_size, out, out_size);
	case ANDR_MTL_M_BO_IMPORT:
		return mt_bo_import(c, in, in_size, out, out_size);
	case ANDR_MTL_M_BO_MAP:
		return mt_bo_map(c, in, in_size, out, out_size);
	case ANDR_MTL_M_BO_FREE:
		return mt_bo_free(c, in, in_size);
	case ANDR_MTL_M_BO_CPU_MAP:
		return mt_bo_cpu_map(c, in, in_size, out, out_size);
	case ANDR_MTL_M_BO_CPU_UNMAP:
		return ANDR_IOR_SUCCESS;	/* the mapping is the client's (IOConnectUnmapMemory64) */
	case ANDR_MTL_M_SUBMIT:
		return mt_submit(c, in, in_size, out, out_size);
	case ANDR_MTL_M_WAIT_FENCE:
		return mt_wait_fence(in, in_size);
	case ANDR_MTL_M_GET_TIMESTAMP: {
		MT_OUT(andr_mtl_u64_t, rp);
		rp->value = mt_timestamp();
		return ANDR_IOR_SUCCESS;
	}
	case ANDR_MTL_M_GET_STATUS: {
		MT_OUT(andr_mtl_status_t, rp);
		rp->faults = mt_get_state() == MT_STOPPED ? 1 : 0;	/* a change means device lost */
		return ANDR_IOR_SUCCESS;
	}
	case ANDR_MTL_M_SET_METADATA:
		return ANDR_IOR_SUCCESS;	/* only for dma-buf exports, which Darwin has not */
	case ANDR_MTL_M_GET_METADATA: {
		MT_OUT(andr_mtl_metadata_t, rp);
		return ANDR_IOR_SUCCESS;	/* bytes 0 */
	}
	case ANDR_MTL_M_CREATE_QUEUE: {
		MT_OUT(andr_mtl_u64_t, rp);
		rp->value = 1;			/* one ring: every queue is the same */
		return ANDR_IOR_SUCCESS;
	}
	case ANDR_MTL_M_CLOSE_QUEUE:
		return ANDR_IOR_SUCCESS;
	default:
		return ANDR_IOR_UNSUPPORTED;
	}
}

int
andr_mtl_external(struct andr_mtl_client *c, uint32_t selector, const void *in, uint32_t in_size,
    void *out, uint32_t *out_size)
{
	uint32_t none = 0;
	if (out_size == NULL || out == NULL) {
		out_size = &none;
		out = NULL;
	}
	if (!c->open) {
		return ANDR_IOR_NOT_READY;
	}
	if (in != NULL && in_size >= 8) {
		uint32_t w[2];
		memcpy(w, in, sizeof(w));
		if (w[0] == ANDR_MTL_TUNNEL) {
			if (w[1] == ANDR_MTL_PROBE) {
				*out_size = 0;
				return ANDR_IOR_SUCCESS;	/* the selector is admitted */
			}
			return mt_method(c, w[1], in_size > 8 ? (const uint8_t *)in + 8 : NULL, in_size - 8, out, out_size);
		}
	}
	return mt_method(c, selector, in_size ? in : NULL, in_size, out, out_size);
}
/*
 * ---- the framebuffer's CPU copy of Metal frames (andrfbsync, patch 0201) ----
 * With Metal on, backboardd's frames are drawn by the GPU into IOSurfaces and
 * swapped right after the submit; the framebuffer's CPU copy (0094) read them
 * without waiting for the GPU and through the kernel's cached mapping of the
 * surface (2026-11-07: wallpaper visible, many glitches). Before each copy:
 * wait for every submit issued so far (at most MT_FBSYNC_MS), then clean and
 * invalidate the frame's lines. ios-flame avoids the question by presenting
 * on the GPU ring after the frame (its 0119). andrfbsync=0 at boot or
 * "andctl.py fbsync 0" switches it off, "fbsync 1" on again.
 */
#define MT_FBSYNC_MS	150u	/* 50 ms timed out on 37 of 149 frames (2026-11-07) */
#if defined(KERNEL)
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
#endif
static uint32_t mt_fbsync = 1, mt_fbsync_parsed;
static uint64_t mt_fbsync_frames, mt_fbsync_timeouts;

static void
mt_fbsync_args(void)
{
	if (__atomic_exchange_n(&mt_fbsync_parsed, 1u, __ATOMIC_ACQ_REL) == 0) {
		uint32_t v = 1;
		(void)PE_parse_boot_argn("andrfbsync", &v, sizeof(v));
		__atomic_store_n(&mt_fbsync, v ? 1u : 0u, __ATOMIC_RELAXED);
	}
}

void
andromeda_fbsync_before_read(uintptr_t kva, uint64_t bytes)
{
	mt_fbsync_args();
	if (!__atomic_load_n(&mt_fbsync, __ATOMIC_RELAXED) || mt_get_state() != MT_READY || mt_g == NULL ||
	    kva == 0 || bytes == 0 || bytes > 0x10000000ull) {
		return;
	}
	uint32_t target = __atomic_load_n(&mt_g->fence, __ATOMIC_ACQUIRE);
	int rc = mt_wait(target, (uint64_t)MT_FBSYNC_MS * 1000000ull);
	uint64_t n = __atomic_add_fetch(&mt_fbsync_frames, 1, __ATOMIC_RELAXED);
	if (rc == 1) {
		uint64_t t = __atomic_add_fetch(&mt_fbsync_timeouts, 1, __ATOMIC_RELAXED);
		if (t <= 5 || (t % 1000) == 0) {
			printf(MT_TAG " fbsync: GPU not done after %u ms (fence %u, done %u); %llu of %llu frames\n",
			    MT_FBSYNC_MS, target, mt_fence_read(), (unsigned long long)t, (unsigned long long)n);
		}
	}
#if defined(KERNEL)
	flush_dcache64((addr64_t)kva, (unsigned)bytes, FALSE);
#endif
	if (n == 1) {
		printf(MT_TAG " fbsync: the CPU copy waits for the GPU and cleans each frame first"
		    " (andrfbsync=0 or \"andctl fbsync 0\" turns it off)\n");
	}
}

int
andromeda_fbsync_ctl(unsigned on, char *why, size_t len)
{
	mt_fbsync_args();
	__atomic_store_n(&mt_fbsync, on ? 1u : 0u, __ATOMIC_RELAXED);
	snprintf(why, len, "fbsync %s (%llu frames synced so far, %llu GPU waits timed out)", on ? "on" : "off",
	    (unsigned long long)__atomic_load_n(&mt_fbsync_frames, __ATOMIC_RELAXED),
	    (unsigned long long)__atomic_load_n(&mt_fbsync_timeouts, __ATOMIC_RELAXED));
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
