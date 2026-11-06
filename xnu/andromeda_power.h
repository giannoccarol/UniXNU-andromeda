/*
 * Mi Mix 3 5G (andromeda, SM8150): state shared by the power patches
 * 0100-0105 (docs/research/sospensione.md, phases P0 and P1). Everything is
 * defined in andromeda_power.c (patch 0100); include this after
 * <sys/param.h>. The C++ framebuffer (iokit/Kernel/arm/SweetFramebuffer.cpp,
 * patch 0102) and osfmk/arm64/cpu.c (patch 0101) cannot include a bsd/dev
 * header and declare the few symbols they use themselves: keep them in step.
 */
#ifndef ANDROMEDA_POWER_H
#define ANDROMEDA_POWER_H

/*
 * Display state, 1 = off (backboardd's RequestPowerChange(0)), set by
 * AppleCLCD through andromeda_display_set(). Read it with __atomic_load_n.
 */
extern int andromeda_display_off;
void andromeda_display_set(int off);
/*
 * Sleep up to ms milliseconds, woken early when the display turns on: the
 * slow display-off polling of keys (0104) and touch (0105) resumes at once.
 */
void andromeda_display_wait(unsigned ms);

/* Loop counters of the polled drivers; one writer each, relaxed atomics. */
extern uint64_t andromeda_pwr_fb_vsync;    /* AppleCLCD vsync timer ticks (0102) */
extern uint64_t andromeda_pwr_fb_notify;   /* vsync notifications to userland (0102) */
extern uint64_t andromeda_pwr_usb_polls;   /* sweet_usb.c event polls (0103) */
extern uint64_t andromeda_pwr_usb_slow;    /* ... of them slow, andrusbidle (0103) */
extern uint64_t andromeda_pwr_keys_polls;  /* sweet_keys.c PON reads (0104) */
extern uint64_t andromeda_pwr_touch_polls; /* cepheus_touch.c FIFO reads (0105) */

/*
 * Per-CPU idle counters, written only by cpu_idle() on its own CPU with
 * interrupts masked, while andromeda_pwr_measure is set (andrpwr=1).
 * ANDROMEDA_PWR_IDLE_STRIDE words (one 64-byte line) per CPU id.
 */
#define ANDROMEDA_PWR_CPUS            8
#define ANDROMEDA_PWR_IDLE_STRIDE     8
#define ANDROMEDA_PWR_IDLE_WFI        0  /* WFI entries (= wakeups from idle) */
#define ANDROMEDA_PWR_IDLE_WFI_ABS    1  /* time in WFI, Mach absolute units */
#define ANDROMEDA_PWR_IDLE_LONG_ABS   2  /* time in WFI periods >= andromeda_pwr_long_abs */
#define ANDROMEDA_PWR_IDLE_FLUSH      3  /* platform_cache_idle_enter() calls */
#define ANDROMEDA_PWR_IDLE_FLUSH_ABS  4  /* time spent in them */
#define ANDROMEDA_PWR_IDLE_FLUSH_MAX  5  /* longest since the last line (the reader zeroes it) */
extern uint64_t andromeda_pwr_idle[ANDROMEDA_PWR_CPUS * ANDROMEDA_PWR_IDLE_STRIDE];
extern int andromeda_pwr_measure;
extern uint64_t andromeda_pwr_long_abs;

void andromeda_power_init(void);

#endif /* ANDROMEDA_POWER_H */
