#include <assert.h>
#include <stdio.h>
#include "cepheus_gx_va_layout.h"
int main(void)
{
    struct ceph_gx_va_partition p;
    /* This device has gfx3d_secure, so secured=1 with the default 256MiB. */
    assert(!ceph_gx_va_partition(1,CE_GX_SECURE_SIZE,&p));
    assert(p.secure_context_present==1);
    assert(p.secure.start==0xec000000ull && p.secure.end==0xfc000000ull);
    assert(p.user32.start==0x300000ull && p.user32.end==0xec000000ull);
    assert(p.user64.start==CE_GX_VA_BASE64_LO && p.user64.end==CE_GX_VA_BASE64_HI);
    /* The user window must not overlap the secure window. */
    assert(p.user32.end<=p.secure.start && p.secure.end<=p.user64.start);
    /* The GMU workspace VA (0x60000000) is inside user32 and stays untouched. */
    assert(ceph_gx_va_fits(&p,0x60000000,0x30000)>=0);
    /* A mapping straddling the secure boundary is refused, not clamped. */
    assert(ceph_gx_va_fits(&p,0xebfff000,0x2000)<0);
    assert(ceph_gx_va_fits(&p,0xec000000,0x1000)<0);
    assert(ceph_gx_va_fits(&p,0xfc000000,0x1000)<0);
    /* 64-bit window works, but only with 48-bit tables. */
    assert(ceph_gx_va_fits(&p,CE_GX_VA_BASE64_LO,0x1000)>=0);
    assert(ceph_gx_va_plan_check(&p,48)==0);
    /* The 64-bit window ends at 24GiB, so a 39-bit table (512GiB) can address
     * it; a narrower 36-bit table (64GiB) still can, but 34-bit (16GiB) cannot. */
    assert(ceph_gx_va_plan_check(&p,39)==0);
    assert(ceph_gx_va_plan_check(&p,34)<0);
    assert(ceph_gx_va_plan_check(&p,0)<0);
    /* Unaligned/overflowing/empty requests are refused. */
    assert(ceph_gx_va_fits(&p,0x60000001,0x1000)<0);
    assert(ceph_gx_va_fits(&p,0x60000000,0)<0);
    assert(ceph_gx_va_fits(&p,0xfffffffffffff000ull,0x2000)<0);
    /* Without a secure CB the whole 32-bit window belongs to user space. */
    assert(!ceph_gx_va_partition(0,0,&p));
    assert(!p.secure_context_present && p.user32.end==CE_GX_GLOBAL_MEM_BASE64);
    assert(ceph_gx_va_fits(&p,0xf0000000,0x1000)>=0);
    assert(ceph_gx_va_plan_check(&p,48)==0);
    /* Bad secure sizes are refused rather than silently defaulted. */
    assert(ceph_gx_va_partition(1,0,&p)<0);
    assert(ceph_gx_va_partition(1,CE_GX_SECURE_SIZE+0x1000,&p)<0);
    assert(ceph_gx_va_partition(1,0x1000,&p)==0);          /* 4KiB is legal */
    assert(ceph_gx_va_partition(1,0x1001,&p)<0);           /* unaligned */
    assert(ceph_gx_va_partition(2,CE_GX_SECURE_SIZE,&p)<0);
    /* A 256MiB secure window is the largest the FDT-less default allows. */
    assert(ceph_gx_va_partition(1,0xec000000ull,&p)<0);
    puts("GX VA partition: secure CB carve-out, no overlap, geometry bounds, GMU VA untouched PASS");
}
