/*
 * Host test of andromeda_slider.h (camera slider value, patch 0097):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined xnu/test_andromeda_slider.c -o /tmp/slider-test
 *   /tmp/slider-test
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "andromeda_slider.h"

#define KH ANDROMEDA_SLIDER_POLARITY_KEYHANDLER
#define INV ANDROMEDA_SLIDER_POLARITY_INVERTED

struct sim {
	uint64_t v;
	unsigned stable;
	unsigned polarity;
};

static void
sim_init(struct sim *s, unsigned polarity)
{
	s->v = ANDROMEDA_SLIDER_INIT;
	s->stable = ANDROMEDA_SLIDER_NONE;
	s->polarity = polarity;
}

static unsigned
step(struct sim *s, unsigned h42, unsigned h114)
{
	s->v = andromeda_slider_next(s->v, &s->stable, s->polarity, h42, h114);
	return andromeda_slider_state(s->v);
}

static void
test_init(void)
{
	uint64_t v = ANDROMEDA_SLIDER_INIT;
	assert(andromeda_slider_state(v) == ANDROMEDA_SLIDER_NONE);
	assert(andromeda_slider_raw(v) == 0xffu);
	assert(andromeda_slider_opens(v) == 0u);
	assert(andromeda_slider_generation(v) == 0u);
}

static void
test_decode(void)
{
	/* KeyHandler meaning: (42, 114) = (0, 1) open, (1, 0) closed. */
	assert(andromeda_slider_decode(KH, 0, 1) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_decode(KH, 1, 0) == ANDROMEDA_SLIDER_CLOSED);
	assert(andromeda_slider_decode(KH, 0, 0) == ANDROMEDA_SLIDER_MOVING);
	assert(andromeda_slider_decode(KH, 1, 1) == ANDROMEDA_SLIDER_MOVING);
	assert(andromeda_slider_decode(INV, 1, 0) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_decode(INV, 0, 1) == ANDROMEDA_SLIDER_CLOSED);
	/* Only bit 0 of each reading counts. */
	assert(andromeda_slider_decode(KH, 2, 3) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_open_raw(KH) == 1u && andromeda_slider_open_raw(INV) == 2u);
}

static void
test_sequence(void)
{
	struct sim s;
	sim_init(&s, KH);
	assert(step(&s, 1, 0) == ANDROMEDA_SLIDER_CLOSED);  /* first reading */
	assert(andromeda_slider_generation(s.v) == 1u && andromeda_slider_opens(s.v) == 0u);
	assert(andromeda_slider_raw(s.v) == 2u);
	assert(step(&s, 0, 0) == ANDROMEDA_SLIDER_MOVING);
	assert(andromeda_slider_opens(s.v) == 0u);
	assert(step(&s, 0, 1) == ANDROMEDA_SLIDER_OPEN);    /* closed -> open */
	assert(andromeda_slider_opens(s.v) == 1u && andromeda_slider_generation(s.v) == 3u);
	assert(step(&s, 1, 1) == ANDROMEDA_SLIDER_MOVING);  /* bounce ... */
	assert(step(&s, 0, 1) == ANDROMEDA_SLIDER_OPEN);    /* ... is not an opening */
	assert(andromeda_slider_opens(s.v) == 1u);
	assert(step(&s, 1, 0) == ANDROMEDA_SLIDER_CLOSED);
	assert(step(&s, 0, 1) == ANDROMEDA_SLIDER_OPEN);    /* closed -> open, no moving read */
	assert(andromeda_slider_opens(s.v) == 2u && andromeda_slider_generation(s.v) == 7u);
}

static void
test_open_at_boot(void)
{
	struct sim s;
	sim_init(&s, KH);
	assert(step(&s, 0, 1) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_opens(s.v) == 0u);          /* open already: not an opening */
	assert(step(&s, 0, 0) == ANDROMEDA_SLIDER_MOVING);
	assert(step(&s, 1, 0) == ANDROMEDA_SLIDER_CLOSED);
	assert(step(&s, 0, 1) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_opens(s.v) == 1u);
	/* Moving first, then open: still no opening without a closed state. */
	sim_init(&s, INV);
	assert(step(&s, 1, 1) == ANDROMEDA_SLIDER_MOVING);
	assert(s.stable == ANDROMEDA_SLIDER_NONE);
	assert(step(&s, 1, 0) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_opens(s.v) == 0u);
}

static void
test_wrap(void)
{
	unsigned stable = ANDROMEDA_SLIDER_CLOSED;
	uint64_t v = ((uint64_t)0xffffffffu << 32) | ((uint64_t)0xffffu << 16) | (2u << 8) |
	    ANDROMEDA_SLIDER_CLOSED;
	v = andromeda_slider_next(v, &stable, KH, 0, 1);
	assert(andromeda_slider_generation(v) == 1u);       /* skips 0 */
	assert(andromeda_slider_opens(v) == 0u);            /* mod 2^16 */
	assert(andromeda_slider_state(v) == ANDROMEDA_SLIDER_OPEN);
	assert(andromeda_slider_raw(v) == 1u);
}

static void
test_names(void)
{
	assert(andromeda_slider_name(ANDROMEDA_SLIDER_OPEN)[0] == 'o');
	assert(andromeda_slider_name(ANDROMEDA_SLIDER_CLOSED)[0] == 'c');
	assert(andromeda_slider_name(ANDROMEDA_SLIDER_MOVING)[0] == 'm');
	assert(andromeda_slider_name(ANDROMEDA_SLIDER_NONE)[0] == 'n');
}

int
main(void)
{
	test_init();
	test_decode();
	test_sequence();
	test_open_at_boot();
	test_wrap();
	test_names();
	printf("andromeda_slider: all tests passed\n");
	return 0;
}
