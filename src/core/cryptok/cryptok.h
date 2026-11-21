#ifndef DYN_CRYPTOK_CRYPTOK_H
#define DYN_CRYPTOK_CRYPTOK_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-common.h"
#include "../dyn-par.h"

void dyn_wipe(void* p, size_t n);

extern const uint8_t dyn_bcrypt_b64_inv[128];

#define BL3_BLOCK 64

extern const uint32_t BL3_IV[8];

typedef struct {
    uint32_t cv[8];
    uint64_t chunk_counter;
    uint8_t buf[BL3_BLOCK];
    uint32_t buf_len, blocks, flags;
} bl3_chunk_t;

#if !defined(BL3_NO_SIMD) && (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define BL3_LANES 4
typedef uint32x4_t bl3_vec;
#define bl3_v_add(a, b) vaddq_u32((a), (b))
#define bl3_v_xor(a, b) veorq_u32((a), (b))
#define bl3_v_set1(x) vdupq_n_u32((uint32_t)(x))
#define bl3_v_loadu(p) vreinterpretq_u32_u8(vld1q_u8((const uint8_t*)(p)))
#define bl3_v_store(p, v) vst1q_u32((uint32_t*)(p), (v))
#define BL3_ROT(x, n) vsriq_n_u32(vshlq_n_u32((x), 32 - (n)), (x), (n))
#define BL3_TRANSPOSE(a0, a1, a2, a3, r0, r1, r2, r3)                                  \
    do {                                                                               \
        uint32x4_t t0_ = vtrn1q_u32((a0), (a1)), t1_ = vtrn2q_u32((a0), (a1));         \
        uint32x4_t t2_ = vtrn1q_u32((a2), (a3)), t3_ = vtrn2q_u32((a2), (a3));         \
        uint64x2_t u0_ = vreinterpretq_u64_u32(t0_), u1_ = vreinterpretq_u64_u32(t1_); \
        uint64x2_t u2_ = vreinterpretq_u64_u32(t2_), u3_ = vreinterpretq_u64_u32(t3_); \
        (r0) = vreinterpretq_u32_u64(vtrn1q_u64(u0_, u2_));                            \
        (r1) = vreinterpretq_u32_u64(vtrn1q_u64(u1_, u3_));                            \
        (r2) = vreinterpretq_u32_u64(vtrn2q_u64(u0_, u2_));                            \
        (r3) = vreinterpretq_u32_u64(vtrn2q_u64(u1_, u3_));                            \
    } while (0)
#elif defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define BL3_LANES 4
typedef __m128i bl3_vec;
#define bl3_v_add(a, b) _mm_add_epi32((a), (b))
#define bl3_v_xor(a, b) _mm_xor_si128((a), (b))
#define bl3_v_set1(x) _mm_set1_epi32((int)(uint32_t)(x))
#define bl3_v_loadu(p) _mm_loadu_si128((const __m128i*)(const void*)(p))
#define bl3_v_store(p, v) _mm_storeu_si128((__m128i*)(void*)(p), (v))
#define BL3_ROT(x, n) _mm_or_si128(_mm_srli_epi32((x), (n)), \
    _mm_slli_epi32((x), 32 - (n)))
#define BL3_TRANSPOSE(a0, a1, a2, a3, r0, r1, r2, r3) \
    do {                                              \
        __m128i t0_ = _mm_unpacklo_epi32((a0), (a1)); \
        __m128i t1_ = _mm_unpackhi_epi32((a0), (a1)); \
        __m128i t2_ = _mm_unpacklo_epi32((a2), (a3)); \
        __m128i t3_ = _mm_unpackhi_epi32((a2), (a3)); \
        (r0) = _mm_unpacklo_epi64(t0_, t2_);          \
        (r1) = _mm_unpackhi_epi64(t0_, t2_);          \
        (r2) = _mm_unpacklo_epi64(t1_, t3_);          \
        (r3) = _mm_unpackhi_epi64(t1_, t3_);          \
    } while (0)
#endif
#endif

typedef struct {
    bl3_chunk_t chunk;
    uint32_t key[8];
    uint32_t stack[54][8];
    size_t stack_len;
    uint32_t flags;
} bl3_hasher_t;

void bl3_init(bl3_hasher_t* h);

void bl3_update(bl3_hasher_t* h, const uint8_t* p, size_t n);

void bl3_final(const bl3_hasher_t* h, uint8_t* out, size_t out_len);

extern const uint64_t BL2B_IV[8];

void bl2b_compress(uint64_t h[8], const uint8_t block[128],
    uint64_t t, int last);

void bl2s_compress(uint32_t h[8], const uint8_t block[64],
    uint64_t t, int last);

#define ARG2_MAX_LANES 16

#define ARG2_MAX_SYNC_MEMORY (1 << 20)

#define ARG2_MAX_TIME 16

#define ARG2_MAX_PWD (1 << 24)

#define ARG2_MAX_SALT (1 << 24)

#define ARG2_MAX_OUT (1 << 20)

#define ARG2_MIN_SALT_LEN 8

#define DYN_BCRYPT_MIN_COST 4

#define DYN_BCRYPT_MAX_COST 31

#define BL3_KEYED 0x10u

#define BL3_DERIVE_CTX 0x20u

#define BL3_DERIVE_MAT 0x40u

int arg_b64u_decode(const char* s, size_t n, uint8_t* out, size_t* outn);

int arg_hash_id(const uint8_t* pwd, size_t pwdlen,
    const uint8_t* salt, size_t saltlen,
    uint32_t t, uint32_t m, uint32_t lanes,
    uint8_t* out, size_t outlen);

char* arg_phc_encode(const uint8_t* salt, size_t saltlen,
    const uint8_t* tag, size_t taglen,
    uint32_t t, uint32_t m, uint32_t p);

int arg_phc_parse(const char* str, const char** psalt, size_t* psaltlen,
    const char** ptag, size_t* ptaglen,
    uint32_t* pt, uint32_t* pm, uint32_t* pp);

int dyn_bcrypt_core(const uint8_t* key, size_t keylen,
    const uint8_t csalt[16], uint32_t logr, char* out);

int dyn_bcrypt_parse(const char* s, size_t len, uint32_t* logr,
    uint8_t csalt[16]);

void dyn_bcrypt_pbkdf_hash(const uint8_t sha2pass[64],
    const uint8_t sha2salt[64], uint8_t out[32]);

void bl2b_hash_key(const uint8_t* p, size_t n, uint8_t* out,
    size_t out_len, const uint8_t* key, size_t keylen);

void bl2s_hash_key(const uint8_t* p, size_t n, uint8_t* out,
    size_t out_len, const uint8_t* key, size_t keylen);

void bl3_ctx_adopt(const char* cs, size_t n, const uint8_t** ctxb,
    size_t* ctxn);

void bl3_init_flags(bl3_hasher_t* h, uint32_t flags);

void bl3_init_keyed(bl3_hasher_t* h, const uint8_t key[32],
    uint32_t flags);


#endif
