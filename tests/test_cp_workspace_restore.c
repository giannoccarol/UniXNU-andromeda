#include <assert.h>
#include <string.h>
#include "cepheus_cp_workspace.h"
#include "cepheus_cp_snapshot_mmio.h"
#include "cepheus_gx_core.h"
static uint32_t gxregs[0x40000/4],gmuregs[0x30000/4];
static int lease(void *p){(void)p;return 0;}
static void finish(void *p){(void)p;}
static void keep(void *p,int retained){(void)p;assert(retained);}
void ceph_test_cp_workspace_restore(struct ceph_cp_workspace *w,const struct ceph_gmu_start_config *config)
{
    struct ceph_gmu_mmio g={(volatile uint8_t *)gmuregs,sizeof(gmuregs),*config,0x20000001,NULL,lease};gxregs[0x808]=3;
    struct ceph_gx_core core={&g,gxregs,sizeof(gxregs),{0x06040001,0x100000,0x100000,15,32,3},NULL,lease,keep,0,0,0,0};
    assert(!ceph_gx_core_configure(&core));
    struct ceph_cp_protection protection={&g,gxregs,sizeof(gxregs),NULL,lease,keep,0,0,0,0};assert(!ceph_cp_protection_install(&protection));
    struct ceph_cp_snapshot_mmio snap={&g,gxregs,sizeof(gxregs),NULL,lease,finish,0};
    uint32_t packet[12];memset(packet,0xa5,sizeof(packet));
    gxregs[0x842]=0;assert(cepheus_cp_workspace_restore(w,&snap,packet)<0 && packet[0]==0xa5a5a5a5 && !w->restore_ready);gxregs[0x842]=1;
    assert(cepheus_cp_workspace_restore(w,&snap,w->views[CE_CP_RESTORE].cpu)<0);
    assert(cepheus_cp_workspace_restore(w,&snap,(uint32_t *)&snap)<0);
    assert(!cepheus_cp_workspace_restore(w,&snap,packet) && w->restore_ready && packet[1]==0x16f);
    assert(packet[8]==(uint32_t)w->regions[CE_CP_RESTORE].va && packet[9]==(uint32_t)(w->regions[CE_CP_RESTORE].va>>32));
    assert(((uint32_t *)w->views[CE_CP_RESTORE].cpu)[3]==0x4c0084 && !gxregs[0x807] && gxregs[0x808]==3);
    assert(cepheus_cp_workspace_restore(w,&snap,packet)<0);
}
