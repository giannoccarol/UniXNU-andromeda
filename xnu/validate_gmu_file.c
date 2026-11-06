/* Local offline validation of a caller-supplied GMU image. Never uploads it. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "cepheus_gmu_firmware.h"
int main(int argc,char **argv)
{
	if(argc!=2) { fprintf(stderr,"usage: validate_gmu_file FILE\n");return 2; }
	FILE *f=fopen(argv[1],"rb");if(!f) { perror("open");return 1; }
	if(fseek(f,0,SEEK_END)) { fclose(f);return 1; }
	long size=ftell(f);if(size<=0 || size>1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f);return 1; }
	uint8_t *data=malloc((size_t)size);if(!data) { fclose(f);return 1; }
	if(fread(data,1,(size_t)size,f)!=(size_t)size) { free(data);fclose(f);return 1; } fclose(f);
	struct ceph_gmu_block plan[128];unsigned count=0;
	int error=ceph_gmu_plan(data,(size_t)size,plan,128,&count);
	if(error) { fprintf(stderr,"image validation failed: %d\n",error);free(data);return 1; }
	printf("GMU blocks=%u bytes=%ld (offline only, no upload)\n",count,size);
	for(unsigned i=0;i<count;i++) {
		printf("block %u file=%zu addr=%08x size=%u type=%u value=%08x region=%u\n",
		    i,plan[i].file_offset,plan[i].address,plan[i].size,plan[i].type,plan[i].value,(unsigned)plan[i].region);
	}
	free(data);return 0;
}
