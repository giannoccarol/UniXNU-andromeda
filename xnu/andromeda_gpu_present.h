/*
 * Phase F4 on andromeda: the Adreno 640 copies each presented frame (the
 * IOSurface backboardd swaps) onto the panel, with the same fit as the CPU
 * copy of patch 0094 (sweetfb_fit=1: 1125x2436 -> 1080x2338, scale 1080/1125),
 * and the CPU copy stays the fallback for every frame the GPU cannot do.
 *
 * Shared by bsd/dev/andromeda_gpu_present.c (the GPU side, C) and
 * iokit/Kernel/arm/SweetFramebuffer.cpp (patch 0120, the hook, C++); the
 * staging script 0120-stage-andromeda-gpu-present.sh installs this same file
 * in both directories.
 *
 * The idea and the register stream are ios-flame's GPU present path (Nick
 * Wylynko / NWylynko, https://github.com/NWylynko/ios-flame, patches
 * 0095-flame-gpu-present, 0111-flame-gpu-display-mask, 0119-flame-gpu-async-
 * present; MIT, patched XNU files APSL 2.0). See andromeda_gpu_present.c and
 * CREDITS.md.
 *
 * Boot-args (all parsed by andromeda_gpu_present.c):
 *   andrgpupresent=1        use the GPU for the panel copy once andrgpu=4
 *                           has passed its CP_MEM_WRITE test in this boot
 *                           (needs sweetfb_fit=1). 0/absent: CPU copy only.
 *   andrgpupresentcheck=N   compare the first N GPU frames with the CPU
 *                           result (CRC-32 of both, pixel by pixel; default
 *                           8, 0..64). A real difference turns the GPU path
 *                           off for the rest of the boot.
 *   andrgpupresentevery=M   then check one GPU frame in M (default 1000,
 *                           0 = never).
 *   andrgpuhot=C            (andromeda_gpu.c's limit, default 80) the
 *                           present path re-reads the GPU temperature every
 *                           10 s and stops at/above C degC or when unreadable.
 */
#ifndef ANDROMEDA_GPU_PRESENT_H
#define ANDROMEDA_GPU_PRESENT_H
#ifdef KERNEL
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Surfaces the GPU keeps mapped at once (backboardd rotates three). */
#define ANDR_PRESENT_SLOTS		4u
/* Largest surface memory one slot maps (1125x2436 BGRA at 4608 B/row: 11.2 MB). */
#define ANDR_PRESENT_SLOT_BYTES		0x1000000ull

#ifdef __cplusplus
extern "C" {
#endif

struct andr_gpu;

/* The panel and the fit, as SweetFramebuffer.cpp (0094) computes them. */
struct andr_present_geom {
	uint64_t panel_pa;		/* panel memory: the boot framebuffer */
	uintptr_t panel_kva;		/* its kernel mapping (write-combining) */
	uint32_t panel_w, panel_h, panel_rowbytes;
	uint32_t logical_w, logical_h;	/* the display iOS renders (1125x2436) */
	uint32_t fit_w, fit_h, fit_y;	/* fitted image: columns 0..fit_w-1, rows fit_y.. */
	const uint16_t *fit_col;	/* panel column -> logical column (fit_w entries) */
	const uint16_t *fit_row;	/* fitted row -> logical row (fit_h entries) */
};

/* One frame: a full-display BGRA (4 bytes per pixel) layer. */
struct andr_present_src {
	uintptr_t kva;			/* kernel mapping of the surface memory (cacheable) */
	uint64_t len;			/* bytes of that memory */
	uint32_t offset;		/* first pixel */
	uint32_t width, height, bytes_per_row;
	int console_quiet;		/* sweetfb_noconsole: only frames write the panel */
};

/*
 * From andromeda_gpu.c, once step 4 (CP_MEM_WRITE) has passed: the GPU is
 * handed over and the bring-up thread does not touch it again. Does nothing
 * unless andrgpupresent=1.
 */
void andromeda_gpu_present_attach(struct andr_gpu *g, uint64_t pool_size);
/*
 * From andromeda_gpu.c instead of attach, when another user has the GPU's
 * ring this boot (Metal, andrmtl=N: one ring user at a time). A requested
 * present path (andrgpupresent=1) is turned off for the boot with why in the
 * log; the CPU copy carries on. Does nothing unless andrgpupresent=1.
 */
void andromeda_gpu_present_decline(const char *why);

/*
 * From the framebuffer's work loop only (one thread). Each returns 0 on
 * success; anything else means "do this frame with the CPU copy".
 */
int andromeda_gpu_present_ready(void);	/* 1: attached, set up or settable, not failed */
int andromeda_gpu_present_setup(const struct andr_present_geom *geom);
/* Map a surface's physical pages read-only for the GPU in slot (< ANDR_PRESENT_SLOTS). */
int andromeda_gpu_present_map(uint32_t slot, const uint64_t *pages, uint32_t npages, uint32_t page_size);
/*
 * Drop the slot's GPU mapping. 0: the pages are no longer reachable by the
 * GPU (PTEs cleared, TLB invalidated and synced) and may be unwired. Not 0:
 * keep them wired and the surface retained.
 */
int andromeda_gpu_present_unmap(uint32_t slot);
/* Clean the frame from the CPU caches, blit it onto the panel, wait for the fence. */
int andromeda_gpu_present_frame(uint32_t slot, const struct andr_present_src *src);
/*
 * From the framebuffer's CPU copy (patch 0201), before reading a surface at
 * kva: with Metal (andrmtl) on and andrfbsync not 0, wait for the GPU's
 * submits and clean the surface from the caches. Does nothing otherwise.
 * In andromeda_gpu_mtl.c.
 */
void andromeda_fbsync_before_read(uintptr_t kva, uint64_t bytes);

#ifdef __cplusplus
}
#endif
#endif /* ANDROMEDA_GPU_PRESENT_H */
