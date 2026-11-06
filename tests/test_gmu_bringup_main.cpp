#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" int ceph_test_bringup(const char *,unsigned);
extern "C" int ceph_test_bringup_prepare_failure(const char *);
int main(int argc,char **argv){if(argc!=3){puts("Whole GMU chain skipped: explicit local GMU firmware required");return 0;}
    unsigned mode=(unsigned)std::strtoul(argv[2],nullptr,10);assert(mode<=22);
    if(mode==10){mock_io::fail_complete=1;assert(!ceph_test_bringup_prepare_failure(argv[1]));
        assert(mock_io::live==16 && mock_io::prepared==16);puts("CPU prepare failure: retained firmware handle preserved PASS (IOKit double)");return 0;}
    int e=ceph_test_bringup(argv[1],mode);assert((e!=0)==(mode!=0 && mode!=8 && mode!=18));
    if(!e)assert(!mock_io::live && !mock_io::prepared && !mock_io::raw_views);
    if(mode==11)assert(mock_io::live==6 && mock_io::prepared==6 && !mock_io::raw_views);
    return 0;}
