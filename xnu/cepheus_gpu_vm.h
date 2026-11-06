/* Offline immutable ARM64 stage-1 GMU page tables: 32-bit VA, 4KiB pages,
 * 3 levels (L1/L2/L3), 40-bit PA. Allocation is caller-owned memory whose
 * physical addresses are supplied by a real allocator, not cast virtuals.
 * Table publication requires cache clean+SMMU context/TLB programming in a
 * platform backend. This module never alters the system SMMU configuration.
 * Permissions govern device transactions, NOT CPU pmap protections. GMU cache
 * backing needs source-faithful RW with no XN; do not impose CPU W^X on it.
 */
#ifndef CEPHEUS_GPU_VM_H
#define CEPHEUS_GPU_VM_H
#include <stddef.h>
#include <stdint.h>
#define CE_VM_PAGE 4096u
#define CE_VM_PA_MASK 0x000000fffffff000ULL
enum { CE_VM_READ=1, CE_VM_WRITE=2, CE_VM_EXEC=4, CE_VM_UNPRIV=8, CE_VM_NOCACHE=16 };
struct ceph_vm_region { uint64_t va, pa, bytes; unsigned permissions; };
struct ceph_vm_pool { uint64_t *words; uint64_t pa; unsigned pages, used; };
static inline int ceph_vm_valid(const struct ceph_vm_region *r)
{
	if (!r || !r->bytes || ((r->va | r->pa | r->bytes) & 4095u) ||
	    r->va >= (1ULL<<32) || r->bytes > (1ULL<<32)-r->va ||
	    r->pa >= (1ULL<<40) || r->bytes > (1ULL<<40)-r->pa ||
	    !(r->permissions & CE_VM_READ) || (r->permissions & ~31u)) { return 0; }
	return 1;
}
static inline uint64_t ceph_vm_leaf(uint64_t pa, unsigned permissions)
{
	/* MAIR[0]=WBWA (0xff), MAIR[1]=normal NC (0x44). nG is mandatory
	 * for ASID-scoped invalidation. HFI is unprivileged RW in kgsl_gmu.c. */
	uint64_t pte=(pa & CE_VM_PA_MASK)|3u|(1u<<10)|(1u<<11)|(2u<<8);
	if (!(permissions & CE_VM_WRITE)) { pte|=2u<<6; }
	if (permissions & CE_VM_UNPRIV) { pte|=1u<<6; }
	if (permissions & CE_VM_NOCACHE) { pte|=1u<<2; }
	if (!(permissions & CE_VM_EXEC)) { pte|=3ULL<<53; }
	return pte;
}
static inline uint64_t *ceph_vm_table(struct ceph_vm_pool *p, uint64_t descriptor)
{
	uint64_t pa=descriptor & CE_VM_PA_MASK;
	if ((descriptor & 3)!=3 || pa<p->pa || (pa-p->pa)/CE_VM_PAGE>=p->used) { return NULL; }
	return p->words+(pa-p->pa)/sizeof(uint64_t);
}
/* Validate region overlap, pool disjointness and total table budget before
 * touching pool memory. Max16 disjoint regions keeps bounded preflight cheap.
 * A new immutable pool only; changing live mappings requires a separate API.
 */
static inline int ceph_vm_build(struct ceph_vm_pool *p,const struct ceph_vm_region *regions,unsigned count)
{
	if (!p || !p->words || !regions || !count || count>16 || p->used ||
	    ((uintptr_t)p->words & 7u) || (p->pa & 4095u) || !p->pages || p->pages>64 ||
	    p->pa>=(1ULL<<40) || (uint64_t)p->pages*CE_VM_PAGE>(1ULL<<40)-p->pa) { return -1; }
	/* Do not zero our own input descriptors while building. Caller supplies
	 * backing size; pointer range checks catch descriptor/pool host aliases. */
	uintptr_t pool_start=(uintptr_t)p->words,desc_start=(uintptr_t)regions;
	size_t pool_bytes=(size_t)p->pages*CE_VM_PAGE,desc_bytes=count*sizeof(*regions);
	if (pool_bytes>UINTPTR_MAX-pool_start || desc_bytes>UINTPTR_MAX-desc_start ||
	    (pool_start<desc_start+desc_bytes && desc_start<pool_start+pool_bytes)) { return -1; }
	/* L1 index 0..3 and L2 index 0..511 require at most 2048 bits. */
	uint8_t l2_seen[4]={0}, l3_seen[256]={0};
	unsigned needed=1;
	for (unsigned i=0;i<count;i++) {
		const struct ceph_vm_region *r=&regions[i];
		if (!ceph_vm_valid(r)) { return -1; }
		if (r->pa < p->pa+(uint64_t)p->pages*CE_VM_PAGE && p->pa<r->pa+r->bytes) { return -1; }
		for (unsigned j=0;j<i;j++) {
			if (r->va<regions[j].va+regions[j].bytes && regions[j].va<r->va+r->bytes) { return -1; }
			// Reject physical aliases too: one device VA/permission set per backing.
			if (r->pa<regions[j].pa+regions[j].bytes && regions[j].pa<r->pa+r->bytes) { return -1; }
		}
		for (uint64_t va=r->va;va<r->va+r->bytes;) {
			unsigned l1=(unsigned)(va>>30), slot=(unsigned)(va>>21);
			if (!l2_seen[l1]) { l2_seen[l1]=1;needed++; }
			if (!(l3_seen[slot/8] & (1u<<(slot%8)))) { l3_seen[slot/8]|=(uint8_t)(1u<<(slot%8));needed++; }
			if (needed>p->pages) { return -2; }
			va=((va>>21)+1)<<21;
		}
	}
	for (unsigned i=0;i<p->pages*512u;i++) { p->words[i]=0; }
	p->used=1;
	for (unsigned i=0;i<count;i++) {
		const struct ceph_vm_region *r=&regions[i];
		for (uint64_t offset=0;offset<r->bytes;offset+=CE_VM_PAGE) {
			uint64_t va=r->va+offset;
			uint64_t *table=p->words;
			const unsigned indices[]={(unsigned)(va>>30),(unsigned)((va>>21)&511u),(unsigned)((va>>12)&511u)};
			for (unsigned level=0;level<2;level++) {
				if (!table[indices[level]]) {
					table[indices[level]]=(p->pa+(uint64_t)p->used++*CE_VM_PAGE)|3u;
				}
				table=ceph_vm_table(p,table[indices[level]]);
				if (!table) { p->used=0;return -1; } // preflight makes this unreachable
			}
			table[indices[2]]=ceph_vm_leaf(r->pa+offset,r->permissions);
		}
	}
	return 0;
}
static inline int ceph_vm_lookup(struct ceph_vm_pool *p,uint64_t va,uint64_t *pa,uint64_t *flags)
{
	if (!p || !p->words || !p->used || p->used>p->pages || p->pages>64 ||
	    ((uintptr_t)p->words & 7u) || (p->pa & 4095u) || va>=(1ULL<<32) || !pa || !flags) { return -1; }
	uint64_t *t=ceph_vm_table(p,p->words[va>>30]);if(!t)return -1;
	t=ceph_vm_table(p,t[(va>>21)&511u]);if(!t)return -1;
	uint64_t v=t[(va>>12)&511u];if((v&3)!=3)return -1;
	*pa=(v&CE_VM_PA_MASK)|(va&4095u);*flags=v&~CE_VM_PA_MASK;return 0;
}
/* Verify the entire materialized table, not just first/last translated bytes.
 * Reject extra mappings, block descriptors, hidden permission bits, aliasing
 * table pointers and unreachable nonzero pages. No allocation or scratch page
 * array; at most64 table pages/32768 descriptors are inspected. */
static inline int ceph_vm_matches_limit(struct ceph_vm_pool *p,const struct ceph_vm_region *regions,unsigned count,unsigned va_bits)
{
    if(!p || !p->words || ((uintptr_t)p->words&7u) || (p->pa&4095u) ||
        !p->used || p->used>p->pages || p->pages>64 || p->pa>=(1ULL<<40) ||
        (uint64_t)p->pages*4096>(1ULL<<40)-p->pa || !regions || !count || count>16 || (va_bits!=32 && va_bits!=39))return -1;
    uint64_t wanted=0,leaves=0,seen=1;
    for(unsigned i=0;i<count;++i) {
        struct ceph_vm_region r=regions[i];
        if(r.va>=(1ULL<<va_bits) || r.bytes>(1ULL<<va_bits)-r.va || (r.va&4095u))return -1;
        /* Reuse PA/alignment/permission validation without widening GMU32. */
        r.va=0;if(!ceph_vm_valid(&r) || (va_bits==39 && r.bytes>256*1024))return -1;
        if(r.pa<p->pa+(uint64_t)p->pages*4096 && p->pa<r.pa+r.bytes)return -1;
        for(unsigned j=0;j<i;++j)
            if((regions[i].va<regions[j].va+regions[j].bytes && regions[j].va<regions[i].va+regions[i].bytes) ||
                (r.pa<regions[j].pa+regions[j].bytes && regions[j].pa<r.pa+r.bytes))return -1;
        wanted+=regions[i].bytes/4096;
    }
    for(unsigned l1=0;l1<512;++l1) {
        uint64_t d=p->words[l1];if(!d)continue;
        if(l1>=(1u<<(va_bits-30)) || d!=((d&CE_VM_PA_MASK)|3u))return -1;
        uint64_t *l2=ceph_vm_table(p,d);if(!l2)return -1;
        unsigned page=(unsigned)(((d&CE_VM_PA_MASK)-p->pa)/4096);
        if(seen&(1ULL<<page))return -1;
        seen|=1ULL<<page;
        for(unsigned j=0;j<512;++j) {
            d=l2[j];if(!d)continue;
            if(d!=((d&CE_VM_PA_MASK)|3u))return -1;
            uint64_t *l3=ceph_vm_table(p,d);if(!l3)return -1;
            page=(unsigned)(((d&CE_VM_PA_MASK)-p->pa)/4096);
            if(seen&(1ULL<<page))return -1;
            seen|=1ULL<<page;
            for(unsigned k=0;k<512;++k) {
                uint64_t actual=l3[k];if(!actual)continue;
                uint64_t va=((uint64_t)l1<<30)|((uint64_t)j<<21)|((uint64_t)k<<12),expected=0;
                unsigned matches=0;
                for(unsigned i=0;i<count;++i)if(va>=regions[i].va && va-regions[i].va<regions[i].bytes) {
                    expected=ceph_vm_leaf(regions[i].pa+va-regions[i].va,regions[i].permissions);++matches;
                }
                if(matches!=1 || actual!=expected)return -1;
                ++leaves;
            }
        }
    }
    if(leaves!=wanted || seen!=(p->used==64?UINT64_MAX:(1ULL<<p->used)-1))return -1;
    for(unsigned i=p->used*512;i<p->pages*512;++i)if(p->words[i])return -1;
    return 0;
}
static inline int ceph_vm_matches(struct ceph_vm_pool *p,const struct ceph_vm_region *regions,unsigned count)
{return ceph_vm_matches_limit(p,regions,count,32);}
#endif
