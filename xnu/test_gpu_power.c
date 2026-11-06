#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_power.h"

int main(void)
{
    struct ceph_gpu_power_snapshot s = {
        .cx_gdscr = 0x222001, .cx_hw = 1u << 31, .gx_gdscr = 0x222001,
        .cfg = 1, .ahb = 1, .gmu = 1, .cxo = 1, .rcg = 0,
        .ddr = 1, .memnoc = 1, .dvm = 1,
    };
    assert(ceph_gpu_gmu_blockers(NULL) == CE_GPU_BAD_READ);
    assert(!ceph_gpu_gmu_blockers(&s)); /* hw status, not HLOS vote */
    assert(!ceph_gpu_memory_blockers(&s));
    s.cx_hw = 0; s.cx_gdscr |= 1u << 31;
    assert(ceph_gpu_gmu_blockers(&s) == CE_GPU_CX_OFF);
    s.cx_hw = 1u << 31;
    uint32_t *required[] = {&s.cx_gdscr, &s.cx_hw, &s.gx_gdscr, &s.cfg,
        &s.ahb, &s.gmu, &s.cxo, &s.rcg, &s.ddr, &s.memnoc, &s.dvm};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        uint32_t old = *required[i]; *required[i] = UINT32_MAX;
        assert(ceph_gpu_memory_blockers(&s) & CE_GPU_BAD_READ);
        *required[i] = old;
    }
    s.gmu = 0x80000001; assert(ceph_gpu_gmu_blockers(&s) & CE_GPU_GMU_OFF);
    s.gmu = 0x80000003; assert(ceph_gpu_gmu_blockers(&s) & CE_GPU_GMU_OFF);
    s.gmu = 0x20000001; assert(!ceph_gpu_gmu_blockers(&s));
    s.rcg = 1; assert(ceph_gpu_gmu_blockers(&s) == CE_GPU_RCG_BUSY);
    s.rcg = 0x80000000; assert(ceph_gpu_gmu_blockers(&s) == CE_GPU_RCG_BUSY);
    s.rcg = 0; s.ddr = 0x80000001;
    assert(!ceph_gpu_gmu_blockers(&s));
    assert(ceph_gpu_memory_blockers(&s) == CE_GPU_DDR_OFF);
    s.memnoc = s.dvm = 0x80000001;
    assert(ceph_gpu_memory_blockers(&s) == (CE_GPU_DDR_OFF | CE_GPU_MEMNOC_OFF | CE_GPU_DVM_OFF));
    /* Cepheus d336276 boot, 2026-11-02: real hardware status is OFF.
     * DDR_AXI=0 has no halt bit, but that is not proof of clock ownership. */
    s = (struct ceph_gpu_power_snapshot){
        .cx_gdscr=0x222001, .cx_hw=0x5ff4, .gx_gdscr=0x222001,
        .cfg=1, .ahb=0x80000001, .gmu=0x80004220, .cxo=0x80000001,
        .rcg=0x80000000, .ddr=0, .memnoc=0x80000221, .dvm=0x80000001,
    };
    assert(ceph_gpu_gmu_blockers(&s) == 0x7a);
    assert(ceph_gpu_memory_blockers(&s) == 0x37a);
    puts("GPU power: CX hardware status, invalid reads, clocks and RCG fail-closed: PASS (no MMIO)");
}
