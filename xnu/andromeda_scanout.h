/*
 * andromeda: the display reads iOS's frames itself (patch 0207).
 *
 * Until now every frame backboardd swaps was copied by the CPU into the
 * ABL's panel memory (0x9c000000), which VIG0 of the SM8150 display
 * controller (DPU 5.0) scans out: 1125x2436 scaled to 1080x2338, about
 * 11 MB read and 10 MB written per frame (patch 0094 fit; 2026-11-08, Home
 * pages: 25 frames/s made, copy the largest part of each frame's CPU time).
 * Here the pipe is pointed at the IOSurface itself at every swap, and its
 * QSEED3 scaler does the fit, as an iPhone's display pipe takes the frame
 * buffer CA hands it.
 *
 * The design is ShueiTsuki's 0217-pandora-scanout of data-a-live
 * (https://github.com/ShueiTsuki/data-a-live, commit d2c7089, APSL 2.0), for
 * the SM8750's DPU 12: the display stream gets page tables of its own in its
 * SMMU context bank (the ABL leaves translation off, so the pipe could read
 * only 32-bit physical addresses), the panel memory mapped at its own address,
 * surfaces in IOVA slots; the pipe's address, stride and scaler are written
 * at every swap, then a CTL flush; command mode with autorefresh takes the
 * flushed registers at the next frame. Adapted here to DPU 5.0 (one pipe, the
 * SSPP layout before v13, QSEED3 at pipe + 0xa00, tear check and autorefresh
 * in INTF1) from the register definitions of Linux drm/msm dpu1
 * (dpu_hw_sspp.c, dpu_hw_util.c, dpu_hw_ctl.c, dpu_hw_intf.c, catalog
 * dpu_5_0_sm8150.h). See CREDITS.md.
 *
 * Shared by bsd/dev/andromeda_scanout.c (the hardware, C) and
 * iokit/Kernel/arm/SweetFramebuffer.cpp (which frame, C++);
 * 0207-stage-andromeda-scanout.sh installs this file in both directories.
 *
 * Boot-arg andrscanout=N (parsed by andromeda_scanout.c), also set at run
 * time by "andctl scanout N":
 *   0  CPU copy, as before (default)
 *   1  scan out every frame the pipe can take; any other frame is copied
 *   2  as 1, and the CPU caches of the frame are cleaned first (for frames
 *      the CPU may have drawn; Metal frames are in memory already)
 *   3  as 1, but the surfaces the display holds are not counted as in use
 *      (IOSurfaceIsInUse): to see whether Core Animation waits for them
 * The first frame with mode 1/2 sets the hardware up: a stream match or bank
 * not as expected, or any SMMU fault in the first 50 ms, leaves the CPU copy
 * for the rest of the boot. Nothing is written before that frame.
 */
#ifndef ANDROMEDA_SCANOUT_H
#define ANDROMEDA_SCANOUT_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One frame for the pipe: source rectangle in the surface, output rectangle on the panel. */
struct andr_scan_frame {
	uint32_t iova;          /* first byte of the surface for the display (slot + offset) */
	uint32_t stride;        /* bytes per row */
	uint32_t src_x, src_y, src_w, src_h;
	uint32_t dst_x, dst_y, dst_w, dst_h;
};

/* andrscanout / "andctl scanout N": 0 to 3 (see above). */
unsigned andromeda_scanout_mode(void);
int andromeda_scanout_ctl(unsigned mode, char *why, size_t len);

/* Set the display stream up for scanout (once; thread context). 0 = ready. */
int andromeda_scanout_init(uint64_t panel_pa, uint32_t panel_rowbytes, uint32_t panel_w, uint32_t panel_h);
bool andromeda_scanout_ready(void);

/* Map NPAGES 4 KiB pages (physical addresses) into a free IOVA slot: its IOVA, or 0. */
uint32_t andromeda_scanout_map(const uint64_t *pages, uint32_t npages);
void andromeda_scanout_unmap(uint32_t iova, uint32_t npages);

/*
 * Give the pipe a frame (0), or back the ABL's panel memory (panel). Both
 * wait until the previous flush has been taken (at most one frame) and
 * return EBUSY when it was not.
 */
int andromeda_scanout_show(const struct andr_scan_frame *f);
int andromeda_scanout_panel(void);

/* The last flush has been taken: the pipe reads what it was given last. */
bool andromeda_scanout_latched(void);

/* Frames shown, flushes that had to wait, SMMU faults seen (fault status read and cleared here). */
void andromeda_scanout_counts(uint64_t *shows, uint64_t *waits, uint64_t *faults);

/*
 * Frames the framebuffer copies into the panel memory instead (andctl shot
 * reads the panel memory): set by andromeda_scanout_copy_frames(), taken one
 * by one by the framebuffer (andromeda_scanout_take_copy()).
 */
void andromeda_scanout_copy_frames(unsigned n);
bool andromeda_scanout_take_copy(void);
bool andromeda_scanout_copy_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* ANDROMEDA_SCANOUT_H */
