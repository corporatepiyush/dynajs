#ifndef DYN_HASH_H
#define DYN_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_HASH_MAX_BLOCK 128u
#define DYN_HASH_MAX_DIGEST 64u

#define DYN_CRC32_POLY 0xEDB88320u
#define DYN_CRC32C_POLY 0x82F63B78u

typedef enum {
    DYN_HASH_MD5,
    DYN_HASH_SHA1,
    DYN_HASH_SHA224,
    DYN_HASH_SHA256,
    DYN_HASH_SHA384,
    DYN_HASH_SHA512,
    DYN_HASH_ALGO_COUNT
} dyn_hash_algo_id;

typedef union {
    uint32_t w32[8];
    uint64_t w64[8];
} dyn_hash_state_t;

typedef struct {
    const char* name;
    unsigned block_size;
    unsigned digest_size;
    unsigned len_bytes;
    int big_endian_len;
    void (*init)(dyn_hash_state_t* s);
    void (*compress)(dyn_hash_state_t* s, const uint8_t* block);
    void (*extract)(const dyn_hash_state_t* s, uint8_t* out);
} dyn_hash_algo_t;

typedef struct {
    const dyn_hash_algo_t* algo;
    dyn_hash_state_t st;
    uint8_t buffer[DYN_HASH_MAX_BLOCK];
    unsigned buflen;
    uint64_t bytelen;
} dyn_hash_ctx_t;

const dyn_hash_algo_t* dyn_hash_algo_by_name(const char* name);

const dyn_hash_algo_t* dyn_hash_algo_by_id(dyn_hash_algo_id id);

void dyn_hash_init(dyn_hash_ctx_t* c, const dyn_hash_algo_t* a);

void dyn_hash_update(dyn_hash_ctx_t* c, const uint8_t* data, size_t len);

void dyn_hash_final(dyn_hash_ctx_t* c, uint8_t* out);

void dyn_hash_reset(dyn_hash_ctx_t* c);

void dyn_hash_oneshot(const dyn_hash_algo_t* a, const uint8_t* data, size_t len,
    uint8_t* out);

void dyn_md5(const uint8_t* data, size_t len, uint8_t out[16]);
void dyn_sha1(const uint8_t* data, size_t len, uint8_t out[20]);
void dyn_sha256(const uint8_t* data, size_t len, uint8_t out[32]);

void dyn_hmac_key0(const dyn_hash_algo_t* a, const uint8_t* key, size_t keylen,
    uint8_t* k0);

void dyn_hmac_finish(const dyn_hash_algo_t* a, const uint8_t* k0,
    const uint8_t* msg, size_t msglen, uint8_t* out);

void dyn_hmac(const dyn_hash_algo_t* a, const uint8_t* key, size_t keylen,
    const uint8_t* msg, size_t msglen, uint8_t* out);

typedef struct {
    dyn_hash_ctx_t h;
    const dyn_hash_algo_t* algo;
    dyn_hash_state_t istate;
    dyn_hash_state_t ostate;
} dyn_hmac_ctx_t;

void dyn_hmac_init(dyn_hmac_ctx_t* c, const dyn_hash_algo_t* a,
    const uint8_t* key, size_t keylen);
void dyn_hmac_update(dyn_hmac_ctx_t* c, const uint8_t* data, size_t len);
void dyn_hmac_final(dyn_hmac_ctx_t* c, uint8_t* out);
void dyn_hmac_reset(dyn_hmac_ctx_t* c);

int dyn_ct_equal(const uint8_t* a, const uint8_t* b, size_t len);

int dyn_hkdf(const dyn_hash_algo_t* a, const uint8_t* ikm, size_t ikm_len,
    const uint8_t* salt, size_t salt_len,
    const uint8_t* info, size_t info_len,
    uint8_t* out, size_t out_len);

int dyn_pbkdf2(const dyn_hash_algo_t* a, const uint8_t* pw, size_t pw_len,
    const uint8_t* salt, size_t salt_len, uint32_t iters,
    uint8_t* out, size_t out_len);

uint32_t dyn_crc32_poly(const uint8_t* data, size_t len, uint32_t poly);

uint32_t dyn_crc32(const uint8_t* data, size_t len);
uint32_t dyn_crc32c(const uint8_t* data, size_t len);

uint64_t dyn_xxh64(const uint8_t* data, size_t len, uint64_t seed);

uint32_t dyn_xxh32(const uint8_t* data, size_t len, uint32_t seed);

typedef struct {
    uint32_t v1, v2, v3, v4;
    uint64_t total;
    uint8_t mem[16];
    size_t memn;
} dyn_xxh32_ctx_t;

void dyn_xxh32_init(dyn_xxh32_ctx_t* c, uint32_t seed);
void dyn_xxh32_update(dyn_xxh32_ctx_t* c, const uint8_t* data, size_t len);
uint32_t dyn_xxh32_digest(const dyn_xxh32_ctx_t* c);

uint64_t dyn_mix64(uint64_t x);

#ifdef __cplusplus
}
#endif

#endif
