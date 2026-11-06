#include <cassert>
#include <cstdio>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" {
#include "cepheus_gmu_workspace.h"
#include "cepheus_smmu_plan.h"
}
static ceph_gmu_workspace w={};
static void abandon_during_allocation()
{
    mock_io::after_allocate=nullptr;
    assert(!cepheus_gpu_memory_abandon_workspace(&w.publication));
}
int main(void)
{
    assert(!cepheus_gmu_workspace_prepare(&w));
    ceph_smmu_stream stream[8]={};for(auto &s:stream)s.s2cr=2u<<16;
    ceph_smmu_plan plan;assert(!ceph_smmu_gmu_plan(stream,8,2,3,8,5,4,w.table.pa,0,&plan));
    assert(!cepheus_gmu_workspace_share(&w,&plan));
    uint64_t old_cpu;assert(!cepheus_gpu_memory_alloc(4096,&old_cpu));
    unsigned before=mock_io::live;
    mock_io::after_allocate=abandon_during_allocation;
    uint64_t raced=123;assert(cepheus_gpu_memory_alloc(4096,&raced)<0 && !raced);
    assert(mock_io::live==before && mock_io::maps==before && mock_io::prepared==before);
    assert(cepheus_gpu_memory_publish(old_cpu)<0); /* old CPU handle cannot publish either */
    assert(!cepheus_gpu_memory_release_cpu(old_cpu));
    ceph_gpu_group_status status;assert(!cepheus_gpu_memory_group_status(w.publication.group,&status));
    assert(status.pool_terminal && status.state==CE_GPU_GROUP_UNCERTAIN && status.retained_buffers==6);
    assert(cepheus_gpu_memory_alloc(4096,&raced)<0 && !raced);
    assert(mock_io::live==6);
    puts("Terminal pool race: abandonment during unlocked IOKit allocation refuses new handle/publication, retains group PASS");
}
