#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cepheus_gpu_firmware.h"
int main(int argc,char **argv)
{
    static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    uint8_t hash[32];assert(!ceph_firmware_sha256((const uint8_t *)"abc",3,hash) && !memcmp(hash,abc,32));
    uint8_t *data=calloc(1,37680);assert(data);
    assert(ceph_firmware_verify(CE_FW_A640_GMU,data,37680)<0);
    assert(ceph_firmware_verify(CE_FW_A630_SQE,data,32304)<0);
    assert(ceph_firmware_verify(CE_FW_A640_GMU,NULL,37680)<0);
    assert(ceph_firmware_verify(CE_FW_A640_GMU,data,37679)<0);
    free(data);
    if(argc==3)for(unsigned i=0;i<2;++i) {
        FILE *f=fopen(argv[i+1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0&&n<=1048576);rewind(f);
        data=malloc((size_t)n);assert(data);assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
        assert(!ceph_firmware_verify((enum ceph_firmware_kind)i,data,(size_t)n));
        data[n-1]^=1;assert(ceph_firmware_verify((enum ceph_firmware_kind)i,data,(size_t)n)<0);free(data);
        printf("Local firmware kind=%u exact digest accepted; one-bit corruption rejected\n",i);
    }
    puts("GPU firmware: SHA256 KAT, exact artifact size/digest and corrupt-image rejection PASS (not PAS auth)");
}
