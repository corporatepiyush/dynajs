/*
 * dyn-hash -- message digests, HMAC and CRC-32. PURE C: no JSValue, no
 * JSContext, no dynajs.h. Compiles standalone with `-Isrc/core`.
 *
 * Every primitive is implemented from its published specification -- FIPS 180-4
 * (SHA-1/224/256/384/512), RFC 1321 (MD5), RFC 2104 (HMAC), IEEE 802.3 and
 * Castagnoli (CRC-32/32C) -- and is verified against the standard test vectors
 * in tests/test_crypto.js.
 *
 * THE REUSABLE CONTEXT IS THE POINT. `dyn_hash_ctx_t` is built once from an
 * algorithm descriptor and then absorbs unbounded input; it never allocates, so
 * a caller can keep one per server/thread/loop and reset() between messages
 * instead of paying a fresh setup per call. One-shot helpers are thin wrappers
 * over it, provided for call sites that genuinely hash once.
 *
 * The context is a complete struct (not opaque) so callers can put one on the
 * stack. It holds no pointers into the heap and only one pointer into this
 * library's rodata (`algo`), so a SHALLOW STRUCT COPY IS A VALID SNAPSHOT --
 * which is how a streaming caller reads a digest without destroying resumable
 * state (see dyn_hash_final).
 *
 * Thread safety: all state is in the caller's context; the algorithm table is
 * const rodata initialised at compile time. There is no lazily-built static, so
 * concurrent use of DIFFERENT contexts is safe. A SINGLE context is not
 * reentrant -- one operation at a time.
 */
#ifndef DYN_HASH_H
#define DYN_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest block and digest across the supported algorithms (SHA-512: 128/64).
 * Size any scratch buffer with these, never with a per-algorithm constant. */
#define DYN_HASH_MAX_BLOCK  128u
#define DYN_HASH_MAX_DIGEST 64u

/* Reflected CRC-32 polynomials. */
#define DYN_CRC32_POLY  0xEDB88320u /* IEEE 802.3 */
#define DYN_CRC32C_POLY 0x82F63B78u /* Castagnoli */

typedef enum {
    DYN_HASH_MD5,
    DYN_HASH_SHA1,
    DYN_HASH_SHA224,
    DYN_HASH_SHA256,
    DYN_HASH_SHA384,
    DYN_HASH_SHA512,
    DYN_HASH_ALGO_COUNT
} dyn_hash_algo_id;

/* Working state of any supported hash: MD5/SHA-1/SHA-224/SHA-256 use w32[],
 * SHA-384/SHA-512 use w64[]. The generic driver never touches it directly --
 * each algorithm's init/compress/extract casts to its own word type. */
typedef union {
    uint32_t w32[8];
    uint64_t w64[8];
} dyn_hash_state_t;

/* Immutable algorithm descriptor. Instances live in this library's rodata; a
 * pointer to one is stable for the life of the process. */
typedef struct {
    const char *name;
    unsigned block_size;   /* 64 (md5/sha1/sha224/sha256) or 128 (sha384/sha512) */
    unsigned digest_size;  /* output bytes */
    unsigned len_bytes;    /* trailing length field: 8 or 16 bytes */
    int big_endian_len;    /* 1 for SHA (big-endian length), 0 for MD5 (little) */
    void (*init)(dyn_hash_state_t *s);
    void (*compress)(dyn_hash_state_t *s, const uint8_t *block);
    void (*extract)(const dyn_hash_state_t *s, uint8_t *out);
} dyn_hash_algo_t;

/* Streaming context: descriptor + partial-block buffer + total length. */
typedef struct {
    const dyn_hash_algo_t *algo;
    dyn_hash_state_t st;
    uint8_t buffer[DYN_HASH_MAX_BLOCK];
    unsigned buflen;
    uint64_t bytelen; /* total message bytes absorbed so far */
} dyn_hash_ctx_t;

/* ---- algorithm lookup ---- */

/* Descriptor for `name` ("md5" "sha1" "sha224" "sha256" "sha384" "sha512"),
 * or NULL if unknown. Case-sensitive. */
const dyn_hash_algo_t *dyn_hash_algo_by_name(const char *name);

/* Descriptor for `id`, or NULL if out of range. */
const dyn_hash_algo_t *dyn_hash_algo_by_id(dyn_hash_algo_id id);

/* ---- streaming ---- */

/* Begin a message. `a` must be non-NULL and outlive `c`. Allocates nothing. */
void dyn_hash_init(dyn_hash_ctx_t *c, const dyn_hash_algo_t *a);

/* Absorb `len` bytes. Any number of calls; `len` 0 is a no-op. */
void dyn_hash_update(dyn_hash_ctx_t *c, const uint8_t *data, size_t len);

/* Merkle-Damgard finalization: writes exactly `c->algo->digest_size` bytes to
 * `out` and CONSUMES `c` (its state is no longer resumable). A caller that must
 * keep absorbing passes a shallow copy:
 *     dyn_hash_ctx_t snapshot = *c;  dyn_hash_final(&snapshot, digest);
 * `out` must have room for digest_size bytes; DYN_HASH_MAX_DIGEST always fits. */
void dyn_hash_final(dyn_hash_ctx_t *c, uint8_t *out);

/* Return `c` to its initial state for the same algorithm, ready for a new
 * message. This is the reuse path -- it allocates nothing and cannot fail. */
void dyn_hash_reset(dyn_hash_ctx_t *c);

/* ---- one-shot ---- */

/* Whole-message digest in one call. Writes `a->digest_size` bytes to `out`. */
void dyn_hash_oneshot(const dyn_hash_algo_t *a, const uint8_t *data, size_t len,
                      uint8_t *out);

/* Named shorthands for the algorithms other modules need directly. */
void dyn_md5(const uint8_t *data, size_t len, uint8_t out[16]);
void dyn_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void dyn_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* ---- HMAC (RFC 2104) ---- */

/* Derive the block-sized key K0 (zero-padded, or H(key) when the key exceeds
 * the block size). `k0` must be DYN_HASH_MAX_BLOCK bytes and PRE-ZEROED by the
 * caller -- the short-key path writes only keylen bytes and the hashed-key path
 * only digest_size, relying on the rest already being zero.
 *
 * Split from dyn_hmac_finish so a caller can consume a borrowed key pointer
 * immediately and release it before coercing a later (large) message argument. */
void dyn_hmac_key0(const dyn_hash_algo_t *a, const uint8_t *key, size_t keylen,
                   uint8_t *k0);

/* H((K0 ^ opad) || H((K0 ^ ipad) || msg)) with K0 already derived. */
void dyn_hmac_finish(const dyn_hash_algo_t *a, const uint8_t *k0,
                     const uint8_t *msg, size_t msglen, uint8_t *out);

/* Key derivation + finish in one call, for callers holding both at once. */
void dyn_hmac(const dyn_hash_algo_t *a, const uint8_t *key, size_t keylen,
              const uint8_t *msg, size_t msglen, uint8_t *out);

/* ---- streaming HMAC ----
 *
 * The keyed twin of dyn_hash_ctx_t, and the reason `class Hmac` can exist: the
 * key schedule (K0 and the ipad block) is derived once at init and the context
 * is reset by final(), so one object signs many messages without re-deriving
 * anything. It holds the key, so it is not shareable. */
typedef struct {
    dyn_hash_ctx_t h;
    const dyn_hash_algo_t *algo;
    /* The key schedule, already COMPRESSED. K0 and the two pad blocks are
     * derived once on the stack in init() and never stored: what a later
     * message needs is not the pad bytes but the chaining state they produce,
     * and that is 64 bytes instead of 128. Priming h from a saved state is a
     * struct assignment where it used to be a full compression. */
    dyn_hash_state_t istate;   /* after absorbing K0 ^ ipad */
    dyn_hash_state_t ostate;   /* after absorbing K0 ^ opad */
} dyn_hmac_ctx_t;

void dyn_hmac_init(dyn_hmac_ctx_t *c, const dyn_hash_algo_t *a,
                   const uint8_t *key, size_t keylen);
void dyn_hmac_update(dyn_hmac_ctx_t *c, const uint8_t *data, size_t len);
/* Writes digest_size bytes and RESETS the context for the next message. */
void dyn_hmac_final(dyn_hmac_ctx_t *c, uint8_t *out);
void dyn_hmac_reset(dyn_hmac_ctx_t *c);

/* ---- key derivation and constant-time comparison ----
 *
 * These are the operations that depend on a SECRET, which is the line
 * dyna:hash and dyna:crypto are split on: everything above reduces bytes to a
 * tag and leaks nothing by running fast, everything here must not leak by
 * running fast.
 *
 * dyn_ct_equal is the reason this is a library function rather than a memcmp at
 * each call site: it must accumulate over the WHOLE input and return the
 * accumulated difference, so its running time depends only on the length. A
 * caller that writes the obvious loop with an early exit publishes the position
 * of the first differing byte. */
int dyn_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* HKDF (RFC 5869): extract-then-expand. `out_len` is capped at 255 * digest
 * size by the construction; a larger request returns -1. */
int dyn_hkdf(const dyn_hash_algo_t *a, const uint8_t *ikm, size_t ikm_len,
             const uint8_t *salt, size_t salt_len,
             const uint8_t *info, size_t info_len,
             uint8_t *out, size_t out_len);

/* PBKDF2-HMAC (RFC 8018). `iters` must be >= 1. Returns 0, or -1 on a bad
 * parameter. Deliberately the plain construction: it is what an existing
 * password database is stored with, not what a new one should use. */
int dyn_pbkdf2(const dyn_hash_algo_t *a, const uint8_t *pw, size_t pw_len,
               const uint8_t *salt, size_t salt_len, uint32_t iters,
               uint8_t *out, size_t out_len);

/* ---- CRC-32 ---- */

/* Reflected bitwise CRC with an explicit polynomial. No lookup tables, so
 * there is no lazily-initialised static to race across threads. */
uint32_t dyn_crc32_poly(const uint8_t *data, size_t len, uint32_t poly);

uint32_t dyn_crc32(const uint8_t *data, size_t len);  /* IEEE 802.3 */
uint32_t dyn_crc32c(const uint8_t *data, size_t len); /* Castagnoli */

/* ---- XXH64 -- non-cryptographic 64-bit hash (Collet, xxHash spec v0.7.3) ----
 *
 * NOT a security primitive: it is seed-recoverable and trivially collidable by
 * an adversary. It is here because three consumers need a fast, well-mixed
 * 64-bit value with a PUBLISHED byte-exact definition: the LZ4 frame content
 * checksum, and the register/probe indices of HyperLogLog and CountMinSketch,
 * whose serialised form is only portable if the hash is.
 *
 * Byte order is little-endian by definition of the format, independent of the
 * host, so a sketch written on one machine reads back on another. */
uint64_t dyn_xxh64(const uint8_t *data, size_t len, uint64_t seed);

/* XXH32, which is a DIFFERENT function, not a truncation of the above. It is
 * here because the LZ4 frame format names it for the frame-descriptor checksum
 * byte and the optional content checksum, so interop with the `lz4` tool
 * requires exactly this. */
uint32_t dyn_xxh32(const uint8_t *data, size_t len, uint32_t seed);

/* Mix a 64-bit value into a well-distributed 64-bit value (SplitMix64's
 * finaliser). Used where the input is already an integer and running it through
 * a byte hash would be waste. */
uint64_t dyn_mix64(uint64_t x);

#ifdef __cplusplus
}
#endif

#endif /* DYN_HASH_H */
