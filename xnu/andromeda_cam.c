/*
 * Mi Mix 3 5G (andromeda, SM8150): rear main camera (Sony IMX363) chip-ID
 * read, step C1 of docs/research/fotocamere.md. Off unless andrcam=1.
 *
 * Boot-args:
 *   andrcam=1         once, andrcamdelay seconds after start: power up only
 *                     the IMX363 with the sequence of its vendor module file,
 *                     read register 0x0016-0x0017 (expected 0x0363) over a
 *                     GPIO-driven I2C on the CCI0 master 0 pins, log it on an
 *                     "ANDROMEDA CAM:" line and power everything down again
 *                     with the module's power-down sequence. Any error stops
 *                     the sequence and powers down the same way.
 *   andrcamdelay=N    seconds before starting (default 120).
 *   andrcamaddr=0x1a  7-bit address to ask instead of the module's 0x10
 *                     (only 0x10 and 0x1a are accepted; one address per boot).
 *
 * Power sequence: com.qti.sensormodule.andromeda_semco_imx363.bin from the
 * phone's vendor partition (not in git), slaveInfo: address 0x20 (8-bit),
 * id register 0x0016, id 0x0363; powerUpSequence, as msm_camera_power_seq_type
 * values (Xiaomi andromeda-p-oss cam_sensor_cmn_header.h):
 *   CUSTOM_REG1 1 ms, VANA 1 ms, VIO 1 ms, VDIG 1 ms, RESET=1 12 ms,
 *   MCLK 19.2 MHz 10 ms;
 * powerDownSequence: RESET=0, VDIG, VIO, VANA, CUSTOM_REG1, MCLK, 1 ms each.
 * What each step drives comes from the live DT node qcom,cam-sensor@0 and
 * from cam_sensor_core_power_up() in the Xiaomi kernel (cam_sensor_util.c,
 * GPL-2.0, read as documentation only):
 *   CUSTOM_REG1 = cam_v_custom1, RPMh ldoa17 (PM8150 L17A), DT min 2856 mV;
 *   VANA = cam_vana (BOB, shared, never voted here) + gpio-vana GPIO 21;
 *   VIO  = cam_vio, regulator-fixed camera_viodd_ldo enabled by GPIO 34;
 *   VDIG = cam_vdig, RPMh smpc8 (PM8150L S8C), 1352 mV;
 *   RESET = gpio-reset GPIO 28;
 *   MCLK = cam_clk (the titan_top GDSC) then CAM_CC_MCLK0_CLK at 19.2 MHz
 *          (XO, no PLL) on GPIO 13 in its cam_mclk function.
 * The two RPMh votes go through andromeda_rpmh_cam_rail() of patch
 * 0098-andromeda-cam-rpmh.patch (closed list, DT voltages only).
 *
 * Register layout from Linux, used as documentation only (GPL-2.0, nothing
 * copied): camcc-sm8150.c (MCLK0 RCG 0x5004 / CBCR 0x501c, titan_top GDSC
 * 0xc1bc, always-on CAM_CC_GDSC_CLK 0xc1e4), gdsc.c, clk-rcg2.c (shared RCG
 * update), clk-branch.c, gcc-sm8150.c (GCC_CAMERA_AHB_CLK 0xb008,
 * GCC_CAMERA_XO_CLK 0xb044) and pinctrl-sm8150.c (tiles and functions).
 * The bounded I2C transport is cepheus_i2c_bitbang.h from YangQi0408's
 * UniXNU-cepheus port; driving the CCI pins as GPIOs for this is the method
 * of nomorecoolnicknames' hakios-sm8150 (vayu, idea only), and "MCLK from
 * XO, GDSC before the MCLK branch, one address per try" are lessons from
 * Yofuness' ios-sunfish camera notes (MIT). See CREDITS.md.
 *
 * Writes, all undone at the end: TLMM GPIO 13/17/18/21/28/34 configuration
 * and output latch; camcc CAM_CC_GDSC_CLK enable (only if off), titan_top
 * GDSCR SW_COLLAPSE (only if collapsed), MCLK0 RCG (only if not XO/1) and
 * MCLK0 branch enable; the two RPMh votes. camcc is touched only while
 * GCC_CAMERA_AHB_CLK runs. No sensor register is written (only the 16-bit
 * register pointer of the read), no CCI, CSIPHY, CSID, IFE, ISP, SMMU or DMA.
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
#include "cepheus_clock_status.h"
#include "cepheus_i2c_bitbang.h"

extern vm_offset_t andromeda_io_map(vm_offset_t, vm_size_t); /* andromeda_probe.c: shared, outside io_submap */
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
/* bsd/dev/sweet_rpmh.c, patch 0098-andromeda-cam-rpmh: ldoa17/smpc8 only. */
extern int andromeda_rpmh_cam_rail(const char *res, bool on);
void andromeda_cam_init(void);

#define CAM_TAG "ANDROMEDA CAM:"

/* TLMM tiles (pinctrl-sm8150.c): 13/17/18/34 NORTH, 21/28 EAST. */
#define CAM_TLMM_NORTH_PA 0x03900000u
#define CAM_TLMM_EAST_PA  0x03500000u
#define CAM_TLMM_SZ       0x00030000u
#define CAM_GPIO_CFG(n)   ((uint32_t)(n) * 0x1000u)
#define CAM_GPIO_IO(n)    (CAM_GPIO_CFG(n) + 4u)
#define PIN_CFG_MASK      0x3ffu       /* pull 1:0, func 5:2, drive 8:6, oe 9 */
#define PIN_FUNC(cfg)     (((cfg) >> 2) & 0xfu)
#define PIN_PULL_NONE     0u
#define PIN_PULL_UP       3u
#define PIN_DRV(ma)       ((((ma) / 2u) - 1u) << 6)
#define PIN_OE            (1u << 9)
#define PIN_IN            (1u << 0)
#define PIN_OUT           (1u << 1)

/* GCC (gcc-sm8150.c): both branches are CLK_IS_CRITICAL in Linux. */
#define CAM_GCC_PA        0x00100000u
#define CAM_GCC_SZ        0x00010000u
#define GCC_CAMERA_AHB    0xb008u      /* hwcg bit 1; the camcc register bus */
#define GCC_CAMERA_XO     0xb044u

/* Camera clock controller (live DT qcom,camcc @0xad00000, camcc-sm8150.c). */
#define CAM_CC_PA         0x0ad00000u
#define CAM_CC_SZ         0x00010000u
#define CC_MCLK0_RCG      0x5004u      /* CMD; CFG at +4, mnd 8, hid 5 */
#define CC_MCLK0_CBCR     0x501cu
#define CC_TITAN_GDSCR    0xc1bcu      /* live DT qcom,gdsc@0xad0c1bc */
#define CC_GDSC_CLK       0xc1e4u      /* CAM_CC_GDSC_CLK, kept on by Linux */
#define RCG_CMD           0x0u
#define RCG_CFG           0x4u
#define RCG_UPDATE        (1u << 0)
#define RCG_ROOT_EN       (1u << 1)
#define RCG_ROOT_OFF      (1u << 31)
#define RCG_CFG_MASK      0x371fu      /* div 4:0, src 10:8, mode 13:12 */
#define RCG_XO_DIV1       0x1u         /* src 0 = BI_TCXO, div field 1 = /1 */
#define CBCR_EN           (1u << 0)
#define GDSC_SW_COLLAPSE  (1u << 0)
#define GDSC_HW_CONTROL   (1u << 1)
#define GDSC_PWR_ON       (1u << 31)

/* com.qti.sensormodule.andromeda_semco_imx363.bin slaveInfo. */
#define IMX363_ADDR       0x10u        /* slaveAddress 0x20 >> 1 */
#define IMX363_ALT_ADDR   0x1au        /* Pixel 4a / Pixel 5 (ios-sunfish) */
#define IMX363_ID_REG     0x0016u
#define IMX363_ID         0x0363u

enum { P_MCLK, P_SDA, P_SCL, P_VANA, P_RESET, P_VIO, P_COUNT };

static const struct {
	unsigned gpio;
	bool east;
	unsigned alt_func;   /* the other function we accept at boot (0 = gpio only) */
	const char *name;
} cam_pin[P_COUNT] = {
	[P_MCLK]  = {13, false, 1, "MCLK0"},           /* func 1 = cam_mclk */
	[P_SDA]   = {17, false, 1, "CCI0 SDA0"},       /* func 1 = cci_i2c */
	[P_SCL]   = {18, false, 1, "CCI0 SCL0"},
	[P_VANA]  = {21, true, 0, "CAM_VDD0_EN"},
	[P_RESET] = {28, true, 0, "CAM_RESET_W"},
	[P_VIO]   = {34, false, 0, "VIO_EN"},
};

static const char *const cam_rail_name[2] = {"ldoa17", "smpc8"};
#define RAIL_CUSTOM1 0u
#define RAIL_VDIG    1u

static struct {
	vm_offset_t north, east, gcc, cc;
	uint32_t saved_cfg[P_COUNT], saved_io[P_COUNT], bb_cfg[2];
	uint32_t saved_rcg_cfg;
	unsigned addr;
	unsigned rails;          /* bit i: cam_rail_name[i] on, or uncertain */
	bool pins_set, i2c_pins, vana, vio, reset;
	bool gdsc_clk_ours, gdsc_ours, rcg_changed, mclk_ours;
	bool cc_dead;            /* camcc must not be accessed any more */
} cam;

static unsigned cam_sleep_chan;
static unsigned cam_delay_s = 120;
static struct cepheus_bb cam_bb;

static void
cam_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&cam_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
cam_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
cam_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Poll until (reg & mask) == want, at most us microseconds. */
static bool
cam_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i <= us; ++i) {
		if ((cam_rd(base, off) & mask) == want) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* clk-branch.c: wait for CLK_OFF to clear (on) or to set (off). */
static bool
cam_poll_branch(vm_offset_t base, uint32_t off, bool on, unsigned us)
{
	for (unsigned i = 0; i <= us; ++i) {
		if (cepheus_branch_enabled(cam_rd(base, off), 0) == (int)on) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* ---- pins ---------------------------------------------------------------- */

static vm_offset_t
cam_tile(unsigned p)
{
	return cam_pin[p].east ? cam.east : cam.north;
}

static uint32_t
cam_cfg_word(unsigned p, unsigned func, unsigned pull, unsigned ma, bool oe)
{
	uint32_t cfg = (cam.saved_cfg[p] & ~PIN_CFG_MASK) | (func << 2) | pull | PIN_DRV(ma) |
	    (oe ? PIN_OE : 0u);
	if (cfg & (1u << 11)) { cfg |= 1u << 12; }   /* as cepheus_touch.c / andromeda_bt.c */
	return cfg;
}

/* Output pin of cam_sensor_active_wide / cam_sensor_viodd_en: gpio, no pull, 2 mA. */
static void
cam_out(unsigned p, bool high)
{
	vm_offset_t t = cam_tile(p);
	unsigned g = cam_pin[p].gpio;
	/* Latch first, then output enable (pinctrl-msm order). */
	cam_wr(t, CAM_GPIO_IO(g), (cam_rd(t, CAM_GPIO_IO(g)) & ~PIN_OUT) | (high ? PIN_OUT : 0u));
	cam_wr(t, CAM_GPIO_CFG(g), cam_cfg_word(p, 0, PIN_PULL_NONE, 2, true));
}

/* Configuration first (output off), then the latch: no glitch towards high. */
static void
cam_pin_restore(unsigned p)
{
	vm_offset_t t = cam_tile(p);
	unsigned g = cam_pin[p].gpio;
	cam_wr(t, CAM_GPIO_CFG(g), cam.saved_cfg[p]);
	cam_wr(t, CAM_GPIO_IO(g), cam.saved_io[p]);
}

/*
 * cam_sensor_core_power_up() starts with pinctrl "cam_default" and the GPIO
 * table request: GPIO 13 cam_mclk, no pull, 4 mA; GPIO 21/28 output low;
 * GPIO 34 is the regulator-fixed enable, low until VIO.
 */
static bool
cam_pins_default(void)
{
	cam.pins_set = true;
	cam_out(P_VANA, false);
	cam_out(P_RESET, false);
	cam_out(P_VIO, false);
	const uint32_t mclk_cfg = cam_cfg_word(P_MCLK, 1, PIN_PULL_NONE, 4, false);
	cam_wr(cam.north, CAM_GPIO_CFG(cam_pin[P_MCLK].gpio), mclk_cfg);
	bool ok = cam_rd(cam.north, CAM_GPIO_CFG(cam_pin[P_MCLK].gpio)) == mclk_cfg;
	for (unsigned p = P_VANA; p <= P_VIO; ++p) {
		ok &= cam_rd(cam_tile(p), CAM_GPIO_CFG(cam_pin[p].gpio)) ==
		    cam_cfg_word(p, 0, PIN_PULL_NONE, 2, true);
	}
	printf(CAM_TAG " pins cam_default: 13=%#x 21=%#x 28=%#x 34=%#x %s\n",
	    cam_rd(cam.north, CAM_GPIO_CFG(13)), cam_rd(cam.east, CAM_GPIO_CFG(21)),
	    cam_rd(cam.east, CAM_GPIO_CFG(28)), cam_rd(cam.north, CAM_GPIO_CFG(34)),
	    ok ? "ok" : "READBACK MISMATCH");
	return ok;
}

/* ---- I2C over GPIO 17/18 ------------------------------------------------- */

static void
cam_bb_drive(void *ctx, unsigned line, int low)
{
	(void)ctx;
	unsigned g = cam_pin[line == BB_SDA ? P_SDA : P_SCL].gpio;
	/* Latch stays zero. Released = input with pull-up; low = output enabled. */
	cam_wr(cam.north, CAM_GPIO_CFG(g), cam.bb_cfg[line] | (low ? PIN_OE : 0u));
}

static int
cam_bb_sample(void *ctx, unsigned line)
{
	(void)ctx;
	return (cam_rd(cam.north, CAM_GPIO_IO(cam_pin[line == BB_SDA ? P_SDA : P_SCL].gpio)) & PIN_IN) != 0;
}

static void
cam_bb_wait(void *ctx, unsigned us)
{
	(void)ctx;
	delay((int)us);
}

/* cci0_active: pull-up, 2 mA, here in the gpio function. */
static bool
cam_i2c_claim(void)
{
	cam.i2c_pins = true;
	for (unsigned line = 0; line < 2; ++line) {
		unsigned p = line == BB_SDA ? P_SDA : P_SCL, g = cam_pin[p].gpio;
		cam.bb_cfg[line] = cam_cfg_word(p, 0, PIN_PULL_UP, 2, false);
		cam_wr(cam.north, CAM_GPIO_CFG(g), cam.bb_cfg[line]);
		cam_wr(cam.north, CAM_GPIO_IO(g), cam.saved_io[p] & ~PIN_OUT);
	}
	cam_bb = (struct cepheus_bb){NULL, cam_bb_drive, cam_bb_sample, cam_bb_wait};
	delay(20);
	bool ok = cam_rd(cam.north, CAM_GPIO_CFG(17)) == cam.bb_cfg[BB_SDA] &&
	    cam_rd(cam.north, CAM_GPIO_CFG(18)) == cam.bb_cfg[BB_SCL];
	printf(CAM_TAG " GPIO I2C on 17/18: cfg %#x/%#x %s, SDA=%d SCL=%d\n",
	    cam_rd(cam.north, CAM_GPIO_CFG(17)), cam_rd(cam.north, CAM_GPIO_CFG(18)),
	    ok ? "ok" : "READBACK MISMATCH", cam_bb_sample(NULL, BB_SDA), cam_bb_sample(NULL, BB_SCL));
	return ok;
}

static void
cam_i2c_release(void)
{
	if (!cam.i2c_pins) {
		return;
	}
	bb_release(&cam_bb);
	cam_pin_restore(P_SDA);
	cam_pin_restore(P_SCL);
	cam.i2c_pins = false;
}

static const char *
cam_i2c_error(int err)
{
	switch (err) {
	case ENXIO: return "no ACK (NACK)";
	case EBUSY: return "SDA held low before START";
	case ETIMEDOUT: return "SCL held low (stretch timeout)";
	case EAGAIN: return "SDA low while released (arbitration)";
	default: return "error";
	}
}

/* ---- clocks ---------------------------------------------------------------- */

/* GCC_CAMERA_AHB_CLK must run (enable bit and status) for any camcc access. */
static bool
cam_cc_ok(const char *phase)
{
	uint32_t ahb = cam_rd(cam.gcc, GCC_CAMERA_AHB);
	if (cam.cc_dead || !(ahb & CBCR_EN) || !cepheus_branch_enabled(ahb, 1)) {
		if (!cam.cc_dead) {
			printf(CAM_TAG " %s: GCC_CAMERA_AHB_CLK off (%#x): camcc not touched any more\n",
			    phase, ahb);
		}
		cam.cc_dead = true;
		return false;
	}
	return true;
}

/* clk_rcg2_shared_ops: force the root on, write CFG, update, release the root. */
static bool
cam_rcg_write(uint32_t cfg)
{
	vm_offset_t r = cam.cc + CC_MCLK0_RCG;
	cam_wr(r, RCG_CMD, cam_rd(r, RCG_CMD) | RCG_ROOT_EN);
	bool root = cam_poll(r, RCG_CMD, RCG_ROOT_OFF, 0, 500);
	cam_wr(r, RCG_CFG, cfg);
	cam_wr(r, RCG_CMD, cam_rd(r, RCG_CMD) | RCG_UPDATE);
	bool done = cam_poll(r, RCG_CMD, RCG_UPDATE, 0, 500);
	cam_wr(r, RCG_CMD, cam_rd(r, RCG_CMD) & ~RCG_ROOT_EN);
	if (!root || !done) {
		printf(CAM_TAG " MCLK0 RCG %s cmd=%#x cfg=%#x\n", !root ? "root stays off" : "update timeout",
		    cam_rd(r, RCG_CMD), cam_rd(r, RCG_CFG));
	}
	return root && done;
}

/* SENSOR_MCLK: cam_clk regulator (titan_top GDSC), then the clock at 19.2 MHz. */
static bool
cam_mclk_on(void)
{
	if (!cam_cc_ok("MCLK on")) {
		return false;
	}
	vm_offset_t cc = cam.cc;
	uint32_t gclk = cam_rd(cc, CC_GDSC_CLK);
	if (!(gclk & CBCR_EN)) {
		cam_wr(cc, CC_GDSC_CLK, gclk | CBCR_EN);
		cam.gdsc_clk_ours = true;
		if (!cam_poll_branch(cc, CC_GDSC_CLK, true, 500)) {
			printf(CAM_TAG " CAM_CC_GDSC_CLK does not start (%#x)\n", cam_rd(cc, CC_GDSC_CLK));
			return false;
		}
	}
	uint32_t gdscr = cam_rd(cc, CC_TITAN_GDSCR);
	if (!(gdscr & GDSC_PWR_ON)) {
		cam_wr(cc, CC_TITAN_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
		cam.gdsc_ours = true;
		if (!cam_poll(cc, CC_TITAN_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON, 2000)) {
			printf(CAM_TAG " titan_top GDSC does not power on (%#x); MMCX too low?\n",
			    cam_rd(cc, CC_TITAN_GDSCR));
			return false;
		}
	}
	printf(CAM_TAG " gdsc titan_top %#x -> %#x (%s), gdsc_clk %#x -> %#x\n", gdscr,
	    cam_rd(cc, CC_TITAN_GDSCR), cam.gdsc_ours ? "powered on" : "was on", gclk,
	    cam_rd(cc, CC_GDSC_CLK));
	if (!cam_cc_ok("MCLK RCG")) {
		return false;
	}
	if ((cam.saved_rcg_cfg & RCG_CFG_MASK) > RCG_XO_DIV1) {
		cam.rcg_changed = true;
		if (!cam_rcg_write((cam.saved_rcg_cfg & ~RCG_CFG_MASK) | RCG_XO_DIV1)) {
			return false;
		}
	}
	cam_wr(cc, CC_MCLK0_CBCR, cam_rd(cc, CC_MCLK0_CBCR) | CBCR_EN);
	cam.mclk_ours = true;
	bool on = cam_poll_branch(cc, CC_MCLK0_CBCR, true, 500);
	printf(CAM_TAG " mclk0 xo 19.2 MHz: rcg cmd=%#x cfg=%#x (%s), cbcr=%#x %s\n",
	    cam_rd(cc, CC_MCLK0_RCG + RCG_CMD), cam_rd(cc, CC_MCLK0_RCG + RCG_CFG),
	    cam.rcg_changed ? "set" : "already XO/1", cam_rd(cc, CC_MCLK0_CBCR),
	    on ? "on" : "DOES NOT START");
	return on;
}

static void
cam_mclk_off(void)
{
	if (!cam.mclk_ours && !cam.rcg_changed && !cam.gdsc_ours && !cam.gdsc_clk_ours) {
		return;
	}
	if (!cam_cc_ok("MCLK off")) {
		printf(CAM_TAG " camcc left as is: mclk=%d rcg=%d gdsc=%d gdsc_clk=%d\n",
		    cam.mclk_ours, cam.rcg_changed, cam.gdsc_ours, cam.gdsc_clk_ours);
		return;
	}
	vm_offset_t cc = cam.cc;
	if (cam.mclk_ours) {
		cam_wr(cc, CC_MCLK0_CBCR, cam_rd(cc, CC_MCLK0_CBCR) & ~CBCR_EN);
		if (!cam_poll_branch(cc, CC_MCLK0_CBCR, false, 500)) {
			printf(CAM_TAG " MCLK0 branch does not stop (%#x)\n", cam_rd(cc, CC_MCLK0_CBCR));
		}
		cam.mclk_ours = false;
	}
	if (cam.rcg_changed) {
		cam.rcg_changed = !cam_rcg_write(cam.saved_rcg_cfg);
	}
	if (cam.gdsc_ours) {
		cam_wr(cc, CC_TITAN_GDSCR, cam_rd(cc, CC_TITAN_GDSCR) | GDSC_SW_COLLAPSE);
		if (!cam_poll(cc, CC_TITAN_GDSCR, GDSC_PWR_ON, 0, 2000)) {
			printf(CAM_TAG " titan_top GDSC does not collapse (%#x)\n", cam_rd(cc, CC_TITAN_GDSCR));
		}
		cam.gdsc_ours = false;
	}
	if (cam.gdsc_clk_ours) {
		cam_wr(cc, CC_GDSC_CLK, cam_rd(cc, CC_GDSC_CLK) & ~CBCR_EN);
		cam.gdsc_clk_ours = false;
	}
	printf(CAM_TAG " mclk0 off: cbcr=%#x rcg cfg=%#x%s, gdsc titan_top=%#x, gdsc_clk=%#x\n",
	    cam_rd(cc, CC_MCLK0_CBCR), cam_rd(cc, CC_MCLK0_RCG + RCG_CFG),
	    cam.rcg_changed ? " (NOT restored)" : "", cam_rd(cc, CC_TITAN_GDSCR),
	    cam_rd(cc, CC_GDSC_CLK));
}

/* ---- rails --------------------------------------------------------------- */

static int
cam_rail(unsigned i, bool on)
{
	int err = EBUSY;
	for (unsigned attempt = 0; attempt < 5 && err == EBUSY; ++attempt) {
		err = andromeda_rpmh_cam_rail(cam_rail_name[i], on);
		if (err == EBUSY) {
			cam_wait(100);   /* another sweet_rpmh user holds the lock or a slot */
		}
	}
	if (on && (err == 0 || err == ETIMEDOUT)) {
		cam.rails |= 1u << i;    /* on, or uncertain after a timeout */
	} else if (!on && err == 0) {
		cam.rails &= ~(1u << i);
	}
	return err;
}

/* ---- sequence ------------------------------------------------------------ */

static bool
cam_prepare(void)
{
	cam.north = andromeda_io_map(CAM_TLMM_NORTH_PA, CAM_TLMM_SZ);
	cam.east = andromeda_io_map(CAM_TLMM_EAST_PA, CAM_TLMM_SZ);
	cam.gcc = andromeda_io_map(CAM_GCC_PA, CAM_GCC_SZ);
	cam.cc = andromeda_io_map(CAM_CC_PA, CAM_CC_SZ);
	if (!cam.north || !cam.east || !cam.gcc || !cam.cc) {
		printf(CAM_TAG " MMIO mapping failed\n");
		cam.cc_dead = true;
		return false;
	}
	bool pins_free = true;
	for (unsigned p = 0; p < P_COUNT; ++p) {
		vm_offset_t t = cam_tile(p);
		unsigned g = cam_pin[p].gpio, func;
		cam.saved_cfg[p] = cam_rd(t, CAM_GPIO_CFG(g));
		cam.saved_io[p] = cam_rd(t, CAM_GPIO_IO(g));
		func = PIN_FUNC(cam.saved_cfg[p]);
		bool driven_high = (cam.saved_cfg[p] & PIN_OE) && (cam.saved_io[p] & PIN_OUT);
		bool ok = (func == 0 || (cam_pin[p].alt_func && func == cam_pin[p].alt_func)) &&
		    !(func == 0 && driven_high && p >= P_VANA);
		printf(CAM_TAG " GPIO%u %s cfg=%#x func=%u pull=%u oe=%u io=%#x%s\n", g, cam_pin[p].name,
		    cam.saved_cfg[p], func, cam.saved_cfg[p] & 3u, (cam.saved_cfg[p] >> 9) & 1u,
		    cam.saved_io[p], ok ? "" : " <- not as expected");
		pins_free &= ok;
	}
	if (!pins_free) {
		printf(CAM_TAG " camera pins are owned or already driven high; nothing written\n");
		return false;
	}
	uint32_t ahb = cam_rd(cam.gcc, GCC_CAMERA_AHB), xo = cam_rd(cam.gcc, GCC_CAMERA_XO);
	printf(CAM_TAG " GCC camera_ahb=%#x camera_xo=%#x\n", ahb, xo);
	if (!(xo & CBCR_EN)) {
		printf(CAM_TAG " GCC_CAMERA_XO_CLK not enabled: no camera XO; nothing written\n");
		cam.cc_dead = true;
		return false;
	}
	if (!cam_cc_ok("preconditions")) {
		return false;
	}
	vm_offset_t r = cam.cc + CC_MCLK0_RCG;
	cam.saved_rcg_cfg = cam_rd(r, RCG_CFG);
	uint32_t cbcr = cam_rd(cam.cc, CC_MCLK0_CBCR), gdscr = cam_rd(cam.cc, CC_TITAN_GDSCR);
	printf(CAM_TAG " camcc titan_top gdscr=%#x gdsc_clk=%#x mclk0 rcg cmd=%#x cfg=%#x cbcr=%#x\n",
	    gdscr, cam_rd(cam.cc, CC_GDSC_CLK), cam_rd(r, RCG_CMD), cam.saved_rcg_cfg, cbcr);
	if (cbcr & CBCR_EN) {
		printf(CAM_TAG " MCLK0 already enabled; another owner? nothing written\n");
		return false;
	}
	if ((gdscr & GDSC_HW_CONTROL) || (!(gdscr & GDSC_PWR_ON) && !(gdscr & GDSC_SW_COLLAPSE))) {
		printf(CAM_TAG " titan_top GDSC under hardware control or in transition; nothing written\n");
		return false;
	}
	return true;
}

static void
cam_rail_off(unsigned i)
{
	if (!(cam.rails & (1u << i))) {
		return;
	}
	int err = cam_rail(i, false);
	if (err) {
		printf(CAM_TAG " %s could not be voted off (err=%d): it stays on\n", cam_rail_name[i], err);
	}
	cam_wait(1);
}

static void
cam_gpio_off(unsigned p, bool *on)
{
	if (*on) {
		cam_out(p, false);
		*on = false;
		cam_wait(1);
	}
}

/* The module's powerDownSequence, for the steps that were done only. */
static void
cam_power_down(void)
{
	printf(CAM_TAG " power-down (module order): reset=%d vdig=%u vio=%d vana=%d custom1=%u mclk=%d\n",
	    cam.reset, (cam.rails >> RAIL_VDIG) & 1u, cam.vio, cam.vana,
	    (cam.rails >> RAIL_CUSTOM1) & 1u, cam.mclk_ours || cam.gdsc_ours);
	cam_i2c_release();
	cam_gpio_off(P_RESET, &cam.reset);                 /* RESET 0, 1 ms */
	cam_rail_off(RAIL_VDIG);                           /* VDIG, 1 ms */
	cam_gpio_off(P_VIO, &cam.vio);                     /* VIO, 1 ms */
	cam_gpio_off(P_VANA, &cam.vana);                   /* VANA: gpio-vana low, 1 ms */
	cam_rail_off(RAIL_CUSTOM1);                        /* CUSTOM_REG1, 1 ms */
	cam_mclk_off();                                    /* MCLK and cam_clk, 1 ms */
	cam_wait(1);
	if (cam.pins_set) {
		for (unsigned p = 0; p < P_COUNT; ++p) {
			if (p != P_SDA && p != P_SCL) {
				cam_pin_restore(p);
			}
		}
		cam.pins_set = false;
	}
	printf(CAM_TAG " down: rails=%#x (0 = voted off) pins 13=%#x 17=%#x 18=%#x 21=%#x 28=%#x 34=%#x\n",
	    cam.rails, cam_rd(cam.north, CAM_GPIO_CFG(13)), cam_rd(cam.north, CAM_GPIO_CFG(17)),
	    cam_rd(cam.north, CAM_GPIO_CFG(18)), cam_rd(cam.east, CAM_GPIO_CFG(21)),
	    cam_rd(cam.east, CAM_GPIO_CFG(28)), cam_rd(cam.north, CAM_GPIO_CFG(34)));
}

static const char *
cam_sequence(void)
{
	if (!cam_prepare()) {
		return "preconditions";
	}
	if (!cam_pins_default()) {
		return "pin set-up";
	}
	int err = cam_rail(RAIL_CUSTOM1, true);            /* CUSTOM_REG1, 1 ms */
	if (err) {
		printf(CAM_TAG " ldoa17 on failed err=%d\n", err);
		return "cam_v_custom1 (ldoa17) vote";
	}
	cam_wait(1);
	cam_out(P_VANA, true);                             /* VANA: gpio-vana high, 1 ms */
	cam.vana = true;
	cam_wait(1);
	cam_out(P_VIO, true);                              /* VIO (ramp 135 us), 1 ms */
	cam.vio = true;
	cam_wait(1);
	err = cam_rail(RAIL_VDIG, true);                   /* VDIG, 1 ms */
	if (err) {
		printf(CAM_TAG " smpc8 on failed err=%d\n", err);
		return "cam_vdig (smpc8) vote";
	}
	cam_wait(1);
	cam_out(P_RESET, true);                            /* RESET 1, 12 ms */
	cam.reset = true;
	printf(CAM_TAG " custom1 ldoa17 2856 mV on; vana gpio21 1; vio gpio34 1; vdig smpc8 1352 mV on;"
	    " reset gpio28 1 (io %#x/%#x/%#x)\n", cam_rd(cam.east, CAM_GPIO_IO(21)),
	    cam_rd(cam.north, CAM_GPIO_IO(34)), cam_rd(cam.east, CAM_GPIO_IO(28)));
	cam_wait(12);
	if (!cam_mclk_on()) {                              /* MCLK 19.2 MHz, 10 ms */
		return "MCLK0";
	}
	cam_wait(10);
	if (!cam_i2c_claim()) {
		return "GPIO I2C pins";
	}
	/* One read: register pointer 0x0016 (16-bit), repeated START, two bytes. */
	const uint8_t reg[2] = {(uint8_t)(IMX363_ID_REG >> 8), (uint8_t)IMX363_ID_REG};
	uint8_t id[2] = {0, 0};
	err = cepheus_bb_xfer(&cam_bb, cam.addr, reg, sizeof(reg), id, sizeof(id));
	cam_i2c_release();
	if (err) {
		printf(CAM_TAG " imx363 reg 0x%04x addr 0x%02x: err=%d, %s\n", IMX363_ID_REG, cam.addr, err,
		    cam_i2c_error(err));
		return "I2C read";
	}
	unsigned value = ((unsigned)id[0] << 8) | id[1];
	printf(CAM_TAG " imx363 reg 0x%04x = 0x%04x addr 0x%02x %s\n", IMX363_ID_REG, value, cam.addr,
	    value == IMX363_ID ? "OK" : "UNEXPECTED (want 0x0363)");
	return value == IMX363_ID ? NULL : "chip ID mismatch";
}

static void
cam_run(void)
{
	printf(CAM_TAG " c1 start sensor 0 imx363 cci0.m0 addr 0x%02x (GPIO I2C)\n", cam.addr);
	const char *failed = cam_sequence();
	if (failed) {
		printf(CAM_TAG " C1 FAIL at: %s\n", failed);
	} else {
		printf(CAM_TAG " C1 PASS: IMX363 chip ID 0x0363 at 0x%02x\n", cam.addr);
	}
	cam_power_down();
	printf(CAM_TAG " done\n");
}

static void
cam_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	printf(CAM_TAG " C1 starting in %u s (andrcam=1, addr 0x%02x)\n", cam_delay_s, cam.addr);
	cam_wait(cam_delay_s * 1000u);
	cam_run();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
andromeda_cam_init(void)
{
	static bool started;
	int level = 0;
	unsigned addr = IMX363_ADDR;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrcam", &level, sizeof(level));
	if (level != 1) {
		if (level) {
			printf(CAM_TAG " andrcam=%d not supported (only 1)\n", level);
		}
		return;
	}
	(void)PE_parse_boot_argn("andrcamdelay", &cam_delay_s, sizeof(cam_delay_s));
	(void)PE_parse_boot_argn("andrcamaddr", &addr, sizeof(addr));
	if (addr != IMX363_ADDR && addr != IMX363_ALT_ADDR) {
		printf(CAM_TAG " andrcamaddr=%#x refused (only 0x10 or 0x1a)\n", addr);
		return;
	}
	cam.addr = addr;
	if (cam_delay_s > 3600u) {
		cam_delay_s = 3600u;
	}
	if (kernel_thread_start(cam_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf(CAM_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
