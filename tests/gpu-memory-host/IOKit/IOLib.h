#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
typedef size_t IOByteCount;
static constexpr int kIOReturnSuccess=0,kIODirectionInOut=1;
static constexpr unsigned kIOMemoryHostPhysicallyContiguous=2,kIOMemoryMapperNone=4,kIOMapWriteCombineCache=8;
static constexpr unsigned kIOMapAnywhere=16,kIOMapUnique=32,kIOMapCacheMask=0xf;
typedef uint64_t addr64_t;
