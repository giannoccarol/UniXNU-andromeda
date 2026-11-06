/* Executes production workspace/pool/retire orchestration with a synthetic
 * firmware ring and fake hardware registers. Failure scenarios are separate
 * processes so retained buffers remain retained, not forcibly cleaned by test. */
#include <assert.h>
#include <string.h>
#include "cepheus_gmu_workspace.h"
#include "cepheus_gmu_slumber.h"
#include "cepheus_gmu_park.h"
#include "cepheus_smmu_detach.h"
#include "cepheus_gmu_hfi_link.h"
#include "cepheus_gmu_retire_mmio.h"
#include "gpu_smmu_fixture.h"
struct fake {
    struct ceph_hfi_queue tx,rx,fw_rx,fw_tx;
    uint32_t tr,tw,rr,rw;
    uint32_t gmu_ram[0x30000/4],gpu_ram[0x40000/4];
    struct ceph_gmu_mmio gmu;
    struct ceph_gmu_park_mmio park;
    uint64_t smmu[0x10000/8],time;struct ceph_smmu_mmio smmu_mmio;
    unsigned phase,mode,notifies,detached;
};
static int okay(void *p){(void)p;return 0;}
static int now(void *p,uint64_t *t){struct fake *f=p;f->time+=10000;*t=f->time;
    if(f->gpu_ram[0x3c45]&2)f->gpu_ram[0x3c46]|=2;
    return 0;}
static int notify(void *p){struct fake *f=p;uint32_t cmd[3];unsigned n;
    assert(!ceph_hfi_read(&f->fw_rx,cmd,3,&n));
    if((cmd[0]&255)==1){uint32_t version[]={ceph_hfi_header(1,4,1,0),cmd[0],0,0x20000042};assert(!ceph_hfi_write(&f->fw_tx,version,4));return 0;}
    assert(n==3 && (cmd[0]&255)==33);++f->notifies;
    uint32_t ack[]={ceph_hfi_header(33,3,1,0),cmd[0],f->mode==2?1u:0u};
    assert(!ceph_hfi_write(&f->fw_tx,ack,3));f->gmu_ram[0x1f8ec-0x1a800]=15;f->gmu_ram[0x1f8d0-0x1a800]=0xc0;return 0;}
static int slbegin(void *p){struct fake *f=p;f->phase=1;return f->mode==1?-1:0;}
static int slread(void *p,enum ceph_slumber_reg r,uint32_t *v){return ceph_gmu_mmio_slumber_read(&((struct fake *)p)->gmu,r,v);}
static int slwrite(void *p,enum ceph_slumber_reg r,uint32_t v){return ceph_gmu_mmio_slumber_write(&((struct fake *)p)->gmu,r,v);}
static void slend(void *p,int done){struct fake *f=p;if(done)f->phase=2;}
static int pkbegin(void *p){struct fake *f=p;assert(f->phase==2);return f->mode==3?-1:0;}
static int pkread(void *p,enum ceph_gmu_park_reg r,uint32_t *v){return ceph_gmu_mmio_park_read(&((struct fake *)p)->park,r,v);}
static int pkwrite(void *p,enum ceph_gmu_park_reg r,uint32_t v){return ceph_gmu_mmio_park_write(&((struct fake *)p)->park,r,v);}
static void pkend(void *p,int done){struct fake *f=p;if(done)f->phase=3;}
static int dtbegin(void *p,const struct ceph_smmu_plan *plan){struct fake *f=p;assert(f->phase==3 && f->gmu_ram[0x1f800-0x1a800]==1 && f->gpu_ram[0x3c45]==2 && plan->cb==3);return 0;}
static int dtread(void *p,enum ceph_smmu_reg r,uint64_t *v){return ceph_smmu_mmio_read(&((struct fake *)p)->smmu_mmio,r,v);}
static int dtwrite(void *p,enum ceph_smmu_reg r,uint64_t v){struct fake *f=p;
    int e=ceph_smmu_mmio_write(&f->smmu_mmio,r,v);return e?e:f->mode==4?-1:0;}
static int delayus(void *p,unsigned u){(void)p;assert(u==1);return 0;}
static void dtend(void *p,int done){struct fake *f=p;f->detached=(unsigned)done;if(done)f->phase=4;}

int ceph_test_retire_path(unsigned mode)
{
    struct ceph_gmu_workspace w={0};assert(!cepheus_gmu_workspace_prepare(&w));
    struct ceph_smmu_stream streams[8]={0};for(unsigned i=0;i<8;++i)streams[i].s2cr=2u<<16;
    struct ceph_smmu_plan plan;assert(!ceph_smmu_gmu_plan(streams,8,2,3,8,5,4,w.table.pa,0,&plan));
    /* Duplicate handles, wrong root and extra views refuse atomically. */
    uint64_t bad[CE_GPU_WORKSPACE_BUFFERS];for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)bad[i]=w.handles[i];bad[1]=bad[0];
    struct ceph_gpu_publication publication;
    assert(cepheus_gpu_memory_share_workspace(bad,&plan,&publication)<0);
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)bad[i]=w.handles[i];
    bad[0]=w.handles[1];bad[1]=w.handles[0];
    assert(cepheus_gpu_memory_share_workspace(bad,&plan,&publication)<0); /* swapped cache role */
    uint64_t *l2=ceph_vm_table(&w.table,w.table.words[0]);assert(l2);
    uint64_t *l3=ceph_vm_table(&w.table,l2[0]);assert(l3);
    uint64_t saved=l3[5];l3[5]^=1u<<6; /* interior permission, not first/last page */
    assert(cepheus_gmu_workspace_share(&w,&plan)<0 && w.state==CE_WS_READY_CPU);l3[5]=saved;
    saved=l3[0x40];l3[0x40]=l3[4]; /* unauthorized extra mapping in hole */
    assert(cepheus_gmu_workspace_share(&w,&plan)<0);l3[0x40]=saved;
    struct ceph_smmu_plan user_plan=plan;user_plan.smr=0x84000004;
    assert(ceph_smmu_plan_canonical(&user_plan));
    assert(cepheus_gmu_workspace_share(&w,&user_plan)<0 && w.state==CE_WS_READY_CPU);
    struct ceph_gpu_memory_view extra;assert(!cepheus_gpu_memory_view_get(w.handles[0],&extra));
    assert(cepheus_gmu_workspace_share(&w,&plan)<0);assert(!cepheus_gpu_memory_view_put(w.handles[0]));
    assert(!cepheus_gmu_workspace_share(&w,&plan) && w.state==CE_WS_SHARED && w.leased==CE_GPU_WORKSPACE_VIEWS);
    assert(cepheus_gpu_memory_release_cpu(w.handles[0])<0 && cepheus_gpu_memory_view_put(w.handles[0])<0);
    assert(cepheus_gpu_memory_view_get(w.handles[0],&extra)<0);
    static struct fake f;f.mode=mode;struct ceph_gmu_hfi_link link={0};
    f.gmu=(struct ceph_gmu_mmio){(volatile uint8_t *)f.gmu_ram,sizeof(f.gmu_ram),w.layout.start,0x20000001,&f,okay};
    f.park=(struct ceph_gmu_park_mmio){&f.gmu,f.gpu_ram,sizeof(f.gpu_ram),&f,okay};
    const uint32_t tcs[]={0x23746,0x237ee,0x23896,0x2393e};
    for(unsigned i=0;i<4;++i)f.gmu_ram[tcs[i]-0x1a800]=1;
    struct ceph_gmu_hfi_transport transport={&f,okay,notify,now,okay,okay};
    assert(!cepheus_gmu_hfi_bind(&link,&w,&transport));
    f.fw_rx=link.tx;f.fw_rx.io=(struct ceph_hfi_ops){&f,okay,okay,okay,okay};
    f.fw_tx=link.rx;f.fw_tx.io=f.fw_rx.io;
    uint32_t version_cmd[2],version_reply[8];unsigned version_count;
    ceph_hfi_fw_version(version_cmd,2,2,0);
    assert(!ceph_hfi_exchange(&link.session,version_cmd,2,version_reply,8,&version_count));
    struct ceph_gmu_slumber_ops sl={&f,slbegin,okay,slread,slwrite,okay,now,okay,slend};
    struct ceph_gmu_park_ops pk={&f,pkbegin,okay,pkread,pkwrite,okay,now,okay,pkend};
    struct ceph_smmu_detach_ops dt={&f,dtbegin,okay,dtread,dtwrite,okay,delayus,dtend};
    smmu_fixture_init(&f.smmu_mmio,f.smmu,&plan,&f,okay);
    const enum ceph_smmu_reg regs[]={CE_SM_SMR,CE_SM_S2CR,CE_SM_SCTLR,CE_SM_TTBR0,CE_SM_TTBR1,
        CE_SM_TCR,CE_SM_TCR2,CE_SM_MAIR0,CE_SM_MAIR1,CE_SM_CBAR,CE_SM_CBA2R,CE_SM_ACTLR};
    const uint64_t values[]={plan.smr,plan.s2cr_translate,plan.sctlr,plan.ttbr0,plan.ttbr1,
        plan.tcr,plan.tcr2,plan.mair0,plan.mair1,plan.cbar,plan.cba2r,0x303};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);++i)smmu_fixture_set(&f.smmu_mmio,regs[i],values[i]);
    struct ceph_gpu_retire_ops ops={&sl,&pk,&dt,&link.session,&plan,3,1,12,8};
    assert(cepheus_gmu_workspace_retire(&w,NULL)<0 && w.state==CE_WS_SHARED && w.views[2].cpu && !f.phase);
    struct ceph_gpu_publication forged=w.publication;forged.group^=0x100;
    assert(cepheus_gpu_memory_retire_workspace(&forged,&ops)<0 && !f.phase);
    forged=w.publication;forged.handles[1]=forged.handles[0];
    assert(cepheus_gpu_memory_retire_workspace(&forged,&ops)<0 && !f.phase);
    struct ceph_gpu_group_status status;
    uint64_t group=w.publication.group;
    assert(!cepheus_gpu_memory_group_status(group,&status) && status.state==CE_GPU_GROUP_SHARED && status.retained_buffers==CE_GPU_WORKSPACE_BUFFERS);
    int e=cepheus_gmu_workspace_retire(&w,&ops);
    cepheus_gmu_hfi_invalidate(&link);
    assert(!w.table.words && !w.leased && !w.views[2].cpu);
    if(!mode)assert(!e && w.state==CE_WS_RELEASED && f.detached && f.notifies==1 && !w.handles[0]);
    else {
        assert(e && w.state==CE_WS_RETAINED);
        assert(!cepheus_gpu_memory_group_status(group,&status) && status.pool_terminal);
        assert(status.identity.ttbr0==plan.ttbr0 && status.retained_buffers==CE_GPU_WORKSPACE_BUFFERS);
        assert(status.state==(mode==5?CE_GPU_GROUP_DETACHED_RETAINED:CE_GPU_GROUP_UNCERTAIN));
        assert(f.detached==(mode==5));
        uint64_t handle;assert(cepheus_gpu_memory_alloc(4096,&handle)<0);
    }
    if(!mode)assert(cepheus_gpu_memory_group_status(group,&status)<0);
    assert(cepheus_gmu_workspace_retire(&w,&ops)<0);
    return e;
}
