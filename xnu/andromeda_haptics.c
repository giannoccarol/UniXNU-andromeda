/*
 * Mi Mix 3 5G (andromeda, SM8150): vibration motor test, step V1 of
 * docs/research/sensori-vibrazione.md. Off by default.
 *
 * Boot-args:
 *   andrhap=1          andrhapdelay seconds after start: set the PM8150B
 *                      haptics block up for the phone's LRA, play two
 *                      constant pulses of 200 ms, 2 s apart, switch the block
 *                      off and write back every register that was changed.
 *   andrhapdelay=N     seconds before the test (default 60, 5..600).
 *   andrhapmv=N        drive amplitude in mV (default and maximum 2552,
 *                      minimum 1160); larger values are clamped to 2552.
 *
 * Runtime control (andromeda_ctl.c, andrctl=1, command "hap 1"): the same
 * test, started from the PC, once per boot, with the same checks and the
 * same amplitude limit. A boot-arg run still in its delay starts at once.
 *
 * Hardware: the LRA is driven by the haptics block of the PM8150B, SPMI SID 3,
 * base 0xC000, no supply to switch on (qcom,haptics@c000 in the phone's live
 * device tree: actuator "lra", shape "sine", auto-resonance "qwd",
 * play-rate 5102 us, vmax 2800 mV; Xiaomi's andromeda-sm8150.dtsi says
 * 2552 mV / 4878 us). We never go above 2552 mV, the lower of the two.
 *
 * Register sequence: Qualcomm/Xiaomi qti-haptics.c (andromeda-p-oss,
 * GPL-2.0), used as documentation only, no code copied:
 *   qti_haptics_hw_init() for an LRA: CFG1 = 0x00 (LRA), ILIM_CFG = 0x02
 *   (1000 mA; not written here, see ah_plan), EN_CTL3 = 0xFD, ZX_CFG[2:0] = 3 (80 us), RATE_CFG1/2 = period
 *   / 5 us (5102 us -> 0x3FC), CFG2 = 0x00 (sine), AUTO_RES_CFG (mask 0x8F) =
 *   0x88 (QWD, CAL_EOP), AUTO_RES_CTRL = 0xEC;
 *   the VMAX branch of qti_haptics_load_constant_waveform() (>= 20 ms):
 *   VMAX_CFG[5:1] = mV / 116, SEL (mask 0x31) = 0x00 (VMAX source);
 *   qti_haptics_playback(): EN_CTL1 = 0x80, PLAY = 0x80; the stop timer:
 *   PLAY = 0x00; the disable timer, one play period + 1 ms later:
 *   EN_CTL1 = 0x00.
 * Two deliberate differences: VMAX_CFG is written with mask 0xFE instead of
 * 0x3E, so SIGN and OVD (bits 7:6) are cleared rather than inherited and the
 * amplitude is the one we chose; and nothing of the waveform buffer, brake or
 * EN_CTL2 is touched (qti_haptics_clear_settings() serves the buffer source,
 * which we never use).
 *
 * Method, after HakiOS vayu_haptic.c by nomorecoolnicknames (hakios-sm8150
 * 3e7be13, POCO X3 Pro, same PM8150B; no licence, so facts and method only,
 * no code): a closed list of thirteen writable registers, every write read
 * back, STATUS1 read every 10 ms while playing, SC_DET stops everything, two
 * pulses 2 s apart, motor off also on the panic path. Their phone read TYPE
 * 0x15 / SUBTYPE 0x09 and owner 0 for this block; we require the same before
 * writing anything.
 *
 * Safety: every register this test writes is read first (twice, the two reads
 * must agree) and written back at the end. Nothing is written unless the
 * channel belongs to our execution environment, the identity is 0x15/0x09,
 * STATUS1 shows neither SC_DET nor BUSY and EN_CTL1/PLAY are off. The SPMI
 * write helper (andromeda_probe.c) refuses anything outside the thirteen
 * registers 0x46-0x70 of SID 3 0xC000; nothing at 0xD0 or above (SEC_ACCESS)
 * is ever written. SC_CLR is not written: a short circuit is reported, not
 * cleared. A kdp callout turns the motor off (PLAY = 0, EN_CTL1 = 0) if the
 * kernel panics while the block is set up.
 *
 * NOT YET RUN ON THE PHONE. Test with the user holding the phone.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kdp/kdp_callout.h>
#include <pexpert/pexpert.h>

/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
/* andromeda_probe.c: allow-listed SPMI access to SID 3 0xC000. */
int andromeda_spmi_hap_open(unsigned *apid, unsigned *owner);
int andromeda_spmi_hap_read(unsigned reg, uint8_t *value);
int andromeda_spmi_hap_write(unsigned reg, uint8_t value, uint8_t *readback);
void andromeda_spmi_hap_quiesce(void);
void andromeda_haptics_init(void);
#include "andromeda_ctl.h"
/* andromeda_ctl.c: runtime control and request state (andromeda_ctl.h). */
extern void andromeda_ctl_delay(uint32_t *st, unsigned seconds);
extern bool andromeda_ctl_is_boot(uint32_t *st);
extern uint32_t andromeda_ctl_begin(uint32_t *st, const char *who);
extern void andromeda_ctl_end(uint32_t *st, unsigned reached, bool failed, const char *who);
extern bool andromeda_ctl_spawn(thread_continue_t fn);
uint32_t andromeda_hap_req;
const char *andromeda_hap_check(unsigned level);
bool andromeda_hap_spawn(void);

#define AH_TAG "ANDROMEDA HAP:"

/* Offsets from 0xC000 (qti-haptics.c register map). */
#define AH_TYPE          0x04u
#define AH_SUBTYPE       0x05u
#define AH_STATUS1       0x0au
#define AH_SC_DET        0x08u   /* STATUS1 bit 3 */
#define AH_BUSY          0x02u   /* STATUS1 bit 1 */
#define AH_EN_CTL1       0x46u
#define AH_EN_CTL3       0x4au
#define AH_AUTO_RES_CTRL 0x4bu
#define AH_CFG1          0x4cu
#define AH_CFG2          0x4du
#define AH_SEL           0x4eu
#define AH_AUTO_RES_CFG  0x4fu
#define AH_VMAX_CFG      0x51u
#define AH_ILIM_CFG      0x52u
#define AH_RATE_CFG1     0x54u
#define AH_RATE_CFG2     0x55u
#define AH_ZX_CFG        0x5au
#define AH_PLAY          0x70u
#define AH_ON            0x80u   /* EN_CTL1 HAP_EN, PLAY HAP_PLAY */

#define AH_TYPE_HAPTICS  0x15u
#define AH_SUBTYPE_8150B 0x09u

#define AH_MV_MAX        2552u   /* Xiaomi andromeda-sm8150.dtsi qcom,vmax-mv */
#define AH_MV_MIN        1160u
#define AH_MV_LSB        116u
#define AH_RATE_US       5102u   /* live DT qcom,play-rate-us */
#define AH_RATE_CODE     (AH_RATE_US / 5u)
#define AH_PULSE_MS      200u
#define AH_GAP_MS        2000u
#define AH_POLL_MS       10u
#define AH_OFF_MS        10u     /* >= one play period + 1 ms */

_Static_assert(AH_RATE_CODE == 0x3fcu, "5102 us must give RATE 0x3FC");
_Static_assert((AH_MV_MAX / AH_MV_LSB) << 1 == 0x2cu, "2552 mV must give VMAX 0x2C");

struct ah_step {
	uint8_t reg, mask, value;
};

/* In qti-haptics.c's order. VMAX_CFG's value is filled in from andrhapmv. */
static const struct ah_step ah_plan[] = {
	{AH_CFG1,          0xff, 0x00},
	/* ILIM_CFG is left as the bootloader set it (0x01 on this phone): Linux
	 * writes 0x02 (HAP_ILIM_SEL_1000MA) without reading it back, but here
	 * bit 1 does not stick (2026-11-07: wrote 0x02, read 0x00), so the write
	 * only cleared bit 0 and the read-back check stopped the test. */
	{AH_EN_CTL3,       0xff, 0xfd},
	{AH_ZX_CFG,        0x07, 0x03},
	{AH_RATE_CFG1,     0xff, (uint8_t)(AH_RATE_CODE & 0xffu)},
	{AH_RATE_CFG2,     0xff, (uint8_t)((AH_RATE_CODE >> 8) & 0x0fu)},
	{AH_CFG2,          0xff, 0x00},
	{AH_AUTO_RES_CFG,  0x8f, 0x88},
	{AH_AUTO_RES_CTRL, 0xff, 0xec},
	{AH_VMAX_CFG,      0xfe, 0x00},
	{AH_SEL,           0x31, 0x00},
};
#define AH_STEPS ((unsigned)(sizeof(ah_plan) / sizeof(ah_plan[0])))

/* Registers logged before and after: identity, status, the written ones, EN_CTL2, BRAKE. */
static const uint8_t ah_dump[] = {0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x10, 0x46, 0x48,
	0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x5a, 0x5c,
	0x5e, 0x70};
#define AH_DUMPS ((unsigned)(sizeof(ah_dump) / sizeof(ah_dump[0])))

static unsigned ah_sleep_chan;
static unsigned ah_mv = AH_MV_MAX;
static unsigned ah_delay_s = 60;
static volatile bool ah_live;    /* the block may drive the motor: the panic hook acts */
/* Register values indexed by offset: before (twice), once configured, after. */
static uint8_t ah_before[0x80], ah_again[0x80], ah_config[0x80], ah_after[0x80];
static uint8_t ah_target[AH_STEPS];
static bool ah_tried[AH_STEPS], ah_failed[AH_STEPS];
static bool ah_pulsed;           /* EN_CTL1/PLAY were written */

static void
ah_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ah_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
ah_now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
ah_panic_callout(void *arg, kdp_event_t event)
{
	(void)arg;
	if (event != KDP_EVENT_EXIT && ah_live) {
		andromeda_spmi_hap_quiesce();
	}
}

/* Read every register of ah_dump into values[] and log them; false if one failed. */
static bool
ah_read_all(const char *what, uint8_t *values, bool log)
{
	char line[256];
	bool ok = true;
	int n = snprintf(line, sizeof(line), AH_TAG " %s:", what);
	for (unsigned i = 0; i < AH_DUMPS; ++i) {
		uint8_t reg = ah_dump[i], value = 0;
		int err = andromeda_spmi_hap_read(reg, &value);
		values[reg] = value;
		if (n > 0 && (size_t)n < sizeof(line) - 8) {
			n += err ? snprintf(line + n, sizeof(line) - (size_t)n, " %02x=E%d", reg, -err)
			    : snprintf(line + n, sizeof(line) - (size_t)n, " %02x=%02x", reg, value);
		}
		if (err) {
			ok = false;
			if (err == -3) {
				break; /* the observer lane is now disabled */
			}
		}
	}
	if (log) {
		printf("%s\n", line);
	}
	return ok;
}

/* Append " reg:old>new" (and the error) to a log line. */
static void
ah_note(char *line, size_t size, int *n, unsigned reg, uint8_t old, uint8_t value, int err,
    uint8_t readback)
{
	if (*n <= 0 || (size_t)*n >= size - 24) {
		return;
	}
	*n += err ? snprintf(line + *n, size - (size_t)*n, " %02x:%02x>%02x!E%d(rb %02x)", reg, old,
	    value, -err, readback)
	    : snprintf(line + *n, size - (size_t)*n, " %02x:%02x>%02x", reg, old, value);
}

/* PLAY = 0, then one play period + 1 ms later EN_CTL1 = 0; retried. True when both took. */
static bool
ah_motor_off(char *line, size_t size, int *n)
{
	static const uint8_t regs[2] = {AH_PLAY, AH_EN_CTL1};
	bool off[2] = {false, false};
	for (unsigned r = 0; r < 2; ++r) {
		if (r) {
			ah_wait(AH_OFF_MS);
		}
		for (unsigned attempt = 0; attempt < 5 && !off[r]; ++attempt) {
			uint8_t rb = 0xff;
			int err = andromeda_spmi_hap_write(regs[r], 0x00, &rb);
			/* -6: written, not read back; -5 with bit 7 clear: off, other bits differ. */
			off[r] = err == 0 || err == -6 || (err == -5 && !(rb & AH_ON));
			if (err || attempt) {
				ah_note(line, size, n, regs[r], AH_ON, 0x00, err, rb);
			}
			if (!off[r]) {
				ah_wait(5);
			}
		}
	}
	return off[0] && off[1];
}

/*
 * One constant vibration: EN_CTL1 = 0x80, PLAY = 0x80, AH_PULSE_MS with
 * STATUS1 read every AH_POLL_MS, then the motor off. Returns 0; 1 on SC_DET;
 * -7 if the motor could not be confirmed off; -8 if PLAY was written but
 * STATUS1 never showed BUSY in the first AH_BUSY_SAMPLES reads; another
 * negative value if an access failed (the motor is switched off in every
 * case). PLAY is a trigger: on this PM8150B it reads back 0 right after
 * 0x80 is written (2026-11-07), so its read-back mismatch is accepted and
 * STATUS1 BUSY (HAP_BUSY_BIT of qti-haptics.c) confirms the playback.
 */
#define AH_BUSY_SAMPLES 3u
static int
ah_pulse(unsigned number)
{
	char line[256];
	int n = snprintf(line, sizeof(line), AH_TAG " pulse %u:", number);
	unsigned samples = 0, busy = 0;
	uint8_t st_or = 0, st_and = 0xff, rb = 0;
	bool short_circuit = false, play_by_status = false;
	ah_pulsed = true;
	int err = andromeda_spmi_hap_write(AH_EN_CTL1, AH_ON, &rb);
	ah_note(line, sizeof(line), &n, AH_EN_CTL1, ah_before[AH_EN_CTL1], AH_ON, err, rb);
	if (!err) {
		err = andromeda_spmi_hap_write(AH_PLAY, AH_ON, &rb);
		ah_note(line, sizeof(line), &n, AH_PLAY, ah_before[AH_PLAY], AH_ON, err, rb);
		if (err == -5) {
			err = 0;               /* written; STATUS1 BUSY must confirm it */
			play_by_status = true;
		}
	}
	uint64_t start = ah_now_ms(), end = start;
	while (!err && ah_now_ms() - start < AH_PULSE_MS) {
		uint8_t status = 0;
		ah_wait(AH_POLL_MS);
		err = andromeda_spmi_hap_read(AH_STATUS1, &status);
		if (err) {
			break;
		}
		++samples;
		st_or |= status;
		st_and &= status;
		busy += (status & AH_BUSY) != 0;
		if (status & AH_SC_DET) {
			short_circuit = true;
			break;
		}
		if (play_by_status && !busy && samples >= AH_BUSY_SAMPLES) {
			err = -8;              /* PLAY did not start the playback */
			break;
		}
	}
	end = ah_now_ms();
	bool off = ah_motor_off(line, sizeof(line), &n);
	uint8_t status = 0;
	int st_err = andromeda_spmi_hap_read(AH_STATUS1, &status);
	printf("%s\n", line);
	printf(AH_TAG " pulse %u: on %llu ms, STATUS1 x%u busy=%u or=%02x and=%02x%s err=%d;"
	    " after off STATUS1=%02x (%d), motor %s\n", number, (unsigned long long)(end - start),
	    samples, busy, st_or, samples ? st_and : 0, short_circuit ? " SC_DET" : "", err, status,
	    st_err, off ? "off" : "NOT CONFIRMED OFF");
	if (!off) {
		return -7;
	}
	return err ? err : (short_circuit ? 1 : 0);
}

/* Write back, in reverse order, every register this run changed or may have changed. */
static unsigned
ah_restore(void)
{
	char line[256];
	unsigned failures = 0, writes = 0;
	int n = snprintf(line, sizeof(line), AH_TAG " restore:");
	for (unsigned i = AH_STEPS; i-- > 0;) {
		uint8_t reg = ah_plan[i].reg, rb = 0;
		if (!ah_tried[i] || (!ah_failed[i] && ah_target[i] == ah_before[reg])) {
			continue;
		}
		int err = andromeda_spmi_hap_write(reg, ah_before[reg], &rb);
		ah_note(line, sizeof(line), &n, reg, ah_target[i], ah_before[reg], err, rb);
		failures += err != 0;
		++writes;
	}
	/* EN_CTL1 and PLAY are 0x00 now; their old values have bit 7 clear (checked before). */
	static const uint8_t state[2] = {AH_EN_CTL1, AH_PLAY};
	for (unsigned r = 0; r < 2 && ah_pulsed; ++r) {
		uint8_t reg = state[r], rb = 0;
		if (ah_before[reg] == 0x00) {
			continue;
		}
		int err = andromeda_spmi_hap_write(reg, ah_before[reg] & (uint8_t)~AH_ON, &rb);
		ah_note(line, sizeof(line), &n, reg, 0x00, ah_before[reg], err, rb);
		failures += err != 0;
		++writes;
	}
	printf("%s%s\n", line, writes ? "" : " nothing to do");
	return failures;
}

/* True when both pulses ran and every written register reads as before. */
static bool
ah_run(void)
{
	unsigned apid = 0, owner = 0;
	uint8_t *again = ah_again, *after = ah_after;
	char line[256];
	int n;

	int open = andromeda_spmi_hap_open(&apid, &owner);
	printf(AH_TAG " SID 3 0xC000 APID=%u owner=%u (0 = this EE)%s\n", apid, owner,
	    open ? ": no write channel, nothing will be written" : "");
	bool read_ok = ah_read_all("before", ah_before, true);
	if (open || apid >= 512) {
		return false;
	}
	bool same = read_ok && ah_read_all("before (again)", again, false);
	for (unsigned i = 0; same && i < AH_DUMPS; ++i) {
		uint8_t reg = ah_dump[i];
		/* Status and interrupt registers may change on their own. */
		bool status = reg == 0x08 || reg == 0x09 || reg == AH_STATUS1 || reg == 0x0b ||
		    reg == 0x10;
		if (!status && again[reg] != ah_before[reg]) {
			printf(AH_TAG " register %02x read %02x then %02x\n", reg, ah_before[reg], again[reg]);
			same = false;
		}
	}
	if (!same) {
		printf(AH_TAG " the two reads before the test disagree or failed: nothing written\n");
		return false;
	}
	if (ah_before[AH_TYPE] != AH_TYPE_HAPTICS || ah_before[AH_SUBTYPE] != AH_SUBTYPE_8150B) {
		printf(AH_TAG " TYPE/SUBTYPE %02x/%02x, expected %02x/%02x: nothing written\n",
		    ah_before[AH_TYPE], ah_before[AH_SUBTYPE], AH_TYPE_HAPTICS, AH_SUBTYPE_8150B);
		return false;
	}
	if ((ah_before[AH_STATUS1] | again[AH_STATUS1]) & (AH_SC_DET | AH_BUSY)) {
		printf(AH_TAG " STATUS1 %02x/%02x: short circuit or busy before the test: nothing written\n",
		    ah_before[AH_STATUS1], again[AH_STATUS1]);
		return false;
	}
	if ((ah_before[AH_EN_CTL1] | ah_before[AH_PLAY]) & AH_ON) {
		printf(AH_TAG " EN_CTL1 %02x PLAY %02x: the block is in use: nothing written\n",
		    ah_before[AH_EN_CTL1], ah_before[AH_PLAY]);
		return false;
	}

	static bool hooked;
	if (!hooked) {
		kdp_register_callout(ah_panic_callout, NULL);
		hooked = true;
	}
	ah_live = true;

	/* Configuration (qti_haptics_hw_init + VMAX branch). */
	unsigned code = ah_mv / AH_MV_LSB;
	bool config_ok = true;
	n = snprintf(line, sizeof(line), AH_TAG " config:");
	for (unsigned i = 0; i < AH_STEPS && config_ok; ++i) {
		const struct ah_step *s = &ah_plan[i];
		uint8_t value = s->reg == AH_VMAX_CFG ? (uint8_t)(code << 1) : s->value;
		uint8_t rb = 0;
		ah_target[i] = (uint8_t)((ah_before[s->reg] & (uint8_t)~s->mask) | (value & s->mask));
		ah_tried[i] = true;
		int err = andromeda_spmi_hap_write(s->reg, ah_target[i], &rb);
		ah_failed[i] = err != 0;
		ah_note(line, sizeof(line), &n, s->reg, ah_before[s->reg], ah_target[i], err, rb);
		config_ok = err == 0;
	}
	printf("%s%s\n", line, config_ok ? " ok" : " FAILED, no pulse");

	bool motor_off = true, pulses_ok = false;
	if (config_ok) {
		(void)ah_read_all("configured", ah_config, true);
		int result = ah_pulse(1);
		if (result == 0) {
			ah_wait(AH_GAP_MS);
			result = ah_pulse(2);
		}
		motor_off = result != -7;
		pulses_ok = result == 0;
		if (result == 1) {
			printf(AH_TAG " SC_DET (short circuit) seen: stopped, no further pulse, SC_CLR not written\n");
		} else if (result) {
			printf(AH_TAG " pulse failed (%d): stopped\n", result);
		}
	}

	/*
	 * If PLAY/EN_CTL1 did not take 0, the block may still drive the motor:
	 * then leave it at our values (<= 2552 mV) instead of writing back older,
	 * possibly stronger ones.
	 */
	unsigned failures = 0;
	if (motor_off) {
		failures = ah_restore();
	} else {
		printf(AH_TAG " motor off not confirmed: no restore, registers left as configured\n");
	}
	bool after_ok = ah_read_all("after", after, true);
	char diff[256];
	int d = snprintf(diff, sizeof(diff), AH_TAG " differences from before:");
	int d0 = d;
	for (unsigned i = 0; i < AH_STEPS; ++i) {
		uint8_t reg = ah_plan[i].reg;
		if (after[reg] != ah_before[reg] && d > 0 && (size_t)d < sizeof(diff) - 16) {
			d += snprintf(diff + d, sizeof(diff) - (size_t)d, " %02x=%02x(was %02x)", reg,
			    after[reg], ah_before[reg]);
		}
	}
	if ((after[AH_EN_CTL1] != ah_before[AH_EN_CTL1] || after[AH_PLAY] != ah_before[AH_PLAY]) &&
	    d > 0 && (size_t)d < sizeof(diff) - 40) {
		d += snprintf(diff + d, sizeof(diff) - (size_t)d, " 46=%02x(was %02x) 70=%02x(was %02x)",
		    after[AH_EN_CTL1], ah_before[AH_EN_CTL1], after[AH_PLAY], ah_before[AH_PLAY]);
	}
	bool restored = after_ok && failures == 0 && d == d0;
	if (restored) {
		printf(AH_TAG " restored: every written register reads as before\n");
	} else {
		printf("%s%s (restore write failures %u)\n", d == d0 ? AH_TAG " restore NOT verified:" : diff,
		    after_ok ? "" : " after-read incomplete", failures);
	}
	/* Keep the panic hook armed unless the block reads as off. */
	if (motor_off && !(after[AH_EN_CTL1] & AH_ON) && !(after[AH_PLAY] & AH_ON)) {
		ah_live = false;
	} else {
		printf(AH_TAG " WARNING: motor off not confirmed (EN_CTL1 %02x PLAY %02x)\n",
		    after[AH_EN_CTL1], after[AH_PLAY]);
		return false;
	}
	return pulses_ok && restored;
}

static void
ah_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	bool boot = andromeda_ctl_is_boot(&andromeda_hap_req);
	if (boot) {
		printf(AH_TAG " test in %u s: %u mV (VMAX code %u), play rate %u us, 2 x %u ms, pause %u ms\n",
		    ah_delay_s, (ah_mv / AH_MV_LSB) * AH_MV_LSB, ah_mv / AH_MV_LSB, AH_RATE_US, AH_PULSE_MS,
		    AH_GAP_MS);
		andromeda_ctl_delay(&andromeda_hap_req, ah_delay_s);
	} else {
		printf(AH_TAG " test now (andrctl): %u mV (VMAX code %u), play rate %u us, 2 x %u ms,"
		    " pause %u ms\n", (ah_mv / AH_MV_LSB) * AH_MV_LSB, ah_mv / AH_MV_LSB, AH_RATE_US,
		    AH_PULSE_MS, AH_GAP_MS);
	}
	(void)andromeda_ctl_begin(&andromeda_hap_req, "hap");
	printf(AH_TAG " begin\n");
	bool ok = ah_run();
	printf(AH_TAG " done\n");
	andromeda_ctl_end(&andromeda_hap_req, ok ? 1u : 0u, !ok, "hap");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

const char *
andromeda_hap_check(unsigned level)
{
	(void)level;
	return NULL;    /* amplitude and delay are clamped at init; ah_run checks the block */
}

bool
andromeda_hap_spawn(void)
{
	return andromeda_ctl_spawn(ah_thread);
}

void
andromeda_haptics_init(void)
{
	static bool started;
	unsigned level = 0;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrhap", &level, sizeof(level));
	(void)PE_parse_boot_argn("andrhapdelay", &ah_delay_s, sizeof(ah_delay_s));
	if (ah_delay_s < 5 || ah_delay_s > 600) {
		ah_delay_s = 60;
	}
	(void)PE_parse_boot_argn("andrhapmv", &ah_mv, sizeof(ah_mv));
	if (ah_mv > AH_MV_MAX) {
		printf(AH_TAG " andrhapmv=%u above %u mV: clamped\n", ah_mv, AH_MV_MAX);
		ah_mv = AH_MV_MAX;
	} else if (ah_mv < AH_MV_MIN) {
		ah_mv = AH_MV_MIN;
	}
	if (level == 0) {
		return;
	}
	if (level != 1) {
		printf(AH_TAG " andrhap=%u: only 1 exists, nothing done\n", level);
		return;
	}
	__atomic_store_n(&andromeda_hap_req, andr_st(ANDR_PH_WAIT, 1u, 0), __ATOMIC_RELEASE);
	if (!andromeda_hap_spawn()) {
		__atomic_store_n(&andromeda_hap_req, 0u, __ATOMIC_RELEASE);
		printf(AH_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
