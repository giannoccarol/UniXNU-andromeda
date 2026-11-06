/* Deterministic malformed-input corpus; ASan/UBSan catches decoder overruns.
 * This is not exhaustive fuzz coverage and never maps hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gmu_firmware.h"
#include "cepheus_arc.h"
#include "cepheus_bcm.h"
#include "cepheus_hfi.h"
#include "cepheus_gpu_va.h"
#include "cepheus_gpu_vm39.h"
static uint32_t rng=0x640a55;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
struct bytes {uint8_t *data;size_t size;};
static uint32_t rd(void *p,uint32_t off){struct bytes *b=p;assert(!(off&3) && off+4<=b->size);return ceph_gmu_le32(b->data+off);}
static int okay(void *p){(void)p;return 0;}
static uint64_t tables[8*512],baseline[8*512];
int main(void)
{
    uint8_t input[512];struct ceph_gmu_block blocks[32];
    for(unsigned run=0;run<20000;++run) {
        for(unsigned i=0;i<sizeof(input);++i)input[i]=(uint8_t)next();
        size_t length=next()%sizeof(input);unsigned count=99;
        int e=ceph_gmu_plan(input,length,blocks,32,&count);
        if(e)assert(!count);else for(unsigned i=0;i<count;++i)assert(blocks[i].file_offset+blocks[i].size<=length);
        /* Every other record has a valid DB prefix to exercise slave bounds. */
        if(run&1){memset(input,0,8);input[0]=1;input[4]=0xdb;input[5]=0x30;input[6]=3;input[7]=0x0c;input[140]&=0xfe;}
        struct bytes data={input,sizeof(input)};struct ceph_arc_db db={&data,sizeof(input),rd};
        struct ceph_arc_resource arc;struct ceph_bcm_resource bcm;char name[9];
        (void)ceph_arc_lookup(&db,"cx.lvl",&arc);
        (void)ceph_arc_lookup_address(&db,0x30000,name,&arc);
        (void)ceph_bcm_lookup(&db,"SH0",&bcm);
        uint32_t words[16],r=next()%20,w=next()%20,out[16];unsigned n;
        for(unsigned i=0;i<16;++i)words[i]=next();
        struct ceph_hfi_queue q={words,&r,&w,16,2,{NULL,okay,okay,okay,okay},0};
        e=ceph_hfi_read(&q,out,16,&n);if(e)assert(!n);else assert(n<=16 && r<16);
    }
    struct ceph_gpu_backing backing[]={{0x120000000ULL,0x3c000},{0x120040000ULL,0x3c000},
        {0x120080000ULL,0x8000},{0x120090000ULL,0x4000},{0x1200a0000ULL,0x4000}};
    struct ceph_gpu_va_plan layout;assert(!ceph_gpu_va_plan(backing,&layout));
    struct ceph_vm_pool pool={tables,0x110000000ULL,8,0};
    assert(!ceph_vm_build(&pool,layout.regions,CE_BUF_COUNT) && !ceph_vm_matches(&pool,layout.regions,CE_BUF_COUNT));
    memcpy(baseline,tables,sizeof(tables));
    for(unsigned run=0;run<10000;++run) {
        memcpy(tables,baseline,sizeof(tables));
        unsigned slot=next()%(8*512);tables[slot]^=1ULL<<(next()%64);
        assert(ceph_vm_matches(&pool,layout.regions,CE_BUF_COUNT)<0);
    }
    memcpy(tables,baseline,sizeof(tables));
    struct ceph_vm_pool bad=pool;bad.used=65;assert(ceph_vm_matches(&bad,layout.regions,CE_BUF_COUNT)<0);
    bad=pool;bad.pa=(1ULL<<40)-4096;assert(ceph_vm_matches(&bad,layout.regions,CE_BUF_COUNT)<0);
    struct ceph_vm_region gx[CE_BUF_COUNT];
    for(unsigned i=0;i<CE_BUF_COUNT;++i){gx[i]=layout.regions[i];gx[i].va+=(1ULL<<32);}
    pool.used=0;assert(!ceph_vm39_build(&pool,gx,CE_BUF_COUNT) && !ceph_vm39_matches(&pool,gx,CE_BUF_COUNT));
    memcpy(baseline,tables,sizeof(tables));
    for(unsigned run=0;run<10000;++run) {
        memcpy(tables,baseline,sizeof(tables));
        tables[next()%(8*512)]^=1ULL<<(next()%64);
        assert(ceph_vm39_matches(&pool,gx,CE_BUF_COUNT)<0);
    }
    puts("GPU deterministic mutation corpus: 20000 GMU/ARC/BCM/HFI cases PASS (bounded negative coverage)");
    puts("Materialized GMU page tables:10000 single-bit descriptor mutations rejected PASS (ASan/UBSan host coverage)");
    puts("Materialized GX39 page tables:10000 single-bit descriptor mutations rejected PASS (ASan/UBSan host coverage)");
}
