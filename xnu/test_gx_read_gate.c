#include <assert.h>
#include <stdio.h>
/* Extracted precondition logic of gpu_gx_probe() so the read gate is testable
 * without XNU. Mirrors xnu/cepheus_soc.c exactly: the GX aperture is only read
 * when GX/SPTP power+clock are on and the AHB fence is in ALLOW. */
static int gx_readable(unsigned state,unsigned fence,unsigned max)
{
    if(state==max || fence==max)return -1;
    if(state&0xd4u)return 0;
    if(fence)return 0;
    return 1;
}
int main(void)
{
    assert(gx_readable(0,0,~0u)==1);
    assert(gx_readable(0xc0,0,~0u)==0);      /* powered down */
    assert(gx_readable(0x04,0,~0u)==0);      /* SPTP off */
    assert(gx_readable(0x10,0,~0u)==0);      /* SP clock off */
    assert(gx_readable(0,1,~0u)==0);         /* fence not ALLOW */
    assert(gx_readable(~0u,0,~0u)==-1);      /* unreadable */
    assert(gx_readable(0,~0u,~0u)==-1);
    puts("GX read gate: refuses powered-down/unfenced apertures, only ALLOW+powered reads PASS");
}
