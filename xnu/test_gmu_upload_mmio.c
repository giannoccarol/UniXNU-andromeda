#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_upload_mmio.h"
static uint32_t ram[0x30000/4];
struct host {int blocked,done,uncertain;};
static int lease(void *p){return ((struct host *)p)->blocked;}
static void finish(void *p,int uncertain){struct host *h=p;h->done++;h->uncertain=uncertain;}
static void word(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
int main(void)
{
    struct host h={0};struct ceph_gmu_mmio g={(volatile uint8_t *)ram,sizeof(ram),
        {0x06040001,0x60100000,20480,0x60200000,4096},0x20000001,&h,lease};
    struct ceph_gmu_upload_mmio m={&g,&h,finish,0,0};struct ceph_gmu_upload u;
    assert(!ceph_gmu_upload_mmio_bind(&m,&u));
    assert(u.itcm==ram+0x3000/4 && u.dtcm==ram+0x7000/4 && u.itcm_words==4096 && u.dtcm_words==4096);
    ram[0x1f800-0x1a800]=1;
    uint8_t file[40]={0};word(file,0);word(file+4,4);word(file+16,0x12345678);
    word(file+20,0x40000);word(file+24,4);word(file+36,0x87654321);
    struct ceph_gmu_block blocks[4];assert(!ceph_gmu_upload_file(&u,file,sizeof(file),blocks,4));
    assert(ram[0x3000/4]==0x12345678 && ram[0x7000/4]==0x87654321 && u.writes==2 && h.done==1 && !h.uncertain);
    assert(ceph_gmu_upload_mmio_bind(&m,&u)<0 && ceph_gmu_upload_mmio_begin(&m)<0);
    m=(struct ceph_gmu_upload_mmio){&g,&h,finish,0,0};assert(!ceph_gmu_upload_mmio_bind(&m,&u));
    ram[0x1f800-0x1a800]=0;assert(ceph_gmu_upload_file(&u,file,sizeof(file),blocks,4)<0 && !u.writes);
    m=(struct ceph_gmu_upload_mmio){&g,&h,finish,0,0};assert(!ceph_gmu_upload_mmio_bind(&m,&u));
    ram[0x1f800-0x1a800]=1;assert(!ceph_gmu_upload_mmio_begin(&m));h.blocked=1;
    assert(ceph_gmu_upload_mmio_publish(&m)<0);ceph_gmu_upload_mmio_end(&m,1);assert(h.uncertain);
    puts("GMU upload MMIO: exact TCM subwindows/shared aperture, reset+fault lease and publication barriers PASS (RAM)");
}
