#include <cassert>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" int ceph_test_hfi_link(void);
int main(){int e=ceph_test_hfi_link();assert(!e && mock_io::live==6 && mock_io::maps==6 && !mock_io::raw_views);return e;}
