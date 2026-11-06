/* Per-trial fail-closed thermal guard, pure logic. Platform samples actual
 * GPU TSENS sensors and monotonic timestamps, serializes state, and must stop
 * work on any refusal. A stop failure quarantines hardware resources separately.
 * Does NOT turn off hardware thermal/LM protection, reset GPU or lower rail votes.
 */
#ifndef CEPHEUS_GPU_THERMAL_H
#define CEPHEUS_GPU_THERMAL_H
#include <stdint.h>
enum ceph_thermal_result {CE_THERMAL_OK=0,CE_THERMAL_INVALID=-1,CE_THERMAL_STALE=-2,
    CE_THERMAL_HOT=-3,CE_THERMAL_EXPIRED=-4,CE_THERMAL_CLOCK=-5,CE_THERMAL_LATCHED=-6};
struct ceph_gpu_thermal {
    uint64_t start_ns,last_ns,limit_ns;
    unsigned active;int failure;
};
struct ceph_gpu_temperature {int gpuss0_mc,gpuss1_mc;uint64_t sampled_ns;unsigned valid_mask;};
static inline int ceph_gpu_thermal_start(struct ceph_gpu_thermal *t,uint64_t now,uint64_t limit)
{
    if(!t || t->active || t->failure || !limit || limit>30000000000ULL)return CE_THERMAL_INVALID;
    *t=(struct ceph_gpu_thermal){now,now,limit,1,0};return 0;
}
static inline int ceph_gpu_thermal_check(struct ceph_gpu_thermal *t,const struct ceph_gpu_temperature *s,uint64_t now)
{
    if(!t || !t->active)return CE_THERMAL_INVALID;
    if(t->failure)return CE_THERMAL_LATCHED;
    int e=0;
    if(now<t->last_ns || now<t->start_ns)e=CE_THERMAL_CLOCK;
    else if(now-t->start_ns>=t->limit_ns)e=CE_THERMAL_EXPIRED;
    else if(!s || s->valid_mask!=3 || s->gpuss0_mc < -40000 || s->gpuss0_mc>125000 ||
        s->gpuss1_mc < -40000 || s->gpuss1_mc>125000)e=CE_THERMAL_INVALID;
    else if(s->sampled_ns>now || now-s->sampled_ns>100000000ULL)e=CE_THERMAL_STALE;
    else if(s->gpuss0_mc>=60000 || s->gpuss1_mc>=60000)e=CE_THERMAL_HOT;
    if(e)t->failure=e;else t->last_ns=now;
    return e;
}
#endif
