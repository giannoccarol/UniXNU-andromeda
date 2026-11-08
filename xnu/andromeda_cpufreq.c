/*
 * Mi Mix 3 5G (andromeda, SM8150): CPU clusters at full clock or under a
 * load governor, with a software thermal guard.
 *
 * The OSM hardware DCVS keeps a frequency table per cluster and takes a
 * desired table index; voltage (CPR) and the hardware limits (LMh) stay in
 * hardware. The bootloader leaves the big cores slow (~0.7-0.8 GHz) and
 * nothing under XNU raises them. Method and register layout from ios-flame
 * (Nick Wylynko / NWylynko, sweet_cpufreq.c, patch 0086, MIT) for the same
 * SoC on the Pixel 4; the cycle-counter check and the TSENS decode follow
 * the Mi 9 port (YangQi0408, cepheus_soc.c / cepheus_cpu_policy.h); Linux
 * clk-cpu-osm.c (msm-4.14) and tsens-v2 are the register documentation.
 * See CREDITS.md. OSM bases are the live DT qcom,cpucc node of andromeda.
 *
 * Boot-args (andrcpu is the master switch and the ceiling, andrgov the policy):
 *   andrcpu=0       leave the clusters as the bootloader set them (default)
 *   andrcpu=1       highest all-core frequency of every cluster
 *   andrcpu=N       (N > 1) highest entry at or below N MHz
 *   andrgov=1       with andrcpu=1 or N: load governor instead of the fixed
 *                   entry, never above it (with andrcpu=0: ignored)
 *   andrgovms=T     governor sample period, 10..200 ms (default 40)
 *   andrgovdown=T   governor evaluation window: nothing drops sooner than
 *                   T ms after the last change, 0..2000 (default 160)
 *   andrgovboost=M  touch floor for Gold/Prime, MHz (default 1804, 0 = off)
 *   andrgovboostms=T  length of the touch floor, 0..2000 ms (default 200)
 *   andrgovframe=M  frame floor for Gold/Prime while frames are drawn, MHz
 *                   (default 1804, 0 = off); andrgovframelittle=M the same
 *                   for Silver (default 1209); andrgovframems=T how long a
 *                   frame keeps it, 0..1000 ms (default 100). Set by every
 *                   swap of the framebuffer (andromeda_cpufreq_frame()):
 *                   Core Animation's work comes in bursts the averaged load
 *                   misses (2026-11-08: Gold sat at 710 MHz, its lowest
 *                   entry, 91 % of a scrolling test while frames came at
 *                   20/s), as Android raises the clocks of the top app.
 *   andrcpuhot=C    above C degrees (default 80) Gold/Prime drop to the
 *                   highest entry <= 1.4 GHz ("cool" index)
 *   andrcpucool=C   back to full speed below C degrees (default 70)
 * Unreadable temperature = treated as hot (fail safe). Silver (A55) stays at
 * its selected entry (andrgov=0) or follows its load (andrgov=1). Never
 * writes voltages, LMh or TSENS configuration.
 *
 * Governor (andrgov=1, policy in andromeda_cpu_governor.h, host-tested):
 * every period, for each cluster, the busiest core's busy share from XNU's
 * own accounting: processor_info(PROCESSOR_CPU_LOAD_INFO) per processor
 * (osfmk/kern/processor.c -> recount_processor_usage(), whose idle time is
 * kept by processor_idle() and is current to "now"; the same call
 * host_statistics(HOST_CPU_LOAD_INFO) makes; no lock taken, no allocation),
 * with processor_t from cpu_to_processor() and the core -> cluster mapping
 * from ml_get_topology_info() (MPIDR). Idle time comes in 10 ms ticks
 * (hz_tick_interval), so one 40 ms sample is only +-25 %: a cluster that is
 * saturated over the last two samples goes up at once (hispeed, then 1.25x
 * per sample), anything else is re-evaluated once per andrgovdown window
 * from the window average (schedutil: 1.25 * busy * applied clock), with a
 * 1/8 band before a drop. Touch raises Gold/Prime to andrgovboost; the
 * thermal guard and andrcpu=N stay hard ceilings over both. A summary per
 * cluster every 60 s; no per-sample logging. XNU interfaces read from
 * apple-oss-distributions/xnu xnu-8796.141.3 (APSL 2.0); policy ideas from
 * Linux schedutil, Android interactive and CAF cpu-boost (see the header).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <mach/processor_info.h>
#include <pexpert/pexpert.h>
#include "andromeda_cpu_governor.h"

extern vm_offset_t andromeda_io_map(vm_offset_t, vm_size_t); /* andromeda_probe.c: shared, outside io_submap */
extern kern_return_t thread_terminate(thread_t);
/* osfmk/kern/processor.c and osfmk/arm/cpu_common.c: the MIG server routine
 * and its processor lookup, not declared in the headers BSD sees. */
extern kern_return_t processor_info(processor_t, processor_flavor_t, host_t *,
    processor_info_t, mach_msg_type_number_t *);
extern processor_t cpu_to_processor(int);
/* osfmk/kern/clock.c: one BSD tick (10 ms) in Mach time, the unit of
 * PROCESSOR_CPU_LOAD_INFO; MACH_KERNEL_PRIVATE in kern/clock.h. */
extern uint32_t hz_tick_interval;
void andromeda_cpufreq_init(void);
void andromeda_cpufreq_boost(void);

#define AC_TAG "ANDROMEDA CPU:"

#define OSM_ENABLE     0x000u
#define OSM_PER_CORE   0x0bcu          /* bit 0: per-core DCVS requests */
#define OSM_FREQ(i)    (0x110u + 32u * (i))
#define OSM_VOLT(i)    (0x114u + 32u * (i))
#define OSM_DESIRED(c) (0x920u + 4u * (c))
#define OSM_CYCLES(c)  (0x9c0u + 4u * (c))
#define OSM_ROWS       40u
#define OSM_XO_KHZ     19200u
#define AC_COOL_KHZ    1401600u        /* the Mi 9 port's cap, same voltage as idle */

#define TSENS0_TM_PA   0x0c263000u
#define TSENS1_TM_PA   0x0c265000u

struct ac_cluster {
	const char *name;
	uint32_t pa;
	unsigned cores;
	bool big;                        /* thermal guard applies */
	vm_offset_t va;
	unsigned rows;
	unsigned boot_index, full_index, cool_index, current;
	uint32_t full_khz, cool_khz;
};

static struct ac_cluster ac[] = {
	{"silver (A55 x4)", 0x18323000u, 4, false, 0, 0, 0, 0, 0, 0, 0, 0},
	{"gold (A76 x3)",   0x18325800u, 3, true,  0, 0, 0, 0, 0, 0, 0, 0},
	{"prime (A76 x1)",  0x18327800u, 1, true,  0, 0, 0, 0, 0, 0, 0, 0},
};
#define AC_N (sizeof(ac) / sizeof(ac[0]))

static unsigned ac_sleep_chan;
static unsigned ac_want, ac_hot_c = 80, ac_cool_c = 70;
static unsigned ac_gov_on, ac_gov_ms = 40, ac_gov_hold_ms = 160;
static unsigned ac_boost_mhz = 1804, ac_boost_ms = 200;
static unsigned ac_frame_mhz = 1804, ac_frame_little_mhz = 1209, ac_frame_ms = 100;

static void
ac_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ac_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
ac_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static uint32_t
ac_khz(uint32_t row)
{
	return agov_row_khz(row);
}

static unsigned
ac_row_cores(uint32_t row)
{
	return agov_row_cores(row);
}

/* Request a table index (clk-cpu-osm.c osm_set_index: write, then read back). */
static void
ac_set(struct ac_cluster *c, unsigned index)
{
	unsigned writers = (ac_rd(c->va, OSM_PER_CORE) & 1u) ? c->cores : 1u;
	__asm__ volatile("dmb oshst" ::: "memory");
	for (unsigned core = 0; core < writers; ++core) {
		*(volatile uint32_t *)(c->va + OSM_DESIRED(core)) = index;
	}
	__asm__ volatile("dsb osh" ::: "memory");
	(void)ac_rd(c->va, OSM_ENABLE);
	c->current = index;
}

/* Read the table; choose the full (andrcpu) and cool (<= 1.4 GHz) entries. */
static bool
ac_scan(struct ac_cluster *c)
{
	c->va = andromeda_io_map(c->pa, 0x1400);
	if (!c->va) {
		printf(AC_TAG " %s: map failed\n", c->name);
		return false;
	}
	uint32_t enable = ac_rd(c->va, OSM_ENABLE);
	if (enable == 0xffffffffu || !(enable & 1u)) {
		printf(AC_TAG " %s: OSM not enabled (%#x)\n", c->name, enable);
		return false;
	}
	c->boot_index = c->current = ac_rd(c->va, OSM_DESIRED(0));
	char line[320];
	int len = 0;
	uint32_t prev = 0;
	c->rows = 0;
	for (unsigned i = 0; i < OSM_ROWS; ++i) {
		uint32_t row = ac_rd(c->va, OSM_FREQ(i));
		uint32_t khz = ac_khz(row);
		if (row == 0xffffffffu || (i && khz == prev)) {
			break; /* a repeated frequency ends the table */
		}
		prev = khz;
		c->rows = i + 1;
		if (len > 0 && (size_t)len < sizeof(line) - 16) {
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %u%s", khz / 1000,
			    ac_row_cores(row) == 1 ? "*" : "");
		} else if (len == 0) {
			len = snprintf(line, sizeof(line), "%u%s", khz / 1000, ac_row_cores(row) == 1 ? "*" : "");
		}
		/* A core count of 1 marks a single-core boost entry: skip, as Linux. */
		if (ac_row_cores(row) == 1) {
			continue;
		}
		if ((ac_want <= 1 || khz / 1000 <= ac_want) && khz > c->full_khz) {
			c->full_khz = khz;
			c->full_index = i;
		}
		if (khz <= AC_COOL_KHZ && khz > c->cool_khz) {
			c->cool_khz = khz;
			c->cool_index = i;
		}
	}
	printf(AC_TAG " %s table MHz (*=1-core boost): %s; boot index %u\n",
	    c->name, line, c->boot_index);
	if (!c->rows || !c->full_khz || c->boot_index >= c->rows) {
		printf(AC_TAG " %s: unusable table, left as is\n", c->name);
		return false;
	}
	if (!c->cool_khz) {
		c->cool_index = c->boot_index;
		c->cool_khz = ac_khz(ac_rd(c->va, OSM_FREQ(c->boot_index)));
	}
	if (c->cool_khz > c->full_khz) {
		/* andrcpu=N below 1.4 GHz: being hot must never raise the clock. */
		c->cool_index = c->full_index;
		c->cool_khz = c->full_khz;
	}
	return true;
}

/* Cycle counters over 100 ms: the clock the cores really run at. */
static void
ac_measure(const char *why)
{
	uint32_t before[AC_N][4];
	for (unsigned n = 0; n < AC_N; ++n) {
		for (unsigned core = 0; core < ac[n].cores && core < 4; ++core) {
			before[n][core] = ac[n].va ? ac_rd(ac[n].va, OSM_CYCLES(core)) : 0;
		}
	}
	uint64_t start = mach_absolute_time(), ns;
	ac_wait(100);
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	for (unsigned n = 0; n < AC_N; ++n) {
		if (!ac[n].va || !ns) {
			continue;
		}
		uint32_t delta = ac_rd(ac[n].va, OSM_CYCLES(0)) - before[n][0];
		printf(AC_TAG " %s (%s): desired=%u rate_khz=%llu\n", ac[n].name, why,
		    ac_rd(ac[n].va, OSM_DESIRED(0)), (unsigned long long)((uint64_t)delta * 1000000u / ns));
	}
}

/* Max temperature over every valid TSENS0/TSENS1 sensor, in m°C. */
static bool
ac_temperature(int *max_mc)
{
	static vm_offset_t tm[2];
	static const uint32_t pa[2] = {TSENS0_TM_PA, TSENS1_TM_PA};
	bool any = false;
	int max = -40000;
	for (unsigned t = 0; t < 2; ++t) {
		if (!tm[t]) {
			tm[t] = andromeda_io_map(pa[t], 0x1000);
		}
		if (!tm[t] || !(ac_rd(tm[t], 0xe4) & 8u)) {
			continue;
		}
		for (unsigned s = 0; s < 16; ++s) {
			uint32_t status = ac_rd(tm[t], 0xa0u + 4u * s);
			if (status == 0xffffffffu || !(status & (1u << 21))) {
				continue; /* sensor absent or no valid sample */
			}
			int code = (int)(status & 0xfffu);
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
	}
	*max_mc = max;
	return any;
}

static void
ac_apply(bool hot)
{
	for (unsigned n = 0; n < AC_N; ++n) {
		struct ac_cluster *c = &ac[n];
		if (!c->va || !c->full_khz) {
			continue;
		}
		unsigned want = (hot && c->big) ? c->cool_index : c->full_index;
		if (ac_rd(c->va, OSM_DESIRED(0)) != c->current) {
			printf(AC_TAG " %s: index changed by someone else (%u), guard stops\n",
			    c->name, ac_rd(c->va, OSM_DESIRED(0)));
			c->full_khz = 0;
			continue;
		}
		if (want != c->current) {
			ac_set(c, want);
			printf(AC_TAG " %s -> index %u (%u MHz)%s\n", c->name, want,
			    ac_khz(ac_rd(c->va, OSM_FREQ(want))) / 1000, hot && c->big ? " [thermal]" : "");
		}
	}
}

/* ---- Load governor (andrgov=1) ---------------------------------------- */

#define AC_GOV_CPUS    4u                        /* cores per cluster, at most */
#define AC_GOV_IDLE_MS 100u                      /* period when all idle at the bottom */
#define AC_GOV_LOG_NS  (60ull * NSEC_PER_SEC)    /* summary interval */
#define AC_TEMP_NS     (1000ull * NSEC_PER_MSEC) /* thermal check, as andrgov=0 */
#define AC_DT_MAX_NS   (10ull * NSEC_PER_SEC)    /* keeps kHz*ns sums far from overflow */

static const char *const ac_short[] = {"silver", "gold", "prime"};

struct ac_gov {
	struct agov_table t;
	struct agov_params p;
	struct agov_state s;
	bool on;
	unsigned applied;                /* level written; t.n = not a level */
	uint32_t cur_khz;                /* clock of the index written */
	unsigned boost_level, cool_level, frame_level;
	unsigned ncpu;
	int cpu[AC_GOV_CPUS];            /* XNU logical CPU numbers */
	bool up[AC_GOV_CPUS];            /* running at the last full sample */
	uint32_t idle_prev[AC_GOV_CPUS], idle_prev2[AC_GOV_CPUS], idle_win[AC_GOV_CPUS];
	uint64_t sample_khz_ns, win_ns, win_khz_ns;
	/* summary */
	uint64_t tis_ns[AGOV_MAX_LEVELS];
	uint64_t st_ns, st_khz_ns, st_busy, st_busy_ns, st_boost_ns, st_hot_ns, st_frame_ns;
	unsigned st_trans, busy_now;
};
static struct ac_gov gov[AC_N];

static uint64_t ac_boost_abs;        /* touch floor length, Mach time; 0 = off */
static uint64_t ac_boost_stamp;      /* last touch, Mach time; 0 = never */
static uint64_t ac_frame_abs;        /* frame floor length, Mach time; 0 = off */
static uint64_t ac_frame_stamp;      /* last framebuffer swap, Mach time; 0 = never */
static bool ac_gov_running;

/*
 * Touch activity, called by the touch driver next to cepheus_cpu_activity():
 * one store; the governor thread is woken only when a new boost starts, so
 * the floor is applied at once instead of at the next sample.
 */
void
andromeda_cpufreq_boost(void)
{
	uint64_t now = mach_absolute_time();
	uint64_t prev = __atomic_exchange_n(&ac_boost_stamp, now, __ATOMIC_RELAXED);
	if (__atomic_load_n(&ac_gov_running, __ATOMIC_ACQUIRE) && ac_boost_abs &&
	    !agov_boost_active(now, prev, ac_boost_abs)) {
		thread_wakeup(&ac_sleep_chan);
	}
}

/*
 * Frame activity, called by the framebuffer at every swap (SweetFramebuffer,
 * patch 0203): one store, the governor is woken only when a frame floor
 * starts, as for touch.
 */
void andromeda_cpufreq_frame(void);
void
andromeda_cpufreq_frame(void)
{
	uint64_t now = mach_absolute_time();
	uint64_t prev = __atomic_exchange_n(&ac_frame_stamp, now, __ATOMIC_RELAXED);
	if (__atomic_load_n(&ac_gov_running, __ATOMIC_ACQUIRE) && ac_frame_abs &&
	    !agov_boost_active(now, prev, ac_frame_abs)) {
		thread_wakeup(&ac_sleep_chan);
	}
}

/* Cumulative idle ticks of a running CPU, as host_statistics() reads them;
 * false if the CPU is absent or not running (a CPU never started reports no
 * idle time at all and would look 100 % busy). */
static bool
ac_cpu_idle(int cpu, uint32_t *idle)
{
	processor_t p = cpu_to_processor(cpu);
	host_t host;
	processor_basic_info_data_t basic;
	processor_cpu_load_info_data_t load;
	mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
	if (p == PROCESSOR_NULL ||
	    processor_info(p, PROCESSOR_BASIC_INFO, &host, (processor_info_t)&basic, &count) != KERN_SUCCESS ||
	    !basic.running) {
		return false;
	}
	count = PROCESSOR_CPU_LOAD_INFO_COUNT;
	if (processor_info(p, PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&load, &count) != KERN_SUCCESS) {
		return false;
	}
	*idle = load.cpu_ticks[CPU_STATE_IDLE];
	return true;
}

/* CPUs per cluster from the topology, levels and limits per cluster. */
static bool
ac_gov_setup(void)
{
	const ml_topology_info_t *topo = ml_get_topology_info();
	if (!topo || !topo->cpus) {
		printf(AC_TAG " gov: no CPU topology, andrcpu=%u behaviour instead\n", ac_want);
		return false;
	}
	for (unsigned i = 0; i < topo->num_cpus && i < 8u; ++i) {
		int n = agov_cluster_of_phys(topo->cpus[i].phys_id);
		if (n < 0) {
			printf(AC_TAG " gov: cpu %u mpidr %#x is not an SM8150 core, ignored\n",
			    topo->cpus[i].cpu_id, topo->cpus[i].phys_id);
			continue;
		}
		struct ac_gov *g = &gov[n];
		if (g->ncpu < AC_GOV_CPUS && g->ncpu < ac[n].cores) {
			g->cpu[g->ncpu++] = (int)topo->cpus[i].cpu_id;
		}
	}
	uint32_t cap = ac_want > 1 ? ac_want * 1000u : 0;
	unsigned usable = 0;
	for (unsigned n = 0; n < AC_N; ++n) {
		struct ac_cluster *c = &ac[n];
		struct ac_gov *g = &gov[n];
		if (!c->va || !c->full_khz) {
			continue;
		}
		uint32_t rows[OSM_ROWS];
		for (unsigned i = 0; i < c->rows && i < OSM_ROWS; ++i) {
			rows[i] = ac_rd(c->va, OSM_FREQ(i));
		}
		if (!agov_table_build(&g->t, rows, c->rows, cap)) {
			printf(AC_TAG " gov %s: no usable level, left as is\n", ac_short[n]);
			continue;
		}
		unsigned top = g->t.n - 1u;
		g->p.sat_permille = AGOV_SAT_PERMILLE;
		g->p.hold_ns = (uint64_t)ac_gov_hold_ms * NSEC_PER_MSEC;
		g->p.hispeed_khz = g->t.khz[agov_level_at_least(&g->t,
		    (uint64_t)g->t.khz[top] * AGOV_HISPEED_PCT / 100u)];
		g->boost_level = (c->big && ac_boost_mhz) ?
		    agov_level_at_least(&g->t, (uint64_t)ac_boost_mhz * 1000u) : 0;
		g->cool_level = c->big ? agov_level_at_most(&g->t, AC_COOL_KHZ) : top;
		unsigned frame_mhz = c->big ? ac_frame_mhz : ac_frame_little_mhz;
		g->frame_level = frame_mhz ? agov_level_at_least(&g->t, (uint64_t)frame_mhz * 1000u) : 0;
		g->cur_khz = ac_khz(ac_rd(c->va, OSM_FREQ(c->current)));
		g->s.level = agov_level_at_least(&g->t, g->cur_khz);
		g->s.window_ns = 0;
		g->applied = g->t.n;
		for (unsigned l = 0; l < g->t.n; ++l) {
			if (g->t.index[l] == c->current) {
				g->applied = l;
			}
		}
		char cpus[24];
		int len = 0;
		cpus[0] = '\0';
		for (unsigned k = 0; k < g->ncpu && len >= 0 && (size_t)len < sizeof(cpus) - 4; ++k) {
			len += snprintf(cpus + len, sizeof(cpus) - (size_t)len, "%s%d", k ? "," : "", g->cpu[k]);
		}
		printf(AC_TAG " gov %s: cpus %s, %u levels %u-%u MHz, hispeed %u, touch floor %u, frame floor %u, hot cap %u MHz\n",
		    ac_short[n], g->ncpu ? cpus : "none", g->t.n, g->t.khz[0] / 1000u, g->t.khz[top] / 1000u,
		    g->p.hispeed_khz / 1000u, c->big && ac_boost_mhz ? g->t.khz[g->boost_level] / 1000u : 0,
		    frame_mhz ? g->t.khz[g->frame_level] / 1000u : 0, g->t.khz[g->cool_level] / 1000u);
		g->on = true;
		++usable;
	}
	return usable != 0;
}

/* One full sample of a cluster: busy shares, policy step, window upkeep. */
static void
ac_gov_sample(struct ac_gov *g, uint64_t tick_ns, uint64_t sample_ns, uint64_t prev_ns)
{
	uint32_t idle[AC_GOV_CPUS];
	bool up[AC_GOV_CPUS];
	unsigned last = 0, pair = 0, window = 0;
	for (unsigned k = 0; k < g->ncpu; ++k) {
		up[k] = ac_cpu_idle(g->cpu[k], &idle[k]);
		if (!up[k] || !g->up[k]) {
			continue;
		}
		unsigned b = agov_busy_permille(g->idle_prev[k], idle[k], tick_ns, sample_ns);
		last = b > last ? b : last;
		b = agov_busy_permille(g->idle_prev2[k], idle[k], tick_ns, sample_ns + prev_ns);
		pair = b > pair ? b : pair;
		b = agov_busy_permille(g->idle_win[k], idle[k], tick_ns, g->win_ns);
		window = b > window ? b : window;
	}
	uint32_t cur_khz = (uint32_t)(g->sample_khz_ns / sample_ns);
	uint32_t win_khz = g->win_ns ? (uint32_t)(g->win_khz_ns / g->win_ns) : cur_khz;
	(void)agov_step(&g->s, &g->t, &g->p, cur_khz, pair, sample_ns, window, win_khz);
	bool restart = g->s.window_ns == 0;
	for (unsigned k = 0; k < g->ncpu; ++k) {
		if (!up[k]) {
			g->up[k] = false;
		} else if (!g->up[k]) {
			/* Just came up (or first sample): baseline, no share yet. */
			g->idle_prev2[k] = g->idle_prev[k] = g->idle_win[k] = idle[k];
			g->up[k] = true;
		} else {
			g->idle_prev2[k] = g->idle_prev[k];
			g->idle_prev[k] = idle[k];
			if (restart) {
				g->idle_win[k] = idle[k];
			}
		}
	}
	if (restart) {
		g->win_ns = g->win_khz_ns = 0;
	}
	g->sample_khz_ns = 0;
	g->st_busy += (uint64_t)last * sample_ns;
	g->st_busy_ns += sample_ns;
	g->busy_now = pair;
}

/* One line per cluster: average clock, busiest-core busy share (average and
 * last two samples), OSM writes, share of time boosted / thermally capped,
 * then time in state as MHz:percent for every level used >= 0.5 %. */
static void
ac_gov_summary(unsigned n, uint64_t secs)
{
	struct ac_gov *g = &gov[n];
	uint64_t ns = g->st_ns;
	char line[320];
	int len = snprintf(line, sizeof(line),
	    AC_TAG " gov %s %llus: avg %llu MHz util %u%% now %u%% trans %u boost %u%% frame %u%% hot %u%%%s |",
	    ac_short[n], (unsigned long long)secs,
	    (unsigned long long)(ns ? g->st_khz_ns / ns / 1000u : 0),
	    g->st_busy_ns ? (unsigned)((g->st_busy / g->st_busy_ns + 5u) / 10u) : 0u,
	    (g->busy_now + 5u) / 10u, g->st_trans, agov_percent(g->st_boost_ns, ns),
	    agov_percent(g->st_frame_ns, ns), agov_percent(g->st_hot_ns, ns), g->on ? "" : " STOPPED");
	for (unsigned l = 0; l < g->t.n && len > 0 && (size_t)len < sizeof(line) - 16; ++l) {
		unsigned pct = agov_percent(g->tis_ns[l], ns);
		if (pct) {
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %u:%u", g->t.khz[l] / 1000u, pct);
		}
		g->tis_ns[l] = 0;
	}
	printf("%s\n", line);
	g->st_ns = g->st_khz_ns = g->st_busy = g->st_busy_ns = g->st_boost_ns = g->st_hot_ns = g->st_frame_ns = 0;
	g->st_trans = 0;
}

static void
ac_gov_run(void)
{
	uint64_t tick_ns = 0;
	absolutetime_to_nanoseconds(hz_tick_interval, &tick_ns);
	if (tick_ns < NSEC_PER_MSEC || tick_ns > 100u * NSEC_PER_MSEC) {
		tick_ns = 10u * NSEC_PER_MSEC; /* clock_timebase_init(): BSD tick = 10 ms */
	}
	uint64_t period_ns = (uint64_t)ac_gov_ms * NSEC_PER_MSEC;
	uint64_t min_sample_ns = period_ns / 2u > 2u * tick_ns ? period_ns / 2u : 2u * tick_ns;
	uint64_t temp_abs, log_abs;
	nanoseconds_to_absolutetime(AC_TEMP_NS, &temp_abs);
	nanoseconds_to_absolutetime(AC_GOV_LOG_NS, &log_abs);
	uint64_t boost_abs = 0;
	if (ac_boost_mhz && ac_boost_ms) {
		nanoseconds_to_absolutetime((uint64_t)ac_boost_ms * NSEC_PER_MSEC, &boost_abs);
	}
	ac_boost_abs = boost_abs;
	uint64_t frame_abs = 0;
	if (ac_frame_ms) {
		nanoseconds_to_absolutetime((uint64_t)ac_frame_ms * NSEC_PER_MSEC, &frame_abs);
	}
	ac_frame_abs = frame_abs;

	for (unsigned n = 0; n < AC_N; ++n) {
		for (unsigned k = 0; k < gov[n].ncpu; ++k) {
			uint32_t idle = 0;
			gov[n].up[k] = ac_cpu_idle(gov[n].cpu[k], &idle);
			gov[n].idle_prev[k] = gov[n].idle_prev2[k] = gov[n].idle_win[k] = idle;
		}
	}
	int mc = 0, mc_max = -40000;
	bool valid = ac_temperature(&mc);
	bool hot = agov_thermal_hot(false, valid, mc, (int)ac_hot_c * 1000, (int)ac_cool_c * 1000);
	printf(AC_TAG " temperature %s %d mC; hot>=%u C cool<%u C; tick %llu us\n",
	    valid ? "max" : "UNREADABLE (fail safe)", mc, ac_hot_c, ac_cool_c,
	    (unsigned long long)(tick_ns / 1000u));
	bool boost = false, frame = false, cost_logged = false;
	unsigned samples = 0, wakes = 0, unreadable = 0;
	uint64_t prev_sample_ns = 0, cost_sum = 0, cost_max = 0, run_ns = 0;
	uint64_t t_prev = mach_absolute_time(), t_sample = t_prev, t_temp = t_prev, t_log = t_prev;
	__atomic_store_n(&ac_gov_running, true, __ATOMIC_RELEASE);

	for (bool first = true;; first = false) {
		bool quiet = !boost;
		for (unsigned n = 0; n < AC_N; ++n) {
			quiet &= !gov[n].on || (gov[n].applied == 0 && gov[n].s.level == 0);
		}
		if (!first) {
			ac_wait(quiet && ac_gov_ms < AC_GOV_IDLE_MS ? AC_GOV_IDLE_MS : ac_gov_ms);
		}
		uint64_t now = mach_absolute_time(), dt_ns, sample_ns;
		absolutetime_to_nanoseconds(now - t_prev, &dt_ns);
		absolutetime_to_nanoseconds(now - t_sample, &sample_ns);
		t_prev = now;
		dt_ns = dt_ns > AC_DT_MAX_NS ? AC_DT_MAX_NS : dt_ns;
		sample_ns = sample_ns > AC_DT_MAX_NS ? AC_DT_MAX_NS : sample_ns;
		++wakes;

		/* The interval that just ended, at the clock written during it. */
		for (unsigned n = 0; n < AC_N; ++n) {
			struct ac_gov *g = &gov[n];
			if (!g->on) {
				continue;
			}
			uint64_t khz_ns = (uint64_t)g->cur_khz * dt_ns;
			g->sample_khz_ns += khz_ns;
			g->win_khz_ns += khz_ns;
			g->win_ns += dt_ns;
			g->st_ns += dt_ns;
			g->st_khz_ns += khz_ns;
			if (g->applied < g->t.n) {
				g->tis_ns[g->applied] += dt_ns;
			}
			g->st_boost_ns += boost && ac[n].big ? dt_ns : 0;
			g->st_frame_ns += frame && g->frame_level ? dt_ns : 0;
			g->st_hot_ns += hot && ac[n].big ? dt_ns : 0;
		}
		run_ns += dt_ns;

		if (now - t_temp >= temp_abs) {
			t_temp = now;
			bool was = hot;
			valid = ac_temperature(&mc);
			hot = agov_thermal_hot(hot, valid, mc, (int)ac_hot_c * 1000, (int)ac_cool_c * 1000);
			unreadable += valid ? 0u : 1u;
			mc_max = valid && mc > mc_max ? mc : mc_max;
			if (hot != was) {
				printf(AC_TAG " temperature %s %d mC -> %s\n", valid ? "max" : "UNREADABLE",
				    mc, hot ? "cool down" : "full speed");
			}
		}
		boost = agov_boost_active(now, __atomic_load_n(&ac_boost_stamp, __ATOMIC_RELAXED), boost_abs);
		frame = agov_boost_active(now, __atomic_load_n(&ac_frame_stamp, __ATOMIC_RELAXED), frame_abs);
		bool full = !first && sample_ns >= min_sample_ns;

		bool any = false;
		for (unsigned n = 0; n < AC_N; ++n) {
			struct ac_cluster *c = &ac[n];
			struct ac_gov *g = &gov[n];
			if (!g->on) {
				continue;
			}
			if (ac_rd(c->va, OSM_DESIRED(0)) != c->current) {
				printf(AC_TAG " %s: index changed by someone else (%u), governor stops\n",
				    c->name, ac_rd(c->va, OSM_DESIRED(0)));
				g->on = false;
				continue;
			}
			any = true;
			if (full) {
				ac_gov_sample(g, tick_ns, sample_ns, prev_sample_ns);
			}
			unsigned hi = hot && c->big ? g->cool_level : g->t.n - 1u;
			unsigned lo = boost && c->big ? g->boost_level : 0u;
			if (frame && g->frame_level > lo) {
				lo = g->frame_level;
			}
			unsigned want = agov_clamp(g->s.level, lo, hi);
			if (want != g->applied) {
				ac_set(c, g->t.index[want]);
				g->applied = want;
				g->cur_khz = g->t.khz[want];
				++g->st_trans;
			}
		}
		if (full) {
			t_sample = now;
			prev_sample_ns = sample_ns;
			++samples;
		}

		uint64_t cost_ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - now, &cost_ns);
		cost_sum += cost_ns;
		cost_max = cost_ns > cost_max ? cost_ns : cost_max;

		if (now - t_log >= log_abs || !any) {
			uint64_t secs = 0;
			absolutetime_to_nanoseconds(now - t_log, &secs);
			secs /= NSEC_PER_SEC;
			t_log = now;
			for (unsigned n = 0; n < AC_N; ++n) {
				if (gov[n].on || gov[n].st_ns) {
					ac_gov_summary(n, secs);
				}
			}
			printf(AC_TAG " gov %llus: %u samples, %u wakes, max %d mC%s\n", (unsigned long long)secs,
			    samples, wakes, mc_max, unreadable ? ", sensors UNREADABLE at times (capped)" : "");
			if (!cost_logged && wakes) {
				/* Own cost, once: time from wake-up to going back to sleep. */
				printf(AC_TAG " gov cost: avg %llu ns, max %llu ns per wake, %llu ppm of one CPU\n",
				    (unsigned long long)(cost_sum / wakes), (unsigned long long)cost_max,
				    (unsigned long long)(run_ns ? cost_sum * 1000000u / run_ns : 0));
				cost_logged = true;
			}
			samples = wakes = unreadable = 0;
			mc_max = -40000;
		}
		if (!any) {
			break;
		}
	}
	__atomic_store_n(&ac_gov_running, false, __ATOMIC_RELEASE);
}

static void
ac_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	unsigned usable = 0;
	for (unsigned n = 0; n < AC_N; ++n) {
		usable += ac_scan(&ac[n]) ? 1u : 0u;
	}
	if (!usable) {
		goto out;
	}
	ac_measure("boot");
	if (ac_gov_on && ac_gov_setup()) {
		ac_gov_run();
		goto out;
	}
	int mc = 0;
	bool valid = ac_temperature(&mc);
	bool hot = !valid || mc >= (int)ac_hot_c * 1000;
	printf(AC_TAG " temperature %s %d mC; hot>=%u C cool<%u C\n",
	    valid ? "max" : "UNREADABLE (fail safe)", mc, ac_hot_c, ac_cool_c);
	ac_apply(hot);
	ac_measure(hot ? "cool" : "full");
	for (unsigned tick = 1;; ++tick) {
		ac_wait(1000);
		valid = ac_temperature(&mc);
		bool was = hot;
		if (!valid || mc >= (int)ac_hot_c * 1000) {
			hot = true;
		} else if (mc < (int)ac_cool_c * 1000) {
			hot = false;
		}
		if (hot != was) {
			printf(AC_TAG " temperature %s %d mC -> %s\n", valid ? "max" : "UNREADABLE",
			    mc, hot ? "cool down" : "full speed");
		}
		ac_apply(hot);
		if ((tick % 120u) == 0) {
			printf(AC_TAG " %s %d mC, %s\n", valid ? "max" : "UNREADABLE", mc,
			    hot ? "cooling" : "full speed");
		}
		bool any = false;
		for (unsigned n = 0; n < AC_N; ++n) {
			any |= ac[n].full_khz != 0;
		}
		if (!any) {
			break;
		}
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
andromeda_cpufreq_init(void)
{
	static bool started;
	thread_t thread;
	(void)PE_parse_boot_argn("andrcpu", &ac_want, sizeof(ac_want));
	(void)PE_parse_boot_argn("andrcpuhot", &ac_hot_c, sizeof(ac_hot_c));
	(void)PE_parse_boot_argn("andrcpucool", &ac_cool_c, sizeof(ac_cool_c));
	(void)PE_parse_boot_argn("andrgov", &ac_gov_on, sizeof(ac_gov_on));
	(void)PE_parse_boot_argn("andrgovms", &ac_gov_ms, sizeof(ac_gov_ms));
	(void)PE_parse_boot_argn("andrgovdown", &ac_gov_hold_ms, sizeof(ac_gov_hold_ms));
	(void)PE_parse_boot_argn("andrgovboost", &ac_boost_mhz, sizeof(ac_boost_mhz));
	(void)PE_parse_boot_argn("andrgovboostms", &ac_boost_ms, sizeof(ac_boost_ms));
	(void)PE_parse_boot_argn("andrgovframe", &ac_frame_mhz, sizeof(ac_frame_mhz));
	(void)PE_parse_boot_argn("andrgovframelittle", &ac_frame_little_mhz, sizeof(ac_frame_little_mhz));
	(void)PE_parse_boot_argn("andrgovframems", &ac_frame_ms, sizeof(ac_frame_ms));
	if (ac_frame_ms > 1000) {
		ac_frame_ms = 100;
	}
	if (started) {
		return;
	}
	if (!ac_want) {
		if (ac_gov_on) {
			printf(AC_TAG " andrgov=%u ignored: andrcpu=0 leaves the clusters alone\n", ac_gov_on);
			started = true; /* say it once */
		}
		return;
	}
	if (ac_hot_c > 95 || ac_cool_c >= ac_hot_c) {
		printf(AC_TAG " refusing hot=%u cool=%u (need cool < hot <= 95)\n", ac_hot_c, ac_cool_c);
		return;
	}
	if (ac_gov_on > 1) {
		printf(AC_TAG " andrgov=%u unknown (0 or 1), fixed clock instead\n", ac_gov_on);
		ac_gov_on = 0;
	}
	if (ac_gov_on && (ac_gov_ms < 10 || ac_gov_ms > 200)) {
		printf(AC_TAG " andrgovms=%u outside 10..200, using 40\n", ac_gov_ms);
		ac_gov_ms = 40;
	}
	if (ac_gov_on && ac_gov_hold_ms > 2000) {
		printf(AC_TAG " andrgovdown=%u above 2000, using 160\n", ac_gov_hold_ms);
		ac_gov_hold_ms = 160;
	}
	if (ac_gov_on && ac_boost_ms > 2000) {
		printf(AC_TAG " andrgovboostms=%u above 2000, using 200\n", ac_boost_ms);
		ac_boost_ms = 200;
	}
	started = true;
	if (ac_gov_on) {
		printf(AC_TAG " andrcpu=%u andrgov=1: load governor, %u ms samples, %u ms window, "
		    "touch floor %u MHz for %u ms, thermal guard\n", ac_want, ac_gov_ms, ac_gov_hold_ms,
		    ac_boost_mhz, ac_boost_ms);
	} else {
		printf(AC_TAG " andrcpu=%u: full clock with thermal guard\n", ac_want);
	}
	if (kernel_thread_start(ac_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
