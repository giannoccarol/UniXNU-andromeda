/*
 * Mi Mix 3 5G (andromeda, SM8150): wakeup and idle statistics (phase P0 of
 * docs/research/sospensione.md) and the state the P1 patches share: 0101
 * idle cache flush, 0102 framebuffer vsync, 0103 USB, 0104 keys, 0105 touch.
 *
 * Boot-args (read only: no register is written, no behaviour changes):
 *   andrpwr=1        one ANDROMEDA PWR line every andrpwrsecs seconds
 *                    (default 0 = off; the shared state below always exists)
 *   andrpwrsecs=T    period in seconds, 2..600 (default 10)
 *
 * The line (rates per second over the last period; per-CPU lists are
 * cpu0,cpu1,... and "-" for a CPU that is not running):
 *   ANDROMEDA PWR: t=<s since boot> s=<period> disp=<on|off>
 *     idle=<idle %> irq=<IRQs/s> ipi=<IPIs/s> wfi=<idle entries/s>
 *     long=<% of WFI time in periods >= 4 ms> flush=<avg ns>/<max us>/<% of CPU time>
 *     vsync=<ticks/s> vnotify=<notifications/s> usb=<polls/s>(<% slow>)
 *     keys=<polls/s> touch=<polls/s> bat=<mA>/<mV>/<%>
 * Sources:
 *   idle     processor_info(PROCESSOR_CPU_LOAD_INFO), XNU's own accounting,
 *            as andromeda_cpufreq.c reads it (10 ms ticks: +-0.1 % in 10 s)
 *   irq/ipi  processor_info(PROCESSOR_CPU_STAT64) irq_ex_cnt and ipi_cnt
 *            (osfmk/arm/cpu_common.c); on SWEET the only enabled interrupts
 *            are the virtual timer and SGI 1, so irq = timer + IPI wakeups
 *   wfi/long/flush  cpu_idle() counters of patch 0101 (zero without it):
 *            one WFI entry is one wakeup from idle; "long" is the share of
 *            the time in WFI spent in periods long enough to pay for the
 *            cores' rail power collapse of phase P2 (minimum residency
 *            3934/4488 us, FDT qcom,lpm-levels);
 *            flush is the set/way L1 flush of platform_cache_idle_enter()
 *            (its cost answers whether the hypervisor traps set/way)
 *   vsync..touch  loop counters of patches 0102-0105 (zero without them)
 *   bat      andromeda_battery_sample() of andromeda_probe.c, only once
 *            sweetbattery=2 has set up its SPMI reads: net battery current
 *            (+ = charging), which is the consumption only with the cable
 *            unplugged (docs/research/sospensione.md 3.2)
 * XNU interfaces read from apple-oss-distributions/xnu xnu-8796.141.3
 * (APSL 2.0). The figures to collect come from the plan in
 * docs/research/sospensione.md; see CREDITS.md.
 *
 * Runtime control (andromeda_ctl.c, andrctl=1): "pwr 1" starts the lines
 * (as andrpwr=1, period andrpwrsecs) and "pwr 0" stops them and the idle
 * counting of patch 0101; "idle NAME VALUE" sets one P1 saving
 * (andromeda_power.h), with the ranges the boot-args are clamped to.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <mach/processor_info.h>
#include <pexpert/pexpert.h>
#include "andromeda_power.h"

/* osfmk/kern/processor.c and osfmk/arm/cpu_common.c: not in BSD's headers. */
extern kern_return_t processor_info(processor_t, processor_flavor_t, host_t *,
    processor_info_t, mach_msg_type_number_t *);
extern processor_t cpu_to_processor(int);
/* osfmk/kern/clock.c: one 10 ms tick of PROCESSOR_CPU_LOAD_INFO, Mach units. */
extern uint32_t hz_tick_interval;
/* andromeda_probe.c (patch 0089/0091), read only, -1 until SPMI is set up. */
extern int andromeda_battery_sample(unsigned *percent, unsigned *mv, int *ma,
    bool *external, bool *capable, bool *charging, bool *full);

#define AP_TAG  "ANDROMEDA PWR:"
#define AP_CPUS ANDROMEDA_PWR_CPUS

int andromeda_display_off;
uint64_t andromeda_pwr_fb_vsync;
uint64_t andromeda_pwr_fb_notify;
uint64_t andromeda_pwr_usb_polls;
uint64_t andromeda_pwr_usb_slow;
uint64_t andromeda_pwr_keys_polls;
uint64_t andromeda_pwr_touch_polls;
uint64_t andromeda_pwr_idle[ANDROMEDA_PWR_CPUS * ANDROMEDA_PWR_IDLE_STRIDE] __attribute__((aligned(64)));
int andromeda_pwr_measure;
uint64_t andromeda_pwr_long_abs;
unsigned andromeda_idle_fb_park;
unsigned andromeda_idle_usb_ms;
unsigned andromeda_idle_keys_ms;
unsigned andromeda_idle_touch_ms;
/* osfmk/arm64/cpu.c (patch 0101): TUNABLE_WRITEABLE andridle, bit 0 = no idle flush. */
extern unsigned int andromeda_idle;

int andromeda_power_ctl(unsigned on, char *out, size_t len);
int andromeda_idle_ctl(unsigned which, unsigned value, char *out, size_t len);
void andromeda_power_ctl_status(char *out, size_t len);

struct ap_snap {
	uint64_t at;
	bool     up[AP_CPUS];
	uint32_t idle[AP_CPUS];          /* CPU_STATE_IDLE ticks, wraps */
	uint64_t irq[AP_CPUS], ipi[AP_CPUS];
	uint64_t wfi[AP_CPUS], wfi_abs[AP_CPUS], lng_abs[AP_CPUS];
	uint64_t flush, flush_abs, flush_max;
	uint64_t vsync, vnotify, usb, usb_slow, keys, touch;
};

static struct ap_snap ap_prev, ap_cur;
static unsigned ap_secs = 10;
static int ap_sleep_chan;
static int ap_on;               /* lines wanted (andrpwr=1 or "pwr 1"); also the wait channel */
static bool ap_started;         /* ap_thread exists; set once */
static unsigned ap_gen;         /* +1 at every start: a line never spans an off period */
static char ap_line[768];
static size_t ap_len;

#define AP_PUT(...) do { \
	if (ap_len < sizeof(ap_line)) { \
	        int ap_n = snprintf(ap_line + ap_len, sizeof(ap_line) - ap_len, __VA_ARGS__); \
	        if (ap_n > 0) { \
	                ap_len += (size_t)ap_n; \
	        } \
	} \
} while (0)

void
andromeda_display_set(int off)
{
	__atomic_store_n(&andromeda_display_off, off ? 1 : 0, __ATOMIC_SEQ_CST);
	if (!off) {
		thread_wakeup(&andromeda_display_off);
	}
}

void
andromeda_display_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&andromeda_display_off, THREAD_UNINT, deadline);
	/* Turned on between the caller's check and the wait: do not sleep. */
	if (!__atomic_load_n(&andromeda_display_off, __ATOMIC_SEQ_CST)) {
		thread_wakeup(&andromeda_display_off);
	}
	(void)thread_block(THREAD_CONTINUE_NULL);
}

static void
ap_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ap_sleep_chan, THREAD_UNINT, deadline);
	(void)thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
ap_get(uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/* Cumulative counters of one running CPU; false if absent or not running. */
static bool
ap_cpu(int cpu, uint32_t *idle, uint64_t *irq, uint64_t *ipi)
{
	processor_t p = cpu_to_processor(cpu);
	host_t host;
	processor_basic_info_data_t basic;
	processor_cpu_load_info_data_t load;
	processor_cpu_stat64_data_t stat;
	mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
	if (p == PROCESSOR_NULL ||
	    processor_info(p, PROCESSOR_BASIC_INFO, &host, (processor_info_t)&basic, &count) != KERN_SUCCESS ||
	    !basic.running) {
		return false;
	}
	count = PROCESSOR_CPU_LOAD_INFO_COUNT;
	if (processor_info(p, PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&load, &count) != KERN_SUCCESS) {
		return false;
	}
	count = PROCESSOR_CPU_STAT64_COUNT;
	if (processor_info(p, PROCESSOR_CPU_STAT64, &host, (processor_info_t)&stat, &count) != KERN_SUCCESS) {
		return false;
	}
	*idle = load.cpu_ticks[CPU_STATE_IDLE];
	*irq = stat.irq_ex_cnt;
	*ipi = stat.ipi_cnt;
	return true;
}

static void
ap_sample(struct ap_snap *s)
{
	const ml_topology_info_t *topo = ml_get_topology_info();
	bzero(s, sizeof(*s));
	s->at = mach_absolute_time();
	for (unsigned i = 0; topo && topo->cpus && i < topo->num_cpus; ++i) {
		unsigned cpu = topo->cpus[i].cpu_id;
		if (cpu >= AP_CPUS) {
			continue;
		}
		s->up[cpu] = ap_cpu((int)cpu, &s->idle[cpu], &s->irq[cpu], &s->ipi[cpu]);
	}
	for (unsigned cpu = 0; cpu < AP_CPUS; ++cpu) {
		uint64_t *slot = &andromeda_pwr_idle[cpu * ANDROMEDA_PWR_IDLE_STRIDE];
		/* Benign race with the writer: a new maximum lands in the next line. */
		uint64_t max = __atomic_exchange_n(&slot[ANDROMEDA_PWR_IDLE_FLUSH_MAX], 0, __ATOMIC_RELAXED);
		s->wfi[cpu] = ap_get(&slot[ANDROMEDA_PWR_IDLE_WFI]);
		s->wfi_abs[cpu] = ap_get(&slot[ANDROMEDA_PWR_IDLE_WFI_ABS]);
		s->lng_abs[cpu] = ap_get(&slot[ANDROMEDA_PWR_IDLE_LONG_ABS]);
		s->flush += ap_get(&slot[ANDROMEDA_PWR_IDLE_FLUSH]);
		s->flush_abs += ap_get(&slot[ANDROMEDA_PWR_IDLE_FLUSH_ABS]);
		if (max > s->flush_max) {
			s->flush_max = max;
		}
	}
	s->vsync = ap_get(&andromeda_pwr_fb_vsync);
	s->vnotify = ap_get(&andromeda_pwr_fb_notify);
	s->usb = ap_get(&andromeda_pwr_usb_polls);
	s->usb_slow = ap_get(&andromeda_pwr_usb_slow);
	s->keys = ap_get(&andromeda_pwr_keys_polls);
	s->touch = ap_get(&andromeda_pwr_touch_polls);
}

static uint64_t
ap_rate(uint64_t delta, uint64_t ns)
{
	return ns ? (delta * NSEC_PER_SEC + ns / 2) / ns : 0;
}

static uint64_t
ap_ns(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

/* " name=v0,v1,...": per-CPU values (tenths when tenths), "-" when down. */
static void
ap_list(const char *name, const uint64_t *v, bool tenths)
{
	AP_PUT(" %s=", name);
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		const char *sep = c ? "," : "";
		if (!ap_prev.up[c] || !ap_cur.up[c]) {
			AP_PUT("%s-", sep);
		} else if (tenths) {
			AP_PUT("%s%llu.%llu", sep, v[c] / 10, v[c] % 10);
		} else {
			AP_PUT("%s%llu", sep, v[c]);
		}
	}
}

static void
ap_print(void)
{
	const struct ap_snap *a = &ap_prev, *b = &ap_cur;
	uint64_t elapsed = b->at - a->at, ns = ap_ns(elapsed), v[AP_CPUS];
	uint64_t flushes = b->flush - a->flush, flush_ns = ap_ns(b->flush_abs - a->flush_abs);
	uint64_t usb = b->usb - a->usb;
	unsigned up = 0, percent = 0, mv = 0;
	int ma = 0;
	bool external = false, capable = false, charging = false, full = false;
	if (ns == 0) {
		return;
	}
	ap_len = 0;
	AP_PUT(AP_TAG " t=%llu s=%llu disp=%s", ap_ns(b->at) / NSEC_PER_SEC,
	    (ns + NSEC_PER_SEC / 2) / NSEC_PER_SEC,
	    __atomic_load_n(&andromeda_display_off, __ATOMIC_RELAXED) ? "off" : "on");
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		uint64_t idle_abs = (uint64_t)(uint32_t)(b->idle[c] - a->idle[c]) * hz_tick_interval;
		v[c] = idle_abs >= elapsed ? 1000 : idle_abs * 1000 / elapsed;
		up += (a->up[c] && b->up[c]) ? 1u : 0u;
	}
	ap_list("idle", v, true);
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		v[c] = ap_rate(b->irq[c] - a->irq[c], ns);
	}
	ap_list("irq", v, false);
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		v[c] = ap_rate(b->ipi[c] - a->ipi[c], ns);
	}
	ap_list("ipi", v, false);
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		v[c] = ap_rate(b->wfi[c] - a->wfi[c], ns);
	}
	ap_list("wfi", v, false);
	for (unsigned c = 0; c < AP_CPUS; ++c) {
		uint64_t in_wfi = b->wfi_abs[c] - a->wfi_abs[c], in_long = b->lng_abs[c] - a->lng_abs[c];
		v[c] = in_wfi ? in_long * 1000 / in_wfi : 0;
	}
	ap_list("long", v, true);
	if (flushes == 0) {
		AP_PUT(" flush=-");
	} else {
		/* Share of all running CPUs' time, in hundredths of a percent. */
		uint64_t share = up ? flush_ns * 10000 / (ns * up) : 0;
		AP_PUT(" flush=%lluns/%lluus/%llu.%02llu%%", flush_ns / flushes,
		    ap_ns(b->flush_max) / 1000, share / 100, share % 100);
	}
	AP_PUT(" vsync=%llu vnotify=%llu usb=%llu(%llu%%) keys=%llu touch=%llu",
	    ap_rate(b->vsync - a->vsync, ns), ap_rate(b->vnotify - a->vnotify, ns),
	    ap_rate(usb, ns), usb ? (b->usb_slow - a->usb_slow) * 100 / usb : 0,
	    ap_rate(b->keys - a->keys, ns), ap_rate(b->touch - a->touch, ns));
	if (andromeda_battery_sample(&percent, &mv, &ma, &external, &capable, &charging, &full) == 0) {
		AP_PUT(" bat=%dmA/%umV/%u%%", ma, mv, percent);
	} else {
		AP_PUT(" bat=-");
	}
	printf("%s\n", ap_line);
}

static void ap_thread(void *arg, wait_result_t result) __attribute__((noreturn));

/* Until "pwr 1": sleep on ap_on (same wait/wakeup pattern as andromeda_display_wait). */
static void
ap_wait_on(void)
{
	while (!__atomic_load_n(&ap_on, __ATOMIC_ACQUIRE)) {
		(void)assert_wait(&ap_on, THREAD_UNINT);
		if (__atomic_load_n(&ap_on, __ATOMIC_ACQUIRE)) {
			thread_wakeup(&ap_on);
		}
		(void)thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
ap_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	for (;;) {
		ap_wait_on();
		unsigned gen = __atomic_load_n(&ap_gen, __ATOMIC_ACQUIRE);
		ap_sample(&ap_prev);
		while (__atomic_load_n(&ap_on, __ATOMIC_ACQUIRE)) {
			ap_wait(ap_secs * 1000u);
			if (!__atomic_load_n(&ap_on, __ATOMIC_ACQUIRE) ||
			    __atomic_load_n(&ap_gen, __ATOMIC_ACQUIRE) != gen) {
				break;
			}
			ap_sample(&ap_cur);
			ap_print();
			ap_prev = ap_cur;
		}
	}
}

/* Lines on: cpu_idle() starts counting only after the threshold is in place. */
static bool
ap_start(void)
{
	nanoseconds_to_absolutetime(4 * NSEC_PER_MSEC, &andromeda_pwr_long_abs);
	__atomic_store_n(&andromeda_pwr_measure, 1, __ATOMIC_RELEASE);
	__atomic_fetch_add(&ap_gen, 1u, __ATOMIC_ACQ_REL);
	__atomic_store_n(&ap_on, 1, __ATOMIC_RELEASE);
	if (ap_started) {
		thread_wakeup(&ap_on);
		thread_wakeup(&ap_sleep_chan);  /* still in the period of an earlier "on" */
		return true;
	}
	thread_t thread;
	if (kernel_thread_start(ap_thread, NULL, &thread) != KERN_SUCCESS) {
		__atomic_store_n(&ap_on, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&andromeda_pwr_measure, 0, __ATOMIC_RELEASE);
		return false;
	}
	thread_deallocate(thread);
	ap_started = true;
	return true;
}

/* "pwr 0|1" (andromeda_ctl.c, its single thread). */
int
andromeda_power_ctl(unsigned on, char *out, size_t len)
{
	bool was = __atomic_load_n(&ap_on, __ATOMIC_ACQUIRE) != 0;
	if (on) {
		if (was) {
			snprintf(out, len, "already on, one ANDROMEDA PWR line every %u s", ap_secs);
			return 0;
		}
		if (!ap_start()) {
			snprintf(out, len, "kernel_thread_start failed");
			return EAGAIN;
		}
		printf(AP_TAG " on (andrctl): one line every %u s, read only\n", ap_secs);
		snprintf(out, len, "one ANDROMEDA PWR line every %u s from now (read only)", ap_secs);
		return 0;
	}
	__atomic_store_n(&ap_on, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&andromeda_pwr_measure, 0, __ATOMIC_RELEASE);
	if (was) {
		printf(AP_TAG " off (andrctl)\n");
	}
	snprintf(out, len, was ? "lines and idle counting off (the line in progress is dropped)" :
	    "already off");
	return 0;
}

/* "idle NAME VALUE": ranges already checked by andr_ctl_parse (andromeda_ctl.h). */
int
andromeda_idle_ctl(unsigned which, unsigned value, char *out, size_t len)
{
	const char *what;
	switch (which) {
	case 0:         /* ANDR_IDLE_CPU */
		if (value) {
			__atomic_fetch_or(&andromeda_idle, 1u, __ATOMIC_RELAXED);
		} else {
			__atomic_fetch_and(&andromeda_idle, ~1u, __ATOMIC_RELAXED);
		}
		what = value ? "no set/way L1 flush at idle entry (andridle bit 0)" :
		    "set/way L1 flush at every idle entry again";
		break;
	case 1:         /* ANDR_IDLE_FB */
		__atomic_store_n(&andromeda_idle_fb_park, value, __ATOMIC_RELAXED);
		what = value ? "vsync timer stops once the display is off (andrfbpark)" :
		    "vsync timer keeps running with the display off (a parked one restarts at display on)";
		break;
	case 2:         /* ANDR_IDLE_USB */
		__atomic_store_n(&andromeda_idle_usb_ms, value, __ATOMIC_RELAXED);
		what = value ? "USB events polled slowly after the idle window (andrusbidle)" :
		    "USB events polled every 1 ms";
		break;
	case 3:         /* ANDR_IDLE_KEYS */
		__atomic_store_n(&andromeda_idle_keys_ms, value, __ATOMIC_RELAXED);
		what = value ? "keys polled at this period while the display is off (andrkeysidle)" :
		    "keys polled every 20 ms";
		break;
	case 4:         /* ANDR_IDLE_TOUCH */
		__atomic_store_n(&andromeda_idle_touch_ms, value, __ATOMIC_RELAXED);
		what = value ? "touch polled at this period while the display is off (andrtouchidle)" :
		    "touch polled every 10 ms";
		break;
	default:
		snprintf(out, len, "unknown saving");
		return EINVAL;
	}
	printf(AP_TAG " idle saving %u = %u (andrctl): %s\n", which, value, what);
	snprintf(out, len, "%s", what);
	return 0;
}

/* "pwr on 10 s; idle cpu 0 fb 0 usb 0 keys 0 touch 0" */
void
andromeda_power_ctl_status(char *out, size_t len)
{
	bool on = __atomic_load_n(&ap_on, __ATOMIC_ACQUIRE) != 0;
	snprintf(out, len, "pwr %s %u s; idle cpu %u fb %u usb %u keys %u touch %u", on ? "on" : "off",
	    ap_secs, __atomic_load_n(&andromeda_idle, __ATOMIC_RELAXED) & 1u,
	    __atomic_load_n(&andromeda_idle_fb_park, __ATOMIC_RELAXED),
	    __atomic_load_n(&andromeda_idle_usb_ms, __ATOMIC_RELAXED),
	    __atomic_load_n(&andromeda_idle_keys_ms, __ATOMIC_RELAXED),
	    __atomic_load_n(&andromeda_idle_touch_ms, __ATOMIC_RELAXED));
}

void
andromeda_power_init(void)
{
	static bool started;
	unsigned on = 0, idle = 0;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrpwr", &on, sizeof(on));
	(void)PE_parse_boot_argn("andrpwrsecs", &ap_secs, sizeof(ap_secs));
	/* osfmk/arm64/cpu.c (patch 0101) reads andridle itself: only reported here. */
	(void)PE_parse_boot_argn("andridle", &idle, sizeof(idle));
	if (ap_secs < 2 || ap_secs > 600) {
		if (on) {
			printf(AP_TAG " andrpwrsecs=%u out of 2..600, 10 instead\n", ap_secs);
		}
		ap_secs = 10;
	}
	if (!on) {
		return;
	}
	if (!ap_start()) {
		printf(AP_TAG " kernel_thread_start failed\n");
		return;
	}
	printf(AP_TAG " one line every %u s (andrpwr=%u), read only; idle set/way flush %s (andridle=%u)\n",
	    ap_secs, on, (idle & 1u) ? "off" : "on", idle);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
