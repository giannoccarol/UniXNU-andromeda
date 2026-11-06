/*
 * Mi Mix 3 5G (andromeda, SM8150): first steps of the Wi-Fi/modem bring-up
 * of docs/research/wifi-flame.md, entirely inside the kernel. The WCN3990
 * Wi-Fi firmware runs in the WLAN domain of the modem DSP, so before any
 * Wi-Fi there is a modem (MSS Q6) to power and boot through TrustZone PAS.
 *
 * This file implements only the two safe first steps, off unless the boot-arg
 * andrwlan=N is set (0 or absent: nothing is touched):
 *
 *   andrwlan=1  W0, read only: map SMEM and walk its partition table, read the
 *               modem SMP2P item (slave-kernel/ipa/wlan entries and their
 *               state bits), check the modem reserved region is outside the
 *               XNU-managed window, and read cmd-db for the modem proxy and
 *               WLAN rails and the AOP QMP mailbox magic. No writes anywhere.
 *   andrwlan=2  W1, as W0, then power and boot the modem: open the AOP QMP
 *               mailbox, create our outbound SMP2P item, PAS-load the modem
 *               image (INIT_IMAGE / MEM_SETUP / segment load / AUTH_AND_RESET)
 *               with the firmware read from the iOS root filesystem, signal
 *               load_state to the AOP, and wait (bounded, 30 s) for the modem
 *               to raise the SMP2P "ready" bit. Clean failure path; PAS
 *               SHUTDOWN is never issued (it hung vayu).
 *
 * How the firmware reaches the kernel. The modem image is far too big for the
 * Apple device tree (the GPU ZAP fits in the ADT; modem.b* is tens of MiB), so
 * it cannot travel the gpufw,<name> ADT path that andromeda_gpu.c uses. The
 * safest in-kernel way, and the one ios-flame/ios-sweet-family ports already
 * use, is an in-kernel vnode read of the files from the iOS root filesystem
 * (the root is mounted read-write here, rootrw=1). hakios-sm8150's vayu_pas.c
 * reads the modem image from /fw on the root exactly this way (vnode_open,
 * vn_rdwr); ios-flame's sweet_pmem.c / sweet_files.c read devices and files
 * with the same primitives. No new userland interface to physical memory or
 * to TrustZone is added: the kernel reads ordinary files under andrwlanfw=
 * (default /fw/modem), which scripts/rebuild-root.sh stages only when
 * ANDROMEDA_WLAN=1. The files hold no secret, and they are never written.
 *
 * Provenance (see CREDITS.md). The SMEM partition/item walk and allocation,
 * the AOP QMP open/send, the SMP2P outbound-item protocol, and the PAS
 * INIT_IMAGE/MEM_SETUP/segment/AUTH_AND_RESET sequence are ports of
 * ios-flame's phone/wifi/{smem.c,wland.c,scm.c} (Nick Wylynko / NWylynko,
 * https://github.com/NWylynko/ios-flame, MIT; the plan is wifi-flame.md). The
 * "read the modem image from the root by vnode, map its reserved region
 * write-combining, never SHUTDOWN after a start without a stop-ack" discipline
 * follows hakios-sm8150's vayu_pas.c (nomorecoolnicknames; no licence, used as
 * reference only, nothing copied). The andromeda addresses are from the
 * phone's live device tree (phone/<serial>/live-fdt.dts). The single SMC
 * primitive (andr_smc, is-call-available guard, no PAS_SHUTDOWN) is shared
 * with andromeda_gpu.c, whose caution follows the Mi 9 port (YangQi0408 /
 * UniXNU-cepheus). ios-sweet is dipa-studio's.
 *
 * Boot-args:
 *   andrwlan=N        0/absent: nothing touched. 1 = W0 (read only), 2 = W1.
 *   andrwlandelay=S   seconds before the thread starts (default 150, so the
 *                     log path and the rest of the system are up first, and
 *                     after the touch/probe threads).
 *   andrwlanfw=PATH   directory on the iOS root holding modem.mdt and
 *                     modem.b* (default /fw/modem). W1 only.
 *   andrwlanwait=MS   bounded wait for the modem "ready" bit (default 30000).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#if defined(KERNEL)
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/fcntl.h>
#include <mach/mach_time.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern boolean_t pmap_valid_address(uint64_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern void andr_smc(uint64_t x[8]);		/* andromeda_gpu.c: one SMC path */
extern int vprintf(const char *, va_list);
#else	/* host syntax check: stubinc plus the kernel-only declarations */
typedef int64_t off_t;
typedef int errno_t;
typedef char *caddr_t;
typedef struct vnode *vnode_t;
typedef struct vfs_context *vfs_context_t;
typedef struct ucred *kauth_cred_t;
typedef struct proc *proc_t;
typedef unsigned int boolean_t;
typedef uint32_t ppnum_t;
typedef struct _vm_map *vm_map_t;
typedef struct pmap *pmap_t;
typedef int vm_tag_t;
typedef int kma_flags_t;
typedef uint64_t addr64_t;
enum uio_rw { UIO_READ, UIO_WRITE };
#define UIO_SYSSPACE 0
#define IO_UNIT 0x0001
#define FREAD 0x0001
#define KERN_SUCCESS 0
#define TRUE 1
#define Z_WAITOK 0
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ERANGE
#define ERANGE 34
#endif
#ifndef ENOTSUP
#define ENOTSUP 45
#endif
#define KMA_DATA 0
#define KMA_ZERO 0
#define VM_KERN_MEMORY_IOKIT 0
#define VM_WIMG_WCOMB 6
#define VM_WIMG_IO 7
#ifndef PAGE_SIZE
#define PAGE_SIZE 0x4000
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE - 1)
#endif
#define ptoa_64(x) ((uint64_t)(x) << 14)
#define atop_64(x) ((uint64_t)(x) >> 14)
extern vm_map_t kernel_map;
extern pmap_t kernel_pmap;
vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
void ml_io_unmap(vm_offset_t, vm_size_t);
boolean_t pmap_valid_address(uint64_t);
ppnum_t pmap_find_phys(pmap_t, addr64_t);
void flush_dcache64(addr64_t, unsigned, int);
int kmem_alloc_contig(vm_map_t, vm_offset_t *, vm_size_t, vm_offset_t, ppnum_t, ppnum_t, kma_flags_t, vm_tag_t);
void kmem_free(vm_map_t, vm_offset_t, vm_size_t);
void *kalloc_data(vm_size_t, int);
void kfree_data(void *, vm_size_t);
errno_t vnode_open(const char *, int, int, int, vnode_t *, vfs_context_t);
errno_t vnode_close(vnode_t, int, vfs_context_t);
errno_t vnode_size(vnode_t, off_t *, vfs_context_t);
int vn_rdwr(enum uio_rw, vnode_t, caddr_t, int, off_t, int, int, kauth_cred_t, int *, proc_t);
vfs_context_t vfs_context_kernel(void);
kauth_cred_t vfs_context_ucred(vfs_context_t);
proc_t vfs_context_proc(vfs_context_t);
int vprintf(const char *, va_list);
extern void andr_smc(uint64_t x[8]);
#endif

void andromeda_wlan_init(void);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);

#define AW_TAG "ANDROMEDA WLAN:"

/* ---- andromeda addresses (phone/<serial>/live-fdt.dts) ---- */
#define AW_SMEM_BASE		0x86000000ull	/* smem reg <0x86000000 0x200000> */
#define AW_SMEM_SIZE		0x200000ull
#define AW_SMEM_HOST_APPS	0u
#define AW_SMEM_HOST_MODEM	1u
#define AW_SMEM_HOST_GLOBAL	0xfffeu
#define AW_TCSR_MUTEX		0x1f40000ull	/* hwlock@1f40000, stride 0x1000 */
#define AW_TCSR_STRIDE		0x1000ull
#define AW_SMEM_HWLOCK		3u		/* smem hwlocks <.. 3> */
#define AW_APPS_PROC_ID		1u
#define AW_APCS_IPC		0x17c0000cull	/* syscon@17c0000c (apcs) */
#define AW_IPC_AOSS		0u		/* qmp-aop qcom,irq-mask 1 -> bit 0 */
#define AW_IPC_MODEM_SMP2P	14u		/* smp2p-modem qcom,ipc bit 0x0e */
#define AW_QMP_RAM		0x0c300000ull	/* qmp-aop msgram */
#define AW_CMDDB_LOCATOR	0x0c3f000cull	/* cmd-db locator (same as GPU) */
#define AW_SMP2P_OUT		428u		/* master-kernel, qcom,smem <.. 0x1ac> */
#define AW_SMP2P_IN		435u		/* slave-kernel, qcom,smem <0x1b3 ..> */
#define AW_SMEM_MSS_CRASH	421u		/* mss smem-id 0x1a5: crash reason */
#define AW_MPSS_PA		0x8e000000ull	/* modem_region reg[0] */
#define AW_MPSS_SIZE		0x9600000ull	/* modem_region reg[1] (150 MiB) */
#define AW_PAS_MODEM		4u		/* mss qcom,pas-id 0x04 */

/* XNU window bounds, only to prove the modem carve-out is outside it. */
#define AW_DRAM_BASE		0xc0000000ull
#define AW_DRAM_END		0x200000000ull

/* SMCCC SIP, PIL service 2 (yielding), same as andromeda_gpu_core.c's ZAP. */
#define AW_SCM_PIL(cmd)		(0x42000000ull | (2u << 8) | (cmd))
#define AW_SCM_IS_AVAIL		0xc2000601ull
#define AW_PIL_INIT_IMAGE	1u
#define AW_PIL_MEM_SETUP	2u
#define AW_PIL_AUTH_RESET	5u
/* AW_PIL_SHUTDOWN (6) is deliberately never issued: it hung vayu. */

/* ---- device MMIO (32-bit, device memory; barriers like andromeda_gpu.c) ---- */

static vm_offset_t aw_smem_va, aw_apcs_va, aw_qmp_va, aw_tcsr_va;
static vm_offset_t aw_cmddb_va;
static uint32_t aw_cmddb_size;

static uint32_t
dv_rd(vm_offset_t va)
{
	uint32_t v = *(volatile uint32_t *)va;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
dv_wr(vm_offset_t va, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)va = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* SMEM lives inside the one mapping [AW_SMEM_BASE, +AW_SMEM_SIZE). */
static bool
smem_in(uint64_t pa, uint64_t len)
{
	return aw_smem_va && pa >= AW_SMEM_BASE && pa + len <= AW_SMEM_BASE + AW_SMEM_SIZE;
}

static uint32_t
sm_rd32(uint64_t pa)
{
	if (!smem_in(pa, 4)) {
		return 0xffffffffu;
	}
	return dv_rd(aw_smem_va + (vm_offset_t)(pa - AW_SMEM_BASE));
}

static void
sm_wr32(uint64_t pa, uint32_t v)
{
	if (smem_in(pa, 4)) {
		dv_wr(aw_smem_va + (vm_offset_t)(pa - AW_SMEM_BASE), v);
	}
}

static uint16_t
sm_rd16(uint64_t pa)
{
	uint32_t w = sm_rd32(pa & ~3ull);
	return (uint16_t)(w >> (8u * (pa & 2)));
}

static void
aw_kick(unsigned bit)
{
	dv_wr(aw_apcs_va + (vm_offset_t)(AW_APCS_IPC & 0xfff), 1u << bit);
}

/* ---- device mappings ---- */

static int
aw_map_fixed(void)
{
	if (!aw_smem_va) {
		aw_smem_va = ml_io_map((vm_offset_t)AW_SMEM_BASE, (vm_size_t)AW_SMEM_SIZE);
	}
	if (!aw_apcs_va) {
		aw_apcs_va = ml_io_map((vm_offset_t)(AW_APCS_IPC & ~0xfffull), 0x1000);
	}
	if (!aw_qmp_va) {
		aw_qmp_va = ml_io_map((vm_offset_t)AW_QMP_RAM, 0x1000);
	}
	if (!aw_tcsr_va) {
		aw_tcsr_va = ml_io_map((vm_offset_t)(AW_TCSR_MUTEX + AW_TCSR_STRIDE * AW_SMEM_HWLOCK), 0x1000);
	}
	if (!aw_smem_va || !aw_apcs_va || !aw_qmp_va || !aw_tcsr_va) {
		printf(AW_TAG " device mapping failed (smem/apcs/qmp/tcsr)\n");
		return ENOMEM;
	}
	return 0;
}

/* cmd-db: the locator at 0x0c3f000c holds the reserved RAM address+size. */
static int
aw_map_cmddb(void)
{
	if (aw_cmddb_va) {
		return 0;
	}
	vm_offset_t loc = ml_io_map((vm_offset_t)(AW_CMDDB_LOCATOR & ~0xfffull), 0x1000);
	if (!loc) {
		return ENOMEM;
	}
	uint32_t pa = dv_rd(loc + (vm_offset_t)(AW_CMDDB_LOCATOR & 0xfff));
	uint32_t size = dv_rd(loc + (vm_offset_t)((AW_CMDDB_LOCATOR & 0xfff) + 4));
	ml_io_unmap(loc, 0x1000);
	/* Firmware supplies the reserved address; never scan physical RAM. */
	if (pa < 0x80000000u || (pa & 3u) || (size & 3u) || size < 0x100u ||
	    size > 0x100000u || (uint64_t)pa + size > 0x100000000ull) {
		printf(AW_TAG " cmd-db locator invalid (pa %#x size %#x)\n", pa, size);
		return EINVAL;
	}
	aw_cmddb_va = ml_io_map((vm_offset_t)pa, size);
	if (!aw_cmddb_va) {
		return ENOMEM;
	}
	aw_cmddb_size = size;
	if (dv_rd(aw_cmddb_va + 4) != 0x0c0330dbu) {
		printf(AW_TAG " cmd-db magic %#x at %#x: not a command DB\n", dv_rd(aw_cmddb_va + 4), pa);
		ml_io_unmap(aw_cmddb_va, size);
		aw_cmddb_va = 0;
		aw_cmddb_size = 0;
		return EINVAL;
	}
	printf(AW_TAG " cmd-db at %#x size %#x magic ok\n", pa, size);
	return 0;
}

/* Count the levels of an ARC resource and its RPMh slave; -1 if not present.
 * Read-only port of ios-flame's cmddb_aux/levels (wland.c), used only to log
 * that the modem proxy and WLAN rails exist in this boot's cmd-db. */
static int
cmddb_levels(const char *id)
{
	if (!aw_cmddb_va) {
		return -1;
	}
	uint32_t db = 0;	/* offsets are into the cmd-db mapping */
	uint32_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint32_t rh = db + 8 + 16 * r;
		uint32_t w0 = dv_rd(aw_cmddb_va + rh), w1 = dv_rd(aw_cmddb_va + rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		uint32_t slave = w0 & 0xffff;
		for (uint32_t e = 0; e < cnt; e++) {
			uint32_t ent = data + hoff + 24 * e;
			if (ent + 24 > aw_cmddb_size) {
				return -1;
			}
			char name[9];
			for (uint32_t i = 0; i < 8; i++) {
				name[i] = (char)(dv_rd(aw_cmddb_va + ent + (i & ~3u)) >> (8 * (i & 3)));
			}
			name[8] = 0;
			if (strncmp(name, id, 8)) {
				continue;
			}
			uint32_t w5 = dv_rd(aw_cmddb_va + ent + 20);
			uint32_t len = w5 & 0xffff, aux = data + doff + (w5 >> 16);
			(void)aux;
			printf(AW_TAG "   cmd-db %-8s slave %u addr %#x levels %u\n",
			    name, slave, dv_rd(aw_cmddb_va + ent + 16), len / 2);
			return (int)(len / 2);
		}
	}
	return -1;
}

/* ---- SMEM: partition table walk and item lookup (port of flame smem.c) ---- */

#define SM_PTABLE	(AW_SMEM_BASE + AW_SMEM_SIZE - 0x1000)
#define SM_CANARY	0xa5a5u

static struct sm_part {
	uint64_t pa;
	uint32_t size, cacheline;
	uint16_t host0, host1;
} sm_parts[32];
static unsigned sm_nparts;
static int sm_global = -1;

static int
smem_init(void)
{
	sm_nparts = 0;
	sm_global = -1;
	if (sm_rd32(SM_PTABLE) != 0x434f5424u) {	/* "$TOC" */
		printf(AW_TAG " no SMEM partition table (%#x)\n", sm_rd32(SM_PTABLE));
		return EINVAL;
	}
	uint32_t n = sm_rd32(SM_PTABLE + 8);
	for (uint32_t i = 0; i < n && sm_nparts < 32; i++) {
		uint64_t e = SM_PTABLE + 32 + 48ull * i;
		uint32_t off = sm_rd32(e), size = sm_rd32(e + 4), hosts = sm_rd32(e + 12);
		if (!off || !size) {
			continue;
		}
		uint16_t h0 = (uint16_t)hosts, h1 = (uint16_t)(hosts >> 16);
		bool global = h0 == AW_SMEM_HOST_GLOBAL && h1 == AW_SMEM_HOST_GLOBAL;
		/* Only partitions the apps CPU shares, or the global one: reading a
		 * partition access-protected from here stalls the bus for good. */
		if (!global && h0 != AW_SMEM_HOST_APPS && h1 != AW_SMEM_HOST_APPS) {
			continue;
		}
		struct sm_part *p = &sm_parts[sm_nparts];
		p->pa = AW_SMEM_BASE + off;
		p->size = size;
		p->host0 = h0;
		p->host1 = h1;
		p->cacheline = sm_rd32(e + 16);
		if (sm_rd32(p->pa) != 0x54525024u) {	/* "$PRT" */
			printf(AW_TAG " SMEM partition %u bad magic\n", i);
		}
		if (global) {
			sm_global = (int)sm_nparts;
		}
		sm_nparts++;
	}
	return 0;
}

static struct sm_part *
smem_part(unsigned host)
{
	if (host == AW_SMEM_HOST_GLOBAL) {
		return sm_global >= 0 ? &sm_parts[sm_global] : NULL;
	}
	for (unsigned i = 0; i < sm_nparts; i++) {
		if ((sm_parts[i].host0 == AW_SMEM_HOST_APPS && sm_parts[i].host1 == host) ||
		    (sm_parts[i].host1 == AW_SMEM_HOST_APPS && sm_parts[i].host0 == host)) {
			return &sm_parts[i];
		}
	}
	return NULL;
}

static int
part_get(struct sm_part *p, unsigned item, uint64_t *pa, uint32_t *size)
{
	uint32_t free_unc = sm_rd32(p->pa + 12), free_cac = sm_rd32(p->pa + 16);
	for (uint64_t e = p->pa + 32; e < p->pa + free_unc;) {
		if (sm_rd16(e) != SM_CANARY) {
			return -2;
		}
		uint16_t it = sm_rd16(e + 2);
		uint32_t sz = sm_rd32(e + 4);
		uint16_t pad_data = sm_rd16(e + 8), pad_hdr = sm_rd16(e + 10);
		if (it == item) {
			*pa = e + 16 + pad_hdr;
			*size = sz - pad_data;
			return 0;
		}
		e += 16 + pad_hdr + sz;
	}
	uint32_t cl = p->cacheline ? p->cacheline : 64;
	for (uint64_t e = p->pa + p->size - ((16 + cl - 1) & ~(cl - 1)); e > p->pa + free_cac;) {
		if (sm_rd16(e) != SM_CANARY) {
			return -2;
		}
		uint16_t it = sm_rd16(e + 2);
		uint32_t sz = sm_rd32(e + 4);
		if (it == item) {
			*pa = e - sz;
			*size = sz - sm_rd16(e + 8);
			return 0;
		}
		e -= sz + ((16 + cl - 1) & ~(cl - 1));
	}
	return -1;
}

static int
smem_get(unsigned host, unsigned item, uint64_t *pa, uint32_t *size)
{
	struct sm_part *p = smem_part(host);
	if (p && part_get(p, item, pa, size) == 0) {
		return 0;
	}
	if (sm_global >= 0 && part_get(&sm_parts[sm_global], item, pa, size) == 0) {
		return 0;
	}
	return -1;
}

static int
sm_hwlock(void)
{
	for (int i = 0; i < 100000; i++) {
		dv_wr(aw_tcsr_va, AW_APPS_PROC_ID);
		if (dv_rd(aw_tcsr_va) == AW_APPS_PROC_ID) {
			return 0;
		}
	}
	return -1;
}

static void
sm_hwunlock(void)
{
	dv_wr(aw_tcsr_va, 0);
}

static int
smem_alloc(unsigned host, unsigned item, uint32_t size, uint64_t *pa)
{
	uint32_t got;
	if (smem_get(host, item, pa, &got) == 0) {
		return 0;
	}
	struct sm_part *p = smem_part(host);
	if (!p || sm_hwlock()) {
		return -1;
	}
	uint32_t free_unc = sm_rd32(p->pa + 12), free_cac = sm_rd32(p->pa + 16);
	uint32_t asize = (size + 7) & ~7u;
	if (free_unc + 16 + asize > free_cac) {
		sm_hwunlock();
		return -1;
	}
	uint64_t e = p->pa + free_unc;
	sm_wr32(e, SM_CANARY | ((uint32_t)item << 16));
	sm_wr32(e + 4, asize);
	sm_wr32(e + 8, (asize - size));
	sm_wr32(e + 12, 0);
	for (uint32_t o = 0; o < asize; o += 4) {
		sm_wr32(e + 16 + o, 0);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	sm_wr32(p->pa + 12, free_unc + 16 + asize);
	sm_hwunlock();
	*pa = e + 16;
	return 0;
}

/* ---- SMP2P (port of flame wland.c) ---- */

static uint64_t aw_smp2p_out_pa;
static uint32_t aw_modem_state = 0xffffffffu;

static void
smp2p_wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	for (unsigned i = 0; i < 16 && name[i]; i++) {
		n[i] = name[i];
	}
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		sm_wr32(pa + i, w);
	}
}

/* Allocate (if missing) our outbound item 428 and announce master-kernel.
 * Linux allocates it before the modem boots: the modem needs it to exist. */
static int
smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(AW_SMEM_HOST_MODEM, AW_SMP2P_OUT, 20 + 16 * 20, &pa)) {
		printf(AW_TAG " cannot allocate SMP2P item %u\n", AW_SMP2P_OUT);
		return EIO;
	}
	if (sm_rd32(pa) != 0x504d5324u) {	/* "$SMP" */
		sm_wr32(pa + 8, 0 | (1u << 16));	/* local pid 0, remote pid 1 */
		sm_wr32(pa + 12, 16 | (1u << 16));	/* 16 total entries, 1 valid */
		sm_wr32(pa + 16, 0);			/* flags */
		smp2p_wr_name(pa + 20, "master-kernel");
		sm_wr32(pa + 36, 0);
		sm_wr32(pa + 0, 0x504d5324u);
		__asm__ volatile("dsb sy" ::: "memory");
		sm_wr32(pa + 4, 1 | (1u << 8));		/* version 1, feature: SSR ack */
	}
	aw_smp2p_out_pa = pa;
	aw_kick(AW_IPC_MODEM_SMP2P);
	printf(AW_TAG " SMP2P outbound item %u at %#llx ready, modem kicked\n",
	    AW_SMP2P_OUT, (unsigned long long)pa);
	return 0;
}

/* Address of the value word of the modem's inbound entry NAME, 0 = found. */
static int
smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(AW_SMEM_HOST_MODEM, AW_SMP2P_IN, &pa, &sz)) {
		return -1;
	}
	uint32_t n = sm_rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++) {
			nm[k] = (char)(sm_rd32(e + (k & ~3)) >> (8 * (k & 3)));
		}
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static int
smp2p_in_value(const char *name, uint32_t *v)
{
	uint64_t va;
	if (smp2p_in_entry(name, &va)) {
		return -1;
	}
	*v = sm_rd32(va);
	return 0;
}

/* SSR ack and modem state report (port of flame smp2p_service). */
static void
smp2p_service(void)
{
	uint64_t ip;
	uint32_t sz, v;
	if (aw_smp2p_out_pa && smem_get(AW_SMEM_HOST_MODEM, AW_SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (sm_rd32(ip + 16) & 1) ? 2 : 0;
		if ((sm_rd32(aw_smp2p_out_pa + 16) & 2) != want) {
			sm_wr32(aw_smp2p_out_pa + 16, (sm_rd32(aw_smp2p_out_pa + 16) & ~2u) | want);
			aw_kick(AW_IPC_MODEM_SMP2P);
		}
	}
	if (smp2p_in_value("slave-kernel", &v) == 0 && v != aw_modem_state) {
		printf(AW_TAG " modem state %#x%s%s%s%s\n", v, v & 1 ? " ERR-FATAL" : "",
		    v & 2 ? " ready" : "", v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
		aw_modem_state = v;
	}
}

/* ---- AOP QMP (port of flame wland.c qmp_open/qmp_send) ---- */

#define QMP_MAGIC	0x4d41494cu
#define QMP_UP		0x0000ffffu

static int
qmp_wait_eq(uint64_t off, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (dv_rd(aw_qmp_va + (vm_offset_t)off) == want) {
			return 0;
		}
		delay(1000);
	}
	return -1;
}

static int
qmp_open(void)
{
	if (dv_rd(aw_qmp_va) != QMP_MAGIC || dv_rd(aw_qmp_va + 4) != 1) {
		printf(AW_TAG " QMP magic %#x version %u: AOP mailbox not where expected\n",
		    dv_rd(aw_qmp_va), dv_rd(aw_qmp_va + 4));
		return EINVAL;
	}
	if (dv_rd(aw_qmp_va + 0x24) == QMP_UP && dv_rd(aw_qmp_va + 0x2c) == QMP_UP &&
	    dv_rd(aw_qmp_va + 0x30) == QMP_UP) {
		return 0;	/* already open */
	}
	dv_wr(aw_qmp_va + 0x10, dv_rd(aw_qmp_va + 0x0c));
	dv_wr(aw_qmp_va + 0x24, QMP_UP);
	aw_kick(AW_IPC_AOSS);
	if (qmp_wait_eq(0x28, QMP_UP, 1000)) {
		printf(AW_TAG " QMP link not acked\n");
		return ETIMEDOUT;
	}
	dv_wr(aw_qmp_va + 0x2c, QMP_UP);
	aw_kick(AW_IPC_AOSS);
	if (qmp_wait_eq(0x14, QMP_UP, 1000)) {
		printf(AW_TAG " QMP ucore channel not up\n");
		return ETIMEDOUT;
	}
	dv_wr(aw_qmp_va + 0x18, QMP_UP);
	aw_kick(AW_IPC_AOSS);
	if (qmp_wait_eq(0x30, QMP_UP, 1000)) {
		printf(AW_TAG " QMP mcore channel not acked\n");
		return ETIMEDOUT;
	}
	return 0;
}

static int
qmp_send(const char *msg)
{
	int rc = qmp_open();
	if (rc) {
		return rc;
	}
	uint32_t off = dv_rd(aw_qmp_va + 0x38), buf[16] = { 0 };
	for (unsigned i = 0; i < sizeof(buf) - 1 && msg[i]; i++) {
		((char *)buf)[i] = msg[i];
	}
	for (int i = 0; i < 16; i++) {
		dv_wr(aw_qmp_va + (vm_offset_t)off + 4 + 4 * i, buf[i]);
	}
	dv_wr(aw_qmp_va + (vm_offset_t)off, sizeof(buf));
	(void)dv_rd(aw_qmp_va + (vm_offset_t)off);
	aw_kick(AW_IPC_AOSS);
	rc = qmp_wait_eq(off, 0, 1000);
	printf(AW_TAG " QMP \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- firmware from the iOS root (in-kernel vnode read, like vayu_pas.c) ---- */

static char aw_fwdir[128] = "/fw/modem";

/* Read a whole file under aw_fwdir into a kalloc'd buffer; caller frees. */
static uint8_t *
aw_read_file(const char *name, uint64_t *len)
{
	char path[192];
	snprintf(path, sizeof(path), "%s/%s", aw_fwdir, name);
#if defined(KERNEL)
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		printf(AW_TAG " cannot open %s\n", path);
		return NULL;
	}
	off_t size = 0;
	if (vnode_size(vp, &size, ctx) != 0 || size <= 0 || (uint64_t)size > AW_MPSS_SIZE) {
		printf(AW_TAG " %s: bad size %lld\n", path, (long long)size);
		vnode_close(vp, FREAD, ctx);
		return NULL;
	}
	uint8_t *buf = kalloc_data((vm_size_t)size, Z_WAITOK);
	if (!buf) {
		vnode_close(vp, FREAD, ctx);
		return NULL;
	}
	int resid = 0;
	int err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)size, 0, UIO_SYSSPACE, IO_UNIT,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_close(vp, FREAD, ctx);
	if (err || resid) {
		printf(AW_TAG " %s: read error %d, %d left\n", path, err, resid);
		kfree_data(buf, (vm_size_t)size);
		return NULL;
	}
	*len = (uint64_t)size;
	return buf;
#else
	(void)path;
	(void)len;
	return NULL;
#endif
}

static void
aw_free_file(uint8_t *buf, uint64_t len)
{
#if defined(KERNEL)
	if (buf) {
		kfree_data(buf, (vm_size_t)len);
	}
#else
	(void)buf;
	(void)len;
#endif
}

/* One SMC through the shared andr_smc path. fn already SCM_PIL(cmd). */
static int64_t
aw_scm(uint64_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { fn, arginfo, a, b, c, 0, 0, 0 };
	andr_smc(x);
	if (r1) {
		*r1 = x[1];
	}
	return (int64_t)x[0];
}

static bool
aw_pas_auth_available(void)
{
	uint64_t r1 = 0;
	int64_t rc = aw_scm(AW_SCM_IS_AVAIL, 1, AW_SCM_PIL(AW_PIL_AUTH_RESET), 0, 0, &r1);
	printf(AW_TAG " is-call-available(auth) rc %lld avail %llu\n",
	    (long long)rc, (unsigned long long)r1);
	/* A clean "0" means definitely not implemented; anything else proceeds. */
	return !(rc == 0 && r1 == 0);
}

/* ELF32 program header. */
struct aw_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

/* PAS-load and authenticate the modem. Returns 0 on AUTH_AND_RESET success. */
static int
aw_pas_load_modem(void)
{
	uint64_t mlen = 0;
	uint8_t *mdt = aw_read_file("modem.mdt", &mlen);
	if (!mdt) {
		return ENOENT;
	}
	if (mlen < 0x34) {
		aw_free_file(mdt, mlen);
		return EINVAL;
	}
	uint32_t phoff = 0, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	if (!phnum || phnum > 64 || (uint64_t)phoff + 32ull * phnum > mlen) {
		printf(AW_TAG " modem.mdt program headers out of range (phoff %#x phnum %u)\n", phoff, phnum);
		aw_free_file(mdt, mlen);
		return EINVAL;
	}
	struct aw_phdr *ph = (struct aw_phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++) {
		if (((ph[i].flags >> 24) & 7) == 2) {
			hash = (int)i;
		}
	}
	if (hash < 0) {
		printf(AW_TAG " modem.mdt has no hash segment\n");
		aw_free_file(mdt, mlen);
		return EINVAL;
	}

	/* 1. metadata = ELF header + program headers + hash segment, in a
	 *    physically contiguous kernel buffer whose PA goes to TrustZone. */
	uint64_t ehsz = (uint64_t)phoff + 32ull * phnum;
	uint64_t meta_len = ehsz + ph[hash].filesz;
	uint8_t *hashbuf = NULL;
	uint64_t hashlen = 0;
	const uint8_t *hashsrc;
	if ((uint64_t)ph[hash].offset + ph[hash].filesz <= mlen) {
		hashsrc = mdt + ph[hash].offset;
	} else {
		char hn[16];
		snprintf(hn, sizeof(hn), "modem.b%02d", hash);
		hashbuf = aw_read_file(hn, &hashlen);
		if (!hashbuf || hashlen < ph[hash].filesz) {
			printf(AW_TAG " hash segment %s missing/short\n", hn);
			aw_free_file(hashbuf, hashlen);
			aw_free_file(mdt, mlen);
			return EINVAL;
		}
		hashsrc = hashbuf;
	}
	vm_offset_t meta_va = 0;
	int rc_i = EIO;
#if defined(KERNEL)
	vm_size_t asize = (vm_size_t)((meta_len + PAGE_MASK) & ~((uint64_t)PAGE_MASK));
	if (kmem_alloc_contig(kernel_map, &meta_va, asize, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		meta_va = 0;
	}
	if (meta_va) {
		memcpy((void *)meta_va, mdt, (size_t)ehsz);
		memcpy((void *)(meta_va + (vm_offset_t)ehsz), hashsrc, (size_t)ph[hash].filesz);
		uint64_t meta_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)meta_va));
		flush_dcache64((addr64_t)meta_pa, (unsigned)asize, TRUE);
		uint64_t r1 = 0;
		int64_t rc = aw_scm(AW_SCM_PIL(AW_PIL_INIT_IMAGE), 2 | (2u << 6), AW_PAS_MODEM, meta_pa, 0, &r1);
		printf(AW_TAG " PAS init image (%llu bytes at %#llx) -> %lld/%#llx\n",
		    (unsigned long long)meta_len, (unsigned long long)meta_pa, (long long)rc,
		    (unsigned long long)r1);
		rc_i = (rc || r1) ? EIO : 0;
		kmem_free(kernel_map, meta_va, asize);
	}
#else
	(void)meta_va;
	(void)hashsrc;
	(void)meta_len;
#endif
	aw_free_file(hashbuf, hashlen);
	if (rc_i) {
		aw_free_file(mdt, mlen);
		return rc_i;
	}

	/* 2. tell TrustZone where the image will live. */
	uint64_t r1 = 0;
	int64_t rc = aw_scm(AW_SCM_PIL(AW_PIL_MEM_SETUP), 3, AW_PAS_MODEM, AW_MPSS_PA, AW_MPSS_SIZE, &r1);
	printf(AW_TAG " PAS mem setup %#llx+%#llx -> %lld/%#llx\n",
	    (unsigned long long)AW_MPSS_PA, (unsigned long long)AW_MPSS_SIZE, (long long)rc,
	    (unsigned long long)r1);
	if (rc || r1) {
		aw_free_file(mdt, mlen);
		return EIO;
	}

	/* 3. copy each loadable segment to its address in the modem carve-out.
	 *    The region is a reserved PIL carve-out, never XNU-managed, mapped
	 *    write-combining (like vayu); no relocation (reject out-of-region). */
	int err = 0;
#if defined(KERNEL)
	vm_offset_t region = ml_io_map_unmappable((vm_offset_t)AW_MPSS_PA, (vm_size_t)AW_MPSS_SIZE, VM_WIMG_WCOMB);
	if (!region) {
		printf(AW_TAG " cannot map the modem region\n");
		aw_free_file(mdt, mlen);
		return ENOMEM;
	}
	uint64_t total = 0;
	for (uint32_t i = 0; i < phnum && !err; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz) {
			continue;
		}
		uint64_t pa = ph[i].paddr;
		if (pa < AW_MPSS_PA || pa + ph[i].memsz > AW_MPSS_PA + AW_MPSS_SIZE) {
			printf(AW_TAG " segment %u at %#llx outside the region (no relocation here)\n",
			    i, (unsigned long long)pa);
			err = ERANGE;
			break;
		}
		volatile uint8_t *dst = (volatile uint8_t *)(region + (vm_offset_t)(pa - AW_MPSS_PA));
		if (ph[i].filesz) {
			char sn[16];
			snprintf(sn, sizeof(sn), "modem.b%02u", i);
			uint64_t slen = 0;
			uint8_t *seg = aw_read_file(sn, &slen);
			if (!seg || slen < ph[i].filesz) {
				printf(AW_TAG " %s missing/short\n", sn);
				aw_free_file(seg, slen);
				err = EIO;
				break;
			}
			for (uint32_t o = 0; o < ph[i].filesz; o++) {
				dst[o] = seg[o];
			}
			aw_free_file(seg, slen);
			total += ph[i].filesz;
		}
		for (uint64_t o = ph[i].filesz; o < ph[i].memsz; o++) {
			dst[o] = 0;
		}
	}
	__asm__ volatile("dsb osh" ::: "memory");
	ml_io_unmap(region, (vm_size_t)AW_MPSS_SIZE);
	printf(AW_TAG " %llu bytes of modem segments loaded\n", (unsigned long long)total);
#else
	(void)hash;
#endif
	aw_free_file(mdt, mlen);
	if (err) {
		return err;
	}

	/* 4. this platform uses qcom,signal-aop: the AOP raises the proxy CX/MSS
	 *    corners when it sees load_state=on, so no AP RPMh vote is needed. */
	(void)qmp_send("{class: image, res: load_state, name: modem, val: on}");

	/* Forget any stale state bits from a previous run before auth. */
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0) {
		sm_wr32(ve, 0);
	}
	aw_modem_state = 0xffffffffu;

	/* 5. authenticate and release the Q6 out of reset. */
	rc = aw_scm(AW_SCM_PIL(AW_PIL_AUTH_RESET), 1, AW_PAS_MODEM, 0, 0, &r1);
	printf(AW_TAG " PAS auth and reset -> %lld/%#llx\n", (long long)rc, (unsigned long long)r1);
	return (rc || r1) ? EIO : 0;
}

/* ---- W0: read-only reconnaissance ---- */

static void
aw_w0(void)
{
	printf(AW_TAG " W0: read-only reconnaissance\n");

	/* SMEM partition table and the items that matter. */
	if (smem_init() == 0) {
		printf(AW_TAG " SMEM: %u partitions, global %d\n", sm_nparts, sm_global);
		for (unsigned i = 0; i < sm_nparts; i++) {
			struct sm_part *p = &sm_parts[i];
			printf(AW_TAG "   part %u: hosts %u-%u pa %#llx size %#x\n", i, p->host0,
			    p->host1, (unsigned long long)p->pa, p->size);
		}
		static const unsigned items[] = { AW_SMEM_MSS_CRASH, AW_SMP2P_OUT, AW_SMP2P_IN,
		    478, 479, 480 };
		for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
			uint64_t pa;
			uint32_t sz;
			if (smem_get(AW_SMEM_HOST_MODEM, items[i], &pa, &sz) == 0) {
				printf(AW_TAG "   item %u: pa %#llx size %u\n", items[i],
				    (unsigned long long)pa, sz);
			} else {
				printf(AW_TAG "   item %u: absent\n", items[i]);
			}
		}
		/* The modem SMP2P inbound entries and their state (read only). */
		uint32_t v;
		if (smp2p_in_value("slave-kernel", &v) == 0) {
			printf(AW_TAG "   smp2p slave-kernel = %#x%s%s\n", v,
			    v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "");
		}
		if (smp2p_in_value("wlan", &v) == 0) {
			printf(AW_TAG "   smp2p wlan = %#x%s%s\n", v,
			    v & 1 ? " force-fatal" : "", v & 2 ? " early-crash" : "");
		}
	}

	/* cmd-db: the modem proxy and the WLAN/BT rails this boot exposes. */
	if (aw_map_cmddb() == 0) {
		static const char *res[] = { "cx.lvl", "mss.lvl", "ldoa1", "ldoa7", "ldoc2",
		    "ldoc11", "rfclka2" };
		for (unsigned i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
			if (cmddb_levels(res[i]) < 0) {
				printf(AW_TAG "   cmd-db %-8s: not present\n", res[i]);
			}
		}
	}

	/* AOP QMP mailbox magic (read only, no kick). */
	printf(AW_TAG " QMP magic %#x version %u (expect %#x/1)\n",
	    dv_rd(aw_qmp_va), dv_rd(aw_qmp_va + 4), QMP_MAGIC);

	/* The modem reserved region must lie outside the XNU-managed window. */
	bool managed = false;
	for (uint64_t o = 0; o < AW_MPSS_SIZE; o += 0x100000ull) {
		if (pmap_valid_address(AW_MPSS_PA + o)) {
			managed = true;
			break;
		}
	}
	printf(AW_TAG " modem region %#llx+%#llx: %s, XNU window %#llx..%#llx\n",
	    (unsigned long long)AW_MPSS_PA, (unsigned long long)AW_MPSS_SIZE,
	    managed ? "OVERLAPS XNU-MANAGED RAM (unsafe)" : "outside XNU-managed RAM (ok)",
	    (unsigned long long)AW_DRAM_BASE, (unsigned long long)AW_DRAM_END);
	printf(AW_TAG " W0 done\n");
}

/* ---- W1: power and boot the modem through TrustZone PAS ---- */

static int
aw_w1(unsigned wait_ms)
{
	printf(AW_TAG " W1: power and boot the modem\n");

	/* Only ask TrustZone first (the Mi 9 safeguard); a clean "no" stops. */
	if (!aw_pas_auth_available()) {
		printf(AW_TAG " PAS auth not available under this UEFI/TrustZone; stopping\n");
		return ENOTSUP;
	}

	/* The modem reserved region must be outside XNU-managed RAM before we
	 * write the image into it (the "big trap" otherwise). Fail-safe. */
	for (uint64_t o = 0; o < AW_MPSS_SIZE; o += 0x100000ull) {
		if (pmap_valid_address(AW_MPSS_PA + o)) {
			printf(AW_TAG " modem region %#llx is XNU-managed; refusing to write it\n",
			    (unsigned long long)AW_MPSS_PA);
			return EPERM;
		}
	}

	/* Open the AOP mailbox (needed for load_state) and create the outbound
	 * SMP2P item before the modem starts, as Linux does. */
	if (qmp_open()) {
		printf(AW_TAG " AOP QMP did not open; stopping before the modem boot\n");
		return EIO;
	}
	if (smp2p_create()) {
		return EIO;
	}

	int rc = aw_pas_load_modem();
	if (rc) {
		printf(AW_TAG " modem PAS load failed (%d); not booted, no SHUTDOWN issued\n", rc);
		return rc;
	}

	/* Bounded wait for the modem to raise "ready" (bit 1) without
	 * "err-fatal" (bit 0). Never spin forever; never call SHUTDOWN. */
	unsigned steps = wait_ms / 100u;
	for (unsigned t = 0; t <= steps; t++) {
		uint32_t v;
		smp2p_service();
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1)) {
			printf(AW_TAG " modem READY after %u ms\n", t * 100u);
			return 0;
		}
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 1)) {
			printf(AW_TAG " modem ERR-FATAL (%#x); stopping, no SHUTDOWN (see SMEM item %u)\n",
			    v, AW_SMEM_MSS_CRASH);
			return EIO;
		}
		delay(100 * 1000);
	}
	printf(AW_TAG " no READY from the modem in %u ms; stopping, no SHUTDOWN\n", wait_ms);
	return ETIMEDOUT;
}

/* ---- gated bring-up thread ---- */

static unsigned aw_step, aw_delay_s = 150, aw_wait_ms = 30000;
static unsigned aw_sleep_chan;

static void
aw_wait_s(unsigned s)
{
	for (unsigned i = 0; i < s; i++) {
		uint64_t deadline;
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&aw_sleep_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
aw_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	if (aw_delay_s) {
		aw_wait_s(aw_delay_s);
	}
	printf(AW_TAG " andrwlan=%u: starting (fw dir %s)\n", aw_step, aw_fwdir);
	if (aw_map_fixed()) {
		goto done;
	}
	aw_w0();
	if (aw_step >= 2) {
		(void)aw_w1(aw_wait_ms);
	}
done:
	printf(AW_TAG " bring-up thread finished\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
andromeda_wlan_init(void)
{
	static bool started;
	thread_t thread;
	(void)PE_parse_boot_argn("andrwlan", &aw_step, sizeof(aw_step));
	(void)PE_parse_boot_argn("andrwlandelay", &aw_delay_s, sizeof(aw_delay_s));
	(void)PE_parse_boot_argn("andrwlanwait", &aw_wait_ms, sizeof(aw_wait_ms));
	(void)PE_parse_boot_argn("andrwlanfw", aw_fwdir, sizeof(aw_fwdir));
	if (!aw_step || started) {
		return;
	}
	if (aw_wait_ms < 1000u || aw_wait_ms > 120000u) {
		aw_wait_ms = 30000u;
	}
	started = true;
	printf(AW_TAG " andrwlan=%u delay=%us wait=%ums: starting bring-up thread\n",
	    aw_step, aw_delay_s, aw_wait_ms);
	if (kernel_thread_start(aw_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
