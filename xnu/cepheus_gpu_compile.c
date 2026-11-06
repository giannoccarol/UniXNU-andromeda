/* Linkable integration wrappers: compile real headers as XNU C, not only host
 * fixtures. These have no startup hook or MMIO backend. API calls are private
 * kernel building blocks and are NEVER exposed as a userspace dispatch table.
 */
#include "cepheus_gpu_compile.h"
#include "cepheus_gpu_firmware.h"
int cepheus_gpu_gx_smmu_plan(const struct ceph_smmu_stream *s,unsigned n,const struct ceph_smmu_geometry *g,
    unsigned slot,unsigned cb,unsigned asid,uint64_t root,uint32_t sctlr,struct ceph_gx_smmu_plan *out)
{ return ceph_gx_smmu_plan(s,n,g,slot,cb,asid,root,sctlr,out); }
int cepheus_gpu_gx_core_configure(struct ceph_gx_core *s)
{ return ceph_gx_core_configure(s); }
int cepheus_gpu_gx_perfcounter_configure(struct ceph_gx_perfcounter *s)
{ return ceph_gx_perf_configure(s); }
int cepheus_gpu_gx_gbif_configure(struct ceph_gx_gbif *s)
{ return ceph_gx_gbif_configure(s); }
int cepheus_gpu_gx_llc_configure(struct ceph_gx_llc *s)
{ return ceph_gx_llc_configure(s); }
int cepheus_gpu_scm_discover(struct ceph_scm_discovery *s,struct ceph_scm_discovery_ops *b)
{ return ceph_scm_discover_pas13(s,b); }
int cepheus_gpu_gx_va_partition(unsigned secured,uint64_t size,struct ceph_gx_va_partition *out)
{ return ceph_gx_va_partition(secured,size,out); }
int cepheus_gpu_gx_hwcg_configure(struct ceph_gx_hwcg *s)
{ return ceph_gx_hwcg_configure(s); }
int cepheus_gpu_cp_unhalt_check(const struct ceph_cp_unhalt_input *i)
{ return (int)ceph_cp_unhalt_check(i); }
int cepheus_gpu_vm39_build(struct ceph_vm_pool *p,const struct ceph_vm_region *r,unsigned n)
{ return ceph_vm39_build(p,r,n); }
int cepheus_gpu_cp_snapshot_mmio(struct ceph_cp_snapshot_mmio *s,uint32_t *out,unsigned words)
{ return ceph_cp_snapshot_mmio_copy(s,out,words); }
int cepheus_gpu_cp_protect_configure(struct ceph_cp_protection *s)
{ return ceph_cp_protection_install(s); }
int cepheus_gpu_cp_ring_configure(struct ceph_cp_ring_config *s)
{ return ceph_cp_ring_configure_halted(s); }
int cepheus_gpu_temperature_sample(struct ceph_gpu_tsens *s,struct ceph_gpu_temperature *out)
{ return ceph_gpu_tsens_sample(s,out); }
#if defined(KERNEL)
#include <sys/systm.h>
#include <pexpert/arm64/board_config.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#if defined(SWEET) && (defined(__arm64__) || defined(__aarch64__))
extern int get_preemption_level(void);
static LCK_GRP_DECLARE(ceph_scm_info_group,"ceph_scm_info");
static LCK_MTX_DECLARE(ceph_scm_info_lock,&ceph_scm_info_group);
static unsigned ceph_scm_info_attempted;
static int ceph_scm_info_context(void *unused)
{
    (void)unused;
    return ml_get_interrupts_enabled() && !ml_at_interrupt_context() &&
        get_preemption_level()==0?0:-1;
}
static int ceph_scm_info_invoke(void *unused,struct ceph_scm_regs *r)
{
    (void)unused;
    if(!r || ceph_scm_info_context(NULL))return -1;
    /* No indirect UEFI pointers and no shared-buffer address. This callback
     * is static/private and its sole caller is the fixed discovery engine. */
    register uint64_t x0 __asm__("x0")=r->x[0],x1 __asm__("x1")=r->x[1];
    register uint64_t x2 __asm__("x2")=r->x[2],x3 __asm__("x3")=r->x[3];
    register uint64_t x4 __asm__("x4")=r->x[4],x5 __asm__("x5")=r->x[5];
    register uint64_t x6 __asm__("x6")=r->x[6],x7 __asm__("x7")=r->x[7];
    __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
        "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) : :
        "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
    *r=(struct ceph_scm_regs){{x0,x1,x2,x3,x4,x5,x6,x7}};
    return 0;
}
int cepheus_gpu_scm_probe_once(struct ceph_scm_discovery *out)
{
    if(!out || ceph_scm_info_context(NULL))return -1;
    *out=(struct ceph_scm_discovery){0};
    lck_mtx_lock(&ceph_scm_info_lock);
    if(ceph_scm_info_attempted){lck_mtx_unlock(&ceph_scm_info_lock);return -1;}
    ceph_scm_info_attempted=1; /* also latches errors; no blind repeated SMC */
    struct ceph_scm_discovery_ops b={NULL,ceph_scm_info_context,ceph_scm_info_invoke};
    int e=ceph_scm_discover_pas13(out,&b);
    lck_mtx_unlock(&ceph_scm_info_lock);
    printf("CEPHEUS SCM INFO: e=%d convention=%u available=%#x supported=%u calls=%u transport=%#llx secure=%#llx; no PAS mutations\n",
        e,(unsigned)out->convention,out->available_mask,out->supported,out->calls,
        (unsigned long long)out->transport,(unsigned long long)out->secure);
    return e;
}
#endif
#else
#include <stdio.h>
#endif
int cepheus_gpu_config_probe(const struct ceph_arc_db *db,uint32_t chipid,uint32_t fuse)
{
    struct ceph_gpu_config c;
    int configured=ceph_gpu_config_build(db,chipid,fuse,&c);
    if(configured)return configured; /* keep exact refused stage, not generic -1 */
    printf("CEPHEUS GPU CONFIG: fixed Cepheus FDT profile, observed bin=%u gx=%u bw=%u cnoc=%u ddr=%u initial=%u/%u (NOT SENT)\n",
        c.policy.bin,c.policy.gx_levels,c.bw.levels,c.bw.cnoc_count,c.bw.ddr_count,c.initial_freq,c.initial_bw);
    printf("CEPHEUS GPU CONFIG: profile_sha256=%s live FDT identity still unverified; ARC ownership UNKNOWN\n",CE_GPU_CONFIG_FDT_SHA256);
    uint32_t gmu_vote=0;
    if(ceph_hfi_vote(&c.cx,&c.mx,48,&gmu_vote))return -1;
    printf("CEPHEUS GPU CONFIG: GMU 200MHz requested_min=48 rounded_vlvl=%u cx_index=%u mx_index=%u packed_vote=%#x (NOT SENT)\n",
        (gmu_vote>>16)&0xffff,gmu_vote&0xff,(gmu_vote>>8)&0xff,gmu_vote);
    uint32_t perf[59];
    if(ceph_hfi_perf(perf,59,ceph_config_gx,c.policy.gx_levels,ceph_config_cx,2,&c.gfx,&c.cx,&c.mx)!=59)return -1;
    for(unsigned i=0;i<c.policy.gx_levels;++i)
        printf("CEPHEUS GPU CONFIG: GX index=%u khz=%u packed_vote=%#x (NOT SENT)\n",i,perf[5+3*i],perf[3+3*i]);
    for(unsigned i=0;i<c.bw.ddr_count;++i)
        printf("CEPHEUS GPU CONFIG: DDR cmd%u addr=%#x off=%#x max=%#x wait=%u (NOT SENT)\n",i,c.bw.ddr_addr[i],c.bw.ddr_data[0][i],c.bw.ddr_data[c.bw.levels-1][i],(c.bw.ddr_wait>>i)&1u);
    for(unsigned i=0;i<c.bw.cnoc_count;++i)
        printf("CEPHEUS GPU CONFIG: CNOC cmd%u addr=%#x off=%#x active=%#x wait=%u (NOT SENT)\n",i,c.bw.cnoc_addr[i],c.bw.cnoc_data[0][i],c.bw.cnoc_data[1][i],(c.bw.cnoc_wait>>i)&1u);
    return 0;
}
int cepheus_gpu_qmp_connect(struct ceph_qmp_connect *s,struct ceph_qmp_mmio *m)
{ return ceph_qmp_connect_cold(s,m); }
int cepheus_gpu_upload_mmio_bind(struct ceph_gmu_upload_mmio *m,struct ceph_gmu_upload *out)
{ return ceph_gmu_upload_mmio_bind(m,out); }
int cepheus_gpu_fault_mmio_read(struct ceph_gpu_fault_mmio *m,struct ceph_gpu_fault_record *r)
{ return ceph_gpu_fault_mmio_snapshot(m,r); }
int cepheus_gpu_pdc_mmio_bind(struct ceph_pdc_mmio *m,struct ceph_pdc_ops *out)
{ return ceph_pdc_mmio_ops(m,out); }
int cepheus_gpu_park_read(void *m,enum ceph_gmu_park_reg r,uint32_t *v)
{ return ceph_gmu_mmio_park_read(m,r,v); }
int cepheus_gpu_park_write(void *m,enum ceph_gmu_park_reg r,uint32_t v)
{ return ceph_gmu_mmio_park_write(m,r,v); }
int cepheus_gpu_slumber_read(void *m,enum ceph_slumber_reg r,uint32_t *v)
{ return ceph_gmu_mmio_slumber_read(m,r,v); }
int cepheus_gpu_slumber_write(void *m,enum ceph_slumber_reg r,uint32_t v)
{ return ceph_gmu_mmio_slumber_write(m,r,v); }
int cepheus_gpu_qmp_bind(struct ceph_qmp_mmio *m,unsigned enable,struct ceph_qmp_ops *out)
{ if(!out || ceph_qmp_mmio_bind(m,enable))return -1;return ceph_qmp_mmio_ops(m,out); }
int cepheus_gpu_config_build(const struct ceph_arc_db *db,uint32_t chipid,uint32_t fuse,struct ceph_gpu_config *out)
{ return ceph_gpu_config_build(db,chipid,fuse,out); }
int cepheus_gpu_hfi_boot(struct ceph_hfi_session *s,const struct ceph_hfi_boot_config *c,uint32_t *v)
{ return ceph_hfi_boot_a640(s,c,v); }
int cepheus_gpu_upload(struct ceph_gmu_upload *u,const uint8_t *f,size_t n,struct ceph_gmu_block *p,unsigned capacity)
{ return ceph_firmware_verify(CE_FW_A640_GMU,f,n)?-1:ceph_gmu_upload_file(u,f,n,p,capacity); }
int cepheus_gpu_smmu_install(struct ceph_smmu_install *s,struct ceph_smmu_ops *b,const struct ceph_smmu_plan *p,uint32_t actlr)
{ return ceph_smmu_install_context(s,b,p,actlr); }
int cepheus_gpu_power_start(struct ceph_power_txn *t,struct ceph_power_ops *b)
{ return ceph_power_start(t,b); }
int cepheus_gpu_submit(struct ceph_submit *s,struct ceph_submit_ops *b,uint32_t seq)
{ return ceph_submit_once(s,b,seq); }
int cepheus_gpu_gmu_start(struct ceph_gmu_start *s,struct ceph_gmu_start_ops *b,const struct ceph_gmu_start_config *c)
{ return ceph_gmu_start_cold(s,b,c); }
int cepheus_gpu_ring_submit(struct ceph_gpu_ring *r,uint64_t scratch,uint64_t bytes,uint64_t address,uint32_t value,uint64_t fence,uint32_t sequence)
{ return ceph_gpu_ring_scratch(r,scratch,bytes,address,value,fence,sequence); }
int cepheus_gpu_sqe_copy(const uint8_t *f,size_t bytes,uint8_t *d,size_t capacity,struct ceph_sqe_image *out)
{ return ceph_firmware_verify(CE_FW_A630_SQE,f,bytes)?-1:ceph_sqe_copy(f,bytes,d,capacity,out); }
int cepheus_gpu_bandwidth_table(const struct ceph_bw_route *c,const struct ceph_bw_level *cl,const struct ceph_bw_route *d,const struct ceph_bw_level *dl,unsigned count,struct ceph_hfi_bw *out)
{ return ceph_gpu_bw_table(c,cl,d,dl,count,out); }
int cepheus_gpu_fault_read(struct ceph_gpu_fault_ops *b,struct ceph_gpu_fault_record *r)
{ return ceph_gpu_fault_snapshot(b,r); }
int cepheus_gpu_upload_snapshot(struct ceph_gmu_upload *u,uint64_t handle,struct ceph_gmu_block *plan,unsigned capacity)
{
    struct ceph_gpu_firmware_view view;
    if(cepheus_gpu_firmware_get(handle,CE_FW_A640_GMU,&view))return -1;
    int result=cepheus_gpu_upload(u,view.data,view.bytes,plan,capacity);
    if(cepheus_gpu_memory_view_put(handle))return -1;
    return result;
}
int cepheus_gpu_sqe_snapshot_copy(uint64_t handle,uint8_t *destination,size_t capacity,struct ceph_sqe_image *image)
{
    struct ceph_gpu_firmware_view view;
    if(cepheus_gpu_firmware_get(handle,CE_FW_A630_SQE,&view))return -1;
    int result=cepheus_gpu_sqe_copy(view.data,view.bytes,destination,capacity,image);
    if(cepheus_gpu_memory_view_put(handle))return -1;
    return result;
}
int cepheus_gpu_vm_build(struct ceph_vm_pool *pool,const struct ceph_vm_region *regions,unsigned count)
{ return ceph_vm_build(pool,regions,count); }
int cepheus_gpu_vm_lookup(struct ceph_vm_pool *pool,uint64_t va,uint64_t *pa,uint64_t *flags)
{ return ceph_vm_lookup(pool,va,pa,flags); }
int cepheus_gpu_va_build(const struct ceph_gpu_backing *backing,struct ceph_gpu_va_plan *plan)
{ return ceph_gpu_va_plan(backing,plan); }
int cepheus_gpu_pdc_install(struct ceph_pdc_install *s,struct ceph_pdc_ops *b,const struct ceph_pdc_input *in)
{ return ceph_pdc_install_a640v2(s,b,in); }
int cepheus_gpu_thermal_guard(struct ceph_gpu_thermal *t,const struct ceph_gpu_temperature *s,uint64_t now)
{ return ceph_gpu_thermal_check(t,s,now); }
int cepheus_gpu_cp_init_packet(uint32_t *out,unsigned capacity,uint64_t va,size_t bytes)
{ return ceph_cp_me_init(out,capacity,va,bytes); }
int cepheus_gpu_cp_reglist(struct ceph_cp_snapshot_ops *b,uint32_t *out,unsigned words)
{ return ceph_cp_snapshot(b,out,words); }
int cepheus_gpu_smmu_detach(struct ceph_smmu_detach *s,struct ceph_smmu_detach_ops *b,const struct ceph_smmu_plan *p)
{ return ceph_smmu_detach_context(s,b,p); }
int cepheus_gpu_gmu_park(struct ceph_gmu_park *s,struct ceph_gmu_park_ops *b)
{ return ceph_gmu_park_slumbered(s,b); }
int cepheus_gpu_gmu_slumber(struct ceph_gmu_slumber *s,struct ceph_gmu_slumber_ops *b,struct ceph_hfi_session *h,unsigned bw,unsigned freq,unsigned nb,unsigned ng)
{ return ceph_gmu_slumber_request(s,b,h,bw,freq,nb,ng); }
int cepheus_gpu_acd_message(struct ceph_qmp_tx *s,struct ceph_qmp_ops *b,unsigned enable)
{ return ceph_qmp_send_acd(s,b,enable); }
int cepheus_gpu_smmu_geometry(uint32_t id0,uint32_t id1,uint32_t id2,uint32_t bytes,struct ceph_smmu_geometry *out)
{ return ceph_smmu_geometry(id0,id1,id2,bytes,out); }
int cepheus_gpu_smmu_mmio_read(void *m,enum ceph_smmu_reg r,uint64_t *value)
{ return ceph_smmu_mmio_read(m,r,value); }
int cepheus_gpu_smmu_mmio_write(void *m,enum ceph_smmu_reg r,uint64_t value)
{ return ceph_smmu_mmio_write(m,r,value); }
int cepheus_gpu_oob_acquire(struct ceph_gmu_oob *s,struct ceph_oob_ops *b,uint32_t version)
{ return ceph_gmu_oob_acquire(s,b,version); }
int cepheus_gpu_oob_release(struct ceph_gmu_oob *s,struct ceph_oob_ops *b)
{ return ceph_gmu_oob_release(s,b); }
int cepheus_gpu_gmu_read(void *m,enum ceph_gmu_reg r,uint32_t *v)
{ return ceph_gmu_mmio_start_read(m,r,v); }
int cepheus_gpu_gmu_write(void *m,enum ceph_gmu_reg r,uint32_t v)
{ return ceph_gmu_mmio_start_write(m,r,v); }
int cepheus_gpu_hfi_notify(void *m)
{ return ceph_gmu_mmio_hfi_notify(m); }
int cepheus_gpu_gmu_fault(void *m)
{ return ceph_gmu_mmio_fault(m); }
