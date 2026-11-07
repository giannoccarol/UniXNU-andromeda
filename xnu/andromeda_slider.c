/*
 * Mi Mix 3 5G (andromeda, SM8150): camera slider position for user space,
 * so that phone/andromedad can open the Camera app when the slider is
 * pushed open (as Xiaomi's Android does).
 *
 * Boot-args:
 *   andrslidercam=1   publish the readings of the probe's slider thread
 *                     (andromeda_probe.c, needs andrslider=1) as the sysctls
 *                     below; (hall42, hall114) = (0, 1) is "open", the
 *                     meaning the LineageOS KeyHandler gives it
 *                     (andromeda_slider.h, docs/research/sensori-vibrazione.md
 *                     1.3).
 *   andrslidercam=2   the same with the other polarity: (1, 0) is "open".
 *   absent or 0       off: both sysctls answer ENOENT, as if absent.
 *
 * Runtime control (andromeda_ctl.c, andrctl=1): "slidercam N" sets the same
 * mode while iOS runs, so that the polarity can be tried without a reboot.
 * The last reading is decoded again with the new polarity and published
 * with a new generation (andromeda_slider_repolarize): no opening is
 * counted by the switch itself. The reader is "slider 1" (andrslider=1).
 *
 * Sysctls (64-bit value, layout in andromeda_slider.h):
 *   andromeda.slider       read only, never blocks.
 *   andromeda.slider_wait  root only. Write the value last read (only its
 *                          generation matters): the call returns when the
 *                          generation differs, after 60 s at the latest or
 *                          on a signal, and reads back the current value.
 *                          A read without a write does not block. This
 *                          lets the daemon sleep instead of polling.
 *
 * No hardware access here: ap_slider_thread() (andromeda_probe.c) reads the
 * two TLMM input bits every 100 ms and calls andromeda_slider_publish() on
 * every change. The handlers are CTLFLAG_LOCKED: sysctl_root() has dropped
 * the geometry lock and takes no other lock before calling them, so
 * sleeping in slider_wait does not hold up other sysctls.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include "andromeda_slider.h"

void andromeda_slider_publish(unsigned h42, unsigned h114);
int andromeda_slider_mode_ctl(unsigned mode, char *out, size_t len);
unsigned andromeda_slider_mode(void);

#define AS_TAG     "ANDROMEDA SLIDERCAM:"
#define AS_WAIT_S  60u
#define AS_LOG_MAX 100u

static LCK_GRP_DECLARE(as_lck_grp, "andromeda_slider");
static LCK_MTX_DECLARE(as_lock, &as_lck_grp);

static int as_mode = -1;                        /* -1: boot-arg not read yet */
static uint64_t as_value = ANDROMEDA_SLIDER_INIT; /* written under as_lock */
static unsigned as_stable = ANDROMEDA_SLIDER_NONE; /* under as_lock */
static unsigned as_logged;                      /* under as_lock */
static unsigned as_raw = 0xffu;                 /* last reading, even while off; 0xff none */

/* 0 off, otherwise the polarity (1 or 2). */
static unsigned
as_polarity(void)
{
	int mode = __atomic_load_n(&as_mode, __ATOMIC_ACQUIRE);
	if (mode < 0) {
		unsigned arg = 0;
		(void)PE_parse_boot_argn("andrslidercam", &arg, sizeof(arg));
		mode = (arg == ANDROMEDA_SLIDER_POLARITY_KEYHANDLER ||
		    arg == ANDROMEDA_SLIDER_POLARITY_INVERTED) ? (int)arg : 0;
		__atomic_store_n(&as_mode, mode, __ATOMIC_RELEASE);
	}
	return (unsigned)mode;
}

void
andromeda_slider_publish(unsigned h42, unsigned h114)
{
	__atomic_store_n(&as_raw, ((h42 & 1u) << 1) | (h114 & 1u), __ATOMIC_RELEASE);
	/* Under as_lock: a "slidercam N" in between cannot be overwritten by a
	 * value decoded with the old polarity. */
	lck_mtx_lock(&as_lock);
	unsigned polarity = as_polarity();
	if (polarity == 0u) {
		lck_mtx_unlock(&as_lock);
		return;
	}
	unsigned before = as_stable;
	uint64_t v = andromeda_slider_next(as_value, &as_stable, polarity, h42, h114);
	__atomic_store_n(&as_value, v, __ATOMIC_RELEASE);
	bool first = andromeda_slider_generation(v) == 1u;
	bool report = as_stable != before && as_logged < AS_LOG_MAX;
	if (report) {
		as_logged++;
	}
	wakeup(&as_value);
	lck_mtx_unlock(&as_lock);

	if (first) {
		unsigned open_raw = andromeda_slider_open_raw(polarity);
		printf(AS_TAG " andromeda.slider on: open = hall42=%u hall114=%u (andrslidercam=%u)\n",
		    open_raw >> 1, open_raw & 1u, polarity);
	}
	if (report) {
		printf(AS_TAG " %s (generation %u, opens %u)\n",
		    andromeda_slider_name(andromeda_slider_state(v)),
		    andromeda_slider_generation(v), andromeda_slider_opens(v));
	}
}

unsigned
andromeda_slider_mode(void)
{
	return as_polarity();
}

int
andromeda_slider_mode_ctl(unsigned mode, char *out, size_t len)
{
	if (mode != 0u && mode != ANDROMEDA_SLIDER_POLARITY_KEYHANDLER &&
	    mode != ANDROMEDA_SLIDER_POLARITY_INVERTED) {
		snprintf(out, len, "mode must be 0, 1 or 2");
		return EINVAL;
	}
	(void)as_polarity();    /* the boot-arg is read first, so it never overrides this later */
	lck_mtx_lock(&as_lock);
	unsigned old = (unsigned)__atomic_load_n(&as_mode, __ATOMIC_ACQUIRE);
	uint64_t v = as_value;
	if (mode != 0u) {
		v = andromeda_slider_repolarize(as_value, &as_stable, mode,
		    __atomic_load_n(&as_raw, __ATOMIC_ACQUIRE));
		__atomic_store_n(&as_value, v, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&as_mode, (int)mode, __ATOMIC_RELEASE);
	wakeup(&as_value);
	lck_mtx_unlock(&as_lock);
	if (mode == 0u) {
		printf(AS_TAG " off (andrctl; was andrslidercam=%u)\n", old);
		snprintf(out, len, "andromeda.slider off (the sysctls answer ENOENT)");
	} else {
		unsigned open_raw = andromeda_slider_open_raw(mode);
		printf(AS_TAG " andrslidercam=%u (andrctl, was %u): open = hall42=%u hall114=%u; now %s\n",
		    mode, old, open_raw >> 1, open_raw & 1u,
		    andromeda_slider_name(andromeda_slider_state(v)));
		snprintf(out, len, "open = hall42=%u hall114=%u; slider now %s (generation %u)",
		    open_raw >> 1, open_raw & 1u, andromeda_slider_name(andromeda_slider_state(v)),
		    andromeda_slider_generation(v));
	}
	return 0;
}

static int
as_sysctl_slider SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (as_polarity() == 0u) {
		return ENOENT;
	}
	uint64_t v = __atomic_load_n(&as_value, __ATOMIC_ACQUIRE);
	return SYSCTL_OUT(req, &v, sizeof(v));
}

static int
as_sysctl_slider_wait SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (as_polarity() == 0u) {
		return ENOENT;
	}
	if (req->newptr != USER_ADDR_NULL) {
		uint64_t seen = 0, deadline = 0;
		if (req->newlen != sizeof(seen)) {
			return EINVAL;
		}
		int error = SYSCTL_IN(req, &seen, sizeof(seen));
		if (error) {
			return error;
		}
		clock_interval_to_deadline(AS_WAIT_S, NSEC_PER_SEC, &deadline);
		lck_mtx_lock(&as_lock);
		while (andromeda_slider_generation(as_value) == andromeda_slider_generation(seen)) {
			/* Drops as_lock while asleep, holds it again on return. */
			error = msleep1(&as_value, &as_lock, PZERO | PCATCH, "andrslider", deadline);
			if (error) {
				break;
			}
		}
		lck_mtx_unlock(&as_lock);
		/*
		 * Timeout: report the unchanged value. EWOULDBLOCK is EAGAIN, which
		 * userland_sysctl() would turn into an endless retry. EINTR or
		 * ERESTART (signal) go back to the caller.
		 */
		if (error && error != EWOULDBLOCK) {
			return error;
		}
	}
	uint64_t v = __atomic_load_n(&as_value, __ATOMIC_ACQUIRE);
	return SYSCTL_OUT(req, &v, sizeof(v));
}

/* Other andromeda drivers can hang their sysctls here: SYSCTL_DECL(_andromeda). */
SYSCTL_NODE(, OID_AUTO, andromeda, CTLFLAG_RW | CTLFLAG_LOCKED, 0,
    "Xiaomi Mi Mix 3 5G (andromeda) drivers");

SYSCTL_PROC(_andromeda, OID_AUTO, slider,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, as_sysctl_slider, "QU",
    "camera slider: generation << 32 | opens << 16 | raw Halls << 8 | state (andrslidercam)");

SYSCTL_PROC(_andromeda, OID_AUTO, slider_wait,
    CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, as_sysctl_slider_wait, "QU",
    "write the andromeda.slider value last seen: returns when it changes, or after 60 s");
#endif /* ARM64_BOARD_CONFIG_SWEET */
