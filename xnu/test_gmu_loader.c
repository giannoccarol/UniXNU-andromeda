#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "cepheus_gmu_loader.h"
struct mock { unsigned calls, fail_at, cleans; int uncertain; };
static int step(void *p) { struct mock *m=p; return ++m->calls==m->fail_at ? -9 : 0; }
static int maps(void *p,const struct ceph_gmu_block *b,unsigned n) { assert(n==1 && b[0].size==4);return step(p); }
static int block(void *p,uint32_t a,enum ceph_gmu_region r,const uint8_t *d,uint32_t n)
{ assert(a==0 && r==CE_GMU_ITCM && n==4 && d[0]==0x42);return step(p); }
static int init(void *p,uint32_t expected,unsigned ms) { assert(expected==0xbabefaceu && ms==100);return step(p); }
static int hfi(void *p,unsigned ms) { assert(ms==100);return step(p); }
static int cleanup(void *p,enum ceph_gmu_stage s) { struct mock *m=p;assert(s>=CE_GMU_POWER);m->cleans++;return m->uncertain; }
int main(void)
{
	uint8_t fw[20]={0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0x42,0,0,0};
	struct ceph_gmu_block plan[2];
	for(unsigned fail=0;fail<=10;++fail) {
		struct mock m={0,fail,0,fail==8}; struct ceph_gmu_loader l={0};
		struct ceph_gmu_backend b={&m,step,maps,step,step,block,step,step,step,init,hfi,cleanup};
		int e=ceph_gmu_boot(&l,&b,fw,sizeof(fw),plan,2);
		if(!fail) { assert(!e && l.stage==CE_GMU_HFI && m.calls==10 && !m.cleans); }
		else { assert(e==-9 && l.stage==CE_GMU_FAILED && m.cleans==1);assert(l.quarantined==(fail==8)); }
		unsigned calls=m.calls; assert(ceph_gmu_boot(&l,&b,fw,sizeof(fw),plan,2)==-1 && m.calls==calls);
	}
	struct mock m={0};struct ceph_gmu_loader l={0};
	struct ceph_gmu_backend b={&m,step,maps,step,step,block,step,step,step,init,hfi,cleanup};
	assert(ceph_gmu_boot(&l,&b,fw,19,plan,2)<0 && m.calls==0);
	uint8_t metadata[16]={0};
	assert(ceph_gmu_boot(&l,&b,metadata,sizeof(metadata),plan,2)<0 && m.calls==0);
	puts("GMU staged loader: success, each failure, cleanup/quarantine and preflight: PASS (mock)");
}
