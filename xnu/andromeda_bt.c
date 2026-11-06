/*
 * Mi Mix 3 5G (andromeda, SM8150): WCN3990 Bluetooth bring-up, step B1 of
 * docs/research/wifi-bluetooth.md. Off unless the boot-arg andrbt=1 is set.
 *
 * Boot-args:
 *   andrbt=1        once, andrbtdelay seconds after start: vote the three BT
 *                   rails over RPMh (sweet_rpmh.c, patch 0092), mux GPIO 43-46
 *                   to qup13, vote and clock SE13, GENI UART in FIFO mode,
 *                   power pulses 0xC0 at 2400 baud and 0xFC at 115200, then H4
 *                   commands: Qualcomm EDL "read SoC version" (0xFC00, 0x19),
 *                   HCI_Reset and HCI_Read_Local_Version_Information. Every
 *                   reply byte is logged. Then the chip is powered down again
 *                   (0xC0 at 2400, rails off) and pins/clock are restored.
 *                   Any error stops the sequence and powers down the same way.
 *   andrbtdelay=N   seconds before starting (default 150, after the touch and
 *                   the andrprobe threads have finished with the GCC vote).
 *   andrbtxo=1      115200 baud from XO * 48/125 instead of the Linux table
 *                   entry GPLL0_OUT_EVEN * 384/15625 (both 7.3728 MHz).
 *   andrbtflow=0    HCI phase without RTS/CTS flow control (diagnostic).
 *
 * Hardware numbers: live DT of the phone (qupv3_se13_4uart @0xc8c000,
 * pinctrl qupv3_se13_{ctsrx,rts,tx}, bt_wcn3990 supplies) and the
 * andromeda_probe.c log (SE13 firmware proto 2, RCG XO/1 without DFS, wrapper
 * AHB clocks on, s3 voteable, pins 43-46 function 0). Register layout and
 * sequence from Linux, used as documentation only (GPL-2.0, nothing copied):
 * drivers/tty/serial/qcom_geni_serial.c and soc/qcom/qcom-geni-se.c
 * (UART set-up, FIFO packing, termios, RX/TX, cancel/abort), drivers/
 * bluetooth/hci_qca.c (WCN399x power pulses, flow control around them, the
 * 100 ms boot wait, the power-off sequence), drivers/bluetooth/btqca.c/.h
 * (EDL version request and reply layout), drivers/clk/qcom/gcc-sm8150.c
 * (SE13 RCG, parent map, frequency table) and clk-rcg2.c (M/N/D, update).
 * See CREDITS.md.
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

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
/* bsd/dev/sweet_rpmh.c, patch 0092: ldoa7/ldoc2/ldoc11 at their DT voltages. */
extern int andromeda_rpmh_bt_rail(const char *res, bool on);
void andromeda_bt_init(void);

#define BT_TAG "ANDROMEDA BT:"

/* TLMM EAST tile (pinctrl-sm8150.c): GPIO 43 CTS, 44 RTS, 45 TX, 46 RX. */
#define BT_TLMM_EAST_PA 0x03500000u
#define BT_TLMM_EAST_SZ 0x00030000u
#define BT_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define BT_PIN_FIRST    43u
#define BT_FUNC_QUP13   1u           /* PINGROUP(43..46, EAST, qup13, ...) */

/* GCC (gcc-sm8150.c). */
#define BT_GCC_PA       0x00100000u
#define BT_GCC_SZ       0x00060000u
#define BT_GPLL0_MODE   0x00000u     /* Trion: MODE 0x0, L 0x4, USER_CTL 0xc, ALPHA 0x40 */
#define BT_GPLL0_L      0x00004u
#define BT_GPLL0_USER   0x0000cu
#define BT_GPLL0_ALPHA  0x00040u
#define BT_GPLL0_VOTE   0x52000u     /* bit 0: gpll0 */
#define BT_GCC_VOTE     0x52014u     /* APCS_CLOCK_BRANCH_ENA_VOTE_1 */
#define BT_S3_BIT       (1u << 7)    /* gcc_qupv3_wrap2_s3_clk */
#define BT_M_AHB        0x1e004u
#define BT_S_AHB        0x1e008u
#define BT_S3_CBCR      0x1e4d4u
#define BT_RCG          0x1e4d8u     /* gcc_qupv3_wrap2_s3_clk_src, mnd 16, hid 5 */
#define RCG_CMD         0x00u        /* bit 0 UPDATE, bit 31 ROOT_OFF */
#define RCG_CFG         0x04u        /* 4:0 div, 10:8 src, 13:12 mode, 20 HW_CTL */
#define RCG_M           0x08u
#define RCG_N           0x0cu
#define RCG_D           0x10u
#define RCG_DFSR        0x14u        /* bit 0 DFS enable */
#define RCG_CFG_MASK    0x371fu
#define RCG_SRC_XO      0u
#define RCG_SRC_GPLL0_EVEN 6u
#define RCG_MODE_DUAL   (2u << 12)
#define BT_XO_HZ        19200000u

/* QUPv3 wrapper 2 (live DT qupv3_2 @0xcc0000): QUP_HW_VER at +4. */
#define BT_QUP2_PA      0x00cc0000u
#define BT_QUP_VER_2_5  0x20050000u  /* sampling rate halves from here on */

/* qupv3_se13_4uart (live DT, reg 0xc8c000 0x4000). qcom-geni-se.h names. */
#define BT_SE13_PA      0x00c8c000u
#define BT_SE13_SZ      0x00004000u
#define GENI_FORCE_DEFAULT 0x020u
#define GENI_OUTPUT_CTRL   0x024u
#define GENI_CGC_CTRL      0x028u
#define GENI_STATUS        0x040u     /* bit 0 M active, bit 12 S active */
#define GENI_M_CLK_CFG     0x048u
#define GENI_S_CLK_CFG     0x04cu
#define GENI_IF_DISABLE    0x064u
#define GENI_FW_REV        0x068u
#define GENI_CLK_SEL       0x07cu
#define UART_LOOPBACK      0x22cu
#define GENI_BYTE_GRAN     0x254u
#define GENI_DMA_MODE      0x258u
#define UART_TX_TRANS_CFG  0x25cu     /* bit 1 CTS_MASK: ignore CTS */
#define GENI_TX_PACK0      0x260u
#define GENI_TX_PACK1      0x264u
#define UART_TX_WORD_LEN   0x268u
#define UART_TX_STOP_LEN   0x26cu
#define UART_TX_TRANS_LEN  0x270u
#define UART_RX_TRANS_CFG  0x280u
#define GENI_RX_PACK0      0x284u
#define GENI_RX_PACK1      0x288u
#define UART_RX_WORD_LEN   0x28cu
#define UART_RX_STALE_CNT  0x294u
#define UART_TX_PARITY     0x2a4u
#define UART_RX_PARITY     0x2a8u
#define UART_MANUAL_RFR    0x2acu     /* bit 31 manual, bit 1 not ready */
#define GENI_M_CMD0        0x600u
#define GENI_M_CMD_CTRL    0x604u     /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS  0x610u
#define GENI_M_IRQ_EN      0x614u
#define GENI_M_IRQ_CLEAR   0x618u
#define GENI_S_CMD0        0x630u
#define GENI_S_CMD_CTRL    0x634u
#define GENI_S_IRQ_STATUS  0x640u
#define GENI_S_IRQ_EN      0x644u
#define GENI_S_IRQ_CLEAR   0x648u
#define GENI_TX_FIFO       0x700u
#define GENI_RX_FIFO       0x780u
#define GENI_TX_FIFO_STS   0x800u     /* 27:0 words */
#define GENI_RX_FIFO_STS   0x804u     /* 31 last, 30:28 valid bytes, 24:0 words */
#define GENI_TX_WM         0x80cu
#define GENI_RX_WM         0x810u
#define GENI_RX_RFR_WM     0x814u
#define GENI_IOS           0x908u     /* bit 0 RX in, bit 1 CTS in (low = asserted) */
#define SE_DMA_TX_IRQ_CLR  0xc44u
#define SE_DMA_RX_IRQ_CLR  0xd44u
#define SE_GSI_EVENT_EN    0xe18u
#define SE_IRQ_EN          0xe1cu
#define SE_HW_PARAM_0      0xe24u
#define SE_HW_PARAM_1      0xe28u
#define SE_DMA_GENERAL_CFG 0xe30u

#define M_CMD_DONE      (1u << 0)
#define M_CMD_CANCEL    (1u << 4)
#define M_CMD_ABORT     (1u << 5)
#define M_ERRORS        ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 28) | (1u << 29))
#define M_IRQ_ENABLES   (M_CMD_DONE | 0x7eu | (1u << 22) | (1u << 23) | (1u << 24) | \
                         (1u << 25) | (1u << 28) | (1u << 29))
#define S_CMD_CANCEL    (1u << 4)
#define S_CMD_ABORT     (1u << 5)
#define S_RX_ERRORS     ((0xfu << 9) | (1u << 24) | (1u << 25)) /* parity/break, FIFO */
#define S_IRQ_ENABLES   (1u | 0x3eu | (0x1fu << 9) | (1u << 24) | (1u << 25) | \
                         (1u << 26) | (1u << 27))
#define UART_START_TX   1u
#define UART_START_READ 1u
/* geni_se_config_packing(8 bits, 4 per word, LSB first, tx and rx). */
#define UART_PACK0      0x0004380eu
#define UART_PACK1      0x000c3e0eu

#define BT_RX_MAX       192u

static struct {
	vm_offset_t gcc, tlmm, se, qup;
	uint32_t saved_vote, saved_pin[4];
	uint32_t saved_cfg, saved_m, saved_n, saved_d;
	unsigned oversampling, rx_depth, tx_depth;
	unsigned rails;                 /* bit i: rail i voted on by us */
	bool vote_set, pins_set, rcg_changed, uart_set, rx_on, chip_pulsed, se_dead;
	bool xo_mnd, flow;
} bt;

static unsigned bt_sleep_chan;
static unsigned bt_delay_s = 150;

static const char *const bt_rail_name[3] = {"ldoa7", "ldoc2", "ldoc11"};

static void
bt_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&bt_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
bt_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
bt_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Poll until (reg & mask) == want, at most us microseconds. */
static bool
bt_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i <= us; ++i) {
		if ((bt_rd(base, off) & mask) == want) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
bt_hex(const char *what, const uint8_t *buf, unsigned n)
{
	char line[140];
	if (n == 0) {
		printf(BT_TAG " %s: no bytes\n", what);
		return;
	}
	for (unsigned start = 0; start < n; start += 32) {
		int len = snprintf(line, sizeof(line), BT_TAG " %s[%u+%u]:", what, n, start);
		for (unsigned i = start; i < n && i < start + 32 && len > 0 &&
		    (size_t)len < sizeof(line) - 4; ++i) {
			len += snprintf(line + len, sizeof(line) - (size_t)len, " %02x", buf[i]);
		}
		printf("%s\n", line);
	}
}

/* ---- clock --------------------------------------------------------------- */

static bool
bt_s3_on(void)
{
	return cepheus_branch_enabled(bt_rd(bt.gcc, BT_M_AHB), 0) &&
	       cepheus_branch_enabled(bt_rd(bt.gcc, BT_S_AHB), 1) &&
	       cepheus_branch_enabled(bt_rd(bt.gcc, BT_S3_CBCR), 0);
}

/* Our vote bit must still be there before every SE13 access phase. */
static bool
bt_clock_ok(const char *phase)
{
	if (!bt.vote_set || !(bt_rd(bt.gcc, BT_GCC_VOTE) & BT_S3_BIT) || !bt_s3_on()) {
		printf(BT_TAG " %s: SE13 clock vote lost (vote=%#x CBCR=%#x); stopping\n", phase,
		    bt_rd(bt.gcc, BT_GCC_VOTE), bt_rd(bt.gcc, BT_S3_CBCR));
		bt.se_dead = true;          /* no SE13 register access without its clock */
		return false;
	}
	return true;
}

/* clk-rcg2.c __clk_rcg2_configure_mnd + update_config, 16-bit M/N/D. */
static bool
bt_rcg_program(unsigned src, bool mnd, uint32_t m, uint32_t n)
{
	vm_offset_t r = bt.gcc + BT_RCG;
	if (mnd) {
		uint32_t d = n, top = 2u * (n - m);
		if (d < m) { d = m; }
		if (d > top) { d = top; }
		bt_wr(r, RCG_M, (bt_rd(r, RCG_M) & ~0xffffu) | (m & 0xffffu));
		bt_wr(r, RCG_N, (bt_rd(r, RCG_N) & ~0xffffu) | (~(n - m) & 0xffffu));
		bt_wr(r, RCG_D, (bt_rd(r, RCG_D) & ~0xffffu) | (~d & 0xffffu));
	}
	uint32_t cfg = (bt_rd(r, RCG_CFG) & ~RCG_CFG_MASK) | 1u | (src << 8) |
	    (mnd ? RCG_MODE_DUAL : 0u);
	bt_wr(r, RCG_CFG, cfg);
	bt.rcg_changed = true;
	bt_wr(r, RCG_CMD, bt_rd(r, RCG_CMD) | 1u);
	if (!bt_poll(r, RCG_CMD, 1u, 0u, 500)) {
		printf(BT_TAG " RCG update timeout cmd=%#x cfg=%#x\n", bt_rd(r, RCG_CMD), bt_rd(r, RCG_CFG));
		return false;
	}
	return true;
}

static bool
bt_rcg_restore(void)
{
	vm_offset_t r = bt.gcc + BT_RCG;
	bt_wr(r, RCG_M, bt.saved_m);
	bt_wr(r, RCG_N, bt.saved_n);
	bt_wr(r, RCG_D, bt.saved_d);
	bt_wr(r, RCG_CFG, bt.saved_cfg);
	bt_wr(r, RCG_CMD, bt_rd(r, RCG_CMD) | 1u);
	bool ok = bt_poll(r, RCG_CMD, 1u, 0u, 500);
	bt.rcg_changed = !ok;
	return ok;
}

/* gpll0_out_even must be the 300 MHz the Linux table assumes (read only). */
static bool
bt_gpll0_even_ok(void)
{
	uint32_t mode = bt_rd(bt.gcc, BT_GPLL0_MODE), l = bt_rd(bt.gcc, BT_GPLL0_L) & 0xffffu;
	uint32_t user = bt_rd(bt.gcc, BT_GPLL0_USER), alpha = bt_rd(bt.gcc, BT_GPLL0_ALPHA) & 0xffffu;
	uint64_t hz = (uint64_t)BT_XO_HZ * l + (((uint64_t)BT_XO_HZ * alpha) >> 16);
	unsigned postdiv = (user >> 8) & 0xfu;
	bool ok = (mode & (1u << 31)) && (user & 2u) && postdiv == 1u &&
	    hz >= 599000000ull && hz <= 601000000ull;
	printf(BT_TAG " GPLL0 mode=%#x L=%u alpha=%#x user=%#x vote=%#x -> %llu Hz, even /%u: %s\n",
	    mode, l, alpha, user, bt_rd(bt.gcc, BT_GPLL0_VOTE), (unsigned long long)hz,
	    postdiv == 1u ? 2u : postdiv + 1u, ok ? "300 MHz even output usable" :
	    "not as expected (andrbtxo=1 uses XO instead)");
	return ok;
}

/* ---- GENI UART ----------------------------------------------------------- */

static void
bt_m_clear(void)
{
	bt_wr(bt.se, GENI_M_IRQ_CLEAR, 0xffffffffu);
}

/* qcom_geni_serial stop_rx: cancel, then abort, the secondary sequencer. */
static bool
bt_rx_stop(void)
{
	if (bt.se_dead) {
		return false;
	}
	if (bt_rd(bt.se, GENI_STATUS) & (1u << 12)) {
		bt_wr(bt.se, GENI_S_CMD_CTRL, 1u << 2);
		(void)bt_poll(bt.se, GENI_S_IRQ_STATUS, S_CMD_CANCEL, S_CMD_CANCEL, 20000);
		bt_wr(bt.se, GENI_S_IRQ_CLEAR, bt_rd(bt.se, GENI_S_IRQ_STATUS));
		if (bt_rd(bt.se, GENI_STATUS) & (1u << 12)) {
			bt_wr(bt.se, GENI_S_CMD_CTRL, 1u << 1);
			(void)bt_poll(bt.se, GENI_S_CMD_CTRL, 1u << 1, 0u, 20000);
			bt_wr(bt.se, GENI_S_IRQ_CLEAR, 1u | S_CMD_ABORT);
			bt_wr(bt.se, GENI_FORCE_DEFAULT, 1u);
		}
	}
	bt.rx_on = (bt_rd(bt.se, GENI_STATUS) & (1u << 12)) != 0;
	if (bt.rx_on) {
		printf(BT_TAG " RX sequencer does not stop (status=%#x); SE13 left alone\n",
		    bt_rd(bt.se, GENI_STATUS));
		bt.se_dead = true;
		return false;
	}
	return true;
}

static void
bt_rx_start(void)
{
	bt_wr(bt.se, GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, GENI_S_CMD0, UART_START_READ << 27);
	bt.rx_on = true;
}

/* Read what the RX FIFO holds (Linux handle_rx_fifo); returns bytes stored. */
static unsigned
bt_rx_read(uint8_t *buf, unsigned have, unsigned cap)
{
	static unsigned errors_logged;
	uint32_t s_irq = bt_rd(bt.se, GENI_S_IRQ_STATUS);
	if (s_irq & S_RX_ERRORS) {
		if (errors_logged++ < 8) {
			printf(BT_TAG " RX status %#x (parity/break/FIFO error)\n", s_irq & S_RX_ERRORS);
		}
	}
	if (s_irq) {
		bt_wr(bt.se, GENI_S_IRQ_CLEAR, s_irq);
	}
	uint32_t status = bt_rd(bt.se, GENI_RX_FIFO_STS);
	unsigned words = status & 0x1ffffffu;
	if (words == 0) {
		return have;
	}
	if (words > bt.rx_depth) {
		words = bt.rx_depth;    /* never trust an impossible count */
	}
	unsigned last = (status >> 28) & 7u;
	unsigned total = 4u * (words - 1u) + (((status >> 31) & 1u) && last ? last : 4u);
	for (unsigned w = 0; w < words; ++w) {
		uint32_t word = bt_rd(bt.se, GENI_RX_FIFO);
		for (unsigned b = 0; b < 4 && 4u * w + b < total; ++b) {
			if (have < cap) {
				buf[have++] = (uint8_t)(word >> (8u * b));
			}
		}
	}
	return have;
}

/* One FIFO transmission (Linux setup_tx + send_chunk_fifo), polled. */
static int
bt_tx(const uint8_t *data, unsigned n, unsigned timeout_ms)
{
	if (bt.se_dead || n == 0 || n > 4u * bt.tx_depth) {
		return EINVAL;
	}
	if (bt_rd(bt.se, GENI_STATUS) & 1u) {
		printf(BT_TAG " TX sequencer busy before send\n");
		return EBUSY;
	}
	bt_m_clear();
	bt_wr(bt.se, UART_TX_TRANS_LEN, n);
	bt_wr(bt.se, GENI_M_CMD0, UART_START_TX << 27);
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t word = 0;
		for (unsigned b = 0; b < 4 && i + b < n; ++b) {
			word |= (uint32_t)data[i + b] << (8u * b);
		}
		bt_wr(bt.se, GENI_TX_FIFO, word);
	}
	uint32_t irq = 0;
	for (unsigned i = 0; i < timeout_ms * 20u; ++i) {
		irq = bt_rd(bt.se, GENI_M_IRQ_STATUS);
		if (irq & (M_CMD_DONE | M_ERRORS)) {
			break;
		}
		delay(50);
	}
	if ((irq & M_CMD_DONE) && !(irq & M_ERRORS)) {
		bt_m_clear();
		return 0;
	}
	printf(BT_TAG " TX %s irq=%#x status=%#x tx-fifo=%#x ios=%#x\n",
	    (irq & M_ERRORS) ? "error" : "timeout (CTS not asserted?)", irq,
	    bt_rd(bt.se, GENI_STATUS), bt_rd(bt.se, GENI_TX_FIFO_STS), bt_rd(bt.se, GENI_IOS));
	/* Cancel, then abort, the primary sequencer (qcom_geni_serial). */
	bt_wr(bt.se, GENI_M_CMD_CTRL, 1u << 2);
	(void)bt_poll(bt.se, GENI_M_IRQ_STATUS, M_CMD_CANCEL, M_CMD_CANCEL, 20000);
	if (bt_rd(bt.se, GENI_STATUS) & 1u) {
		bt_wr(bt.se, GENI_M_CMD_CTRL, 1u << 1);
		(void)bt_poll(bt.se, GENI_M_IRQ_STATUS, M_CMD_ABORT, M_CMD_ABORT, 20000);
	}
	bt_m_clear();
	if (bt_rd(bt.se, GENI_STATUS) & 1u) {
		printf(BT_TAG " TX sequencer does not stop; SE13 left alone\n");
		bt.se_dead = true;
	}
	return (irq & M_ERRORS) ? EIO : ETIMEDOUT;
}

/* Wait until a short transmission has left the wire (one 10-bit char). */
static void
bt_char_time(unsigned baud)
{
	delay((int)(10000000u / baud + 50u));
}

/*
 * Baud rate change (Linux set_termios): RX stopped, RCG and SE dividers set,
 * 8N1, flow control as asked, RX restarted. rcg_hz is what the RCG runs at.
 */
static bool
bt_uart_baud(unsigned baud, unsigned rcg_hz, bool flow)
{
	unsigned want = baud * bt.oversampling;
	unsigned div = want ? rcg_hz / want : 0;
	if (!div || div > 0xfffu || rcg_hz % want) {
		printf(BT_TAG " %u baud: %u Hz has no exact divider\n", baud, rcg_hz);
		return false;
	}
	if (!bt_rx_stop()) {
		return false;
	}
	bt_wr(bt.se, UART_LOOPBACK, 0);
	bt_wr(bt.se, UART_TX_TRANS_CFG, flow ? 0u : 2u);
	bt_wr(bt.se, UART_TX_PARITY, 0);
	bt_wr(bt.se, UART_RX_TRANS_CFG, 0);
	bt_wr(bt.se, UART_RX_PARITY, 0);
	bt_wr(bt.se, UART_TX_WORD_LEN, 8);
	bt_wr(bt.se, UART_RX_WORD_LEN, 8);
	bt_wr(bt.se, UART_TX_STOP_LEN, 0);
	bt_wr(bt.se, GENI_M_CLK_CFG, (div << 4) | 1u);
	bt_wr(bt.se, GENI_S_CLK_CFG, (div << 4) | 1u);
	/* RTS (our RFR) not ready while flow control is off, hardware-driven when on. */
	bt_wr(bt.se, UART_MANUAL_RFR, flow ? 0u : ((1u << 31) | (1u << 1)));
	bt_rx_start();
	printf(BT_TAG " UART %u baud: clock %u Hz / %u / %u, flow control %s\n",
	    baud, rcg_hz, div, bt.oversampling, flow ? "on" : "off");
	return true;
}

/* Linux geni_se_init + port setup in FIFO mode; does not start a transfer. */
static bool
bt_uart_setup(void)
{
	bt_wr(bt.se, GENI_CGC_CTRL, bt_rd(bt.se, GENI_CGC_CTRL) | 0x7fu);
	bt_wr(bt.se, SE_DMA_GENERAL_CFG, bt_rd(bt.se, SE_DMA_GENERAL_CFG) | 0xfu);
	bt_wr(bt.se, GENI_OUTPUT_CTRL, 0x7fu);
	bt_wr(bt.se, GENI_FORCE_DEFAULT, 1u);
	bt_wr(bt.se, SE_GSI_EVENT_EN, 0);
	bt_wr(bt.se, SE_IRQ_EN, 0);                  /* polled: no GIC line */
	bt_wr(bt.se, GENI_M_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_DMA_TX_IRQ_CLR, 0xffffffffu);
	bt_wr(bt.se, SE_DMA_RX_IRQ_CLR, 0xffffffffu);
	bt_wr(bt.se, GENI_DMA_MODE, 0);              /* FIFO mode */
	bt_wr(bt.se, GENI_CLK_SEL, 0);
	bt_wr(bt.se, UART_RX_STALE_CNT, 10u * 16u);  /* 16 characters */
	bt_wr(bt.se, GENI_TX_PACK0, UART_PACK0);
	bt_wr(bt.se, GENI_TX_PACK1, UART_PACK1);
	bt_wr(bt.se, GENI_RX_PACK0, UART_PACK0);
	bt_wr(bt.se, GENI_RX_PACK1, UART_PACK1);
	bt_wr(bt.se, GENI_BYTE_GRAN, 0);
	bt_wr(bt.se, GENI_TX_WM, 0);
	bt_wr(bt.se, GENI_RX_WM, 2);
	bt_wr(bt.se, GENI_RX_RFR_WM, bt.rx_depth - 2u);
	bt_wr(bt.se, GENI_M_IRQ_EN, M_IRQ_ENABLES);
	bt_wr(bt.se, GENI_S_IRQ_EN, S_IRQ_ENABLES);
	bt.uart_set = true;
	if (bt_rd(bt.se, GENI_TX_PACK0) != UART_PACK0 || bt_rd(bt.se, GENI_RX_PACK1) != UART_PACK1 ||
	    bt_rd(bt.se, GENI_DMA_MODE) != 0) {
		printf(BT_TAG " SE13 readback mismatch (pack %#x/%#x dma %#x)\n",
		    bt_rd(bt.se, GENI_TX_PACK0), bt_rd(bt.se, GENI_RX_PACK1), bt_rd(bt.se, GENI_DMA_MODE));
		return false;
	}
	return true;
}

/* hci_qca.c qca_send_power_pulse: one byte, flow control off, RTS deasserted. */
static bool
bt_pulse(uint8_t byte, unsigned baud)
{
	if (!bt_clock_ok("pulse")) {
		return false;
	}
	int err = bt_tx(&byte, 1, 50);
	bt_char_time(baud);
	printf(BT_TAG " power pulse %#04x at %u baud: %s (err=%d) ios=%#x\n", byte, baud,
	    err ? "NOT sent" : "sent", err, bt_rd(bt.se, GENI_IOS));
	return err == 0;
}

/* ---- pins ---------------------------------------------------------------- */

static uint32_t
bt_pin_cfg(unsigned pin, uint32_t saved)
{
	/* Live DT: ctsrx 43/46 bias-disable, rts 44 pull-down, tx 45 pull-up, 2 mA. */
	static const uint32_t pull[4] = {0u, 1u, 3u, 0u};
	uint32_t cfg = (saved & ~0x3ffu) | (BT_FUNC_QUP13 << 2) | (0u << 6) | pull[pin - BT_PIN_FIRST];
	if (cfg & (1u << 11)) { cfg |= 1u << 12; }
	return cfg;
}

static bool
bt_pins_qup13(void)
{
	bt.pins_set = true;
	for (unsigned i = 0; i < 4; ++i) {
		unsigned pin = BT_PIN_FIRST + i;
		bt_wr(bt.tlmm, BT_GPIO_CFG(pin), bt_pin_cfg(pin, bt.saved_pin[i]));
	}
	bool ok = true;
	for (unsigned i = 0; i < 4; ++i) {
		unsigned pin = BT_PIN_FIRST + i;
		ok &= bt_rd(bt.tlmm, BT_GPIO_CFG(pin)) == bt_pin_cfg(pin, bt.saved_pin[i]);
	}
	printf(BT_TAG " pins qup13: 43=%#x 44=%#x 45=%#x 46=%#x %s, ios=%#x\n",
	    bt_rd(bt.tlmm, BT_GPIO_CFG(43)), bt_rd(bt.tlmm, BT_GPIO_CFG(44)),
	    bt_rd(bt.tlmm, BT_GPIO_CFG(45)), bt_rd(bt.tlmm, BT_GPIO_CFG(46)),
	    ok ? "ok" : "READBACK MISMATCH", bt_rd(bt.se, GENI_IOS));
	return ok;
}

/* ---- rails --------------------------------------------------------------- */

static int
bt_rail(unsigned i, bool on)
{
	int err = EBUSY;
	for (unsigned attempt = 0; attempt < 5 && err == EBUSY; ++attempt) {
		err = andromeda_rpmh_bt_rail(bt_rail_name[i], on);
		if (err == EBUSY) {
			bt_wait(100);   /* another sweet_rpmh user holds the lock or a slot */
		}
	}
	if (!err) {
		bt.rails = on ? (bt.rails | (1u << i)) : (bt.rails & ~(1u << i));
	}
	return err;
}

/* ---- HCI over H4 --------------------------------------------------------- */

struct bt_event {
	uint8_t code, len;
	const uint8_t *p;
};

/* Find the first complete H4 event (type 4) in buf that match() accepts. */
static bool
bt_find_event(const uint8_t *buf, unsigned n, bool (*match)(const struct bt_event *),
    struct bt_event *out)
{
	for (unsigned i = 0; i + 3 <= n; ++i) {
		if (buf[i] != 0x04 || i + 3u + buf[i + 2] > n) {
			continue;
		}
		struct bt_event e = {buf[i + 1], buf[i + 2], buf + i + 3};
		if (match(&e)) {
			*out = e;
			return true;
		}
	}
	return false;
}

static bool
bt_is_edl_version(const struct bt_event *e)
{
	/* btqca.c: VSE {cresp EDL_CMD_REQ_RES_EVT 0, rtype EDL_APP_VER_RES_EVT 2, 12 bytes} */
	if (e->code == 0xff && e->len >= 14 && e->p[0] == 0x00 && e->p[1] == 0x02) {
		return true;
	}
	/* WCN3991 and later: Command Complete for 0xFC00, then {status, 0x19, version}. */
	return e->code == 0x0e && e->len >= 18 && e->p[1] == 0x00 && e->p[2] == 0xfc;
}

static uint16_t bt_cc_opcode;

static bool
bt_is_complete(const struct bt_event *e)
{
	return (e->code == 0x0e && e->len >= 4 && (e->p[1] | (e->p[2] << 8)) == bt_cc_opcode) ||
	       (e->code == 0x0f && e->len >= 4 && (e->p[2] | (e->p[3] << 8)) == bt_cc_opcode &&
	        e->p[0] != 0);   /* Command Status with an error */
}

/* Send one H4 command and collect replies until match() or ms elapse. */
static bool
bt_command(const char *what, const uint8_t *cmd, unsigned n, bool (*match)(const struct bt_event *),
    uint8_t *rx, unsigned *rx_n, unsigned ms, struct bt_event *event)
{
	*rx_n = 0;
	if (!bt_clock_ok(what)) {
		return false;
	}
	bt_hex(what, cmd, n);
	int err = bt_tx(cmd, n, 100);
	if (err) {
		printf(BT_TAG " %s: send failed err=%d\n", what, err);
		return false;
	}
	bool found = false;
	for (unsigned t = 0; t < ms && !found; ++t) {
		*rx_n = bt_rx_read(rx, *rx_n, BT_RX_MAX);
		found = bt_find_event(rx, *rx_n, match, event);
		if (!found) {
			bt_wait(1);
		}
	}
	if (found) {
		bt_wait(20);        /* trailing bytes of the same answer */
		*rx_n = bt_rx_read(rx, *rx_n, BT_RX_MAX);
		(void)bt_find_event(rx, *rx_n, match, event);
	}
	bt_hex(found ? "reply" : "no matching reply, got", rx, *rx_n);
	return found;
}

static const char *
bt_hci(void)
{
	uint8_t rx[BT_RX_MAX];
	unsigned rx_n = 0;
	struct bt_event e;

	/* btqca.c qca_read_soc_version: EDL_PATCH_CMD_OPCODE 0xFC00, EDL_PATCH_VER_REQ_CMD 0x19. */
	static const uint8_t edl[] = {0x01, 0x00, 0xfc, 0x01, 0x19};
	if (!bt_command("tx EDL version", edl, sizeof(edl), bt_is_edl_version, rx, &rx_n, 500, &e)) {
		return "no EDL version reply";
	}
	/* qca_btsoc_version after {cresp, rtype}; a Command Complete adds num, opcode, status. */
	const uint8_t *v = e.code == 0xff ? e.p + 2 : e.p + 6;
	uint32_t product = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
	unsigned patch = v[4] | (v[5] << 8), rom = v[6] | (v[7] << 8);
	uint32_t soc = (uint32_t)v[8] | ((uint32_t)v[9] << 8) | ((uint32_t)v[10] << 16) | ((uint32_t)v[11] << 24);
	printf(BT_TAG " SoC version: product_id=%#x soc_id=%#x rom_ver=%#x patch_ver=%#x (%s)\n",
	    product, soc, rom, patch, e.code == 0xff ? "vendor event" : "command complete");
	if (soc == 0 || rom == 0) {
		return "EDL version reply with soc_id or rom_ver 0";
	}

	static const uint8_t reset[] = {0x01, 0x03, 0x0c, 0x00};
	bt_cc_opcode = 0x0c03;
	if (!bt_command("tx HCI_Reset", reset, sizeof(reset), bt_is_complete, rx, &rx_n, 1000, &e)) {
		return "no HCI_Reset completion";
	}
	if (e.code != 0x0e || e.p[3] != 0) {
		printf(BT_TAG " HCI_Reset status=%#x (event %#x)\n", e.code == 0x0e ? e.p[3] : e.p[0], e.code);
		return "HCI_Reset failed";
	}
	printf(BT_TAG " HCI_Reset status=0\n");

	static const uint8_t version[] = {0x01, 0x01, 0x10, 0x00};
	bt_cc_opcode = 0x1001;
	if (!bt_command("tx Read_Local_Version", version, sizeof(version), bt_is_complete, rx, &rx_n,
	    500, &e)) {
		return "no Read_Local_Version completion";
	}
	if (e.code != 0x0e || e.len < 12 || e.p[3] != 0) {
		return "Read_Local_Version failed";
	}
	const uint8_t *q = e.p + 4;
	unsigned manufacturer = q[4] | (q[5] << 8);
	printf(BT_TAG " local version: hci=%u rev=%#x lmp=%u manufacturer=%u lmp_subver=%#x\n",
	    q[0], q[1] | (q[2] << 8), q[3], manufacturer, q[6] | (q[7] << 8));
	return manufacturer == 29 ? NULL : "manufacturer is not 29 (Qualcomm)";
}

/* ---- sequence ------------------------------------------------------------ */

static bool
bt_prepare(void)
{
	bt.gcc = ml_io_map(BT_GCC_PA, BT_GCC_SZ);
	bt.tlmm = ml_io_map(BT_TLMM_EAST_PA, BT_TLMM_EAST_SZ);
	bt.se = ml_io_map(BT_SE13_PA, BT_SE13_SZ);
	bt.qup = ml_io_map(BT_QUP2_PA, 0x1000);
	if (!bt.gcc || !bt.tlmm || !bt.se || !bt.qup) {
		printf(BT_TAG " MMIO mapping failed\n");
		return false;
	}
	bool pins_free = true;
	for (unsigned i = 0; i < 4; ++i) {
		bt.saved_pin[i] = bt_rd(bt.tlmm, BT_GPIO_CFG(BT_PIN_FIRST + i));
		pins_free &= ((bt.saved_pin[i] >> 2) & 0xfu) == 0;
	}
	vm_offset_t r = bt.gcc + BT_RCG;
	bt.saved_vote = bt_rd(bt.gcc, BT_GCC_VOTE);
	bt.saved_cfg = bt_rd(r, RCG_CFG);
	bt.saved_m = bt_rd(r, RCG_M);
	bt.saved_n = bt_rd(r, RCG_N);
	bt.saved_d = bt_rd(r, RCG_D);
	uint32_t dfs = bt_rd(r, RCG_DFSR);
	printf(BT_TAG " pins 43=%#x 44=%#x 45=%#x 46=%#x; GCC vote=%#x m_ahb=%#x s_ahb=%#x s3=%#x"
	    " rcg cmd=%#x cfg=%#x m=%#x n=%#x d=%#x dfs=%#x\n",
	    bt.saved_pin[0], bt.saved_pin[1], bt.saved_pin[2], bt.saved_pin[3], bt.saved_vote,
	    bt_rd(bt.gcc, BT_M_AHB), bt_rd(bt.gcc, BT_S_AHB), bt_rd(bt.gcc, BT_S3_CBCR),
	    bt_rd(r, RCG_CMD), bt.saved_cfg, bt.saved_m, bt.saved_n, bt.saved_d, dfs);
	if (!pins_free) {
		printf(BT_TAG " GPIO 43-46 are not in their boot GPIO function; someone owns them\n");
		return false;
	}
	if ((bt.saved_vote & BT_S3_BIT) || cepheus_branch_enabled(bt_rd(bt.gcc, BT_S3_CBCR), 0)) {
		printf(BT_TAG " SE13 clock already voted or running; another owner?\n");
		return false;
	}
	if (!cepheus_branch_enabled(bt_rd(bt.gcc, BT_M_AHB), 0) ||
	    !cepheus_branch_enabled(bt_rd(bt.gcc, BT_S_AHB), 1)) {
		printf(BT_TAG " QUP wrapper 2 AHB clocks off: not touching SE13\n");
		return false;
	}
	if (dfs & 1u) {
		printf(BT_TAG " SE13 RCG in DFS mode: not supported by this step\n");
		return false;
	}
	/* The only GCC vote write: our s3 bit, every other voter preserved. */
	bt_wr(bt.gcc, BT_GCC_VOTE, bt_rd(bt.gcc, BT_GCC_VOTE) | BT_S3_BIT);
	bt.vote_set = true;
	bool on = false;
	for (unsigned i = 0; i < 5000 && !on; ++i) {
		on = bt_s3_on();
		if (!on) {
			delay(1);
		}
	}
	printf(BT_TAG " s3 vote set: vote=%#x CBCR=%#x %s\n", bt_rd(bt.gcc, BT_GCC_VOTE),
	    bt_rd(bt.gcc, BT_S3_CBCR), on ? "on" : "STILL OFF");
	if (!on) {
		bt.se_dead = true;
		return false;
	}
	uint32_t fw = bt_rd(bt.se, GENI_FW_REV), hw0 = bt_rd(bt.se, SE_HW_PARAM_0);
	uint32_t hw1 = bt_rd(bt.se, SE_HW_PARAM_1), status = bt_rd(bt.se, GENI_STATUS);
	uint32_t qup = bt_rd(bt.qup, 4);
	bt.tx_depth = (hw0 >> 16) & 0x3fu;
	bt.rx_depth = (hw1 >> 16) & 0x3fu;
	bt.oversampling = qup >= BT_QUP_VER_2_5 ? 16u : 32u;
	printf(BT_TAG " SE13 fw=%#x proto=%u status=%#x if_disable=%#x tx=%ux%u rx=%ux%u"
	    " ios=%#x qup=%#x oversampling=%u\n", fw, (fw >> 8) & 0xffu, status,
	    bt_rd(bt.se, GENI_IF_DISABLE), (hw0 >> 24) & 0x3fu, bt.tx_depth,
	    (hw1 >> 24) & 0x3fu, bt.rx_depth, bt_rd(bt.se, GENI_IOS), qup, bt.oversampling);
	if (((fw >> 8) & 0xffu) != 2u) {
		printf(BT_TAG " SE13 firmware is not UART (proto 2)\n");
		return false;
	}
	if ((status & ((1u << 0) | (1u << 12))) || (bt_rd(bt.se, GENI_IF_DISABLE) & 1u)) {
		printf(BT_TAG " SE13 busy or FIFO interface disabled\n");
		return false;
	}
	if (((hw0 >> 24) & 0x3fu) != 32u || ((hw1 >> 24) & 0x3fu) != 32u ||
	    bt.tx_depth < 4u || bt.rx_depth < 4u) {
		printf(BT_TAG " unexpected SE13 FIFO geometry\n");
		return false;
	}
	return true;
}

/* hci_qca.c qca_power_shutdown order: 0xC0 at 2400, rails off; then ours. */
static void
bt_power_down(void)
{
	printf(BT_TAG " powering down (rails=%#x pulsed=%d)\n", bt.rails, bt.chip_pulsed);
	bool se_ok = bt.vote_set && !bt.se_dead && bt_clock_ok("power-down");
	if (bt.chip_pulsed && se_ok && bt.pins_set) {
		bool ok = (bt_rd(bt.gcc, BT_RCG + RCG_CFG) & RCG_CFG_MASK) <= 1u ||
		    bt_rcg_program(RCG_SRC_XO, false, 0, 0);
		ok = ok && bt_uart_baud(2400, BT_XO_HZ, false);
		if (ok) {
			(void)bt_pulse(0xc0, 2400);
		}
		bt_wait(10);
	}
	for (int i = 2; i >= 0; --i) {
		if (bt.rails & (1u << i)) {
			int err = bt_rail((unsigned)i, false);
			if (err) {
				printf(BT_TAG " %s could not be voted off (err=%d): it stays on\n",
				    bt_rail_name[i], err);
			}
		}
	}
	if (bt.uart_set && se_ok && !bt.se_dead) {
		(void)bt_rx_stop();
		bt_wr(bt.se, UART_MANUAL_RFR, 0);
		bt_wr(bt.se, GENI_M_IRQ_CLEAR, 0xffffffffu);
		bt_wr(bt.se, GENI_S_IRQ_CLEAR, 0xffffffffu);
	}
	if (bt.pins_set) {
		for (unsigned i = 0; i < 4; ++i) {
			bt_wr(bt.tlmm, BT_GPIO_CFG(BT_PIN_FIRST + i), bt.saved_pin[i]);
		}
		bt.pins_set = false;
	}
	if (bt.rcg_changed && bt.vote_set) {
		(void)bt_rcg_restore();
	}
	if (bt.vote_set && !(bt.saved_vote & BT_S3_BIT)) {
		bt_wr(bt.gcc, BT_GCC_VOTE, bt_rd(bt.gcc, BT_GCC_VOTE) & ~BT_S3_BIT);
		bt.vote_set = false;
	}
	printf(BT_TAG " down: rails=%#x (0 = all voted off) pins 43-46=%#x/%#x/%#x/%#x vote=%#x rcg cfg=%#x\n",
	    bt.rails, bt_rd(bt.tlmm, BT_GPIO_CFG(43)), bt_rd(bt.tlmm, BT_GPIO_CFG(44)),
	    bt_rd(bt.tlmm, BT_GPIO_CFG(45)), bt_rd(bt.tlmm, BT_GPIO_CFG(46)),
	    bt_rd(bt.gcc, BT_GCC_VOTE), bt_rd(bt.gcc, BT_RCG + RCG_CFG));
}

static const char *
bt_sequence(void)
{
	uint8_t rx[BT_RX_MAX];
	unsigned rx_n;

	if (!bt_prepare()) {
		return "preconditions";
	}
	vm_offset_t r = bt.gcc + BT_RCG;
	/* 2400 baud from XO/1 (the boot configuration of this RCG). */
	if ((bt.saved_cfg & RCG_CFG_MASK) > 1u && !bt_rcg_program(RCG_SRC_XO, false, 0, 0)) {
		return "RCG XO/1";
	}
	if (!bt_clock_ok("setup") || !bt_uart_setup() || !bt_uart_baud(2400, BT_XO_HZ, false)) {
		return "UART set-up";
	}
	for (unsigned i = 0; i < 3; ++i) {
		int err = bt_rail(i, true);
		if (err) {
			printf(BT_TAG " %s on failed err=%d\n", bt_rail_name[i], err);
			return "RPMh rail vote";
		}
	}
	printf(BT_TAG " rails on: ldoa7 1800 mV, ldoc2 1304 mV, ldoc11 3312 mV (HPM)\n");
	bt_wait(20);
	if (!bt_pins_qup13()) {
		return "pin mux";
	}
	bt_wait(10);
	rx_n = bt_rx_read(rx, 0, BT_RX_MAX);
	if (rx_n) {
		bt_hex("rx before pulses", rx, rx_n);
	}
	/* hci_qca.c: off pulse at 2400 forces boot mode, then on pulse at 115200. */
	bt.chip_pulsed = true;
	if (!bt_pulse(0xc0, 2400)) {
		return "0xC0 pulse";
	}
	bt_wait(10);
	unsigned rate = 7372800u;
	if (bt.xo_mnd) {
		printf(BT_TAG " 115200 from XO * 48/125 (andrbtxo=1)\n");
		if (!bt_rcg_program(RCG_SRC_XO, true, 48, 125)) {
			return "RCG XO M/N";
		}
	} else {
		if (!bt_gpll0_even_ok()) {
			return "GPLL0 even output";
		}
		if (!bt_rcg_program(RCG_SRC_GPLL0_EVEN, true, 384, 15625)) {
			return "RCG GPLL0_EVEN M/N";
		}
	}
	printf(BT_TAG " RCG now cmd=%#x cfg=%#x m=%#x n=%#x d=%#x\n", bt_rd(r, RCG_CMD),
	    bt_rd(r, RCG_CFG), bt_rd(r, RCG_M), bt_rd(r, RCG_N), bt_rd(r, RCG_D));
	if (!bt_clock_ok("115200") || !bt_uart_baud(115200, rate, false)) {
		return "115200 baud";
	}
	if (!bt_pulse(0xfc, 115200)) {
		return "0xFC pulse";
	}
	bt_wait(100);               /* hci_qca.c: give the controller time to boot */
	/* Linux reopens the port here; drain and log what arrived meanwhile. */
	rx_n = bt_rx_read(rx, 0, BT_RX_MAX);
	bt_hex("rx after power-on", rx, rx_n);
	if (!bt_uart_baud(115200, rate, bt.flow)) {
		return "flow control";
	}
	uint32_t ios = bt_rd(bt.se, GENI_IOS);
	printf(BT_TAG " lines: RX %s, CTS %s (ios=%#x)\n", (ios & 1u) ? "high (idle)" : "LOW",
	    (ios & 2u) ? "deasserted" : "asserted", ios);
	return bt_hci();
}

static void
bt_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	printf(BT_TAG " B1 starting in %u s (andrbt=1, %s, flow control %s)\n", bt_delay_s,
	    bt.xo_mnd ? "115200 from XO M/N" : "115200 from GPLL0_EVEN M/N", bt.flow ? "on" : "off");
	bt_wait(bt_delay_s * 1000u);
	printf(BT_TAG " begin\n");
	const char *failed = bt_sequence();
	if (failed) {
		printf(BT_TAG " B1 FAIL at: %s\n", failed);
	} else {
		printf(BT_TAG " B1 PASS: WCN3990 answered EDL version, HCI_Reset and Read_Local_Version\n");
	}
	bt_power_down();
	printf(BT_TAG " done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
andromeda_bt_init(void)
{
	static bool started;
	int level = 0;
	unsigned value;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrbt", &level, sizeof(level));
	if (level != 1) {
		if (level) {
			printf(BT_TAG " andrbt=%d not supported (only 1)\n", level);
		}
		return;
	}
	(void)PE_parse_boot_argn("andrbtdelay", &bt_delay_s, sizeof(bt_delay_s));
	value = 0;
	(void)PE_parse_boot_argn("andrbtxo", &value, sizeof(value));
	bt.xo_mnd = value != 0;
	value = 1;
	(void)PE_parse_boot_argn("andrbtflow", &value, sizeof(value));
	bt.flow = value != 0;
	/* andrprobe=2 toggles the same s3 vote bit: keep the two apart in time. */
	int probe = 0;
	unsigned probe_delay = 100;
	(void)PE_parse_boot_argn("andrprobe", &probe, sizeof(probe));
	(void)PE_parse_boot_argn("andrprobedelay", &probe_delay, sizeof(probe_delay));
	if (probe >= 2 && bt_delay_s + 20u > probe_delay && bt_delay_s < probe_delay + 30u) {
		printf(BT_TAG " refusing: andrprobe=2 at %u s votes the same SE13 clock; use andrbtdelay >= %u\n",
		    probe_delay, probe_delay + 30u);
		return;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf(BT_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
