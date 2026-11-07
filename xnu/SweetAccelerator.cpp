/*
 * andromeda: the GPU as Metal and Turnip see it (phases F5/F6, patch 0200,
 * andrmtl=N; off by default). Ported from ios-flame (Nick Wylynko /
 * NWylynko, https://github.com/NWylynko/ios-flame, commit 745c4341, MIT,
 * patched XNU files APSL 2.0; see CREDITS.md and licenses/ios-flame-MIT.txt):
 * SweetAccelerator.cpp of patches 0096-flame-metal-accelerator,
 * 0097-flame-metal-userclient, 0115-flame-gpu-v2-client-turnip and
 * 0116-flame-sweetmetal2-plugin-switch. The class names are flame's. The
 * wire ABI (NativeAdrenoABI.h, staged unchanged next to this file) and the
 * 'NTNL' tunnel are UniXNU's NativeAdreno (UniXNU contributors, MIT,
 * licenses/NativeAdreno-MIT.txt).
 *
 * Metal.framework (MTLRegisterDevices) iterates
 * IOServiceMatching("IOAcceleratorES"), loads
 * /System/Library/Extensions/<MetalPluginName>.bundle, takes the class named
 * MetalPluginClassName and calls -initWithAcceleratorPort: with the service
 * (flame 0096). Turnip's darwin backend (UniXNU's tu_knl_darwin.cc, inside
 * SweetMetal2.bundle and phone/turnip) opens the same service with type
 * 0x4147 and speaks the NativeAdreno ABI v1 through one admitted selector.
 *
 * Here only the IOKit classes and the "OS layer" (memory descriptors, task
 * page pinning, publication) live; the methods, the BO pool, the GPU address
 * windows, page tables, submits and fences are in bsd/dev/andromeda_gpu_mtl.c
 * (host-tested). Changes from flame, levels and boot-args: andromeda_gpu_mtl.h.
 *
 *   andrmtl=0/absent  probe() declines: no service, no user client
 *   andrmtl=1|2       the service starts at boot but is registered (visible
 *                     to IOServiceGetMatchingService) only once andrgpu's step
 *                     4 has passed and the C side has its BO pool; 2 also
 *                     names the SweetMetal2 plug-in
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOReturn.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include "NativeAdrenoABI.h"
#include "andromeda_gpu_mtl.h"

extern "C" void proc_selfname(char *buf, int size);

/* The C mirror (andromeda_gpu_mtl.h) is the ABI, record by record. */
#define SA_SAME(c, abi) (sizeof(c) == sizeof(abi))
#define SA_EQ(a, b) ((long long)(a) == (long long)(b))	/* two unnamed enums */
static_assert(SA_SAME(andr_mtl_params_t, NativeAdrenoAbiParams) &&
    offsetof(andr_mtl_params_t, max_bo_bytes) == offsetof(NativeAdrenoAbiParams, max_bo_bytes), "params");
static_assert(SA_SAME(andr_mtl_bo_alloc_t, NativeAdrenoAbiBoAlloc) &&
    offsetof(andr_mtl_bo_alloc_t, contiguous) == offsetof(NativeAdrenoAbiBoAlloc, contiguous), "alloc");
static_assert(SA_SAME(andr_mtl_handle_t, NativeAdrenoAbiBoAllocReply) && SA_SAME(andr_mtl_handle_t, NativeAdrenoAbiBoFree) &&
    SA_SAME(andr_mtl_handle_t, NativeAdrenoAbiBoCpuMap), "handles");
static_assert(SA_SAME(andr_mtl_bo_import_t, NativeAdrenoAbiBoImport) &&
    offsetof(andr_mtl_bo_import_t, bytes) == offsetof(NativeAdrenoAbiBoImport, bytes), "import");
static_assert(SA_SAME(andr_mtl_bo_map_t, NativeAdrenoAbiBoMap) &&
    offsetof(andr_mtl_bo_map_t, iova) == offsetof(NativeAdrenoAbiBoMap, iova) &&
    offsetof(andr_mtl_bo_map_t, permissions) == offsetof(NativeAdrenoAbiBoMap, permissions) &&
    offsetof(andr_mtl_bo_map_t, writeback) == offsetof(NativeAdrenoAbiBoMap, writeback) &&
    offsetof(andr_mtl_bo_map_t, reserved) == offsetof(NativeAdrenoAbiBoMap, reserved), "map");
static_assert(SA_SAME(andr_mtl_bo_map_reply_t, NativeAdrenoAbiBoMapReply), "map reply");
static_assert(SA_SAME(andr_mtl_submit_buffer_t, NativeAdrenoAbiSubmitBuffer) &&
    offsetof(andr_mtl_submit_buffer_t, iova) == offsetof(NativeAdrenoAbiSubmitBuffer, iova) &&
    offsetof(andr_mtl_submit_buffer_t, bytes) == offsetof(NativeAdrenoAbiSubmitBuffer, bytes), "buffer");
static_assert(SA_SAME(andr_mtl_submit_t, NativeAdrenoAbiSubmit) &&
    offsetof(andr_mtl_submit_t, queue) == offsetof(NativeAdrenoAbiSubmit, queue) &&
    offsetof(andr_mtl_submit_t, wait_fence) == offsetof(NativeAdrenoAbiSubmit, wait_fence) &&
    offsetof(andr_mtl_submit_t, wait_timeout_ns) == offsetof(NativeAdrenoAbiSubmit, wait_timeout_ns) &&
    offsetof(andr_mtl_submit_t, buffers) == offsetof(NativeAdrenoAbiSubmit, buffers) &&
    SA_EQ(ANDR_MTL_MAX_BUFFERS, NativeAdrenoAbiMaxBuffers), "submit");
static_assert(SA_SAME(andr_mtl_metadata_t, NativeAdrenoAbiMetadata) &&
    offsetof(andr_mtl_metadata_t, text) == offsetof(NativeAdrenoAbiMetadata, text), "metadata");
static_assert(SA_SAME(andr_mtl_u64_t, NativeAdrenoAbiSubmitReply) && SA_SAME(andr_mtl_u64_t, NativeAdrenoAbiTimestamp) &&
    SA_SAME(andr_mtl_u64_t, NativeAdrenoAbiQueueReply), "u64 replies");
static_assert(SA_SAME(andr_mtl_wait_fence_t, NativeAdrenoAbiWaitFence) &&
    offsetof(andr_mtl_wait_fence_t, timeout_ns) == offsetof(NativeAdrenoAbiWaitFence, timeout_ns), "wait");
static_assert(SA_SAME(andr_mtl_cpu_map_reply_t, NativeAdrenoAbiBoCpuMapReply) &&
    offsetof(andr_mtl_cpu_map_reply_t, bytes) == offsetof(NativeAdrenoAbiBoCpuMapReply, bytes), "cpu map reply");
static_assert(SA_SAME(andr_mtl_cpu_unmap_t, NativeAdrenoAbiBoCpuUnmap), "cpu unmap");
static_assert(SA_SAME(andr_mtl_status_t, NativeAdrenoAbiStatus), "status");
static_assert(SA_SAME(andr_mtl_device_info_t, NativeAdrenoAbiDeviceInfo) &&
    offsetof(andr_mtl_device_info_t, gmem_base) == offsetof(NativeAdrenoAbiDeviceInfo, gmem_base) &&
    offsetof(andr_mtl_device_info_t, va_start) == offsetof(NativeAdrenoAbiDeviceInfo, va_start) &&
    offsetof(andr_mtl_device_info_t, highest_bank_bit) == offsetof(NativeAdrenoAbiDeviceInfo, highest_bank_bit) &&
    offsetof(andr_mtl_device_info_t, flags) == offsetof(NativeAdrenoAbiDeviceInfo, flags), "device info");
static_assert(SA_EQ(ANDR_MTL_ABI_VERSION, NativeAdrenoAbiVersion) && SA_EQ(ANDR_MTL_MAX_BO_SHIFT, NativeAdrenoAbiMaxBoBytesShift) &&
    SA_EQ(ANDR_MTL_METADATA_BYTES, NativeAdrenoAbiMetadataBytes), "ABI constants");
static_assert(SA_EQ(ANDR_MTL_M_QUERY, NativeAdrenoMethodQuery) && SA_EQ(ANDR_MTL_M_BO_ALLOC, NativeAdrenoMethodBoAlloc) &&
    SA_EQ(ANDR_MTL_M_BO_MAP, NativeAdrenoMethodBoMap) && SA_EQ(ANDR_MTL_M_BO_FREE, NativeAdrenoMethodBoFree) &&
    SA_EQ(ANDR_MTL_M_SUBMIT, NativeAdrenoMethodSubmit) && SA_EQ(ANDR_MTL_M_WAIT_FENCE, NativeAdrenoMethodWaitFence) &&
    SA_EQ(ANDR_MTL_M_GET_TIMESTAMP, NativeAdrenoMethodGetTimestamp) && SA_EQ(ANDR_MTL_M_GET_STATUS, NativeAdrenoMethodGetStatus) &&
    SA_EQ(ANDR_MTL_M_SET_METADATA, NativeAdrenoMethodSetMetadata) && SA_EQ(ANDR_MTL_M_GET_METADATA, NativeAdrenoMethodGetMetadata) &&
    SA_EQ(ANDR_MTL_M_CREATE_QUEUE, NativeAdrenoMethodCreateQueue) && SA_EQ(ANDR_MTL_M_CLOSE_QUEUE, NativeAdrenoMethodCloseQueue) &&
    SA_EQ(ANDR_MTL_M_BO_CPU_MAP, NativeAdrenoMethodBoCpuMap) && SA_EQ(ANDR_MTL_M_BO_CPU_UNMAP, NativeAdrenoMethodBoCpuUnmap) &&
    SA_EQ(ANDR_MTL_M_GET_DEVICE_INFO, NativeAdrenoMethodGetDeviceInfo) && SA_EQ(ANDR_MTL_M_BO_IMPORT, NativeAdrenoMethodBoImport),
    "ABI methods");
static_assert(SA_EQ(ANDR_MTL_PARAMS_HOST_IMPORT, NativeAdrenoParamsHostImport) &&
    SA_EQ(ANDR_MTL_DEVICE_CLIENT_IOVA, NativeAdrenoDeviceClientIova), "ABI flags");
static_assert((IOReturn)ANDR_IOR_SUCCESS == kIOReturnSuccess && (IOReturn)ANDR_IOR_NO_MEMORY == kIOReturnNoMemory &&
    (IOReturn)ANDR_IOR_NO_RESOURCES == kIOReturnNoResources && (IOReturn)ANDR_IOR_BAD_ARGUMENT == kIOReturnBadArgument &&
    (IOReturn)ANDR_IOR_UNSUPPORTED == kIOReturnUnsupported && (IOReturn)ANDR_IOR_VM_ERROR == kIOReturnVMError &&
    (IOReturn)ANDR_IOR_BUSY == kIOReturnBusy && (IOReturn)ANDR_IOR_TIMEOUT == kIOReturnTimeout &&
    (IOReturn)ANDR_IOR_NOT_READY == kIOReturnNotReady, "IOReturn values");

/* ---- the service (flame 0096 / 0116) ---- */

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class SweetAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(SweetAccelerator);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void publish(unsigned level);
};

OSDefineMetaClassAndStructors(SweetAccelerator, IOAcceleratorES);

static SweetAccelerator *gSweetAccelerator;
static IOLock *gSweetZombieLock;
static OSArray *gSweetZombies;		/* OSData {IOMemoryDescriptor *, pa, bytes} */

IOService *
SweetAccelerator::probe(IOService *provider, SInt32 *score)
{
	return andromeda_mtl_level() ? IOService::probe(provider, score) : NULL;
}

bool
SweetAccelerator::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	gSweetZombieLock = IOLockAlloc();
	gSweetZombies = OSArray::withCapacity(16);
	if (gSweetZombieLock == NULL || gSweetZombies == NULL) {
		IOLog("ANDROMEDA MTL: SweetAccelerator: no memory\n");
		return false;
	}
	gSweetAccelerator = this;
	IOLog("ANDROMEDA MTL: SweetAccelerator (IOAcceleratorES) started, andrmtl=%u: registered once andrgpu step 4 "
	    "has passed\n", andromeda_mtl_level());
	andromeda_mtl_service_started();
	return true;
}

void
SweetAccelerator::publish(unsigned level)
{
	setProperty("model", "Adreno 640");
	setProperty(kIOUserClientClassKey, "SweetAcceleratorUserClient");
	setProperty("AndromedaMetalLevel", (unsigned long long)level, 32);
	if (level >= ANDR_MTL_METAL) {
		/* SweetMetal2.bundle: UniXNU's frontend + metal2vulkan + Turnip (phone/metal2, flame 0116 sweetmtl=2) */
		setProperty("MetalPluginName", "SweetMetal2");
		setProperty("MetalPluginClassName", "SweetMTL2Device");
		setProperty("IOGLBundleName", "SweetMetal2");
	}
	registerService();
	IOLog("ANDROMEDA MTL: IOAcceleratorES registered (%s)\n", level >= ANDR_MTL_METAL ?
	    "MetalPluginName SweetMetal2: Metal devices go to SweetMetal2.bundle" :
	    "no MetalPluginName: user client only, Core Animation stays in software");
}

/* ---- OS layer for andromeda_gpu_mtl.c ---- */

extern "C" void
andr_mtl_os_publish(unsigned level)
{
	if (gSweetAccelerator != NULL) {
		gSweetAccelerator->publish(level);
	}
}

extern "C" void *
andr_mtl_os_phys(uint64_t pa, uint64_t bytes)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)pa, (IOByteCount)bytes,
	    kIODirectionInOut);
	return md;
}

extern "C" void *
andr_mtl_os_import(void *task, uint64_t address, uint64_t bytes, bool *writable)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(address, bytes, kIODirectionInOut, (task_t)task);
	*writable = true;
	if (md != NULL && md->prepare() != kIOReturnSuccess) {
		/* read-only pages: pinned for reading only, and mapped read-only for the GPU (flame pins them too) */
		md->release();
		*writable = false;
		md = IOMemoryDescriptor::withAddressRange(address, bytes, kIODirectionOut, (task_t)task);
		if (md != NULL && md->prepare() != kIOReturnSuccess) {
			md->release();
			md = NULL;
		}
	}
	return md;
}

extern "C" uint64_t
andr_mtl_os_import_pa(void *md, uint64_t offset)
{
	IOByteCount seg = 0;
	return ((IOMemoryDescriptor *)md)->getPhysicalSegment((IOByteCount)offset, &seg, kIOMemoryMapperNone);
}

extern "C" void
andr_mtl_os_import_release(void *md)
{
	IOMemoryDescriptor *m = (IOMemoryDescriptor *)md;
	m->complete();
	m->release();
}

extern "C" void
andr_mtl_os_retain(void *md)
{
	((IOMemoryDescriptor *)md)->retain();
}

/*
 * Pool memory still mapped into some task (a client that freed a buffer
 * before unmapping it, or one that died): its descriptor waits until IOKit has
 * dropped every mapping (retain count back to ours), then the memory goes back
 * to the pool. Swept on every allocation (flame 0115).
 */
struct sweet_zombie {
	IOMemoryDescriptor *md;
	uint64_t pa, bytes;
};

extern "C" void
andr_mtl_os_phys_release(void *md, uint64_t pa, uint64_t bytes)
{
	IOMemoryDescriptor *m = (IOMemoryDescriptor *)md;
	if (m->getRetainCount() <= 1) {
		m->release();
		andr_mtl_pool_free(pa, bytes);
		return;
	}
	struct sweet_zombie z = { m, pa, bytes };
	OSData *d = gSweetZombieLock != NULL ? OSData::withBytes(&z, sizeof(z)) : NULL;
	bool kept = false;
	if (d != NULL) {
		IOLockLock(gSweetZombieLock);
		kept = gSweetZombies->setObject(d);
		IOLockUnlock(gSweetZombieLock);
	}
	OSSafeReleaseNULL(d);
	if (!kept) {
		/* no record: the memory is never reused (a leak, not a reuse under a live mapping) */
		IOLog("ANDROMEDA MTL: %llu KiB of pool memory at 0x%llx leak (still mapped)\n",
		    (unsigned long long)(bytes >> 10), (unsigned long long)pa);
		m->release();
	}
}

extern "C" void
andr_mtl_os_sweep(void)
{
	if (gSweetZombieLock == NULL) {
		return;
	}
	IOLockLock(gSweetZombieLock);
	for (unsigned i = 0; i < gSweetZombies->getCount();) {
		OSData *d = OSDynamicCast(OSData, gSweetZombies->getObject(i));
		const struct sweet_zombie *z = d != NULL ? (const struct sweet_zombie *)d->getBytesNoCopy() : NULL;
		if (z != NULL && z->md->getRetainCount() <= 1) {
			uint64_t pa = z->pa, bytes = z->bytes;
			z->md->release();
			gSweetZombies->removeObject(i);
			andr_mtl_pool_free(pa, bytes);
			continue;
		}
		i++;
	}
	IOLockUnlock(gSweetZombieLock);
}

extern "C" struct andr_mtl_bo *
andr_mtl_os_bo_table(uint32_t n)
{
	return IONewZero(andr_mtl_bo, n);
}

extern "C" void
andr_mtl_os_bo_table_free(struct andr_mtl_bo *t, uint32_t n)
{
	IODelete(t, andr_mtl_bo, n);
}

/* ---- the user client (flame 0097 / 0115) ---- */

class SweetAcceleratorUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetAcceleratorUserClient);

	struct andr_mtl_client fClient;

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAcceleratorUserClient, IOUserClient);

bool
SweetAcceleratorUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	char name[32] = "";
	proc_selfname(name, sizeof(name));
	return andr_mtl_open(&fClient, owningTask, type, name) == ANDR_IOR_SUCCESS;
}

IOReturn
SweetAcceleratorUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

void
SweetAcceleratorUserClient::free(void)
{
	/* waits for the client's last fence, unmaps and releases its buffers, frees its window */
	andr_mtl_close(&fClient);
	IOUserClient::free();
}

IOReturn
SweetAcceleratorUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	/* token = handle; write-combined, as the pool's kernel view (imports have no token) */
	IOMemoryDescriptor *md = (IOMemoryDescriptor *)andr_mtl_cpu_memory(&fClient, (uint32_t)type);
	if (md == NULL) {
		return kIOReturnBadArgument;
	}
	*options = (*options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
	*memory = md;		/* retained; IOUserClient drops it once the mapping exists */
	return kIOReturnSuccess;
}

IOReturn
SweetAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	/* struct methods only: tunnelled {'NTNL', method, payload} through any selector, or selector = method */
	if (args->structureInputDescriptor != NULL || args->structureOutputDescriptor != NULL) {
		return kIOReturnBadArgument;	/* every record is far below the inline limit */
	}
	const void *in = args->structureInput;
	uint32_t isz = in != NULL ? args->structureInputSize : 0;
	void *out = args->structureOutput;
	uint32_t osz = out != NULL ? args->structureOutputSize : 0;
	int r = andr_mtl_external(&fClient, selector, in, isz, out, &osz);
	if (out != NULL) {
		args->structureOutputSize = r == ANDR_IOR_SUCCESS ? osz : 0;
	}
	return (IOReturn)r;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
