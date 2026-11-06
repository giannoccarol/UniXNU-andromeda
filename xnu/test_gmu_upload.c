#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cepheus_gmu_upload.h"
static uint32_t itcm[4096],dtcm[4096];static uint8_t icache[0x3c000],dcache[0x3c000];
struct mock {unsigned calls,fail,end,uncertain,corrupt;};
static int step(void *p) {struct mock *m=p;return ++m->calls==m->fail?-9:0;}
static int publish(void *p){struct mock *m=p;
    if(m->corrupt==1)itcm[0]^=1;
    if(m->corrupt==2)dcache[0x13000]^=1;
    return step(p);}
static void end(void *p,int uncertain) {struct mock *m=p;m->end++;m->uncertain=(unsigned)uncertain;}
static struct ceph_gmu_upload config(struct mock *m,const struct ceph_gmu_mapping *maps)
{return (struct ceph_gmu_upload){itcm,dtcm,4096,4096,maps,2,m,step,step,publish,end,0,0};}
static void le32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static void verify_block(const uint8_t *file,const struct ceph_gmu_block *b)
{
    const uint8_t *want=file+b->file_offset;
    if(b->region==CE_GMU_ITCM || b->region==CE_GMU_DTCM) {
        const uint32_t *p=b->region==CE_GMU_ITCM?itcm:dtcm;
        unsigned off=(b->address-(b->region==CE_GMU_ITCM?0:0x40000))/4;
        for(unsigned j=0;j<b->size/4;++j)assert(p[off+j]==ceph_gmu_le32(want+j*4));
    } else {
        const uint8_t *p=b->region==CE_GMU_ICACHE?icache:dcache;
        unsigned off=b->address-(b->region==CE_GMU_ICACHE?0x4000:0x44000);
        assert(!memcmp(p+off,want,b->size));
    }
}
int main(int argc,char **argv)
{
    struct ceph_gmu_mapping maps[]={{0x4000,sizeof(icache),icache,CE_GMU_ICACHE},
        {0x44000,sizeof(dcache),dcache,CE_GMU_DCACHE}};
    uint8_t sample[80]={0};const unsigned addrs[]={0,0x40000,0x4000,0x57000};
    for(unsigned i=0;i<4;++i) {le32(sample+20*i,addrs[i]);le32(sample+20*i+4,4);le32(sample+20*i+16,0x12345678+i);}
    struct mock m={0};struct ceph_gmu_upload u=config(&m,maps);struct ceph_gmu_block plan[32];
    assert(!ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32) && u.writes==10 && m.end==1 && !m.uncertain);
    for(unsigned i=0;i<4;++i)verify_block(sample,&plan[i]);
    unsigned calls=m.calls;
    for(unsigned f=1;f<=calls;++f) {
        memset(itcm,0,sizeof(itcm));m=(struct mock){.fail=f};u=config(&m,maps);
        assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)==-9);
        if(f==1)assert(!m.end && !u.writes);else assert(m.end==1 && m.uncertain==(u.writes!=0));
    }
    for(unsigned corrupt=1;corrupt<=2;++corrupt) {
        m=(struct mock){.corrupt=corrupt};u=config(&m,maps);
        assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)<0 && u.quarantined && m.uncertain && m.end==1);
    }
    m=(struct mock){0};u=config(&m,maps);u.map_count=1;
    assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)<0 && !m.calls && !u.writes);
    u=config(&m,maps);u.dtcm=u.itcm;
    assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)<0 && !m.calls);
    u=config(&m,maps);u.dtcm_words=4095;
    assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)<0 && !m.calls);
    u=config(&m,maps);assert(ceph_gmu_upload_file(&u,sample,79,plan,32)<0 && !m.calls);
    u=config(&m,maps);itcm[0]=0xabc;
    assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),(struct ceph_gmu_block *)(void *)itcm,32)<0 && itcm[0]==0xabc && !m.calls);
    struct ceph_gmu_mapping badmaps[2]={maps[0],maps[1]};badmaps[1].region=CE_GMU_BAD;badmaps[1].va=0xf0000000;
    u=config(&m,badmaps);assert(ceph_gmu_upload_file(&u,sample,sizeof(sample),plan,32)<0 && !m.calls);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>0 && length<=1048576);
        rewind(f);uint8_t *file=malloc((size_t)length);assert(file);assert(fread(file,1,(size_t)length,f)==(size_t)length);fclose(f);
        m=(struct mock){0};u=config(&m,maps);
        assert(!ceph_gmu_upload_file(&u,file,(size_t)length,plan,32));
        unsigned n;assert(!ceph_gmu_plan(file,(size_t)length,plan,32,&n));
        for(unsigned i=0;i<n;++i)verify_block(file,&plan[i]);
        printf("Local GMU image: %u blocks copied and compared against RAM-backed TCM/cache windows (NOT device)\n",n);
        free(file);
    }
    puts("GMU upload: actual LE stores/mapped copies, all-target preflight, reset-loss quarantine PASS (RAM windows)");
}
