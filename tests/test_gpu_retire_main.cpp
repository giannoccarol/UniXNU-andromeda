#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" int ceph_test_retire_path(unsigned);
int main(int argc,char **argv){assert(argc==2);unsigned mode=(unsigned)std::strtoul(argv[1],nullptr,10);assert(mode<=5);
    if(mode==5)mock_io::fail_complete=1;
    int e=ceph_test_retire_path(mode);assert((e!=0)==(mode!=0));
    assert(mock_io::live==(mode?6u:0u) && mock_io::maps==(mode?6u:0u) && !mock_io::raw_views);
    printf("Production share/retire: mode=%u retained=%u, real HFI ring + slumber/park/detach orchestrator PASS (fake hardware)\n",mode,mock_io::live.load());}
