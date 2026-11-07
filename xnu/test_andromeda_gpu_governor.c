/* Host test of xnu/andromeda_gpu_governor.h (andrgpugov). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "andromeda_gpu_governor.h"

static const struct agpu_gov_cfg cfg = {
	.khz = { 0, 257000, 345000, 427000, 499200, 585000 },
	.min = 1, .max = 5, .up_pct = 90, .target_pct = 70, .idle_pct = 3,
	.hold_ms = 60, .idle_ms = 100, .boost_level = 4, .warm_cap = 3,
};

int
main(void)
{
	struct agpu_gov_state s = { .level = 1 };

	/* utilisation formula: 19.2e6 busy cycles in 1 s = 100 % */
	assert(agpu_gov_util(19200000, 1000000000) == 100);
	assert(agpu_gov_util(9600000, 1000000000) == 50);
	assert(agpu_gov_util(0, 25000000) == 0);
	assert(agpu_gov_util(1, 0) == 0);

	/* idle at the floor stays there */
	assert(agpu_gov_next(&cfg, &s, 0, 25, false, AGPU_GOV_COOL) == 1);
	/* saturated: straight to the top */
	assert(agpu_gov_next(&cfg, &s, 95, 25, false, AGPU_GOV_COOL) == 5);
	/* 60 % at 585 MHz needs 501 MHz at 70 %: 585 (499.2 < 501) - no change */
	assert(agpu_gov_next(&cfg, &s, 60, 25, false, AGPU_GOV_COOL) == 5);
	/* 40 % at 585 needs 334 MHz -> 345 (level 2), but only after the hold, one step at a time */
	assert(agpu_gov_next(&cfg, &s, 40, 25, false, AGPU_GOV_COOL) == 5);
	assert(agpu_gov_next(&cfg, &s, 40, 25, false, AGPU_GOV_COOL) == 5);
	assert(agpu_gov_next(&cfg, &s, 40, 25, false, AGPU_GOV_COOL) == 4);
	/* rising load goes up at once */
	assert(agpu_gov_next(&cfg, &s, 85, 25, false, AGPU_GOV_COOL) == 5);
	/* no work: one step down after the 60 ms hold, the floor at 100 ms */
	assert(agpu_gov_next(&cfg, &s, 0, 25, false, AGPU_GOV_COOL) == 5);
	assert(agpu_gov_next(&cfg, &s, 0, 25, false, AGPU_GOV_COOL) == 5);
	assert(agpu_gov_next(&cfg, &s, 0, 25, false, AGPU_GOV_COOL) == 4);
	assert(agpu_gov_next(&cfg, &s, 0, 25, false, AGPU_GOV_COOL) == 1);
	/* touch: at least the boost level even when idle */
	assert(agpu_gov_next(&cfg, &s, 0, 25, true, AGPU_GOV_COOL) == 4);
	assert(agpu_gov_next(&cfg, &s, 95, 25, true, AGPU_GOV_COOL) == 5);
	/* warm caps, hot drops to the floor */
	assert(agpu_gov_next(&cfg, &s, 95, 25, true, AGPU_GOV_WARM) == 3);
	assert(agpu_gov_next(&cfg, &s, 95, 25, true, AGPU_GOV_HOT) == 1);
	/* moderate steady load settles where it stays under the target */
	s.level = 1;
	assert(agpu_gov_next(&cfg, &s, 80, 25, false, AGPU_GOV_COOL) == 2);   /* 257 * 80 / 70 = 294 MHz -> 345 */
	puts("gpu governor: ok");
	return 0;
}
