#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_mmio.h"
static uint32_t aperture[0x30000/4];
static int lease(void *p){return *(int *)p;}
int main(void)
{
    int blocked=0;struct ceph_gmu_mmio m={(volatile uint8_t *)aperture,sizeof(aperture),
        {0x06040001,0x60000000,20480,0x60100000,4096},0x20000001,&blocked,lease};
    assert(!ceph_gmu_mmio_start_write(&m,CE_GMU_QTBL_ADDR,0x60000000));
    assert(aperture[0x1f985-0x1a800]==0x60000000);
    assert(ceph_gmu_mmio_start_write(&m,CE_GMU_QTBL_ADDR,0x60001000)<0);
    assert(ceph_gmu_mmio_start_write(&m,CE_GMU_HFI_VERSION,2)<0);
    aperture[0x1f8e8-0x1a800]=0x20;
    assert(!ceph_gmu_mmio_start_write(&m,CE_GMU_RPMH_CTRL,0xf31));
    assert(ceph_gmu_mmio_start_write(&m,CE_GMU_RPMH_CTRL,0xf11)<0);
    uint32_t value;assert(!ceph_gmu_mmio_start_read(&m,CE_GMU_RPMH_CTRL,&value) && value==0xf31);
    assert(!ceph_gmu_mmio_oob_write(&m,CE_OOB_H2G_SET,1u<<30));
    assert(ceph_gmu_mmio_oob_write(&m,CE_OOB_G2H_CLEAR,1)<0);
    assert(!ceph_gmu_mmio_hfi_notify(&m) && aperture[0x1f994-0x1a800]==1);
    assert(!ceph_gmu_mmio_fault(&m));aperture[0x23b05-0x1a800]=1;assert(ceph_gmu_mmio_fault(&m)<0);
    assert(!ceph_gmu_mmio_gx_access(&m));
    for(unsigned bit=2;bit<=7;++bit)if((1u<<bit)&0xd4u) {
        aperture[0x1f8d0-0x1a800]=1u<<bit;assert(ceph_gmu_mmio_gx_access(&m)<0);
    }
    aperture[0x1f8d0-0x1a800]=0;aperture[0x23b10-0x1a800]=2;
    assert(ceph_gmu_mmio_gx_access(&m)<0 && aperture[0x23b10-0x1a800]==2);
    blocked=1;assert(ceph_gmu_mmio_start_write(&m,CE_GMU_SYSRESET,0)<0);
    blocked=0;m.bytes=0x1000;assert(ceph_gmu_mmio_hfi_notify(&m)<0);
    puts("GMU MMIO: shared aperture offsets, exact boot/OOB/HFI values, status-write rejection and lease gates PASS (RAM)");
}
