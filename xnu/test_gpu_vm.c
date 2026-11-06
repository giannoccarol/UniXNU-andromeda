#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include "cepheus_gpu_vm.h"
static uint64_t tables[64*512];
int main(void)
{
	struct ceph_vm_pool pool={tables,0x110000000ULL,64,0};
	struct ceph_vm_region regions[]={{0x4000,0x120000000ULL,0x3c000,CE_VM_READ|CE_VM_EXEC},
	    {0x44000,0x120040000ULL,0x3c000,CE_VM_READ|CE_VM_WRITE},
	    {0x60000000,0x130000000ULL,0x5000,CE_VM_READ|CE_VM_WRITE|CE_VM_UNPRIV|CE_VM_NOCACHE}};
	assert(!ceph_vm_build(&pool,regions,3) && pool.used==5);
	assert(!ceph_vm_matches(&pool,regions,3));
	/* Even a caller-supplied expected map cannot bless physical aliases. */
	uint64_t *check_l2=ceph_vm_table(&pool,tables[0]);assert(check_l2);
	uint64_t *check_l3=ceph_vm_table(&pool,check_l2[0]);assert(check_l3);
	uint64_t saved_leaf=check_l3[4],saved_pa=regions[0].pa;
	regions[0].pa=regions[1].pa;check_l3[4]=ceph_vm_leaf(regions[0].pa,regions[0].permissions);
	assert(ceph_vm_matches(&pool,regions,3)<0);regions[0].pa=saved_pa;check_l3[4]=saved_leaf;
	regions[0].pa=pool.pa;assert(ceph_vm_matches(&pool,regions,3)<0);regions[0].pa=saved_pa;
	tables[63*512]=3;assert(ceph_vm_matches(&pool,regions,3)<0);tables[63*512]=0;
	uint64_t pa,flags;
	assert(!ceph_vm_lookup(&pool,0x4123,&pa,&flags) && pa==0x120000123ULL);
	assert((flags&(3ULL<<53))==0 && (flags&(3u<<6))==(2u<<6));
	assert(!ceph_vm_lookup(&pool,0x60004000,&pa,&flags) && pa==0x130004000ULL);
	assert((flags&(3ULL<<53))==(3ULL<<53));
	assert((flags&(3u<<6))==(1u<<6) && (flags&(7u<<2))==(1u<<2) && (flags&(1u<<11)));
	assert(ceph_vm_lookup(&pool,0x80000,&pa,&flags)==-1);
	assert(ceph_vm_build(&pool,regions,3)==-1); // live reuse refused
	pool.used=0;pool.pages=4;tables[0]=0xabc;
	assert(ceph_vm_build(&pool,regions,3)==-2 && tables[0]==0xabc && pool.used==0);
	pool.pages=64;regions[1].va=0x4000;
	assert(ceph_vm_build(&pool,regions,3)==-1);regions[1].va=0x44000;
	regions[1].pa=regions[0].pa;assert(ceph_vm_build(&pool,regions,3)==-1);
	regions[1].pa=0x120040000ULL;regions[0].permissions|=CE_VM_WRITE;
	assert(!ceph_vm_build(&pool,regions,3)); /* device RWX != CPU RWX */
	assert(!ceph_vm_lookup(&pool,0x4000,&pa,&flags) && !(flags&((3ULL<<53)|(3u<<6))));
	pool.used=0;regions[0].permissions|=32;assert(ceph_vm_build(&pool,regions,3)==-1);
	regions[0].permissions=CE_VM_READ;regions[0].pa=pool.pa;
	assert(ceph_vm_build(&pool,regions,3)==-1);
	assert(!ceph_vm_valid(&(struct ceph_vm_region){0xfffff000,0x12345000,8192,CE_VM_READ}));
	struct ceph_vm_pool bad={0};assert(ceph_vm_lookup(&bad,0,&pa,&flags)==-1);
	assert(!ceph_vm_valid(NULL));
	pool=(struct ceph_vm_pool){tables,0x110000000ULL,64,0};
	struct ceph_vm_region *alias=(struct ceph_vm_region *)(void *)tables;
	*alias=(struct ceph_vm_region){0x4000,0x120000000ULL,4096,CE_VM_READ};
	uint64_t saved=tables[0];assert(ceph_vm_build(&pool,alias,1)==-1 && tables[0]==saved && !pool.used);
	puts("GMU device page tables: walk/AP/XN/aliases/capacity/overflow/preflight: PASS (offline, not CPU pmap)");
}
