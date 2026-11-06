/* SM8150 CPU/GPU bring-up diagnostics. Explicit boot opt-in, thread context.
 * CPU frequency table/requests/counters are observed by default. cephboost=1
 * opts into a bounded 120s request experiment on Gold/Prime; always restore.
 * GPUCC is gated by GCC CFG_AHB; GMU/GX are NOT poked if powered down.
 * No voltage writes, thermal override, GPU resets, DMA or firmware upload.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "cepheus_soc.h"
#include "cepheus_cpu_policy.h"
#include "cepheus_gpu_power.h"
#include "cepheus_gpu_memory.h"
#include "cepheus_gpu_bin.h"
extern void cepheus_gmu_workspace_selftest(void);

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
void cepheus_soc_init(void);
void cepheus_cpu_activity(void);
extern void cepheus_gpu_arc_probe(void);
static uint64_t activity_ticks;
static uint32_t gpu_fuse_value;
static unsigned gpu_fuse_valid;
int cepheus_gpu_fuse_sample(uint32_t *value);
/* Only returns the completed opt-in read below; never touches hardware itself
 * and never substitutes a default bin when sampling was disabled/failed. */
int cepheus_gpu_fuse_sample(uint32_t *value)
{
	if(!value || !__atomic_load_n(&gpu_fuse_valid,__ATOMIC_ACQUIRE))return -1;
	*value=gpu_fuse_value;return 0;
}
void cepheus_cpu_activity(void)
{
	__atomic_store_n(&activity_ticks, mach_absolute_time(), __ATOMIC_RELAXED);
}
static int soc_wait_chan;
static uint32_t sr(vm_offset_t base, unsigned off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}
static void swait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&soc_wait_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}
struct osm_bank { const char *name; uint64_t pa; unsigned cores; vm_offset_t va; bool enabled; };
static struct osm_bank banks[] = {
	{"l3", 0x18321000, 1, 0, false}, {"silver", 0x18323000, 4, 0, false},
	{"gold", 0x18325800, 3, 0, false}, {"prime", 0x18327800, 1, 0, false}
};
struct osm_trial { bool active; unsigned old_index, new_index; };
static struct osm_trial trials[4];
static void osm_request(struct osm_bank *b, unsigned index)
{
	// clk-cpu-osm.c osm_set_index(): PERF_STATE_DESIRED then ENABLE read.
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(b->va + 0x920) = index;
	__asm__ volatile("dsb osh" ::: "memory");
	(void)sr(b->va, 0);
}
static void osm_trial_start(void)
{
	int enable = 0;
	PE_parse_boot_argn("cephboost", &enable, sizeof(enable));
	if (enable != 1 && enable != 2) { return; }
	for (unsigned n = 2; n < 4; ++n) {
		struct osm_bank *b = &banks[n];
		if (!b->enabled || sr(b->va, 0xbc) != 0) { continue; }
		unsigned current = sr(b->va, 0x920);
		uint32_t freq[CEPHEUS_OSM_ROWS], volt[CEPHEUS_OSM_ROWS];
		for (unsigned i = 0; i < CEPHEUS_OSM_ROWS; ++i) {
			freq[i] = sr(b->va, 0x110 + i * 32);
			volt[i] = sr(b->va, 0x114 + i * 32);
		}
		unsigned target = cepheus_osm_same_voltage(freq, volt, CEPHEUS_OSM_ROWS, current);
		if (target == current || sr(b->va, 0x920) != current) { continue; }
		printf("CEPHEUS OSM: trial %s old=%u/%u new=%u/%u mv=%u old_corner=%u new_corner=%u duration=120s\n",
		    b->name, current, cepheus_osm_khz(freq[current]), target, cepheus_osm_khz(freq[target]),
		    volt[target] & 0xfffu, (volt[current] >> 16) & 63u, (volt[target] >> 16) & 63u);
		trials[n] = (struct osm_trial){true, current, target};
		osm_request(b, target);
		printf("CEPHEUS OSM: trial %s readback=%u\n", b->name, sr(b->va, 0x920));
	}
}
static vm_offset_t tsens;
static int cpu_temperature(int *max_mc)
{
	if (!tsens) { tsens = ml_io_map(0x0c263000, 0x1000); }
	if (!tsens || !(sr(tsens, 0xe4) & 8u)) { return 0; }
	int max = -40000;
	for (unsigned sensor = 7; sensor <= 14; ++sensor) {
		int mc = 0, valid = 0;
		for (unsigned tries = 0; tries < 3 && !valid; ++tries) {
			valid = ceph_tsens_mc(sr(tsens, 0xa0 + sensor * 4), &mc);
		}
		if (!valid) { return 0; }
		if (mc > max) { max = mc; }
	}
	*max_mc = max;
	return 1;
}
static void osm_trial_restore(void)
{
	for (unsigned n = 2; n < 4; ++n) {
		if (!trials[n].active) { continue; }
		struct osm_bank *b = &banks[n];
		unsigned current = sr(b->va, 0x920);
		if (current == trials[n].new_index) {
			osm_request(b, trials[n].old_index);
			printf("CEPHEUS OSM: restored %s desired=%u\n", b->name, sr(b->va, 0x920));
		} else {
			printf("CEPHEUS OSM: %s external desired=%u; not overwriting another owner's request\n", b->name, current);
		}
		trials[n].active = false;
	}
}
static void cpu_interactive(void)
{
	/* opt-in, max 30min per boot. No attempt to re-init TSENS or raise the
	 * voltage; invalid sensor/ownership drops to the saved request. */
	uint64_t until, previous = mach_absolute_time();
	clock_interval_to_deadline(1800, NSEC_PER_SEC, &until);
	int hot = 1, last_raise = -1;
	unsigned iterations = 0;
	while (mach_absolute_time() < until) {
		int mc = 0;
		int valid = cpu_temperature(&mc);
		uint64_t now = mach_absolute_time(), now_ns, act_ns, age_ns;
		absolutetime_to_nanoseconds(now, &now_ns);
		absolutetime_to_nanoseconds(__atomic_load_n(&activity_ticks, __ATOMIC_RELAXED), &act_ns);
		absolutetime_to_nanoseconds(now - previous, &age_ns);
		previous = now;
		int raise = ceph_cpu_should_raise(valid, mc, &hot, now_ns, act_ns, age_ns);
		if (raise != last_raise || (iterations++ % 40) == 0) {
			printf("CEPHEUS CPU POLICY: valid=%d max_mc=%d hot=%d raise=%d\n", valid, mc, hot, raise);
			last_raise = raise;
		}
		if (raise) {
			bool active = false;
			for (unsigned n = 2; n < 4; ++n) {
				if (trials[n].active && sr(banks[n].va, 0x920) != trials[n].new_index) {
					printf("CEPHEUS CPU POLICY: external OSM change, stop\n");
					osm_trial_restore(); return;
				}
				active |= trials[n].active;
			}
			if (!active) { osm_trial_start(); }
		} else { osm_trial_restore(); }
		swait(250);
	}
	osm_trial_restore();
	printf("CEPHEUS CPU POLICY: session expired, baseline restored\n");
}
static void osm_dump(struct osm_bank *b)
{
	b->va = ml_io_map((vm_offset_t)b->pa, 0x1400);
	if (!b->va) { printf("CEPHEUS OSM: %s map failed\n", b->name); return; }
	uint32_t enable = sr(b->va, 0), dcvs = sr(b->va, 0xbc);
	printf("CEPHEUS OSM: %s pa=%#llx enable=%#x per_core=%#x\n", b->name, b->pa, enable, dcvs);
	b->enabled = (enable != 0xffffffffu && (enable & 1u));
	if (!b->enabled) { return; }
	uint32_t prev = 0;
	for (unsigned i = 0; i < CEPHEUS_OSM_ROWS; ++i) {
		uint32_t raw = sr(b->va, 0x110 + i * 32), v = sr(b->va, 0x114 + i * 32);
		uint32_t khz = cepheus_osm_khz(raw);
		printf("CEPHEUS OSM: %s LUT%u raw=%#x khz=%u cores=%u volt_raw=%#x\n",
		    b->name, i, raw, khz, cepheus_osm_core_count(raw), v);
		if (raw == 0xffffffffu || (i && khz == prev)) { break; }
		prev = khz;
	}
}
static void osm_sample(unsigned sample)
{
	uint32_t before[4][4] = {{0}};
	uint64_t start = mach_absolute_time();
	for (unsigned n = 0; n < 4; ++n) {
		if (!banks[n].enabled) { continue; }
		unsigned cores = (sr(banks[n].va, 0xbc) & 1u) ? banks[n].cores : 1u;
		for (unsigned c = 0; c < cores; ++c) { before[n][c] = sr(banks[n].va, 0x9c0 + c * 4); }
	}
	swait(100);
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	for (unsigned n = 0; n < 4; ++n) {
		struct osm_bank *b = &banks[n];
		if (!b->enabled) { continue; }
		unsigned cores = (sr(b->va, 0xbc) & 1u) ? b->cores : 1u;
		for (unsigned c = 0; c < cores; ++c) {
			uint32_t desired = sr(b->va, 0x920 + c * 4);
			uint32_t after = sr(b->va, 0x9c0 + c * 4);
			uint32_t delta = cepheus_counter_delta(before[n][c], after);
			uint32_t khz = desired < CEPHEUS_OSM_ROWS ? cepheus_osm_khz(sr(b->va, 0x110 + desired * 32)) : 0;
			printf("CEPHEUS OSM: sample%u %s core%u desired=%u target_khz=%u cycles=%u ns=%llu rate_khz=%llu\n",
			    sample, b->name, c, desired, khz, delta, ns,
			    ns ? (uint64_t)delta * 1000000u / ns : 0);
		}
	}
}
static void gpu_probe(void)
{
	vm_offset_t gcc = ml_io_map(0x00100000, 0x90000);
	if (!gcc) { return; }
	uint32_t ahb = sr(gcc, CE_GPU_GCC_CFG);
	printf("CEPHEUS GPU: GCC CFG_AHB=%#x MEMNOC=%#x SNOC_DVM=%#x DDR_AXI=%#x\n",
	    ahb, sr(gcc, CE_GPU_GCC_MEMNOC), sr(gcc, CE_GPU_GCC_DVM), sr(gcc, CE_GPU_GCC_DDR));
	if (!ceph_gpu_branch_active(ahb)) {
		printf("CEPHEUS GPU: CFG_AHB unavailable; GPUCC/GMU/GX not read\n"); return;
	}
	vm_offset_t cc = ml_io_map(0x02c90000, 0x9000);
	if (!cc) { return; }
	uint32_t cx = sr(cc, 0x106c), gx = sr(cc, 0x100c), gmuclk = sr(cc, 0x1098);
	struct ceph_gpu_power_snapshot power = {
	    .cx_gdscr = cx, .cx_hw = sr(cc, CE_GPU_CX_HW_CTRL), .gx_gdscr = gx,
	    .cfg = ahb, .ahb = sr(cc, CE_GPU_AHB_CBCR), .gmu = gmuclk,
	    .cxo = sr(cc, CE_GPU_CXO_CBCR), .rcg = sr(cc, CE_GPU_GMU_CMD),
	    .ddr = sr(gcc, CE_GPU_GCC_DDR), .memnoc = sr(gcc, CE_GPU_GCC_MEMNOC),
	    .dvm = sr(gcc, CE_GPU_GCC_DVM),
	};
	printf("CEPHEUS GPU: CX_GDSCR=%#x GX_GDSCR=%#x AHB=%#x GMU_CLK=%#x CXO=%#x SLEEP=%#x RCG=%#x CFG=%#x\n",
	    cx, gx, sr(cc, 0x1078), gmuclk, sr(cc, 0x109c), sr(cc, 0x1090), sr(cc, 0x1120), sr(cc, 0x1124));
	printf("CEPHEUS GPU: CX_HW_CTRL=%#x gmu_blockers=%#x memory_blockers=%#x (not ownership)\n",
	    power.cx_hw, ceph_gpu_gmu_blockers(&power), ceph_gpu_memory_blockers(&power));
	if (!ceph_gpu_gmu_blockers(&power)) {
		// GMU offsets are word offsets from GPU base, not byte offsets from GMU base.
		vm_offset_t gmu = ml_io_map(0x02c6a000, 0x30000);
		if (gmu) {
			printf("CEPHEUS GPU: GMU SYSRESET=%#x FW_BUSY=%#x INIT_RESULT=%#x POWER=%#x HFI=%#x\n",
			    sr(gmu, 0x1f800 * 4 - 0x6a000), sr(gmu, 0x1f81a * 4 - 0x6a000),
			    sr(gmu, 0x1f81c * 4 - 0x6a000), sr(gmu, 0x1f8d0 * 4 - 0x6a000),
			    sr(gmu, 0x1f980 * 4 - 0x6a000));
		}
	} else { printf("CEPHEUS GPU: GMU powered/clocked state not confirmed; skipped\n"); }
	// Do not access GX: GMU AHB fence must also be validated before GX reads.
	printf("CEPHEUS GPU: probe only; no GPU submission, firmware upload or Metal support\n");
}
static void gpu_fuse_probe(void)
{
	/* Saved live FDT qfprom_memory=<0x780000 0x6fff>, speed-bin offset
	 *0x4130/mask0xe0000000/shift29. Map only the required page. Read only. */
	vm_offset_t efuse=ml_io_map(0x00784000,0x1000);
	if(!efuse) {printf("CEPHEUS GPU BIN: QFPROM map failed\n");return;}
	uint32_t first=sr(efuse,0x130),second=sr(efuse,0x130);
	struct ceph_gpu_bin_policy policy;
	if(first!=second || ceph_gpu_bin_policy(0x06040001,first,&policy)) {
		printf("CEPHEUS GPU BIN: unreadable/unknown speed bin (no enable)\n");return;
	}
	gpu_fuse_value=first;__atomic_store_n(&gpu_fuse_valid,1u,__ATOMIC_RELEASE);
	printf("CEPHEUS GPU BIN: bin=%u gx_levels=%u lm=%u acd=%u throttle=%#x (read-only, no GPU enable)\n",
	    policy.bin,policy.gx_levels,policy.lm,policy.acd,policy.throttle_mask);
}
/* Read-only GX/CP halted-window snapshot. Never writes, never clears, never
 * unhalts. Requires the same GMU-accessible precondition as the GMU dump, plus
 * the GX power/clock and AHB-fence checks, so a powered-down GX reports
 * "not readable" instead of returning reset-value garbage as fact. */
static void gpu_gx_probe(void)
{
	vm_offset_t gcc = ml_io_map(0x00100000, 0x90000);
	if (!gcc) { printf("CEPHEUS GPU GX: GCC map failed\n"); return; }
	uint32_t ahb = sr(gcc, CE_GPU_GCC_CFG);
	if (!ceph_gpu_branch_active(ahb)) {
		printf("CEPHEUS GPU GX: CFG_AHB unavailable; GX not read\n"); return;
	}
	vm_offset_t cc = ml_io_map(0x02c90000, 0x9000);
	if (!cc) { printf("CEPHEUS GPU GX: GPUCC map failed\n"); return; }
	struct ceph_gpu_power_snapshot power = {
	    .cx_gdscr = sr(cc, 0x106c), .cx_hw = sr(cc, CE_GPU_CX_HW_CTRL),
	    .gx_gdscr = sr(cc, 0x100c), .cfg = ahb, .ahb = sr(cc, CE_GPU_AHB_CBCR),
	    .gmu = sr(cc, 0x1098), .cxo = sr(cc, CE_GPU_CXO_CBCR),
	    .rcg = sr(cc, CE_GPU_GMU_CMD), .ddr = sr(gcc, CE_GPU_GCC_DDR),
	    .memnoc = sr(gcc, CE_GPU_GCC_MEMNOC), .dvm = sr(gcc, CE_GPU_GCC_DVM),
	};
	if (ceph_gpu_gmu_blockers(&power)) {
		printf("CEPHEUS GPU GX: GMU not powered/clocked; GX not read\n"); return;
	}
	vm_offset_t gmu = ml_io_map(0x02c6a000, 0x30000);
	if (!gmu) { printf("CEPHEUS GPU GX: GMU map failed\n"); return; }
	/* Same gates the production GX window uses: GX/SPTP power+clock on
	 * (status bits clear) and AHB fence in ALLOW (reads 0). */
	uint32_t state = sr(gmu, 0x1f8d0 * 4 - 0x6a000);
	uint32_t fence = sr(gmu, 0x23b10 * 4 - 0x6a000);
	if (state == UINT32_MAX || fence == UINT32_MAX) {
		printf("CEPHEUS GPU GX: GMU aperture unreadable\n"); return;
	}
	if (state & 0xd4u) {
		printf("CEPHEUS GPU GX: SPTPRAC_PWR_CLK=%#x (GX not powered/clocked); GX not read\n", state);
		return;
	}
	if (fence) {
		printf("CEPHEUS GPU GX: AHB_FENCE=%#x (not ALLOW); GX not read\n", fence);
		return;
	}
	vm_offset_t gx = ml_io_map(0x02c00000, 0x40000);
	if (!gx) { printf("CEPHEUS GPU GX: GX aperture map failed\n"); return; }
	/* Read-only: CP halt/fault, ring/SQE addresses, protection control and the
	 * 64-bit address-mode words. Nothing here is a write or a clear. */
	printf("CEPHEUS GPU GX: CP_HALT=%#x CP_FAULT=%#x PROTECT_CNTL=%#x RING=%#x/%#x SQE=%#x/%#x\n",
	    sr(gx, 0x808), sr(gx, 0x823), sr(gx, 0x84f), sr(gx, 0x800), sr(gx, 0x801),
	    sr(gx, 0x830), sr(gx, 0x831));
	uint32_t modes = 0;
	static const uint32_t mode_words[] = {0x842, 0xc01, 0x8601, 0x8e05, 0x9e01, 0xbe05,
	    0xa601, 0x9601, 0xe00, 0xae01, 0xb601, 0xf810};
	for (unsigned i = 0; i < 12; ++i) {
		uint32_t v = sr(gx, mode_words[i]);
		modes |= (v & 1u) ? (1u << i) : 0u;
	}
	printf("CEPHEUS GPU GX: addr_mode_mask=%#x (0xfff means all 64-bit) UCHE_TRAP=%#x/%#x GMEM=%#x/%#x\n",
	    modes, sr(gx, 0xe09), sr(gx, 0xe0a), sr(gx, 0xe0b), sr(gx, 0xe0d));
	printf("CEPHEUS GPU GX: probe only; no GX write, no unhalt, no submission\n");
}
static void soc_thread(void *arg, wait_result_t wr)
{
	(void)arg; (void)wr;
	swait(45000); // keep boot/USB alive before the hardware probe
	for (unsigned n = 0; n < 4; ++n) { osm_dump(&banks[n]); }
	gpu_probe();
	int fuse=0;
	PE_parse_boot_argn("cephfuse",&fuse,sizeof(fuse));
	if(fuse==1) {gpu_fuse_probe();}
	int gxprobe=0;
	PE_parse_boot_argn("cephgx",&gxprobe,sizeof(gxprobe));
	if(gxprobe==1) {gpu_gx_probe();}
	int arc = 0;
	PE_parse_boot_argn("cepharc", &arc, sizeof(arc));
	if (arc == 1) { cepheus_gpu_arc_probe(); }
	int memory_test = 0;
	PE_parse_boot_argn("cephmem", &memory_test, sizeof(memory_test));
	if (memory_test == 1) { cepheus_gpu_memory_selftest(); }
	if (memory_test == 2) { cepheus_gmu_workspace_selftest(); }
	osm_sample(0);
	int temperature = 0, temperature_valid = cpu_temperature(&temperature);
	printf("CEPHEUS TSENS: valid=%d CPU_max_mc=%d (no sensor configuration writes)\n", temperature_valid, temperature);
	/* Resolve by GPU tzone-names in captured FDT, not legacy tsens-name. */
	vm_offset_t tsens1=ml_io_map(0x0c265000,0x1000);
	int gpuss0=0,gpuss1=0,v0=0,v1=0;
	if(tsens && (sr(tsens,0xe4)&8u))v0=ceph_tsens_mc(sr(tsens,0xa0+15*4),&gpuss0);
	if(tsens1 && (sr(tsens1,0xe4)&8u))v1=ceph_tsens_mc(sr(tsens1,0xa0+11*4),&gpuss1);
	printf("CEPHEUS GPU TSENS: valid_mask=%u gpuss0_mc=%d gpuss1_mc=%d (TSENS0:15 TSENS1:11, sample only)\n",
	    (unsigned)v0|((unsigned)v1<<1),gpuss0,gpuss1);
	swait(60000); // let SpringBoard reach UI before an optional timed experiment
	int mode = 0;
	PE_parse_boot_argn("cephboost", &mode, sizeof(mode));
	if (mode == 2) {
		cpu_interactive();
		(void)thread_terminate(current_thread()); thread_block(THREAD_CONTINUE_NULL); return;
	}
	osm_trial_start();
	uint64_t until;
	clock_interval_to_deadline(120, NSEC_PER_SEC, &until);
	for (unsigned sample = 1; sample <= 24 && mach_absolute_time() < until; ++sample) {
		osm_sample(sample); swait(4900);
	}
	osm_trial_restore();
	osm_sample(25);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}
void cepheus_soc_init(void)
{
	static bool started;
	int enabled = 0;
	thread_t thread;
	PE_parse_boot_argn("cephsoc", &enabled, sizeof(enabled));
	if (!enabled || started) { return; }
	started = true;
	if (kernel_thread_start(soc_thread, NULL, &thread) == KERN_SUCCESS) { thread_deallocate(thread); }
}
#endif
