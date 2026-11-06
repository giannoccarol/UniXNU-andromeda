/* XNU-owned GMU backing allocation/cache primitives. No SMMU/GPU MMIO here.
 * Default off: exported selftest only runs under cephmem=1. It allocates,
 * checks actual PA contiguity and CPU visibility, then releases CPU-only memory.
 * Device publication latches retention; there is deliberately no bool-based
 * "GPU done" API to release a still-live DMA target. A future verified stop/unmap
 * backend must supply the retirement path before this is used for submission.
 */
#include <pexpert/arm64/board_config.h>
#if defined(SWEET)
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#if defined(KERNEL) || defined(CEPHEUS_GPU_MEMORY_HOST_TEST)
extern "C" {
#include <vm/memory_types.h>
/* IOKitServer.h hides this bridge behind MACH_KERNEL_PRIVATE. Same ABI as
 * osfmk/device/iokit_rpc.c, which calls pmap_cache_attributes(pa>>PAGE_SHIFT). */
unsigned int IODefaultCacheBits(addr64_t pa);
/* Real pmap bounds used by pa_valid().
 * Without this check a mask-derived allocation may land below gPhysBase and
 * panic inside ml_static_vtop/phystokv_range.
 * Declared with the underlying 64-bit integer type: pmap_paddr_t is not visible
 * in this translation unit (it lives in the pmap-private headers, which this
 * file must not pull in), and the symbols really are 64-bit addresses. */
extern uint64_t vm_first_phys;
extern uint64_t vm_last_phys;
}
#else
#error "GPU memory requires XNU bounds or the explicit host IOKit test double"
#endif
#include <sys/systm.h>
#include "cepheus_gpu_memory.h"
#include "cepheus_gpu_memory_state.h"
#include "cepheus_gpu_firmware.h"
#include "cepheus_gpu_memory_private.h"

struct ceph_gpu_buffer {
    IOBufferMemoryDescriptor *memory;
    IOMemoryMap *mapping;
    void *cpu;
    uint64_t pa;
    size_t bytes;
    bool prepared,published;
};

/* One-shot selftest counter and bounded outstanding buffer budget. */
static unsigned ceph_memory_once,ceph_memory_pages;
static IOLock *ceph_pool_lock;
static struct {struct ceph_mem_slot state;struct ceph_gpu_buffer buffer;unsigned firmware_kind;size_t firmware_bytes;uint64_t group;} ceph_pool[16];
static struct ceph_gpu_publication ceph_groups[16];
static uint64_t ceph_group_generation;
static unsigned ceph_pool_terminal;
static unsigned ceph_group_status[16];

static void ceph_gpu_memory_barrier(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif defined(CEPHEUS_GPU_MEMORY_HOST_TEST) && !defined(KERNEL)
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* test process only, never XNU */
#else
#error "GPU memory barrier requires arm64 (or explicit non-kernel host test)"
#endif
}

static IOLock *
ceph_gpu_pool_lock(void)
{
    IOLock *lock=__atomic_load_n(&ceph_pool_lock,__ATOMIC_ACQUIRE);
    if(lock)return lock;
    IOLock *candidate=IOLockAlloc();
    if(!candidate)return NULL;
    IOLock *expected=NULL;
    if(!__atomic_compare_exchange_n(&ceph_pool_lock,&expected,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        IOLockFree(candidate);return expected;
    }
    return candidate;
}

static void
ceph_gpu_buffer_dispose_cpu(struct ceph_gpu_buffer *b)
{
    if (b->published) return; /* no speculative free after possible DMA */
    if (b->prepared && b->memory->complete(kIODirectionInOut)!=kIOReturnSuccess) {
        b->published=true; /* uncertain completion: retain allocation/accounting */
        printf("CEPHEUS GPU MEM: complete failed; allocation retained\n");
        return;
    }
    if (b->mapping) b->mapping->release();
    if (b->memory) b->memory->release();
    __atomic_fetch_sub(&ceph_memory_pages,(unsigned)(b->bytes/4096),__ATOMIC_RELEASE);
    b->memory=NULL;b->mapping=NULL;b->cpu=NULL;b->pa=0;b->bytes=0;b->prepared=false;
}

static int
ceph_gpu_buffer_alloc_cpu(struct ceph_gpu_buffer *b,size_t bytes)
{
    if (!b || b->memory || !bytes || bytes>256*1024 || (bytes&4095u)) return -1;
    /* SWEET XNU uses 16KiB host pages while GMU tables use 4KiB. Account
     * physical allocation, not the smaller requested length. No odd geometry. */
    size_t host_page=(size_t)page_size;
    if(host_page!=4096 && host_page!=16384)return -1;
    bytes=(bytes+host_page-1)&~(host_page-1);
    uint64_t window_mask=cepheus_gpu_pa_window_mask(vm_first_phys,vm_last_phys,host_page);
    if(!window_mask)return -1; /* no fixed-mask fallback for invalid bounds */
    unsigned pages=(unsigned)(bytes/4096),used=__atomic_load_n(&ceph_memory_pages,__ATOMIC_ACQUIRE);
    do {
        /* ICACHE+DCACHE alone need480KiB; include HFI/log/page tables and
         * immutable firmware snapshots while retaining a hard1MiB cap. */
        if (used>256 || pages>256-used) return -1;
    } while (!__atomic_compare_exchange_n(&ceph_memory_pages,&used,used+pages,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    b->bytes=bytes;
    printf("CEPHEUS GPU MEM: phase=allocate bytes=%#lx host_page=%#lx managed=[%#llx,%#llx) mask=%#llx\n",
        (unsigned long)bytes,(unsigned long)host_page,(unsigned long long)vm_first_phys,
        (unsigned long long)vm_last_phys,(unsigned long long)window_mask);
    /* Default-cache allocation ONLY. Non-default cache flags cause IOBMD's
     * constructor to create a persistent memory entry and change attributes
     * before it returns, too early for our PA check. Request Normal-NC only in
     * the explicit map after validation. MapperNone prevents an IOVA being
     * mistaken for a CPU PA. maxPhys is an upper bound, never a lower bound. */
    b->memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone,
        bytes,window_mask);
    if (!b->memory) {ceph_gpu_buffer_dispose_cpu(b);return -1;}
    printf("CEPHEUS GPU MEM: phase=prepare\n");
    if (b->memory->prepare(kIODirectionInOut)!=kIOReturnSuccess) {
        ceph_gpu_buffer_dispose_cpu(b);return -1;
    }
    b->prepared=true;
    IOByteCount length=0;
    b->pa=b->memory->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    /* This check MUST run before createMappingInTask(). Remapping the buffer
     * with a different cache mode makes pmap change the page's attributes,
     * which goes pmap_sync_wimg -> pmap_sync_page_attributes_phys ->
     * flush_dcache(pa, PAGE_SIZE, TRUE) -> phystokv_range(pa). phystokv_range()
     * panics for any PA outside [gPhysBase, gPhysBase+real_phys_size), so a
     * buffer the allocator placed below the managed window would take the
     * kernel down during the mapping call itself -- after this point is far too
     * late. maxPhys in the allocation mask is only an upper bound and cannot
     * express this lower bound, so the check has to be here. */
    if(!b->pa || (b->pa&(host_page-1)) || length<bytes || b->memory->getLength()!=bytes ||
        !cepheus_gpu_pa_range_managed(b->pa,bytes,vm_first_phys,vm_last_phys)) {
        printf("CEPHEUS GPU MEM: backing invalid pa=%#llx bytes=%#lx segment=%#lx host_page=%#lx managed=[%#llx,%#llx); refused before mapping\n",
            (unsigned long long)b->pa,(unsigned long)bytes,
            (unsigned long)length,(unsigned long)host_page,
            (unsigned long long)vm_first_phys,(unsigned long long)vm_last_phys);
        ceph_gpu_buffer_dispose_cpu(b);return -1;
    }
    /* Allocation cache flags alone do not establish the attributes of the
     * original getBytesNoCopy VA. Request an explicit NC kernel map and use
     * ONLY its VA; XNU performs cache-mode/alias transitions while mapping. */
    printf("CEPHEUS GPU MEM: phase=map_nc pa=%#llx bytes=%#lx\n",(unsigned long long)b->pa,(unsigned long)bytes);
    b->mapping=b->memory->createMappingInTask(kernel_task,0,
        kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache,0,bytes);
    if(!b->mapping || b->mapping->getLength()!=bytes ||
        (b->mapping->getMapOptions()&kIOMapCacheMask)!=kIOMapWriteCombineCache) {
        ceph_gpu_buffer_dispose_cpu(b);return -1;
    }
    b->cpu=(void *)(uintptr_t)b->mapping->getVirtualAddress();
    if (!b->cpu || !b->pa || ((b->pa|(uintptr_t)b->cpu)&4095u) || length<bytes ||
        b->pa>=(1ULL<<40) || bytes>(1ULL<<40)-b->pa || b->memory->getLength()!=bytes) {
        ceph_gpu_buffer_dispose_cpu(b);return -1;
    }
    /* Attribute options describe the request; independently verify pmap's
     * effective host-page cache attributes before exposing any shared view.
     * Never repair this by forcing attributes on an unowned physical page. */
    for(size_t offset=0;offset<bytes;offset+=host_page) {
        unsigned attrs=IODefaultCacheBits(b->pa+offset)&VM_WIMG_MASK;
        if(attrs!=VM_WIMG_WCOMB) {
            printf("CEPHEUS GPU MEM: pmap cache mismatch attrs=%#x expected=%#x; allocation refused\n",attrs,VM_WIMG_WCOMB);
            ceph_gpu_buffer_dispose_cpu(b);return -1;
        }
    }
    bzero(b->cpu,bytes);
    ceph_gpu_memory_barrier();
    return 0;
}

/* Called with ceph_pool_lock held. */
static int
ceph_gpu_pool_index(uint64_t handle)
{
    uint32_t slot=(uint32_t)handle;
    if(!slot || slot>16 || !ceph_mem_matches(&ceph_pool[slot-1].state,slot-1,handle))return -1;
    return (int)slot-1;
}

extern "C" int
cepheus_gpu_memory_alloc(size_t bytes,uint64_t *handle)
{
    if(!handle || !bytes || bytes>256*1024 || (bytes&4095u))return -1;
    *handle=0;IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=-1;
    if(ceph_pool_terminal) {IOLockUnlock(lock);return -1;}
    for(unsigned i=0;i<16;++i)if(!ceph_mem_reserve(&ceph_pool[i].state)){index=(int)i;break;}
    IOLockUnlock(lock);if(index<0)return -1;
    auto *slot=&ceph_pool[index];
    /* ALLOCATING excludes all handle lookups, so slow IOKit work and slot
     * initialization can run outside the pool mutex without concurrent views. */
    slot->firmware_bytes=0;slot->firmware_kind=0;slot->group=0;
    int e=ceph_gpu_buffer_alloc_cpu(&slot->buffer,bytes);
    IOLockLock(lock);
    /* Allocation may sleep outside this mutex while another thread abandons
     * the GPU session. Recheck before handing out a new usable handle. This
     * allocation was never published, so CPU-only disposal remains safe. */
    if(!e && ceph_pool_terminal) {
        IOLockUnlock(lock);ceph_gpu_buffer_dispose_cpu(&slot->buffer);IOLockLock(lock);e=-1;
    }
    (void)ceph_mem_finish_allocation(&slot->state,!e,slot->buffer.published);
    if(!e)*handle=ceph_mem_handle((unsigned)index,slot->state.generation);
    if(e && slot->buffer.published)printf("CEPHEUS GPU MEM: retained slot=%u bytes=%lu generation=%u\n",
        (unsigned)index,(unsigned long)slot->buffer.bytes,slot->state.generation);
    IOLockUnlock(lock);return e;
}

extern "C" int
cepheus_gpu_firmware_snapshot(unsigned kind,const uint8_t *data,size_t bytes,uint64_t *handle)
{
    if(!handle)return -1;
    *handle=0;
    if(!data || !((kind==CE_FW_A640_GMU && bytes==37680) ||
        (kind==CE_FW_A630_SQE && bytes==32304)))return -1;
    uint64_t id=0;
    if(cepheus_gpu_memory_alloc((bytes+4095u)&~(size_t)4095u,&id))return -1;
    struct ceph_gpu_memory_view view={};
    if(cepheus_gpu_memory_view_get(id,&view)) { (void)cepheus_gpu_memory_release_cpu(id);return -1; }
    memcpy(view.cpu,data,bytes);
    ceph_gpu_memory_barrier();
    int e=ceph_firmware_verify((enum ceph_firmware_kind)kind,(const uint8_t *)view.cpu,bytes);
    (void)cepheus_gpu_memory_view_put(id);
    if(e) { (void)cepheus_gpu_memory_release_cpu(id);return -1; }
    IOLock *lock=ceph_gpu_pool_lock(); /* already allocated by memory_alloc */
    IOLockLock(lock);int index=ceph_gpu_pool_index(id);
    e=index<0?-1:ceph_mem_seal(&ceph_pool[index].state);
    if(!e) {ceph_pool[index].firmware_bytes=bytes;ceph_pool[index].firmware_kind=kind;*handle=id;}
    IOLockUnlock(lock);
    if(e)(void)cepheus_gpu_memory_release_cpu(id);
    return e;
}

extern "C" int
cepheus_gpu_firmware_get(uint64_t handle,unsigned expected_kind,struct ceph_gpu_firmware_view *view)
{
    if(!view || expected_kind>CE_FW_A630_SQE)return -1;
    *view={};IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=ceph_gpu_pool_index(handle),e=-1;
    if(index>=0 && ceph_pool[index].firmware_bytes && ceph_pool[index].firmware_kind==expected_kind &&
        !ceph_mem_readonly_get(&ceph_pool[index].state)) {
        *view={(const uint8_t *)ceph_pool[index].buffer.cpu,ceph_pool[index].firmware_bytes,expected_kind};e=0;
    }
    IOLockUnlock(lock);return e;
}

extern "C" int
cepheus_gpu_memory_view_get(uint64_t handle,struct ceph_gpu_memory_view *view)
{
    if(!view)return -1;
    *view={};IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=ceph_gpu_pool_index(handle),e=-1;
    if(index>=0 && !ceph_mem_view_get(&ceph_pool[index].state)) {
        const auto *b=&ceph_pool[index].buffer;*view={b->cpu,b->pa,b->bytes};e=0;
    }
    IOLockUnlock(lock);return e;
}

extern "C" int
cepheus_gpu_memory_view_put(uint64_t handle)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=ceph_gpu_pool_index(handle);
    int e=index<0?-1:ceph_mem_view_put(&ceph_pool[index].state);
    IOLockUnlock(lock);return e;
}

extern "C" int
cepheus_gpu_memory_publish(uint64_t handle)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=ceph_gpu_pool_index(handle),e=-1;
    if(!ceph_pool_terminal && index>=0 && !ceph_mem_publish(&ceph_pool[index].state)) {
        /* Finish NC writes before any device address is installed/doorbelled. */
        ceph_gpu_memory_barrier();
        ceph_pool[index].buffer.published=true;e=0;
    }
    IOLockUnlock(lock);return e;
}

extern "C" int
cepheus_gpu_memory_release_cpu(uint64_t handle)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int index=ceph_gpu_pool_index(handle);
    if(index<0 || ceph_mem_release_begin(&ceph_pool[index].state)) {IOLockUnlock(lock);return -1;}
    IOLockUnlock(lock);
    auto *slot=&ceph_pool[index];ceph_gpu_buffer_dispose_cpu(&slot->buffer);
    IOLockLock(lock);
    int e=slot->buffer.published?-1:0;
    (void)ceph_mem_finish_allocation(&slot->state,0,slot->buffer.published);
    if(e)printf("CEPHEUS GPU MEM: retained slot=%u bytes=%lu generation=%u\n",
        (unsigned)index,(unsigned long)slot->buffer.bytes,slot->state.generation);
    IOLockUnlock(lock);return e;
}

extern "C" void
cepheus_gpu_memory_selftest(void)
{
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&ceph_memory_once,&expected,1u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
    uint64_t handle=0;struct ceph_gpu_memory_view b={};
    if (cepheus_gpu_memory_alloc(5*4096,&handle)) {
        printf("CEPHEUS GPU MEM: CPU-only allocation test failed (no GPU access)\n");
        return;
    }
    if(cepheus_gpu_memory_view_get(handle,&b)) {
        (void)cepheus_gpu_memory_release_cpu(handle);
        printf("CEPHEUS GPU MEM: CPU view failed\n");return;
    }
    volatile uint32_t *words=(volatile uint32_t *)b.cpu;
    unsigned count=(unsigned)(b.bytes/sizeof(uint32_t));
    for (unsigned i=0;i<count;++i) words[i]=0xa6400000u^i;
    ceph_gpu_memory_barrier();
    bool okay=true;
    for (unsigned i=0;i<count;++i) if (words[i]!=(0xa6400000u^i)) {okay=false;break;}
    printf("CEPHEUS GPU MEM: CPU-only NC contiguous bytes=%lu pa=%#llx pattern=%s; no DMA/SMMU\n",
        (unsigned long)b.bytes,(unsigned long long)b.pa,okay?"PASS":"FAIL");
    bzero(b.cpu,b.bytes);ceph_gpu_memory_barrier();
    (void)cepheus_gpu_memory_view_put(handle);
    int released=cepheus_gpu_memory_release_cpu(handle);
    printf("CEPHEUS GPU MEM: CPU-only release=%d stale_release=%d\n",released,
        cepheus_gpu_memory_release_cpu(handle));
    printf("CEPHEUS GPU MEM: CPU-only test retained_pages=%u\n",
        __atomic_load_n(&ceph_memory_pages,__ATOMIC_ACQUIRE));
}

/* These batch operations preserve exact handles, root PA and group membership.
 * All-or-none checks happen under the pool lock before state mutations. */
static int ceph_memory_group_indices(const struct ceph_gpu_publication *p,int indices[CE_GPU_WORKSPACE_BUFFERS])
{
    if(!p || !p->group)return -1;
    unsigned group=(unsigned)(p->group&255u);
    if(!group || group>16)return -1;
    const auto *saved=&ceph_groups[group-1];
    if(saved->group!=p->group || saved->ttbr0!=p->ttbr0 || saved->ttbr1!=p->ttbr1 ||
        saved->smr!=p->smr || saved->cb!=p->cb || saved->slot!=p->slot || saved->asid!=p->asid)return -1;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
        if(saved->handles[i]!=p->handles[i])return -1;
        indices[i]=ceph_gpu_pool_index(p->handles[i]);
        if(indices[i]<0 || ceph_pool[indices[i]].group!=p->group)return -1;
    }
    return (int)group-1;
}

extern "C" int ceph_memory_share_group(const uint64_t handles[CE_GPU_WORKSPACE_BUFFERS],const struct ceph_gpu_publication *identity,
    struct ceph_gpu_publication *out)
{
    if(!handles || !identity || !out)return -1;
    *out={};IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int indices[CE_GPU_WORKSPACE_BUFFERS],group=-1,e=-1;
    if(ceph_pool_terminal)goto done;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
        indices[i]=ceph_gpu_pool_index(handles[i]);
        if(indices[i]<0 || !ceph_mem_can_share(&ceph_pool[indices[i]].state) ||
            ceph_pool[indices[i]].buffer.published || ceph_pool[indices[i]].group)goto done;
        for(unsigned j=0;j<i;++j)if(indices[j]==indices[i])goto done;
    }
    if((identity->ttbr0&0xffffffffffULL)!=ceph_pool[indices[CE_GPU_WORKSPACE_ROOT]].buffer.pa)goto done;
    {
        struct ceph_gpu_memory_view views[CE_GPU_WORKSPACE_BUFFERS];
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
            const auto *b=&ceph_pool[indices[i]].buffer;views[i]={b->cpu,b->pa,b->bytes};
        }
        if(ceph_memory_validate_workspace(views,identity))goto done;
    }
    for(unsigned i=0;i<16;++i)if(!ceph_groups[i].group){group=(int)i;break;}
    if(group<0 || ceph_group_generation==0x00ffffffffffffffULL)goto done;
    {
        struct ceph_gpu_publication p=*identity;
        p.group=(++ceph_group_generation<<8)|((unsigned)group+1u);
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)p.handles[i]=handles[i];
        ceph_gpu_memory_barrier();
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
            auto *slot=&ceph_pool[indices[i]];
            slot->state.state=CE_MEM_SHARED;slot->buffer.published=true;slot->group=p.group;
        }
        ceph_groups[group]=p;ceph_group_status[group]=CE_GPU_GROUP_SHARED;*out=p;e=0;
    }
done:IOLockUnlock(lock);return e;
}

extern "C" int ceph_memory_retire_begin(const struct ceph_gpu_publication *p)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int indices[CE_GPU_WORKSPACE_BUFFERS],e=-1;
    if(ceph_memory_group_indices(p,indices)<0)goto done;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)if(!ceph_mem_can_retire(&ceph_pool[indices[i]].state))goto done;
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)ceph_pool[indices[i]].state.state=CE_MEM_RETIRING;
    ceph_group_status[(unsigned)(p->group&255u)-1]=CE_GPU_GROUP_RETIRING;
    e=0;
done:IOLockUnlock(lock);return e;
}

extern "C" int ceph_memory_retire_finish(const struct ceph_gpu_publication *p,int success)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int indices[CE_GPU_WORKSPACE_BUFFERS];int group=ceph_memory_group_indices(p,indices);
    if(group<0){IOLockUnlock(lock);return -1;}
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)if(ceph_pool[indices[i]].state.state!=CE_MEM_RETIRING){IOLockUnlock(lock);return -1;}
    if(!success) {
        ceph_pool_terminal=1;
        ceph_group_status[group]=CE_GPU_GROUP_UNCERTAIN;
        for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)ceph_pool[indices[i]].state.state=CE_MEM_QUARANTINED;
        printf("CEPHEUS GPU MEM: terminal GPU session group=%#llx; all six allocations retained; new allocations blocked\n",(unsigned long long)p->group);
        IOLockUnlock(lock);return -1;
    }
    /* Stop/unmap/TLB proof is complete, callers have surrendered CPU views.
     * Keep slots unobservable while slow IOKit completion runs. */
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i) {
        ceph_pool[indices[i]].state.views=0;
        ceph_pool[indices[i]].state.state=CE_MEM_ALLOCATING;
        ceph_pool[indices[i]].buffer.published=false;
    }
    IOLockUnlock(lock);
    int e=0;
    for(unsigned i=CE_GPU_WORKSPACE_BUFFERS;i>0;--i) {
        auto *slot=&ceph_pool[indices[i-1]];ceph_gpu_buffer_dispose_cpu(&slot->buffer);
        IOLockLock(lock);
        if(slot->buffer.published)e=-1;
        (void)ceph_mem_finish_allocation(&slot->state,0,slot->buffer.published);
        if(!slot->buffer.published)slot->group=0;
        else printf("CEPHEUS GPU MEM: detached but complete failed slot=%u bytes=%lu\n",
            (unsigned)indices[i-1],(unsigned long)slot->buffer.bytes);
        IOLockUnlock(lock);
    }
    IOLockLock(lock);
    if(!e) {ceph_groups[group]={};ceph_group_status[group]=0;}
    else {ceph_group_status[group]=CE_GPU_GROUP_DETACHED_RETAINED;ceph_pool_terminal=1;}
    IOLockUnlock(lock);return e;
}

extern "C" int cepheus_gpu_memory_group_status(uint64_t group,struct ceph_gpu_group_status *out)
{
    if(!out)return -1;
    *out={};IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);unsigned slot=(unsigned)(group&255u);
    if(!group || !slot || slot>16 || ceph_groups[slot-1].group!=group) {IOLockUnlock(lock);return -1;}
    out->identity=ceph_groups[slot-1];out->state=ceph_group_status[slot-1];out->pool_terminal=ceph_pool_terminal;
    /* ALLOCATING buffers are being completed outside the lock. Their pointer
     * and byte fields may change; don't race them for diagnostic counts. A
     * RETIRING group can therefore report only its currently stable slots. */
    for(unsigned i=0;i<16;++i)if(ceph_pool[i].state.state!=CE_MEM_ALLOCATING &&
        ceph_pool[i].group==group && ceph_pool[i].buffer.memory) {
        ++out->retained_buffers;out->retained_bytes+=ceph_pool[i].buffer.bytes;
    }
    IOLockUnlock(lock);return 0;
}

extern "C" int cepheus_gpu_memory_abandon_workspace(const struct ceph_gpu_publication *p)
{
    IOLock *lock=ceph_gpu_pool_lock();if(!lock)return -1;
    IOLockLock(lock);int indices[CE_GPU_WORKSPACE_BUFFERS];
    int group=ceph_memory_group_indices(p,indices);
    if(group<0 || ceph_group_status[group]!=CE_GPU_GROUP_SHARED) {IOLockUnlock(lock);return -1;}
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)
        if(ceph_pool[indices[i]].state.state!=CE_MEM_SHARED) {IOLockUnlock(lock);return -1;}
    for(unsigned i=0;i<CE_GPU_WORKSPACE_BUFFERS;++i)ceph_pool[indices[i]].state.state=CE_MEM_QUARANTINED;
    ceph_group_status[group]=CE_GPU_GROUP_UNCERTAIN;ceph_pool_terminal=1;
    printf("CEPHEUS GPU MEM: startup abandoned group=%#llx; all backing retained, pool terminal\n",(unsigned long long)p->group);
    IOLockUnlock(lock);return 0;
}
#endif
