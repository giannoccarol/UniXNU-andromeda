/* A640 GMU block-image validation, following kgsl_gmu.h and load_gmu_fw().
 * The file is a sequence of LE {addr,size,type,value}, followed by size bytes.
 * Zero-size records are metadata, never payload. This only builds a plan;
 * it does not authenticate Qualcomm firmware or start/upload to the GPU.
 * Backing GMU/SMMU mappings must be established by a future loader.
 */
#ifndef CEPHEUS_GMU_FIRMWARE_H
#define CEPHEUS_GMU_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
enum ceph_gmu_region { CE_GMU_ITCM, CE_GMU_ICACHE, CE_GMU_DTCM, CE_GMU_DCACHE,
                      CE_GMU_KERNEL, CE_GMU_USER, CE_GMU_BAD };
struct ceph_gmu_block {
	uint32_t address, size, type, value;
	size_t file_offset;
	enum ceph_gmu_region region;
};
static uint32_t ceph_gmu_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static enum ceph_gmu_region ceph_gmu_region_for(uint32_t addr, uint32_t size)
{
	static const uint32_t base[] = {0, 0x4000, 0x40000, 0x44000, 0x60000000, 0x80000000};
	static const uint32_t length[] = {0x4000, 0x3c000, 0x4000, 0x3c000, 0x20000000, 0x40000000};
	for (unsigned i = 0; i < 6; ++i) {
		if (addr >= base[i] && addr - base[i] < length[i] &&
		    size <= length[i] - (addr - base[i])) { return (enum ceph_gmu_region)i; }
	}
	return CE_GMU_BAD;
}
/* Caller supplies bounded plan storage. Validate all before publishing count.
 * Reject overlaps rather than applying order-dependent code overwrites.
 * -1 malformed, -2 insufficient plan capacity; *count=0 on any failure.
 */
static int ceph_gmu_plan(const uint8_t *file, size_t size,
    struct ceph_gmu_block *plan, unsigned capacity, unsigned *count)
{
	if (!count) { return -1; }
	*count = 0;
	if (!file || !plan || !size || size > 1024u * 1024u || !capacity || capacity>256) { return -1; }
	size_t pos = 0;
	unsigned n = 0;
	while (pos < size) {
		if (size - pos < 16) { return -1; }
		if (n == capacity) { return -2; }
		struct ceph_gmu_block b;
		b.address = ceph_gmu_le32(file + pos);
		b.size = ceph_gmu_le32(file + pos + 4);
		b.type = ceph_gmu_le32(file + pos + 8);
		b.value = ceph_gmu_le32(file + pos + 12);
		b.file_offset = pos + 16;
		pos += 16;
		if (b.size > size - pos) { return -1; }
		b.region = CE_GMU_BAD; // zero-size metadata is not an upload range
		if (b.size) {
			if ((b.address & 3u) || (b.size & 3u)) { return -1; }
			b.region = ceph_gmu_region_for(b.address, b.size);
			if (b.region == CE_GMU_BAD) { return -1; }
			for (unsigned i = 0; i < n; ++i) {
				if (plan[i].size && (uint64_t)b.address < (uint64_t)plan[i].address + plan[i].size &&
				    (uint64_t)plan[i].address < (uint64_t)b.address + b.size) { return -1; }
			}
		}
		plan[n++] = b;
		pos += b.size;
	}
	*count = n;
	return 0;
}
#endif
