#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include "andromeda_cpu_governor.h"

#define MS 1000000ull
#define ROW(lval) (0x40040000u | (uint32_t)(lval))       /* all-core entry */
#define BOOST(lval) (0x40010000u | (uint32_t)(lval))     /* 1-core boost */

/* SM8150 Gold-like table: 710.4 ... 2419.2 MHz, a 2841.6 MHz 1-core boost
 * row, then the repeated row that ends the table. */
static const uint32_t gold_l[] = {37, 43, 49, 55, 61, 67, 73, 78, 84, 89, 94, 100, 105, 111, 116, 121, 126};

static void
gold(struct agov_table *t, uint32_t cap)
{
	uint32_t rows[AGOV_MAX_LEVELS];
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof(gold_l) / sizeof(gold_l[0]); ++i) {
		rows[n++] = ROW(gold_l[i]);
	}
	rows[n++] = BOOST(148);
	rows[n++] = BOOST(148);
	rows[n++] = 0xffffffffu;
	agov_table_build(t, rows, n, cap);
}

static void
test_decode_and_table(void)
{
	assert(agov_row_khz(0) == 300000);
	assert(agov_row_khz(0x4004005d) == 1785600);
	assert(agov_row_khz(0x40010094) == 2841600);
	assert(agov_row_cores(0x4004005d) == 4 && agov_row_cores(0x40010094) == 1);

	struct agov_table t;
	gold(&t, 0);
	assert(t.n == 17);
	assert(t.khz[0] == 710400 && t.index[0] == 0);
	assert(t.khz[7] == 1497600 && t.khz[10] == 1804800);
	assert(t.khz[16] == 2419200 && t.index[16] == 16); /* boost row skipped */
	gold(&t, 1500000);                                  /* andrcpu=1500 */
	assert(t.n == 8 && t.khz[7] == 1497600);
	gold(&t, 500000);                                   /* cap below every entry */
	assert(t.n == 0 && agov_level_at_least(&t, 1) == 0);

	/* Unsorted rows, a duplicate and a 300 MHz GPLL0 entry; 0xffffffff ends. */
	const uint32_t odd[] = {ROW(78), 0, ROW(37), ROW(55), ROW(37), 0xffffffffu, ROW(100)};
	agov_table_build(&t, odd, 7, 0);
	assert(t.n == 4);
	assert(t.khz[0] == 300000 && t.index[0] == 1);
	assert(t.khz[1] == 710400 && t.index[1] == 2);
	assert(t.khz[2] == 1056000 && t.index[2] == 3);
	assert(t.khz[3] == 1497600 && t.index[3] == 0);
	/* A repeated frequency ends the table (as ac_scan); count is bounded. */
	const uint32_t rep[] = {ROW(37), ROW(43), ROW(43), ROW(49)};
	assert(agov_table_build(&t, rep, 4, 0) == 2);
	uint32_t many[60];
	for (unsigned i = 0; i < 60; ++i) {
		many[i] = ROW(10 + i);
	}
	assert(agov_table_build(&t, many, 60, 0) == AGOV_MAX_LEVELS);

	gold(&t, 0);
	assert(agov_level_at_least(&t, 0) == 0);
	assert(agov_level_at_least(&t, 710400) == 0);
	assert(agov_level_at_least(&t, 710401) == 1);
	assert(agov_level_at_least(&t, 1800000) == 10);
	assert(agov_level_at_least(&t, 9000000) == 16);
	assert(agov_level_at_most(&t, 1401600) == 6);
	assert(agov_level_at_most(&t, 1401599) == 5);
	assert(agov_level_at_most(&t, 1) == 0);
}

static void
test_busy(void)
{
	assert(agov_busy_permille(100, 100, 10 * MS, 40 * MS) == 1000);
	assert(agov_busy_permille(100, 104, 10 * MS, 40 * MS) == 0);
	assert(agov_busy_permille(100, 105, 10 * MS, 40 * MS + 50000) == 0);
	assert(agov_busy_permille(100, 102, 10 * MS, 40 * MS) == 500);
	assert(agov_busy_permille(100, 103, 10 * MS, 40 * MS) == 250);
	assert(agov_busy_permille(100, 99, 10 * MS, 40 * MS) == 1000);   /* backwards */
	assert(agov_busy_permille(0xfffffffeu, 2, 10 * MS, 80 * MS) == 500); /* wrap */
	assert(agov_busy_permille(1, 2, 10 * MS, 0) == 0);
	assert(agov_target_khz(1000, 1497600) == 1872000);
	assert(agov_target_khz(0, 2419200) == 0);
	assert(agov_target_khz(500, 1000000) == 625000);
	assert(agov_percent(1, 3) == 33 && agov_percent(2, 3) == 67);
	assert(agov_percent(5, 0) == 0 && agov_percent(9, 4) == 100);
}

static struct agov_params
params(const struct agov_table *t, uint64_t hold_ns)
{
	struct agov_params p = {0, AGOV_SAT_PERMILLE, hold_ns};
	p.hispeed_khz = t->khz[agov_level_at_least(t, (uint64_t)t->khz[t->n - 1] * AGOV_HISPEED_PCT / 100u)];
	return p;
}

static void
test_targets_and_hold(void)
{
	struct agov_table t;
	gold(&t, 0);
	struct agov_params p = params(&t, 160 * MS);
	assert(p.hispeed_khz == 1497600);
	/* Saturated below hispeed: hispeed (more than 1.25 * 710.4 = 888). */
	assert(agov_up_target(&t, &p, 710400, 1000) == 7);
	/* Saturated at/above hispeed: schedutil, 1.25 * 1497.6 = 1872 -> 1920. */
	assert(agov_up_target(&t, &p, 1497600, 1000) == 11);
	assert(agov_up_target(&t, &p, 1920000, 1000) == 16);
	assert(agov_up_target(&t, &p, 2419200, 900) == 16);

	struct agov_state s = {16, 0};
	/* 10 % busy at the top: nothing for 160 ms, then the window target. */
	for (unsigned i = 0; i < 3; ++i) {
		assert(agov_step(&s, &t, &p, 2419200, 100, 40 * MS, 100, 2419200) == 16);
		assert(s.window_ns == (i + 1) * 40 * MS);
	}
	assert(agov_step(&s, &t, &p, 2419200, 100, 40 * MS, 100, 2419200) == 0);
	assert(s.window_ns == 0);
	/* Saturation: up at once and the window restarts. */
	assert(agov_step(&s, &t, &p, 710400, 400, 40 * MS, 400, 710400) == 0);
	assert(s.window_ns == 40 * MS);
	assert(agov_step(&s, &t, &p, 710400, 1000, 40 * MS, 700, 710400) == 7);
	assert(s.window_ns == 0);
	assert(agov_step(&s, &t, &p, 1497600, 1000, 40 * MS, 1000, 1497600) == 11);
	assert(agov_step(&s, &t, &p, 1920000, 1000, 40 * MS, 1000, 1920000) == 16);
	/* Saturated at the top already: the window just runs on. */
	assert(agov_step(&s, &t, &p, 2419200, 1000, 40 * MS, 1000, 2419200) == 16);
	assert(s.window_ns == 40 * MS);
	/* A non-saturated window can also raise: 1.25 * 85 % * 1497.6 -> 1612.8. */
	s.level = 7;
	s.window_ns = 120 * MS;
	assert(agov_step(&s, &t, &p, 1497600, 875, 40 * MS, 850, 1497600) == 8);
	/* Down headroom: at 825.6, 69 % -> 712 (> 710.4) stays; 60 % -> 619,
	 * +1/8 = 697 fits 710.4 -> drops. */
	s.level = 1;
	s.window_ns = 120 * MS;
	assert(agov_step(&s, &t, &p, 825600, 690, 40 * MS, 690, 825600) == 1);
	s.window_ns = 120 * MS;
	assert(agov_step(&s, &t, &p, 825600, 600, 40 * MS, 600, 825600) == 0);
	/* The window clock is the time-weighted applied clock, e.g. a boost:
	 * 20 % of 1804.8 -> 451 -> the bottom. */
	s.level = 8;
	s.window_ns = 120 * MS;
	assert(agov_step(&s, &t, &p, 1804800, 200, 40 * MS, 200, 1804800) == 0);
	/* hold 0: every sample decides. Out-of-range level is clamped; an
	 * empty table is level 0. */
	struct agov_params fast = params(&t, 0);
	s.level = 99;
	assert(agov_step(&s, &t, &fast, 2419200, 0, 40 * MS, 0, 2419200) == 0);
	struct agov_table empty = {0, {0}, {0}};
	assert(agov_step(&s, &empty, &fast, 0, 1000, 40 * MS, 1000, 0) == 0);
}

static void
test_clamps(void)
{
	assert(agov_clamp(0, 10, 16) == 10);   /* boost floor 1804.8 */
	assert(agov_clamp(12, 10, 16) == 12);
	assert(agov_clamp(0, 10, 6) == 6);     /* thermal ceiling beats boost */
	assert(agov_clamp(16, 0, 6) == 6);
	assert(!agov_boost_active(1000, 0, 200));
	assert(agov_boost_active(1000, 900, 200));
	assert(!agov_boost_active(1100, 900, 200));
	assert(agov_boost_active(1000, 1200, 200));
	assert(!agov_boost_active(1000, 900, 0));
	assert(!agov_thermal_hot(false, true, 79999, 80000, 70000));
	assert(agov_thermal_hot(false, true, 80000, 80000, 70000));
	assert(agov_thermal_hot(true, true, 75000, 80000, 70000));
	assert(!agov_thermal_hot(true, true, 69999, 80000, 70000));
	assert(!agov_thermal_hot(false, true, 75000, 80000, 70000));
	assert(agov_thermal_hot(false, false, 20000, 80000, 70000));
	assert(agov_cluster_of_phys(0x000) == 0 && agov_cluster_of_phys(0x300) == 0);
	assert(agov_cluster_of_phys(0x400) == 1 && agov_cluster_of_phys(0x600) == 1);
	assert(agov_cluster_of_phys(0x700) == 2);
	assert(agov_cluster_of_phys(0x800) == -1 && agov_cluster_of_phys(0x101) == -1);
}

/*
 * Closed loop with the kernel's bookkeeping: one core whose work needs
 * demand_khz of clock, idle time counted in whole 10 ms ticks as
 * processor_info() reports it, 40 ms samples plus 50 us of wake latency.
 */
struct sim {
	struct agov_table t;
	struct agov_params p;
	struct agov_state s;
	uint64_t idle_ns;
	uint32_t prev, prev2, win;
	uint64_t prev_ns, win_ns, win_khz_ns;
	unsigned transitions;
};

static uint32_t
sim_ticks(const struct sim *m)
{
	return (uint32_t)(m->idle_ns / (10 * MS));
}

static void
sim_init(struct sim *m, unsigned level)
{
	gold(&m->t, 0);
	m->p = params(&m->t, 160 * MS);
	m->s.level = level;
	m->s.window_ns = 0;
	m->idle_ns = 3 * MS;
	m->prev = m->prev2 = m->win = sim_ticks(m);
	m->prev_ns = m->win_ns = m->win_khz_ns = 0;
	m->transitions = 0;
}

static unsigned
sim_run(struct sim *m, uint64_t demand_khz, unsigned samples)
{
	const uint64_t e = 40 * MS + 50000;
	for (unsigned i = 0; i < samples; ++i) {
		uint32_t cur = m->t.khz[m->s.level];
		uint64_t busy = demand_khz >= cur ? e : e * demand_khz / cur;
		m->idle_ns += e - busy;
		m->win_ns += e;
		m->win_khz_ns += (uint64_t)cur * e;
		uint32_t now = sim_ticks(m);
		unsigned b2 = agov_busy_permille(m->prev2, now, 10 * MS, e + m->prev_ns);
		unsigned bw = agov_busy_permille(m->win, now, 10 * MS, m->win_ns);
		unsigned before = m->s.level;
		agov_step(&m->s, &m->t, &m->p, cur, b2, e, bw, (uint32_t)(m->win_khz_ns / m->win_ns));
		m->transitions += m->s.level != before;
		if (!m->s.window_ns) {
			m->win = now;
			m->win_ns = m->win_khz_ns = 0;
		}
		m->prev2 = m->prev;
		m->prev = now;
		m->prev_ns = e;
	}
	return m->s.level;
}

static void
test_closed_loop(void)
{
	struct sim m;
	/* Full load after idling at the bottom: hispeed after 2 samples (80 ms),
	 * then 1.25x per sample: top after 4 (160 ms). */
	sim_init(&m, 0);
	assert(sim_run(&m, 0, 4) == 0);
	assert(sim_run(&m, 9000000, 1) == 0);
	assert(sim_run(&m, 9000000, 1) == 7);
	assert(sim_run(&m, 9000000, 1) == 11);
	assert(sim_run(&m, 9000000, 1) == 16);
	/* Idle from the top: held 160 ms, at the bottom within 200 ms. */
	assert(sim_run(&m, 0, 3) == 16);
	assert(sim_run(&m, 0, 2) == 0);
	/* Steady work from above and from below: settles near 1.25 * demand
	 * (schedutil), at most the 1/8 headroom band above it, and does not
	 * flip between neighbouring levels (+-1 idle tick per window). */
	static const struct { uint32_t demand, lo, hi; } steady[] = {
		{300000, 710400, 710400}, {600000, 710400, 940800},
		{1000000, 1286400, 1497600}, {1500000, 1920000, 2131200},
		{2000000, 2419200, 2419200},
	};
	for (unsigned k = 0; k < sizeof(steady) / sizeof(steady[0]); ++k) {
		for (unsigned start = 0; start <= 16; start += 16) {
			sim_init(&m, start);
			sim_run(&m, steady[k].demand, 50);
			unsigned before = m.transitions;
			for (unsigned i = 0; i < 250; ++i) {
				unsigned l = sim_run(&m, steady[k].demand, 1);
				assert(m.t.khz[l] >= steady[k].lo && m.t.khz[l] <= steady[k].hi);
			}
			assert(m.transitions - before <= 1);
		}
	}
}

int
main(void)
{
	test_decode_and_table();
	test_busy();
	test_targets_and_hold();
	test_clamps();
	test_closed_loop();
	puts("CPU governor table, busy share, schedutil target, hispeed, hold, clamps, closed loop: PASS");
	return 0;
}
