/*
 * Host test of andromeda_scanout.c's pure parts: the display bank's page
 * tables (panel memory at its own address, surfaces in slots) and the pipe /
 * scaler values of a frame.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined xnu/test_andromeda_scanout.c -o /tmp/scanout-test
 */
#define ANDR_SCANOUT_HOST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "andromeda_scanout.c"

static unsigned cleans;

static void
clean(uint64_t pa, unsigned len)
{
	(void)pa;
	assert(len == 8);
	cleans++;
}

/* Walk the tables as the SMMU does: the leaf for IOVA, 0 when not mapped. */
static uint64_t
walk(const struct sc_tables *t, uint32_t iova)
{
	const uint64_t *table = t->va;
	for (unsigned level = 1; level < 3; level++) {
		const uint64_t e = table[(iova >> (level == 1 ? 30 : 21)) & (level == 1 ? 3u : 511u)];
		if ((e & 3) != 3) {
			return 0;
		}
		uint64_t pa = e & 0xfffffffff000ull;
		assert(pa >= t->pa && pa < t->pa + 0x1000ull * SC_POOL_PAGES && (pa & 0xfff) == 0);
		table = (const uint64_t *)((const uint8_t *)t->va + (pa - t->pa));
	}
	return table[(iova >> 12) & 511];
}

static void
test_tables(void)
{
	struct sc_tables t;
	t.va = aligned_alloc(4096, 0x1000 * SC_POOL_PAGES);
	assert(t.va != NULL);
	memset(t.va, 0, 0x1000 * SC_POOL_PAGES);
	t.pa = 0x1c0000000ull;          /* above 4 GiB, as kernel memory may be */
	t.used = 1;

	/* The ABL's panel memory: 1080x2340, stride 4320, at its own address. */
	const uint64_t lo = 0x9c000000ull, hi = (lo + 4320ull * 2340 + 0xfff) & ~0xfffull;
	const uint32_t n = (uint32_t)((hi - lo) >> 12);
	assert(n == 2468);
	assert(sc_pt_map(&t, (uint32_t)lo, NULL, lo, n, clean));
	assert(t.used == 1 + 1 + 5);    /* level 1, one level 2 (2-3 GiB), five level 3 */
	for (uint32_t i = 0; i < n; i++) {
		const uint64_t e = walk(&t, (uint32_t)lo + 0x1000u * i);
		assert((e & 0xfffffffff000ull) == lo + 0x1000ull * i);
		assert((e & 3) == 3 && (e & (1u << 10)) && (e & (1u << 7)) && ((e >> 2) & 7) == 0);
		assert((e >> 53 & 3) == 3);
	}
	assert(walk(&t, (uint32_t)hi) == 0 && walk(&t, (uint32_t)lo - 0x1000u) == 0);

	/* A 1125x2436 surface, stride 4608: 2741 scattered pages above 4 GiB in slot 0. */
	const uint32_t sn = (4608u * 2436u + 0xfff) >> 12;
	assert(sn == 2741);
	uint64_t *pages = calloc(sn, sizeof(*pages));
	for (uint32_t i = 0; i < sn; i++) {
		pages[i] = 0x200000000ull + 0x4000ull * (sn - i) + 0x1000ull * (i & 3);
	}
	const unsigned before = t.used;
	assert(sc_pt_map(&t, SC_SLOT_BASE, pages, 0, sn, clean));
	assert(t.used == before + 6);  /* the slot shares the panel's level 2 */
	for (uint32_t i = 0; i < sn; i++) {
		assert((walk(&t, SC_SLOT_BASE + 0x1000u * i) & 0xfffffffff000ull) == pages[i]);
	}
	/* Unmapped again: no leaf left, tables kept for the next surface of the slot. */
	sc_pt_unmap(&t, SC_SLOT_BASE, sn, clean);
	for (uint32_t i = 0; i < sn; i++) {
		assert(walk(&t, SC_SLOT_BASE + 0x1000u * i) == 0);
	}
	assert(sc_pt_map(&t, SC_SLOT_BASE, pages, 0, sn, clean) && t.used == before + 6);
	/* The panel is untouched by all that. */
	assert((walk(&t, (uint32_t)lo) & 0xfffffffff000ull) == lo);

	/* Every slot holds a surface without running the pool out. */
	for (unsigned s = 1; s < SC_SLOTS; s++) {
		assert(sc_pt_map(&t, SC_SLOT_BASE + s * SC_SLOT_BYTES, pages, 0, sn, clean));
	}
	assert(t.used <= SC_POOL_PAGES);
	/* A pool that is full refuses instead of writing past its end. */
	t.used = SC_POOL_PAGES;
	assert(!sc_pt_map(&t, 0x10000000u, pages, 0, 1, clean));
	assert(cleans > 0);
	free(pages);
	free(t.va);
}

static void
test_frames(void)
{
	struct sc_regs r;
	/* sweetfb_fit=1: the 1125x2436 frame into 1080x2338 at y 1 (patch 0094's fit). */
	struct andr_scan_frame f = { 0x80000000u, 4608, 0, 0, 1125, 2436, 0, 1, 1080, 2338 };
	assert(sc_frame_regs(&f, 0x3000, &r));
	assert(r.src_size == ((2436u << 16) | 1125u) && r.src_xy == 0);
	assert(r.out_size == ((2338u << 16) | 1080u) && r.out_xy == (1u << 16));
	assert(r.addr == 0x80000000u && r.stride == 4608 && r.req == r.src_size && r.scaled);
	assert(r.step_h == (uint32_t)((1125ull << 21) / 1080) && r.step_v == (uint32_t)((2436ull << 21) / 2338));
	assert(r.step_h > (1u << 21) && r.step_v > (1u << 21));     /* down-scaling */
	assert(r.qsrc == (1125u | (2436u << 16)) && r.qdst == (1080u | (2338u << 16)));
	assert(r.preload == 0x03040304u);
	assert(r.qop == (1u | (3u << 16) | (1u << 31) | (1u << 14) | (1u << 10) | (1u << 29)));
	assert(sc_frame_regs(&f, 0x1002, &r) && (r.qop & (1u << 30)) && !(r.qop & (1u << 29)));

	/* sweetfb_fit=0: the centred 1080x2340 part, unscaled. */
	struct andr_scan_frame c = { 0x81000000u, 4608, 22, 48, 1080, 2340, 0, 0, 1080, 2340 };
	assert(sc_frame_regs(&c, 0x3000, &r) && !r.scaled);
	assert(r.src_xy == ((48u << 16) | 22u) && r.out_size == r.src_size && r.req == r.src_size);

	/* Refused: no stride, misaligned address, more than 4x down, sizes out of range. */
	struct andr_scan_frame bad = f;
	bad.stride = 0;
	assert(!sc_frame_regs(&bad, 0x3000, &r));
	bad = f;
	bad.iova += 4;
	assert(!sc_frame_regs(&bad, 0x3000, &r));
	bad = f;
	bad.dst_w = 200;
	assert(!sc_frame_regs(&bad, 0x3000, &r));
	bad = f;
	bad.src_w = 5000;
	assert(!sc_frame_regs(&bad, 0x3000, &r));
	bad = f;
	bad.stride = 0x10000;
	assert(!sc_frame_regs(&bad, 0x3000, &r));
}

/* SwapWait as Core Animation uses it: two buffers, swaps 1, 2, 3... alternating between them. */
static void
test_swap_wait(void)
{
	/* swap 5 queued, 4 going on screen, 3 on screen */
	assert(!andr_swap_done(5, 0, true, 5, 4, 3, false));       /* queued: not off the screen */
	assert(!andr_swap_done(4, 0, true, 5, 4, 3, false));       /* active */
	assert(!andr_swap_done(3, 0, true, 5, 4, 3, false));       /* on screen: its buffer is read */
	assert(andr_swap_done(2, 0, true, 5, 4, 3, false));        /* gone: its buffer may be drawn into */
	assert(andr_swap_done(3, 1, true, 5, 4, 3, false));        /* on screen counts as done for test 1 */
	assert(!andr_swap_done(4, 1, true, 5, 4, 3, false));
	assert(!andr_swap_done(9, 2, true, 5, 4, 3, false) && !andr_swap_done(9, 2, false, 0, 4, 3, false));
	assert(andr_swap_done(9, 2, false, 0, 0, 3, false));       /* nothing queued or active */
	/* a swap taken over by a newer one (never shown) is done at once */
	assert(andr_swap_done(5, 0, true, 6, 4, 3, false));
	/* after a CPU copy nothing is on screen: the swap is done once copied */
	assert(andr_swap_done(4, 0, false, 0, 0, 0, false));
	/* id 0 and display off: done */
	assert(andr_swap_done(0, 0, true, 5, 4, 3, false));
	assert(andr_swap_done(3, 0, true, 5, 4, 3, true) && andr_swap_done(5, 2, true, 5, 4, 3, true));
}

int
main(void)
{
	test_tables();
	test_frames();
	test_swap_wait();
	puts("andromeda_scanout: ok");
	return 0;
}
