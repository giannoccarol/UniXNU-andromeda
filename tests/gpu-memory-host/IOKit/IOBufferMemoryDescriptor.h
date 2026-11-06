#pragma once
#include "IOLib.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include "cepheus_gpu_memory.h"
/* Test double for the IOKit allocator ONLY. Actual production .cpp is compiled
 * and linked into this host executable, not reimplemented. No GPU simulated. */
namespace mock_io {
inline std::atomic<unsigned> live{0},prepared{0},released{0};
inline std::atomic<unsigned> maps{0},raw_views{0};
inline std::atomic<unsigned> map_calls{0},cache_queries{0},alloc_calls{0};
inline std::atomic<uint64_t> next_pa{0x120000000ULL};
inline std::atomic<int> fail_alloc{0},fail_prepare{0},fail_complete{0},bad_segment{0};
inline std::atomic<int> fail_map{0},bad_cache{0};
inline std::atomic<int> bad_pmap_cache{0};
inline void (*after_allocate)()=nullptr; /* deterministic outside-pool-lock fault injection */
}
extern "C" {
inline uint64_t vm_first_phys=0x101004000ULL,vm_last_phys=0x1c1004000ULL;
inline unsigned int IODefaultCacheBits(addr64_t pa) {
    assert(cepheus_gpu_pa_range_managed(pa,page_size,vm_first_phys,vm_last_phys));
    ++mock_io::cache_queries;
    return mock_io::bad_pmap_cache?0:VM_WIMG_WCOMB;
}
}
class IOMemoryMap {
    void *mapped;size_t length;unsigned options;
public:
    IOMemoryMap(size_t bytes,unsigned flags):mapped(nullptr),length(bytes),options(flags){
        assert(!posix_memalign(&mapped,page_size,bytes));memset(mapped,0xc3,bytes);++mock_io::maps;
    }
    uintptr_t getVirtualAddress(){return (uintptr_t)mapped;}
    size_t getLength(){return length;}
    unsigned getMapOptions(){return mock_io::bad_cache?0:options;}
    void release(){free(mapped);--mock_io::maps;delete this;}
};
class IOBufferMemoryDescriptor {
    void *memory=nullptr;size_t length=0;bool wired=false;uint64_t pa=0;
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(void *,unsigned options,size_t bytes,uint64_t mask){
        /* Cache flags here would change attributes BEFORE production checks. */
        assert(options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone));
        assert(mask && mask==cepheus_gpu_pa_window_mask(vm_first_phys,vm_last_phys,page_size));
        ++mock_io::alloc_calls;
        if(mock_io::fail_alloc)return nullptr;
        auto *p=new IOBufferMemoryDescriptor;
        assert(!(bytes&(page_size-1)));
        if(posix_memalign(&p->memory,page_size,bytes)){delete p;return nullptr;}
        p->length=bytes;p->pa=mock_io::next_pa.fetch_add(bytes);++mock_io::live;
        if(mock_io::after_allocate)mock_io::after_allocate();
        return p;
    }
    int prepare(int){if(mock_io::fail_prepare)return -1;assert(!wired);wired=true;++mock_io::prepared;return 0;}
    int complete(int){assert(wired);if(mock_io::fail_complete)return -1;wired=false;--mock_io::prepared;return 0;}
    uint64_t getPhysicalSegment(size_t offset,IOByteCount *bytes,unsigned options){
        assert(!offset && options==kIOMemoryMapperNone && wired);*bytes=mock_io::bad_segment?4096:length;
        return pa; /* simulated unique PA, never claim hardware mapping */
    }
    void *getBytesNoCopy(){++mock_io::raw_views;return memory;}
    IOMemoryMap *createMappingInTask(void *,uint64_t address,unsigned flags,size_t offset,size_t bytes){
        ++mock_io::map_calls;
        assert(cepheus_gpu_pa_range_managed(pa,bytes,vm_first_phys,vm_last_phys));
        assert(wired && !address && !offset && bytes==length && flags==(kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache));
        return mock_io::fail_map?nullptr:new IOMemoryMap(bytes,flags);
    }
    size_t getLength(){return length;}
    void release(){assert(!wired);free(memory);--mock_io::live;++mock_io::released;delete this;}
};
