/* A640v2 speed-bin policy from adreno_a6xx.c:a6xx_efuse_power_features and
 * kgsl_gmu.c:gmu_aop_mailbox_init, with the saved Cepheus DT bin tables.
 * readback is QFPROM base+0x4130, mask0xe0000000 >>29. No MMIO here.
 * Raw value must be independently read from a permitted live eFuse mapping;
 * a saved DT lists bins but does not choose one. Unreadable/unknown is refused.
 */
#ifndef CEPHEUS_GPU_BIN_H
#define CEPHEUS_GPU_BIN_H
#include <stdint.h>
struct ceph_gpu_bin_policy {unsigned bin,gx_levels,lm,acd;uint32_t throttle_mask;};
static inline int ceph_gpu_bin_policy(uint32_t chipid,uint32_t readback,struct ceph_gpu_bin_policy *out)
{
    if(!out || chipid!=0x06040001 || readback==UINT32_MAX)return -1;
    unsigned bin=(readback&0xe0000000u)>>29;
    if(bin>1)return -1;
    *out=(struct ceph_gpu_bin_policy){bin,bin?7u:8u,bin?1u:0u,bin?1u:0u,bin?0x40u:0x80u};
    return 0;
}
static inline int ceph_gpu_bin_policy_valid(const struct ceph_gpu_bin_policy *p)
{
    if(!p || p->bin>1)return 0;
    return p->gx_levels==(p->bin?7u:8u) && p->lm==p->bin && p->acd==p->bin &&
        p->throttle_mask==(p->bin?0x40u:0x80u);
}
#endif
