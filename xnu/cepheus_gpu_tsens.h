/* Read-only gpuss-0/1 sampler: saved FDT thermal zones resolve to TSENS0 sensor15
 * and TSENS1 sensor11 (NOT legacy tsens_tz_sensor12 or TSENS0 sensors12/13).
 * Caller owns both actual mappings
 * and excludes TSENS reinitialization; no threshold/calibration/recovery writes.
 * Both valid bits are required: unlike downstream fallback, repeated invalid
 * readings are NOT accepted just because their numeric codes agree.
 * sampled_ns timestamps CPU observation, not a hardware conversion counter.
 * lease must guarantee ongoing conversion/clock health; valid alone can't.
 */
#ifndef CEPHEUS_GPU_TSENS_H
#define CEPHEUS_GPU_TSENS_H
#include <stddef.h>
#include "cepheus_gpu_thermal.h"
struct ceph_gpu_tsens {
    volatile uint32_t *tm[2];size_t bytes[2];uint64_t pa[2];
    void *owner;int (*lease)(void *);int (*now_ns)(void *,uint64_t *);
    volatile uint32_t *srot[2];size_t srot_bytes[2];uint64_t srot_pa[2];
};
static inline void ceph_gpu_tsens_order(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#elif !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#else
#error "TSENS sampler requires arm64"
#endif
}
static inline int ceph_gpu_tsens_decode(uint32_t word,int *mc)
{
    if(!mc || word==UINT32_MAX || !(word&(1u<<21)))return -1;
    int code=(int)(word&0xfff);if(code&0x800)code-=4096;
    int temperature=code*100;if(temperature < -40000 || temperature>125000)return -1;
    *mc=temperature;return 0;
}
static inline int ceph_gpu_tsens_sample(void *ctx,struct ceph_gpu_temperature *out)
{
    if(!out)return -1;
    *out=(struct ceph_gpu_temperature){0};struct ceph_gpu_tsens *m=ctx;
    if(!m || !m->lease || !m->now_ns || m->lease(m->owner))return -1;
    for(unsigned i=0;i<2;++i)
        if(!m->tm[i] || ((uintptr_t)m->tm[i]&3u) || m->pa[i]!=(i?0xc265000u:0xc263000u) || m->bytes[i]!=4096 ||
            !m->srot[i] || ((uintptr_t)m->srot[i]&3u) || m->srot_pa[i]!=(i?0xc223000u:0xc222000u) || m->srot_bytes[i]!=4096)return -1;
    if(m->tm[0]==m->tm[1] || m->srot[0]==m->srot[1])return -1;
    uint64_t start,end;if(m->now_ns(m->owner,&start))return -1;
    struct ceph_gpu_temperature s={0};s.sampled_ns=start;
    for(unsigned i=0;i<2;++i) {
        /* Downstream tsens2xxx_hw_sensor_en reads SROT+4. FDT gives a4-byte
         * resource but ioremap covers its containing page; platform must own
         * the full page explicitly, not claim offset4 is inside a4-byte map. */
        uint32_t enabled=m->srot[i][1];ceph_gpu_tsens_order();
        unsigned sensor=i?11:15;
        if(enabled==UINT32_MAX || !(enabled&1) || !(enabled&(1u<<(sensor+3))))return -1;
        uint32_t ready=m->tm[i][0xe4/4];ceph_gpu_tsens_order();
        if(ready==UINT32_MAX || !(ready&8))return -1;
        uint32_t faults=m->tm[i][0x14/4];ceph_gpu_tsens_order();
        if(faults==UINT32_MAX || (faults&((1u<<31)|(1u<<sensor))))return -1;
        int value=0,valid=0;
        for(unsigned attempt=0;attempt<3 && !valid;++attempt) {
            if(m->lease(m->owner))return -1;
            uint32_t word=m->tm[i][0xa0/4+(i?11:15)];ceph_gpu_tsens_order();
            valid=!ceph_gpu_tsens_decode(word,&value);
        }
        if(!valid)return -1;
        if(i)s.gpuss1_mc=value;else s.gpuss0_mc=value;
        s.valid_mask|=1u<<i;
    }
    for(unsigned i=0;i<2;++i) {
        uint32_t enabled=m->srot[i][1];ceph_gpu_tsens_order();
        if(enabled==UINT32_MAX || !(enabled&1) || !(enabled&(1u<<((i?11:15)+3))))return -1;
        uint32_t ready=m->tm[i][0xe4/4];ceph_gpu_tsens_order();
        if(ready==UINT32_MAX || !(ready&8))return -1;
        uint32_t faults=m->tm[i][0x14/4];ceph_gpu_tsens_order();
        if(faults==UINT32_MAX || (faults&((1u<<31)|(1u<<(i?11:15)))))return -1;
    }
    if(m->lease(m->owner) || m->now_ns(m->owner,&end) ||
        end<start || end-start>100000000ULL)return -1;
    *out=s;return 0;
}
#endif
