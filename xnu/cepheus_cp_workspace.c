#include "cepheus_cp_workspace.h"
#include "cepheus_gpu_compile.h"
#include "cepheus_gpu_firmware.h"

int cepheus_cp_workspace_validate(struct ceph_cp_workspace *w)
{
    if(!w || (w->state!=CE_CP_WS_READY && w->state!=CE_CP_WS_TABLED) ||
        w->leased!=((1u<<CE_CP_BUFFERS)-1) || w->firmware || w->firmware_view ||
        w->sqe.offset!=4 || w->sqe.bytes!=32300 || w->sqe.words!=8075 || w->sqe.version!=0x16ee190)return -1;
    const size_t minimum[]={32768,4096,32768,4096,4096,131072};
    for(unsigned i=0;i<CE_CP_BUFFERS;++i) {
        struct ceph_gpu_memory_view actual;
        if(cepheus_gpu_memory_view_get(w->handles[i],&actual))return -1;
        int invalid=actual.cpu!=w->views[i].cpu || actual.pa!=w->views[i].pa || actual.bytes!=w->views[i].bytes ||
            actual.bytes<minimum[i] || actual.bytes>((minimum[i]+16383)&~(size_t)16383);
        if(cepheus_gpu_memory_view_put(w->handles[i]) || invalid)return -1;
        for(unsigned j=0;j<i;++j)if(w->handles[j]==w->handles[i])return -1;
    }
    static const uint8_t expected[32]={0xec,0x4c,0x62,0xfc,0x9d,0x38,0x44,0x33,0x7d,0x54,0xfc,0x6a,0x04,0xfe,0x01,0x82,
        0x9e,0xff,0xf3,0x77,0x9f,0xe0,0x3b,0xcc,0xe9,0x6e,0xe8,0x7d,0x2d,0x52,0x3c,0xe0};
    uint8_t digest[32];if(ceph_firmware_sha256(w->views[CE_CP_SQE].cpu,32300,digest))return -1;
    unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=digest[i]^expected[i];if(diff)return -1;
    const volatile uint8_t *sqe=w->views[CE_CP_SQE].cpu;
    for(size_t i=32300;i<w->views[CE_CP_SQE].bytes;++i)if(sqe[i])return -1;
    if(w->state==CE_CP_WS_TABLED) {
        if(w->table.words!=w->views[CE_CP_ROOT].cpu || w->table.pa!=w->views[CE_CP_ROOT].pa || w->table.pages!=32)return -1;
        for(unsigned i=0;i<CE_CP_ROOT;++i)if(w->regions[i].pa!=w->views[i].pa ||
            w->regions[i].bytes!=minimum[i] || w->regions[i].permissions!=ceph_cp_workspace_permissions(i) || w->regions[i].va<0x300000)return -1;
        if(ceph_vm39_matches(&w->table,w->regions,CE_CP_ROOT))return -1;
    } else if(w->table.words || w->table.used)return -1;
    return 0;
}

int cepheus_cp_workspace_release(struct ceph_cp_workspace *w)
{
    if(!w || (w->state!=CE_CP_WS_BUILDING && w->state!=CE_CP_WS_READY && w->state!=CE_CP_WS_TABLED))return -1;
    int e=0;
    if(w->firmware_view) {
        if(cepheus_gpu_memory_view_put(w->firmware))e=-1;
        else w->firmware_view=0;
    }
    if(w->firmware && !w->firmware_view) {
        if(cepheus_gpu_memory_release_cpu(w->firmware))e=-1;
        else w->firmware=0;
    }
    for(unsigned i=CE_CP_BUFFERS;i>0;--i) {
        unsigned n=i-1;
        if(w->leased&(1u<<n)) {
            if(cepheus_gpu_memory_view_put(w->handles[n])){e=-1;continue;}
            w->leased&=~(1u<<n);w->views[n]=(struct ceph_gpu_memory_view){0};
        }
        if(w->handles[n]) {
            if(cepheus_gpu_memory_release_cpu(w->handles[n]))e=-1;
            else w->handles[n]=0;
        }
    }
    w->sqe=(struct ceph_sqe_image){0};w->table=(struct ceph_vm_pool){0};w->restore_ready=0;
    for(unsigned i=0;i<CE_CP_ROOT;++i)w->regions[i]=(struct ceph_vm_region){0};
    w->state=e?CE_CP_WS_RETAINED:CE_CP_WS_RELEASED;return e;
}
int cepheus_cp_workspace_prepare(struct ceph_cp_workspace *w,const uint8_t *file,size_t bytes)
{
    if(!w || w->state!=CE_CP_WS_NEW || w->leased || w->firmware || w->firmware_view)return -1;
    for(unsigned i=0;i<CE_CP_BUFFERS;++i)if(w->handles[i])return -1;
    w->state=CE_CP_WS_BUILDING;
    if(cepheus_gpu_firmware_snapshot(1,file,bytes,&w->firmware))goto failed;
    static const size_t sizes[]={32768,4096,32768,4096,4096,32*4096};
    for(unsigned i=0;i<CE_CP_BUFFERS;++i) {
        if(cepheus_gpu_memory_alloc(sizes[i],&w->handles[i]) ||
            cepheus_gpu_memory_view_get(w->handles[i],&w->views[i]))goto failed;
        w->leased|=1u<<i;
    }
    struct ceph_gpu_firmware_view view;
    if(cepheus_gpu_firmware_get(w->firmware,1,&view))goto failed;
    w->firmware_view=1;
    if(ceph_sqe_copy(view.data,view.bytes,w->views[CE_CP_SQE].cpu,w->views[CE_CP_SQE].bytes,&w->sqe) ||
        w->sqe.bytes!=32300 || w->sqe.version!=0x16ee190)goto failed;
    /* SHA-verified source snapshot remains pinned through byte comparison.
     * Trailing zero bytes in32KiB SQE backing remain from pool allocation. */
    const volatile uint8_t *copy=w->views[CE_CP_SQE].cpu;
    for(size_t i=0;i<w->sqe.bytes;++i)if(copy[i]!=view.data[i+4])goto failed;
    if(cepheus_gpu_memory_view_put(w->firmware))goto failed;
    w->firmware_view=0;
    if(cepheus_gpu_memory_release_cpu(w->firmware))goto failed;
    w->firmware=0;w->state=CE_CP_WS_READY;return 0;
failed:
    (void)cepheus_cp_workspace_release(w);return -1;
}

int cepheus_cp_workspace_table(struct ceph_cp_workspace *w,const uint64_t va[CE_CP_ROOT])
{
    if(!w || w->state!=CE_CP_WS_READY || !va || w->leased!=((1u<<CE_CP_BUFFERS)-1) ||
        w->firmware || w->table.words)return -1;
    if(cepheus_cp_workspace_validate(w))return -1;
    static const size_t bytes[]={32768,4096,32768,4096,4096};
    /* KGSL: ring GPUREADONLY; SQE GPUREADONLY|UCODE; scratch/RPTR and powerup
     * list privileged RW. None is host-coherent: Normal-NC backing/MAIR. */
    struct ceph_vm_region regions[CE_CP_ROOT];
    for(unsigned i=0;i<CE_CP_ROOT;++i) {
        /* Keep low GMEM/trap/null area out of system-memory tables. Current
         * A6401MiB GMEM occupies[1MiB,2MiB); KGSL begins SVM at3MiB. */
        if(va[i]<0x300000 || w->views[i].bytes<bytes[i])return -1;
        regions[i]=(struct ceph_vm_region){va[i],w->views[i].pa,bytes[i],ceph_cp_workspace_permissions(i)};
    }
    const struct ceph_gpu_memory_view *root=&w->views[CE_CP_ROOT];
    if(!root->cpu || root->bytes!=32*4096)return -1;
    struct ceph_vm_pool p={root->cpu,root->pa,32,0};
    if(ceph_vm39_build(&p,regions,CE_CP_ROOT) || ceph_vm39_matches(&p,regions,CE_CP_ROOT))return -1;
    for(unsigned i=0;i<CE_CP_ROOT;++i)w->regions[i]=regions[i];
    w->table=p;w->state=CE_CP_WS_TABLED;return 0;
}

int cepheus_cp_workspace_restore(struct ceph_cp_workspace *w,struct ceph_cp_snapshot_mmio *mmio,uint32_t packet[12])
{
    if(!w || w->state!=CE_CP_WS_TABLED || w->restore_ready || !packet || !mmio || cepheus_cp_workspace_validate(w))return -1;
    uintptr_t dst=(uintptr_t)packet;
    if((dst&3u) || 48>UINTPTR_MAX-dst)return -1;
    /* Caller packet buffer cannot alias any backing or lease object. */
    for(unsigned i=0;i<CE_CP_BUFFERS;++i) {
        uintptr_t base=(uintptr_t)w->views[i].cpu;
        if(dst<base+w->views[i].bytes && base<dst+48)return -1;
    }
    uintptr_t self=(uintptr_t)w,regs=(uintptr_t)mmio->gpu,adapter=(uintptr_t)mmio;
    if(sizeof(*w)>UINTPTR_MAX-self || sizeof(*mmio)>UINTPTR_MAX-adapter ||
        (dst<adapter+sizeof(*mmio) && adapter<dst+48) ||
        (dst<self+sizeof(*w) && self<dst+48) || mmio->bytes>UINTPTR_MAX-regs ||
        (dst<regs+mmio->bytes && regs<dst+48))return -1;
    uint32_t local_packet[12],snapshot[CE_CP_REGLIST_WORDS];
    if(ceph_cp_me_init(local_packet,12,w->regions[CE_CP_RESTORE].va,w->regions[CE_CP_RESTORE].bytes)!=12 ||
        ceph_cp_snapshot_mmio_copy(mmio,snapshot,CE_CP_REGLIST_WORDS) || cepheus_cp_workspace_validate(w))return -1;
    uint32_t *target=w->views[CE_CP_RESTORE].cpu;
    for(unsigned i=0;i<CE_CP_REGLIST_WORDS;++i)target[i]=snapshot[i];
    for(unsigned i=0;i<12;++i)packet[i]=local_packet[i];
    w->restore_ready=1;return 0;
}
