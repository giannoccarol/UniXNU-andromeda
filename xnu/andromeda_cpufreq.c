/*
 * Mi Mix 3 5G (andromeda, SM8150): CPU clusters at full clock, with a
 * software thermal guard.
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
 * Boot-args:
 *   andrcpu=0       leave the clusters as the bootloader set them (default)
 *   andrcpu=1       highest all-core frequency of every cluster
 *   andrcpu=N       (N > 1) highest entry at or below N MHz
 *   andrcpuhot=C    above C degrees (default 80) Gold/Prime drop to the
 *                   highest entry <= 1.4 GHz ("cool" index)
 *   andrcpucool=C   back to full speed below C degrees (default 70)
 * Unreadable temperature = treated as hot (fail safe). Silver (A55) stays at
 * its selected entry. Never writes voltages, LMh or TSENS configuration.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
void andromeda_cpufreq_init(void);

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
	return (row >> 30) ? OSM_XO_KHZ * (row & 0xffu) : 300000u;
}

static unsigned
ac_row_cores(uint32_t row)
{
	return (row >> 16) & 7u;
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
	c->va = ml_io_map(c->pa, 0x1400);
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
			tm[t] = ml_io_map(pa[t], 0x1000);
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
	if (!ac_want || started) {
		return;
	}
	if (ac_hot_c > 95 || ac_cool_c >= ac_hot_c) {
		printf(AC_TAG " refusing hot=%u cool=%u (need cool < hot <= 95)\n", ac_hot_c, ac_cool_c);
		return;
	}
	started = true;
	printf(AC_TAG " andrcpu=%u: full clock with thermal guard\n", ac_want);
	if (kernel_thread_start(ac_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
