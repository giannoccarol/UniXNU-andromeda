/*
 * Mi Mix 3 5G (andromeda, SM8150): panel brightness over DSI0, DCS 0x51.
 * Off unless the boot-arg andrbl is set.
 *
 * The bootloader/UEFI leaves the Samsung EA8076 command-mode panel running on
 * DSI0 (DSI 6G v2.3, live DT dsi-ctrl-0 @0xae94000), with MDP autorefresh
 * pushing the splash framebuffer. This file only adds one DCS long write per
 * request through the controller's command DMA path, as Linux drm/msm
 * dsi_host.c does (msm_dsi_host_xfer_prepare, dsi_cmd_dma_add,
 * dsi_cmd_dma_tx, msm_dsi_host_xfer_restore; register layout from dsi.xml.h
 * with the 6G +4 shift; used as documentation only, GPL-2.0, nothing
 * copied). It never resets the controller, never touches the PHY, clocks or
 * the MDP, and restores every DSI register it changes.
 *
 * Panel facts (live DT qcom,mdss_dsi_samsung_fhd_ea8076_cmd, the active
 * dsi_samsung_fhd_ea8076_cmd_display): bl_ctrl_dcs with
 * qcom,mdss-dsi-bl-dcs-type-ss (two bytes, MSB first, as the panel on-command
 * "39 51 00 00"), levels 2..0x7ff (bl-min-level, bl-max-level). Peak
 * brightness 430 nits (qcom,mdss-dsi-panel-peak-brightness 4300000).
 *
 * The command packet lives in the last 4 KiB page of the UEFI "Display
 * Reserved" region (0x9C000000 + 0x2400000, Project Aloha's andromeda memory
 * map and the live DT cont_splash_region): memory the display engine already
 * reads, outside XNU's RAM, and past the 1080x2340x4 framebuffer. It is mapped
 * as device memory, so no cache maintenance is needed.
 *
 * Boot-args:
 *   andrbl=1         test, andrbldelay s after start (default 30): log the DSI0
 *                    state, set andrbllevel, 10 s later andrbllevel2, 10 s
 *                    later andrbllevel again. No IOKit service.
 *   andrbl=2         set andrbllevel once; if that worked, publish the
 *                    AppleARMBacklight service (AndromedaBacklight.cpp,
 *                    backlight-control) and send what CoreBrightness asks.
 *   andrbllevel=N    first level, 2..2047 (default 1023)
 *   andrbllevel2=N   second test level (default 256)
 *   andrblhs=1       send in high-speed mode (default: the LP/HS choice the
 *                    UEFI left in CMD_DMA_CTRL)
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

extern vm_offset_t andromeda_io_map(vm_offset_t, vm_size_t); /* andromeda_probe.c: shared, outside io_submap */
extern kern_return_t thread_terminate(thread_t);
/* iokit/Kernel/arm/AndromedaBacklight.cpp (patch 0093) */
extern int andromeda_backlight_publish(unsigned level, unsigned max_level, unsigned max_nits);
void andromeda_backlight_init(void);
int andromeda_backlight_request(unsigned level);

#define BL_TAG "ANDROMEDA BL:"

#define BL_DSI0_PA       0x0ae94000u   /* live DT dsi-ctrl-0 "dsi_ctrl", 0x400 */
#define BL_BUF_PA        0x9e3ff000u   /* last page of 0x9C000000 + 0x2400000 */
#define BL_FB_END        (0x9c000000u + 1080u * 2340u * 4u)

/* DSI 6G registers (dsi.xml.h offset + 4). */
#define DSI_HW_VERSION   0x000u
#define DSI_CTRL         0x004u        /* 0 ENABLE, 1 VID_MODE_EN, 2 CMD_MODE_EN */
#define DSI_STATUS       0x008u        /* 1 CMD_MODE_DMA_BUSY, 2 MDP_BUSY, 31 contention */
#define DSI_CMD_DMA_CTRL 0x03cu        /* 26 LOW_POWER, 28 FROM_FRAME_BUFFER */
#define DSI_DMA_BASE     0x048u
#define DSI_DMA_LEN      0x04cu
#define DSI_ACK_ERR      0x068u
#define DSI_TRIG_CTRL    0x084u        /* 2:0 DMA trigger (4 = SW), 12 BLOCK_DMA_WITHIN_FRAME */
#define DSI_TRIG_DMA     0x090u
#define DSI_LANE_STATUS  0x0a8u
#define DSI_DLN0_PHY_ERR 0x0b4u
#define DSI_TIMEOUT_STS  0x0c0u
#define DSI_INTR_CTRL    0x110u        /* status/mask pairs; 0 CMD_DMA_DONE, 24 ERROR */
#define DSI_CLK_CTRL     0x11cu
#define DSI_INTR_MASKS   0x02220202u   /* the enable ("MASK") bits: never changed */
#define DSI_CTRL_ENABLE  (1u << 0)
#define DSI_CTRL_VID     (1u << 1)
#define DSI_CTRL_CMD     (1u << 2)
#define DMA_LOW_POWER    (1u << 26)
#define DMA_FROM_FB      (1u << 28)
#define TRIG_SW          4u
#define TRIG_BLOCK_FRAME (1u << 12)

#define BL_MIN           2u
#define BL_MAX           0x7ffu
#define BL_MAX_NITS      430u

static vm_offset_t bl_dsi, bl_buf;
static int bl_mode;
static bool bl_hs, bl_dead, bl_ready;
static unsigned bl_level1 = 1023, bl_level2 = 256;
static volatile uint32_t bl_pending;    /* bit 31: new level in bits 15:0 */
static unsigned bl_sleep_chan;
static unsigned bl_sends, bl_fails;

static void
bl_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&bl_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
bl_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
bl_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
bl_state(const char *when)
{
	printf(BL_TAG " %s: ver=%#x ctrl=%#x status=%#x dma_ctrl=%#x dma=%#x+%#x trig=%#x intr=%#x"
	    " ack_err=%#x timeout=%#x phy_err=%#x lanes=%#x clk_ctrl=%#x\n", when,
	    bl_rd(bl_dsi, DSI_HW_VERSION), bl_rd(bl_dsi, DSI_CTRL), bl_rd(bl_dsi, DSI_STATUS),
	    bl_rd(bl_dsi, DSI_CMD_DMA_CTRL), bl_rd(bl_dsi, DSI_DMA_BASE), bl_rd(bl_dsi, DSI_DMA_LEN),
	    bl_rd(bl_dsi, DSI_TRIG_CTRL), bl_rd(bl_dsi, DSI_INTR_CTRL), bl_rd(bl_dsi, DSI_ACK_ERR),
	    bl_rd(bl_dsi, DSI_TIMEOUT_STS), bl_rd(bl_dsi, DSI_DLN0_PHY_ERR),
	    bl_rd(bl_dsi, DSI_LANE_STATUS), bl_rd(bl_dsi, DSI_CLK_CTRL));
}

/* Read only: is DSI0 the running command-mode link this code expects? */
static bool
bl_probe(void)
{
	bl_dsi = andromeda_io_map(BL_DSI0_PA, 0x1000);
	bl_buf = andromeda_io_map(BL_BUF_PA, 0x1000);
	if (!bl_dsi || !bl_buf) {
		printf(BL_TAG " MMIO mapping failed\n");
		return false;
	}
	bl_state("inherited");
	uint32_t ver = bl_rd(bl_dsi, DSI_HW_VERSION), ctrl = bl_rd(bl_dsi, DSI_CTRL);
	if ((ver & 0xffff0000u) != 0x20030000u) {
		printf(BL_TAG " DSI0 version %#x is not 6G v2.3 (SM8150); not sending\n", ver);
		return false;
	}
	if ((ctrl & (DSI_CTRL_ENABLE | DSI_CTRL_CMD | DSI_CTRL_VID)) != (DSI_CTRL_ENABLE | DSI_CTRL_CMD)) {
		printf(BL_TAG " DSI0 is not an enabled command-mode link (ctrl=%#x); not sending\n", ctrl);
		return false;
	}
	if (BL_BUF_PA < BL_FB_END) {
		printf(BL_TAG " packet page overlaps the framebuffer; not sending\n");
		return false;
	}
	return true;
}

/*
 * One DCS long write (data type 0x39, virtual channel 0) through command
 * DMA. Memory format of dsi_cmd_dma_add: word count low/high, data type,
 * flags (bit 7 last packet, bit 6 long), payload, 0xff padding to 4 bytes.
 */
static int
bl_dcs_long(const uint8_t *payload, unsigned len)
{
	if (bl_dead || !bl_dsi || !bl_buf || len == 0 || len > 16) {
		return EINVAL;
	}
	uint8_t pkt[20];
	unsigned size = (4u + len + 3u) & ~3u;
	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = (uint8_t)len;
	pkt[1] = 0;
	pkt[2] = 0x39;
	pkt[3] = 0xc0;
	memcpy(pkt + 4, payload, len);
	for (unsigned i = 0; i < size; i += 4) {
		uint32_t word = (uint32_t)pkt[i] | ((uint32_t)pkt[i + 1] << 8) |
		    ((uint32_t)pkt[i + 2] << 16) | ((uint32_t)pkt[i + 3] << 24);
		bl_wr(bl_buf, i, word);
	}
	/* The DMA engine must be idle (another command, or the UEFI's last one). */
	bool idle = false;
	for (unsigned i = 0; i < 200 && !idle; ++i) {
		idle = !(bl_rd(bl_dsi, DSI_STATUS) & 2u);
		if (!idle) {
			delay(100);
		}
	}
	if (!idle) {
		printf(BL_TAG " command DMA busy (status=%#x); not sending\n", bl_rd(bl_dsi, DSI_STATUS));
		return EBUSY;
	}
	uint32_t trig = bl_rd(bl_dsi, DSI_TRIG_CTRL), dma = bl_rd(bl_dsi, DSI_CMD_DMA_CTRL);
	uint32_t want_trig = (trig & ~7u) | TRIG_SW | TRIG_BLOCK_FRAME;
	uint32_t want_dma = (dma | DMA_FROM_FB) & ~(bl_hs ? DMA_LOW_POWER : 0u);
	if (want_trig != trig) {
		bl_wr(bl_dsi, DSI_TRIG_CTRL, want_trig);
	}
	if (want_dma != dma) {
		bl_wr(bl_dsi, DSI_CMD_DMA_CTRL, want_dma);
	}
	/* Acknowledge only CMD_DMA_DONE; leave every interrupt enable as it is. */
	bl_wr(bl_dsi, DSI_INTR_CTRL, (bl_rd(bl_dsi, DSI_INTR_CTRL) & DSI_INTR_MASKS) | 1u);
	bl_wr(bl_dsi, DSI_DMA_BASE, BL_BUF_PA);
	bl_wr(bl_dsi, DSI_DMA_LEN, size);
	bl_wr(bl_dsi, DSI_TRIG_DMA, 1u);
	bool done = false, busy_seen = false;
	uint32_t intr = 0, status = 0;
	for (unsigned i = 0; i < 500 && !done; ++i) {   /* 50 ms: three frames */
		intr = bl_rd(bl_dsi, DSI_INTR_CTRL);
		status = bl_rd(bl_dsi, DSI_STATUS);
		busy_seen |= (status & 2u) != 0;
		done = (intr & 1u) != 0;
		if (!done) {
			delay(100);
		}
	}
	bool idle_after = !(bl_rd(bl_dsi, DSI_STATUS) & 2u);
	if (done) {
		bl_wr(bl_dsi, DSI_INTR_CTRL, (bl_rd(bl_dsi, DSI_INTR_CTRL) & DSI_INTR_MASKS) | 1u);
	}
	if (want_dma != dma) {
		bl_wr(bl_dsi, DSI_CMD_DMA_CTRL, dma);
	}
	if (want_trig != trig) {
		bl_wr(bl_dsi, DSI_TRIG_CTRL, trig);
	}
	uint32_t ack = bl_rd(bl_dsi, DSI_ACK_ERR), tmo = bl_rd(bl_dsi, DSI_TIMEOUT_STS);
	bool error = (intr & (1u << 24)) || (status & (1u << 31));
	++bl_sends;
	if (!done || error || bl_sends <= 4) {
		printf(BL_TAG " DCS %02x len=%u %s: intr=%#x status=%#x busy-seen=%d idle=%d ack_err=%#x"
		    " timeout=%#x %s mode\n", payload[0], len,
		    done ? (error ? "DONE WITH ERROR" : "done") : "NO DMA DONE",
		    intr, status, busy_seen, idle_after, ack, tmo,
		    (want_dma & DMA_LOW_POWER) ? "LP" : "HS");
	}
	if (!done && !idle_after) {
		printf(BL_TAG " command DMA stuck; no further DCS this boot\n");
		bl_dead = true;
	}
	if (!done || error) {
		++bl_fails;
		return EIO;
	}
	return 0;
}

static int
bl_set(unsigned level)
{
	if (level < BL_MIN) { level = BL_MIN; }
	if (level > BL_MAX) { level = BL_MAX; }
	const uint8_t cmd[3] = {0x51, (uint8_t)(level >> 8), (uint8_t)level};   /* MSB first */
	int err = bl_dcs_long(cmd, sizeof(cmd));
	if (err || bl_sends <= 4 || (bl_sends % 100u) == 0) {
		printf(BL_TAG " level %u (%#x) -> %s (sent %u, failed %u)\n", level, level,
		    err ? "FAILED" : "ok", bl_sends, bl_fails);
	}
	return err;
}

/* For AndromedaBacklight.cpp: never blocks; the newest level wins. */
int
andromeda_backlight_request(unsigned level)
{
	if (!bl_ready || bl_dead) {
		return ENXIO;
	}
	if (level > BL_MAX) {
		level = BL_MAX;
	}
	__atomic_store_n(&bl_pending, (1u << 31) | level, __ATOMIC_RELEASE);
	return 0;
}

static void
bl_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	unsigned delay_s = 30;
	(void)PE_parse_boot_argn("andrbldelay", &delay_s, sizeof(delay_s));
	printf(BL_TAG " andrbl=%d, starting in %u s (levels %u/%u, %s)\n", bl_mode, delay_s,
	    bl_level1, bl_level2, bl_hs ? "HS" : "inherited LP/HS");
	bl_wait(delay_s * 1000u);
	if (!bl_probe()) {
		printf(BL_TAG " not available\n");
		goto out;
	}
	if (bl_set(bl_level1)) {
		bl_state("after failed DCS");
		goto out;
	}
	if (bl_mode == 1) {
		bl_wait(10000);
		(void)bl_set(bl_level2);
		bl_wait(10000);
		(void)bl_set(bl_level1);
		bl_state("test done");
		printf(BL_TAG " test done: %u DCS, %u failed (look for %u -> %u -> %u on the panel)\n",
		    bl_sends, bl_fails, bl_level1, bl_level2, bl_level1);
		goto out;
	}
	bl_ready = true;
	if (andromeda_backlight_publish(bl_level1, BL_MAX, BL_MAX_NITS)) {
		printf(BL_TAG " AppleARMBacklight not published; DCS sender idle\n");
		bl_ready = false;
		goto out;
	}
	printf(BL_TAG " AppleARMBacklight published (backlight-control); serving requests\n");
	unsigned last = bl_level1;
	while (!bl_dead) {
		uint32_t request = __atomic_exchange_n(&bl_pending, 0u, __ATOMIC_ACQ_REL);
		if ((request >> 31) && (request & 0xffffu) != last) {
			last = request & 0xffffu;
			(void)bl_set(last);
		}
		bl_wait(33);    /* two frames: a fade is coalesced to its newest level */
	}
	bl_ready = false;
	printf(BL_TAG " sender stopped after an error\n");
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
andromeda_backlight_init(void)
{
	static bool started;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("andrbl", &bl_mode, sizeof(bl_mode));
	if (bl_mode != 1 && bl_mode != 2) {
		if (bl_mode) {
			printf(BL_TAG " andrbl=%d not supported (1 test, 2 service)\n", bl_mode);
		}
		return;
	}
	unsigned hs = 0;
	(void)PE_parse_boot_argn("andrbllevel", &bl_level1, sizeof(bl_level1));
	(void)PE_parse_boot_argn("andrbllevel2", &bl_level2, sizeof(bl_level2));
	(void)PE_parse_boot_argn("andrblhs", &hs, sizeof(hs));
	bl_hs = hs != 0;
	if (bl_level1 < BL_MIN || bl_level1 > BL_MAX || bl_level2 < BL_MIN || bl_level2 > BL_MAX) {
		printf(BL_TAG " levels must be %u..%u; refusing\n", BL_MIN, BL_MAX);
		return;
	}
	if (kernel_thread_start(bl_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf(BL_TAG " kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
