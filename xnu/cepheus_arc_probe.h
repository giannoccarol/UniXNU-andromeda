/* Included only by sweet_rpmh.c, after its transport definitions. Reuses the
 * SAME rh_lock as touch power. Never calls rh_write/rh_sync/rh_send.
 */
#ifndef CEPHEUS_ARC_PROBE_H
#define CEPHEUS_ARC_PROBE_H
#include "cepheus_arc.h"
#include "cepheus_bcm.h"
void cepheus_gpu_arc_probe(void);
int cepheus_gpu_fuse_sample(uint32_t *);
int cepheus_gpu_config_probe(const struct ceph_arc_db *,uint32_t,uint32_t);

static uint32_t
ceph_arc_read_db(void *ctx __unused, uint32_t offset)
{
    return rh_read(rh_db, offset);
}

void
cepheus_gpu_arc_probe(void)
{
    if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
        printf("CEPHEUS ARC: transport busy; no retry or writes\n");
        return;
    }
    int error = EIO;
    if (rh_poisoned) goto out;
    error = rh_map();
    if (error) goto out;
    struct ceph_arc_db db = {NULL, rh_db_size, ceph_arc_read_db};
    struct ceph_arc_resource rails[3];
    const char *names[] = {"cx.lvl", "mx.lvl", "gfx.lvl"};
    for (unsigned i=0; i<3; ++i) {
        error = ceph_arc_lookup(&db, names[i], &rails[i]);
        if (error) {
            printf("CEPHEUS ARC: %s lookup error=%d\n", names[i], error);
            goto out;
        }
        printf("CEPHEUS ARC: %s addr=%#x aux_version=%#x priority=%#x:%#x count=%u\n",
            names[i], rails[i].address, rails[i].aux_version,
            rails[i].priority[0], rails[i].priority[1], rails[i].count);
        for (unsigned j=0; j<rails[i].count; ++j)
            printf("CEPHEUS ARC: %s hlvl=%u raw_vlvl=%u\n", names[i], j, rails[i].levels[j]);
    }
    unsigned cx=0, mx=0;
    /* Source minimum is48; cmd-db need not contain it exactly. Apply KGSL's
     * primary ceil and MX ceil of the ROUNDED primary. Proposal only. */
    error = ceph_arc_pair(&rails[0], &rails[1], 48, UINT16_MAX, &cx, &mx);
    if (!error)
        printf("CEPHEUS ARC: 200MHz minimum=48 rounded candidate cx_hlvl=%u raw=%u mx_hlvl=%u raw=%u (NOT SENT)\n",
            cx,rails[0].levels[cx],mx,rails[1].levels[mx]);
    else
        printf("CEPHEUS ARC: no covering MIN_SVS pair error=%d (NOT SENT)\n", error);
    const uint32_t pdc_resources[]={0x30010,0x30000,0x30090};
    for(unsigned i=0;i<3;++i) {
        char resource[9];struct ceph_arc_resource rail;
        int result=ceph_arc_lookup_address(&db,pdc_resources[i],resource,&rail);
        if(result) {printf("CEPHEUS PDC RESOURCE: addr=%#x unresolved error=%d\n",pdc_resources[i],result);continue;}
        printf("CEPHEUS PDC RESOURCE: addr=%#x name=%s levels=%u (no votes)\n",rail.address,resource,rail.count);
        for(unsigned n=0;n<rail.count;++n)printf("CEPHEUS PDC RESOURCE: %s index=%u value=%u\n",resource,n,rail.levels[n]);
    }
    /* Exact resources on the saved live FDT GPU->DDR and GPU->CNOC paths.
     * Address/unit/width/VCD must come from this boot's cmd-db, not defaults. */
    const char *bcms[]={"SH0","MC0","ACV","SH2","SN15","SN3"};
    for(unsigned i=0;i<sizeof(bcms)/sizeof(bcms[0]);++i) {
        struct ceph_bcm_resource bcm;
        struct ceph_bcm_metadata metadata;
        int result=ceph_bcm_inspect(&db,bcms[i],&bcm,i==2,&metadata);
        if(i==2)printf("CEPHEUS BCM META: ACV address=%#x aux_version=%#x aux_offset=%#x aux_bytes=%u bounded_aux_read=%u raw_unit=%#x raw_info=%#x result=%d (evidence only; no votes)\n",
            metadata.address,metadata.aux_version,metadata.aux_offset,metadata.aux_bytes,
            metadata.bounded_aux_read,metadata.unit,metadata.info,result);
        if(result) {printf("CEPHEUS BCM: %s lookup error=%d; no votes\n",bcms[i],result);continue;}
        printf("CEPHEUS BCM: %s address=%#x unit=%u width=%u vcd=%u (no votes)\n",
            bcms[i],bcm.address,bcm.unit,(unsigned)bcm.width,(unsigned)bcm.vcd);
    }
    uint32_t fuse;
    if(cepheus_gpu_fuse_sample(&fuse))
        printf("CEPHEUS GPU CONFIG: skipped: no completed cephfuse sample; bin UNKNOWN\n");
    else {
        int result=cepheus_gpu_config_probe(&db,0x06040001,fuse);
        printf("CEPHEUS GPU CONFIG: dry-run result=%d; no HFI, MMIO writes or ownership grant\n",result);
    }
    for (unsigned t=0; t<RH_NORMAL_N; ++t) {
        uint32_t base=t*RH_TCS_STRIDE;
        uint32_t control=rh_read(rh_tcs,base+RH_CONTROL);
        uint32_t enable=rh_read(rh_tcs,base+RH_ENABLE);
        uint32_t wait=rh_read(rh_tcs,base+RH_WAIT);
        uint32_t status=rh_read(rh_tcs,base+RH_STATUS);
        printf("CEPHEUS ARC: TCS%u %s control=%#x enable=%#x wait=%#x status=%#x\n",
            t, t<2 ? "ACTIVE" : (t<5 ? "SLEEP" : "WAKE"), control, enable, wait, status);
        if (control==UINT32_MAX || wait==UINT32_MAX || status==UINT32_MAX ||
            (enable & ~((1u<<rh_ncpt)-1u))) {
            error=EIO; goto out;
        }
        for (unsigned c=0; c<rh_ncpt; ++c) {
            if (!(enable & (1u<<c))) continue;
            uint32_t offset=base+c*RH_CMD_STRIDE;
            uint32_t address=rh_read(rh_tcs,offset+RH_ADDR);
            for (unsigned r=0; r<3; ++r) {
                if (address!=rails[r].address) continue;
                printf("CEPHEUS ARC: TCS%u cmd%u %s msg=%#x data=%#x status=%#x\n",
                    t,c,names[r],rh_read(rh_tcs,offset+RH_MSGID),
                    rh_read(rh_tcs,offset+RH_DATA),rh_read(rh_tcs,offset+RH_CMD_STATUS));
            }
        }
        if (control!=rh_read(rh_tcs,base+RH_CONTROL) ||
            enable!=rh_read(rh_tcs,base+RH_ENABLE) || wait!=rh_read(rh_tcs,base+RH_WAIT) ||
            status!=rh_read(rh_tcs,base+RH_STATUS)) {
            printf("CEPHEUS ARC: TCS%u changed during snapshot; do not infer ownership\n",t);
            error=EBUSY; goto out;
        }
    }
out:
    printf("CEPHEUS ARC: read-only result=%d; current ARC vote UNKNOWN, no power writes\n",error);
    __atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
}
#endif
