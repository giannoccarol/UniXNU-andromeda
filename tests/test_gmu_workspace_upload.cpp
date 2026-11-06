#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" {
#include "cepheus_gmu_workspace.h"
#include "cepheus_gmu_upload.h"
}
static uint32_t itcm[4096],dtcm[4096];
struct state {unsigned calls;int ended,uncertain;};
static int step(void *p){((state *)p)->calls++;return 0;}
static void finish(void *p,int uncertain){auto *s=(state *)p;s->ended++;s->uncertain=uncertain;}
int main(int argc,char **argv)
{
    if(argc!=2){puts("Workspace+real firmware upload skipped: supply explicit local a640_gmu.bin");return 0;}
    std::ifstream f(argv[1],std::ios::binary);assert(f);std::vector<uint8_t> fw((std::istreambuf_iterator<char>(f)),{});
    uint64_t image=0;assert(!cepheus_gpu_firmware_snapshot(0,fw.data(),fw.size(),&image));
    ceph_gpu_firmware_view input={};assert(!cepheus_gpu_firmware_get(image,0,&input));
    ceph_gmu_workspace workspace={};assert(!cepheus_gmu_workspace_prepare(&workspace));
    ceph_gmu_mapping maps[]={{0x4000,0x3c000,(uint8_t *)workspace.views[0].cpu,CE_GMU_ICACHE},
        {0x44000,0x3c000,(uint8_t *)workspace.views[1].cpu,CE_GMU_DCACHE}};
    state s={};ceph_gmu_upload uploader={itcm,dtcm,4096,4096,maps,2,&s,step,step,step,finish,0,0};
    ceph_gmu_block plan[32];assert(!ceph_gmu_upload_file(&uploader,input.data,input.bytes,plan,32));
    unsigned count;assert(!ceph_gmu_plan(input.data,input.bytes,plan,32,&count) && count==7);
    for(unsigned i=0;i<count;++i){auto &b=plan[i];const uint8_t *actual=nullptr;
        if(b.region==CE_GMU_ITCM)actual=(uint8_t *)itcm+b.address;
        else if(b.region==CE_GMU_DTCM)actual=(uint8_t *)dtcm+b.address-0x40000;
        else if(b.region==CE_GMU_ICACHE)actual=(uint8_t *)workspace.views[0].cpu+b.address-0x4000;
        else if(b.region==CE_GMU_DCACHE)actual=(uint8_t *)workspace.views[1].cpu+b.address-0x44000;
        assert(actual && !memcmp(actual,input.data+b.file_offset,b.size));
        if(b.region==CE_GMU_ICACHE || b.region==CE_GMU_DCACHE){uint64_t pa,flags;
            assert(!ceph_vm_lookup(&workspace.table,b.address,&pa,&flags));
            auto &v=workspace.views[b.region==CE_GMU_ICACHE?0:1];
            assert(pa==v.pa+b.address-(b.region==CE_GMU_ICACHE?0x4000:0x44000));}
    }
    uint32_t *hfi=(uint32_t *)workspace.views[2].cpu;
    assert(hfi[1]==54 && hfi[7]==0x601000e0 && hfi[42]==0);
    assert(!cepheus_gpu_memory_view_put(image));assert(!cepheus_gpu_memory_release_cpu(image));
    assert(!cepheus_gmu_workspace_release_cpu(&workspace) && !mock_io::live && !mock_io::prepared);
    puts("Integration: exact firmware -> sealed XNU-pool copy -> workspace/VM -> RAM TCM+cache upload -> cleanup PASS (no device)");
}
