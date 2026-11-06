/* Whole production bringup chain with fake hardware and REAL local firmware.
 * No firmware embedded in source. Unavailable fixture -> explicit skip.
 * Each fault mode runs in a fresh process: don't free potentially live buffers.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cepheus_gmu_bringup.h"
#include "cepheus_gpu_config.h"
#include "cepheus_qmp_mmio.h"
#include "cepheus_pdc_mmio.h"
#include "cepheus_gmu_retire_mmio.h"
#include "cepheus_smmu_detach.h"
#include "cepheus_gmu_upload_mmio.h"
#include "cepheus_gpu_fault_mmio.h"
#include "cepheus_qmp_connect.h"
#include "cepheus_gpu_tsens.h"
#include "gpu_config_fixture.h"
#include "gpu_smmu_fixture.h"
int cepheus_gpu_config_probe(const struct ceph_arc_db *,uint32_t,uint32_t);
struct hw {
    struct ceph_gmu_bringup *s;unsigned mode,phase,votes,commands,failed;
    uint32_t power[CE_PWR_REG_COUNT],gmu[0x30000/4],pdc_cfg[0x10000/4],pdc_seq[0x10000/4];
    uint32_t gpu[0x40000/4];unsigned retiring,slumbered,parked,detached;
    struct ceph_gmu_mmio gmu_mmio;struct ceph_pdc_mmio pdc_mmio;
    struct ceph_gmu_park_mmio park_mmio;
    struct ceph_gmu_upload_mmio upload_mmio;
    struct ceph_gpu_fault_mmio fault_mmio;
    unsigned fault_held;
    uint32_t tsens_ram[2][1024],tsens_srot[2][1024];struct ceph_gpu_tsens tsens;
    uint64_t smmu[0x10000/8],time;struct ceph_smmu_mmio smmu_mmio;
    uint32_t qmp[1024],qirq;unsigned qmp_notified;struct ceph_hfi_queue fw_rx,fw_tx;
    struct ceph_gmu_slumber_ops sl;struct ceph_gmu_park_ops pk;
    struct ceph_smmu_detach_ops dt;struct ceph_gpu_retire_ops retire;
    struct ceph_oob_ops oob;uint32_t oob_info,oob_gx;unsigned oob_held,oob_released;
};
static int guard(void *p){struct hw *h=p;assert(h->s);return 0;}
static int temperature(void *p,struct ceph_gpu_temperature *sample){struct hw *h=p;
    h->tsens_ram[0][0xe4/4]=h->tsens_ram[1][0xe4/4]=8;
    h->tsens_ram[0][0xdc/4]=(1u<<21)|400;h->tsens_ram[1][0xcc/4]=(1u<<21)|420;
    if(h->mode==15 || (h->mode==16 && h->s->stage==CE_BR_SMMU) ||
        (h->mode==20 && h->s->stage==CE_BR_HFI) || (h->mode==21 && h->s->dcvs_acked))
        h->tsens_ram[1][0xcc/4]=(1u<<21)|600;
    if(h->mode==17)h->tsens_ram[1][0xcc/4]=420;
    return ceph_gpu_tsens_sample(&h->tsens,sample);}
static uint32_t *gmu_reg(struct hw *h,enum ceph_gmu_reg r){uint32_t off;assert(!ceph_gmu_register_offset(r,&off));return &h->gmu[off/4];}
static int now(void *p,uint64_t *n){struct hw *h=p;h->time+=10000;*n=h->time;
    if(h->gpu[0x3c45]&2 && h->mode!=11)h->gpu[0x3c46]|=2;
    return 0;}
static int delayus(void *p,unsigned n){struct hw *h=p;h->time+=n*1000;return 0;}
static void end(void *p,int keep){struct hw *h=p;assert(h->s);(void)keep;}
static int power_prepare(void *p,struct ceph_power_input *i){struct hw *h=p;if(h->mode==1)return -11;
    i->cx=(struct ceph_arc_resource){.address=0x30000,.levels={0,16,48,64,128,192,224,256,320},.count=9};
    i->mx=i->cx;i->mx.address=0x30004;i->current_cx=i->current_mx=1;i->parent_hz=300000000;h->phase=1;return 0;}
static int power_read(void *p,enum ceph_power_reg r,uint32_t *v){*v=((struct hw *)p)->power[r];return 0;}
static int power_write(void *p,enum ceph_power_reg r,uint32_t v){struct hw *h=p;assert(h->votes==2);h->power[r]=v;
    if(r==CE_PWR_CX && !(v&1)){h->power[CE_PWR_CX_STATUS]=1u<<31;for(unsigned c=CE_PWR_GMU;c<=CE_PWR_DVM;++c)if(h->power[c]&1)h->power[c]&=~(1u<<31);}
    if(r==CE_PWR_RCG){h->power[r]&=~1u;if((v&2)||(h->power[CE_PWR_GMU]&1))h->power[r]&=~(1u<<31);else h->power[r]|=1u<<31;}
    if(r>=CE_PWR_GMU && r<=CE_PWR_DVM && v&1)h->power[r]&=~(1u<<31);
    if(r==CE_PWR_GMU)h->power[CE_PWR_RCG]&=~(1u<<31);
    return 0;}
static int vote(void *p,uint32_t address,unsigned v){struct hw *h=p;assert(v==2 && address==(h->votes?0x30000u:0x30004u));h->votes++;return 0;}
static int pdc_begin(void *p,const struct ceph_pdc_input *i){struct hw *h=p;assert(h->s->workspace.state==CE_WS_SHARED && i->cfg_pa==0xb290000 && *gmu_reg(h,CE_GMU_TCM_RETENTION)==1);return h->mode==3?-13:ceph_pdc_mmio_begin(&h->pdc_mmio,i);}
static int pdc_read(void *p,enum ceph_pdc_window w,uint32_t o,uint32_t *v){return ceph_pdc_mmio_read(&((struct hw *)p)->pdc_mmio,w,o,v);}
static int pdc_write(void *p,enum ceph_pdc_window w,uint32_t o,uint32_t v){return ceph_pdc_mmio_write(&((struct hw *)p)->pdc_mmio,w,o,v);}
static int pdc_validate(void *p){return ceph_pdc_mmio_validate(&((struct hw *)p)->pdc_mmio);}
static int pdc_barrier(void *p){return ceph_pdc_mmio_barrier(&((struct hw *)p)->pdc_mmio);}
static void pdc_end(void *p,int keep){ceph_pdc_mmio_end(&((struct hw *)p)->pdc_mmio,keep);}
static int smmu_begin(void *p,const struct ceph_smmu_plan *plan,uint32_t actlr){struct hw *h=p;assert(h->s->pdc.attempted==48 && actlr==0x303 && plan->cb==3);return h->mode==4?-14:0;}
static int smmu_read(void *p,enum ceph_smmu_reg r,uint64_t *v){return ceph_smmu_mmio_read(&((struct hw *)p)->smmu_mmio,r,v);}
static int smmu_write(void *p,enum ceph_smmu_reg r,uint64_t v){struct hw *h=p;
    int e=ceph_smmu_mmio_write(&h->smmu_mmio,r,v);if(e)return e;
    if(r==CE_SM_TLBSYNC)smmu_fixture_set(&h->smmu_mmio,CE_SM_TLBSTATUS,0);
    return 0;}
static int publish_tables(void *p){struct hw *h=p;assert(h->s->workspace.state==CE_WS_SHARED);
    for(unsigned i=0;i<CE_BUF_COUNT;++i) {
        uint64_t pa,flags;const struct ceph_vm_region *r=&h->s->workspace.layout.regions[i];
        assert(!ceph_vm_lookup(&h->s->workspace.table,r->va,&pa,&flags) && pa==r->pa);
    }
    ceph_smmu_mmio_order();return 0;}
static int upload_begin(void *p){struct ceph_gmu_upload_mmio *m=p;struct hw *h=m->owner;
    assert(h->s->smmu.state==CE_SM_ENABLED);return h->mode==5?-15:ceph_gmu_upload_mmio_begin(m);}
static int upload_publish(void *p){struct ceph_gmu_upload_mmio *m=p;struct hw *h=m->owner;
    int e=ceph_gmu_upload_mmio_publish(m);if(!e && h->mode==14)h->gmu[0x3000/4]^=1;return e;}
static int cm3_begin(void *p,const struct ceph_gmu_start_config *c){struct hw *h=p;
    assert(h->s->backend.upload.writes && c->hfi_va==0x60100000 && c->log_va==0x60200000);return h->mode==6?-16:0;}
static int cm3_read(void *p,enum ceph_gmu_reg r,uint32_t *v){return ceph_gmu_mmio_start_read(&((struct hw *)p)->gmu_mmio,r,v);}
static int cm3_write(void *p,enum ceph_gmu_reg r,uint32_t v){struct hw *h=p;
    int e=ceph_gmu_mmio_start_write(&h->gmu_mmio,r,v);if(e)return e;
    if(r==CE_GMU_SYSRESET && !v)*gmu_reg(h,CE_GMU_INIT_RESULT)=0xbabeface;
    if(r==CE_GMU_HFI_INIT)*gmu_reg(h,CE_GMU_HFI_STATUS)=1;
    return 0;}
static int notify(void *p){struct hw *h=p;assert(h->s->cm3.state==CE_GS_HFI_READY);uint32_t cmd[255];unsigned count;
    if(!h->retiring)assert(h->s->cm3.state==CE_GS_HFI_READY);
    assert(!ceph_gmu_mmio_hfi_notify(&h->gmu_mmio));
    h->fw_rx=h->s->link.tx;h->fw_tx=h->s->link.rx;
    h->fw_rx.io=(struct ceph_hfi_ops){h,guard,guard,guard,guard};h->fw_tx.io=h->fw_rx.io;
    assert(!ceph_hfi_read(&h->fw_rx,cmd,255,&count));
    if(h->retiring || h->s->stopping) {
        assert(count==3 && (cmd[0]&255)==33 && cmd[1]==3 && cmd[2]==1);
        uint32_t ack[]={ceph_hfi_header(33,3,1,0),cmd[0],0};
        h->gmu[0x1f8ec-0x1a800]=15;h->gmu[0x1f8d0-0x1a800]=0xc0;
        const uint32_t tcs[]={0x23746,0x237ee,0x23896,0x2393e};
        for(unsigned i=0;i<4;++i)h->gmu[tcs[i]-0x1a800]=1;
        return ceph_hfi_write(&h->fw_tx,ack,3);
    }
    const unsigned ids0[]={1,4,3,11,14,30,10},ids1[]={1,4,3,11,7,11,11,14,30,10};
    const unsigned *ids=h->mode==8?ids1:ids0;unsigned total=h->mode==8?10:7;
    assert(h->commands<total && (cmd[0]&255)==ids[h->commands]);h->commands++;
    uint32_t expected[160];int words=0;const struct ceph_hfi_boot_config *cfg=&h->s->backend.hfi;
    switch(cmd[0]&255) {
    case 4: words=ceph_hfi_perf(expected,160,cfg->gx,cfg->ng,cfg->cx,cfg->nc,cfg->gfx,cfg->cxl,cfg->mx);break;
    case 3: words=ceph_hfi_bandwidth(expected,160,cfg->bw);assert(cmd[1]==12 && cmd[2]==3 && cmd[3]==3);break;
    case 7: words=ceph_hfi_acd_table(expected,160,cfg->acd);assert(cmd[5]==2687328253u && cmd[16]==21521);break;
    case 30: assert(count==4 && cmd[1]==1 && cmd[2]==1 && cmd[3]==0xff && h->s->stage==CE_BR_DCVS);break;
    case 10: assert(count==1 && h->s->stage==CE_BR_START && h->s->keepalive.state==CE_OOB_HELD);break;
    }
    if(words){assert(words==(int)count);assert(!memcmp(cmd+1,expected+1,(count-1)*4));}
    unsigned n=(cmd[0]&255)==1?4:3;uint32_t ack[]={ceph_hfi_header(cmd[0]&255,n,1,0),cmd[0],h->mode==7?1u:0u,0x20000042};
    if(h->mode==19 && (cmd[0]&255)==30)ack[2]=1;
    if(h->mode==9) {
        struct ceph_hfi_queue debug=h->s->link.debug;debug.io=h->fw_tx.io;
        uint32_t error[18]={0};error[0]=ceph_hfi_header(100,18,0,0);error[1]=0xc001;
        assert(!ceph_hfi_write(&debug,error,18));
    }
    if(h->mode==13)smmu_fixture_set(&h->smmu_mmio,CE_SM_FSR,2);
    return ceph_hfi_write(&h->fw_tx,ack,n);}
static void failed(void *p,enum ceph_bringup_stage stage,int e){struct hw *h=p;assert(e && stage>=CE_BR_POWER);h->failed=1;}
static int qnow(void *p,uint64_t *n){struct hw *h=p;
    if(h->qirq && h->qmp[32]==32){assert(!memcmp(h->qmp+33,"{class: gpu, res: acd, value: 1}",32));h->qmp[32]=0;h->qirq=0;h->qmp_notified=1;}
    return now(p,n);}
static int qpause(void *p){struct hw *h=p;
    if(!h->qirq)return 0;
    h->qirq=0;
    if(h->qmp[9]==CE_QMP_UP)h->qmp[10]=CE_QMP_UP;
    if(h->qmp[11]==CE_QMP_UP){h->qmp[5]=CE_QMP_UP;h->qmp[12]=CE_QMP_UP;}
    return 0;}
static int bind(void *p,struct ceph_gmu_bringup *s){struct hw *h=p;assert(h->s==s && s->power.state==CE_PWR_READY);if(h->mode==2)return -12;
    struct ceph_smmu_stream streams[16]={0};for(unsigned i=0;i<16;++i)streams[i].s2cr=2u<<16;
    assert(!ceph_smmu_gmu_plan(streams,16,2,3,8,5,4,s->workspace.table.pa,0,&s->backend.smmu_plan));
    smmu_fixture_init(&h->smmu_mmio,h->smmu,&s->backend.smmu_plan,h,guard);
    h->upload_mmio=(struct ceph_gmu_upload_mmio){&h->gmu_mmio,h,end,0,0};
    assert(!ceph_gmu_upload_mmio_bind(&h->upload_mmio,&s->backend.upload));
    s->backend.upload.begin=upload_begin;s->backend.upload.publish=upload_publish;
    if(h->mode==12)++s->backend.hfi.min_fw_minor;
    return 0;}
static int slbegin(void *p){struct hw *h=p;
    assert((h->s->stage==CE_BR_RETIRING || h->s->stopping) && !h->retiring);h->retiring=1;return 0;}
static int slread(void *p,enum ceph_slumber_reg r,uint32_t *v){return ceph_gmu_mmio_slumber_read(&((struct hw *)p)->gmu_mmio,r,v);}
static int slwrite(void *p,enum ceph_slumber_reg r,uint32_t v){return ceph_gmu_mmio_slumber_write(&((struct hw *)p)->gmu_mmio,r,v);}
static void slend(void *p,int done){struct hw *h=p;h->slumbered=(unsigned)done;}
static int park_begin(void *p){struct hw *h=p;assert(h->slumbered);return ceph_gmu_mmio_park_ready(&h->park_mmio);}
static int park_read(void *p,enum ceph_gmu_park_reg r,uint32_t *v){return ceph_gmu_mmio_park_read(&((struct hw *)p)->park_mmio,r,v);}
static int park_write(void *p,enum ceph_gmu_park_reg r,uint32_t v){return ceph_gmu_mmio_park_write(&((struct hw *)p)->park_mmio,r,v);}
static void park_end(void *p,int done){struct hw *h=p;h->parked=(unsigned)done;}
static int detach_begin(void *p,const struct ceph_smmu_plan *plan){struct hw *h=p;
    assert(h->parked && plan->ttbr0==h->s->backend.smmu_plan.ttbr0);return 0;}
static void detach_end(void *p,int done){struct hw *h=p;h->detached=(unsigned)done;}
static int oob_begin(void *p){struct hw *h=p;assert(!h->oob_held);h->oob_held=1;return 0;}
static int oob_validate(void *p){struct hw *h=p;return h->oob_held?0:-1;}
static int oob_read(void *p,enum ceph_oob_reg r,uint32_t *v){struct hw *h=p;
    if(!h->oob_held)return -1;
    *v=r==CE_OOB_G2H_INFO?h->oob_info:h->oob_gx;return 0;}
static int oob_write(void *p,enum ceph_oob_reg r,uint32_t v){struct hw *h=p;
    struct ceph_oob_bits bits;
    assert(h->oob_held && !ceph_gmu_oob_gpu_bits(h->s->cm3.hfi_version,&bits));
    if(r==CE_OOB_H2G_SET) {
        if(v==bits.request) {
            if(h->mode!=22)h->oob_info|=bits.ack;
        } else {
            assert(v==bits.release);++h->oob_released;
        }
        return 0;
    }
    assert(r==CE_OOB_G2H_CLEAR && v==bits.ack);h->oob_info&=~v;return 0;}
static int oob_drained(void *p){struct hw *h=p;return h->retiring?0:-1;}
static void oob_end(void *p,int keep){struct hw *h=p;(void)keep;h->oob_held=1;}
static int fault_acquire(void *p,unsigned *domains){struct hw *h=p;assert(!h->fault_held);h->fault_held=1;*domains=6;return 0;}
static int fault_lease(void *p,unsigned domains){struct hw *h=p;assert(h->fault_held && domains==6);return 0;}
static void fault_release(void *p){struct hw *h=p;assert(h->fault_held);h->fault_held=0;}
static int fault_health(void *p){return ceph_gpu_fault_mmio_gmu_health(&((struct hw *)p)->fault_mmio);}
int ceph_test_bringup(const char *path,unsigned mode)
{
    static struct ceph_gmu_bringup s;static struct hw h;h.s=&s;h.mode=mode;
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size==37680);rewind(f);
    uint8_t *data=malloc((size_t)size);assert(data);assert(fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
    assert(!cepheus_gmu_bringup_prepare(&s,data,(size_t)size));free(data);
    struct ceph_gmu_bringup_backends *b=&s.backend;
    b->owner=&h;b->bind=bind;b->failed=failed;
    h.tsens=(struct ceph_gpu_tsens){{h.tsens_ram[0],h.tsens_ram[1]},{4096,4096},{0xc263000,0xc265000},&h,guard,now,
        {h.tsens_srot[0],h.tsens_srot[1]},{4096,4096},{0xc222000,0xc223000}};
    h.tsens_srot[0][1]=1|(1u<<18);h.tsens_srot[1][1]=1|(1u<<14);
    b->temperature=temperature;b->now_ns=now;b->trial_ns=30000000000ULL;
    b->power=(struct ceph_power_ops){&h,power_prepare,guard,power_read,power_write,vote,delayus,end};
    b->pdc=(struct ceph_pdc_ops){&h,pdc_begin,pdc_validate,pdc_read,pdc_write,pdc_barrier,pdc_end};
    b->smmu=(struct ceph_smmu_ops){&h,smmu_begin,guard,smmu_read,smmu_write,publish_tables,guard,delayus,end};
    b->cm3=(struct ceph_gmu_start_ops){&h,cm3_begin,guard,cm3_read,cm3_write,guard,now,guard,end};
    b->transport=(struct ceph_gmu_hfi_transport){&h,guard,notify,now,guard,fault_health};
    config_fixture();struct ceph_arc_db db={config_db,sizeof(config_db),config_read};
    if(mode==0){assert(!cepheus_gpu_config_probe(&db,0x06040001,0));assert(cepheus_gpu_config_probe(NULL,0x06040001,0)<0);}
    struct ceph_gpu_config config;
    assert(!ceph_gpu_config_build(&db,0x06040001,mode==8?0x20000000:0,&config));
    assert(!ceph_gpu_config_hfi(&config,0x20000001,&b->hfi));
    b->initial_freq=config.initial_freq;b->initial_bw=config.initial_bw;
    struct ceph_qmp_mmio qmp={.ram=h.qmp,.irq=&h.qirq,.ram_pa=0xc300000,.irq_pa=0x17c0000c,
        .bytes=4096,.owner=&h,.lease=guard,.now_ns=qnow,.pause=qpause,.release=end};
    if(mode==8) {
        h.qmp[0]=0x4d41494c;h.qmp[1]=1;
        for(unsigned c=0;c<2;++c)for(unsigned i=0;i<4;++i)h.qmp[3+6*c+i]=CE_QMP_DOWN;
        h.qmp[3]=CE_QMP_UP;
        h.qmp[7]=64;h.qmp[8]=64;h.qmp[13]=128;h.qmp[14]=128;
        struct ceph_qmp_connect connect={0};
        assert(!ceph_qmp_connect_cold(&connect,&qmp) && connect.connected && connect.writes==4);
        assert(!ceph_qmp_mmio_bind(&qmp,1) && !ceph_qmp_mmio_ops(&qmp,&b->acd));
    }
    b->pdc_input=config.pdc;
    h.power[CE_PWR_CX]=h.power[CE_PWR_GX]=0x222001;h.power[CE_PWR_CX_STATUS]=0x5ff4;h.power[CE_PWR_CFG_AHB]=1;
    h.power[CE_PWR_RCG]=0x80000000;h.power[CE_PWR_GMU]=0x80004220;h.power[CE_PWR_CM3_RESET]=1;
    for(unsigned i=CE_PWR_CXO;i<=CE_PWR_DVM;++i)h.power[i]=0x80000001;
    *gmu_reg(&h,CE_GMU_SYSRESET)=1;*gmu_reg(&h,CE_GMU_HFI_VERSION)=0x20000001;
    h.gmu[0x1f8d0-0x1a800]=0xc0;
    h.gmu_mmio=(struct ceph_gmu_mmio){(volatile uint8_t *)h.gmu,sizeof(h.gmu),s.workspace.layout.start,0x20000001,&h,guard};
    h.pdc_mmio=(struct ceph_pdc_mmio){.gmu=&h.gmu_mmio,.cfg=h.pdc_cfg,.seq=h.pdc_seq,
        .cfg_pa=0xb290000,.seq_pa=0xb490000,.cfg_bytes=sizeof(h.pdc_cfg),.seq_bytes=sizeof(h.pdc_seq),.owner=&h,.lease=guard,.finish=end};
    h.fault_mmio=(struct ceph_gpu_fault_mmio){&h.gmu_mmio,&h.smmu_mmio,NULL,0,&h,fault_acquire,fault_lease,now,fault_release,0,0};
    h.park_mmio=(struct ceph_gmu_park_mmio){&h.gmu_mmio,h.gpu,sizeof(h.gpu),&h,guard};
    h.sl=(struct ceph_gmu_slumber_ops){&h,slbegin,guard,slread,slwrite,guard,now,guard,slend};
    h.pk=(struct ceph_gmu_park_ops){&h,park_begin,guard,park_read,park_write,guard,now,guard,park_end};
    h.dt=(struct ceph_smmu_detach_ops){&h,detach_begin,guard,smmu_read,smmu_write,guard,delayus,detach_end};
    /* Failure-path stop must use the same session/plan as retirement. */
    h.retire=(struct ceph_gpu_retire_ops){&h.sl,&h.pk,&h.dt,&s.link.session,&b->smmu_plan,
        config.initial_bw,config.initial_freq,config.bw.levels,config.policy.gx_levels};
    b->stop=&h.retire;
    h.oob=(struct ceph_oob_ops){&h,oob_begin,oob_validate,oob_read,oob_write,guard,now,guard,oob_drained,oob_end};
    b->oob=h.oob;b->oob_release=&h.oob;
    int e=cepheus_gmu_bringup_start(&s);
    if(!mode || mode==8 || mode==11 || mode==18) {
        assert(!e && !h.failed && s.stage==CE_BR_HFI && s.dcvs_acked && s.fw_version==0x20000042 && h.commands==(mode==8?10u:7u) && s.start_sent && !s.firmware && h.qmp_notified==(mode==8));
        /* Full closed loop: no reset of SMMU/RAM/session between boot and
         * retirement. Actual installed context and same HFI sequence retire. */
        struct ceph_gpu_retire_ops retire=h.retire;
        uint64_t group=s.workspace.publication.group;
        struct ceph_hfi_session foreign=s.link.session;retire.hfi=&foreign;
        assert(cepheus_gmu_bringup_retire(&s,&retire)<0 && s.stage==CE_BR_HFI && !h.retiring);
        retire.hfi=&s.link.session;
        /* Slumber must name the pair firmware actually entered. */
        retire.bw=0;
        assert(cepheus_gmu_bringup_retire(&s,&retire)<0 && s.stage==CE_BR_HFI && !h.retiring && !s.link.session.busy);
        retire.bw=config.initial_bw;retire.freq=0;
        assert(cepheus_gmu_bringup_retire(&s,&retire)<0 && s.stage==CE_BR_HFI && !h.retiring && !s.link.session.busy);
        retire.freq=config.initial_freq;
        if(mode==18) {
            h.time+=30000000000ULL;
            assert(cepheus_gmu_bringup_thermal_check(&s)==CE_THERMAL_EXPIRED);
            assert(cepheus_gmu_bringup_thermal_check(&s)==CE_THERMAL_LATCHED);
        }
        e=cepheus_gmu_bringup_retire(&s,&retire);
        if(mode==11) {
            assert(e && h.failed && s.stage==CE_BR_FAILED && s.workspace.state==CE_WS_RETAINED);
            assert(!s.link.bound && !b->upload.maps && !b->upload.itcm && b->upload.quarantined);
            struct ceph_gpu_group_status status;assert(!cepheus_gpu_memory_group_status(group,&status));
            assert(status.pool_terminal && status.state==CE_GPU_GROUP_UNCERTAIN && status.retained_buffers==6);
            assert(!h.detached && !h.parked);
            puts("Whole-session retire timeout: no detach/free, all aliases revoked, retained identity PASS (RAM)");
            return e;
        }
        assert(!e);
        assert(s.workspace.state==CE_WS_RELEASED && h.slumbered && h.parked && h.detached);
        assert(s.link.session.sequence==h.commands+1);
        assert(s.stage==CE_BR_RETIRED && !s.link.bound && !b->upload.maps && !b->upload.itcm);
        /* Keepalive must be dropped only after slumber/park/detach completed. */
        assert(h.oob_released==1 && s.keepalive.state==CE_OOB_RELEASED && h.detached);
        struct ceph_gpu_group_status status;assert(cepheus_gpu_memory_group_status(group,&status)<0);
        assert(cepheus_gmu_bringup_retire(&s,&retire)<0);
        printf("Closed loop bin%u: same installed SMMU/HFI -> slumber -> park -> detach -> all buffers released PASS (RAM hardware)\n",config.policy.bin);
    }
    else {
        assert(e && h.failed && s.stage==CE_BR_FAILED);
        /* Quarantine flag must distinguish a device-touching failure from a
         * refusal that happened before any publish/power write. */
        assert(b->upload.quarantined==(int)s.device_visible);
        assert(!b->upload.itcm && !b->upload.dtcm && !b->upload.maps && !b->upload.map_count);
        for(unsigned i=0;i<3;++i)assert(!s.upload_maps[i].cpu);
        /* After CM3 is released the failure path must attempt the bounded stop
         * BEFORE quarantining; a poisoned session must report refusal instead. */
        if(ceph_bringup_stage_cm3_maybe_live(s.failed_stage)) {
            assert(s.stop_attempted);
            if(mode==21) {
                /* Post-HFI failure with a healthy session: the bounded stop must
                 * actually run and complete before the backing is quarantined. */
                assert(!s.stop_error && h.slumbered && h.parked && h.detached && !h.fault_held);
            } else if(mode==22) {
                /* CM3/HFI are live but the keepalive was never acquired, so the
                 * stop still runs; no keepalive release is owed because none
                 * was held. */
                assert(!s.stop_error && h.slumbered && h.parked && h.detached && !h.oob_released);
            } else {
                /* An already-poisoned session cannot stop the device; the
                 * refusal must be recorded, never reported as quiescent. */
                assert(s.stop_error && !h.detached);
            }
        } else assert(!s.stop_attempted);
        if(mode==9)assert(s.link.debug_error==CE_HFI_FW_ERROR && s.link.last_error[1]==0xc001 && s.fw_version==0);
        if((mode>=3 && mode<=9) || mode==13 || mode==14 || mode==16 || mode==19 || mode==20 || mode==21) {
            struct ceph_gpu_group_status status;
            assert(s.workspace.state==CE_WS_RETAINED && !s.workspace.table.words && !s.workspace.views[0].cpu);
            assert(!cepheus_gpu_memory_group_status(s.workspace.publication.group,&status));
            assert(status.state==CE_GPU_GROUP_UNCERTAIN && status.pool_terminal && status.retained_buffers==CE_GPU_WORKSPACE_BUFFERS);
        }
        if(mode==12)assert(s.failed_stage==CE_BR_BOUND && !s.workspace.publication.group && !s.retention_writes && !s.pdc.attempted);
        if(mode==13) {
            uint64_t fsr;assert(!ceph_smmu_mmio_read(&h.smmu_mmio,CE_SM_FSR,&fsr) && fsr==2);
            assert(s.failed_stage==CE_BR_HFI && !s.fw_version && h.commands==1 && !h.fault_held);
        }
        if(mode==14)assert(s.failed_stage==CE_BR_UPLOAD && !s.cm3.effects && !h.commands);
        if(mode==15 || mode==17)assert(s.failed_stage==CE_BR_THERMAL && !s.power.effects && !s.workspace.publication.group && s.thermal.failure);
        if(mode==13 || mode==14)assert(!s.thermal_refused);
        /* Mode 16 fails the SMMU stage on a hot sensor: refusal must be marked. */
        if(mode==16)assert(s.thermal_refused);
        if(mode==20 || mode==21) {
            assert(s.thermal.failure==CE_THERMAL_HOT && s.thermal_refused);
            /* CE_THERMAL_HOT and CE_HFI_IO are both -3, so the flag, not the
             * numeric error, must carry the thermal-vs-fault distinction. */
            /* Raw error code is preserved; only the flag distinguishes cause. */
            assert(s.error==s.io_error && s.error<0);
        }
        /* Mode 20 fails before any HFI exchange: no slumber session exists, so
         * the stop must be refused and reported rather than assumed quiet. */
        if(mode==20)assert(s.failed_stage==CE_BR_HFI && s.stop_error && !h.slumbered);
        if(mode==21)assert(s.failed_stage==CE_BR_DCVS);
        /* A refused keepalive must fail before id10 is ever sent. */
        if(mode==22) {
            assert(s.failed_stage==CE_BR_KEEPALIVE && !s.start_sent && h.commands==6);
            assert(s.keepalive.state==CE_OOB_FAILED && !h.oob_released);
        }
        if(mode==16)assert(s.failed_stage==CE_BR_SMMU && !b->upload.writes && !s.cm3.effects && s.thermal.failure);
        if(mode==19)assert(s.failed_stage==CE_BR_DCVS && !s.dcvs_acked && h.commands==6);
    }
    printf("Whole GMU chain mode=%u: sealed file/workspace -> power -> PDC -> SMMU -> upload -> CM3 -> actual HFI rings PASS (emulated hardware)\n",mode);
    return e;
}

int ceph_test_bringup_prepare_failure(const char *path)
{
    /* Fill eleven slots: firmware can allocate slot12, workspace fails after
     * four buffers. Test main arranges complete() failure, so all handles
     * remain diagnostic identities rather than being force-freed. */
    uint64_t occupied[11];for(unsigned i=0;i<11;++i)assert(!cepheus_gpu_memory_alloc(4096,&occupied[i]));
    uint8_t data[37680];FILE *f=fopen(path,"rb");assert(f);assert(fread(data,1,sizeof(data),f)==sizeof(data));fclose(f);
    struct ceph_gmu_bringup s={0};assert(cepheus_gmu_bringup_prepare(&s,data,sizeof(data))<0);
    assert(s.stage==CE_BR_FAILED && s.workspace.state==CE_WS_RETAINED && s.firmware);
    assert(!s.workspace.table.words && !s.workspace.layout.start.hfi_va);
    return 0;
}
