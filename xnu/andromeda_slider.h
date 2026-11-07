/*
 * Camera slider value of the Mi Mix 3 5G (andromeda), shared by the kernel
 * (andromeda_slider.c, patch 0097) and user space (phone/andromedad). Pure
 * C: no hardware access, no allocation, no locks, no libc. Host-tested by
 * test_andromeda_slider.c. Include the kernel's <sys/param.h> (or
 * <stdint.h> in user space and on the host) first.
 *
 * 64-bit value of the sysctls andromeda.slider and andromeda.slider_wait:
 *   bits  0-7   state: ANDROMEDA_SLIDER_CLOSED/OPEN/MOVING, or
 *               ANDROMEDA_SLIDER_NONE before the first reading
 *   bits  8-15  raw Hall bits, hall42 << 1 | hall114 (0xff before the
 *               first reading)
 *   bits 16-31  opens: closed -> open transitions since boot (mod 2^16)
 *   bits 32-63  generation: +1 for every reading that differs from the
 *               previous one (mod 2^32, skipping 0); 0 = no reading yet
 *
 * Meaning of the two Hall sensors (TLMM GPIO 42 and 114): Xiaomi's halls.c
 * and the LineageOS rewrite by Demon000 (Cosmin Tanislav) map
 * (42, 114) = (0, 1) to STATE_SLIDER_DOWN and (1, 0) to STATE_SLIDER_UP;
 * the KeyHandler of device_xiaomi_andromeda opens the camera on the first
 * (docs/research/sensori-vibrazione.md 1.3). Facts only, no code copied;
 * see CREDITS.md. Until the slider test on the phone confirms it, the
 * polarity is a boot-arg choice (andrslidercam=1 or 2).
 */
#ifndef ANDROMEDA_SLIDER_H
#define ANDROMEDA_SLIDER_H

#define ANDROMEDA_SLIDER_CLOSED 0u
#define ANDROMEDA_SLIDER_OPEN   1u
#define ANDROMEDA_SLIDER_MOVING 2u      /* both sensors equal: magnet in between */
#define ANDROMEDA_SLIDER_NONE   0xffu   /* no reading yet */

/* Generation 0, no opens, raw and state "no reading yet". */
#define ANDROMEDA_SLIDER_INIT   ((uint64_t)0xffffu)

/* Polarity 1: (hall42, hall114) = (0, 1) is open (LineageOS KeyHandler).
 * Polarity 2: (1, 0) is open. */
#define ANDROMEDA_SLIDER_POLARITY_KEYHANDLER 1u
#define ANDROMEDA_SLIDER_POLARITY_INVERTED   2u

static inline unsigned
andromeda_slider_state(uint64_t v)
{
	return (unsigned)(v & 0xffu);
}

static inline unsigned
andromeda_slider_raw(uint64_t v)
{
	return (unsigned)((v >> 8) & 0xffu);
}

static inline unsigned
andromeda_slider_opens(uint64_t v)
{
	return (unsigned)((v >> 16) & 0xffffu);
}

static inline uint32_t
andromeda_slider_generation(uint64_t v)
{
	return (uint32_t)(v >> 32);
}

static inline const char *
andromeda_slider_name(unsigned state)
{
	switch (state) {
	case ANDROMEDA_SLIDER_CLOSED:
		return "closed";
	case ANDROMEDA_SLIDER_OPEN:
		return "open";
	case ANDROMEDA_SLIDER_MOVING:
		return "moving";
	default:
		return "no reading";
	}
}

/* Hall pattern that means "open" for a polarity: hall42 in bit 1. */
static inline unsigned
andromeda_slider_open_raw(unsigned polarity)
{
	return polarity == ANDROMEDA_SLIDER_POLARITY_INVERTED ? 2u : 1u;
}

static inline unsigned
andromeda_slider_decode(unsigned polarity, unsigned h42, unsigned h114)
{
	unsigned raw = ((h42 & 1u) << 1) | (h114 & 1u);
	if (raw == 0u || raw == 3u) {
		return ANDROMEDA_SLIDER_MOVING;
	}
	return raw == andromeda_slider_open_raw(polarity) ?
	       ANDROMEDA_SLIDER_OPEN : ANDROMEDA_SLIDER_CLOSED;
}

/*
 * Value after a reading that differs from the previous one. *stable is the
 * last closed/open state seen (ANDROMEDA_SLIDER_NONE before the first):
 * only closed -> open counts as an opening, so neither a slider already
 * open at boot nor a bounce open -> moving -> open opens the camera.
 */
static inline uint64_t
andromeda_slider_next(uint64_t prev, unsigned *stable, unsigned polarity,
    unsigned h42, unsigned h114)
{
	unsigned state = andromeda_slider_decode(polarity, h42, h114);
	unsigned raw = ((h42 & 1u) << 1) | (h114 & 1u);
	unsigned opens = andromeda_slider_opens(prev);
	uint32_t gen = andromeda_slider_generation(prev) + 1u;
	if (gen == 0u) {
		gen = 1u;               /* 0 is reserved for "no reading yet" */
	}
	if (state != ANDROMEDA_SLIDER_MOVING) {
		if (state == ANDROMEDA_SLIDER_OPEN && *stable == ANDROMEDA_SLIDER_CLOSED) {
			opens = (opens + 1u) & 0xffffu;
		}
		*stable = state;
	}
	return ((uint64_t)gen << 32) | ((uint64_t)opens << 16) |
	       ((uint64_t)raw << 8) | (uint64_t)state;
}

/*
 * Value after the polarity changes at run time ("slidercam N" of the runtime
 * control): the last raw reading (raw, hall42 << 1 | hall114, or 0xff for
 * none) decoded with the new polarity, generation + 1 so that a waiting
 * daemon reads it, and no opening counted, whatever the old state was.
 */
static inline uint64_t
andromeda_slider_repolarize(uint64_t prev, unsigned *stable, unsigned polarity, unsigned raw)
{
	unsigned opens = andromeda_slider_opens(prev);
	uint32_t gen = andromeda_slider_generation(prev) + 1u;
	unsigned state;
	if (gen == 0u) {
		gen = 1u;
	}
	if (raw > 3u) {
		*stable = ANDROMEDA_SLIDER_NONE;
		return prev;            /* no reading yet: nothing to decode */
	}
	state = andromeda_slider_decode(polarity, raw >> 1, raw & 1u);
	*stable = state == ANDROMEDA_SLIDER_MOVING ? ANDROMEDA_SLIDER_NONE : state;
	return ((uint64_t)gen << 32) | ((uint64_t)opens << 16) |
	       ((uint64_t)raw << 8) | (uint64_t)state;
}

#endif /* ANDROMEDA_SLIDER_H */
