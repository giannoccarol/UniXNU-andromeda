#include "cepheus_gmu_workspace.h"
#include "cepheus_hfi.h"
#include "cepheus_smmu_plan.h"
#if defined(KERNEL)
#include <sys/systm.h>
#else
#include <stdio.h>
#endif

int cepheus_gmu_workspace_release_cpu(struct ceph_gmu_workspace *w)
{
    if(!w || (w->state!=CE_WS_BUILDING && w->state!=CE_WS_READY_CPU && w->state!=CE_WS_FAILED))return -1;
    int result=0;
    for(unsigned i=CE_GPU_WORKSPACE_BUFFERS;i>0;--i) {
        unsigned n=i-1;
        if(w->leased&(1u<<n)) {
            if(cepheus_gpu_memory_view_put(w->handles[n])){result=-1;continue;}
            w->leased&=~(1u<<n);w->views[n]=(struct ceph_gpu_memory_view){0};
        }
        if(w->handles[n]) {
            if(cepheus_gpu_memory_release_cpu(w->handles[n])){result=-1;continue;}
            w->handles[n]=0;
        }
    }
    w->state=result?CE_WS_RETAINED:CE_WS_RELEASED;
    /* Some buffers may already have been released when another complete
     * failed. Never leave callable page-table pointers into that freed memory.
     * Retained handles and outstanding view bits preserve cleanup evidence. */
    w->table=(struct ceph_vm_pool){0};w->layout=(struct ceph_gpu_va_plan){0};
    return result;
}

int cepheus_gmu_workspace_prepare(struct ceph_gmu_workspace *w)
{
    if(!w || w->state!=CE_WS_NEW || w->leased)return -1;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)if(w->handles[i])return -1;
    w->state=CE_WS_BUILDING;
    const size_t sizes[]={0x3c000,0x3c000,5*4096,4096,4096,8*4096};
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
        if(cepheus_gpu_memory_alloc(sizes[i],&w->handles[i]))goto failed;
        if(cepheus_gpu_memory_view_get(w->handles[i],&w->views[i]))goto failed;
        w->leased|=1u<<i;
    }
    struct ceph_gpu_backing backing[CE_BUF_COUNT];
    for(unsigned i=0;i<CE_BUF_COUNT;++i)backing[i]=(struct ceph_gpu_backing){w->views[i].pa,w->views[i].bytes};
    if(ceph_gpu_va_plan(backing,&w->layout))goto failed;
    w->table=(struct ceph_vm_pool){w->views[CE_GPU_WORKSPACE_ROOT].cpu,w->views[CE_GPU_WORKSPACE_ROOT].pa,8,0};
    if(ceph_vm_build(&w->table,w->layout.regions,CE_BUF_COUNT) ||
        ceph_vm_matches(&w->table,w->layout.regions,CE_BUF_COUNT))goto failed;
    if(ceph_hfi_table(w->views[2].cpu,(unsigned)(w->views[2].bytes/4),w->layout.start.hfi_va,w->layout.start.hfi_bytes))goto failed;
    for(unsigned i=0;i<CE_BUF_COUNT;++i) {
        const struct ceph_vm_region *r=&w->layout.regions[i];uint64_t pa,flags;
        if(ceph_vm_lookup(&w->table,r->va,&pa,&flags) || pa!=r->pa ||
            ceph_vm_lookup(&w->table,r->va+r->bytes-1,&pa,&flags) || pa!=r->pa+r->bytes-1)goto failed;
    }
    w->state=CE_WS_READY_CPU;return 0;
failed:
    w->state=CE_WS_FAILED;
    (void)cepheus_gmu_workspace_release_cpu(w);
    if(w->state==CE_WS_RELEASED)w->state=CE_WS_FAILED;
    return -1;
}

void cepheus_gmu_workspace_selftest(void)
{
    static unsigned once;static struct ceph_gmu_workspace w;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&once,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    if(cepheus_gmu_workspace_prepare(&w)) {
        printf("CEPHEUS GMU MEM: workspace preparation failed state=%u; no SMMU/DMA\n",(unsigned)w.state);return;
    }
    printf("CEPHEUS GMU MEM: CPU workspace table_pages=%u HFI=%#x log=%#x; no SMMU/DMA\n",
        w.table.used,w.layout.start.hfi_va,w.layout.start.log_va);
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)
        printf("CEPHEUS GMU MEM: CPU backing index=%u pa=%#llx bytes=%#lx; not published\n",
            i,(unsigned long long)w.views[i].pa,(unsigned long)w.views[i].bytes);
    int result=cepheus_gmu_workspace_release_cpu(&w);
    printf("CEPHEUS GMU MEM: CPU workspace release=%d state=%u\n",result,(unsigned)w.state);
    unsigned retained=0;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)if(w.handles[i])++retained;
    printf("CEPHEUS GMU MEM: CPU cleanup retained_handles=%u leased_mask=%#x; no SMMU/DMA\n",retained,w.leased);
}

int cepheus_gmu_workspace_share(struct ceph_gmu_workspace *w,const struct ceph_smmu_plan *p)
{
    if(!w || w->state!=CE_WS_READY_CPU || w->leased!=CE_GPU_WORKSPACE_VIEWS || !p || p->ttbr0!=(w->table.pa|((uint64_t)p->asid<<48)))return -1;
    if(cepheus_gpu_memory_share_workspace(w->handles,p,&w->publication))return -1;
    w->state=CE_WS_SHARED;return 0;
}

int cepheus_gmu_workspace_retire(struct ceph_gmu_workspace *w,struct ceph_gpu_retire_ops *b)
{
    if(!w || w->state!=CE_WS_SHARED)return -1;
    int e=cepheus_gpu_memory_retire_workspace(&w->publication,b);
    if(e) {
        struct ceph_gpu_group_status status;
        if(!cepheus_gpu_memory_group_status(w->publication.group,&status) &&
            status.state==CE_GPU_GROUP_SHARED && !status.pool_terminal)return e;
    }
    /* Whether freed or quarantined, callers must cease CPU use. */
    w->table=(struct ceph_vm_pool){0};w->layout=(struct ceph_gpu_va_plan){0};w->leased=0;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)w->views[i]=(struct ceph_gpu_memory_view){0};
    if(!e)for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)w->handles[i]=0;
    w->state=e?CE_WS_RETAINED:CE_WS_RELEASED;return e;
}
