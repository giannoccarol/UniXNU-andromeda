/*
 * Runtime control of the andromeda drivers (andromeda_ctl.c, patch 0140):
 * the closed command grammar, the request state word every controllable
 * driver keeps, the peer check and the rate limit. Pure C: no hardware
 * access, no allocation, no locks, no libc. Host-tested by
 * test_andromeda_ctl.c. Include the kernel's <sys/param.h> (or <stdint.h>,
 * <stdbool.h> and <stddef.h> on the host) first.
 *
 * Request state word (uint32_t, one per driver, changed only with
 * compare-and-swap):
 *   bits  0-7   level: the highest level asked for (WAIT, RUN) or the
 *               highest level that passed (DONE)
 *   bits  8-15  from: first level of the run in progress (RUN), else 0
 *   bits 16-19  phase: ANDR_PH_NONE (nothing asked), ANDR_PH_WAIT (the
 *               boot-arg thread is in its start delay), ANDR_PH_RUN,
 *               ANDR_PH_DONE
 *   bit  20     ANDR_ST_GO: "start now", set on a WAIT word by the control
 *   bit  21     ANDR_ST_FAILED: the run stopped on a failure (DONE)
 *
 * The rules a runtime request follows (andr_decide) are those of the
 * boot-args: a level that already ran does not run again, a failed run is
 * not retried, and a driver without "steps" runs once per boot.
 */
#ifndef ANDROMEDA_CTL_H
#define ANDROMEDA_CTL_H

#define ANDR_CTL_PORT     5141u
#define ANDR_CTL_PEER     0xc0a80701u   /* 192.168.7.1, host byte order */
#define ANDR_CTL_IFNAME   "ue0"         /* sweet_usb.c USB Ethernet (ios-sweet 0075) */
#define ANDR_CTL_MAX      96u           /* longest command, bytes */
#define ANDR_CTL_BURST    8u            /* commands accepted at once ... */
#define ANDR_CTL_REFILL_MS 250u         /* ... then one every 250 ms */

/* ---- request state word ---- */

#define ANDR_PH_NONE   0u
#define ANDR_PH_WAIT   1u
#define ANDR_PH_RUN    2u
#define ANDR_PH_DONE   3u
#define ANDR_ST_GO     (1u << 20)
#define ANDR_ST_FAILED (1u << 21)

static inline uint32_t
andr_st(unsigned phase, unsigned level, unsigned from)
{
	return ((uint32_t)(phase & 0xfu) << 16) | ((uint32_t)(from & 0xffu) << 8) |
	       (uint32_t)(level & 0xffu);
}

static inline unsigned
andr_st_phase(uint32_t s)
{
	return (unsigned)((s >> 16) & 0xfu);
}

static inline unsigned
andr_st_level(uint32_t s)
{
	return (unsigned)(s & 0xffu);
}

static inline unsigned
andr_st_from(uint32_t s)
{
	return (unsigned)((s >> 8) & 0xffu);
}

static inline bool
andr_st_failed(uint32_t s)
{
	return (s & ANDR_ST_FAILED) != 0;
}

/* "-", "wait", "run", "done" or "fail", for the status line. */
static inline const char *
andr_st_name(uint32_t s)
{
	switch (andr_st_phase(s)) {
	case ANDR_PH_NONE:
		return "-";
	case ANDR_PH_WAIT:
		return "wait";
	case ANDR_PH_RUN:
		return "run";
	default:
		return andr_st_failed(s) ? "fail" : "done";
	}
}

/* What a runtime request does. The first three are accepted. */
enum andr_act {
	ANDR_ACT_START = 0,     /* nothing ran: a new thread runs levels 1..want now */
	ANDR_ACT_NOW,           /* the boot-arg thread is waiting: it starts now */
	ANDR_ACT_MORE,          /* levels 1..L passed: a new thread runs L+1..want */
	ANDR_ACT_RANGE,         /* refused: want outside 1..max */
	ANDR_ACT_BUSY,          /* refused: a run is in progress */
	ANDR_ACT_DONE,          /* refused: want <= L, already passed this boot */
	ANDR_ACT_ONCE,          /* refused: the driver runs once per boot */
	ANDR_ACT_FAILED,        /* refused: the last run failed */
};

static inline bool
andr_act_ok(enum andr_act a)
{
	return a == ANDR_ACT_START || a == ANDR_ACT_NOW || a == ANDR_ACT_MORE;
}

/*
 * Request for level want (1..max) on state st. steps: a higher level may
 * follow a run that passed (GPU steps, Wi-Fi W0 -> W1, brightness test ->
 * service). On an accepted request *next is the word to swap in.
 */
static inline enum andr_act
andr_decide(uint32_t st, unsigned want, unsigned max, bool steps, uint32_t *next)
{
	unsigned level = andr_st_level(st);
	if (want < 1u || want > max || max > 0xffu) {
		return ANDR_ACT_RANGE;
	}
	switch (andr_st_phase(st)) {
	case ANDR_PH_NONE:
		*next = andr_st(ANDR_PH_RUN, want, 1u);
		return ANDR_ACT_START;
	case ANDR_PH_WAIT:
		*next = andr_st(ANDR_PH_WAIT, level > want ? level : want, 0u) | ANDR_ST_GO;
		return ANDR_ACT_NOW;
	case ANDR_PH_RUN:
		return ANDR_ACT_BUSY;
	default:
		if (andr_st_failed(st)) {
			return ANDR_ACT_FAILED;
		}
		if (want <= level) {
			return ANDR_ACT_DONE;
		}
		if (!steps) {
			return ANDR_ACT_ONCE;
		}
		*next = andr_st(ANDR_PH_RUN, want, level + 1u);
		return ANDR_ACT_MORE;
	}
}

/* Word a run thread swaps in when it starts: WAIT (with or without GO)
 * becomes RUN of levels 1..level; a RUN word stored by the control stays. */
static inline uint32_t
andr_begin_word(uint32_t st)
{
	if (andr_st_phase(st) == ANDR_PH_WAIT) {
		return andr_st(ANDR_PH_RUN, andr_st_level(st), 1u);
	}
	return st & ~ANDR_ST_GO;
}

static inline uint32_t
andr_done_word(unsigned reached, bool failed)
{
	return andr_st(ANDR_PH_DONE, reached, 0u) | (failed ? ANDR_ST_FAILED : 0u);
}

/*
 * One-owner claim (TrustZone PAS: the GPU ZAP of andrgpu>=3 and the modem
 * boot of andrwlan=2 never share a boot). who > 0. True if it was free or
 * already who's; it is never released.
 */
static inline bool
andr_claim(unsigned *owner, unsigned who)
{
	unsigned expected = 0u;
	if (__atomic_compare_exchange_n(owner, &expected, who, false, __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE)) {
		return true;
	}
	return expected == who;
}

/* ---- peer and rate ---- */

/* Only 192.168.7.1, from a real port, received on ue0. */
static inline bool
andr_ctl_peer_ok(uint32_t addr, uint16_t port, const char *ifname, unsigned nlen)
{
	static const char want[] = ANDR_CTL_IFNAME;
	if (addr != ANDR_CTL_PEER || port == 0u || ifname == NULL ||
	    nlen != sizeof(want) - 1u) {
		return false;
	}
	for (unsigned i = 0; i < nlen; ++i) {
		if (ifname[i] != want[i]) {
			return false;
		}
	}
	return true;
}

/* Token bucket; zero-initialised = full at the first call. */
struct andr_rate {
	uint64_t last_ms;
	unsigned tokens;
	bool started;
};

static inline bool
andr_rate_take(struct andr_rate *r, uint64_t now_ms)
{
	if (!r->started) {
		r->started = true;
		r->tokens = ANDR_CTL_BURST;
		r->last_ms = now_ms;
	} else if (now_ms > r->last_ms) {
		uint64_t add = (now_ms - r->last_ms) / ANDR_CTL_REFILL_MS;
		if (add >= ANDR_CTL_BURST - r->tokens) {
			r->tokens = ANDR_CTL_BURST;
			r->last_ms = now_ms;
		} else if (add) {
			r->tokens += (unsigned)add;
			r->last_ms += add * ANDR_CTL_REFILL_MS;
		}
	}
	if (r->tokens == 0u) {
		return false;
	}
	r->tokens--;
	return true;
}

/* ---- command grammar ---- */

enum andr_verb {
	ANDR_V_STATUS = 0,
	ANDR_V_HELP,
	ANDR_V_GPU,
	ANDR_V_BT,
	ANDR_V_HAP,
	ANDR_V_CAM,
	ANDR_V_WLAN,
	ANDR_V_BL,
	ANDR_V_PWR,
	ANDR_V_IDLE,
	ANDR_V_SLIDER,
	ANDR_V_SLIDERCAM,
	ANDR_V_FASTBOOT,
	ANDR_V_SHOT,
	ANDR_V_FBSYNC,
	ANDR_V_KEY,
	ANDR_V_TOUCH,
	ANDR_V_SWIPE,
	ANDR_V_MEMBW,
	ANDR_V_DDR,
	ANDR_V_TCOAL,
	ANDR_V_DPU,
	ANDR_V_LITTLE,
	ANDR_V_SCHED,
	ANDR_V_COUNT
};

enum andr_idle {
	ANDR_IDLE_CPU = 0,      /* andridle bit 0: no set/way flush at idle (0101) */
	ANDR_IDLE_FB,           /* andrfbpark: vsync timer parked, display off (0102) */
	ANDR_IDLE_USB,          /* andrusbidle: slow USB event polling, ms (0103) */
	ANDR_IDLE_KEYS,         /* andrkeysidle: key polling, display off, ms (0104) */
	ANDR_IDLE_TOUCH,        /* andrtouchidle: touch polling, display off, ms (0105) */
	ANDR_IDLE_COUNT
};

/* A value is 0 (off) or min..max. */
struct andr_range {
	const char *name;
	unsigned min, max;
	bool zero;              /* 0 is also accepted */
	bool arg;               /* takes a value (verbs) */
	unsigned min_args, max_args;  /* several values (touch, swipe), each in min..max */
};

static const struct andr_range andr_verbs[ANDR_V_COUNT] __attribute__((unused)) = {
	[ANDR_V_STATUS]    = { "status",    0, 0, false, false, 0, 0 },
	[ANDR_V_HELP]      = { "help",      0, 0, false, false, 0, 0 },
	[ANDR_V_GPU]       = { "gpu",       1, 4, false, true, 0, 0 },
	[ANDR_V_BT]        = { "bt",        1, 2, false, true, 0, 0 },
	[ANDR_V_HAP]       = { "hap",       1, 1, false, true, 0, 0 },
	[ANDR_V_CAM]       = { "cam",       1, 1, false, true, 0, 0 },
	[ANDR_V_WLAN]      = { "wlan",      1, 2, false, true, 0, 0 },
	[ANDR_V_BL]        = { "bl",        1, 2, false, true, 0, 0 },
	[ANDR_V_PWR]       = { "pwr",       0, 1, false, true, 0, 0 },
	[ANDR_V_IDLE]      = { "idle",      0, 0, false, true, 0, 0 },  /* NAME VALUE */
	[ANDR_V_SLIDER]    = { "slider",    1, 1, false, true, 0, 0 },
	[ANDR_V_SLIDERCAM] = { "slidercam", 0, 2, false, true, 0, 0 },
	[ANDR_V_FASTBOOT]  = { "fastboot",  0, 0, false, false, 0, 0 },
	[ANDR_V_SHOT]      = { "shot",      4, 60, false, true, 0, 0 },  /* block size, pixels */
	[ANDR_V_FBSYNC]    = { "fbsync",    1, 1, true, true, 0, 0 },
	[ANDR_V_KEY]       = { "key",       1, 3, true, true, 0, 0 },   /* 0 power, 1 vol+, 2 vol-, 3 home */
	[ANDR_V_TOUCH]     = { "touch",     1, 2399, true, true, 2, 2 },  /* X Y, HID canvas 1080x2400 */
	[ANDR_V_SWIPE]     = { "swipe",     1, 2399, true, true, 4, 5 },  /* X1 Y1 X2 Y2 [MS] */
	[ANDR_V_MEMBW]     = { "membw",     0, 0, false, false, 0, 0 },   /* memory bandwidth benchmark */
	[ANDR_V_DDR]       = { "ddr",       1, 1, true, true, 0, 0 },     /* DDR vote: 0 zero, 1 max */
	[ANDR_V_TCOAL]     = { "tcoal",     1, 1, true, true, 0, 0 },     /* XNU timer coalescing 0|1 */
	[ANDR_V_DPU]       = { "dpu",       0, 0, false, false, 0, 0 },   /* display controller dump, read only */
	[ANDR_V_LITTLE]    = { "little",    1, 1, true, true, 0, 0 },     /* Silver cores out (0) / in (1) */
	[ANDR_V_SCHED]     = { "sched",     0, 0, false, false, 0, 0 },   /* scheduler's view of each CPU, read only */
};

/* The clamps of the boot-args of patches 0101-0105, as accepted ranges. */
static const struct andr_range andr_idles[ANDR_IDLE_COUNT] __attribute__((unused)) = {
	[ANDR_IDLE_CPU]   = { "cpu",   1, 1,    true, true, 0, 0 },
	[ANDR_IDLE_FB]    = { "fb",    1, 1,    true, true, 0, 0 },
	[ANDR_IDLE_USB]   = { "usb",   2, 20,   true, true, 0, 0 },
	[ANDR_IDLE_KEYS]  = { "keys",  20, 200, true, true, 0, 0 },
	[ANDR_IDLE_TOUCH] = { "touch", 20, 1000, true, true, 0, 0 },
};

enum andr_perr {
	ANDR_P_OK = 0,
	ANDR_P_EMPTY,           /* nothing but blanks */
	ANDR_P_LONG,            /* more than ANDR_CTL_MAX bytes */
	ANDR_P_CHAR,            /* a byte outside printable ASCII */
	ANDR_P_VERB,            /* not in the list */
	ANDR_P_ARGS,            /* a value missing or one too many */
	ANDR_P_NUMBER,          /* not 1-5 decimal digits */
	ANDR_P_RANGE,           /* outside the accepted range (cmd->verb/idle tell which) */
	ANDR_P_IDLE,            /* unknown saving name */
};

static inline const char *
andr_perr_name(enum andr_perr e)
{
	switch (e) {
	case ANDR_P_OK:
		return "ok";
	case ANDR_P_EMPTY:
		return "empty command";
	case ANDR_P_LONG:
		return "command too long";
	case ANDR_P_CHAR:
		return "only printable ASCII is accepted";
	case ANDR_P_VERB:
		return "unknown command (\"help\" lists them)";
	case ANDR_P_ARGS:
		return "wrong number of values (\"help\" lists them)";
	case ANDR_P_NUMBER:
		return "values are decimal numbers";
	case ANDR_P_RANGE:
		return "value out of range";
	default:
		return "unknown saving (cpu, fb, usb, keys, touch)";
	}
}

struct andr_cmd {
	enum andr_verb verb;
	enum andr_idle idle;    /* ANDR_V_IDLE */
	unsigned value;         /* level or value (the first of args) */
	bool has_value;
	unsigned args[5];       /* touch, swipe */
	unsigned nargs;
};

/* Word [*pos, end) after blanks; false at the end. */
static inline bool
andr_word(const char *buf, size_t len, size_t *pos, size_t *start, size_t *wlen)
{
	size_t i = *pos;
	while (i < len && buf[i] == ' ') {
		++i;
	}
	if (i >= len) {
		*pos = i;
		return false;
	}
	*start = i;
	while (i < len && buf[i] != ' ') {
		++i;
	}
	*wlen = i - *start;
	*pos = i;
	return true;
}

static inline bool
andr_word_is(const char *w, size_t wlen, const char *name)
{
	size_t i = 0;
	for (; i < wlen && name[i] != '\0'; ++i) {
		if (w[i] != name[i]) {
			return false;
		}
	}
	return i == wlen && name[i] == '\0';
}

static inline bool
andr_number(const char *w, size_t wlen, unsigned *out)
{
	unsigned v = 0;
	if (wlen == 0u || wlen > 5u) {
		return false;
	}
	for (size_t i = 0; i < wlen; ++i) {
		if (w[i] < '0' || w[i] > '9') {
			return false;
		}
		v = v * 10u + (unsigned)(w[i] - '0');
	}
	*out = v;
	return true;
}

static inline bool
andr_in_range(const struct andr_range *r, unsigned v)
{
	return (r->zero && v == 0u) || (v >= r->min && v <= r->max);
}

/*
 * One datagram: "VERB", "VERB VALUE" or "idle NAME VALUE", words separated
 * by spaces, one trailing "\n" or "\r\n" allowed. Lower case only.
 */
static inline enum andr_perr
andr_ctl_parse(const char *buf, size_t len, struct andr_cmd *cmd)
{
	size_t pos = 0, w = 0, wl = 0, v = 0, vl = 0, x = 0, xl = 0;
	cmd->verb = ANDR_V_COUNT;
	cmd->idle = ANDR_IDLE_COUNT;
	cmd->value = 0;
	cmd->has_value = false;
	cmd->nargs = 0;
	if (len > 0u && buf[len - 1u] == '\n') {
		--len;
		if (len > 0u && buf[len - 1u] == '\r') {
			--len;
		}
	}
	if (len > ANDR_CTL_MAX) {
		return ANDR_P_LONG;
	}
	for (size_t i = 0; i < len; ++i) {
		if (buf[i] < 0x20 || buf[i] > 0x7e) {
			return ANDR_P_CHAR;
		}
	}
	if (!andr_word(buf, len, &pos, &w, &wl)) {
		return ANDR_P_EMPTY;
	}
	unsigned verb = 0;
	while (verb < ANDR_V_COUNT && !andr_word_is(buf + w, wl, andr_verbs[verb].name)) {
		++verb;
	}
	if (verb == ANDR_V_COUNT) {
		return ANDR_P_VERB;
	}
	cmd->verb = (enum andr_verb)verb;
	const struct andr_range *range = &andr_verbs[verb];
	bool has_v = andr_word(buf, len, &pos, &v, &vl);
	if (cmd->verb == ANDR_V_IDLE) {
		/* "idle" alone shows the savings; else NAME VALUE. */
		if (!has_v) {
			return andr_word(buf, len, &pos, &x, &xl) ? ANDR_P_ARGS : ANDR_P_OK;
		}
		unsigned idle = 0;
		while (idle < ANDR_IDLE_COUNT && !andr_word_is(buf + v, vl, andr_idles[idle].name)) {
			++idle;
		}
		if (idle == ANDR_IDLE_COUNT) {
			return ANDR_P_IDLE;
		}
		cmd->idle = (enum andr_idle)idle;
		range = &andr_idles[idle];
		if (!andr_word(buf, len, &pos, &v, &vl)) {
			return ANDR_P_ARGS;
		}
		has_v = true;
	}
	if (range->max_args) {
		/* Several numbers, each in min..max (or 0). */
		unsigned n = 0;
		if (has_v) {
			for (;;) {
				if (n == range->max_args) {
					return ANDR_P_ARGS;
				}
				if (!andr_number(buf + v, vl, &cmd->args[n])) {
					return ANDR_P_NUMBER;
				}
				if (!andr_in_range(range, cmd->args[n])) {
					return ANDR_P_RANGE;
				}
				++n;
				if (!andr_word(buf, len, &pos, &v, &vl)) {
					break;
				}
			}
		}
		if (n < range->min_args) {
			return ANDR_P_ARGS;
		}
		cmd->nargs = n;
		cmd->value = cmd->args[0];
		cmd->has_value = true;
		return ANDR_P_OK;
	}
	if (has_v != range->arg) {
		return ANDR_P_ARGS;
	}
	if (andr_word(buf, len, &pos, &x, &xl)) {
		return ANDR_P_ARGS;
	}
	if (!has_v) {
		return ANDR_P_OK;
	}
	if (!andr_number(buf + v, vl, &cmd->value)) {
		return ANDR_P_NUMBER;
	}
	cmd->has_value = true;
	return andr_in_range(range, cmd->value) ? ANDR_P_OK : ANDR_P_RANGE;
}

#endif /* ANDROMEDA_CTL_H */
