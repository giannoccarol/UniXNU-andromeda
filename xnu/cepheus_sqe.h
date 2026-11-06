/* A6xx SQE file layout per adreno_a6xx.c:_load_firmware and
 * adreno.c:adreno_get_ucode_version. Caller verifies firmware digest first.
 * Strip first DWORD when copying payload; version lives at file DWORD1 with
 * optional low12 bits from DWORD3. This is not ZAP authentication or CP start.
 */
#ifndef CEPHEUS_SQE_H
#define CEPHEUS_SQE_H
#include <stddef.h>
#include <stdint.h>
struct ceph_sqe_image {size_t offset,bytes;uint32_t version,words;};
static inline uint32_t ceph_sqe_le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static inline int ceph_sqe_parse(const uint8_t *file,size_t bytes,struct ceph_sqe_image *out)
{
    if(!out)return -1;
    *out=(struct ceph_sqe_image){0};
    if(!file || bytes<8 || bytes>1024*1024 || (bytes&3))return -1;
    uint32_t v=ceph_sqe_le32(file+4);
    if((v&15)==10) {
        if(bytes<16)return -1;
        v=(v&~0xfffu)|((ceph_sqe_le32(file+12)>>12)&0xfffu);
    }
    if(!v || v==UINT32_MAX)return -1;
    *out=(struct ceph_sqe_image){4,bytes-4,v,(uint32_t)((bytes-4)/4)};return 0;
}
static inline int ceph_sqe_copy(const uint8_t *file,size_t bytes,uint8_t *destination,size_t capacity,
    struct ceph_sqe_image *out)
{
    struct ceph_sqe_image image;
    if(!out || ceph_sqe_parse(file,bytes,&image) || !destination || capacity<image.bytes)return -1;
    uintptr_t f=(uintptr_t)file,d=(uintptr_t)destination;
    if(bytes>UINTPTR_MAX-f || image.bytes>UINTPTR_MAX-d || (f<d+image.bytes && d<f+bytes))return -1;
    for(size_t i=0;i<image.bytes;++i)destination[i]=file[i+4];
    *out=image;return 0;
}
#endif
