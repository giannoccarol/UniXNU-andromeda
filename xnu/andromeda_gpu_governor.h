/*
 * andromeda: GPU frequency governor, the decision part (andrgpugov=1).
 * Pure logic, no kernel calls: built in-kernel by andromeda_gpu.c and on the
 * host by test_andromeda_gpu_governor.c.
 *
 * One sample per period gives the GPU utilisation (busy XO cycles of the GMU
 * power counter over the period, as drm/msm a6xx_gpu_busy reads them) at the
 * current level. The next level is the lowest whose frequency keeps the same
 * work under the target (schedutil's idea; Linux simple_ondemand's
 * up-threshold jumps to the top): at or above up_pct straight to the highest
 * level, otherwise up at once and down only after hold_ms below the current
 * level's need, and to the lowest level after idle_ms without work. Touch
 * keeps at least boost_level for a while (Android's touch boost); warm caps
 * at warm_cap, hot at the lowest level.
 */
#ifndef ANDROMEDA_GPU_GOVERNOR_H
#define ANDROMEDA_GPU_GOVERNOR_H

/* Needs uint32_t/uint64_t and bool from the including file. */

#define AGPU_GOV_LEVELS 6       /* index 0 unused, 1..5 */

struct agpu_gov_cfg {
	uint32_t khz[AGPU_GOV_LEVELS];  /* frequency of each level, 0 = absent */
	unsigned min, max;              /* levels allowed */
	unsigned up_pct;                /* utilisation that jumps to max */
	unsigned target_pct;            /* utilisation aimed at */
	unsigned idle_pct;              /* below this the GPU counts as idle */
	unsigned hold_ms, idle_ms;      /* before a step down, before the floor */
	unsigned boost_level, warm_cap;
};

struct agpu_gov_state {
	unsigned level;
	unsigned low_ms;        /* time the wanted level was below the current one */
	unsigned idle_ms;       /* time without work */
};

enum agpu_gov_heat { AGPU_GOV_COOL, AGPU_GOV_WARM, AGPU_GOV_HOT };

/* Lowest level >= min whose frequency is at least khz (max if none). */
static inline unsigned
agpu_gov_level_for(const struct agpu_gov_cfg *c, uint64_t khz)
{
	for (unsigned l = c->min; l <= c->max; l++) {
		if (c->khz[l] >= khz) {
			return l;
		}
	}
	return c->max;
}

/*
 * util: percent busy at the current level over dt_ms; boost: touch floor in
 * force. Returns the level to vote (also stored in s->level).
 */
static inline unsigned
agpu_gov_next(const struct agpu_gov_cfg *c, struct agpu_gov_state *s, unsigned util,
    unsigned dt_ms, bool boost, enum agpu_gov_heat heat)
{
	unsigned cur = s->level < c->min ? c->min : (s->level > c->max ? c->max : s->level);
	unsigned want;

	if (util > 100) {
		util = 100;
	}
	if (util >= c->up_pct) {
		want = c->max;
	} else {
		uint64_t need = (uint64_t)c->khz[cur] * util / (c->target_pct ? c->target_pct : 1);
		want = agpu_gov_level_for(c, need);
	}
	s->idle_ms = util < c->idle_pct ? s->idle_ms + dt_ms : 0;

	unsigned next = cur;
	if (want > cur) {
		next = want;
		s->low_ms = 0;
	} else if (want < cur) {
		s->low_ms += dt_ms;
		if (s->idle_ms >= c->idle_ms) {
			next = c->min;
			s->low_ms = 0;
		} else if (s->low_ms >= c->hold_ms) {
			next = cur - 1 > want ? cur - 1 : want;
			s->low_ms = 0;
		}
	} else {
		s->low_ms = 0;
	}
	if (boost && next < c->boost_level) {
		next = c->boost_level;
	}
	if (heat == AGPU_GOV_WARM && next > c->warm_cap) {
		next = c->warm_cap;
	} else if (heat == AGPU_GOV_HOT) {
		next = c->min;
	}
	if (next < c->min) {
		next = c->min;
	}
	if (next > c->max) {
		next = c->max;
	}
	s->level = next;
	return next;
}

/* Busy percent from counter deltas: busy XO cycles (19.2 MHz) over dt_ns. */
static inline unsigned
agpu_gov_util(uint64_t busy_xo, uint64_t dt_ns)
{
	if (!dt_ns) {
		return 0;
	}
	/* busy_ns = busy_xo / 19.2 MHz = busy_xo * 1000 / 19.2 */
	uint64_t busy_ns = busy_xo * 625 / 12;
	uint64_t pct = busy_ns * 100 / dt_ns;
	return pct > 100 ? 100 : (unsigned)pct;
}

#endif /* ANDROMEDA_GPU_GOVERNOR_H */
