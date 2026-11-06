/* Minimal A6xx PM4 builder for a future owned-scratch write test.
 * Packet fields from msm/adreno_pm4types.h; no MMIO/DMA in this module.
 * This is NOT a GPU driver or Metal implementation. GPU VM, GMU/SQE and
 * completion fences must exist before this buffer may be submitted.
 */
#ifndef CEPHEUS_A640_PM4_H
#define CEPHEUS_A640_PM4_H
#include <stdint.h>
static inline unsigned ceph_pm4_odd(uint32_t value)
{
	unsigned parity = 1;
	while (value) { parity ^= value & 1u; value >>= 1; }
	return parity;
}
static inline uint32_t ceph_pm4_type7(unsigned opcode, unsigned count)
{
	return 0x70000000u | count | (ceph_pm4_odd(count) << 15) |
	    (opcode << 16) | (ceph_pm4_odd(opcode) << 23);
}
static inline int ceph_pm4_valid_type7(uint32_t header)
{
	unsigned opcode = (header >> 16) & 0x7fu, count = header & 0x3fffu;
	return (header & 0xff004000u) == 0x70000000u &&
	    ((header >> 15) & 1u) == ceph_pm4_odd(count) &&
	    ((header >> 23) & 1u) == ceph_pm4_odd(opcode);
}
/* Exactly one CP_MEM_WRITE (3 payload words) and WAIT_FOR_IDLE. Restrict
 * destination to a caller-owned, already-GPU-mapped scratch allocation.
 * Passing CPU PA as gpu_address without an established mapping is invalid.
 */
static inline int ceph_a640_write_test(uint32_t *out, unsigned capacity,
    uint64_t scratch_base, uint64_t scratch_bytes, uint64_t gpu_address, uint32_t value)
{
	if (!out || capacity < 5 || (gpu_address & 3u) || scratch_bytes < 4 ||
	    scratch_base > UINT64_MAX - scratch_bytes || gpu_address < scratch_base ||
	    gpu_address - scratch_base > scratch_bytes - 4) { return -1; }
	out[0] = ceph_pm4_type7(0x3d, 3);
	out[1] = (uint32_t)gpu_address;
	out[2] = (uint32_t)(gpu_address >> 32);
	out[3] = value;
	out[4] = ceph_pm4_type7(0x26, 0);
	return 5;
}
/* End-of-pipeline completion for a future scratch test. CACHE_FLUSH_TS=4,
 * CP_EVENT_WRITE payload=event + 64-bit address + value. No interrupt request.
 * Caller still needs CPU cache invalidation AND quiescence before unmapping.
 */
static inline int ceph_a640_fenced_write_test(uint32_t *out,unsigned capacity,
    uint64_t scratch_base,uint64_t scratch_bytes,uint64_t data_address,uint32_t value,
    uint64_t fence_address,uint32_t sequence)
{
    if(!out || capacity<10 || !sequence || data_address==fence_address ||
        (fence_address&3u) || scratch_bytes<4 || scratch_base>UINT64_MAX-scratch_bytes ||
        fence_address<scratch_base || fence_address-scratch_base>scratch_bytes-4) return -1;
    if(ceph_a640_write_test(out,capacity,scratch_base,scratch_bytes,data_address,value)!=5) return -1;
    out[5]=ceph_pm4_type7(0x46,4);out[6]=4;
    out[7]=(uint32_t)fence_address;out[8]=(uint32_t)(fence_address>>32);out[9]=sequence;
    return 10;
}
#endif
