#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cepheus_sqe.h"
int main(int argc,char **argv)
{
    uint8_t file[20]={0,0,0,0,0x49,1,0,0,0,0,0,0,0,0x90,0x14,0,1,2,3,4},out[17];
    struct ceph_sqe_image image;
    assert(!ceph_sqe_parse(file,20,&image) && image.version==0x149 && image.offset==4 && image.bytes==16);
    file[4]=0x0a;assert(!ceph_sqe_parse(file,20,&image) && image.version==0x149);
    memset(out,0xaa,sizeof(out));assert(!ceph_sqe_copy(file,20,out,16,&image) && !memcmp(out,file+4,16) && out[16]==0xaa);
    assert(ceph_sqe_copy(file,20,file+4,16,&image)<0);
    assert(ceph_sqe_copy(file,20,out,15,&image)<0);
    for(size_t bytes=0;bytes<16;++bytes)assert(ceph_sqe_parse(file,bytes,&image)<0);
    assert(ceph_sqe_parse(file,19,&image)<0);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>0 && length<=1048576);
        rewind(f);uint8_t *data=malloc((size_t)length),*copy=malloc((size_t)length);assert(data&&copy);
        assert(fread(data,1,(size_t)length,f)==(size_t)length);fclose(f);
        assert(!ceph_sqe_copy(data,(size_t)length,copy,(size_t)length,&image));
        assert(!memcmp(copy,data+4,(size_t)length-4));
        printf("Local SQE: version=%#x words=%u payload_offset=%zu bytes=%zu (RAM copy only)\n",image.version,image.words,image.offset,image.bytes);
        free(data);free(copy);
    }
    puts("SQE parser/copy: legacy/new version fields, alignment/truncation/alias/capacity PASS (not authenticated/executed)");
}
