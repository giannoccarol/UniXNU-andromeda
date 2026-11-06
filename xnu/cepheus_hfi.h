/* A640 HFI v2 SPSC queues. Caller serializes producers/consumers and owns
 * established GMU mappings. Cache callbacks are required even on coherent
 * memory (barriers still matter). No firmware execution in this module.
 * All lengths/indices are DWORDS. Keep one slot empty to distinguish full.
 */
#ifndef CEPHEUS_HFI_H
#define CEPHEUS_HFI_H
#include <stddef.h>
#include <stdint.h>
enum { CE_HFI_OK = 0, CE_HFI_EMPTY = 1, CE_HFI_FULL = 2,
       CE_HFI_INVALID = -1, CE_HFI_SMALL = -2, CE_HFI_IO = -3,
       CE_HFI_POISONED = -4 };
struct ceph_hfi_ops {
	void *ctx;
	int (*acquire)(void *); /* invalidate peer header+data, acquire barrier */
	/* Clean payload and complete its release barrier BEFORE updating index.
	 * Header-index cache lines must not share peer-owned dirty fields unless
	 * the platform provides coherent mappings; volatile is not cache coherency. */
	int (*publish_data)(void *);
	int (*publish_index)(void *); /* clean owned index, complete release barrier */
	int (*notify)(void *);  /* doorbell after publish */
};
struct ceph_hfi_queue {
	volatile uint32_t *words;
	volatile uint32_t *read_index, *write_index;
	unsigned capacity, major;
	struct ceph_hfi_ops io;
	int poisoned; /* uncertain publication: no retries until reset/recreation */
};
/* Ordering around the index itself is distinct from a cache-maintenance
 * callback made BEFORE loading it. After observing peer publication, payload
 * loads must wait; before returning a slot, payload reads must be complete.
 * Full barrier also handles Normal-NC shared memory on arm64. */
static inline void ceph_hfi_memory_order(void)
{
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ volatile("dsb sy" ::: "memory");
#elif !defined(KERNEL)
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error "HFI shared memory requires arm64"
#endif
}
static inline uint32_t ceph_hfi_header(unsigned id, unsigned words, unsigned type, unsigned seq)
{
	if (id > 255 || !words || words > 255 || type > 15 || seq > 4095) { return 0; }
	return id | (words << 8) | (type << 16) | (seq << 20);
}
static inline unsigned ceph_hfi_padded(unsigned count, unsigned major)
{
	return major == 2 ? (count + 3u) & ~3u : count;
}
static inline int ceph_hfi_indices(struct ceph_hfi_queue *q, unsigned *r, unsigned *w)
{
	if (!q || !q->words || !q->read_index || !q->write_index || !r || !w ||
	    (((uintptr_t)q->words|(uintptr_t)q->read_index|(uintptr_t)q->write_index)&3u) ||
	    !q->io.acquire || !q->io.publish_data || !q->io.publish_index || !q->io.notify ||
	    q->capacity < 8 || q->capacity > 1024 || (q->capacity & 3u) ||
	    (q->major != 1 && q->major != 2)) { return CE_HFI_INVALID; }
	if (q->poisoned) { return CE_HFI_POISONED; }
	if (q->io.acquire(q->io.ctx)) { return CE_HFI_IO; }
	*r = *q->read_index; *w = *q->write_index;
	ceph_hfi_memory_order();
	if (*r >= q->capacity || *w >= q->capacity ||
	    (q->major == 2 && ((*r | *w) & 3u))) { return CE_HFI_INVALID; }
	return 0;
}
static inline int ceph_hfi_write(struct ceph_hfi_queue *q, const uint32_t *msg, unsigned count)
{
	unsigned r, w;
	int error = ceph_hfi_indices(q, &r, &w);
	if (error) { return error; }
	if (!msg || !count || count > 255 || ((msg[0] >> 8) & 255u) != count) { return CE_HFI_INVALID; }
	uintptr_t begin=(uintptr_t)q->words,input=(uintptr_t)msg;
	if (q->capacity*sizeof(uint32_t)>UINTPTR_MAX-begin || count*sizeof(uint32_t)>UINTPTR_MAX-input ||
	    (input<begin+q->capacity*sizeof(uint32_t) && begin<input+count*sizeof(uint32_t))) { return CE_HFI_INVALID; }
	unsigned used = (w + q->capacity - r) % q->capacity;
	unsigned padded = ceph_hfi_padded(count, q->major);
	if (padded >= q->capacity - used) { return CE_HFI_FULL; }
	for (unsigned i = 0; i < padded; ++i) {
		q->words[(w + i) % q->capacity] = i < count ? msg[i] : 0xfafafafau;
	}
	if (q->io.publish_data(q->io.ctx)) { q->poisoned=1; return CE_HFI_IO; }
	ceph_hfi_memory_order();
	*q->write_index = (w + padded) % q->capacity;
	if (q->io.publish_index(q->io.ctx) || q->io.notify(q->io.ctx)) {
		q->poisoned=1; return CE_HFI_IO;
	}
	return 0;
}
static inline int ceph_hfi_read(struct ceph_hfi_queue *q, uint32_t *out, unsigned capacity, unsigned *count)
{
	if (!count) { return CE_HFI_INVALID; }
	*count = 0;
	unsigned r, w;
	int error = ceph_hfi_indices(q, &r, &w);
	if (error) { return error; }
	if (r == w) { return CE_HFI_EMPTY; }
	unsigned length = (q->words[r] >> 8) & 255u;
	unsigned padded = ceph_hfi_padded(length, q->major);
	unsigned used = (w + q->capacity - r) % q->capacity;
	if (!length || padded > used || padded >= q->capacity) { return CE_HFI_INVALID; }
	if (!out || length > capacity) { return CE_HFI_SMALL; }
	uintptr_t begin=(uintptr_t)q->words,output=(uintptr_t)out;
	if (q->capacity*sizeof(uint32_t)>UINTPTR_MAX-begin || length*sizeof(uint32_t)>UINTPTR_MAX-output ||
	    (output<begin+q->capacity*sizeof(uint32_t) && begin<output+length*sizeof(uint32_t))) { return CE_HFI_INVALID; }
	for (unsigned i = 0; i < length; ++i) { out[i] = q->words[(r + i) % q->capacity]; }
	ceph_hfi_memory_order(); /* copy finishes before firmware may reuse slots */
	*q->read_index = (r + padded) % q->capacity;
	if (q->io.publish_index(q->io.ctx)) { q->poisoned=1; return CE_HFI_IO; }
	*count = length;
	return 0;
}
/* v2 ACK carries the original command header in word1, status in word2.
 * Match the ENTIRE header (id/type/length/sequence), not just wrapping seq.
 * Caller must not reuse a sequence while an older command is outstanding.
 */
static inline int ceph_hfi_ack(const uint32_t *msg, unsigned count, uint32_t sent)
{
	return msg && count >= 3 && ((msg[0] >> 8) & 255u) == count &&
	    ((msg[0] >> 16) & 15u) == 1 && msg[1] == sent && msg[2] == 0;
}
/* Queue table matches 4 headers (12 DWORDs), three enabled, DSP disabled.
 * table requires 54 DWORDs; rings start at 224 bytes, each 4KiB. */
static inline int ceph_hfi_table(uint32_t *table, unsigned words, uint64_t gmu_base, size_t bytes)
{
	if (!table || words < 54 || (gmu_base & 4095u) || bytes < 224 + 4 * 4096 ||
	    gmu_base > UINT32_MAX - (224 + 4 * 4096)) { return CE_HFI_INVALID; }
	for (unsigned i = 0; i < 54; ++i) { table[i] = 0; }
	table[1] = 54; table[2] = 6; table[3] = 12; table[4] = 4; table[5] = 4;
	static const unsigned priority[] = {10, 10, 40, 20};
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t *h = table + 6 + i * 12;
		h[0] = i == 3 ? 0 : 1;
		h[1] = (uint32_t)gmu_base + 224 + i * 4096;
		h[2] = i | (priority[i] << 8); h[3] = 1024;
		h[6] = h[7] = h[8] = 1;
	}
	return 0;
}
#endif
