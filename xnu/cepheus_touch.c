/*
 * CEPHEUS (Xiaomi Mi 9, SM8150) SE17 FIFO I2C / ST-FTS ("st,fts", addr 0x49).
 * Forked from ios-sweet's sweet_touch.c (SE7 / Goodix GT9896).
 *
 * Every hardware number below comes from THIS phone:
 *   - live device tree (adb root; /sys/firmware/fdt, 646932 bytes, 2026-10-31)
 *   - the LineageOS kernel source the phone actually runs (~/android):
 *       drivers/input/touchscreen/xiaomi/fts_521/   ST-FTS protocol
 *       drivers/clk/qcom/gcc-sm8150.c               GCC SE17 clock registers
 *
 * Live FDT facts:
 *   qupv3_se17_i2c: i2c@0xc80000   (interrupts = <0 0x175 0>)
 *     clocks = <&gcc GCC_QUPV3_WRAP2_S0_CLK>   -> global SE17
 *              <&gcc GCC_QUPV3_WRAP_2_M_AHB_CLK>
 *              <&gcc GCC_QUPV3_WRAP_2_S_AHB_CLK>
 *     pinctrl SDA/SCL = gpio55 / gpio56, function "qup17", 2 mA, bias-disable
 *     fts@49:
 *       compatible     = "st,fts", reg = <0x49>
 *       fts,irq-gpio   = <&tlmm 122 0x2008>   IRQF_ONESHOT | IRQF_TRIGGER_LOW
 *       fts,reset-gpio = <&tlmm 12 0>         ACTIVE LOW (0 = reset)
 *       vdd-supply     = <&touch_vddio_vreg>  regulator-fixed, gpio 84, 1.8 V,
 *                                             regulator-boot-on
 *       avdd-supply    = <&pm8150l_l1>        L1C, 1.8 V, RPMh
 *       fts,x-max = 1080, fts,y-max = 2340
 *
 * GCC registers (gcc-sm8150.c; wrap2 local S0 == global SE17):
 *   RCG   0x1e148   cmd_rcgr, hid_width 5, parent_map_0 (0=bi_tcxo 1=gpll0)
 *   HALT  0x1e144   bit 31 = branch halt
 *   VOTE  0x52014   bit 4  = SE17 branch enable (BRANCH_HALT_VOTED)
 *
 * ST-FTS protocol (fts_521/fts_lib/ftsHardware.h and fts.c):
 *   chip id : 2 bytes at hw register 0x20000000 -> DCHIP_ID_0 = 0x36
 *   FIFO    : opcode 0x86 (FIFO_CMD_READALL), 8-byte events
 *             event[0] = event id: 0x13 enter, 0x23 motion, 0x33 leave
 *             touchId = (event[1] & 0xF0) >> 4, touchType = event[1] & 0x0F
 *             x = ((event[3] & 0x0F) << 8) | event[2]
 *             y = (event[4] << 4) | ((event[3] & 0xF0) >> 4)
 *             event[7] & 0x1F = events still queued
 *
 * Call once after the scheduler is running; all work runs on one thread.
 * Requires ABL-loaded GENI I2C firmware. No firmware/config upload and no
 * direct PMIC writes: IC commands read FIFO/chip ID and enable active touch
 * scan. AVDD L1C is inherited from firmware, not the sweet L11C vote.
 * Reset-low pulse is opt-in (sweettouchreset=1); by default preserve boot
 * firmware's controller state, since firmware loading is not implemented.
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
#include "cepheus_fts_events.h"
#include "cepheus_clock_status.h"
#include "cepheus_gpi_layout.h"
#include "cepheus_i2c_bitbang.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern int cepheus_touch_power_enable(void);
extern void cepheus_soc_init(void);
extern void andromeda_probe_init(void); /* andromeda_probe.c, andrprobe=1 */
extern void andromeda_cpufreq_init(void); /* andromeda_cpufreq.c, andrcpu=1 */
extern void cepheus_cpu_activity(void);
void sweet_touch_init(void);

#define TOUCH_SE_PA      0x00c80000u  /* qupv3_se17_i2c: i2c@0xc80000 */
#define TOUCH_GCC_PA     0x00100000u
#define TOUCH_NORTH_PA   0x03900000u /* TLMM 0x03000000 + NORTH 0x00900000 */
#define TOUCH_SOUTH_PA   0x03d00000u /* SDA/SCL/IRQ: TLMM + SOUTH 0x00d00000 */
#define TOUCH_GCC_VOTE   0x52014u   /* wrap2 branch vote (0x5200c was wrap1) */
#define TOUCH_RCG        0x1e148u   /* wrap2 LOCAL S0 = global SE17 */
#define TOUCH_RCG_HALT   0x1e144u   /* wrap2 LOCAL S0 branch halt */
#define TOUCH_VOTE_BIT   (1u << 4)  /* SE17 branch enable in TOUCH_GCC_VOTE */
#define TOUCH_AHB_BITS   ((1u << 2) | (1u << 1)) /* wrap2 M/S AHB */
#define TOUCH_GPIO_CFG(n) ((uint32_t)(n) * 0x1000u)
#define TOUCH_GPIO_IO(n)  (TOUCH_GPIO_CFG(n) + 4u)
#define TOUCH_MAX        10u

/* Xiaomi Mi 9 pins (live FDT). Reset is ACTIVE LOW: 0 asserts, 1 releases.
 * The Mi Mix 3 5G (andromeda) has the same controller on the same SE17 pins
 * and IRQ, but reset on GPIO54 and the 1.8 V regulator enable on GPIO37
 * (live FDT fts@49: fts,reset-gpio <&tlmm 54>, touch_vddio_vreg gpio 37).
 * Boot-args sweettouchrst= and sweettouchvddio= select them; only the pins
 * of these two phones are accepted, so a wrong value cannot drive a random pin.
 * The GPIO37 regulator is named disp_vddio_vreg there: it is only ever
 * driven high, never low. */
#define TOUCH_PIN_SDA    55u
#define TOUCH_PIN_SCL    56u
#define TOUCH_PIN_RESET  touch_pin_reset
#define TOUCH_PIN_IRQ    122u
#define TOUCH_PIN_VDDIO  touch_pin_vddio  /* touch_vddio_vreg enable, active high */
static unsigned touch_pin_reset = 12u;
static unsigned touch_pin_vddio = 84u;
/* sweettouchraw=N: print the first N non-empty FIFO events as raw bytes and a
 * poll counter now and then, to see what the controller reports when the
 * parser recognises nothing (andromeda bring-up). 0 (default) = silent. */
static unsigned touch_raw_log;
static unsigned touch_raw_logged;
static unsigned touch_raw_polls;
static unsigned touch_raw_nonempty;
/* sweettouchscan=N: settings byte of the ST-FTS "scan mode active" command
 * (Xiaomi fts_521 Sense ON uses 0x01 = multi-touch). Default 0x01. */
static unsigned touch_scan_settings = 0x01u;
static unsigned touch_rescans;
#define FTS_EVT_CONTROLLER_READY 0x03u
#define TOUCH_I2C_ADDR   0x49u
#define TOUCH_X_MAX      1080
#define TOUCH_Y_MAX      2340

/* ST-FTS opcodes and addresses (fts_lib/ftsHardware.h, I2C interface). */
#define FTS_CMD_READ_ALL_EVENTS 0x86u
#define FTS_CMD_HW_REG_R        0xFAu
#define FTS_ADDR_DCHIP_ID       0x20000000u  /* chip id, low byte first */
#define FTS_DCHIP_ID_0          0x36u        /* LSB of an FTM5 ST-FTS part */
#define FTS_FIFO_EVENT_SIZE     8u
#define FTS_FIFO_DEPTH          32u

#define SE_STATUS       0x040u
#define SE_MCLK         0x048u
#define SE_FW           0x068u
#define SE_CLK_SEL      0x07cu
#define SE_DMA          0x258u
#define SE_TX_LEN       0x26cu
#define SE_RX_LEN       0x270u
#define SE_SCL          0x278u
#define SE_CMD          0x600u
#define SE_CTRL         0x604u
#define SE_IRQ          0x610u
#define SE_IRQ_EN       0x614u
#define SE_IRQ_CLR      0x618u
#define SE_TX           0x700u
#define SE_RX           0x780u
#define SE_TX_COUNT     0x800u
#define SE_RX_COUNT     0x804u
#define SE_HW_TX        0xe24u
#define SE_HW_RX        0xe28u
#define SE_TX_WM        0x80cu
#define SE_PACK0        0x0007f8feu /* Linux geni_se_config_packing(8,4,true,...) */
#define SE_PACK1        0x000ffefeu
#define SE_DONE         (1u << 0)
#define SE_NACK         (1u << 10)
#define SE_ERRORS       (0x0eu | (1u << 9) | (1u << 10) | (1u << 12) | \
                         (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS       (SE_DONE | SE_ERRORS | (1u << 5) | (1u << 26) | \
                         (1u << 27) | (1u << 30))

static struct {
	vm_offset_t se, gcc, gpio, south;
	bool started, dead;
	int irq_hint;
	int gpio_i2c;
	bool pins_claimed;
	uint32_t saved_cfg[2], saved_io[2], bb_cfg[2];
	uint32_t irq_status, active;
	int x[TOUCH_MAX], y[TOUCH_MAX];
} st;
static int touch_sleep_chan;
static struct cepheus_bb touch_bb;

static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
touch_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
touch_bb_drive(void *ctx, unsigned line, int low)
{
	(void)ctx;
	unsigned pin = line == BB_SDA ? TOUCH_PIN_SDA : TOUCH_PIN_SCL;
	/* Latch stays zero. Released = input; low = output enabled. */
	touch_wr(st.south, TOUCH_GPIO_CFG(pin), st.bb_cfg[line] | (low ? 0x200u : 0));
}
static int touch_bb_sample(void *ctx, unsigned line)
{
	(void)ctx;
	return (touch_rd(st.south, TOUCH_GPIO_IO(line == BB_SDA ? TOUCH_PIN_SDA : TOUCH_PIN_SCL)) & 1u) != 0;
}
static void touch_bb_wait(void *ctx, unsigned us) { (void)ctx; delay(us); }

static bool
touch_bb_init(void)
{
	for (unsigned line = 0; line < 2; ++line) {
		unsigned pin = line == BB_SDA ? TOUCH_PIN_SDA : TOUCH_PIN_SCL;
		st.saved_cfg[line] = touch_rd(st.south, TOUCH_GPIO_CFG(pin));
		st.saved_io[line] = touch_rd(st.south, TOUCH_GPIO_IO(pin));
		st.bb_cfg[line] = (st.saved_cfg[line] & ~0x3ffu) | 3u; /* gpio, pull-up, 2mA, input */
		if (st.bb_cfg[line] & (1u << 11)) { st.bb_cfg[line] |= 1u << 12; }
		touch_wr(st.south, TOUCH_GPIO_CFG(pin), st.bb_cfg[line]);
		touch_wr(st.south, TOUCH_GPIO_IO(pin), st.saved_io[line] & ~2u);
	}
	st.pins_claimed = true;
	touch_bb = (struct cepheus_bb){NULL, touch_bb_drive, touch_bb_sample, touch_bb_wait};
	for (unsigned line = 0; line < 2; ++line) {
		unsigned pin = line == BB_SDA ? TOUCH_PIN_SDA : TOUCH_PIN_SCL;
		uint32_t cfg = touch_rd(st.south, TOUCH_GPIO_CFG(pin));
		printf("SWEET TOUCH (cepheus): GPIO%u cfg=%#x want=%#x io=%#x\n",
		    pin, cfg, st.bb_cfg[line], touch_rd(st.south, TOUCH_GPIO_IO(pin)));
		if (cfg != st.bb_cfg[line]) { return false; }
	}
	printf("SWEET TOUCH (cepheus): GPIO open-drain I2C SDA=%d SCL=%d; no GPI/SMMU writes\n",
	    touch_bb_sample(NULL, BB_SDA), touch_bb_sample(NULL, BB_SCL));
	return true;
}

static void
touch_bb_restore(void)
{
	if (!st.pins_claimed) { return; }
	bb_release(&touch_bb);
	for (unsigned line = 0; line < 2; ++line) {
		unsigned pin = line == BB_SDA ? TOUCH_PIN_SDA : TOUCH_PIN_SCL;
		touch_wr(st.south, TOUCH_GPIO_IO(pin), st.saved_io[line]);
		touch_wr(st.south, TOUCH_GPIO_CFG(pin), st.saved_cfg[line]);
	}
	st.pins_claimed = false;
}

static bool
touch_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(touch_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* SM8150 TLMM tile of each pin this driver may touch (Linux
 * drivers/pinctrl/qcom/pinctrl-sm8150.c): 54, 55, 56 and 122 are SOUTH;
 * 12, 37 and 84 are NORTH. */
static vm_offset_t
touch_tile(unsigned pin)
{
	switch (pin) {
	case 54u: case 55u: case 56u: case 122u:
		return st.south;
	default:
		return st.gpio;
	}
}

static void
touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out,
    bool high)
{
	uint32_t off = TOUCH_GPIO_CFG(pin);
	vm_offset_t base = touch_tile(pin);
	uint32_t cfg = touch_rd(base, off);
	if (out) {
		/* Set the latch before enabling output; reset is active low. */
		touch_wr(base, TOUCH_GPIO_IO(pin),
		    (touch_rd(base, TOUCH_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	if (cfg & (1u << 11)) { cfg |= 1u << 12; }
	touch_wr(base, off, cfg);
}

static bool
touch_branch(unsigned off, bool has_hwcg)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < 5000; ++i) {
		value = touch_rd(st.gcc, off);
		if (cepheus_branch_enabled(value, has_hwcg)) {
			printf("SWEET TOUCH (cepheus): branch %#x CBCR=%#x %s\n", off, value,
			    has_hwcg && (value & 2u) ? "hardware-gated" : "on");
			return true;
		}
		delay(1);
	}
	printf("SWEET TOUCH (cepheus): branch %#x CBCR=%#x still halted; stopping\n", off, value);
	return false;
}

static bool
touch_clocks(void)
{
	/*
	 * Vote the SE17 branch and wrapper2 AHB branches, preserving other users.
	 * gcc_sm8150: SE17 bit 4, M_AHB bit 2, S_AHB bit 1 at 0x52014.
	 */
	touch_wr(st.gcc, TOUCH_GCC_VOTE,
	    touch_rd(st.gcc, TOUCH_GCC_VOTE) | TOUCH_VOTE_BIT | TOUCH_AHB_BITS);
	uint32_t dfs = touch_rd(st.gcc, TOUCH_RCG + 0x14u);
	uint32_t cfg = touch_rd(st.gcc, TOUCH_RCG + 4u);
	printf("SWEET TOUCH (cepheus): gcc vote=%#x rcg=%#x cfg=%#x dfs=%#x\n",
	    touch_rd(st.gcc, TOUCH_GCC_VOTE), touch_rd(st.gcc, TOUCH_RCG), cfg, dfs);
	if (dfs & 1u) {
		/* SE CLK_SEL=0 selects DFS performance level 0. Do not alter tables. */
		cfg = touch_rd(st.gcc, TOUCH_RCG + 0x1cu);
		if ((cfg & 0x371fu) != 1u) {
			printf("SWEET TOUCH (cepheus): DFS level0=%#x is not XO/1; need clock integration\n", cfg);
			return false;
		}
	} else {
		/* XO parent 0, HID divider 1 => 19.2 MHz. No PLL or voltage change. */
		cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
		touch_wr(st.gcc, TOUCH_RCG + 4u, cfg);
		touch_wr(st.gcc, TOUCH_RCG, touch_rd(st.gcc, TOUCH_RCG) | 1u);
		if (!touch_wait_clear(st.gcc, TOUCH_RCG, 1u)) {
			printf("SWEET TOUCH (cepheus): RCG update timeout\n");
			return false;
		}
	}
	/* Mirrors clk_branch_wait()/clk_branch2_check_halt() in SM8150 Linux:
	 * skip CLK_OFF for S_AHB when its hardware-gate bit is set; otherwise
	 * accept clock-on or NOC FSM ON. Do not treat an idle gated branch as
	 * an enable failure, or print SE17's value for an AHB failure.
	 */
	if (!touch_branch(0x1e004, false) || !touch_branch(0x1e008, true) ||
	    !touch_branch(TOUCH_RCG_HALT, false)) {
		return false;
	}
	return true;
}

/* Called only for a transfer started by this driver, including a stretched TX. */
static void
touch_abort(void)
{
	touch_wr(st.se, SE_CTRL, 2); /* abort M command, release held bus */
	bool done = false;
	for (unsigned i = 0; i < 5000; ++i) {
		if (touch_rd(st.se, SE_IRQ) & (1u << 5)) {
			done = true;
			break;
		}
		delay(1);
	}
	if (!done || !touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		printf("SWEET TOUCH (cepheus): abort failed status=%#x irq=%#x; stopping\n",
		    touch_rd(st.se, SE_STATUS), touch_rd(st.se, SE_IRQ));
	}
	/* Bound drain even if the hardware reports an impossible FIFO count. */
	for (unsigned i = 0; i < 64 && (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)touch_rd(st.se, SE_RX);
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	if (touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) {
		st.dead = true;
		printf("SWEET TOUCH (cepheus): TX FIFO not empty after abort; stopping\n");
	}
}

/* One Linux-style FIFO message. TX at most 5 bytes; RX at most 256.
 * Pack at most four bytes per word (a 5-byte hardware read spans two words).
 */
static int
touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	if (st.gpio_i2c) {
		if (stretch) { return EINVAL; } /* use a combined GPIO transfer */
		return cepheus_bb_xfer(&touch_bb, addr, tx, nt, rx, nr);
	}
	if (st.dead || nt > 5 || nr > FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH ||
	    (nt && nr) || (!nt && !nr) || (nt && !tx) || (nr && !rx)) {
		return EINVAL;
	}
	if (!touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true; /* no other SE17 owner is supported */
		return EBUSY;
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, SE_TX_WM, nt ? 1u : 0u);
	touch_wr(st.se, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	touch_wr(st.se, SE_CMD, ((nr ? 2u : 1u) << 27) |
	    ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	unsigned sent = 0, got = 0;
	int err = ETIMEDOUT;
	for (unsigned i = 0; i < 20000; ++i) {
		uint32_t irq = touch_rd(st.se, SE_IRQ);
		st.irq_status = irq;
		if (irq & SE_ERRORS) {
			err = (irq & SE_NACK) ? ENXIO : EIO;
			/* A completed NACK is normal during address probing. Aborting an
			 * idle sequencer may never produce ABORT_DONE. Wait for completion
			 * and leave a clean idle engine available for the next transfer. */
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); ++j) {
				delay(1);
				irq = touch_rd(st.se, SE_IRQ);
			}
			if ((irq & SE_DONE) && !(touch_rd(st.se, SE_STATUS) & 1u) &&
			    !(touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) &&
			    !(touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
				touch_wr(st.se, SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (sent < nt && (irq & (1u << 30))) {
			uint32_t word = 0;
			for (unsigned j = 0; j < 4 && sent < nt; ++j) {
				word |= (uint32_t)tx[sent++] << (8u * j);
			}
			touch_wr(st.se, SE_TX, word);
			if (sent == nt) {
				touch_wr(st.se, SE_TX_WM, 0);
			}
		}
		uint32_t count = touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu;
		if (count > 64) {
			err = EIO;
			break;
		}
		while (count--) {
			uint32_t word = touch_rd(st.se, SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; ++j) {
				rx[got++] = (uint8_t)(word >> (8u * j));
			}
		}
		/* DONE is checked after draining RX, so final short words aren't lost. */
		if (irq & SE_DONE) {
			touch_wr(st.se, SE_IRQ_CLR, irq);
			if (got == nr && sent == nt) {
				return 0;
			}
			err = EIO;
			break;
		}
		/* Preserve DONE/errors if they arrive during this iteration. */
		touch_wr(st.se, SE_IRQ_CLR, irq & ((1u << 26) | (1u << 27) | (1u << 30)));
		delay(1);
	}
	touch_abort();
	return err;
}

/*
 * ST-FTS "writeReadU8UX": write an opcode byte, repeated START, read N bytes.
 * fts_writeReadU8UX() in fts_lib/ftsIO.c does exactly this for addrSize 0.
 */
static int
fts_read_cmd(uint8_t cmd, uint8_t *buf, unsigned len)
{
	uint8_t op = cmd;
	if (st.gpio_i2c) { return cepheus_bb_xfer(&touch_bb, TOUCH_I2C_ADDR, &op, 1, buf, len); }
	int err = touch_msg(TOUCH_I2C_ADDR, &op, 1, NULL, 0, true);
	if (err) {
		return err;
	}
	return touch_msg(TOUCH_I2C_ADDR, NULL, 0, buf, len, false);
}

/* Read an ST-FTS hardware register (fts_writeReadU8UX with a 4-byte address). */
static int
fts_read_hw(uint32_t address, uint8_t *buf, unsigned len)
{
	uint8_t cmd[5] = {
		FTS_CMD_HW_REG_R,
		(uint8_t)(address >> 24), (uint8_t)(address >> 16),
		(uint8_t)(address >> 8), (uint8_t)address
	};
	if (st.gpio_i2c) { return cepheus_bb_xfer(&touch_bb, TOUCH_I2C_ADDR, cmd, sizeof(cmd), buf, len); }
	if (len > FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH) {
		return EINVAL;
	}
	int err = touch_msg(TOUCH_I2C_ADDR, cmd, sizeof(cmd), NULL, 0, true);
	if (err) {
		return err;
	}
	return touch_msg(TOUCH_I2C_ADDR, NULL, 0, buf, len, false);
}

static void
touch_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; ++id) {
		uint32_t bit = 1u << id;
		if (mask & bit) {
			if (!(st.active & bit) || x[id] != st.x[id] || y[id] != st.y[id]) {
				cepheus_cpu_activity();
				/* Bound diagnostics so finger motion cannot flood the log. */
				static unsigned logged;
				if (logged++ < 40) {
					printf("SWEET TOUCH (cepheus): contact %u x=%d y=%d\n", id, x[id], y[id]);
				}
				/* SweetHID's physical input canvas is 1080x2400. Map our
				 * 2340-row sensor to it, including the bottom edge gesture.
				 */
				int hx = x[id] >= 1080 ? 1079 : x[id];
				int hy = y[id] >= 2340 ? 2399 : (int)((int64_t)y[id] * 2399 / 2339);
				sweet_hid_touch((int)id, 1, hx, hy);
			}
			st.x[id] = x[id];
			st.y[id] = y[id];
		} else if (st.active & bit) {
			int hx = st.x[id] >= 1080 ? 1079 : st.x[id];
			int hy = st.y[id] >= 2340 ? 2399 : (int)((int64_t)st.y[id] * 2399 / 2339);
			sweet_hid_touch((int)id, 0, hx, hy);
		}
	}
	st.active = mask;
}

/* sweettouchraw: print one FIFO event, up to the configured count. */
static void
touch_raw_event(const char *why, const uint8_t *e)
{
	if (!touch_raw_log || touch_raw_logged >= touch_raw_log) { return; }
	touch_raw_logged++;
	printf("SWEET TOUCH (cepheus): %s %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    why, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
}

/* Xiaomi fts_521 Sense ON: setScanMode(SCAN_MODE_ACTIVE, 0x01) = A0 00 01. */
static int
touch_scan_on(const char *why)
{
	const uint8_t scan[] = {0xa0, 0x00, (uint8_t)touch_scan_settings};
	int err = touch_msg(TOUCH_I2C_ADDR, scan, sizeof(scan), NULL, 0, false);
	printf("SWEET TOUCH (cepheus): scan on (%s) settings=%#x err=%d\n",
	    why, touch_scan_settings, err);
	return err;
}

/*
 * Pop FIFO events one at a time for up to ms milliseconds, logging each.
 * Returns true once an event with id `want` was seen (0: never stop early).
 */
static bool
touch_drain_log(const char *why, unsigned ms, unsigned want)
{
	uint8_t e[FTS_FIFO_EVENT_SIZE];
	unsigned events = 0;
	for (unsigned t = 0; t < ms;) {
		if (fts_read_cmd(FTS_CMD_READ_ALL_EVENTS, e, sizeof(e)) != 0) { return false; }
		if (e[0] != 0) {
			if (++events > 2u * FTS_FIFO_DEPTH) { return false; } /* never spin */
			printf("SWEET TOUCH (cepheus): %s +%ums %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    why, t, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
			if (want && e[0] == want) { return true; }
			continue; /* more may be queued: read again at once */
		}
		touch_wait(10);
		t += 10;
	}
	return false;
}

/*
 * Drain the ST-FTS event FIFO. Returns 0 on success, or an errno. A single
 * read may carry up to FIFO_DEPTH events; event[7] bits 0..4 say how many
 * more follow, so drain until that field is zero.
 */
static int
touch_fts(void)
{
	uint8_t buf[FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH];
	struct cepheus_fts_state frame = {.active = st.active};
	memcpy(frame.x, st.x, sizeof(frame.x));
	memcpy(frame.y, st.y, sizeof(frame.y));
	int err = fts_read_cmd(FTS_CMD_READ_ALL_EVENTS, buf, FTS_FIFO_EVENT_SIZE);
	if (err) { return err; }
	if (touch_raw_log) {
		touch_raw_polls++;
		if (buf[0] != 0) {
			touch_raw_nonempty++;
		}
		if ((touch_raw_polls % 3000u) == 0) {
			printf("SWEET TOUCH (cepheus): polls=%u nonempty=%u irq-pin=%u\n",
			    touch_raw_polls, touch_raw_nonempty,
			    touch_rd(st.south, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)) & 1u);
		}
	}
	unsigned remaining = buf[7] & 0x1fu;
	/* Preserve the first popped event; read only the remaining events into
	 * buf+8, matching Linux fts_event_handler. Do not overwrite the enter.
	 */
	if (remaining) {
		err = fts_read_cmd(FTS_CMD_READ_ALL_EVENTS, buf + FTS_FIFO_EVENT_SIZE,
		    remaining * FTS_FIFO_EVENT_SIZE);
		if (err) { return err; }
	}
	bool rescan = false;
	for (unsigned i = 0; i <= remaining; ++i) {
		const uint8_t *e = buf + i * FTS_FIFO_EVENT_SIZE;
		if (e[0] == 0) { break; }
		touch_raw_event("raw", e);
		/* Controller ready = the IC rebooted (ESD, watchdog, late boot) and
		 * is back in its default sense-off state: switch scanning on again. */
		if (e[0] == FTS_EVT_CONTROLLER_READY) { rescan = true; }
		if (cepheus_fts_event(&frame, e)) { return EIO; }
	}
	touch_report(frame.active, frame.x, frame.y);
	if (rescan && touch_rescans < 5) {
		touch_rescans++;
		(void)touch_scan_on("controller ready while polling");
	}
	return 0;
}

static bool
touch_smmu_probe(void)
{
	/* Only GR0 identification and matching descriptors, never a reset, map
	 * update, TLB invalidation, or address-translation command. */
	printf("SWEET GPI: reading apps SMMU GR0 for stream %#x\n", CEPHEUS_GPI_SID);
	vm_offset_t smmu = ml_io_map(CEPHEUS_SMMU_PA, 0x1000);
	if (!smmu) { return false; }
	uint32_t cr0 = touch_rd(smmu, 0), id0 = touch_rd(smmu, 0x20);
	uint32_t id1 = touch_rd(smmu, 0x24), id2 = touch_rd(smmu, 0x28);
	unsigned groups = id0 & 0xffu, matches = 0;
	printf("SWEET GPI: SMMU cr0=%#x id0=%#x id1=%#x id2=%#x groups=%u\n",
	    cr0, id0, id1, id2, groups);
	if (id0 == 0 || id0 == 0xffffffffu || !(id0 & (1u << 27)) || (cr0 & 8u)) {
		printf("SWEET GPI: unsupported SMMU ID/match mode; DMA domain unknown\n");
		return false;
	}
	for (unsigned i = 0; i < groups; ++i) {
		uint32_t smr = touch_rd(smmu, 0x800u + i * 4u);
		if (!cepheus_smr_matches(smr, CEPHEUS_GPI_SID)) { continue; }
		uint32_t s2cr = touch_rd(smmu, 0xc00u + i * 4u);
		++matches;
		printf("SWEET GPI: SID=%#x match=%u SMR=%#x S2CR=%#x type=%u cb=%u\n",
		    CEPHEUS_GPI_SID, i, smr, s2cr, (s2cr >> 16) & 3u, s2cr & 0xffu);
	}
	printf("SWEET GPI: SMMU matches=%u clientpd=%u unmatched_fault=%u; no mapping changed\n",
	    matches, cr0 & 1u, (cr0 >> 10) & 1u);
	return true;
}

static bool
touch_gpi_probe(void)
{
	/* GPI DMA needs channel ownership, event ring state and a validated DMA
	 * address domain before any allocation/doorbell. Observe only contexts
	 * assigned to Linux by the live FDT mask. No GPI MMIO stores are issued.
	 */
	vm_offset_t gpi = ml_io_map(CEPHEUS_GPI2_PA, CEPHEUS_GPI2_SIZE);
	if (!gpi) {
		printf("SWEET GPI: read-only GPI2 mapping failed\n");
		return false;
	}
	printf("SWEET GPI: GPI2 %#x mask=%#x read-only context probe; DMA not started\n",
	    CEPHEUS_GPI2_PA, CEPHEUS_GPII_MASK);
	for (unsigned n = 0; n < CEPHEUS_GPII_MAX; ++n) {
		if (!cepheus_gpii_allowed(n)) { continue; }
		uint32_t event = cepheus_gpi_event_context(n);
		if (!cepheus_gpi_offset_valid(event + 0x34u)) { return false; }
		uint32_t ec = touch_rd(gpi, event);
		printf("SWEET GPI: i%u event ctx=%#x state=%u len=%#x base=%#x:%08x rp=%#x:%08x wp=%#x:%08x\n",
		    n, ec, (ec >> 20) & 15u, touch_rd(gpi, event + 4),
		    touch_rd(gpi, event + 12), touch_rd(gpi, event + 8),
		    touch_rd(gpi, event + 20), touch_rd(gpi, event + 16),
		    touch_rd(gpi, event + 28), touch_rd(gpi, event + 24));
		for (unsigned ch = 0; ch < 2; ++ch) {
			uint32_t ctx = cepheus_gpi_channel_context(n, ch);
			if (!cepheus_gpi_offset_valid(ctx + 0x6cu)) { return false; }
			uint32_t cc = touch_rd(gpi, ctx), scratch = touch_rd(gpi, ctx + 0x60u);
			printf("SWEET GPI: i%u ch%u ctx=%#x state=%u len=%#x base=%#x:%08x rp=%#x:%08x wp=%#x:%08x scratch=%#x seid=%u proto=%u\n",
			    n, ch, cc, (cc >> 20) & 15u, touch_rd(gpi, ctx + 4),
			    touch_rd(gpi, ctx + 12), touch_rd(gpi, ctx + 8),
			    touch_rd(gpi, ctx + 20), touch_rd(gpi, ctx + 16),
			    touch_rd(gpi, ctx + 28), touch_rd(gpi, ctx + 24),
			    scratch, scratch & 15u, (scratch >> 4) & 15u);
		}
	}
	printf("SWEET GPI: probe complete; channel allocation and SMMU DMA domain not verified\n");
	(void)touch_smmu_probe();
	return true;
}

static bool
touch_se_init(void)
{
	uint32_t fw = touch_rd(st.se, SE_FW);
	uint32_t tx = touch_rd(st.se, SE_HW_TX), rx = touch_rd(st.se, SE_HW_RX);
	uint32_t fifo_disabled = touch_rd(st.se, 0x64);
	printf("SWEET TOUCH (cepheus): se17 fw=%#x proto=%u status=%#x tx=%#x rx=%#x ios=%#x\n",
	    fw, (fw >> 8) & 0xffu, touch_rd(st.se, SE_STATUS), tx, rx,
	    touch_rd(st.se, 0x908));
	printf("SWEET TOUCH (cepheus): FIFO_DISABLE=%#x TX width=%u depth=%u RX width=%u depth=%u\n",
	    fifo_disabled, (tx >> 24) & 0x3fu, (tx >> 16) & 0x3fu,
	    (rx >> 24) & 0x3fu, (rx >> 16) & 0x3fu);
	if (st.gpio_i2c) {
		if ((touch_rd(st.se, SE_STATUS) & 0x1001u) || ((fw >> 8) & 0xffu) != 3u) {
			printf("SWEET TOUCH (cepheus): GENI busy or wrong protocol; GPIO ownership refused\n");
			return false;
		}
		printf("SWEET TOUCH (cepheus): GPIO transport selected; GENI FIFO/GPI untouched\n");
		return true;
	}
	if (fifo_disabled) {
		/* Linux geni_i2c_runtime_resume selects GSI_ONLY when this read-only
		 * register is nonzero. FIFO capability registers still report width
		 * and depth in that mode; they are not permission to access FIFO.
		 */
		printf("SWEET TOUCH (cepheus): SE17 is GSI-only; FIFO driver cannot probe; stopping\n");
		printf("SWEET TOUCH (cepheus): inherited pins SDA=%#x/%#x SCL=%#x/%#x RESET=%#x/%#x IRQ=%#x/%#x VDDIO=%#x/%#x\n",
		    touch_rd(st.south, TOUCH_GPIO_CFG(55)), touch_rd(st.south, TOUCH_GPIO_IO(55)),
		    touch_rd(st.south, TOUCH_GPIO_CFG(56)), touch_rd(st.south, TOUCH_GPIO_IO(56)),
		    touch_rd(touch_tile(TOUCH_PIN_RESET), TOUCH_GPIO_CFG(TOUCH_PIN_RESET)),
		    touch_rd(touch_tile(TOUCH_PIN_RESET), TOUCH_GPIO_IO(TOUCH_PIN_RESET)),
		    touch_rd(st.south, TOUCH_GPIO_CFG(122)), touch_rd(st.south, TOUCH_GPIO_IO(122)),
		    touch_rd(touch_tile(TOUCH_PIN_VDDIO), TOUCH_GPIO_CFG(TOUCH_PIN_VDDIO)),
		    touch_rd(touch_tile(TOUCH_PIN_VDDIO), TOUCH_GPIO_IO(TOUCH_PIN_VDDIO)));
		(void)touch_gpi_probe();
		return false;
	}
	if (((fw >> 8) & 0xffu) != 3u ||
	    ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 2u || ((rx >> 16) & 0x3fu) < 2u) {
		printf("SWEET TOUCH (cepheus): need ABL-loaded I2C firmware and 32-bit FIFOs; probes skipped\n");
		return false;
	}
	if (touch_rd(st.se, SE_STATUS) & 0x1001u) {
		printf("SWEET TOUCH (cepheus): SE17 already busy; probes skipped\n");
		return false;
	}
	touch_wr(st.se, 0xe18, 0); /* no GSI events */
	touch_wr(st.se, 0xe1c, 0); /* mask external IRQ routing; poll local status */
	touch_wr(st.se, 0x644, 0);
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, 0x648, 0xffffffffu);
	touch_wr(st.se, SE_DMA, 0);
	touch_wr(st.se, 0x28, touch_rd(st.se, 0x28) | 0x7fu);
	touch_wr(st.se, 0xe30, touch_rd(st.se, 0xe30) | 0x0fu);
	touch_wr(st.se, 0x24, 0x7f);
	touch_wr(st.se, 0x20, 1);
	touch_wr(st.se, SE_CLK_SEL, 0);
	touch_wr(st.se, SE_MCLK, (7u << 4) | 1u);
	touch_wr(st.se, SE_SCL, (10u << 20) | (11u << 10) | 26u); /* 100 kHz */
	/* Four 8-bit bytes per 32-bit word, LSB byte first (Linux packing). */
	touch_wr(st.se, 0x254, 0);
	touch_wr(st.se, 0x260, SE_PACK0);
	touch_wr(st.se, 0x264, SE_PACK1);
	touch_wr(st.se, 0x284, SE_PACK0);
	touch_wr(st.se, 0x288, SE_PACK1);
	touch_wr(st.se, SE_TX_WM, 0);
	touch_wr(st.se, 0x810, 0);
	touch_wr(st.se, 0x814, ((rx >> 16) & 0x3fu) - 1u);
	touch_wr(st.se, SE_IRQ_EN, SE_EVENTS);
	if (touch_rd(st.se, 0x260) != SE_PACK0 || touch_rd(st.se, 0x264) != SE_PACK1 ||
	    touch_rd(st.se, 0x284) != SE_PACK0 || touch_rd(st.se, 0x288) != SE_PACK1) {
		printf("SWEET TOUCH (cepheus): FIFO packing readback mismatch; stopping\n");
		return false;
	}
	if ((touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) ||
	    (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
		printf("SWEET TOUCH (cepheus): inherited FIFO data; probes skipped\n");
		return false;
	}
	return true;
}

static void
touch_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 45;
	(void)arg;
	(void)result;
	/*
	 * Start late (boot-arg sweettouchdelay, seconds): if the bus access
	 * below hangs the SoC, the persistent log and the GUI got there first.
	 */
	(void)PE_parse_boot_argn("sweettouchdelay", &delay_s, sizeof(delay_s));
	printf("SWEET TOUCH (cepheus): starting in %u s\n", delay_s);
	touch_wait(delay_s * 1000u);
	printf("SWEET TOUCH (cepheus): mapping GCC/TLMM/SE17\n");
	st.gcc = ml_io_map(TOUCH_GCC_PA, 0x60000);
	st.gpio = ml_io_map(TOUCH_NORTH_PA, 0x80000);
	st.south = ml_io_map(TOUCH_SOUTH_PA, 0x80000);
	st.se = ml_io_map(TOUCH_SE_PA, 0x4000);
	if (!st.gcc || !st.gpio || !st.south || !st.se) {
		printf("SWEET TOUCH (cepheus): MMIO mapping failed\n");
		goto out;
	}
	if (!touch_clocks() || !touch_se_init()) {
		goto out;
	}
	int power = 0;
	(void)PE_parse_boot_argn("sweettouchpower", &power, sizeof(power));
	if (power) {
		if (!st.gpio_i2c || cepheus_touch_power_enable() != 0) { goto out; }
		touch_wait(10);
	}
	printf("SWEET TOUCH (cepheus): TLMM reset=%#x irq=%#x vddio=%#x\n",
	    touch_rd(touch_tile(TOUCH_PIN_RESET), TOUCH_GPIO_IO(TOUCH_PIN_RESET)),
	    touch_rd(st.south, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)),
	    touch_rd(touch_tile(TOUCH_PIN_VDDIO), TOUCH_GPIO_IO(TOUCH_PIN_VDDIO)));
	/* SDA/SCL on qup17, IRQ input with pull-up, VDDIO enable high. */
	if (!st.gpio_i2c) {
		touch_gpio(TOUCH_PIN_SDA, 1, 0, 0, false, false);
		touch_gpio(TOUCH_PIN_SCL, 1, 0, 0, false, false);
	}
	touch_gpio(TOUCH_PIN_IRQ, 0, 3, 3, false, false);
	touch_gpio(TOUCH_PIN_VDDIO, 0, 0, 0, true, true);
	touch_wait(10);
	int reset = 0;
	(void)PE_parse_boot_argn("sweettouchreset", &reset, sizeof(reset));
	if (reset) {
		touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, false);
		touch_wait(10);
	}
	touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, true);
	touch_wait(300);
	printf("SWEET TOUCH (cepheus): powered RESET cfg=%#x io=%#x VDDIO cfg=%#x io=%#x\n",
	    touch_rd(touch_tile(TOUCH_PIN_RESET), TOUCH_GPIO_CFG(TOUCH_PIN_RESET)),
	    touch_rd(touch_tile(TOUCH_PIN_RESET), TOUCH_GPIO_IO(TOUCH_PIN_RESET)),
	    touch_rd(touch_tile(TOUCH_PIN_VDDIO), TOUCH_GPIO_CFG(TOUCH_PIN_VDDIO)),
	    touch_rd(touch_tile(TOUCH_PIN_VDDIO), TOUCH_GPIO_IO(TOUCH_PIN_VDDIO)));
	if (st.gpio_i2c && !touch_bb_init()) { goto out; }

	uint8_t id[2] = {0};
	int ce = fts_read_hw(FTS_ADDR_DCHIP_ID, id, sizeof(id));
	printf("SWEET TOUCH (cepheus): probe 0x%02x err=%d chip-id=%02x:%02x irq=%#x\n",
	    TOUCH_I2C_ADDR, ce, id[0], id[1], st.irq_status);
	bool fts = !ce && id[0] == FTS_DCHIP_ID_0 && id[1] == 0x39;
	if (st.dead || !fts) {
		printf("SWEET TOUCH (cepheus): no supported IC (fts=%d dead=%d); stopping\n",
		    fts, st.dead);
		goto out;
	}
	/* Vendor setScanMode(SCAN_MODE_ACTIVE, ACTIVE_MULTI_TOUCH). Not firmware
	 * upload or flash erase. After reset the controller does not scan by default.
	 * Like Xiaomi fts_system_reset(), first wait for the controller-ready
	 * event (Linux allows 100 ms; give it 1 s), logging whatever the FIFO
	 * holds; then log what the IC answers to the scan command.
	 */
	bool ready = touch_drain_log("boot event", 1000, FTS_EVT_CONTROLLER_READY);
	printf("SWEET TOUCH (cepheus): controller ready %s\n", ready ? "seen" : "NOT seen in 1 s");
	(void)touch_drain_log("pending event", 50, 0);
	int scan_error = touch_scan_on("after reset");
	printf("SWEET TOUCH (cepheus): active scan err=%d\n", scan_error);
	if (scan_error) { goto out; }
	(void)touch_drain_log("scan answer", 200, 0);
	printf("SWEET TOUCH (cepheus): selected ST-FTS, %dx%d, poll=10ms irq-hint=%d\n",
	    TOUCH_X_MAX, TOUCH_Y_MAX, st.irq_hint);
	unsigned errors = 0, ticks = 0;
	while (!st.dead) {
		/* Optional IRQ hint still reads every 100 ms, and always while down. */
		if (!st.irq_hint || st.active ||
		    !(touch_rd(st.south, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)) & 1u) ||
		    (++ticks % 10u) == 0) {
			int err = touch_fts();
			if (err) {
				if (st.gpio_i2c && (err == EAGAIN || err == ETIMEDOUT || err == EBUSY)) {
					printf("SWEET TOUCH (cepheus): GPIO bus err=%d; restoring pins and stopping\n", err);
					st.dead = true;
				}
				if ((errors++ % 100u) == 0) {
					printf("SWEET TOUCH (cepheus): report err=%d irq=%#x\n",
					    err, st.irq_status);
				}
				if (errors == 10) {
					touch_report(0, st.x, st.y); /* release after bad reads */
				}
			} else {
				errors = 0;
			}
		}
		touch_wait(10);
	}
	touch_report(0, st.x, st.y);
out:
	touch_bb_restore();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_touch_init(void)
{
	cepheus_soc_init(); /* opt-in independent CPU/GPU thread */
	andromeda_probe_init(); /* opt-in read-only BT/Wi-Fi/charger probe */
	andromeda_cpufreq_init(); /* opt-in full CPU clock with thermal guard */
	int enabled = 1;
	thread_t thread;
	if (st.started) {
		return;
	}
	st.started = true;
	(void)PE_parse_boot_argn("sweettouch", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sweettouchirq", &st.irq_hint, sizeof(st.irq_hint));
	(void)PE_parse_boot_argn("sweettouchgpio", &st.gpio_i2c, sizeof(st.gpio_i2c));
	if (!enabled) {
		printf("SWEET TOUCH (cepheus): disabled (sweettouch=0)\n");
		return;
	}
	unsigned pin_reset = touch_pin_reset, pin_vddio = touch_pin_vddio;
	(void)PE_parse_boot_argn("sweettouchrst", &pin_reset, sizeof(pin_reset));
	(void)PE_parse_boot_argn("sweettouchvddio", &pin_vddio, sizeof(pin_vddio));
	(void)PE_parse_boot_argn("sweettouchraw", &touch_raw_log, sizeof(touch_raw_log));
	(void)PE_parse_boot_argn("sweettouchscan", &touch_scan_settings, sizeof(touch_scan_settings));
	touch_scan_settings &= 0xffu;
	if ((pin_reset != 12u && pin_reset != 54u) || (pin_vddio != 84u && pin_vddio != 37u)) {
		printf("SWEET TOUCH (cepheus): refusing pins reset=%u vddio=%u (allowed 12/54, 84/37)\n",
		    pin_reset, pin_vddio);
		return;
	}
	touch_pin_reset = pin_reset;
	touch_pin_vddio = pin_vddio;
	printf("SWEET TOUCH (cepheus): pins reset=%u vddio=%u\n", touch_pin_reset, touch_pin_vddio);
	kern_return_t kr = kernel_thread_start(touch_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET TOUCH (cepheus): kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
