/*
 * Phases F5/F6 on andromeda (Xiaomi Mi Mix 3 5G, SM8150, Adreno 640): the
 * kernel side of Metal. Off unless andrmtl=N, and nothing is published
 * before andrgpu's step 4 (CP_MEM_WRITE) has passed in this boot.
 *
 *   SweetAccelerator : IOAcceleratorES      the service Metal.framework and
 *       Turnip's darwin backend look up (IOServiceMatching("IOAcceleratorES"))
 *   SweetAcceleratorUserClient              the NativeAdreno ABI v1 user
 *       client Turnip speaks (open type 0x4147 or 2): buffer objects, GPU
 *       mappings, imports of the caller's pages, submits, fences, device info
 *
 * Shared by bsd/dev/andromeda_gpu_mtl.c (everything that can be tested on a
 * host: memory pool, GPU address windows, page tables, the ABI methods, ring
 * submits and fences) and iokit/Kernel/arm/SweetAccelerator.cpp (the IOKit
 * classes and the "OS layer" below: memory descriptors, task page pinning,
 * publication). The staging script 0200-stage-andromeda-mtl.sh installs this
 * file in both directories.
 *
 * Sources. A port of ios-flame (Nick Wylynko / NWylynko,
 * https://github.com/NWylynko/ios-flame, commit 745c4341; MIT, patched XNU
 * files APSL 2.0): patches 0096-flame-metal-accelerator and
 * 0116-flame-sweetmetal2-plugin-switch (IOAcceleratorES, the Metal plug-in
 * properties), 0097-flame-metal-userclient (user client skeleton, CPU-map
 * tokens) and 0115-flame-gpu-v2-client-turnip (the v2 client: GPU pool in
 * RAM XNU does not manage, 4 GB address window per connection, BO
 * alloc/import/map/free, zombie descriptors, the a6xx_submit-style ring
 * wrapper with a CACHE_FLUSH_TS fence, hang detection), plus the per-client
 * pool accounting of 0117. The wire ABI and its 'NTNL' tunnel are UniXNU's
 * NativeAdreno (UniXNU contributors, drivers/NativeAdreno/NativeAdrenoABI.h
 * and NativeAdrenoPublish.cpp, MIT): NativeAdrenoABI.h is staged unchanged
 * next to SweetAccelerator.cpp and checked against the mirror below. The
 * hardware is driven only through andromeda_gpu.c / andromeda_gpu_core.c
 * (our port of flame's sweet_gpu.c / sweet_a6xx.c); nothing here powers or
 * resets the GPU. See CREDITS.md.
 *
 * Different from flame, on purpose:
 *   - published only after step 4 passed (flame: at boot, then "not ready");
 *   - no GPU reset/recovery (flame 0108): a hang, an unconfirmed TLB sync or
 *     a thermal stop ends Metal for the boot (clients see device lost, the
 *     display keeps the CPU copy);
 *   - the GPU present path (andrgpupresent, F4) is not used while Metal has
 *     the ring: one ring user at a time;
 *   - imports must be XNU-managed RAM (pmap_valid_address), never the GPU
 *     pools; read-only BOs (permissions without write) and imports that could
 *     only be pinned for reading are mapped read-only for the GPU;
 *   - a submitted IB must lie inside a BO of the caller mapped at that
 *     address (flame: inside the caller's window);
 *   - the BO pool is checked page by page before use (pmap_valid_address,
 *     DRAM bounds, the GPU pool, PStore, MPSS_EFS) and refused otherwise.
 *
 * Boot-args (parsed by andromeda_gpu_mtl.c):
 *   andrmtl=N          0/absent: nothing published, no user client.
 *                      1: after andrgpu step 4, IOAcceleratorES with the
 *                         NativeAdreno user client but no MetalPluginName:
 *                         Metal loads no plug-in, Core Animation stays in
 *                         software; for v2test/vktest (F5).
 *                      2: as 1, plus MetalPluginName/IOGLBundleName
 *                         "SweetMetal2", MetalPluginClassName
 *                         "SweetMTL2Device": processes that ask for a Metal
 *                         device load SweetMetal2.bundle (F6).
 *   andrmtlpool=PA     BO pool base (2 MiB aligned), in RAM XNU does not
 *                      manage. Default 0x1E2000000: below andrgpupool's
 *                      default 0x1FF000000, above the stock 1.5 GiB kernel
 *                      window (ends 0x1E1004000).
 *   andrmtlpoolsize=B  BO pool size (default 0x1D000000, 464 MiB; at least
 *                      0x2000000). Its first 8 MiB hold the GPU page tables.
 *   andrgpuhot=C       (andromeda_gpu.c's limit, default 80) submits re-read
 *                      the GPU temperature every 10 s and stop Metal at/above
 *                      C degC or when unreadable.
 */
#ifndef ANDROMEDA_GPU_MTL_H
#define ANDROMEDA_GPU_MTL_H
#ifdef KERNEL
#include <sys/types.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
#define ANDR_MTL_ASSERT(c, m)	static_assert(c, m)
extern "C" {
#else
#define ANDR_MTL_ASSERT(c, m)	_Static_assert(c, m)
#endif

/* ---- levels, open types, transport ---- */

#define ANDR_MTL_OFF		0u
#define ANDR_MTL_CLIENT		1u	/* user client only (F5) */
#define ANDR_MTL_METAL		2u	/* + the Metal plug-in properties (F6) */

#define ANDR_MTL_OPEN_TYPE	0x4147u		/* the darwin backend's default ('AG') */
#define ANDR_MTL_OPEN_DIRECT	2u
#define ANDR_MTL_TUNNEL		0x4e544e4cu	/* 'NTNL': {magic, method, payload} */
#define ANDR_MTL_PROBE		0xffffffffu	/* tunnel method that only proves the selector */

/* IOReturn values the methods answer (iokit/IOKit/IOReturn.h; SweetAccelerator.cpp checks them). */
#define ANDR_IOR_SUCCESS	0
#define ANDR_IOR_NO_MEMORY	((int)0xe00002bd)
#define ANDR_IOR_NO_RESOURCES	((int)0xe00002be)
#define ANDR_IOR_BAD_ARGUMENT	((int)0xe00002c2)
#define ANDR_IOR_UNSUPPORTED	((int)0xe00002c7)
#define ANDR_IOR_VM_ERROR	((int)0xe00002c8)
#define ANDR_IOR_BUSY		((int)0xe00002d5)
#define ANDR_IOR_TIMEOUT	((int)0xe00002d6)
#define ANDR_IOR_NOT_READY	((int)0xe00002d8)

/* ---- the NativeAdreno ABI v1 (NativeAdrenoABI.h), as C records ----
 * Same sizes and offsets as UniXNU's header (checked here and, against the
 * header itself, in SweetAccelerator.cpp and scripts/test-mtl-abi.py). */

enum {
	ANDR_MTL_ABI_VERSION = 1,
	ANDR_MTL_MAX_BUFFERS = 16,
	ANDR_MTL_MAX_BO_SHIFT = 28,		/* 256 MiB per object */
	ANDR_MTL_M_QUERY = 1,
	ANDR_MTL_M_BO_ALLOC = 2,
	ANDR_MTL_M_BO_MAP = 3,
	ANDR_MTL_M_BO_FREE = 4,
	ANDR_MTL_M_SUBMIT = 5,
	ANDR_MTL_M_WAIT_FENCE = 6,
	ANDR_MTL_M_GET_TIMESTAMP = 7,
	ANDR_MTL_M_GET_STATUS = 8,
	ANDR_MTL_M_SET_METADATA = 9,
	ANDR_MTL_M_GET_METADATA = 10,
	ANDR_MTL_M_CREATE_QUEUE = 11,
	ANDR_MTL_M_CLOSE_QUEUE = 12,
	ANDR_MTL_M_BO_CPU_MAP = 13,
	ANDR_MTL_M_BO_CPU_UNMAP = 14,
	ANDR_MTL_M_GET_DEVICE_INFO = 15,
	ANDR_MTL_M_BO_IMPORT = 16,
	ANDR_MTL_METADATA_BYTES = 64,
	ANDR_MTL_PARAMS_HOST_IMPORT = 1,
	ANDR_MTL_DEVICE_CLIENT_IOVA = 1,
};

typedef struct {
	uint32_t abi_version, input_bits, output_bits, flags;
	uint64_t max_bo_bytes;
} andr_mtl_params_t;

typedef struct {
	uint64_t bytes;
	uint32_t align_log2, contiguous;
} andr_mtl_bo_alloc_t;

typedef struct {
	uint64_t handle;
} andr_mtl_handle_t;			/* BoAllocReply, BoFree, BoCpuMap */

typedef struct {
	uint64_t address, bytes;
} andr_mtl_bo_import_t;

typedef struct {
	uint64_t handle, iova;
	uint32_t permissions, writeback, shareability, reserved;
} andr_mtl_bo_map_t;

typedef struct {
	uint64_t iova;
} andr_mtl_bo_map_reply_t;

typedef struct {
	uint64_t handle, iova, bytes;
} andr_mtl_submit_buffer_t;

typedef struct {
	uint32_t buffer_count, reserved;
	uint64_t queue, wait_fence, wait_timeout_ns;
	andr_mtl_submit_buffer_t buffers[ANDR_MTL_MAX_BUFFERS];
} andr_mtl_submit_t;

typedef struct {
	uint64_t handle;
	uint32_t bytes, reserved;
	char text[ANDR_MTL_METADATA_BYTES];
} andr_mtl_metadata_t;

typedef struct {
	uint64_t value;
} andr_mtl_u64_t;			/* SubmitReply (fence), Timestamp, Queue, QueueReply */

typedef struct {
	uint64_t fence, timeout_ns;
} andr_mtl_wait_fence_t;

typedef struct {
	uint32_t token, reserved;
	uint64_t bytes;
} andr_mtl_cpu_map_reply_t;

typedef struct {
	uint64_t handle;
	uint32_t token, reserved;
} andr_mtl_cpu_unmap_t;

typedef struct {
	uint64_t faults, suspends;
} andr_mtl_status_t;

typedef struct {
	uint32_t chip_id, gmem_bytes;
	uint64_t gmem_base, uche_trap_base, va_start, va_size;
	uint32_t highest_bank_bit, ubwc_swizzle, macrotile_mode, ubwc_version, flags, reserved;
} andr_mtl_device_info_t;

/* NativeAdrenoABI.h's own static_asserts, and the offsets the backend relies on. */
ANDR_MTL_ASSERT(sizeof(andr_mtl_device_info_t) == 64, "ABI device info layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_params_t) == 24, "ABI params layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_bo_alloc_t) == 16, "ABI alloc layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_bo_map_t) == 32, "ABI map layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_bo_import_t) == 16, "ABI import layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_submit_buffer_t) == 24, "ABI buffer layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_submit_t) == 416, "ABI submit layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_handle_t) == 8, "ABI handle layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_cpu_map_reply_t) == 16, "ABI cpu map reply layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_cpu_unmap_t) == 16, "ABI cpu unmap layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_u64_t) == 8, "ABI queue/timestamp/fence layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_metadata_t) == 80, "ABI metadata layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_wait_fence_t) == 16, "ABI wait layout");
ANDR_MTL_ASSERT(sizeof(andr_mtl_status_t) == 16, "ABI status layout");
ANDR_MTL_ASSERT(offsetof(andr_mtl_submit_t, wait_fence) == 16 && offsetof(andr_mtl_submit_t, buffers) == 32,
    "ABI submit offsets");
ANDR_MTL_ASSERT(offsetof(andr_mtl_device_info_t, va_start) == 24 && offsetof(andr_mtl_device_info_t, flags) == 56,
    "ABI device info offsets");
ANDR_MTL_ASSERT(offsetof(andr_mtl_bo_map_t, permissions) == 16 && offsetof(andr_mtl_bo_map_t, reserved) == 28,
    "ABI map offsets");

/* ---- what the device reports (andromeda_gpu_core.c's hw_init programs these) ---- */

#define ANDR_MTL_INPUT_BITS	39u		/* GPU VA: T0SZ 25 (smmu_bank) */
#define ANDR_MTL_OUTPUT_BITS	36u
#define ANDR_MTL_GMEM_BYTES	0x100000u	/* UCHE GMEM range 0x100000..0x1fffff */
#define ANDR_MTL_GMEM_BASE	0x100000ull
#define ANDR_MTL_UCHE_TRAP	0x1fffffffff000ull	/* UCHE_TRAP_BASE 0xfffff000/0x1ffff */
#define ANDR_MTL_HBB		15u		/* UBWC highest bank bit (0x8e08 hbb_lo 2) */
#define ANDR_MTL_UBWC_SWIZZLE	6u
#define ANDR_MTL_UBWC_VERSION	3u

/* ---- GPU address space and memory layout ---- */

/* One window per connection: 4 GB each from 64 GB up (flame), L1 entries
 * 64..319 of the shared GPU page table; the pool (0x10000000), the panel
 * (0x20000000) and the F4 surface slots (4 GB..) stay below. */
#define ANDR_MTL_WIN_BASE	0x1000000000ull
#define ANDR_MTL_WIN_SIZE	0x100000000ull
#define ANDR_MTL_WIN_COUNT	64u
ANDR_MTL_ASSERT(ANDR_MTL_WIN_BASE + ANDR_MTL_WIN_COUNT * ANDR_MTL_WIN_SIZE <= (1ull << ANDR_MTL_INPUT_BITS),
    "windows inside the 39-bit GPU address space");

#define ANDR_MTL_POOL_DEFAULT	0x1e2000000ull
#define ANDR_MTL_POOL_SIZE_DEFAULT 0x1d000000ull
#define ANDR_MTL_POOL_MIN	0x2000000ull
#define ANDR_MTL_PT_BYTES	0x800000ull	/* page tables at the pool's start: 2048 x 4K */
#define ANDR_MTL_UNIT		0x4000ull	/* BO memory comes in 16 KiB units (a CPU page) */
#define ANDR_MTL_FREE_MAX	4096u		/* free-list entries (sorted, merged) */
#define ANDR_MTL_MAX_BOS	65536u		/* per connection */

/* ---- per connection (embedded in SweetAcceleratorUserClient) ---- */

struct andr_mtl_bo {
	void *md;			/* the OS memory object; NULL: free slot */
	uint64_t bytes;			/* as asked (4K multiple) */
	uint64_t len;			/* the descriptor's length: allocations 16K-rounded */
	uint64_t iova;			/* GPU address while mapped, else 0 */
	uint64_t pool_pa;		/* allocations: their pool memory; imports 0 */
	uint8_t imported;		/* the caller's own pages */
	uint8_t writable;		/* imports: pinned for writing too */
	uint8_t gpu_rw;			/* mapped writable for the GPU */
	uint8_t kept;			/* once mapped without a confirmed TLB sync: never released */
	uint8_t pad[4];
};

struct andr_mtl_client {
	void *task;
	uint64_t win_base, win_size;
	struct andr_mtl_bo *bo;
	uint32_t bo_cap, bo_live;
	uint32_t last_fence;		/* of this client's last submit */
	uint32_t open;
	uint64_t pool_bytes, pool_peak;	/* pool memory this client holds (flame 0117) */
	char name[32];
};

/* ---- the C side (andromeda_gpu_mtl.c) ---- */

struct andr_gpu;

unsigned andromeda_mtl_level(void);	/* andrmtl, parsed once */
/*
 * From andromeda_gpu.c once step 4 has passed. true: Metal has the GPU (its
 * ring, fence and page tables) from now on, and the present path must not
 * use it; false: Metal is off or could not set up, the GPU is free.
 */
bool andromeda_gpu_mtl_attach(struct andr_gpu *g, uint64_t gpu_pool_size);
/* From SweetAccelerator::start: the service exists, publish it when the GPU is attached. */
void andromeda_mtl_service_started(void);
bool andromeda_mtl_usable(void);	/* attached and not stopped */

/* name: the calling process (logs only). ANDR_IOR_*. */
int andr_mtl_open(struct andr_mtl_client *c, void *task, uint32_t type, const char *name);
void andr_mtl_close(struct andr_mtl_client *c);
/* One external method: tunnelled {'NTNL', method, payload} or selector = method. */
int andr_mtl_external(struct andr_mtl_client *c, uint32_t selector, const void *in, uint32_t in_size,
    void *out, uint32_t *out_size);
/* The descriptor clientMemoryForType hands out for token, retained (andr_mtl_os_retain); NULL if none. */
void *andr_mtl_cpu_memory(struct andr_mtl_client *c, uint32_t token);
/* Pool memory back (the OS layer's zombie sweep). */
void andr_mtl_pool_free(uint64_t pa, uint64_t bytes);

/* ---- the OS layer (SweetAccelerator.cpp; the host test has its own) ---- */

void *andr_mtl_os_phys(uint64_t pa, uint64_t bytes);	/* descriptor over pool memory; NULL on failure */
/* Pin [address, address + bytes) of task: writable, else read-only (*writable false). NULL if neither. */
void *andr_mtl_os_import(void *task, uint64_t address, uint64_t bytes, bool *writable);
uint64_t andr_mtl_os_import_pa(void *md, uint64_t offset);	/* PA of the 4K piece at offset, 0 on error */
void andr_mtl_os_import_release(void *md);		/* unpin, release */
/* Release a pool descriptor; its memory goes back (andr_mtl_pool_free) once no task maps it. */
void andr_mtl_os_phys_release(void *md, uint64_t pa, uint64_t bytes);
void andr_mtl_os_sweep(void);				/* such memory whose last mapping went */
void andr_mtl_os_retain(void *md);
struct andr_mtl_bo *andr_mtl_os_bo_table(uint32_t n);	/* zeroed, NULL on failure */
void andr_mtl_os_bo_table_free(struct andr_mtl_bo *t, uint32_t n);
void andr_mtl_os_publish(unsigned level);		/* properties + registerService, once */

#ifdef __cplusplus
}
#endif
#endif /* ANDROMEDA_GPU_MTL_H */
