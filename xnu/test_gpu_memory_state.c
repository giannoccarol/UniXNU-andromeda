#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_memory_state.h"
int main(void)
{
    struct ceph_mem_slot s={0};assert(!ceph_mem_reserve(&s));uint64_t old=ceph_mem_handle(0,s.generation);
    assert(!ceph_mem_matches(&s,0,old));assert(!ceph_mem_finish_allocation(&s,1,0));assert(ceph_mem_matches(&s,0,old));
    assert(!ceph_mem_view_get(&s));assert(ceph_mem_release_begin(&s)<0 && ceph_mem_publish(&s)<0);
    assert(!ceph_mem_view_put(&s));assert(ceph_mem_view_put(&s)<0);
    assert(!ceph_mem_release_begin(&s));assert(!ceph_mem_finish_allocation(&s,0,0));
    assert(!ceph_mem_reserve(&s));assert(!ceph_mem_finish_allocation(&s,1,0));assert(!ceph_mem_matches(&s,0,old));
    assert(!ceph_mem_publish(&s));assert(ceph_mem_release_begin(&s)<0 && ceph_mem_view_get(&s)<0);
    s=(struct ceph_mem_slot){0};assert(!ceph_mem_reserve(&s));assert(!ceph_mem_finish_allocation(&s,0,1));
    assert(s.state==CE_MEM_QUARANTINED && ceph_mem_reserve(&s)<0 && ceph_mem_release_begin(&s)<0);
    s=(struct ceph_mem_slot){.generation=UINT32_MAX};assert(ceph_mem_reserve(&s)<0 && s.state==CE_MEM_RETIRED);
    s=(struct ceph_mem_slot){.state=CE_MEM_CPU,.generation=1,.views=UINT32_MAX};assert(ceph_mem_view_get(&s)<0);
    s=(struct ceph_mem_slot){.state=CE_MEM_CPU,.generation=7};
    assert(!ceph_mem_view_get(&s) && ceph_mem_seal(&s)<0);assert(!ceph_mem_view_put(&s));
    assert(!ceph_mem_seal(&s) && ceph_mem_view_get(&s)<0 && ceph_mem_publish(&s)<0);
    assert(!ceph_mem_readonly_get(&s) && ceph_mem_release_begin(&s)<0);
    assert(!ceph_mem_view_put(&s) && !ceph_mem_release_begin(&s));
    s=(struct ceph_mem_slot){.state=CE_MEM_CPU,.generation=8,.views=1};
    assert(ceph_mem_can_share(&s));s.state=CE_MEM_SHARED;
    assert(ceph_mem_can_retire(&s) && ceph_mem_view_get(&s)<0 && ceph_mem_view_put(&s)<0 && ceph_mem_release_begin(&s)<0);
    s.state=CE_MEM_RETIRING;assert(!ceph_mem_can_retire(&s) && ceph_mem_view_put(&s)<0);
    puts("GPU memory lifecycle: leases, stale handles, publication, uncertain completion and generation wrap PASS");
}
