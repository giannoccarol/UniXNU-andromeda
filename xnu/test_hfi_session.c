#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_hfi_boot.h"
struct fixture {
    uint32_t cmd[1024],reply[1024],cr,cw,rr,rw;
    struct ceph_hfi_queue tx,rx,fw_rx,fw_tx;
    struct ceph_hfi_session session;
    uint64_t time;unsigned received,ids[16];int mode,fail_at;
};
enum { NORMAL,SILENT,WRONG_HEADER,FW_ERROR,WRONG_VERSION,BAD_RX_INDEX,TIME_REVERSE,FROZEN,FAULT,
       ASYNC_ERROR,BAD_LENGTH,ALT_ACK_ID };
static int okay(void *p) {(void)p;return 0;}
static int notify(void *p)
{
    struct fixture *f=p;uint32_t cmd[255],reply[4];unsigned n;
    if(f->mode==SILENT || f->mode==FROZEN)return 0;
    assert(!ceph_hfi_read(&f->fw_rx,cmd,255,&n));
    unsigned index=f->received++;assert(index<16);f->ids[index]=cmd[0]&255u;
    unsigned count=(cmd[0]&255u)==1?4:3;
    reply[0]=ceph_hfi_header(cmd[0]&255u,count,1,0);reply[1]=cmd[0];reply[2]=0;
    reply[3]=f->mode==WRONG_VERSION?0x10000000:0x20000042;
    if(f->mode==WRONG_HEADER)reply[1]^=1u<<20;
    if(f->mode==ASYNC_ERROR)reply[0]=ceph_hfi_header(100,count,0,0);
    if(f->mode==ALT_ACK_ID)reply[0]=ceph_hfi_header(126,count,1,0);
    if(f->mode==BAD_LENGTH)reply[0]=ceph_hfi_header(cmd[0]&255u,2,1,0),count=2;
    if(f->mode==FW_ERROR || (f->fail_at && f->received==(unsigned)f->fail_at))reply[2]=9;
    assert(!ceph_hfi_write(&f->fw_tx,reply,count));
    if(f->mode==BAD_RX_INDEX)f->rw=1024;
    return 0;
}
static int now(void *p,uint64_t *n)
{
    struct fixture *f=p;
    if(f->mode!=FROZEN)f->time+=1000000;
    *n=f->mode==TIME_REVERSE && f->time>1000000?0:f->time;return 0;
}
static int fault(void *p) {return ((struct fixture *)p)->mode==FAULT;}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->cr=f->cw=1020;f->rr=f->rw=1020; /* real wrap on first request/reply */
    f->tx=(struct ceph_hfi_queue){f->cmd,&f->cr,&f->cw,1024,2,{f,okay,okay,okay,notify},0};
    f->rx=(struct ceph_hfi_queue){f->reply,&f->rr,&f->rw,1024,2,{f,okay,okay,okay,okay},0};
    f->fw_rx=f->tx;f->fw_rx.io.notify=okay;f->fw_tx=f->rx;
    f->session=(struct ceph_hfi_session){&f->tx,&f->rx,f,now,okay,fault,0,0,0,0};
}
int main(void)
{
    struct fixture f;init(&f);uint32_t cmd[2],reply[16];unsigned n;
    ceph_hfi_fw_version(cmd,2,2,0);
    assert(!ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n) && n==4 && reply[3]==0x20000042);
    assert(f.session.sequence==1 && f.cw==0 && f.rw==0);
    for(int mode=SILENT;mode<=BAD_LENGTH;++mode) {
        if(mode==WRONG_VERSION)continue; /* boot sequence checks the version */
        init(&f);f.mode=mode;
        assert(ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n)<0 && n==0 && f.session.poisoned);
        assert(ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n)==CE_HFI_POISONED);
    }
    init(&f);f.session.sequence=4095;assert(ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n)==CE_HFI_INVALID && !f.received);
    init(&f);f.mode=ALT_ACK_ID;assert(!ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n));
    init(&f);f.cr=0;f.cw=1020; /* full ring: cannot send */
    assert(ceph_hfi_exchange(&f.session,cmd,2,reply,16,&n)==CE_HFI_TIMEOUT && !f.received);
    struct ceph_arc_resource rail={.levels={0,16,48,64},.count=4};
    struct ceph_hfi_opp gx[]={{0,0},{257000000,64},{345000000,64},{427000000,64},
        {499200000,64},{585000000,64},{675000000,64},{770000000,64}},cx[]={{0,0},{200000000,48}};
    struct ceph_hfi_bw bw={.levels=2,.cnoc_count=1,.ddr_count=1,.cnoc_addr={0x50000},.ddr_addr={0x50004}};
    struct ceph_hfi_acd acd={.enable_mask=2,.stride=1,.levels=1,.data={123}};
    struct ceph_gpu_bin_policy policy;assert(!ceph_gpu_bin_policy(0x06040001,0x20000000,&policy));
    struct ceph_hfi_boot_config c={0x20000000,0,0x40,gx,cx,7,2,&rail,&rail,&rail,&bw,&acd,&policy};
    uint32_t version=0;init(&f);
    assert(!ceph_hfi_boot_a640(&f.session,&c,&version) && version==0x20000042);
    const unsigned expected[]={1,4,3,11,7,11,11,14};
    assert(f.received==8 && !memcmp(f.ids,expected,sizeof(expected)));
    for(unsigned fail=1;fail<=8;++fail) {
        init(&f);f.fail_at=(int)fail;
        assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_FW_ERROR && !version && f.received==fail);
    }
    init(&f);f.mode=WRONG_VERSION;assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_PROTOCOL && f.received==1);
    init(&f);c.throttle_mask=0;assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_INVALID && !f.received);
    c.throttle_mask=0x40;c.hfi_version=0x10000000;assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_INVALID && !f.received);
    c.hfi_version=0x20000000;f.rw=0;assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_INVALID && !f.received);
    init(&f);assert(!ceph_gpu_bin_policy(0x06040001,0,&policy));c.ng=8;c.acd=NULL;c.throttle_mask=0x80;
    assert(!ceph_hfi_boot_a640(&f.session,&c,&version));
    const unsigned bin0[]={1,4,3,11,14};assert(f.received==5 && !memcmp(f.ids,bin0,sizeof(bin0)));
    init(&f);c.acd=&acd;assert(ceph_hfi_boot_a640(&f.session,&c,&version)==CE_HFI_INVALID && !f.received);
    assert(ceph_gpu_bin_policy(0x06040001,UINT32_MAX,&policy)<0);
    assert(ceph_gpu_bin_policy(0x06040001,0x40000000,&policy)<0);
    puts("HFI A640 boot: real shared-ring exchange, ACK/version/sequence/error/timeout tests PASS (synthetic firmware)");
}
