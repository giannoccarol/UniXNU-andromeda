/*
 * Mi Mix 3 5G (andromeda, SM8150): runtime control of the andromeda drivers
 * from the PC while iOS runs, without a reboot or a new image. Off unless
 * the boot-arg andrctl=1.
 *
 * A kernel UDP socket on port 5141 takes one command per datagram and
 * answers on the same flow with one line, "ok VERB ...: what happens" or
 * "no VERB ...: why not", which also goes to the log as
 * "ANDROMEDA CTL: ...". The PC side is scripts/andctl.py; since the answer
 * is a reply to the PC's own datagram, a stateful firewall (ufw) lets it in
 * without any rule.
 *
 * Perimeter:
 *   - only datagrams received on ue0, the USB Ethernet of sweet_usb.c
 *     (checked with IP_RECVIF), from 192.168.7.1, the PC end of that link;
 *     anything else is dropped without an answer;
 *   - only the closed list of andromeda_ctl.h (andr_ctl_parse): printable
 *     ASCII, at most 96 bytes, fixed words and small decimal values; no
 *     address, register, path or free text reaches a driver;
 *   - at most 8 commands at once, then one every 250 ms;
 *   - a command does what the boot-arg of the same name does, through the
 *     same code and the same checks: a level that already ran does not run
 *     again, a failed run is not retried, the thermal and voltage limits
 *     stay, the GPU ZAP (gpu >= 3) and the modem boot (wlan 2) both go
 *     through TrustZone PAS and never share a boot (andromeda_pas_claim),
 *     and the hardware runs of gpu, bt, cam, hap and wlan take turns
 *     (andromeda_ctl_begin), whether started here or by their boot-arg,
 *     with the SE13 clock vote of andrprobe=2 among them.
 *
 * Commands (grammar and ranges in andromeda_ctl.h):
 *   status, help
 *   gpu 1-4, bt 1-2, hap 1, cam 1, wlan 1-2, bl 1-2   start now at level N
 *       (a boot-arg run still in its start delay starts at once, at the
 *       higher of the two levels; gpu and wlan continue a passed run with
 *       the next levels; bl 2 may follow bl 1; bt, cam and hap run once)
 *   pwr 0|1                       statistics lines (andrpwr) off/on
 *   idle cpu|fb|usb|keys|touch V  a P1 saving (andridle bit 0, andrfbpark,
 *       andrusbidle, andrkeysidle, andrtouchidle), 0 = off; "idle" shows them
 *   slider 1                      start the slider reader (andrslider=1)
 *   slidercam 0-2                 slider polarity for andromedad (andrslidercam)
 *   fastboot                      reboot into fastboot now (andrfastboot)
 *
 * The kernel socket and the USB link addresses follow ios-sweet's live log
 * (patch 0077-sweet-usb-live-log, dipa-studio, MIT); the socket KPI
 * (sock_socket, sock_bind, sock_receive with control data, sock_send) and
 * IP_RECVIF are those of apple-oss-distributions/xnu xnu-8796.141.3
 * (bsd/kern/kpi_socket.c, bsd/netinet/ip_input.c ip_savecontrol). See
 * CREDITS.md.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/kpi_socket.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include "andromeda_ctl.h"

#define CTL_TAG "ANDROMEDA CTL:"

extern kern_return_t thread_terminate(thread_t);

/* Shared with the drivers (declared there). */
void andromeda_ctl_init(void);
void andromeda_ctl_delay(uint32_t *st, unsigned seconds);
bool andromeda_ctl_is_boot(uint32_t *st);
uint32_t andromeda_ctl_begin(uint32_t *st, const char *who);
void andromeda_ctl_end(uint32_t *st, unsigned reached, bool failed, const char *who);
void andromeda_ctl_turn(const char *who);
void andromeda_ctl_turn_end(void);
bool andromeda_ctl_spawn(thread_continue_t fn);
bool andromeda_pas_claim(unsigned who);
unsigned andromeda_pas_owner(void);

/* andromeda_pas_claim() owners. */
#define ANDR_PAS_GPU  1u
#define ANDR_PAS_WLAN 2u

/* ---- the drivers' side (each file documents its own) ---- */

extern uint32_t andromeda_gpu_req, andromeda_bt_req, andromeda_hap_req, andromeda_cam_req,
    andromeda_wlan_req, andromeda_bl_req;
extern const char *andromeda_gpu_check(unsigned level);
extern const char *andromeda_bt_check(unsigned level);
extern const char *andromeda_hap_check(unsigned level);
extern const char *andromeda_cam_check(unsigned level);
extern const char *andromeda_wlan_check(unsigned level);
extern const char *andromeda_bl_check(unsigned level);
extern bool andromeda_gpu_spawn(void);
extern bool andromeda_bt_spawn(void);
extern bool andromeda_hap_spawn(void);
extern bool andromeda_cam_spawn(void);
extern bool andromeda_wlan_spawn(void);
extern bool andromeda_bl_spawn(void);
/* andromeda_power.c */
extern int andromeda_power_ctl(unsigned on, char *out, size_t len);
extern int andromeda_idle_ctl(unsigned which, unsigned value, char *out, size_t len);
extern void andromeda_power_ctl_status(char *out, size_t len);
/* andromeda_probe.c */
extern int andromeda_probe_slider_ctl(char *out, size_t len);
extern int andromeda_probe_fastboot_ctl(char *out, size_t len);
extern int andromeda_fbsync_ctl(unsigned on, char *out, size_t len);	/* andromeda_gpu_mtl.c, patch 0201 */
extern bool andromeda_probe_slider_on(void);
/* andromeda_slider.c */
extern int andromeda_slider_mode_ctl(unsigned mode, char *out, size_t len);
extern unsigned andromeda_slider_mode(void);

struct ctl_unit {
	enum andr_verb verb;
	const char *name;
	uint32_t *st;
	unsigned max;
	bool steps;             /* a higher level may follow a run that passed */
	unsigned pas_from;      /* first level that goes through TrustZone PAS, 0 never */
	unsigned pas_who;
	const char *(*check)(unsigned level);
	bool (*spawn)(void);
};

static const struct ctl_unit ctl_units[] = {
	{ ANDR_V_GPU,  "gpu",  &andromeda_gpu_req,  4, true,  3, ANDR_PAS_GPU,  andromeda_gpu_check,  andromeda_gpu_spawn },
	{ ANDR_V_BT,   "bt",   &andromeda_bt_req,   2, false, 0, 0,             andromeda_bt_check,   andromeda_bt_spawn },
	{ ANDR_V_HAP,  "hap",  &andromeda_hap_req,  1, false, 0, 0,             andromeda_hap_check,  andromeda_hap_spawn },
	{ ANDR_V_CAM,  "cam",  &andromeda_cam_req,  1, false, 0, 0,             andromeda_cam_check,  andromeda_cam_spawn },
	{ ANDR_V_WLAN, "wlan", &andromeda_wlan_req, 2, true,  2, ANDR_PAS_WLAN, andromeda_wlan_check, andromeda_wlan_spawn },
	{ ANDR_V_BL,   "bl",   &andromeda_bl_req,   2, true,  0, 0,             andromeda_bl_check,   andromeda_bl_spawn },
};
#define CTL_UNITS ((unsigned)(sizeof(ctl_units) / sizeof(ctl_units[0])))

/* ---- helpers for the drivers ---- */

static LCK_GRP_DECLARE(ctl_grp, "andromeda_ctl");
static LCK_MTX_DECLARE(ctl_run_lock, &ctl_grp);
static const char *volatile ctl_run_owner;
static unsigned ctl_pas;

static const char *
ctl_pas_name(unsigned who)
{
	return who == ANDR_PAS_GPU ? "the GPU ZAP (gpu >= 3)" :
	       who == ANDR_PAS_WLAN ? "the modem boot (wlan 2)" : "nobody";
}

bool
andromeda_pas_claim(unsigned who)
{
	return andr_claim(&ctl_pas, who);
}

/* Undo a claim this request has just taken and then did not use. */
static void
ctl_pas_unclaim(unsigned who)
{
	unsigned expected = who;
	(void)__atomic_compare_exchange_n(&ctl_pas, &expected, 0u, false, __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE);
}

unsigned
andromeda_pas_owner(void)
{
	return __atomic_load_n(&ctl_pas, __ATOMIC_ACQUIRE);
}

bool
andromeda_ctl_is_boot(uint32_t *st)
{
	return andr_st_phase(__atomic_load_n(st, __ATOMIC_ACQUIRE)) == ANDR_PH_WAIT;
}

/*
 * The boot-arg start delay: up to seconds, cut short when the control sets
 * ANDR_ST_GO (it then calls thread_wakeup(st)). Returns at once for a run
 * the control started. Same wait/wakeup pattern as andromeda_display_wait().
 */
void
andromeda_ctl_delay(uint32_t *st, unsigned seconds)
{
	uint64_t deadline;
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	for (;;) {
		uint32_t s = __atomic_load_n(st, __ATOMIC_ACQUIRE);
		if (andr_st_phase(s) != ANDR_PH_WAIT || (s & ANDR_ST_GO) ||
		    mach_absolute_time() >= deadline) {
			return;
		}
		(void)assert_wait_deadline((event_t)st, THREAD_UNINT, deadline);
		s = __atomic_load_n(st, __ATOMIC_ACQUIRE);
		if (andr_st_phase(s) != ANDR_PH_WAIT || (s & ANDR_ST_GO)) {
			thread_wakeup((event_t)st);     /* set meanwhile: do not sleep */
		}
		(void)thread_block(THREAD_CONTINUE_NULL);
	}
}

/*
 * The hardware turn: one of the gpu, bt, cam, hap and wlan runs (and the
 * SE13 clock vote of andrprobe=2, andromeda_probe.c) at a time.
 */
void
andromeda_ctl_turn(const char *who)
{
	if (!lck_mtx_try_lock(&ctl_run_lock)) {
		const char *owner = ctl_run_owner;
		printf(CTL_TAG " %s waits for %s to finish (one hardware run at a time)\n", who,
		    owner != NULL ? owner : "another run");
		lck_mtx_lock(&ctl_run_lock);
	}
	ctl_run_owner = who;
}

void
andromeda_ctl_turn_end(void)
{
	ctl_run_owner = NULL;
	lck_mtx_unlock(&ctl_run_lock);
}

/*
 * A run starts: the state becomes RUN (andr_begin_word) and the returned
 * word says which levels to run (andr_st_from .. andr_st_level). With who
 * set, the run first waits its turn behind any other hardware run.
 */
uint32_t
andromeda_ctl_begin(uint32_t *st, const char *who)
{
	uint32_t cur = __atomic_load_n(st, __ATOMIC_ACQUIRE), next;
	do {
		next = andr_begin_word(cur);
	} while (!__atomic_compare_exchange_n(st, &cur, next, false, __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE));
	if (who != NULL) {
		andromeda_ctl_turn(who);
	}
	return next;
}

/* A run ends: levels up to reached passed; failed if it stopped on an error. */
void
andromeda_ctl_end(uint32_t *st, unsigned reached, bool failed, const char *who)
{
	__atomic_store_n(st, andr_done_word(reached, failed), __ATOMIC_RELEASE);
	if (who != NULL) {
		andromeda_ctl_turn_end();
	}
}

bool
andromeda_ctl_spawn(thread_continue_t fn)
{
	thread_t thread;
	if (kernel_thread_start(fn, NULL, &thread) != KERN_SUCCESS) {
		return false;
	}
	thread_deallocate(thread);
	return true;
}

/* ---- commands ---- */

static char ctl_out[768];       /* the answer; only the control thread writes it */
static char ctl_why[256];       /* a handler's explanation */
static unsigned ctl_ok, ctl_no, ctl_dropped, ctl_limited;

static uint64_t
ctl_now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* "gpu 2" or "idle keys 50": the command as understood. */
static void
ctl_name(const struct andr_cmd *c, char *out, size_t len)
{
	const char *verb = c->verb < ANDR_V_COUNT ? andr_verbs[c->verb].name : "?";
	if (c->verb == ANDR_V_IDLE && c->idle < ANDR_IDLE_COUNT) {
		snprintf(out, len, "%s %s %u", verb, andr_idles[c->idle].name, c->value);
	} else if (c->has_value) {
		snprintf(out, len, "%s %u", verb, c->value);
	} else {
		snprintf(out, len, "%s", verb);
	}
}

static bool
ctl_unit_request(const struct ctl_unit *u, unsigned want)
{
	const char *why = u->check != NULL ? u->check(want) : NULL;
	if (why != NULL) {
		snprintf(ctl_why, sizeof(ctl_why), "%s", why);
		return false;
	}
	for (unsigned tries = 0; tries < 4; ++tries) {
		uint32_t cur = __atomic_load_n(u->st, __ATOMIC_ACQUIRE), next = 0;
		enum andr_act act = andr_decide(cur, want, u->max, u->steps, &next);
		unsigned level = andr_st_level(cur);
		switch (act) {
		case ANDR_ACT_RANGE:
			snprintf(ctl_why, sizeof(ctl_why), "level must be 1..%u", u->max);
			return false;
		case ANDR_ACT_BUSY:
			snprintf(ctl_why, sizeof(ctl_why), "running now (levels %u..%u); see the log",
			    andr_st_from(cur), level);
			return false;
		case ANDR_ACT_DONE:
			snprintf(ctl_why, sizeof(ctl_why), "level %u already passed this boot%s", level,
			    u->steps ? "" : "; it runs once per boot");
			return false;
		case ANDR_ACT_ONCE:
			snprintf(ctl_why, sizeof(ctl_why), "it ran this boot (level %u passed) and runs "
			    "once per boot", level);
			return false;
		case ANDR_ACT_FAILED:
			snprintf(ctl_why, sizeof(ctl_why), "the last run failed (levels up to %u passed): "
			    "not retried, reboot to try again", level);
			return false;
		default:
			break;
		}
		unsigned top = andr_st_level(next);
		/* PAS before the swap (a woken boot run may start at once); a claim
		 * this request took is given back if the request is not carried out. */
		bool fresh = false;
		if (u->pas_from && top >= u->pas_from) {
			fresh = andromeda_pas_owner() == 0u;
			if (!andromeda_pas_claim(u->pas_who)) {
				snprintf(ctl_why, sizeof(ctl_why), "TrustZone PAS is taken by %s this boot; "
				    "the GPU ZAP and the modem boot never share a boot",
				    ctl_pas_name(andromeda_pas_owner()));
				return false;
			}
		}
		if (!__atomic_compare_exchange_n(u->st, &cur, next, false, __ATOMIC_ACQ_REL,
		    __ATOMIC_ACQUIRE)) {
			if (fresh) {
				ctl_pas_unclaim(u->pas_who);
			}
			continue;       /* the driver moved meanwhile: decide again */
		}
		if (act == ANDR_ACT_NOW) {
			thread_wakeup((event_t)u->st);
			snprintf(ctl_why, sizeof(ctl_why), "the boot-arg run starts now instead of after "
			    "its delay, levels 1..%u", top);
			return true;
		}
		if (!u->spawn()) {
			__atomic_store_n(u->st, cur, __ATOMIC_RELEASE);
			if (fresh) {
				ctl_pas_unclaim(u->pas_who);
			}
			snprintf(ctl_why, sizeof(ctl_why), "kernel_thread_start failed");
			return false;
		}
		snprintf(ctl_why, sizeof(ctl_why), "%s levels %u..%u now (after any other hardware "
		    "run in progress)", act == ANDR_ACT_MORE ? "continuing with" : "running",
		    andr_st_from(next), top);
		return true;
	}
	snprintf(ctl_why, sizeof(ctl_why), "the driver changed state while asked; ask again");
	return false;
}

static void
ctl_status(void)
{
	char pwr[160];
	uint64_t ns;
	int n;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	n = snprintf(ctl_why, sizeof(ctl_why), "up %llu s;", (unsigned long long)(ns / NSEC_PER_SEC));
	for (unsigned i = 0; i < CTL_UNITS && n > 0 && (size_t)n < sizeof(ctl_why); ++i) {
		uint32_t s = __atomic_load_n(ctl_units[i].st, __ATOMIC_ACQUIRE);
		if (andr_st_phase(s) == ANDR_PH_NONE) {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s -", ctl_units[i].name);
		} else if (andr_st_phase(s) == ANDR_PH_RUN) {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s run:%u-%u",
			    ctl_units[i].name, andr_st_from(s), andr_st_level(s));
		} else {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s %s:%u%s",
			    ctl_units[i].name, andr_st_name(s), andr_st_level(s),
			    (s & ANDR_ST_GO) ? "!" : "");
		}
	}
	andromeda_power_ctl_status(pwr, sizeof(pwr));
	if (n > 0 && (size_t)n < sizeof(ctl_why)) {
		unsigned pas = andromeda_pas_owner();
		(void)snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n,
		    "; pas %s; %s; slider %s cam %u; ctl ok %u no %u dropped %u",
		    pas == ANDR_PAS_GPU ? "gpu" : pas == ANDR_PAS_WLAN ? "wlan" : "-", pwr,
		    andromeda_probe_slider_on() ? "on" : "off", andromeda_slider_mode(), ctl_ok, ctl_no,
		    ctl_dropped + ctl_limited);
	}
}

static void
ctl_help(void)
{
	int n = snprintf(ctl_why, sizeof(ctl_why), "commands:");
	for (unsigned v = 0; v < ANDR_V_COUNT && n > 0 && (size_t)n < sizeof(ctl_why); ++v) {
		const struct andr_range *r = &andr_verbs[v];
		if (v == ANDR_V_IDLE) {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " idle [cpu 0-1|fb 0-1|"
			    "usb 0,2-20|keys 0,20-200|touch 0,20-1000],");
		} else if (!r->arg) {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s,", r->name);
		} else if (r->min == r->max) {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s %u,", r->name, r->min);
		} else {
			n += snprintf(ctl_why + n, sizeof(ctl_why) - (size_t)n, " %s %u-%u,", r->name,
			    r->min, r->max);
		}
	}
	if (n > 1 && (size_t)n < sizeof(ctl_why) && ctl_why[n - 1] == ',') {
		ctl_why[n - 1] = '\0';
	}
}

static bool
ctl_idle_show(void)
{
	char pwr[160];
	andromeda_power_ctl_status(pwr, sizeof(pwr));
	snprintf(ctl_why, sizeof(ctl_why), "%s", pwr);
	return true;
}

/*
 * "shot N": a thumbnail of what the panel shows, into the log, so the screen
 * can be looked at from the PC (2026-11-07: Metal frames with glitches).
 * The panel framebuffer the UEFI set up (SweetFramebuffer: AppleCLCD
 * 1080x2340, stride 4320, BGRA at 0x9c000000) is read through a device
 * mapping; each N x N block is the average of a 4 x 4 grid of samples. One
 * line per row of blocks and per 30 blocks: "ANDROMEDA SHOT row.part: RRGGBB...",
 * header and footer lines give the size; scripts/andshot.py makes a PNG.
 * Read only.
 */
#define CTL_PANEL_PA     0x9c000000u
#define CTL_PANEL_W      1080u
#define CTL_PANEL_H      2340u
#define CTL_PANEL_STRIDE 4320u
#define CTL_SHOT_CHUNK   30u
extern vm_offset_t andromeda_io_map(vm_offset_t pa, vm_size_t size);
static void ctl_sleep_ms(unsigned ms);

static bool
ctl_shot(unsigned block)
{
	vm_offset_t fb = andromeda_io_map(CTL_PANEL_PA, (vm_size_t)CTL_PANEL_STRIDE * CTL_PANEL_H);
	if (!fb) {
		snprintf(ctl_why, sizeof(ctl_why), "panel framebuffer not mapped");
		return false;
	}
	unsigned cols = CTL_PANEL_W / block, rows = CTL_PANEL_H / block;
	static unsigned shot_no;
	unsigned no = ++shot_no;
	printf("ANDROMEDA SHOT begin %u: %ux%u blocks of %u px (panel %ux%u)\n", no, cols, rows, block,
	    CTL_PANEL_W, CTL_PANEL_H);
	char line[16 + 6 * CTL_SHOT_CHUNK + 4];
	for (unsigned r = 0; r < rows; ++r) {
		for (unsigned c0 = 0; c0 < cols; c0 += CTL_SHOT_CHUNK) {
			int n = 0;
			for (unsigned c = c0; c < cols && c < c0 + CTL_SHOT_CHUNK; ++c) {
				uint32_t sr = 0, sg = 0, sb = 0;
				for (unsigned sy = 0; sy < 4; ++sy) {
					for (unsigned sx = 0; sx < 4; ++sx) {
						unsigned x = c * block + (2 * sx + 1) * block / 8;
						unsigned y = r * block + (2 * sy + 1) * block / 8;
						uint32_t px = *(volatile uint32_t *)(fb + (vm_offset_t)y * CTL_PANEL_STRIDE + 4u * x);
						sb += px & 0xffu;
						sg += (px >> 8) & 0xffu;
						sr += (px >> 16) & 0xffu;
					}
				}
				n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x%02x%02x", sr / 16, sg / 16, sb / 16);
			}
			printf("ANDROMEDA SHOT %u %03u.%u: %s\n", no, r, c0 / CTL_SHOT_CHUNK, line);
		}
		if ((r % 8) == 7) {
			ctl_sleep_ms(10);   /* let the USB log drain */
		}
	}
	printf("ANDROMEDA SHOT end %u\n", no);
	snprintf(ctl_why, sizeof(ctl_why), "shot %u: %ux%u blocks of %u px in the log", no, cols, rows, block);
	return true;
}

/* Run one parsed command; ctl_why gets the explanation. */
static bool
ctl_execute(const struct andr_cmd *c)
{
	ctl_why[0] = '\0';
	switch (c->verb) {
	case ANDR_V_STATUS:
		ctl_status();
		return true;
	case ANDR_V_HELP:
		ctl_help();
		return true;
	case ANDR_V_GPU:
	case ANDR_V_BT:
	case ANDR_V_HAP:
	case ANDR_V_CAM:
	case ANDR_V_WLAN:
	case ANDR_V_BL:
		for (unsigned i = 0; i < CTL_UNITS; ++i) {
			if (ctl_units[i].verb == c->verb) {
				return ctl_unit_request(&ctl_units[i], c->value);
			}
		}
		snprintf(ctl_why, sizeof(ctl_why), "no such driver");
		return false;
	case ANDR_V_PWR:
		return andromeda_power_ctl(c->value, ctl_why, sizeof(ctl_why)) == 0;
	case ANDR_V_IDLE:
		if (c->idle == ANDR_IDLE_COUNT) {
			return ctl_idle_show();
		}
		return andromeda_idle_ctl((unsigned)c->idle, c->value, ctl_why, sizeof(ctl_why)) == 0;
	case ANDR_V_SLIDER:
		return andromeda_probe_slider_ctl(ctl_why, sizeof(ctl_why)) == 0;
	case ANDR_V_SLIDERCAM:
		return andromeda_slider_mode_ctl(c->value, ctl_why, sizeof(ctl_why)) == 0;
	case ANDR_V_FASTBOOT:
		return andromeda_probe_fastboot_ctl(ctl_why, sizeof(ctl_why)) == 0;
	case ANDR_V_SHOT:
		return ctl_shot(c->value);
	case ANDR_V_FBSYNC:
		return andromeda_fbsync_ctl(c->value, ctl_why, sizeof(ctl_why)) == 0;
	default:
		snprintf(ctl_why, sizeof(ctl_why), "unknown command");
		return false;
	}
}

/* Answer for one datagram that passed the perimeter. */
static void
ctl_answer(const char *buf, size_t len, bool truncated)
{
	struct andr_cmd c;
	char name[48];
	bzero(&c, sizeof(c));
	c.verb = ANDR_V_COUNT;
	enum andr_perr e = truncated ? ANDR_P_LONG : andr_ctl_parse(buf, len, &c);
	if (e != ANDR_P_OK) {
		++ctl_no;
		if (e == ANDR_P_RANGE && c.verb < ANDR_V_COUNT) {
			const struct andr_range *r = c.verb == ANDR_V_IDLE && c.idle < ANDR_IDLE_COUNT ?
			    &andr_idles[c.idle] : &andr_verbs[c.verb];
			ctl_name(&c, name, sizeof(name));
			snprintf(ctl_out, sizeof(ctl_out), "no %s: accepted %s%u..%u", name,
			    r->zero ? "0 or " : "", r->min, r->max);
		} else {
			snprintf(ctl_out, sizeof(ctl_out), "no: %s", andr_perr_name(e));
		}
		return;
	}
	ctl_name(&c, name, sizeof(name));
	bool ok = ctl_execute(&c);
	if (ok) {
		++ctl_ok;
	} else {
		++ctl_no;
	}
	snprintf(ctl_out, sizeof(ctl_out), "%s %s: %s", ok ? "ok" : "no", name, ctl_why);
}

/* ---- socket ---- */

static void
ctl_sleep_ms(unsigned ms)
{
	static unsigned chan;
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&chan, THREAD_UNINT, deadline);
	(void)thread_block(THREAD_CONTINUE_NULL);
}

/* The receiving interface's name from the IP_RECVIF control message; 0 if absent. */
static unsigned
ctl_recvif(struct msghdr *msg, char *name, size_t size)
{
	size_t head = __builtin_offsetof(struct sockaddr_dl, sdl_data);
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm)) {
		if (cm->cmsg_level != IPPROTO_IP || cm->cmsg_type != IP_RECVIF ||
		    cm->cmsg_len < CMSG_LEN(head)) {
			continue;
		}
		const struct sockaddr_dl *sdl = (const struct sockaddr_dl *)(const void *)CMSG_DATA(cm);
		unsigned n = sdl->sdl_nlen;
		if (sdl->sdl_family != AF_LINK || n == 0u || n >= size || CMSG_LEN(head + n) > cm->cmsg_len) {
			return 0;
		}
		memcpy(name, sdl->sdl_data, n);
		name[n] = '\0';
		return n;
	}
	return 0;
}

static socket_t
ctl_open(unsigned *tries)
{
	ifnet_t ifp = NULL;
	socket_t so = NULL;
	struct sockaddr_in sin;
	int one = 1;
	if (ifnet_find_by_name(ANDR_CTL_IFNAME, &ifp) != 0 || ifp == NULL) {
		if (++*tries == 120u) {
			printf(CTL_TAG " no %s after 2 minutes; still waiting for the USB link\n",
			    ANDR_CTL_IFNAME);
		}
		return NULL;
	}
	ifnet_release(ifp);
	errno_t err = sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, NULL, &so);
	if (err == 0) {
		err = sock_setsockopt(so, IPPROTO_IP, IP_RECVIF, &one, sizeof(one));
	}
	if (err == 0) {
		bzero(&sin, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = AF_INET;
		sin.sin_port = htons(ANDR_CTL_PORT);
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
		err = sock_bind(so, (struct sockaddr *)&sin);
	}
	if (err != 0) {
		if (so != NULL) {
			sock_close(so);
		}
		if ((*tries)++ % 60u == 0u) {
			printf(CTL_TAG " udp/%u not opened (err %d); retrying every second\n",
			    ANDR_CTL_PORT, err);
		}
		return NULL;
	}
	printf(CTL_TAG " listening on udp/%u: commands from 192.168.7.1 on %s only (andrctl=1)\n",
	    ANDR_CTL_PORT, ANDR_CTL_IFNAME);
	return so;
}

static void
ctl_reply(socket_t so, struct sockaddr_in *to)
{
	static char line[sizeof(ctl_out) + 1];
	size_t n = strlen(ctl_out);
	memcpy(line, ctl_out, n);
	line[n] = '\n';
	struct iovec iov = { .iov_base = line, .iov_len = n + 1 };
	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = to;
	msg.msg_namelen = sizeof(*to);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	size_t sent = 0;
	errno_t err = sock_send(so, &msg, 0, &sent);
	if (err != 0) {
		printf(CTL_TAG " answer not sent (err %d)\n", err);
	}
}

/* One datagram. False if the socket should be opened again. */
static bool
ctl_serve(socket_t so)
{
	static char buf[ANDR_CTL_MAX + 8];
	static uint8_t ctrl[128] __attribute__((aligned(8)));
	static struct andr_rate rate;
	static unsigned rx_errors;
	struct sockaddr_in from;
	char ifname[IFNAMSIZ];
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct msghdr msg;
	size_t got = 0;

	bzero(&from, sizeof(from));
	bzero(&msg, sizeof(msg));
	msg.msg_name = &from;
	msg.msg_namelen = sizeof(from);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);
	errno_t err = sock_receive(so, &msg, 0, &got);
	if (err != 0) {
		if (++rx_errors <= 5u) {
			printf(CTL_TAG " receive error %d\n", err);
		}
		ctl_sleep_ms(100);
		return err != EBADF && err != ENOTCONN && err != EINVAL;
	}
	unsigned nlen = ctl_recvif(&msg, ifname, sizeof(ifname));
	uint32_t peer = ntohl(from.sin_addr.s_addr);
	if (from.sin_family != AF_INET ||
	    !andr_ctl_peer_ok(peer, ntohs(from.sin_port), ifname, nlen)) {
		if (++ctl_dropped <= 10u || ctl_dropped % 100u == 0u) {
			printf(CTL_TAG " dropped a datagram from %u.%u.%u.%u:%u on %s (%u so far): only "
			    "192.168.7.1 on %s is answered\n", (peer >> 24) & 0xffu, (peer >> 16) & 0xffu,
			    (peer >> 8) & 0xffu, peer & 0xffu, ntohs(from.sin_port), nlen ? ifname : "?",
			    ctl_dropped, ANDR_CTL_IFNAME);
		}
		return true;
	}
	if (!andr_rate_take(&rate, ctl_now_ms())) {
		if (++ctl_limited <= 10u || ctl_limited % 100u == 0u) {
			printf(CTL_TAG " too many commands: %u ignored so far (8 at once, then 4 per second)\n",
			    ctl_limited);
		}
		return true;
	}
	ctl_answer(buf, got, (msg.msg_flags & MSG_TRUNC) != 0 || got > ANDR_CTL_MAX + 2u);
	printf(CTL_TAG " %s\n", ctl_out);
	ctl_reply(so, &from);
	return true;
}

static void ctl_thread(void *arg, wait_result_t wr) __attribute__((noreturn));

static void
ctl_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	socket_t so = NULL;
	unsigned tries = 0;
	for (;;) {
		if (so == NULL) {
			so = ctl_open(&tries);
			if (so == NULL) {
				ctl_sleep_ms(1000);
				continue;
			}
		}
		if (!ctl_serve(so)) {
			printf(CTL_TAG " socket closed after errors; opening it again\n");
			sock_close(so);
			so = NULL;
		}
	}
}

void
andromeda_ctl_init(void)
{
	static bool started;
	unsigned on = 0;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrctl", &on, sizeof(on));
	if (on == 0) {
		return;
	}
	if (on != 1) {
		printf(CTL_TAG " andrctl=%u: only 1 exists, not started\n", on);
		return;
	}
	if (!andromeda_ctl_spawn(ctl_thread)) {
		printf(CTL_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
