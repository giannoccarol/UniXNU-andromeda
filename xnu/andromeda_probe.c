/*
 * Mi Mix 3 5G (andromeda, SM8150): read-only hardware probe for the
 * Bluetooth UART, the Wi-Fi/BT regulators and the PM8150B charger.
 *
 * Boot-args:
 *   andrprobe=1        read only: TLMM pins, GCC branches, SE13 (only if
 *                      its clocks already run), PMIC registers over SPMI.
 *   andrprobe=2        as 1, and for the SE13 read only: set the GCC vote
 *                      bit of gcc_qupv3_wrap2_s3_clk, read, restore the vote.
 *   andrprobedelay=N   seconds before the probe starts (default 100).
 *
 * Nothing is written to the PMIC. The only SPMI MMIO store is the observer
 * read command (EXT_READL), as in ios-sweet's sweet_spmi.c (MIT, dipa-studio),
 * whose observer-read sequence this file follows; see CREDITS.md. Register
 * addresses come from the SM8150 device tree of the phone and from Linux
 * (gcc-sm8150.c, pinctrl-sm8150.c, qcom-geni-se.h, spmi-pmic-arb.c and the
 * downstream smb5/fg-gen4 register headers), used as documentation only.
 *
 * Only the allow-listed PMIC peripherals below are read: no reads of the
 * modem, TrustZone or any area the hypervisor protects.
 *
 * The probe itself writes nothing to the PMIC. The only PMIC writes in this
 * file are further down and run only on request: the PON spare register
 * (andrfastboot=N), the PM8150B haptics registers for the vibration test
 * in andromeda_haptics.c (andrhap=1) and three registers of PM8150B GPIO5,
 * the vdd_boost_vreg enable (andrboost=1).
 *
 * Runtime control (andromeda_ctl.c, andrctl=1): "slider 1" starts the slider
 * reader of andrslider=1 (once), "fastboot" runs the andrfastboot reboot
 * now. The SE13 clock vote of andrprobe=2 takes the hardware turn of
 * andromeda_ctl.c, so it never overlaps the Bluetooth run (same vote bit),
 * whenever either starts.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include "cepheus_clock_status.h"

#include <kern/locks.h>
#include <vm/pmap.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
void andromeda_probe_init(void);
void andromeda_slider_publish(unsigned h42, unsigned h114); /* andromeda_slider.c, 0097 */
vm_offset_t andromeda_io_map(vm_offset_t pa, vm_size_t size);

#define AP_TAG "ANDROMEDA PROBE:"

/* ---- shared device mappings (all andromeda drivers) ------------------------
 * ml_io_map() takes its VA from XNU's io_submap: 8 MiB on arm
 * (osfmk/arm/io_map.c, IO_MAP_SIZE), and every mapping is permanent. With
 * each driver mapping the same GCC and TLMM tiles again it filled up:
 * "kmem_alloc(io_submap, 2097152, 0x100021): failed with 3" (KERN_NO_SPACE)
 * when andrgpu=1 asked for its first 2 MiB window. As ios-flame's
 * sweet_gpu.c, map with ml_io_map_unmappable() (kernel_map's data range,
 * gigabytes) and hand out a mapping that already covers the range. Device
 * memory either way (VM_WIMG_IO). Never call ml_io_unmap() on these: they
 * are shared and kept.
 */
#define AIO_SLOTS 64
static struct {
	uint64_t    pa;
	uint64_t    size;
	vm_offset_t va;
} aio_map[AIO_SLOTS];
static unsigned aio_count;
static uint64_t aio_bytes;
static LCK_GRP_DECLARE(aio_grp, "andromeda_io_map");
static LCK_MTX_DECLARE(aio_lock, &aio_grp);

vm_offset_t
andromeda_io_map(vm_offset_t pa, vm_size_t size)
{
	if (!size || (uint64_t)pa + size < (uint64_t)pa) {
		return 0;
	}
	lck_mtx_lock(&aio_lock);
	vm_offset_t va = 0;
	for (unsigned i = 0; i < aio_count; ++i) {
		if (pa >= aio_map[i].pa && (uint64_t)pa + size <= aio_map[i].pa + aio_map[i].size) {
			va = aio_map[i].va + (pa - aio_map[i].pa);
			break;
		}
	}
	if (!va) {
		va = ml_io_map_unmappable(pa, size, VM_WIMG_IO);
		if (va && aio_count < AIO_SLOTS) {
			aio_map[aio_count].pa = pa;
			aio_map[aio_count].size = size;
			aio_map[aio_count].va = va;
			++aio_count;
		}
		if (va) {
			aio_bytes += size;
		}
		printf(AP_TAG " io map 0x%llx+0x%llx -> %s (%u ranges, %llu KiB)\n",
		    (unsigned long long)pa, (unsigned long long)size, va ? "ok" : "FAILED",
		    aio_count, (unsigned long long)(aio_bytes >> 10));
	}
	lck_mtx_unlock(&aio_lock);
	return va;
}

/* TLMM EAST tile (pinctrl-sm8150.c): BT UART qup13 on GPIO 43-46. */
#define AP_TLMM_EAST_PA 0x03500000u
#define AP_TLMM_EAST_SZ 0x00030000u
#define AP_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define AP_GPIO_IO(n)   (AP_GPIO_CFG(n) + 4u)

/* GCC (gcc-sm8150.c): QUPv3 wrapper 2. */
#define AP_GCC_PA       0x00100000u
#define AP_GCC_SZ       0x00060000u
#define AP_GCC_VOTE     0x52014u     /* APCS_CLOCK_BRANCH_ENA_VOTE_1 */
#define AP_GCC_M_AHB    0x1e004u     /* vote bit 2 */
#define AP_GCC_S_AHB    0x1e008u     /* vote bit 1, hardware gated */
#define AP_GCC_S3       0x1e4d4u     /* wrap2 s3 = SE13 UART, vote bit 7 */
#define AP_GCC_S3_BIT   (1u << 7)
#define AP_GCC_S3_RCG   0x1e4d8u     /* gcc_qupv3_wrap2_s3_clk_src */

/* qupv3_se13_4uart (phone DT): wrapper 2 SE3. qcom-geni-se.h offsets. */
#define AP_SE13_PA      0x00c8c000u
#define AP_SE13_SZ      0x00001000u

/* SPMI PMIC arbiter v5 (phone DT reg-names core/obsrvr/cnfg). */
#define AP_SPMI_CORE_PA 0x0c440000u
#define AP_SPMI_CFG_PA  0x0c40a000u
#define AP_SPMI_OBS_PA  0x0e600000u
#define AP_NO_APID      0xffffu

static unsigned ap_sleep_chan;

static void
ap_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ap_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
ap_rd(vm_offset_t base, uint32_t off)
{
	uint32_t value = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

/* ---- SPMI observer reads ------------------------------------------------ */

static vm_offset_t sp_core, sp_cfg, sp_obs;
static unsigned sp_count;
static bool sp_ready;
/* A timed-out command may complete late: never reuse that lane this boot. */
static bool sp_poisoned[512];

/*
 * Peripherals we may read: (sid, peripheral) pairs. Keys (PON 0x08) and
 * ios-sweet's battery lanes (SID0 0x10-0x13, 0x48) are deliberately absent,
 * so no observer lane is shared with another driver.
 */
static const struct { uint8_t sid, periph; const char *name; } ap_allowed[] = {
	{0, 0x1d, "PM8150 S4A (BT vddio 1.8V)"},
	{0, 0x40, "PM8150 L1A (WLAN cx-mx)"},
	{0, 0x46, "PM8150 L7A (WLAN xo / BT core 1.8V)"},
	{4, 0x41, "PM8150L L2C (WLAN rfa / BT pa 1.3V)"},
	{4, 0x4a, "PM8150L L11C (WLAN ch0 / BT ldo 3.3V)"},
	{2, 0x10, "PM8150B CHGR"},
	{2, 0x11, "PM8150B DCDC"},
	{2, 0x12, "PM8150B BATIF"},
	{2, 0x13, "PM8150B USBIN"},
	{2, 0x14, "PM8150B DCIN"},
	{2, 0x15, "PM8150B TYPEC"},
	{2, 0x16, "PM8150B MISC"},
	{2, 0x40, "PM8150B FG BATT_SOC"},
	{2, 0x41, "PM8150B FG BATT_INFO"},
	{3, 0xc0, "PM8150B HAPTICS"},
	{2, 0x42, "PM8150B FG 0x42"},
	{2, 0x43, "PM8150B FG 0x43 (MEM_IF?)"},
	{2, 0x44, "PM8150B FG MEM_IF"},
	{2, 0x45, "PM8150B FG ADC_RR"},
	/* Touch investigation: vdd_boost_vreg (regulator-fixed on PM8150B GPIO5,
	 * regulator-always-on, no consumer in the DT) and the touch bus supply. */
	{2, 0xc4, "PM8150B GPIO5 (vdd_boost_vreg)"},
	{4, 0x40, "PM8150L L1C (touch vdd 1.8V)"},
};

static bool
sp_allowed(unsigned sid, unsigned periph)
{
	for (unsigned i = 0; i < sizeof(ap_allowed) / sizeof(ap_allowed[0]); ++i) {
		if (ap_allowed[i].sid == sid && ap_allowed[i].periph == periph) {
			return true;
		}
	}
	return false;
}

static uint16_t
sp_find_apid(unsigned ppid)
{
	uint16_t found = AP_NO_APID;
	for (unsigned i = 0; i < sp_count; ++i) {
		uint32_t map = ap_rd(sp_core, 0x900u + 4u * i);
		if (!map || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		unsigned owner = ap_rd(sp_cfg, 0x700u + 4u * i) & 7u;
		if (found == AP_NO_APID || owner == 0) {
			found = (uint16_t)i;
		}
	}
	return found;
}

/*
 * The probe thread and the battery service (SweetBattery, sweetbattery=2)
 * share these lanes: initialise once, and serialise every observer command.
 */
static IOLock *sp_lock;
static volatile UInt32 sp_state; /* 0 idle, 1 initialising, 2 ready, 3 failed */

static bool sp_init_hw(void);

static bool
sp_init(void)
{
	if (OSCompareAndSwap(0, 1, &sp_state)) {
		bool ok = sp_init_hw();
		__asm__ volatile("dmb ish" ::: "memory");
		sp_state = ok ? 2 : 3;
		return ok;
	}
	for (unsigned i = 0; i < 2000 && sp_state == 1; ++i) {
		delay(1000);
	}
	return sp_state == 2;
}

static bool
sp_init_hw(void)
{
	sp_lock = IOLockAlloc();
	if (!sp_lock) {
		printf(AP_TAG " SPMI lock allocation failed\n");
		return false;
	}
	sp_core = andromeda_io_map(AP_SPMI_CORE_PA, 0x2000);
	sp_cfg = andromeda_io_map(AP_SPMI_CFG_PA, 0x1000);
	sp_obs = andromeda_io_map(AP_SPMI_OBS_PA, 0x10000); /* EE 0, 512 * 0x80 */
	if (!sp_core || !sp_cfg || !sp_obs) {
		printf(AP_TAG " SPMI mapping failed\n");
		return false;
	}
	uint32_t version = ap_rd(sp_core, 0);
	sp_count = ap_rd(sp_core, 4) & 0x7ffu;
	if (version < 0x50000000u || version >= 0x70000000u || !sp_count || sp_count > 512) {
		printf(AP_TAG " unsupported SPMI arb=%#x APIDs=%u\n", version, sp_count);
		return false;
	}
	sp_ready = true;
	printf(AP_TAG " SPMI arb=%#x APIDs=%u, observer reads only\n", version, sp_count);
	return true;
}

static int sp_read_locked(unsigned sid, unsigned addr, uint8_t *out);

/* 0 on success; -1 not allowed/no lane; -2 error status; -3 timeout. */
static int
sp_read(unsigned sid, unsigned addr, uint8_t *out)
{
	unsigned periph = (addr >> 8) & 0xffu;
	if (!sp_ready || !sp_lock || addr > 0xffffu || !sp_allowed(sid, periph)) {
		return -1;
	}
	IOLockLock(sp_lock);
	int err = sp_read_locked(sid, addr, out);
	IOLockUnlock(sp_lock);
	return err;
}

static int
sp_read_locked(unsigned sid, unsigned addr, uint8_t *out)
{
	unsigned periph = (addr >> 8) & 0xffu;
	uint16_t apid = sp_find_apid((sid << 8) | periph);
	if (apid == AP_NO_APID || apid >= 512 || sp_poisoned[apid]) {
		return -1;
	}
	unsigned off = 0x80u * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sp_obs + off) = (1u << 27) | ((addr & 0xffu) << 4); /* EXT_READL, 1 byte */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t status = ap_rd(sp_obs, off + 0x08u);
		if (status & 1u) {
			if (status & 0x0eu) {
				return -2;
			}
			*out = (uint8_t)ap_rd(sp_obs, off + 0x18u);
			return 0;
		}
		delay(1);
	}
	sp_poisoned[apid] = true;
	printf(AP_TAG " SPMI timeout sid=%u addr=%#x APID=%u disabled\n", sid, addr, apid);
	return -3;
}

/* Print "name sid:pp off=val ..." for a list of register offsets. */
static void
sp_dump(unsigned sid, unsigned periph, const char *name, const uint8_t *offs, unsigned count)
{
	char line[256];
	int n = snprintf(line, sizeof(line), AP_TAG " %s sid%u %#x:", name, sid, periph << 8);
	uint16_t apid = sp_find_apid((sid << 8) | periph);
	if (apid == AP_NO_APID) {
		printf("%s no APID (peripheral absent or not visible)\n", line);
		return;
	}
	for (unsigned i = 0; i < count && n > 0 && (size_t)n < sizeof(line) - 12; ++i) {
		uint8_t value = 0;
		int err = sp_read(sid, (periph << 8) | offs[i], &value);
		if (err == 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x=%02x", offs[i], value);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x=E%d", offs[i], -err);
			if (err == -3) {
				break;
			}
		}
	}
	printf("%s\n", line);
}

/* TYPE, SUBTYPE, STATUS1-4, INT_RT_STS. */
static const uint8_t ap_offs_generic[] = {0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x10};
/* PMIC5 LDO/SMPS: VSET LSB/MSB, MODE_CTL, EN_CTL. */
static const uint8_t ap_offs_vreg[] = {0x04, 0x05, 0x08, 0x10, 0x40, 0x41, 0x45, 0x46};
/* smb5 status registers 0x06-0x0e plus INT_RT_STS. */
static const uint8_t ap_offs_chg[] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
	0x0c, 0x0d, 0x0e, 0x10};
/* FG BATT_SOC: ALG_STS, AUX, SLEEP, MONOTONIC_SOC, MONOTONIC_SOC_CP, INT_RT_STS, EN_CTL. */
static const uint8_t ap_offs_soc[] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x10, 0x46};
/* FG BATT_INFO: TEMP_STS, SYS_BATT, FG_STS, BATT_TEMP, VBATT, IBATT. */
static const uint8_t ap_offs_info[] = {0x04, 0x05, 0x06, 0x07, 0x09, 0x10, 0x50, 0x51,
	0xa0, 0xa1, 0xa2, 0xa3};
/* PMIC GPIO (pinctrl-spmi-gpio.c): TYPE, SUBTYPE, STATUS1, INT_RT_STS, MODE_CTL,
 * DIG_VIN_CTL, DIG_PULL_CTL, DIG_IN_CTL, DIG_OUT_SRC_CTL, DIG_OUT_CTL, EN_CTL. */
static const uint8_t ap_offs_gpio[] = {0x04, 0x05, 0x08, 0x10, 0x40, 0x41, 0x42, 0x43,
	0x44, 0x45, 0x46};
/* MEM_IF / ADC_RR: identity only (MEM_IF data registers are a SRAM window). */
static const uint8_t ap_offs_id[] = {0x04, 0x05};

#define AP_N(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

static void
ap_pmic(void)
{
	if (!sp_init()) {
		return;
	}
	for (unsigned i = 0; i < AP_N(ap_allowed); ++i) {
		unsigned sid = ap_allowed[i].sid, p = ap_allowed[i].periph;
		const uint8_t *offs = ap_offs_generic;
		unsigned count = AP_N(ap_offs_generic);
		if (sid == 3) {
			continue; /* haptics: dedicated dumps below */
		}
		if (sid != 2) {
			offs = ap_offs_vreg; count = AP_N(ap_offs_vreg);
		} else if (p >= 0x10 && p <= 0x16) {
			offs = ap_offs_chg; count = AP_N(ap_offs_chg);
		} else if (p == 0x40) {
			offs = ap_offs_soc; count = AP_N(ap_offs_soc);
		} else if (p == 0x41) {
			offs = ap_offs_info; count = AP_N(ap_offs_info);
		} else if (p >= 0xc0 && p <= 0xcb) {
			offs = ap_offs_gpio; count = AP_N(ap_offs_gpio);
		} else {
			offs = ap_offs_id; count = AP_N(ap_offs_id);
		}
		sp_dump(sid, p, ap_allowed[i].name, offs, count);
	}
	/*
	 * Charger configuration that decides whether it charges (docs/research/
	 * ricarica.md): CHGR charging-enable command 0x42, CFG2 0x51, FCC 0x61,
	 * FV 0x70, inhibit 0x72, auto-recharge SOC 0x7d, JEITA 0x90; USBIN
	 * suspend/override 0x40/0x42, HVDCP 0x62, load/ICL mode 0x65/0x66, ICL
	 * 0x70, AICL 0x80/0x81; MISC watchdog config 0x51/0x53, SMB_EN 0x48,
	 * thermal regulation 0x70. Reads only: the watchdog is not petted.
	 */
	static const uint8_t chgr_cfg[] = {0x42, 0x51, 0x61, 0x70, 0x72, 0x7d, 0x90};
	static const uint8_t usbin_cfg[] = {0x40, 0x42, 0x58, 0x62, 0x65, 0x66, 0x70, 0x80, 0x81};
	static const uint8_t misc_cfg[] = {0x48, 0x51, 0x53, 0x61, 0x70};
	sp_dump(2, 0x10, "PM8150B CHGR cfg", chgr_cfg, AP_N(chgr_cfg));
	sp_dump(2, 0x13, "PM8150B USBIN cfg", usbin_cfg, AP_N(usbin_cfg));
	sp_dump(2, 0x16, "PM8150B MISC cfg", misc_cfg, AP_N(misc_cfg));
	/*
	 * Vibrator: PM8150B haptics block, SID 3 0xC000 (qti-haptics.c register
	 * map, docs/research/sensori-vibrazione.md): identity, STATUS1 (0x0A),
	 * EN_CTL1-3, auto resonance, CFG, SEL, VMAX and play-rate registers.
	 * Read only: the motor is not driven. Everything is below 0xD0 (no
	 * SEC_ACCESS region).
	 */
	static const uint8_t hap_id[] = {0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x10, 0x46, 0x48,
		0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
	static const uint8_t hap_cfg[] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
		0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x70};
	sp_dump(3, 0xc0, "PM8150B HAPTICS", hap_id, AP_N(hap_id));
	sp_dump(3, 0xc0, "PM8150B HAPTICS cfg", hap_cfg, AP_N(hap_cfg));
}

/* ---- battery source for SweetBattery (sweetbattery=2) ------------------- */

int andromeda_battery_init(void);
int andromeda_battery_sample(unsigned *percent, unsigned *mv, int *ma,
    bool *external, bool *capable, bool *charging, bool *full);

int
andromeda_battery_init(void)
{
	return sp_init() ? 0 : -1;
}

/* Two matching LSB/MSB pairs, as SweetBattery::readWord does (torn samples). */
static bool
ap_word(unsigned sid, unsigned addr, uint16_t *value)
{
	uint16_t previous = 0;
	for (unsigned attempt = 0; attempt < 4; ++attempt) {
		uint8_t lo, hi;
		if (sp_read(sid, addr, &lo) || sp_read(sid, addr + 1, &hi)) {
			return false;
		}
		uint16_t next = (uint16_t)((unsigned)lo | ((unsigned)hi << 8));
		if (attempt && next == previous) {
			*value = next;
			return true;
		}
		previous = next;
	}
	return false;
}

/*
 * PM8150B on SID 2, read only. Fuel gauge (fg-gen4 register layout):
 * BATT_SOC MONOTONIC_SOC 0x4009 (0-255), BATT_INFO VBATT 0x41A0 (122.07 uV
 * per LSB), IBATT 0x41A2 (488.28 uA per LSB, positive = discharging).
 * Charger (smb5, same decode as SweetBattery for the PM6150): USBIN
 * INT_RT_STS 0x1310 bit 4, POWER_PATH_STATUS 0x110B, BATTERY_CHARGER_STATUS
 * 1/2/7 at 0x1006/0x1007/0x100D. Values checked on the phone, see
 * docs/research/ricarica.md section 0. Returns 0, or -1 if anything is off.
 */
int
andromeda_battery_sample(unsigned *percent, unsigned *mv, int *ma,
    bool *external, bool *capable, bool *charging, bool *full)
{
	uint8_t info_type, soc, usb, path, status, status2, temp;
	uint16_t vraw, iraw;
	if (!sp_ready || sp_read(2, 0x4104, &info_type) || info_type != 0x0d ||
	    sp_read(2, 0x4009, &soc) || !ap_word(2, 0x41a0, &vraw) ||
	    !ap_word(2, 0x41a2, &iraw) || sp_read(2, 0x1310, &usb) ||
	    sp_read(2, 0x110b, &path) || sp_read(2, 0x1006, &status) ||
	    sp_read(2, 0x1007, &status2) || sp_read(2, 0x100d, &temp)) {
		return -1;
	}
	unsigned millivolts = (unsigned)(((uint64_t)vraw * 12207u) / 100000u);
	if (millivolts < 2800 || millivolts > 4600) {
		return -1; /* never turn a bad sample into an empty battery */
	}
	int signed_raw = (iraw & 0x8000u) ? (int)iraw - 65536 : (int)iraw;
	/* IOPMPowerSource: positive amperage = charging. */
	*ma = (int)(-((int64_t)signed_raw * 48828) / 100000);
	*mv = millivolts;
	*percent = ((unsigned)soc * 100u + 127u) / 255u;
	*external = (usb & 0x10u) != 0;
	*capable = *external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
	unsigned state = status & 7u;
	*charging = *capable && state >= 1 && state <= 4 && !(status2 & 2u) && !(temp & 0x0cu);
	*full = *external && state == 5 && !(status2 & 2u);
	if (*full) {
		*percent = 100;
	}
	static unsigned logged;
	if (logged++ < 3 || (logged % 120u) == 0) {
		printf(AP_TAG " battery %u%% %umV %dmA external=%d charging=%d state=%u\n",
		    *percent, *mv, *ma, *external, *charging, state);
	}
	return 0;
}

/* ---- TLMM, GCC and SE13 ------------------------------------------------- */

static void
ap_tlmm(void)
{
	vm_offset_t east = andromeda_io_map(AP_TLMM_EAST_PA, AP_TLMM_EAST_SZ);
	if (!east) {
		printf(AP_TAG " TLMM EAST mapping failed\n");
		return;
	}
	static const char *const role[] = {"CTS", "RTS", "TX", "RX"};
	for (unsigned pin = 43; pin <= 46; ++pin) {
		uint32_t cfg = ap_rd(east, AP_GPIO_CFG(pin));
		uint32_t io = ap_rd(east, AP_GPIO_IO(pin));
		/* cfg: pull 1:0, func 5:2, drive 8:6, oe 9. io: in 0, out 1. */
		printf(AP_TAG " GPIO%u %s cfg=%#x func=%u pull=%u drv=%u oe=%u io=%#x\n",
		    pin, role[pin - 43], cfg, (cfg >> 2) & 0xfu, cfg & 3u,
		    (cfg >> 6) & 7u, (cfg >> 9) & 1u, io);
	}
}

static void
ap_se13_regs(vm_offset_t se)
{
	/* FW_REVISION_RO, GENI_STATUS, CLK_SEL, M/S_CLK_CFG, GENI_IOS, HW_PARAM_0/1. */
	uint32_t fw = ap_rd(se, 0x68u);
	printf(AP_TAG " SE13 fw=%#x proto=%u status=%#x clk_sel=%#x mclk=%#x sclk=%#x ios=%#x hw0=%#x hw1=%#x\n",
	    fw, (fw >> 8) & 0xffu, ap_rd(se, 0x40u), ap_rd(se, 0x7cu),
	    ap_rd(se, 0x48u), ap_rd(se, 0x4cu), ap_rd(se, 0x908u),
	    ap_rd(se, 0xe24u), ap_rd(se, 0xe28u));
}

static bool
ap_s3_on(vm_offset_t gcc)
{
	return cepheus_branch_enabled(ap_rd(gcc, AP_GCC_M_AHB), 0) &&
	       cepheus_branch_enabled(ap_rd(gcc, AP_GCC_S_AHB), 1) &&
	       cepheus_branch_enabled(ap_rd(gcc, AP_GCC_S3), 0);
}

static void
ap_gcc_se13(int level)
{
	vm_offset_t gcc = andromeda_io_map(AP_GCC_PA, AP_GCC_SZ);
	vm_offset_t se = andromeda_io_map(AP_SE13_PA, AP_SE13_SZ);
	if (!gcc || !se) {
		printf(AP_TAG " GCC/SE13 mapping failed\n");
		return;
	}
	uint32_t vote = ap_rd(gcc, AP_GCC_VOTE);
	printf(AP_TAG " GCC vote=%#x m_ahb=%#x s_ahb=%#x s3=%#x s3_rcg=%#x cfg=%#x dfs=%#x\n",
	    vote, ap_rd(gcc, AP_GCC_M_AHB), ap_rd(gcc, AP_GCC_S_AHB), ap_rd(gcc, AP_GCC_S3),
	    ap_rd(gcc, AP_GCC_S3_RCG), ap_rd(gcc, AP_GCC_S3_RCG + 4u),
	    ap_rd(gcc, AP_GCC_S3_RCG + 0x14u));
	if (ap_s3_on(gcc)) {
		ap_se13_regs(se);
		return;
	}
	if (level < 2) {
		printf(AP_TAG " SE13 clocks off: registers not read (andrprobe=2 votes s3 briefly)\n");
		return;
	}
	if (!cepheus_branch_enabled(ap_rd(gcc, AP_GCC_M_AHB), 0) ||
	    !cepheus_branch_enabled(ap_rd(gcc, AP_GCC_S_AHB), 1)) {
		printf(AP_TAG " wrapper 2 AHB clocks off: not voting\n");
		return;
	}
	/* The only GCC write: set the s3 vote bit, preserving every other voter. */
	*(volatile uint32_t *)(gcc + AP_GCC_VOTE) = vote | AP_GCC_S3_BIT;
	__asm__ volatile("dsb sy" ::: "memory");
	bool on = false;
	for (unsigned i = 0; i < 5000 && !on; ++i) {
		on = ap_s3_on(gcc);
		if (!on) {
			delay(1);
		}
	}
	printf(AP_TAG " s3 vote set: CBCR=%#x %s\n", ap_rd(gcc, AP_GCC_S3), on ? "on" : "still off");
	if (on) {
		ap_se13_regs(se);
	}
	if (!(vote & AP_GCC_S3_BIT)) {
		*(volatile uint32_t *)(gcc + AP_GCC_VOTE) = ap_rd(gcc, AP_GCC_VOTE) & ~AP_GCC_S3_BIT;
		__asm__ volatile("dsb sy" ::: "memory");
	}
	printf(AP_TAG " s3 vote restored: vote=%#x\n", ap_rd(gcc, AP_GCC_VOTE));
}

/* ---- thread ------------------------------------------------------------- */

static int ap_level;
static void ap_boost(void);
void andromeda_boost_rail(void);
/* andromeda_ctl.c: the hardware turn shared with the gpu, bt, cam, hap, wlan runs. */
extern void andromeda_ctl_turn(const char *who);
extern void andromeda_ctl_turn_end(void);

static void
ap_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 100;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("andrprobedelay", &delay_s, sizeof(delay_s));
	printf(AP_TAG " level %d, starting in %u s\n", ap_level, delay_s);
	ap_wait(delay_s * 1000u);
	printf(AP_TAG " begin (read only%s)\n", ap_level >= 2 ? ", brief s3 vote" : "");
	ap_tlmm();
	if (ap_level >= 2) {
		andromeda_ctl_turn("probe s3 vote");   /* the Bluetooth run votes the same bit */
	}
	ap_gcc_se13(ap_level);
	if (ap_level >= 2) {
		andromeda_ctl_turn_end();
	}
	ap_pmic();
	ap_boost();
	printf(AP_TAG " done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* ---- reboot into the bootloader (andrfastboot=N) -------------------------
 *
 * The same sequence as Linux msm-poweroff.c "reboot bootloader" (Xiaomi
 * andromeda-p-oss): IMEM restart_reason = 0x77665500, PM8150 PON
 * SOFT_RB_SPARE (SID 0, 0x88F, gen2: reason in bits 7:1) = BOOTLOADER (2),
 * then a reset. ABL reads the reason through the reset-reason protocol and
 * FASTBOOT_MODE is 2 (QcomModulePkg ShutdownServices.h, LinuxLoader.c). The
 * reset is PSCI SYSTEM_RESET over SMC, the conduit ios-sweet uses for
 * CPU_ON. Disks are synced first. The PON write is done only here, on
 * request; the only other PMIC writes in this file are the haptics ones
 * below (andromeda_spmi_hap_write, andrhap=1) and the vdd_boost_vreg
 * enable (ap_boost, andrboost=1).
 *
 * Verified on the phone 2026-11-07 ("andctl.py fastboot"): ABL honours the
 * reason and stops in fastboot, 9 s from the command.
 */
#define AP_CHNLS_PA        0x0c600000u  /* arbiter v5 "chnls": 0x10000 per APID */
#define AP_PON_SPARE       0x088fu
#define AP_IMEM_PA         0x146bf000u
#define AP_IMEM_RESTART    0x65cu
#define AP_PSCI_SYSTEM_RESET 0x84000009ull

struct sync_args;
struct proc;
extern int sync(struct proc *, struct sync_args *, int32_t *);

/* One observer read of a lane outside the probe allow-list (PON spare only). */
static int
sp_read_pon_spare(uint8_t *out)
{
	IOLockLock(sp_lock);
	uint16_t apid = sp_find_apid(0x008u);
	int err = -1;
	if (apid != AP_NO_APID && apid < 512 && !sp_poisoned[apid]) {
		unsigned off = 0x80u * apid;
		__asm__ volatile("dmb oshst" ::: "memory");
		*(volatile uint32_t *)(sp_obs + off) = (1u << 27) | ((AP_PON_SPARE & 0xffu) << 4);
		__asm__ volatile("dsb osh" ::: "memory");
		err = -3;
		for (unsigned i = 0; i < 1000; ++i) {
			uint32_t status = ap_rd(sp_obs, off + 0x08u);
			if (status & 1u) {
				err = (status & 0x0eu) ? -2 : 0;
				if (!err) {
					*out = (uint8_t)ap_rd(sp_obs, off + 0x18u);
				}
				break;
			}
			delay(1);
		}
	}
	IOLockUnlock(sp_lock);
	return err;
}

/* EXT_WRITEL of one byte to the PON spare register through our own channel. */
static int
sp_write_pon_spare(uint8_t value)
{
	IOLockLock(sp_lock);
	int err = -1;
	uint16_t apid = sp_find_apid(0x008u);
	unsigned owner = apid < 512 ? (ap_rd(sp_cfg, 0x700u + 4u * apid) & 7u) : 7u;
	vm_offset_t ch = (apid < 512 && owner == 0) ?
	    andromeda_io_map(AP_CHNLS_PA + 0x10000u * apid, 0x1000) : 0;
	if (!ch) {
		printf(AP_TAG " PON APID %u owner %u: not ours, no write\n", apid, owner);
	} else {
		*(volatile uint32_t *)(ch + 0x10u) = value;               /* WDATA0 */
		__asm__ volatile("dmb oshst" ::: "memory");
		*(volatile uint32_t *)(ch + 0x00u) = (0u << 27) | ((AP_PON_SPARE & 0xffu) << 4);
		__asm__ volatile("dsb osh" ::: "memory");
		err = -3;
		for (unsigned i = 0; i < 1000; ++i) {
			uint32_t status = ap_rd(ch, 0x08u);
			if (status & 1u) {
				err = (status & 0x0eu) ? -2 : 0;
				break;
			}
			delay(1);
		}
	}
	IOLockUnlock(sp_lock);
	return err;
}

static void
ap_reboot_bootloader(const char *why)
{
	printf(AP_TAG " REBOOT to fastboot (%s): syncing disks\n", why);
	(void)sync(NULL, NULL, NULL);
	ap_wait(3000);
	(void)sync(NULL, NULL, NULL);
	ap_wait(1000);
	if (!sp_init()) {
		printf(AP_TAG " REBOOT aborted: SPMI unavailable\n");
		return;
	}
	uint8_t spare = 0, again = 1;
	int rd = sp_read_pon_spare(&spare);
	if (!rd) {
		rd = sp_read_pon_spare(&again);
	}
	if (rd || spare != again) {
		printf(AP_TAG " REBOOT aborted: PON spare read err=%d (%#x/%#x)\n", rd, spare, again);
		return;
	}
	uint8_t value = (uint8_t)((spare & 1u) | (2u << 1)); /* PON_RESTART_REASON_BOOTLOADER */
	int wr = sp_write_pon_spare(value);
	uint8_t check = 0;
	int rb = sp_read_pon_spare(&check);
	printf(AP_TAG " REBOOT PON spare %#x -> %#x write=%d readback=%#x (%d)\n",
	    spare, value, wr, check, rb);
	if (wr || rb || check != value) {
		printf(AP_TAG " REBOOT aborted: the reason did not stick\n");
		return;
	}
	vm_offset_t imem = andromeda_io_map(AP_IMEM_PA, 0x1000);
	if (imem) {
		*(volatile uint32_t *)(imem + AP_IMEM_RESTART) = 0x77665500u;
		__asm__ volatile("dsb sy" ::: "memory");
	}
	printf(AP_TAG " REBOOT IMEM restart_reason=%#x; PSCI SYSTEM_RESET now\n",
	    imem ? ap_rd(imem, AP_IMEM_RESTART) : 0u);
	ap_wait(200);
	register uint64_t x0 __asm__("x0") = AP_PSCI_SYSTEM_RESET;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	printf(AP_TAG " REBOOT: PSCI SYSTEM_RESET returned %#llx\n", (unsigned long long)x0);
}

/* ---- PM8150B haptics registers for andromeda_haptics.c (andrhap=1) -------
 *
 * One-byte EXT_WRITEL to the PM8150B haptics block (SID 3, 0xC000), with the
 * checks of sp_write_pon_spare(): before every write the APID map must still
 * give the block's PPID 0x3C0 and the SPMI ownership table must name our
 * execution environment (EE 0) as the channel's owner; every write is then
 * read back through the observer and compared. Command format and status
 * bits as in Linux spmi-pmic-arb.c (arbiter v5 "chnls" RW channel), used as
 * documentation only.
 *
 * Writable: only the thirteen registers in ap_hap_writable[] (offsets 0x46 to
 * 0x70, the ones qti-haptics.c programs for a constant LRA vibration). Never
 * 0xD0 or above (SEC_ACCESS range), never another peripheral. A timed-out
 * write disables the channel for the rest of the boot, except for the two
 * "motor off" writes (PLAY = 0, EN_CTL1 = 0), which are always allowed.
 *
 * Return values of andromeda_spmi_hap_write(): 0 written and read back equal;
 * -1 refused (register not allowed, SPMI not ready, no APID, channel not
 * ours, channel disabled); -2 arbiter FAILURE/DENIED; -3 timeout; -4 dropped
 * three times; -5 read back differs (*readback holds it); -6 written, read
 * back failed.
 */
#define AP_HAP_SID      3u
#define AP_HAP_PERIPH   0xc0u
#define AP_HAP_PPID     ((AP_HAP_SID << 8) | AP_HAP_PERIPH)
#define AP_HAP_EN_CTL1  0x46u
#define AP_HAP_PLAY     0x70u

int andromeda_spmi_hap_open(unsigned *apid, unsigned *owner);
int andromeda_spmi_hap_read(unsigned reg, uint8_t *value);
int andromeda_spmi_hap_write(unsigned reg, uint8_t value, uint8_t *readback);
void andromeda_spmi_hap_quiesce(void);

/*
 * EN_CTL1, EN_CTL3, AUTO_RES_CTRL, CFG1, CFG2, SEL, AUTO_RES_CFG, VMAX_CFG,
 * ILIM_CFG, RATE_CFG1, RATE_CFG2, ZX_CFG, PLAY (qti-haptics.c register map).
 */
static const uint8_t ap_hap_writable[] = {0x46, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
	0x51, 0x52, 0x54, 0x55, 0x5a, 0x70};

static vm_offset_t sp_hap_ch;            /* RW channel, mapped once */
static uint16_t sp_hap_apid = AP_NO_APID;
static volatile bool sp_hap_disabled;    /* a write timed out */

static bool
sp_hap_writable(unsigned reg)
{
	if (reg < AP_HAP_EN_CTL1 || reg > AP_HAP_PLAY) {
		return false;
	}
	for (unsigned i = 0; i < AP_N(ap_hap_writable); ++i) {
		if (ap_hap_writable[i] == reg) {
			return true;
		}
	}
	return false;
}

/* The block's RW channel if it is still ours, else 0. Called with sp_lock held. */
static vm_offset_t
sp_hap_channel_locked(unsigned *apid_out, unsigned *owner_out)
{
	uint16_t apid = sp_find_apid(AP_HAP_PPID);
	unsigned owner = apid < 512 ? (ap_rd(sp_cfg, 0x700u + 4u * apid) & 7u) : 7u;
	if (apid_out) {
		*apid_out = apid;
	}
	if (owner_out) {
		*owner_out = owner;
	}
	if (apid >= 512 || owner != 0) {
		return 0;
	}
	if (sp_hap_ch) {
		return sp_hap_apid == apid ? sp_hap_ch : 0; /* the map changed: refuse */
	}
	vm_offset_t ch = andromeda_io_map(AP_CHNLS_PA + 0x10000u * apid, 0x1000);
	if (ch) {
		sp_hap_apid = apid;
		__asm__ volatile("dmb ish" ::: "memory");
		sp_hap_ch = ch;
	}
	return ch;
}

/* One EXT_WRITEL (opcode 0, one byte) on the RW channel; bounded wait. */
static int
sp_hap_cmd(vm_offset_t ch, unsigned reg, uint8_t value)
{
	*(volatile uint32_t *)(ch + 0x10u) = value;                       /* WDATA0 */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ch + 0x00u) = (0u << 27) | ((reg & 0xffu) << 4); /* CMD */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t status = ap_rd(ch, 0x08u);
		if (status & 1u) {                       /* DONE */
			if (status & 0x06u) {            /* FAILURE, DENIED */
				return -2;
			}
			return (status & 0x08u) ? -4 : 0; /* DROPPED */
		}
		delay(1);
	}
	return -3;
}

int
andromeda_spmi_hap_open(unsigned *apid, unsigned *owner)
{
	*apid = AP_NO_APID;
	*owner = 7;
	if (!sp_init()) {
		return -1;
	}
	IOLockLock(sp_lock);
	vm_offset_t ch = sp_hap_channel_locked(apid, owner);
	IOLockUnlock(sp_lock);
	return ch ? 0 : -1;
}

int
andromeda_spmi_hap_read(unsigned reg, uint8_t *value)
{
	if (reg >= 0xd0u) {
		return -1;
	}
	return sp_read(AP_HAP_SID, (AP_HAP_PERIPH << 8) | reg, value);
}

int
andromeda_spmi_hap_write(unsigned reg, uint8_t value, uint8_t *readback)
{
	bool off = (reg == AP_HAP_PLAY || reg == AP_HAP_EN_CTL1) && value == 0;
	if (!sp_ready || !sp_lock || !sp_hap_writable(reg) || (sp_hap_disabled && !off)) {
		return -1;
	}
	IOLockLock(sp_lock);
	int err = -1;
	vm_offset_t ch = sp_hap_channel_locked(NULL, NULL);
	if (ch) {
		for (unsigned attempt = 0; attempt < 3; ++attempt) {
			err = sp_hap_cmd(ch, reg, value);
			if (err != -4) {
				break;
			}
		}
		if (err == -3) {
			sp_hap_disabled = true;
			printf(AP_TAG " haptics write %#x timed out: channel %u disabled except motor off\n",
			    reg, sp_hap_apid);
		}
	}
	if (!err) {
		uint8_t check = 0;
		if (sp_read_locked(AP_HAP_SID, (AP_HAP_PERIPH << 8) | reg, &check)) {
			err = -6;
		} else {
			if (readback) {
				*readback = check;
			}
			err = check == value ? 0 : -5;
		}
	}
	IOLockUnlock(sp_lock);
	return err;
}

/*
 * Panic path (kdp callout of andromeda_haptics.c): PLAY = 0, EN_CTL1 = 0 on
 * the channel already checked by an earlier write. Interrupts are off and the
 * other CPUs are stopped, so no lock (its holder may be stopped) and only a
 * bounded wait; the owner is looked at once more. No read back.
 */
void
andromeda_spmi_hap_quiesce(void)
{
	vm_offset_t ch = sp_hap_ch;
	uint16_t apid = sp_hap_apid;
	if (!ch || apid >= 512 || (ap_rd(sp_cfg, 0x700u + 4u * apid) & 7u) != 0) {
		return;
	}
	(void)sp_hap_cmd(ch, AP_HAP_PLAY, 0);
	(void)sp_hap_cmd(ch, AP_HAP_EN_CTL1, 0);
}

/* ---- vdd_boost_vreg on PM8150B GPIO5 (andrboost=1) ----------------------
 *
 * Touch investigation, 2026-11-07: in TWRP the sensor works and this fixed
 * regulator is on; the live DT gives it no consumer (vdd_boost_vreg,
 * regulator-fixed, gpio = <&pm8150b_gpios 5 0>, enable-active-high,
 * regulator-always-on), so Linux turns it on at boot and never off. Linux
 * does it through pinctrl-spmi-gpio.c direction_output (used as documentation
 * only): MODE_CTL = digital output (1), DIG_OUT_SOURCE_CTL = output high
 * (invert bit 7, source 0 = constant), EN_CTL = master enable (bit 7); the
 * other registers keep what the bootloader set.
 *
 * Off unless andrboost=1. Only these three registers of this one GPIO, only
 * if TYPE/SUBTYPE say PMIC GPIO LV/MV and the GPIO is not already an enabled
 * high output, only on a channel our EE owns; each write is read back. PMIC
 * register state is volatile (lost at reset): nothing reaches a partition.
 */
#define AP_BOOST_SID    2u
#define AP_BOOST_PERIPH 0xc4u
#define AP_BOOST_PPID   ((AP_BOOST_SID << 8) | AP_BOOST_PERIPH)

static void
ap_boost(void)
{
	int on = 0;
	(void)PE_parse_boot_argn("andrboost", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (!sp_ready || !sp_lock) {
		printf(AP_TAG " boost: SPMI not ready\n");
		return;
	}
	static const uint8_t regs[] = {0x04, 0x05, 0x08, 0x40, 0x44, 0x45, 0x46};
	uint8_t v[sizeof(regs)] = {0};
	for (unsigned i = 0; i < sizeof(regs); ++i) {
		if (sp_read(AP_BOOST_SID, (AP_BOOST_PERIPH << 8) | regs[i], &v[i])) {
			printf(AP_TAG " boost: read of %#x failed; not writing\n", regs[i]);
			return;
		}
	}
	printf(AP_TAG " boost GPIO5 before: type=%#x subtype=%#x status1=%#x mode=%#x out_src=%#x out_ctl=%#x en=%#x\n",
	    v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
	if (v[0] != 0x10 || v[1] < 0x10 || v[1] > 0x13) {
		printf(AP_TAG " boost: not a PMIC GPIO LV/MV; not writing\n");
		return;
	}
	if ((v[6] & 0x80) && (v[3] & 0x3) == 1 && (v[4] & 0x80)) {
		printf(AP_TAG " boost: already an enabled high output\n");
		return;
	}
	static const struct { uint8_t reg, value; } seq[] = {
		{0x40, 0x01}, /* MODE_CTL: digital output */
		{0x44, 0x80}, /* DIG_OUT_SOURCE_CTL: high, constant source */
		{0x46, 0x80}, /* EN_CTL: enable */
	};
	IOLockLock(sp_lock);
	uint16_t apid = sp_find_apid(AP_BOOST_PPID);
	unsigned owner = apid < 512 ? (ap_rd(sp_cfg, 0x700u + 4u * apid) & 7u) : 7u;
	vm_offset_t ch = (apid < 512 && owner == 0 && !sp_poisoned[apid]) ?
	    andromeda_io_map(AP_CHNLS_PA + 0x10000u * apid, 0x1000) : 0;
	if (!ch) {
		IOLockUnlock(sp_lock);
		printf(AP_TAG " boost: channel APID %u owner %u not ours; not writing\n", apid, owner);
		return;
	}
	for (unsigned i = 0; i < AP_N(seq); ++i) {
		int err = -4;
		for (unsigned attempt = 0; attempt < 3 && err == -4; ++attempt) {
			err = sp_hap_cmd(ch, seq[i].reg, seq[i].value);
		}
		uint8_t check = 0;
		if (!err) {
			err = sp_read_locked(AP_BOOST_SID, (AP_BOOST_PERIPH << 8) | seq[i].reg, &check) ? -6 :
			    (check == seq[i].value ? 0 : -5);
		}
		printf(AP_TAG " boost write %#x=%#x: err=%d readback=%#x\n", seq[i].reg, seq[i].value, err, check);
		if (err) {
			if (err == -3) {
				sp_poisoned[apid] = true;
			}
			break;
		}
	}
	uint8_t st = 0;
	int serr = sp_read_locked(AP_BOOST_SID, (AP_BOOST_PERIPH << 8) | 0x08, &st);
	IOLockUnlock(sp_lock);
	printf(AP_TAG " boost GPIO5 after: status1=%#x (err %d)\n", st, serr);
}

/*
 * Touch (cepheus_touch.c) enables vdd_boost_vreg itself before powering the
 * controller, at its own start instead of the probe's (andrprobedelay).
 * Once per boot; the probe then finds an enabled high output.
 */
void
andromeda_boost_rail(void)
{
	static uint32_t done;
	if (__atomic_exchange_n(&done, 1u, __ATOMIC_ACQ_REL)) {
		return;
	}
	if (!sp_init()) {
		printf(AP_TAG " boost: SPMI not ready\n");
		return;
	}
	ap_boost();
}

/* ---- camera slider (andrslider=1) ---------------------------------------
 *
 * Two Hall sensors on TLMM GPIO 42 (NORTH tile) and 114 (SOUTH tile), as the
 * Xiaomi halls.c (DEF_HALL_GPIO_1/2) and the LineageOS rewrite by Demon000
 * read them: (42, 114) = (0, 1) slider down, (1, 0) slider up, anything else
 * moving; which one is open is to be confirmed on the phone (docs/research/sensori-vibrazione.md 1.3). Read only: the IN bit of
 * GPIO_IN_OUT, polled every 100 ms; pin configuration is not changed.
 */
#define AP_TLMM_NORTH_PA 0x03900000u
#define AP_TLMM_SOUTH_PA 0x03d00000u

static const char *
ap_slider_name(unsigned h42, unsigned h114)
{
	/* LineageOS: (0, 1) = STATE_SLIDER_DOWN = open; to be confirmed on the phone. */
	if (h42 == 0 && h114 == 1) {
		return "slider down (open?)";
	}
	if (h42 == 1 && h114 == 0) {
		return "slider up (closed?)";
	}
	return "moving";
}

static void
ap_slider_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	vm_offset_t north = andromeda_io_map(AP_TLMM_NORTH_PA, 0x30000);
	vm_offset_t south = andromeda_io_map(AP_TLMM_SOUTH_PA, 0x80000);
	if (!north || !south) {
		printf("ANDROMEDA SLIDER: TLMM mapping failed\n");
		goto out;
	}
	printf("ANDROMEDA SLIDER: GPIO42 cfg=%#x GPIO114 cfg=%#x (read only)\n",
	    ap_rd(north, AP_GPIO_CFG(42)), ap_rd(south, AP_GPIO_CFG(114)));
	unsigned last = 0xff, changes = 0;
	for (;;) {
		unsigned h42 = ap_rd(north, AP_GPIO_IO(42)) & 1u;
		unsigned h114 = ap_rd(south, AP_GPIO_IO(114)) & 1u;
		unsigned now = (h42 << 1) | h114;
		if (now != last) {
			if (changes++ < 200u) {
				printf("ANDROMEDA SLIDER: hall42=%u hall114=%u -> %s\n", h42, h114,
				    ap_slider_name(h42, h114));
			}
			andromeda_slider_publish(h42, h114); /* andrslidercam, off by default */
			last = now;
		}
		ap_wait(100);
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static unsigned ap_fastboot_s;
static bool ap_slider_started;  /* the slider thread exists (andrslider=1 or "slider 1") */
static bool ap_fastboot_now;    /* "fastboot" accepted, reboot in progress */

static void
ap_fastboot_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	printf(AP_TAG " will reboot into fastboot in %u s (andrfastboot)\n", ap_fastboot_s);
	ap_wait(ap_fastboot_s * 1000u);
	ap_reboot_bootloader("timer");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* "fastboot": one second for the answer to leave, then the timer's sequence. */
static void
ap_fastboot_now_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	ap_wait(1000);
	ap_reboot_bootloader("andrctl");
	/* Back here only if the reboot was aborted (logged): may be asked again. */
	__atomic_store_n(&ap_fastboot_now, false, __ATOMIC_RELEASE);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
ap_start_slider(void)
{
	thread_t thread;
	if (__atomic_exchange_n(&ap_slider_started, true, __ATOMIC_ACQ_REL)) {
		return true;
	}
	if (kernel_thread_start(ap_slider_thread, NULL, &thread) != KERN_SUCCESS) {
		__atomic_store_n(&ap_slider_started, false, __ATOMIC_RELEASE);
		return false;
	}
	thread_deallocate(thread);
	return true;
}

/* Runtime control (andromeda_ctl.c); 0 = accepted. */
int andromeda_probe_slider_ctl(char *out, size_t len);
int andromeda_probe_fastboot_ctl(char *out, size_t len);
bool andromeda_probe_slider_on(void);

bool
andromeda_probe_slider_on(void)
{
	return __atomic_load_n(&ap_slider_started, __ATOMIC_ACQUIRE);
}

int
andromeda_probe_slider_ctl(char *out, size_t len)
{
	if (andromeda_probe_slider_on()) {
		snprintf(out, len, "the slider reader already runs (read only, every 100 ms)");
		return 0;
	}
	if (!ap_start_slider()) {
		snprintf(out, len, "kernel_thread_start failed");
		return EAGAIN;
	}
	snprintf(out, len, "slider reader started (read only, GPIO 42/114 every 100 ms; "
	    "\"slidercam N\" publishes it to andromedad)");
	return 0;
}

int
andromeda_probe_fastboot_ctl(char *out, size_t len)
{
	thread_t thread;
	if (__atomic_exchange_n(&ap_fastboot_now, true, __ATOMIC_ACQ_REL)) {
		snprintf(out, len, "a reboot into fastboot is already in progress");
		return EBUSY;
	}
	if (kernel_thread_start(ap_fastboot_now_thread, NULL, &thread) != KERN_SUCCESS) {
		__atomic_store_n(&ap_fastboot_now, false, __ATOMIC_RELEASE);
		snprintf(out, len, "kernel_thread_start failed");
		return EAGAIN;
	}
	thread_deallocate(thread);
	printf(AP_TAG " fastboot asked over andrctl: rebooting in 1 s\n");
	snprintf(out, len, "syncing disks and rebooting into fastboot in about 5 s (the andrfastboot "
	    "sequence; ANDROMEDA PROBE: REBOOT in the log)");
	return 0;
}

void
andromeda_probe_init(void)
{
	static bool started;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrfastboot", &ap_fastboot_s, sizeof(ap_fastboot_s));
	if (ap_fastboot_s >= 60 &&
	    kernel_thread_start(ap_fastboot_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	unsigned slider = 0;
	(void)PE_parse_boot_argn("andrslider", &slider, sizeof(slider));
	if (slider == 1) {
		(void)ap_start_slider();
	}
	(void)PE_parse_boot_argn("andrprobe", &ap_level, sizeof(ap_level));
	if (ap_level <= 0) {
		return;
	}
	if (kernel_thread_start(ap_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
