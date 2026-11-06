#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_memory.h"
/* Reproduces the real machine: gPhysBase is above 4GiB, so a DRAM-window check
 * is not the same as a 40-bit mask. The panicking addresses from the two real
 * boots are used verbatim as regression inputs. */
int main(void)
{
    const uint64_t first=0x101004000ULL,last=first+0xc0000000ULL; /* 3 GiB window */
    /* Inside the window: accepted. */
    assert(cepheus_gpu_pa_range_managed(first,0x4000,first,last));
    assert(cepheus_gpu_pa_range_managed(0x181492000ULL,0x4000,first,last));
    assert(cepheus_gpu_pa_range_managed(last-0x1000,0x1000,first,last));
    /* The two addresses that actually panicked (below gPhysBase). */
    assert(!cepheus_gpu_pa_range_managed(0x843e4000ULL,0x1000,first,last));
    assert(!cepheus_gpu_pa_range_managed(0x843b4000ULL,0x1000,first,last));
    /* A range that starts inside but ends outside is rejected, not truncated. */
    assert(!cepheus_gpu_pa_range_managed(last-0x1000,0x2000,first,last));
    assert(!cepheus_gpu_pa_range_managed(first,last-first+0x1000,first,last));
    /* Unaligned, empty, inverted and overflowing inputs are rejected. */
    assert(!cepheus_gpu_pa_range_managed(first+1,0x1000,first,last));
    assert(!cepheus_gpu_pa_range_managed(first,0,first,last));
    assert(!cepheus_gpu_pa_range_managed(first,0x1001,first,last));
    assert(!cepheus_gpu_pa_range_managed(first,0x1000,last,first));
    assert(!cepheus_gpu_pa_range_managed(first,0x1000,first,first));
    assert(!cepheus_gpu_pa_range_managed(UINT64_MAX-0x1000,0x2000,first,last));
    /* A zero-width window can never contain anything. */
    assert(!cepheus_gpu_pa_range_managed(0x1000,0x1000,0,0));
    /* Decode exactly as IOBMD does; top must not expand past last and the
     * requested alignment must not silently turn into a multi-MiB allocation. */
    for(uint64_t top=first+0x8000;top<first+0x100000;top+=0x1000) {
        uint64_t mask=cepheus_gpu_pa_window_mask(first,top,4096);
        assert(mask);
        uint64_t zeros=(~mask)&(mask-1);
        assert(zeros+1==4096 && (mask|zeros)<top);
    }
    assert(cepheus_gpu_pa_window_mask(first,last,16384)==0x1c1003000ULL);
    assert(!cepheus_gpu_pa_window_mask(first,last+1,16384));
    assert(!cepheus_gpu_pa_window_mask(first,first,16384));
    assert(!cepheus_gpu_pa_window_mask(first,last,8192));
    assert(!cepheus_gpu_pa_window_mask(first,(1ULL<<40)+0x4000,16384));
    puts("GPU PA window: real gPhysBase>4GiB layout, panicking PAs rejected, no truncation PASS");
}
