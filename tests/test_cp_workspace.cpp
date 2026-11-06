#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" {
#include "cepheus_cp_workspace.h"
#include "cepheus_gmu_workspace.h"
void ceph_test_cp_workspace_restore(struct ceph_cp_workspace *,const struct ceph_gmu_start_config *);
int ceph_test_cp_preinit(struct ceph_cp_workspace *);
}
int main(int argc,char **argv)
{
    if(argc!=3){puts("CP workspace real-SQE fixture skipped: provide explicit local file and mode");return 0;}
    unsigned mode=(unsigned)std::strtoul(argv[2],nullptr,10);assert(mode<=3);
    std::ifstream input(argv[1],std::ios::binary);assert(input);
    std::vector<uint8_t> fw((std::istreambuf_iterator<char>(input)),{});
    ceph_cp_workspace w={};ceph_gmu_workspace gmu={};uint64_t occupied[12]={};
    if(mode==1)fw.back()^=1;
    if(mode==2)for(auto &h:occupied)assert(!cepheus_gpu_memory_alloc(4096,&h));
    if(mode==3)mock_io::fail_complete=1;
    if(mode) {
        assert(cepheus_cp_workspace_prepare(&w,fw.data(),fw.size())<0);
        if(mode==3){assert(w.state==CE_CP_WS_RETAINED && w.firmware && mock_io::live==7);return 0;}
        assert(w.state==CE_CP_WS_RELEASED);
        if(mode==2)for(auto h:occupied)assert(!cepheus_gpu_memory_release_cpu(h));
        assert(!mock_io::live && !mock_io::maps);return 0;
    }
    assert(!cepheus_gmu_workspace_prepare(&gmu)); /* coexist within same1MiB pool */
    assert(!cepheus_cp_workspace_prepare(&w,fw.data(),fw.size()) && w.state==CE_CP_WS_READY);
    assert(w.sqe.bytes==32300 && w.sqe.version==0x16ee190 && !w.firmware && mock_io::live==12);
    assert(!memcmp(w.views[CE_CP_SQE].cpu,fw.data()+4,fw.size()-4));
    assert(!cepheus_cp_workspace_validate(&w));
    auto *sqe=(uint8_t *)w.views[CE_CP_SQE].cpu;sqe[1024]^=1;
    assert(cepheus_cp_workspace_validate(&w)<0);sqe[1024]^=1;
    sqe[32767]=1;assert(cepheus_cp_workspace_validate(&w)<0);sqe[32767]=0;
    auto saved=w.views[CE_CP_RING];w.views[CE_CP_RING]=w.views[CE_CP_RPTR];
    assert(cepheus_cp_workspace_validate(&w)<0);w.views[CE_CP_RING]=saved;
    for(unsigned role=0;role<CE_CP_BUFFERS;++role) {
        assert(w.views[role].cpu && w.views[role].bytes>=4096);
        auto *p=(uint8_t *)w.views[role].cpu;
        for(size_t i=role==CE_CP_SQE?w.sqe.bytes:0;i<w.views[role].bytes;++i)assert(!p[i]);
        assert(cepheus_gpu_memory_publish(w.handles[role])<0); /* held CPU views, no DMA */
    }
    uint64_t va[]={0xfc000000,0xfc010000,0xfc020000,0xfc030000,0x100000000ULL};
    va[0]=0x100000;assert(cepheus_cp_workspace_table(&w,va)<0 && !w.table.words);va[0]=0xfc000000;
    va[1]=va[0];assert(cepheus_cp_workspace_table(&w,va)<0 && w.state==CE_CP_WS_READY && !w.table.words);va[1]=0xfc010000;
    assert(!cepheus_cp_workspace_table(&w,va) && w.state==CE_CP_WS_TABLED && !ceph_vm39_matches(&w.table,w.regions,CE_CP_ROOT));
    assert(!cepheus_cp_workspace_validate(&w));
    w.table.words[0]^=1;assert(cepheus_cp_workspace_validate(&w)<0);w.table.words[0]^=1;
    for(unsigned i=0;i<CE_CP_ROOT;++i){uint64_t pa,flags;
        assert(!ceph_vm39_lookup(&w.table,va[i],&pa,&flags) && pa==w.views[i].pa);
        if(i==CE_CP_SQE)assert(!(flags&(3ULL<<53)) && (flags&(3u<<6))==(3u<<6));
    }
    assert(cepheus_cp_workspace_table(&w,va)<0);
    assert(cepheus_cp_workspace_prepare(&w,fw.data(),fw.size())<0);
    assert(!ceph_test_cp_preinit(&w));
    ceph_test_cp_workspace_restore(&w,&gmu.layout.start);
    assert(!cepheus_cp_workspace_release(&w) && w.state==CE_CP_WS_RELEASED && mock_io::live==6);
    assert(!cepheus_gmu_workspace_release_cpu(&gmu) && !mock_io::live && !mock_io::maps && !mock_io::prepared);
    assert(cepheus_cp_workspace_release(&w)<0);
    puts("CP CPU workspace: real SQE + ring/RPTR/restore/scratch +39bit page tables, coexistence with GMU, zero DMA publication PASS");
}
