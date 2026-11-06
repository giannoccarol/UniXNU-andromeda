#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gpu_config.h"
#include "../tests/gpu_config_fixture.h"
/* Machine-readable independent comparison target for saved FDT tests. */
static void report(void)
{
    printf("{\"sha\":\"%s\",\"gx\":[",CE_GPU_CONFIG_FDT_SHA256);
    for(unsigned i=0;i<8;++i)printf("%s[%u,%u]",i?",":"",ceph_config_gx[i].hz,ceph_config_gx[i].raw_vlvl);
    printf("],\"cx\":[[0,0],[%u,%u]],\"ddr\":[",ceph_config_cx[1].hz,ceph_config_cx[1].raw_vlvl);
    for(unsigned i=0;i<12;++i)printf("%s[%llu,%llu]",i?",":"",(unsigned long long)ceph_config_ddr[i].ab,(unsigned long long)ceph_config_ddr[i].ib);
    printf("],\"cnoc\":[[0,0],[0,%llu]],\"routes\":[",(unsigned long long)ceph_config_cnoc[1].ib);
    for(unsigned which=0;which<2;++which)for(unsigned i=0;i<3;++i) {
        const struct ceph_bw_node *n=which?&ceph_config_cnoc_nodes[i]:&ceph_config_ddr_nodes[i];
        printf("%s[\"%s\",%u,%u]",(which||i)?",":"",which?ceph_config_cnoc_names[i]:ceph_config_ddr_names[i],n->buswidth,n->ports);
    }
    printf("],\"acd\":[%u,%u,%u",ceph_config_acd.enable_mask,ceph_config_acd.stride,ceph_config_acd.levels);
    for(unsigned i=0;i<12;++i)printf(",%u",ceph_config_acd.data[i]);
    puts("]}");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--report")){report();return 0;}
    config_fixture();struct ceph_arc_db db={config_db,sizeof(config_db),config_read};
    struct ceph_gpu_config c={0};struct ceph_hfi_boot_config hfi;
    for(unsigned bin=0;bin<2;++bin) {
        assert(!ceph_gpu_config_build(&db,0x06040001,bin<<29,&c));
        assert(c.policy.gx_levels==8-bin && c.initial_freq==1 && c.initial_bw==3);
        assert(c.bw.levels==12 && c.bw.ddr_count==3 && c.bw.cnoc_count==3);
        assert(c.pdc.cfg_pa==0xb290000 && c.pdc.seq_pa==0xb490000);
        assert(!ceph_gpu_config_hfi(&c,0x20000001,&hfi) && (!!hfi.acd)==bin);
        assert(hfi.gx[hfi.ng-1].hz==(bin?675000000u:770000000u));
        uint32_t perf[59];assert(ceph_hfi_perf(perf,59,hfi.gx,hfi.ng,hfi.cx,hfi.nc,hfi.gfx,hfi.cxl,hfi.mx)==59);
        assert(perf[8]==257000 && perf[54]==200000);
        assert((c.bw.ddr_data[11][1]&0x3fff)==16383); /*64-bit IB -> saturation*/
        assert((c.bw.ddr_data[11][2]&0x3fff)==8); /* ACV bit vote */
    }
    struct ceph_gpu_config saved=c;
    assert(ceph_gpu_config_build(&db,0x06040000,0,&c)<0 && !memcmp(&c,&saved,sizeof(c)));
    assert(ceph_gpu_config_build(&db,0x06040001,2u<<29,&c)<0);
    assert(ceph_gpu_config_build(NULL,0x06040001,0,&c)<0);
    /* Missing BCM, invalid unit, missing PDC resource, insufficient ARC rail. */
    config_db[400]='X';assert(ceph_gpu_config_build(&db,0x06040001,0,&c)<0);config_fixture();
    config_word(544,0);assert(ceph_gpu_config_build(&db,0x06040001,0,&c)<0);config_fixture();
    config_word(144+4*24+16,0x30094);assert(ceph_gpu_config_build(&db,0x06040001,0,&c)<0);config_fixture();
    config_word(144+20,8);assert(ceph_gpu_config_build(&db,0x06040001,0,&c)<0);
    assert(!memcmp(&c,&saved,sizeof(c)));
    /* Logged real CX/MX levels, transported in a SYNTHETIC command-db carrier.
     * ACV's zero unit/width is a source-admitted test case, not yet a recorded
     * ACV byte snapshot from this phone. No fabricated defaults in production. */
    config_fixture();
    const uint16_t live_cx[]={0,16,64,128,192,256,384,416};
    const uint16_t live_mx[]={0,16,128,192,256,320,384,416};
    for(unsigned i=0;i<2;++i) {
        unsigned entry=144+(i+1)*24,aux=264+(i+1)*20;
        config_word(entry+20,16|((i+1)*20<<16));
        for(unsigned n=0;n<8;++n) {
            unsigned value=i?live_mx[n]:live_cx[n];
            config_db[aux+2*n]=(uint8_t)value;config_db[aux+2*n+1]=(uint8_t)(value>>8);
        }
    }
    config_word(544+2*8,0);config_word(548+2*8,2u<<16);
    assert(!ceph_gpu_config_build(&db,0x06040001,0,&c));
    uint32_t rounded_perf[59];
    assert(ceph_hfi_perf(rounded_perf,59,ceph_config_gx,8,ceph_config_cx,2,&c.gfx,&c.cx,&c.mx)==59);
    assert(rounded_perf[53]==0x00400202u); /* requested48 -> CX64/MX128 */
    assert((c.bw.ddr_data[11][2]&0x3fff)==8);
    config_word(544,0);assert(ceph_gpu_config_build(&db,0x06040001,0,&c)<0); /* not ACV */
    puts("Source-bound Cepheus profile: bin/OPP/12 DDR/2 CNOC/ACD/PDC + parsed synthetic cmd-db, fail-closed PASS (not live metadata)");
}
