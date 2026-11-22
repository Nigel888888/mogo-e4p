/* 仅宿主：使用平台 SHA-256，不伪造摘要。/ Host only: use platform SHA-256, not a fake digest. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX psa_hash_operation_t;
#define CZ_SHA_INIT CC_SHA256_Init
#define CZ_SHA_UPDATE CC_SHA256_Update
#define CZ_SHA_FINAL CC_SHA256_Final
#else
#include <openssl/sha.h>
typedef SHA256_CTX psa_hash_operation_t;
#define CZ_SHA_INIT SHA256_Init
#define CZ_SHA_UPDATE SHA256_Update
#define CZ_SHA_FINAL SHA256_Final
#endif
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
int psa_crypto_init(void);
static inline int psa_hash_setup(psa_hash_operation_t *hash, int alg) { return alg == PSA_ALG_SHA_256 && CZ_SHA_INIT(hash) ? 0 : -1; }
static inline int psa_hash_update(psa_hash_operation_t *hash, const void *data, size_t size) { return CZ_SHA_UPDATE(hash, data, size) ? 0 : -1; }
static inline int psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t cap, size_t *size) {
    *size = 32;
    return cap >= 32 && CZ_SHA_FINAL(out, hash) ? 0 : -1;
}
static inline int psa_hash_abort(psa_hash_operation_t *hash) { (void)hash; return 0; }
