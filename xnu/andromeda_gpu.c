/*
 * Adreno 640 on andromeda: the kernel environment for the bring-up sequence
 * in andromeda_gpu_core.c, the gated bring-up thread, and andromeda_gpu_init.
 *
 * Ported from ios-flame's bsd/dev/sweet_gpu.c (Nick Wylynko / NWylynko,
 * https://github.com/NWylynko/ios-flame, patch 0095-flame-gpu-present; MIT,
 * patched XNU files APSL 2.0). The SMC quirk-resume loop is from flame's
 * sweet_pmem.c (patch 0094, following Linux __scm_smc_do_quirk). The ADT
 * "gpufw,<name>" firmware convention is flame's (tools/mkpayload.py --gpufw);
 * here the files arrive through scripts/make-andromeda-adt.py --gpufw. The
 * SHA-256 firmware identity check and the TSENS thermal guard follow the Mi 9
 * port (YangQi0408 / UniXNU-cepheus: cepheus_gpu_firmware.h, cepheus_gpu_tsens.h)
 * and this repo's andromeda_cpufreq.c. ios-sweet is dipa-studio's; its patch
 * 0023 makes libkern's SHA-256 work in-kernel on SWEET, which this uses. The
 * expected firmware digests are docs/research/gpu.md 6.3b. See CREDITS.md.
 *
 * Boot-args:
 *   andrgpu=N          0/absent: nothing touched. Else N = highest step to run
 *                      (1 probe, 2 CX+GMU, 3 GX+CP+ZAP, 4 CP_MEM_WRITE).
 *   andrgpupool=PA     physical base of the GPU pool, in RAM XNU does not
 *                      manage. Default 0x1FF000000: the top 16 MiB of the
 *                      6 GiB DRAM. DarwinBoot places the kernel window at
 *                      ALIGN(start of the free run, 32 MiB) + 0x1004000
 *                      (0x181004000 with the stock Aloha map, 0x141004000 with
 *                      patches/aloha-andromeda-ram-merge.patch) and the window
 *                      ends below 0x1FF000000 for owned_memory_size up to
 *                      0xBD000000. Refused if any part of it turns out
 *                      XNU-managed (fail-safe, via pmap_valid_address).
 *   andrgpupoolsize=B  pool size (default 0x1000000, 16 MiB).
 *   andrgpudelay=S     seconds before the thread starts (default 90), so the
 *                      log path and the rest of the system are up first.
 *   andrgpuhot=C       refuse to power the GPU at/above C degC (default 80);
 *                      an unreadable temperature also refuses (fail-safe).
 *   andrgpupresent=1   phase F4 (andromeda_gpu_present.c, patch 0120): after
 *                      a step 4 PASS the GPU is handed to the framebuffer,
 *                      which lets it copy the fitted frame (sweetfb_fit=1).
 *   andrmtl=1|2        phases F5/F6 (andromeda_gpu_mtl.c, patch 0200): after
 *                      a step 4 PASS the GPU goes to the NativeAdreno user
 *                      client (1) and the Metal plug-in (2) instead; the
 *                      present path is then declined (one ring user per
 *                      boot) and the CPU copy shows the frames. If Metal
 *                      cannot set up, andrgpupresent applies as without it.
 *
 * Runtime control (andromeda_ctl.c, andrctl=1, command "gpu N"): the same
 * steps, started from the PC. A boot-arg run still in its delay starts at
 * once; after a run that passed step K, "gpu N" runs steps K+1..N only (a
 * step never runs twice); after a failed step nothing runs again this boot.
 * The thermal checks are those of the boot path. Step 3 (ZAP) goes through
 * TrustZone PAS, as the modem boot of andrwlan=2: the first of the two to
 * ask owns PAS for the boot (andromeda_pas_claim) and the other is refused.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#if defined(KERNEL)
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <libkern/crypto/sha2.h>
#include <mach/mach_time.h>
#include <vm/pmap.h>		/* VM_WIMG_WCOMB */
#else	/* host syntax check: stubinc plus the kernel-only declarations */
#define VM_WIMG_WCOMB 6
typedef unsigned int boolean_t;
typedef const void *DTEntry;
#define kSuccess 0
int SecureDTLookupEntry(const void *, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, void const **, unsigned int *);
typedef struct { unsigned char opaque[128]; } SHA256_CTX;
void SHA256_Init(SHA256_CTX *);
void SHA256_Update(SHA256_CTX *, const void *, size_t);
void SHA256_Final(void *, SHA256_CTX *);
int vprintf(const char *, va_list);
#endif
#include "andromeda_gpu.h"
#include "andromeda_gpu_present.h"
#include "andromeda_gpu_mtl.h"

#define AG_TAG "ANDROMEDA GPU:"

extern vm_offset_t andromeda_io_map(vm_offset_t, vm_size_t); /* andromeda_probe.c: shared, outside io_submap */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern boolean_t pmap_valid_address(uint64_t);
extern void _disable_preemption(void);
extern void _enable_preemption(void);
/* andromeda_ctl.c: runtime control and request state (andromeda_ctl.h). */
#include "andromeda_ctl.h"
extern void andromeda_ctl_delay(uint32_t *st, unsigned seconds);
extern bool andromeda_ctl_is_boot(uint32_t *st);
extern uint32_t andromeda_ctl_begin(uint32_t *st, const char *who);
extern void andromeda_ctl_end(uint32_t *st, unsigned reached, bool failed, const char *who);
extern bool andromeda_ctl_spawn(thread_continue_t fn);
extern bool andromeda_pas_claim(unsigned who);
#define AG_PAS_OWNER 1u		/* andromeda_ctl.c ANDR_PAS_GPU */
#if defined(KERNEL)
extern int vprintf(const char *, va_list);
#endif

/* The whole 6 GiB DRAM the Aloha UEFI map exposes (board.toml). This build
 * has no PStore (USE_MEMORY_FOR_SERIAL_OUTPUT = 0); the check below is kept
 * as a harmless guard for builds that enable it. */
#define AG_DRAM_BASE	0xc0000000ull
#define AG_DRAM_END	0x200000000ull
#define AG_PSTORE_BASE	0x17fe00000ull
#define AG_PSTORE_END	0x180000000ull
#define AG_POOL_DEFAULT	0x1ff000000ull	/* top 16 MiB of DRAM, above any kernel window */

/* ---- firmware identity (docs/research/gpu.md 6.3b) ---- */

struct fw_id {
	const char *name;
	uint32_t size;
	uint8_t sha256[32];
};

static const struct fw_id fw_table[ANDR_FW_COUNT] = {
	[ANDR_FW_GMU] = { "a640_gmu.bin", 37680, {
	    0x3f,0xf0,0xc0,0x27,0x08,0xbb,0xe7,0x86,0x41,0xdb,0x88,0x7f,0xa6,0x2f,0x3a,0x7f,
	    0x93,0x37,0x93,0x4d,0x0c,0x2c,0xe0,0xb9,0x61,0xef,0x7c,0x43,0x17,0x25,0x91,0xd2 } },
	[ANDR_FW_SQE] = { "a630_sqe.fw", 32136, {
	    0x36,0x29,0x8b,0xd5,0x3e,0xa3,0x66,0x64,0x13,0xd2,0x88,0xa3,0xa3,0xf5,0x26,0x6f,
	    0x67,0xfc,0xb6,0xdb,0x03,0x8a,0x0a,0x48,0x1a,0xf5,0x86,0x9d,0x1c,0x17,0x85,0x9c } },
	[ANDR_FW_ZAP_MDT] = { "a640_zap.mdt", 6860, {
	    0xed,0x67,0x38,0x82,0x8c,0xce,0xae,0xbf,0x4d,0x69,0x44,0x6e,0x34,0xb2,0x04,0x3b,
	    0x1c,0x51,0x00,0x27,0x50,0x60,0x2d,0x3e,0x7e,0x40,0xbf,0x21,0x8e,0xc3,0xd5,0x9a } },
	[ANDR_FW_ZAP_B02] = { "a640_zap.b02", 1968, {
	    0xe2,0x4a,0x4f,0x48,0x3b,0x71,0xea,0x19,0x71,0x59,0x10,0xf0,0x1e,0x1a,0xbf,0x94,
	    0x28,0x07,0x56,0xdf,0x73,0x1c,0xad,0x05,0xdf,0x48,0xfd,0x77,0x14,0xe3,0x41,0x98 } },
};

static void
ag_sha256(const uint8_t *data, size_t size, uint8_t out[32])
{
	SHA256_CTX ctx;
	SHA256_Init(&ctx);
	SHA256_Update(&ctx, data, size);
	SHA256_Final(out, &ctx);
}

const uint8_t *
andr_firmware(enum andr_fw fw, uint32_t *len)
{
	if (fw >= ANDR_FW_COUNT) {
		return NULL;
	}
	const struct fw_id *id = &fw_table[fw];
	DTEntry chosen;
	void const *data = NULL;
	unsigned int size = 0;
	char prop[40];
	snprintf(prop, sizeof(prop), "gpufw,%s", id->name);
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, prop, &data, &size) != kSuccess || !data) {
		printf(AG_TAG " firmware %s not in the ADT (make-andromeda-adt.py --gpufw)\n", id->name);
		return NULL;
	}
	if (size != id->size) {
		printf(AG_TAG " firmware %s size %u, expected %u\n", id->name, size, id->size);
		return NULL;
	}
	uint8_t got[32];
	ag_sha256((const uint8_t *)data, size, got);
	unsigned diff = 0;
	for (unsigned i = 0; i < 32; i++) {
		diff |= (unsigned)(got[i] ^ id->sha256[i]);
	}
	if (diff) {
		printf(AG_TAG " firmware %s SHA-256 mismatch\n", id->name);
		return NULL;
	}
	if (len) {
		*len = size;
	}
	return (const uint8_t *)data;
}

/* ---- MMIO windows (2 MiB, kept; only a handful of register blocks) ---- */

#define GPU_WIN		0x200000ull
#define GPU_MAPS	24

static struct {
	vm_offset_t va;
	uint64_t win;
} gpu_map[GPU_MAPS];

static volatile uint32_t *
gpu_io(uint64_t pa)
{
	uint64_t win = pa & ~(GPU_WIN - 1);
	for (unsigned i = 0; i < GPU_MAPS; i++) {
		if (gpu_map[i].va && gpu_map[i].win == win) {
			return (volatile uint32_t *)(gpu_map[i].va + (pa - win));
		}
		if (!gpu_map[i].va) {
			gpu_map[i].va = andromeda_io_map((vm_offset_t)win, GPU_WIN);
			gpu_map[i].win = win;
			return gpu_map[i].va ? (volatile uint32_t *)(gpu_map[i].va + (pa - win)) : NULL;
		}
	}
	printf(AG_TAG " out of device windows at 0x%llx\n", (unsigned long long)pa);
	return NULL;
}

/* ---- pool: RAM XNU does not manage, mapped write-combining ---- */

static vm_offset_t pool_kva;
static uint64_t pool_pa, pool_size;

static bool
in_pool(uint64_t pa, size_t len)
{
	return pool_kva && pa >= pool_pa && pa + len <= pool_pa + pool_size;
}

uint32_t
andr_rd(uint64_t pa)
{
	if (in_pool(pa, 4)) {
		uint32_t v = *(volatile uint32_t *)(pool_kva + (pa - pool_pa));
		__asm__ volatile("dmb osh" ::: "memory");
		return v;
	}
	volatile uint32_t *p = gpu_io(pa);
	if (!p) {
		return 0xffffffffu;
	}
	uint32_t v = *p;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

void
andr_wr(uint64_t pa, uint32_t v)
{
	if (in_pool(pa, 4)) {
		*(volatile uint32_t *)(pool_kva + (pa - pool_pa)) = v;
		__asm__ volatile("dsb osh" ::: "memory");
		return;
	}
	volatile uint32_t *p = gpu_io(pa);
	if (!p) {
		return;
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*p = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

int
andr_write(uint64_t pa, const void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		memcpy((void *)(pool_kva + (pa - pool_pa)), buf, len);
		__asm__ volatile("dsb osh" ::: "memory");
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
	if (in_pool(pa, len)) {
		__asm__ volatile("dmb osh" ::: "memory");
		memcpy(buf, (const void *)(pool_kva + (pa - pool_pa)), len);
		return 0;
	}
	uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		w[i] = andr_rd(pa + 4 * i);
	}
	return 0;
}

void
andr_delay_ms(unsigned ms)
{
	delay((int)ms * 1000);
}

void
andr_log(const char *fmt, ...)
{
	va_list ap;
	printf(AG_TAG " ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---- TrustZone SMC (flame sweet_pmem.c / Linux __scm_smc_do_quirk) ---- */

#define QCOM_SCM_INTERRUPTED	1

static void
smc_once(uint64_t x[8])
{
	register uint64_t x0 __asm__("x0") = x[0];
	register uint64_t x1 __asm__("x1") = x[1];
	register uint64_t x2 __asm__("x2") = x[2];
	register uint64_t x3 __asm__("x3") = x[3];
	register uint64_t x4 __asm__("x4") = x[4];
	register uint64_t x5 __asm__("x5") = x[5];
	register uint64_t x6 __asm__("x6") = x[6];
	register uint64_t x7 __asm__("x7") = x[7];
	__asm__ volatile("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
	    :
	    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	x[0] = x0;
	x[1] = x1;
	x[2] = x2;
	x[3] = x3;
	x[6] = x6;
}

void
andr_smc(uint64_t x[8])
{
	uint64_t work[8];
	unsigned resumes = 0;
	memcpy(work, x, sizeof(work));
	_disable_preemption();
	for (;;) {
		smc_once(work);
		if (work[0] != QCOM_SCM_INTERRUPTED || resumes >= 100000) {
			break;
		}
		uint64_t a6 = work[6];
		memcpy(work, x, sizeof(work));
		work[0] = QCOM_SCM_INTERRUPTED;
		work[6] = a6;
		resumes++;
	}
	_enable_preemption();
	memcpy(x, work, 4 * sizeof(uint64_t));
	if (resumes) {
		printf(AG_TAG " smc resumed %u times -> %llx\n", resumes, (unsigned long long)work[0]);
	}
}

/* ---- GPU-island temperature (TSENS, same decode as andromeda_cpufreq.c) ---- */

bool
andr_gpu_temperature(int *max_mc)
{
	static vm_offset_t tm[2];
	static const uint64_t pa[2] = { ANDR_TSENS0_TM_PA, ANDR_TSENS1_TM_PA };
	static const unsigned sensor[2] = { ANDR_TSENS0_GPU_SENSOR, ANDR_TSENS1_GPU_SENSOR };
	bool any = false;
	int max = -40000;
	for (unsigned t = 0; t < 2; t++) {
		if (!tm[t]) {
			tm[t] = andromeda_io_map((vm_offset_t)pa[t], 0x1000);
		}
		if (!tm[t]) {
			continue;
		}
		uint32_t ready = *(volatile uint32_t *)(tm[t] + 0xe4);
		__asm__ volatile("dmb oshld" ::: "memory");
		if (ready == 0xffffffffu || !(ready & 8u)) {
			continue;
		}
		uint32_t status = *(volatile uint32_t *)(tm[t] + 0xa0u + 4u * sensor[t]);
		__asm__ volatile("dmb oshld" ::: "memory");
		if (status == 0xffffffffu || !(status & (1u << 21))) {
			continue;	/* no valid sample */
		}
		int code = (int)(status & 0xfff);
		if (code & 0x800) {
			code -= 4096;
		}
		int mc = code * 100;
		if (mc < -40000 || mc > 125000) {
			continue;
		}
		any = true;
		if (mc > max) {
			max = mc;
		}
	}
	*max_mc = max;
	return any;
}

/* ---- gated bring-up ---- */

static struct andr_gpu gpu;
static unsigned ag_step, ag_hot_c = 80;
static unsigned ag_level = 1;	/* andrgpulevel: GMU perf vote, 1 (257 MHz) .. 5 (585 MHz) */
static uint64_t ag_pool_arg = AG_POOL_DEFAULT, ag_pool_size = ANDR_POOL_SIZE;
static unsigned ag_delay_s = 90;

/* true if the GPU may be powered: temperature readable and below the limit. */
static bool
ag_thermal_ok(const char *why)
{
	int mc = 0;
	bool valid = andr_gpu_temperature(&mc);
	bool ok = valid && mc < (int)ag_hot_c * 1000;
	printf(AG_TAG " temperature %s %d mC (limit %u C): %s to power the GPU (%s)\n",
	    valid ? "max" : "UNREADABLE (fail-safe)", mc, ag_hot_c, ok ? "ok" : "REFUSE", why);
	return ok;
}

/* Map the pool from RAM XNU does not manage. Returns 0 and sets pool_* on
 * success. pmap_valid_address is true only for XNU-managed physical pages, so
 * it is the direct test that we are not about to scribble on kernel RAM. */
static int
ag_pool_map(void)
{
	uint64_t base = ag_pool_arg, size = ag_pool_size;
	if ((base & (ANDR_POOL_ALIGN - 1)) || !size || (size & 0xffffu)) {
		printf(AG_TAG " pool 0x%llx size 0x%llx: base must be 2 MiB aligned, size a page multiple\n",
		    (unsigned long long)base, (unsigned long long)size);
		return -1;
	}
	if (base < AG_DRAM_BASE || base + size > AG_DRAM_END) {
		printf(AG_TAG " pool 0x%llx..0x%llx outside DRAM 0x%llx..0x%llx\n",
		    (unsigned long long)base, (unsigned long long)(base + size),
		    (unsigned long long)AG_DRAM_BASE, (unsigned long long)AG_DRAM_END);
		return -1;
	}
	if (base < AG_PSTORE_END && base + size > AG_PSTORE_BASE) {
		printf(AG_TAG " pool 0x%llx..0x%llx overlaps PStore 0x%llx..0x%llx\n",
		    (unsigned long long)base, (unsigned long long)(base + size),
		    (unsigned long long)AG_PSTORE_BASE, (unsigned long long)AG_PSTORE_END);
		return -1;
	}
	/* Sample the range: refuse if any part is XNU-managed (fail-safe). */
	for (uint64_t o = 0; o < size; o += 0x4000ull) {
		if (pmap_valid_address(base + o)) {
			printf(AG_TAG " pool 0x%llx: page 0x%llx is XNU-managed, refusing\n",
			    (unsigned long long)base, (unsigned long long)(base + o));
			return -1;
		}
	}
	if (pmap_valid_address(base + size - 1)) {
		printf(AG_TAG " pool 0x%llx: top page is XNU-managed, refusing\n", (unsigned long long)base);
		return -1;
	}
	vm_offset_t va = ml_io_map_unmappable((vm_offset_t)base, (vm_size_t)size, VM_WIMG_WCOMB);
	if (!va) {
		printf(AG_TAG " pool 0x%llx: write-combining map failed\n", (unsigned long long)base);
		return -1;
	}
	pool_kva = va;
	pool_pa = base;
	pool_size = size;
	memset((void *)va, 0, (size_t)size);
	__asm__ volatile("dsb osh" ::: "memory");
	andr_gpu_state_init(&gpu, base);
	gpu.perf_level = ag_level;
	printf(AG_TAG " pool at pa 0x%llx size 0x%llx (unmanaged, write-combining)\n",
	    (unsigned long long)base, (unsigned long long)size);
	return 0;
}

uint32_t andromeda_gpu_req;	/* andromeda_ctl.h request state, levels = steps */
const char *andromeda_gpu_check(unsigned level);
bool andromeda_gpu_spawn(void);
void andromeda_gpu_init(void);

/*
 * Steps andr_st_from(run)..andr_st_level(run). A step that passed in an
 * earlier run (boot-arg or andrctl) is not run again; the state it left
 * (pool, CX, GMU) is still in gpu and the pool mapping.
 */
static void
ag_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	uint64_t t0, ns = 0;

	if (andromeda_ctl_is_boot(&andromeda_gpu_req) && ag_delay_s) {
		andromeda_ctl_delay(&andromeda_gpu_req, ag_delay_s);
	}
	uint32_t run = andromeda_ctl_begin(&andromeda_gpu_req, "gpu");
	unsigned from = andr_st_from(run), to = andr_st_level(run), reached = from - 1u;
	bool failed = false;
	int rc = 0;
	t0 = mach_absolute_time();
	if (from <= ANDR_STEP_PROBE) {
		printf(AG_TAG " andrgpu=%u: running gated bring-up steps\n", to);
	} else {
		printf(AG_TAG " andrgpu=%u: continuing with steps %u..%u (step %u passed earlier)\n",
		    to, from, to, from - 1u);
	}

	/* Step 1: read-only probe + firmware verification. No writes, no pool. */
	if (from <= ANDR_STEP_PROBE) {
		rc = andr_gpu_probe(&gpu);
		printf(AG_TAG " step 1 (probe) %s\n", rc ? "reported missing firmware/ids" : "ok");
		if (rc) {
			if (to >= ANDR_STEP_GMU) {
				printf(AG_TAG " not powering the GPU (thermal refuse or probe failed)\n");
			}
			failed = true;
			goto done;
		}
		reached = ANDR_STEP_PROBE;
		if (to < ANDR_STEP_GMU) {
			goto done;
		}
	}

	/* Step 2: CX power + GMU firmware boot (expect 0xBABEFACE). Everything
	 * from here writes to the GPU: honour the thermal limit first. */
	if (from <= ANDR_STEP_GMU) {
		if (!ag_thermal_ok("before power-up")) {
			printf(AG_TAG " not powering the GPU (thermal refuse or probe failed)\n");
			goto done;	/* nothing written: may be asked again when cooler */
		}
		if (!pool_kva && ag_pool_map()) {
			goto done;
		}
		if (andr_power_cx(&gpu) || andr_gmu_boot(&gpu)) {
			printf(AG_TAG " step 2 (CX + GMU boot) FAILED; stopping\n");
			failed = true;
			goto done;
		}
		printf(AG_TAG " step 2 (CX + GMU boot) ok\n");
		reached = ANDR_STEP_GMU;
		if (to < ANDR_STEP_CP) {
			goto done;
		}
	}

	/* Step 3: HFI tables + GX via OOB, GPU SMMU + hw_init + SQE + CP + ZAP. */
	if (from <= ANDR_STEP_CP) {
		if (!ag_thermal_ok("before GX/CP")) {
			printf(AG_TAG " step 3 (GX + CP + ZAP) not started (thermal refuse)\n");
			goto done;
		}
		if (andr_gmu_hfi(&gpu) || andr_gpu_cp(&gpu)) {
			printf(AG_TAG " step 3 (GX + CP + ZAP) FAILED; stopping\n");
			failed = true;
			goto done;
		}
		printf(AG_TAG " step 3 (GX + CP + ZAP) ok\n");
		reached = ANDR_STEP_CP;
		if (to < ANDR_STEP_MEMTEST) {
			goto done;
		}
	}

	/* Step 4: CP_MEM_WRITE 0xcafef00d and read it back. Continuing a run whose
	 * step 3 passed earlier: the thermal check of step 3 is done again here. */
	if (from > ANDR_STEP_CP && !ag_thermal_ok("before CP_MEM_WRITE")) {
		printf(AG_TAG " step 4 (CP_MEM_WRITE) not started (thermal refuse)\n");
		goto done;
	}
	rc = andr_cp_memtest(&gpu);
	printf(AG_TAG " step 4 (CP_MEM_WRITE) %s\n", rc ? "FAILED" : "PASS");
	if (rc) {
		failed = true;
	} else {
		reached = ANDR_STEP_MEMTEST;
		/*
		 * The ring has one user per boot; this thread does not touch the
		 * GPU again. F5/F6, off unless andrmtl=1|2 (patch 0200): Metal
		 * and Turnip get it, and a requested F4 present path is declined
		 * (the CPU copy shows the frames). Otherwise, or when Metal could
		 * not set up (no BO pool), F4 as before, off unless
		 * andrgpupresent=1: the framebuffer's present path gets it.
		 */
		if (andromeda_mtl_level() != 0 && andromeda_gpu_mtl_attach(&gpu, ag_pool_size)) {
			andromeda_gpu_present_decline("Metal (andrmtl) has the GPU ring");
		} else {
			andromeda_gpu_present_attach(&gpu, ag_pool_size);
		}
	}

done:
	andromeda_ctl_end(&andromeda_gpu_req, reached, failed, "gpu");
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf(AG_TAG " bring-up thread finished in %llu ms (steps up to %u passed%s)\n",
	    (unsigned long long)(ns / 1000000), reached, failed ? ", then a step FAILED" : "");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* Preconditions shared by andrgpu=N and "gpu N"; NULL if fine. */
const char *
andromeda_gpu_check(unsigned level)
{
	(void)level;
	if (ag_hot_c == 0 || ag_hot_c > 95) {
		return "andrgpuhot outside 1..95 C: refusing to power the GPU";
	}
	return NULL;
}

bool
andromeda_gpu_spawn(void)
{
	return andromeda_ctl_spawn(ag_thread);
}

void
andromeda_gpu_init(void)
{
	static bool started;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrgpu", &ag_step, sizeof(ag_step));
	(void)PE_parse_boot_argn("andrgpupool", &ag_pool_arg, sizeof(ag_pool_arg));
	(void)PE_parse_boot_argn("andrgpupoolsize", &ag_pool_size, sizeof(ag_pool_size));
	(void)PE_parse_boot_argn("andrgpudelay", &ag_delay_s, sizeof(ag_delay_s));
	(void)PE_parse_boot_argn("andrgpuhot", &ag_hot_c, sizeof(ag_hot_c));
	(void)PE_parse_boot_argn("andrgpulevel", &ag_level, sizeof(ag_level));
	if (ag_level < 1 || ag_level > 5) {
		printf(AG_TAG " andrgpulevel %u out of 1..5: using 1 (257 MHz)\n", ag_level);
		ag_level = 1;
	}
	if (!ag_step) {
		return;
	}
	if (ag_step > ANDR_STEP_MEMTEST) {
		ag_step = ANDR_STEP_MEMTEST;
	}
	if (ag_hot_c == 0 || ag_hot_c > 95) {
		printf(AG_TAG " refusing hot limit %u C (need 1..95)\n", ag_hot_c);
		return;
	}
	if (ag_step >= ANDR_STEP_CP && !andromeda_pas_claim(AG_PAS_OWNER)) {
		printf(AG_TAG " andrgpu=%u: TrustZone PAS (ZAP) is taken by the modem boot this boot;"
		    " not starting\n", ag_step);
		return;
	}
	printf(AG_TAG " andrgpu=%u pool=0x%llx size=0x%llx delay=%us: starting bring-up thread\n",
	    ag_step, (unsigned long long)ag_pool_arg, (unsigned long long)ag_pool_size, ag_delay_s);
	__atomic_store_n(&andromeda_gpu_req, andr_st(ANDR_PH_WAIT, ag_step, 0), __ATOMIC_RELEASE);
	if (!andromeda_gpu_spawn()) {
		__atomic_store_n(&andromeda_gpu_req, 0u, __ATOMIC_RELEASE);
		printf(AG_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
