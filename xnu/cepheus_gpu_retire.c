#include "cepheus_gpu_memory_private.h"
#include "cepheus_smmu_detach.h"
#include "cepheus_gmu_slumber.h"
#include "cepheus_gmu_park.h"
#include "cepheus_gpu_va.h"

int ceph_memory_validate_workspace(const struct ceph_gpu_memory_view views[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_gpu_publication *p)
{
    if(!views || !p || (p->smr&0x7fffu)!=5)return -1;
    const size_t minimum[]={0x3c000,0x3c000,0x5000,0x1000,0x1000,0x8000};
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)
        if(!views[i].cpu || ((uintptr_t)views[i].cpu&4095u) || views[i].bytes<minimum[i] ||
            views[i].bytes>((minimum[i]+16383)&~(size_t)16383))return -1;
    struct ceph_gpu_backing backing[CE_BUF_COUNT];
    for(unsigned i=0;i<CE_BUF_COUNT;++i)backing[i]=(struct ceph_gpu_backing){views[i].pa,views[i].bytes};
    struct ceph_gpu_va_plan layout;if(ceph_gpu_va_plan(backing,&layout))return -1;
    if(views[5].pa!=(p->ttbr0&0xffffffffffULL))return -1;
    for(unsigned i=0;i<CE_BUF_COUNT;++i)
        if(views[5].pa<views[i].pa+views[i].bytes && views[i].pa<views[5].pa+views[5].bytes)return -1;
    struct ceph_vm_pool table={views[5].cpu,views[5].pa,8,6};
    if(ceph_vm_matches(&table,layout.regions,CE_BUF_COUNT))return -1;
    uint32_t expected[54];if(ceph_hfi_table(expected,54,layout.start.hfi_va,layout.start.hfi_bytes))return -1;
    const volatile uint32_t *actual=views[2].cpu;
    for(unsigned i=0;i<54;++i)if(actual[i]!=expected[i])return -1;
    return 0;
}

int cepheus_gpu_memory_share_workspace(const uint64_t handles[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_smmu_plan *p,struct ceph_gpu_publication *out)
{
    /* All buffers here come from KGSL's GMU_CONTEXT_KERNEL, including HFI
     * (unprivileged AP != user SID). SID4 needs its own separate user context;
     * generic SMMU support for it does not permit publishing this workspace. */
    if(!ceph_smmu_plan_canonical(p) || (p->smr&0x7fffu)!=5 || !out)return -1;
    struct ceph_gpu_publication identity={0};
    identity.ttbr0=p->ttbr0;identity.ttbr1=p->ttbr1;identity.smr=p->smr;
    identity.cb=p->cb;identity.slot=p->slot;identity.asid=p->asid;
    return ceph_memory_share_group(handles,&identity,out);
}

int cepheus_gpu_memory_retire_workspace(const struct ceph_gpu_publication *p,struct ceph_gpu_retire_ops *b)
{
    if(!p || !b || !b->slumber || !b->park || !b->detach || !b->hfi || !ceph_smmu_plan_canonical(b->plan) ||
        p->ttbr0!=b->plan->ttbr0 || p->ttbr1!=b->plan->ttbr1 || p->smr!=b->plan->smr ||
        p->cb!=b->plan->cb || p->slot!=b->plan->slot || p->asid!=b->plan->asid)return -1;
    if(ceph_memory_retire_begin(p))return -1;
    struct ceph_gmu_slumber slumber={0};struct ceph_gmu_park park={0};struct ceph_smmu_detach detach={0};
    int e=ceph_gmu_slumber_request(&slumber,b->slumber,b->hfi,b->bw,b->freq,b->bw_levels,b->gx_levels);
    if(!e)e=ceph_gmu_park_slumbered(&park,b->park);
    if(!e)e=ceph_smmu_detach_context(&detach,b->detach,b->plan);
    /* These engines check ACK AND hardware state, scoped TLB sync and context
     * identity. The trusted backend keeps admission/park held until return. */
    int released=ceph_memory_retire_finish(p,e==0);
    return e?e:released;
}
