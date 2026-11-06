/* SM8150 read-only GPU readiness, not a power-on sequence.
 * sm8150-gdsc.dtsi selects GPUCC+0x1540 (not GDSCR) for CX status.
 * No status snapshot establishes regulator/clock ownership or safe DMA.
 */
#ifndef CEPHEUS_GPU_POWER_H
#define CEPHEUS_GPU_POWER_H
#include <stdint.h>

#define CE_GPU_CX_GDSCR 0x106cu
#define CE_GPU_CX_HW_CTRL 0x1540u
#define CE_GPU_GX_GDSCR 0x100cu
#define CE_GPU_AHB_CBCR 0x1078u
#define CE_GPU_GMU_CBCR 0x1098u
#define CE_GPU_CXO_CBCR 0x109cu
#define CE_GPU_GMU_CMD 0x1120u
#define CE_GPU_GMU_CFG 0x1124u
#define CE_GPU_GCC_CFG 0x71004u
#define CE_GPU_GCC_MEMNOC 0x7100cu
#define CE_GPU_GCC_DVM 0x71018u
#define CE_GPU_GCC_DDR 0x71154u

enum ceph_gpu_blocker {
    CE_GPU_BAD_READ = 1u << 0,
    CE_GPU_CX_OFF = 1u << 1,
    CE_GPU_CFG_OFF = 1u << 2,
    CE_GPU_AHB_OFF = 1u << 3,
    CE_GPU_GMU_OFF = 1u << 4,
    CE_GPU_CXO_OFF = 1u << 5,
    CE_GPU_RCG_BUSY = 1u << 6,
    CE_GPU_DDR_OFF = 1u << 7,
    CE_GPU_MEMNOC_OFF = 1u << 8,
    CE_GPU_DVM_OFF = 1u << 9,
};
struct ceph_gpu_power_snapshot {
    uint32_t cx_gdscr, cx_hw, gx_gdscr;
    uint32_t cfg, ahb, gmu, cxo, rcg;
    uint32_t ddr, memnoc, dvm;
};
/* Conservatively require an active branch, not just a local enable vote or
 * HWCG bit. clk-branch.c accepts CLK_OFF clear or the documented FSM-ON state.
 * A gated-but-available clock can be refused here; that is not proof of failure.
 */
static inline int ceph_gpu_branch_active(uint32_t value)
{
    uint32_t state = value & 0xf0000000u;
    return value != UINT32_MAX && (!(state & 0x80000000u) || state == 0x20000000u);
}
static inline unsigned ceph_gpu_gmu_blockers(const struct ceph_gpu_power_snapshot *s)
{
    if (!s) return CE_GPU_BAD_READ;
    unsigned blocked = 0;
    const uint32_t values[] = {s->cx_gdscr, s->cx_hw, s->gx_gdscr, s->cfg,
        s->ahb, s->gmu, s->cxo, s->rcg};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (values[i] == UINT32_MAX) blocked |= CE_GPU_BAD_READ;
    if (!(s->cx_hw & (1u << 31))) blocked |= CE_GPU_CX_OFF;
    if (!ceph_gpu_branch_active(s->cfg)) blocked |= CE_GPU_CFG_OFF;
    if (!ceph_gpu_branch_active(s->ahb)) blocked |= CE_GPU_AHB_OFF;
    if (!ceph_gpu_branch_active(s->gmu)) blocked |= CE_GPU_GMU_OFF;
    if (!ceph_gpu_branch_active(s->cxo)) blocked |= CE_GPU_CXO_OFF;
    if (s->rcg & ((1u << 31) | 1u)) blocked |= CE_GPU_RCG_BUSY;
    return blocked;
}
/* Necessary clock/power checks only. Never grants SMMU or bus ownership. */
static inline unsigned ceph_gpu_memory_blockers(const struct ceph_gpu_power_snapshot *s)
{
    unsigned blocked = ceph_gpu_gmu_blockers(s);
    if (!s) return blocked;
    if (s->ddr == UINT32_MAX || s->memnoc == UINT32_MAX || s->dvm == UINT32_MAX)
        blocked |= CE_GPU_BAD_READ;
    if (!ceph_gpu_branch_active(s->ddr)) blocked |= CE_GPU_DDR_OFF;
    if (!ceph_gpu_branch_active(s->memnoc)) blocked |= CE_GPU_MEMNOC_OFF;
    if (!ceph_gpu_branch_active(s->dvm)) blocked |= CE_GPU_DVM_OFF;
    return blocked;
}
#endif
