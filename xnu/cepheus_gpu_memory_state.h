/* Fixed-slot GPU buffer lifecycle. Caller owns a mutex; no hardware effects.
 * A published target cannot be released by this API. No unverified done boolean.
 */
#ifndef CEPHEUS_GPU_MEMORY_STATE_H
#define CEPHEUS_GPU_MEMORY_STATE_H
#include <stddef.h>
#include <stdint.h>
enum ceph_mem_state {CE_MEM_FREE,CE_MEM_ALLOCATING,CE_MEM_CPU,CE_MEM_PUBLISHED,CE_MEM_QUARANTINED,CE_MEM_RETIRED,CE_MEM_SEALED,CE_MEM_SHARED,CE_MEM_RETIRING};
struct ceph_mem_slot {enum ceph_mem_state state;uint32_t generation;unsigned views;};
static inline uint64_t ceph_mem_handle(unsigned slot,uint32_t generation)
{return ((uint64_t)generation<<32)|(slot+1u);}
static inline int ceph_mem_reserve(struct ceph_mem_slot *s)
{
    if(!s || s->state!=CE_MEM_FREE || s->views)return -1;
    if(s->generation==UINT32_MAX){s->state=CE_MEM_RETIRED;return -1;}
    ++s->generation;s->state=CE_MEM_ALLOCATING;return 0;
}
static inline int ceph_mem_matches(const struct ceph_mem_slot *s,unsigned index,uint64_t handle)
{
    return s && (uint32_t)handle==index+1u && (handle>>32)==s->generation && s->generation &&
        s->state!=CE_MEM_FREE && s->state!=CE_MEM_ALLOCATING && s->state!=CE_MEM_RETIRED;
}
static inline int ceph_mem_finish_allocation(struct ceph_mem_slot *s,int success,int uncertain)
{
    if(!s || s->state!=CE_MEM_ALLOCATING)return -1;
    s->state=uncertain?CE_MEM_QUARANTINED:(success?CE_MEM_CPU:CE_MEM_FREE);return 0;
}
static inline int ceph_mem_view_get(struct ceph_mem_slot *s)
{
    if(!s || s->state!=CE_MEM_CPU || s->views==UINT32_MAX)return -1;
    ++s->views;return 0;
}
static inline int ceph_mem_view_put(struct ceph_mem_slot *s)
{
    if(!s || !s->views || (s->state!=CE_MEM_CPU && s->state!=CE_MEM_PUBLISHED &&
        s->state!=CE_MEM_QUARANTINED && s->state!=CE_MEM_SEALED))return -1;
    --s->views;return 0;
}
static inline int ceph_mem_publish(struct ceph_mem_slot *s)
{
    if(!s || s->state!=CE_MEM_CPU || s->views)return -1;
    s->state=CE_MEM_PUBLISHED;return 0;
}
static inline int ceph_mem_release_begin(struct ceph_mem_slot *s)
{
    if(!s || (s->state!=CE_MEM_CPU && s->state!=CE_MEM_SEALED) || s->views)return -1;
    s->state=CE_MEM_ALLOCATING;return 0; /* reserve slot while complete/release sleeps */
}
static inline int ceph_mem_seal(struct ceph_mem_slot *s)
{
    if(!s || s->state!=CE_MEM_CPU || s->views)return -1;
    s->state=CE_MEM_SEALED;return 0;
}
static inline int ceph_mem_readonly_get(struct ceph_mem_slot *s)
{
    if(!s || s->state!=CE_MEM_SEALED || s->views==UINT32_MAX)return -1;
    ++s->views;return 0;
}
/* Shared batch keeps the sole existing CPU view alive. No new views or puts
 * are admitted: one trusted workspace owns the CPU/device synchronization. */
static inline int ceph_mem_can_share(const struct ceph_mem_slot *s)
{return s && s->state==CE_MEM_CPU && s->views==1;}
static inline int ceph_mem_can_retire(const struct ceph_mem_slot *s)
{return s && s->state==CE_MEM_SHARED && s->views==1;}
#endif
