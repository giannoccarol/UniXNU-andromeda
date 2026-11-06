#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_gmu_firmware.h"
static void u32(uint8_t *p, uint32_t n)
{
	for (unsigned i = 0; i < 4; ++i) { p[i] = (uint8_t)(n >> (8 * i)); }
}
int main(void)
{
	uint8_t file[56] = {0}; struct ceph_gmu_block p[4]; unsigned count = 0;
	u32(file, 0); u32(file + 4, 4); // 4 bytes ITCM
	u32(file + 20, 0x40000); u32(file + 24, 4); // 4 bytes DTCM
	u32(file + 40, 0x60000000); u32(file + 48, 3); // metadata
	assert(!ceph_gmu_plan(file, sizeof(file), p, 4, &count));
	assert(count == 3 && p[0].region == CE_GMU_ITCM && p[1].region == CE_GMU_DTCM);
	assert(p[2].size == 0 && p[2].region == CE_GMU_BAD);
	assert(p[1].file_offset == 36 && p[2].type == 3);
	assert(ceph_gmu_plan(file, sizeof(file) - 1, p, 4, &count) == -1 && count == 0);
	assert(ceph_gmu_plan(file, sizeof(file), p, 1, &count) == -2 && count == 0);
	u32(file + 20, 0); // overlap
	assert(ceph_gmu_plan(file, sizeof(file), p, 4, &count) == -1);
	u32(file + 20, 0x3ffc); u32(file + 24, 8); // crosses TCM boundary
	assert(ceph_gmu_plan(file, sizeof(file), p, 4, &count) == -1);
	u32(file + 20, 0xfffffffc); u32(file + 24, 4);
	assert(ceph_gmu_plan(file, sizeof(file), p, 4, &count) == -1);
	u32(file + 20, 0x40001);
	assert(ceph_gmu_plan(file, sizeof(file), p, 4, &count) == -1);
	assert(ceph_gmu_region_for(0x44000, 0x3c000) == CE_GMU_DCACHE);
	assert(ceph_gmu_region_for(0x60000000, 4096) == CE_GMU_KERNEL);
	assert(ceph_gmu_region_for(0xc0000000, 4) == CE_GMU_BAD);
	assert(ceph_gmu_plan(file,sizeof(file),p,257,&count)==-1 && count==0);
	puts("GMU synthetic image plans: boundaries, truncation, overflow, metadata and overlap PASS");
}
