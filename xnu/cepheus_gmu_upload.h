/* Concrete GMU upload copies: LE32 writes into owned 16KiB ITCM/DTCM windows,
 * bytes into owned CPU mappings for GMU VA regions. No physical address casts.
 * Platform constructs these mappings, verifies firmware digest and pins an
 * immutable file snapshot BEFORE calling. begin/end hold exclusive ownership;
 * held_in_reset reads current CM3 state while power/clocks/mappings stay owned.
 * publish completes MMIO ordering and cache cleaning before any reset release.
 * This routine never releases reset or frees/unmaps uncertain memory.
 */
#ifndef CEPHEUS_GMU_UPLOAD_H
#define CEPHEUS_GMU_UPLOAD_H
#include "cepheus_gmu_firmware.h"
struct ceph_gmu_mapping { uint32_t va,bytes;uint8_t *cpu;enum ceph_gmu_region region; };
struct ceph_gmu_upload {
    volatile uint32_t *itcm,*dtcm;unsigned itcm_words,dtcm_words;
    const struct ceph_gmu_mapping *maps;unsigned map_count;
    void *ctx;
    int (*begin)(void *);
    int (*held_in_reset)(void *);
    int (*publish)(void *);
    void (*end)(void *,int uncertain);
    unsigned writes;int quarantined;
};
static inline int ceph_gmu_upload_target(const struct ceph_gmu_upload *u,
    const struct ceph_gmu_block *b,unsigned *index)
{
    if(b->region==CE_GMU_ITCM) {
        if(!u->itcm || ((uintptr_t)u->itcm&3u) || u->itcm_words!=4096)return -1;
        return 0;
    }
    if(b->region==CE_GMU_DTCM) {
        if(!u->dtcm || ((uintptr_t)u->dtcm&3u) || u->dtcm_words!=4096)return -1;
        return 0;
    }
    unsigned matches=0;
    for(unsigned i=0;i<u->map_count;++i) {
        const struct ceph_gmu_mapping *m=&u->maps[i];
        if(m->region==b->region && b->address>=m->va && b->address-m->va<m->bytes &&
            b->size<=m->bytes-(b->address-m->va)) { *index=i;++matches; }
    }
    return matches==1?0:-1;
}
static inline int ceph_gmu_host_overlap(const void *a,size_t na,const void *b,size_t nb)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if(na>UINTPTR_MAX-x || nb>UINTPTR_MAX-y)return 1;
    return x<y+nb && y<x+na;
}
static inline int ceph_gmu_upload_file(struct ceph_gmu_upload *u,const uint8_t *file,size_t bytes,
    struct ceph_gmu_block *plan,unsigned capacity)
{
    if(!u || u->quarantined || u->writes || !u->begin || !u->held_in_reset || !u->publish ||
        !u->end || u->map_count>16 || (u->map_count && !u->maps))return -1;
    if(!file || !plan || !capacity || capacity>256 || !bytes || bytes>1048576 ||
        ceph_gmu_host_overlap(file,bytes,plan,capacity*sizeof(*plan)))return -1;
    /* Validate every destination and host alias before taking the lease or
     * writing anything. Unknown metadata needs an explicit mapper, not a guess. */
    const void *tcm[]={ (const void *)u->itcm,(const void *)u->dtcm };
    for(unsigned i=0;i<2;++i) if(tcm[i] &&
        (ceph_gmu_host_overlap(file,bytes,tcm[i],16384) ||
         ceph_gmu_host_overlap(plan,capacity*sizeof(*plan),tcm[i],16384)))return -1;
    if(u->itcm && u->dtcm && ceph_gmu_host_overlap(tcm[0],16384,tcm[1],16384))return -1;
    for(unsigned i=0;i<u->map_count;++i) {
        const struct ceph_gmu_mapping *m=&u->maps[i];
        if(!m->cpu || !m->bytes || ((m->va|m->bytes)&4095u) ||
            (m->region!=CE_GMU_ICACHE && m->region!=CE_GMU_DCACHE &&
             m->region!=CE_GMU_KERNEL && m->region!=CE_GMU_USER) ||
            ceph_gmu_region_for(m->va,m->bytes)!=m->region ||
            ceph_gmu_host_overlap(file,bytes,m->cpu,m->bytes) ||
            ceph_gmu_host_overlap(plan,capacity*sizeof(*plan),m->cpu,m->bytes))return -1;
        for(unsigned j=0;j<2;++j)if(tcm[j] && ceph_gmu_host_overlap(tcm[j],16384,m->cpu,m->bytes))return -1;
        for(unsigned j=0;j<i;++j) {
            const struct ceph_gmu_mapping *p=&u->maps[j];
            if((uint64_t)m->va<(uint64_t)p->va+p->bytes && (uint64_t)p->va<(uint64_t)m->va+m->bytes)return -1;
            if(ceph_gmu_host_overlap(m->cpu,m->bytes,p->cpu,p->bytes))return -1;
        }
    }
    unsigned count=0;
    int e=ceph_gmu_plan(file,bytes,plan,capacity,&count);if(e)return e;
    int code=0;
    for(unsigned i=0;i<count;++i) {
        unsigned which=0;
        if(!plan[i].size)return -1; /* current exact A640 image has no metadata */
        if(ceph_gmu_upload_target(u,&plan[i],&which))return -1;
        if(plan[i].region==CE_GMU_ITCM)code=1;
    }
    if(!code)return -1;
    e=u->begin(u->ctx);if(e)return e;
    for(unsigned i=0;i<count;++i) {
        e=u->held_in_reset(u->ctx);if(e)goto done;
        const struct ceph_gmu_block *b=&plan[i];const uint8_t *src=file+b->file_offset;
        if(b->region==CE_GMU_ITCM || b->region==CE_GMU_DTCM) {
            volatile uint32_t *dst=b->region==CE_GMU_ITCM?u->itcm:u->dtcm;
            unsigned offset=(b->address-(b->region==CE_GMU_ITCM?0u:0x40000u))/4;
            for(unsigned j=0;j<b->size/4;++j) {dst[offset+j]=ceph_gmu_le32(src+4*j);++u->writes;}
        } else {
            unsigned which=0;
            e=ceph_gmu_upload_target(u,b,&which);if(e)goto done;
            uint8_t *dst=u->maps[which].cpu+(b->address-u->maps[which].va);
            for(unsigned j=0;j<b->size;++j) {dst[j]=src[j];++u->writes;}
        }
    }
    e=u->publish(u->ctx);
    if(e)goto done;
    /* Compare exactly uploaded ranges AFTER posted writes complete, still
     * under reset/lease. KGSL's TCM snapshot path confirms readable windows.
     * Normal-NC cache backing uses byte loads; TCM remains aligned LE32 MMIO. */
    for(unsigned i=0;i<count;++i) {
        e=u->held_in_reset(u->ctx);if(e)goto done;
        const struct ceph_gmu_block *b=&plan[i];const uint8_t *src=file+b->file_offset;
        if(b->region==CE_GMU_ITCM || b->region==CE_GMU_DTCM) {
            volatile uint32_t *dst=b->region==CE_GMU_ITCM?u->itcm:u->dtcm;
            unsigned offset=(b->address-(b->region==CE_GMU_ITCM?0u:0x40000u))/4;
            for(unsigned j=0;j<b->size/4;++j)if(dst[offset+j]!=ceph_gmu_le32(src+4*j)){e=-1;goto done;}
        } else {
            unsigned which=0;e=ceph_gmu_upload_target(u,b,&which);if(e)goto done;
            volatile uint8_t *dst=u->maps[which].cpu+(b->address-u->maps[which].va);
            for(unsigned j=0;j<b->size;++j)if(dst[j]!=src[j]){e=-1;goto done;}
        }
    }
    e=u->held_in_reset(u->ctx);
done:
    if(e && u->writes)u->quarantined=1;
    u->end(u->ctx,u->quarantined);return e;
}
#endif
