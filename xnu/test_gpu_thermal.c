#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_thermal.h"
int main(void)
{
    struct ceph_gpu_thermal t={0};struct ceph_gpu_temperature s={40000,42000,1000,3};
    assert(!ceph_gpu_thermal_start(&t,1000,30000000000ULL));assert(!ceph_gpu_thermal_check(&t,&s,1000));
    assert(ceph_gpu_thermal_start(&t,1000,1000)==CE_THERMAL_INVALID);
    assert(!ceph_gpu_thermal_check(&t,&s,100001000));
    assert(ceph_gpu_thermal_check(&t,&s,100001001)==CE_THERMAL_STALE);
    s.sampled_ns=100001001;assert(ceph_gpu_thermal_check(&t,&s,100001001)==CE_THERMAL_LATCHED);
    t=(struct ceph_gpu_thermal){0};assert(!ceph_gpu_thermal_start(&t,1000,30000000000ULL));
    s=(struct ceph_gpu_temperature){60000,42000,1000,3};assert(ceph_gpu_thermal_check(&t,&s,1000)==CE_THERMAL_HOT);
    t=(struct ceph_gpu_thermal){0};assert(!ceph_gpu_thermal_start(&t,1000,30000000000ULL));
    s.gpuss0_mc=40000;s.valid_mask=1;assert(ceph_gpu_thermal_check(&t,&s,1000)==CE_THERMAL_INVALID);
    t=(struct ceph_gpu_thermal){0};assert(!ceph_gpu_thermal_start(&t,1000,1000000));
    s.valid_mask=3;assert(ceph_gpu_thermal_check(&t,&s,1001000)==CE_THERMAL_EXPIRED);
    t=(struct ceph_gpu_thermal){0};assert(!ceph_gpu_thermal_start(&t,1000,1000000));
    assert(ceph_gpu_thermal_check(&t,&s,999)==CE_THERMAL_CLOCK);
    t=(struct ceph_gpu_thermal){0};assert(ceph_gpu_thermal_start(&t,0,30000000001ULL)==CE_THERMAL_INVALID);
    puts("GPU thermal trial: two sensors, 60C cap, 100ms freshness, <=30s lifetime and latched stop PASS (no MMIO)");
}
