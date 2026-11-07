/*
 * Host test of the sandbox extension operations of patch 0116 (bsd/kern/
 * sweet_amfi.c). scripts/test-sandbox-extensions.sh cuts the functions out of
 * the patched file into body.c and sys.c; here copyin/copyout are memcpy and
 * the arguments are laid out as libsystem_sandbox (iOS 16.7.16) passes them.
 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t user_addr_t;
typedef int64_t SInt64;
typedef int32_t SInt32;
struct proc;
enum { Z_WAITOK = 1, Z_ZERO = 2 };
#define MAXPATHLEN 1024

static int copyin(user_addr_t u, void *k, size_t n) { memcpy(k, (void *)(uintptr_t)u, n); return 0; }
static int copyout(const void *k, user_addr_t u, size_t n) { memcpy((void *)(uintptr_t)u, k, n); return 0; }
static int
copyinstr(const user_addr_t u, void *k, size_t n, size_t *done)
{
	size_t l = strnlen((const char *)(uintptr_t)u, n);
	if (l == n) {
		return ENAMETOOLONG;
	}
	memcpy(k, (const void *)(uintptr_t)u, l + 1);
	*done = l + 1;
	return 0;
}
static void *kalloc_data(size_t n, int flags) { (void)flags; return calloc(1, n); }
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static SInt64 OSIncrementAtomic64(volatile SInt64 *v) { return (*v)++; }
static SInt32 OSIncrementAtomic(volatile SInt32 *v) { return (*v)++; }
static int proc_pid(struct proc *p) { (void)p; return 123; }
static void proc_name(int pid, char *b, int n) { (void)pid; snprintf(b, (size_t)n, "tccd"); }
static int boot_arg = -1;       /* -1: andrsbext not passed */
static int
PE_parse_boot_argn(const char *name, void *v, int n)
{
	(void)n;
	if (boot_arg < 0 || strcmp(name, "andrsbext") != 0) {
		return 0;
	}
	*(int *)v = boot_arg;
	return 1;
}
static uint64_t sweet_sandbox_seen;
#include "body.c"
static int
#include "sys.c"

static int
hex64(const char *t)
{
	for (int i = 0; i < 64; i++) {
		if (!strchr("0123456789abcdef", t[i])) {
			return 0;
		}
	}
	return t[64] == ';';
}

int
main(void)
{
	char *buf = calloc(1, 0x800);
	const char *cls = "com.apple.tcc.kTCCServicePhotos";
	uint64_t gen[7] = { (uintptr_t)cls, 3, 0, 0x10000, (uintptr_t)buf, 456, 7 };
	assert(sweet_sandbox_syscall(NULL, 5, (uintptr_t)gen) == 0);
	assert(hex64(buf));
	assert(strstr(buf, ";000000000000001f;com.apple.tcc.kTCCServicePhotos;03;") != NULL);

	const char *path = "/private/var/mobile/Media/PhotoData";
	uint64_t file[7] = { (uintptr_t)"com.apple.app-sandbox.read", 0, (uintptr_t)path, 0, (uintptr_t)buf, 0, 0 };
	assert(sweet_sandbox_syscall(NULL, 5, (uintptr_t)file) == 0);
	assert(hex64(buf));
	size_t len = strlen(buf), plen = strlen(path);
	assert(len > plen && strcmp(buf + len - plen, path) == 0);

	int64_t handle = -1;
	uint64_t consume[3] = { (uintptr_t)buf, len + 1, (uintptr_t)&handle };
	assert(sweet_sandbox_syscall(NULL, 6, (uintptr_t)consume) == 0);
	assert(handle > 0);
	uint64_t release[3] = { (uint64_t)handle, 0, 0 };
	assert(sweet_sandbox_syscall(NULL, 7, (uintptr_t)release) == 0);

	/* missing buffer or class, a too long name, the container path */
	uint64_t nobuf[7] = { (uintptr_t)cls, 3, 0, 0, 0, 0, 0 };
	assert(sweet_sandbox_syscall(NULL, 5, (uintptr_t)nobuf) == EINVAL);
	char *longname = malloc(MAXPATHLEN + 8);
	memset(longname, 'a', MAXPATHLEN + 7);
	longname[MAXPATHLEN + 7] = 0;
	uint64_t toolong[7] = { (uintptr_t)cls, 0, (uintptr_t)longname, 0, (uintptr_t)buf, 0, 0 };
	assert(sweet_sandbox_syscall(NULL, 5, (uintptr_t)toolong) == ENAMETOOLONG);
	assert(sweet_sandbox_syscall(NULL, 4, 0) == ENOENT);

	/* andrsbext=0: ENOENT as before patch 0116 */
	sweet_sandbox_ext_on = -1;
	boot_arg = 0;
	assert(sweet_sandbox_syscall(NULL, 5, (uintptr_t)gen) == ENOENT);
	assert(sweet_sandbox_syscall(NULL, 6, (uintptr_t)consume) == ENOENT);

	free(longname);
	free(buf);
	puts("sandbox extensions: ok");
	return 0;
}
