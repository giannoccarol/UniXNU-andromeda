/*
 * Pure policy of the andromeda CPU load governor (andrgov=1 in
 * andromeda_cpufreq.c). No hardware access, no allocation, no locks:
 * OSM table -> frequency levels, busy share from idle ticks, target level
 * per sample, ramp-down hold, boost/thermal clamps. Host-tested by
 * test_andromeda_cpu_governor.c. Include the kernel's <sys/param.h> (or
 * <stdint.h> and <stdbool.h> on the host) first.
 *
 * Ideas only, no code copied (all GPL-2.0, used as documentation):
 *  - Linux schedutil (kernel/sched/cpufreq_schedutil.c, Rafael J. Wysocki,
 *    Intel): next = 1.25 * f * util / max, where f is the maximum clock when
 *    util is frequency invariant and the current clock when it is not. Our
 *    busy share is measured at the clock that was applied, so the target is
 *    1.25 * busy * applied_khz (= 1.25 * max * util_invariant).
 *  - Android "interactive" governor (drivers/cpufreq/cpufreq_interactive.c,
 *    Mike Chan, Google): a saturated core first jumps to hispeed_freq
 *    (go_hispeed_load), above it the target follows the load; the clock only
 *    drops after min_sample_time; a longer period while idle at the bottom
 *    (timer_slack).
 *  - Qualcomm/CAF cpu-boost (drivers/cpufreq/cpu-boost.c, The Linux
 *    Foundation): a frequency floor for a short time after input events.
 * OSM row decode as in the Mi 9 port (YangQi0408, cepheus_soc.h) and Linux
 * clk-cpu-osm.c. See CREDITS.md.
 */
#ifndef ANDROMEDA_CPU_GOVERNOR_H
#define ANDROMEDA_CPU_GOVERNOR_H

#define AGOV_MAX_LEVELS    40u     /* OSM rows per cluster */
#define AGOV_XO_KHZ        19200u
#define AGOV_SAT_PERMILLE  900u    /* 2-sample busy share treated as saturated */
#define AGOV_HISPEED_PCT   60u     /* hispeed = lowest level >= 60 % of the top */
#define AGOV_DOWN_HEADROOM_SHIFT 3u /* a drop must fit target + target/8 */

/* OSM LUT row: source in bits 31:30 (0 = 300 MHz GPLL0 entry), L value in
 * 7:0, core count in 18:16 (1 = single-core boost entry). */
static inline uint32_t
agov_row_khz(uint32_t row)
{
	return (row >> 30) ? AGOV_XO_KHZ * (row & 0xffu) : 300000u;
}

static inline unsigned
agov_row_cores(uint32_t row)
{
	return (row >> 16) & 7u;
}

/* Usable levels of one cluster, ascending frequency. */
struct agov_table {
	unsigned n;
	uint32_t khz[AGOV_MAX_LEVELS];
	uint8_t index[AGOV_MAX_LEVELS];    /* OSM table index of the level */
};

/*
 * Levels from the raw OSM rows: the table ends at an all-ones row or at a
 * repeated frequency; single-core boost rows and rows above cap_khz (0 = no
 * cap) are skipped, as andrcpu=1/N does; duplicates are dropped and the
 * result is sorted ascending. Returns the level count (0 = unusable).
 */
static inline unsigned
agov_table_build(struct agov_table *t, const uint32_t *rows, unsigned count,
    uint32_t cap_khz)
{
	uint32_t prev = 0;
	t->n = 0;
	if (count > AGOV_MAX_LEVELS) {
		count = AGOV_MAX_LEVELS;
	}
	for (unsigned i = 0; i < count; ++i) {
		uint32_t khz = agov_row_khz(rows[i]);
		if (rows[i] == 0xffffffffu || (i && khz == prev)) {
			break;
		}
		prev = khz;
		if (agov_row_cores(rows[i]) == 1u || (cap_khz && khz > cap_khz)) {
			continue;
		}
		unsigned pos = t->n;
		bool dup = false;
		for (unsigned l = 0; l < t->n; ++l) {
			if (t->khz[l] == khz) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		while (pos > 0 && t->khz[pos - 1] > khz) {
			t->khz[pos] = t->khz[pos - 1];
			t->index[pos] = t->index[pos - 1];
			--pos;
		}
		t->khz[pos] = khz;
		t->index[pos] = (uint8_t)i;
		++t->n;
	}
	return t->n;
}

/* Lowest level at or above khz; the top level if none is high enough. */
static inline unsigned
agov_level_at_least(const struct agov_table *t, uint64_t khz)
{
	for (unsigned l = 0; l < t->n; ++l) {
		if (t->khz[l] >= khz) {
			return l;
		}
	}
	return t->n ? t->n - 1u : 0u;
}

/* Highest level at or below khz; level 0 if none is low enough. */
static inline unsigned
agov_level_at_most(const struct agov_table *t, uint64_t khz)
{
	unsigned level = 0;
	for (unsigned l = 0; l < t->n; ++l) {
		if (t->khz[l] <= khz) {
			level = l;
		}
	}
	return level;
}

/*
 * Busy share of one core over a window, in permille, from the cumulative
 * idle-tick counter of processor_info(PROCESSOR_CPU_LOAD_INFO) and the
 * measured window length. The 32-bit counter may wrap (unsigned delta); a
 * delta that went backwards counts as no idle time; idle time longer than
 * the window counts as a fully idle core.
 */
static inline unsigned
agov_busy_permille(uint32_t idle_then, uint32_t idle_now, uint64_t tick_ns,
    uint64_t window_ns)
{
	if (!window_ns) {
		return 0;
	}
	uint32_t ticks = idle_now - idle_then;
	if (ticks & 0x80000000u) {
		ticks = 0;
	}
	uint64_t idle_ns = (uint64_t)ticks * tick_ns;
	if (idle_ns >= window_ns) {
		return 0;
	}
	return (unsigned)((window_ns - idle_ns) * 1000u / window_ns);
}

/* schedutil target: 1.25 * busy * applied clock (Linux map_util_perf). */
static inline uint64_t
agov_target_khz(unsigned busy_permille, uint32_t khz)
{
	uint64_t demand = (uint64_t)khz * busy_permille / 1000u;
	return demand + (demand >> 2);
}

struct agov_params {
	uint32_t hispeed_khz;   /* a saturated cluster below it jumps to it */
	unsigned sat_permille;  /* AGOV_SAT_PERMILLE */
	uint64_t hold_ns;       /* evaluation window; nothing drops sooner */
};

/*
 * Level for a saturated cluster (busy_pair >= sat_permille over the last two
 * samples): at least the schedutil target, and at least hispeed while the
 * applied clock is below hispeed (interactive's go_hispeed_load).
 */
static inline unsigned
agov_up_target(const struct agov_table *t, const struct agov_params *p,
    uint32_t cur_khz, unsigned busy_pair)
{
	unsigned level = agov_level_at_least(t, agov_target_khz(busy_pair, cur_khz));
	if (cur_khz < p->hispeed_khz) {
		unsigned jump = agov_level_at_least(t, p->hispeed_khz);
		if (jump > level) {
			level = jump;
		}
	}
	return level;
}

/* Governor state of one cluster: requested level before the clamps. */
struct agov_state {
	unsigned level;
	uint64_t window_ns;     /* length of the current evaluation window */
};

/*
 * One full sample.
 *  - Saturated (busy_pair over the last two samples >= sat_permille): up at
 *    once, and the evaluation window restarts.
 *  - Otherwise the sample joins the window; when the window reaches hold_ns
 *    the schedutil target of the whole window (busy_window over it,
 *    window_khz its time-weighted applied clock) is applied: up as is, down
 *    only to the level that still fits the target plus 1/8
 *    (AGOV_DOWN_HEADROOM_SHIFT), and the window restarts. A drop therefore
 *    always waits hold_ns after the last change, and it uses a window long
 *    enough that the 10 ms idle-tick quantization (+-25 % on one 40 ms
 *    sample, +-6 % on 160 ms) averages out; the 1/8 band keeps the residual
 *    noise from flipping between two neighbouring levels every window.
 * The caller restarts its window counters whenever window_ns is 0 after the
 * call. Returns the new level.
 */
static inline unsigned
agov_step(struct agov_state *s, const struct agov_table *t,
    const struct agov_params *p, uint32_t cur_khz, unsigned busy_pair,
    uint64_t elapsed_ns, unsigned busy_window, uint32_t window_khz)
{
	if (!t->n) {
		s->level = 0;
		s->window_ns = 0;
		return 0;
	}
	if (s->level >= t->n) {
		s->level = t->n - 1u;
	}
	if (busy_pair >= p->sat_permille) {
		unsigned up = agov_up_target(t, p, cur_khz, busy_pair);
		if (up > s->level) {
			s->level = up;
			s->window_ns = 0;
			return s->level;
		}
	}
	s->window_ns += elapsed_ns;
	if (s->window_ns < p->hold_ns) {
		return s->level;
	}
	uint64_t target = agov_target_khz(busy_window, window_khz);
	unsigned up = agov_level_at_least(t, target);
	if (up > s->level) {
		s->level = up;
	} else {
		unsigned down = agov_level_at_least(t, target + (target >> AGOV_DOWN_HEADROOM_SHIFT));
		if (down < s->level) {
			s->level = down;
		}
	}
	s->window_ns = 0;
	return s->level;
}

/* Boost floor, then the thermal/cap ceiling, which always wins. */
static inline unsigned
agov_clamp(unsigned level, unsigned floor_level, unsigned ceil_level)
{
	if (level < floor_level) {
		level = floor_level;
	}
	if (level > ceil_level) {
		level = ceil_level;
	}
	return level;
}

/* Input boost window, in Mach time: stamp 0 = never; a stamp just after
 * now (written by another CPU) counts as active. */
static inline bool
agov_boost_active(uint64_t now, uint64_t stamp, uint64_t duration)
{
	if (!stamp || !duration) {
		return false;
	}
	return stamp >= now || now - stamp < duration;
}

/* Thermal hysteresis of andrcpu: hot at or above hot_mc or when the sensors
 * are unreadable (fail safe), cool again only below cool_mc. */
static inline bool
agov_thermal_hot(bool was_hot, bool valid, int mc, int hot_mc, int cool_mc)
{
	if (!valid || mc >= hot_mc) {
		return true;
	}
	if (mc < cool_mc) {
		return false;
	}
	return was_hot;
}

/* SM8150 cluster of a CPU from its MPIDR[15:0] (ADT reg): Aff1 is the core,
 * 0-3 silver (A55), 4-6 gold (A76), 7 prime (A76). -1 = not a known core. */
static inline int
agov_cluster_of_phys(uint32_t phys_id)
{
	if ((phys_id & 0xffu) || phys_id > 0x700u) {
		return -1;
	}
	unsigned core = phys_id >> 8;
	return core < 4u ? 0 : core < 7u ? 1 : 2;
}

/* part / whole in percent, rounded; 0 when whole is 0. */
static inline unsigned
agov_percent(uint64_t part, uint64_t whole)
{
	if (!whole) {
		return 0;
	}
	if (part > whole) {
		part = whole;
	}
	return (unsigned)((part * 100u + whole / 2u) / whole);
}

#endif /* ANDROMEDA_CPU_GOVERNOR_H */
