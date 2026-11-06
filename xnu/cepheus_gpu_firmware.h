/* Exact local firmware identity checks. Not Qualcomm signature verification:
 * ZAP/PAS authentication is a separate secure-world operation and remains
 * required. Kernel callers must pin an immutable private snapshot throughout
 * hash+copy/upload; validating a mutable shared buffer has a TOCTOU race.
 * No file loading, MMIO, secret keys or proprietary image bytes live here.
 */
#ifndef CEPHEUS_GPU_FIRMWARE_H
#define CEPHEUS_GPU_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
#include <corecrypto/cc.h>
#ifdef __cplusplus
}
#endif
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
enum ceph_firmware_kind { CE_FW_A640_GMU,CE_FW_A630_SQE };
static inline int ceph_firmware_sha256(const uint8_t *data,size_t size,uint8_t digest[32])
{
    if(!data || !size || size>1048576 || !digest)return -1;
#if defined(KERNEL)
    /* SWEET links incremental digest primitives but not the one-shot ccdigest
     * wrapper. SHA256 fixed state/block sizes also avoid a kernel stack VLA. */
    ccdigest_ctx_decl(32,64,ctx);
    ccdigest_init(&ccsha256_ltc_di,ctx);
    ccdigest_update(&ccsha256_ltc_di,ctx,size,data);
    ccdigest_final(&ccsha256_ltc_di,ctx,digest);
    ccdigest_ctx_clear(32,64,ctx);
#elif defined(__APPLE__)
    if(!CC_SHA256(data,(CC_LONG)size,digest))return -1;
#else
    if(!SHA256(data,size,digest))return -1;
#endif
    return 0;
}
static inline int ceph_firmware_verify(enum ceph_firmware_kind kind,const uint8_t *data,size_t size)
{
    static const uint8_t gmu[32]={0x3f,0xf0,0xc0,0x27,0x08,0xbb,0xe7,0x86,0x41,0xdb,0x88,0x7f,0xa6,0x2f,0x3a,0x7f,
        0x93,0x37,0x93,0x4d,0x0c,0x2c,0xe0,0xb9,0x61,0xef,0x7c,0x43,0x17,0x25,0x91,0xd2};
    static const uint8_t sqe[32]={0xa0,0xe1,0xb5,0x83,0xf6,0x20,0xfa,0xbe,0x32,0x72,0x9c,0xe3,0x67,0x95,0x9d,0x19,
        0x60,0x63,0x86,0x63,0x24,0x4d,0x7d,0x0c,0xfc,0x21,0xb9,0xa5,0x21,0x5a,0x01,0x8b};
    const uint8_t *expected;
    if(kind==CE_FW_A640_GMU && size==37680)expected=gmu;
    else if(kind==CE_FW_A630_SQE && size==32304)expected=sqe;
    else return -1;
    uint8_t actual[32];if(ceph_firmware_sha256(data,size,actual))return -1;
    unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=actual[i]^expected[i];
    return diff?-1:0;
}
#endif
