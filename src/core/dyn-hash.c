/*
 * dyn-hash -- message digests, HMAC and CRC-32. PURE C: no JSValue, no
 * JSContext, no dynajs.h. See dyn-hash.h for the contract.
 *
 * The algorithm cores below (MD5, SHA-1, SHA-224/256, SHA-384/512, HMAC,
 * CRC-32) are the implementations that were verified against FIPS 180-4,
 * RFC 1321/2104/4231 and IEEE 802.3 vectors by tests/test_crypto.js; they are
 * moved here unchanged so that every module needing a digest -- dyna:crypto,
 * dyna:uuid (v3/v5), dyna:http (the WebSocket accept key), dyna:compress (the
 * gzip CRC trailer) -- shares one audited copy instead of carrying its own.
 */
#include "dyn-hash.h"

#if defined(__x86_64__)
#include <stdatomic.h>   /* crc32c_hw_ok: the SSE4.2 probe is a shared static */
#endif

#include <string.h>

/* ==================================================================== *
 *  fixed-width byte load/store (host-endianness independent)            *
 * ==================================================================== */

static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
/* Used only by the scalar SHA-256 compression function, which is not compiled
 * when the ISA supplies the rounds. */
static inline uint32_t rotr32(uint32_t x, int n)
    __attribute__((unused));
static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

/* memcpy of a fixed width is the portable spelling of an unaligned load: the
 * compiler folds it to one `mov`, and the byte order fixup to one `bswap`.
 * Spelling it out byte by byte, as this did, hides that from the optimiser --
 * it cannot know the eight loads are contiguous, so it emits all eight. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define DYN_BE32(v) __builtin_bswap32(v)
#  define DYN_BE64(v) __builtin_bswap64(v)
#  define DYN_LE32(v) (v)
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define DYN_BE32(v) (v)
#  define DYN_BE64(v) (v)
#  define DYN_LE32(v) __builtin_bswap32(v)
#else
#  define DYN_PORTABLE_BYTES 1
#endif

#ifndef DYN_PORTABLE_BYTES
static inline uint32_t load_be32(const uint8_t *p)
{ uint32_t v; memcpy(&v, p, 4); return DYN_BE32(v); }
static inline uint32_t load_le32(const uint8_t *p)
{ uint32_t v; memcpy(&v, p, 4); return DYN_LE32(v); }
static inline uint64_t load_be64(const uint8_t *p)
{ uint64_t v; memcpy(&v, p, 8); return DYN_BE64(v); }
#else
static inline uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
#endif
static inline void store_be32(uint8_t *p, uint32_t v)
{
#ifndef DYN_PORTABLE_BYTES
    uint32_t t = DYN_BE32(v);        /* the 64-bit store was already fixed */
    memcpy(p, &t, 4);
#else
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
#endif
}
static inline void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void store_be64(uint8_t *p, uint64_t v)
{
#ifndef DYN_PORTABLE_BYTES
    uint64_t t = DYN_BE64(v);
    memcpy(p, &t, 8);
#else
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
#endif
}

/* ==================================================================== *
 *  MD5 (RFC 1321)                                                       *
 * ==================================================================== */

static const uint32_t md5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_init(dyn_hash_state_t *s)
{
    s->w32[0] = 0x67452301; s->w32[1] = 0xefcdab89;
    s->w32[2] = 0x98badcfe; s->w32[3] = 0x10325476;
}

static void md5_compress(dyn_hash_state_t *s, const uint8_t *block)
{
    uint32_t m[16], a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = load_le32(block + i * 4);
    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);        g = (7 * i) & 15; }
        f += a + md5_K[i] + m[g];
        a = d; d = c; c = b;
        b += rotl32(f, md5_s[i]);
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d;
}

static void md5_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    store_le32(out,      s->w32[0]);
    store_le32(out + 4,  s->w32[1]);
    store_le32(out + 8,  s->w32[2]);
    store_le32(out + 12, s->w32[3]);
}

/* ==================================================================== *
 *  SHA-1 (FIPS 180-4 sec 6.1)                                           *
 * ==================================================================== */

static void sha1_init(dyn_hash_state_t *s)
{
    s->w32[0] = 0x67452301; s->w32[1] = 0xEFCDAB89; s->w32[2] = 0x98BADCFE;
    s->w32[3] = 0x10325476; s->w32[4] = 0xC3D2E1F0;
}

static void sha1_compress(dyn_hash_state_t *s, const uint8_t *block)
{
    uint32_t w[80];
    uint32_t a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3], e = s->w32[4];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d; s->w32[4] += e;
}

static void sha1_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 5; i++)
        store_be32(out + i * 4, s->w32[i]);
}

/* ==================================================================== *
 *  SHA-224 / SHA-256 (FIPS 180-4 sec 6.2)                               *
 * ==================================================================== */

static const uint32_t sha256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_init(dyn_hash_state_t *s)
{
    s->w32[0] = 0x6a09e667; s->w32[1] = 0xbb67ae85; s->w32[2] = 0x3c6ef372; s->w32[3] = 0xa54ff53a;
    s->w32[4] = 0x510e527f; s->w32[5] = 0x9b05688c; s->w32[6] = 0x1f83d9ab; s->w32[7] = 0x5be0cd19;
}

static void sha224_init(dyn_hash_state_t *s)
{
    s->w32[0] = 0xc1059ed8; s->w32[1] = 0x367cd507; s->w32[2] = 0x3070dd17; s->w32[3] = 0xf70e5939;
    s->w32[4] = 0xffc00b31; s->w32[5] = 0x68581511; s->w32[6] = 0x64f98fa7; s->w32[7] = 0xbefa4fa4;
}

/* One round, with the operand rotation done by the CALLER's argument order
 * instead of by eight register moves per round. `ch` and `maj` are the standard
 * two-operation forms; they are what the spec's expressions reduce to. */
#define DYN_SHA256_R(a,b,c,d,e,f,g,h,k,w) do {                              \
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);             \
    uint32_t ch = (g) ^ ((e) & ((f) ^ (g)));                                \
    uint32_t t1 = (h) + S1 + ch + (k) + (w);                                \
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);             \
    uint32_t mj = ((a) & (b)) | ((c) & ((a) | (b)));                        \
    (d) += t1; (h) = t1 + S0 + mj;                                          \
} while (0)

/* Hardware SHA-256 (ARMv8 SHA2 extension).
 *
 * The compression function is ~15 cycles/byte in C and ~2 with the
 * instructions; the whole of a small digest is one block, so this is the
 * difference between 300 ns and 40 ns for hashing anything short.
 *
 * `sha256h`/`sha256h2` advance the state by four rounds, and
 * `sha256su0`/`sha256su1` produce the next four message-schedule words, so the
 * 64 rounds become 16 groups with no `w[64]` array and no scalar schedule.
 *
 * The instruction takes the state as two vectors {a,b,c,d} and {e,f,g,h} in
 * HOST order, while the state is stored big-endian on the wire -- the message
 * words are byte-reversed, the state words are not.
 */
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2) && !defined(DYN_SHA256_NO_HW)
#define DYN_SHA256_HW 1
#include <arm_neon.h>

/* Four rounds. `m0` supplies the message words for these rounds and is then
 * advanced to the words needed sixteen rounds later; the four vectors rotate
 * so the schedule stays entirely in registers. The K addend uses m0 BEFORE
 * su0 updates it -- that ordering is the whole trick and is easy to get
 * backwards, which is what the differential against the scalar path catches. */
#define DYN_SHA256_QUAD(m0, m1, m2, m3, kidx, sched)                         \
    do {                                                                     \
        uint32x4_t _t = vaddq_u32((m0), vld1q_u32(&sha256_K[(kidx)]));       \
        uint32x4_t _s = abcd;                                                \
        if (sched) (m0) = vsha256su0q_u32((m0), (m1));                       \
        abcd = vsha256hq_u32(abcd, efgh, _t);                                \
        efgh = vsha256h2q_u32(efgh, _s, _t);                                 \
        if (sched) (m0) = vsha256su1q_u32((m0), (m2), (m3));                 \
    } while (0)

static void sha256_compress_hw(dyn_hash_state_t *s, const uint8_t *block)
{
    uint32x4_t abcd = vld1q_u32(&s->w32[0]);
    uint32x4_t efgh = vld1q_u32(&s->w32[4]);
    const uint32x4_t abcd0 = abcd, efgh0 = efgh;
    uint32x4_t w0, w1, w2, w3;
    int i;

    /* Big-endian on the wire: reverse within each 4-byte lane. The STATE is
     * host-order and must not be reversed. */
    w0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block +  0)));
    w1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
    w2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
    w3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));

    for (i = 0; i < 64; i += 16) {
        const int sched = (i < 48);   /* last 16 rounds need no new words */
        DYN_SHA256_QUAD(w0, w1, w2, w3, i +  0, sched);
        DYN_SHA256_QUAD(w1, w2, w3, w0, i +  4, sched);
        DYN_SHA256_QUAD(w2, w3, w0, w1, i +  8, sched);
        DYN_SHA256_QUAD(w3, w0, w1, w2, i + 12, sched);
    }

    vst1q_u32(&s->w32[0], vaddq_u32(abcd0, abcd));
    vst1q_u32(&s->w32[4], vaddq_u32(efgh0, efgh));
}
#undef DYN_SHA256_QUAD
#endif

#ifndef DYN_SHA256_HW   /* the scalar path IS the implementation when there is no ISA one */
static void sha256_compress_sw(dyn_hash_state_t *s, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a = s->w32[0], b = s->w32[1], c = s->w32[2], d = s->w32[3];
    uint32_t e = s->w32[4], f = s->w32[5], g = s->w32[6], h = s->w32[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 64; i += 8) {
        DYN_SHA256_R(a,b,c,d,e,f,g,h, sha256_K[i+0], w[i+0]);
        DYN_SHA256_R(h,a,b,c,d,e,f,g, sha256_K[i+1], w[i+1]);
        DYN_SHA256_R(g,h,a,b,c,d,e,f, sha256_K[i+2], w[i+2]);
        DYN_SHA256_R(f,g,h,a,b,c,d,e, sha256_K[i+3], w[i+3]);
        DYN_SHA256_R(e,f,g,h,a,b,c,d, sha256_K[i+4], w[i+4]);
        DYN_SHA256_R(d,e,f,g,h,a,b,c, sha256_K[i+5], w[i+5]);
        DYN_SHA256_R(c,d,e,f,g,h,a,b, sha256_K[i+6], w[i+6]);
        DYN_SHA256_R(b,c,d,e,f,g,h,a, sha256_K[i+7], w[i+7]);
    }
    s->w32[0] += a; s->w32[1] += b; s->w32[2] += c; s->w32[3] += d;
    s->w32[4] += e; s->w32[5] += f; s->w32[6] += g; s->w32[7] += h;
}
#endif

/* __ARM_FEATURE_SHA2 is a compile-time guarantee from the target, so there is
 * nothing to probe; on a target without it the scalar path is the only one
 * compiled. Build with -DDYN_SHA256_NO_HW=1 to force the scalar path, which is
 * what tests/test_sha256_hw.c diffs against. */
static void sha256_compress(dyn_hash_state_t *s, const uint8_t *block)
{
#ifdef DYN_SHA256_HW
    sha256_compress_hw(s, block);
#else
    sha256_compress_sw(s, block);
#endif
}

static void sha256_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++)
        store_be32(out + i * 4, s->w32[i]);
}

static void sha224_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 7; i++) /* SHA-224 = SHA-256 truncated to 224 bits */
        store_be32(out + i * 4, s->w32[i]);
}

/* ==================================================================== *
 *  SHA-384 / SHA-512 (FIPS 180-4 sec 6.4)                               *
 * ==================================================================== */

static const uint64_t sha512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void sha512_init(dyn_hash_state_t *s)
{
    s->w64[0] = 0x6a09e667f3bcc908ULL; s->w64[1] = 0xbb67ae8584caa73bULL;
    s->w64[2] = 0x3c6ef372fe94f82bULL; s->w64[3] = 0xa54ff53a5f1d36f1ULL;
    s->w64[4] = 0x510e527fade682d1ULL; s->w64[5] = 0x9b05688c2b3e6c1fULL;
    s->w64[6] = 0x1f83d9abfb41bd6bULL; s->w64[7] = 0x5be0cd19137e2179ULL;
}

static void sha384_init(dyn_hash_state_t *s)
{
    s->w64[0] = 0xcbbb9d5dc1059ed8ULL; s->w64[1] = 0x629a292a367cd507ULL;
    s->w64[2] = 0x9159015a3070dd17ULL; s->w64[3] = 0x152fecd8f70e5939ULL;
    s->w64[4] = 0x67332667ffc00b31ULL; s->w64[5] = 0x8eb44a8768581511ULL;
    s->w64[6] = 0xdb0c2e0d64f98fa7ULL; s->w64[7] = 0x47b5481dbefa4fa4ULL;
}

#define DYN_SHA512_R(a,b,c,d,e,f,g,h,k,w) do {                              \
    uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);            \
    uint64_t ch = (g) ^ ((e) & ((f) ^ (g)));                                \
    uint64_t t1 = (h) + S1 + ch + (k) + (w);                                \
    uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);            \
    uint64_t mj = ((a) & (b)) | ((c) & ((a) | (b)));                        \
    (d) += t1; (h) = t1 + S0 + mj;                                          \
} while (0)

static void sha512_compress(dyn_hash_state_t *s, const uint8_t *block)
{
    uint64_t w[80];
    uint64_t a = s->w64[0], b = s->w64[1], c = s->w64[2], d = s->w64[3];
    uint64_t e = s->w64[4], f = s->w64[5], g = s->w64[6], h = s->w64[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = load_be64(block + i * 8);
    for (i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 80; i += 8) {
        DYN_SHA512_R(a,b,c,d,e,f,g,h, sha512_K[i+0], w[i+0]);
        DYN_SHA512_R(h,a,b,c,d,e,f,g, sha512_K[i+1], w[i+1]);
        DYN_SHA512_R(g,h,a,b,c,d,e,f, sha512_K[i+2], w[i+2]);
        DYN_SHA512_R(f,g,h,a,b,c,d,e, sha512_K[i+3], w[i+3]);
        DYN_SHA512_R(e,f,g,h,a,b,c,d, sha512_K[i+4], w[i+4]);
        DYN_SHA512_R(d,e,f,g,h,a,b,c, sha512_K[i+5], w[i+5]);
        DYN_SHA512_R(c,d,e,f,g,h,a,b, sha512_K[i+6], w[i+6]);
        DYN_SHA512_R(b,c,d,e,f,g,h,a, sha512_K[i+7], w[i+7]);
    }
    s->w64[0] += a; s->w64[1] += b; s->w64[2] += c; s->w64[3] += d;
    s->w64[4] += e; s->w64[5] += f; s->w64[6] += g; s->w64[7] += h;
}

static void sha512_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 8; i++)
        store_be64(out + i * 8, s->w64[i]);
}

static void sha384_extract(const dyn_hash_state_t *s, uint8_t *out)
{
    int i;
    for (i = 0; i < 6; i++) /* SHA-384 = SHA-512 truncated to 384 bits */
        store_be64(out + i * 8, s->w64[i]);
}

/* ==================================================================== *
 *  generic hash driver (buffered absorb + Merkle-Damgard padding)       *
 * ==================================================================== */

static const dyn_hash_algo_t dyn_hash_algos[DYN_HASH_ALGO_COUNT] = {
    [DYN_HASH_MD5]    = { "md5",    64, 16,  8, 0, md5_init,    md5_compress,    md5_extract },
    [DYN_HASH_SHA1]   = { "sha1",   64, 20,  8, 1, sha1_init,   sha1_compress,   sha1_extract },
    [DYN_HASH_SHA224] = { "sha224", 64, 28,  8, 1, sha224_init, sha256_compress, sha224_extract },
    [DYN_HASH_SHA256] = { "sha256", 64, 32,  8, 1, sha256_init, sha256_compress, sha256_extract },
    [DYN_HASH_SHA384] = { "sha384", 128, 48, 16, 1, sha384_init, sha512_compress, sha384_extract },
    [DYN_HASH_SHA512] = { "sha512", 128, 64, 16, 1, sha512_init, sha512_compress, sha512_extract },
};

void dyn_hash_init(dyn_hash_ctx_t *c, const dyn_hash_algo_t *a)
{
    c->algo = a;
    c->buflen = 0;
    c->bytelen = 0;
    a->init(&c->st);
}

void dyn_hash_update(dyn_hash_ctx_t *c, const uint8_t *data, size_t len)
{
    unsigned block = c->algo->block_size;
    c->bytelen += len;
    if (c->buflen) { /* top up a partial block first */
        unsigned need = block - c->buflen;
        unsigned take = len < need ? (unsigned)len : need;
        memcpy(c->buffer + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == block) {
            c->algo->compress(&c->st, c->buffer);
            c->buflen = 0;
        }
    }
    while (len >= block) {
        c->algo->compress(&c->st, data);
        data += block;
        len -= block;
    }
    if (len) {
        memcpy(c->buffer, data, len);
        c->buflen = (unsigned)len;
    }
}

/* Merkle-Damgard finalization: append 0x80, zero-pad, append the message bit
 * length in the algorithm's width/endianness, compress the final block(s), and
 * extract the digest. Operates on `c` in place (callers that must stay usable
 * pass a copy). */
void dyn_hash_final(dyn_hash_ctx_t *c, uint8_t *out)
{
    const dyn_hash_algo_t *a = c->algo;
    unsigned block = a->block_size, lenb = a->len_bytes;
    uint64_t bits = c->bytelen << 3;
    uint64_t bits_hi = c->bytelen >> 61; /* high 64 bits of the 128-bit bit length */
    uint8_t *buf = c->buffer;
    unsigned n = c->buflen;
    int i;

    buf[n++] = 0x80;
    if (n > block - lenb) { /* not enough room for the length field: flush */
        while (n < block)
            buf[n++] = 0;
        a->compress(&c->st, buf);
        n = 0;
    }
    while (n < block - lenb)
        buf[n++] = 0;
    if (a->big_endian_len) {
        if (lenb == 16) {
            store_be64(buf + n, bits_hi);
            store_be64(buf + n + 8, bits);
        } else {
            store_be64(buf + n, bits);
        }
    } else { /* MD5: 64-bit little-endian length */
        for (i = 0; i < 8; i++)
            buf[n + i] = (uint8_t)(bits >> (8 * i));
    }
    n += lenb;
    a->compress(&c->st, buf);
    a->extract(&c->st, out);
}

const dyn_hash_algo_t *dyn_hash_algo_by_name(const char *name)
{
    int i;
    for (i = 0; i < DYN_HASH_ALGO_COUNT; i++)
        if (strcmp(name, dyn_hash_algos[i].name) == 0)
            return &dyn_hash_algos[i];
    return NULL;
}

/* ==================================================================== *
 *  HMAC (RFC 2104)                                                      *
 * ==================================================================== */

/* Derive the block-sized key K0 (zero-padded, or H(key) when key exceeds the
 * block size). `k0` must be DYN_HASH_MAX_BLOCK bytes and pre-zeroed by the caller. */
void dyn_hmac_key0(const dyn_hash_algo_t *a, const uint8_t *key,
                          size_t keylen, uint8_t *k0)
{
    if (keylen > a->block_size) {
        dyn_hash_ctx_t c;
        dyn_hash_init(&c, a);
        dyn_hash_update(&c, key, keylen);
        dyn_hash_final(&c, k0); /* writes digest_size bytes; the rest stays 0 */
    } else {
        memcpy(k0, key, keylen);
    }
}

/* HMAC = H((K0 ^ opad) || H((K0 ^ ipad) || msg)), K0 already derived. */
void dyn_hmac_finish(const dyn_hash_algo_t *a, const uint8_t *k0,
                            const uint8_t *msg, size_t msglen, uint8_t *out)
{
    uint8_t ipad[DYN_HASH_MAX_BLOCK], opad[DYN_HASH_MAX_BLOCK], inner[DYN_HASH_MAX_DIGEST];
    dyn_hash_ctx_t c;
    unsigned bs = a->block_size, i;

    for (i = 0; i < bs; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, ipad, bs);
    dyn_hash_update(&c, msg, msglen);
    dyn_hash_final(&c, inner);

    dyn_hash_init(&c, a);
    dyn_hash_update(&c, opad, bs);
    dyn_hash_update(&c, inner, a->digest_size);
    dyn_hash_final(&c, out);
}

/* ==================================================================== *
 *  CRC-32 (reflected, bitwise -- no lookup tables / no static state)    *
 * ==================================================================== */

/* Bit-serial CRC, used for an arbitrary runtime polynomial. Kept because
 * dyn_crc32_poly is public API, not because it is the fast path. */
static uint32_t dyn_crc32_bitwise(const uint8_t *data, size_t len, uint32_t poly)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (poly & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Byte-at-a-time tables for the two polynomials that actually get used.
 *
 * These are `static const` -- fully materialised at compile time, so there is
 * NO initialisation and therefore none of the lazily-built-static thread race
 * that motivated the original table-free loop. That constraint was about *when*
 * a table gets built, not about having one.
 *
 * They also sidestep a real trap. With the polynomial constant-folded into the
 * bit-serial loop, clang -O2 on arm64 SLP-vectorises the eight unrolled bit
 * steps (dup.4s / cmeq.4s / bic.16b plus fmov moves back to GPRs) and the
 * result is 1.33x SLOWER than scalar. It fired for the IEEE constant and not
 * for Castagnoli, so one entry point regressed 399 -> 532 us over 64 KiB while
 * its twin improved to 381. `#pragma clang loop vectorize(disable)` does not
 * help: this is the SLP vectoriser, not the loop vectoriser. A table removes
 * the inner loop the vectoriser was chewing on, and is several times faster
 * than either version.
 *
 * Check values (RFC 3720 / IEEE 802.3, input "123456789"):
 *   IEEE       0xCBF43926
 *   Castagnoli 0xE3069283
 */
/* Slice-by-8 CRC tables, derived from the polynomials by tools/gen-crc.py.
 * Row j holds the contribution of a byte j positions back in the window, so
 * eight bytes are consumed per iteration instead of one. static const: no
 * initialiser, no lazily-built static, nothing to race. Kept in this file
 * rather than a header because nothing else uses them and src/core headers
 * must each stand alone. */
static const uint32_t crc32_ieee_s8[8][256] = {
{
    0x00000000u,0x77073096u,0xEE0E612Cu,0x990951BAu,0x076DC419u,0x706AF48Fu,
    0xE963A535u,0x9E6495A3u,0x0EDB8832u,0x79DCB8A4u,0xE0D5E91Eu,0x97D2D988u,
    0x09B64C2Bu,0x7EB17CBDu,0xE7B82D07u,0x90BF1D91u,0x1DB71064u,0x6AB020F2u,
    0xF3B97148u,0x84BE41DEu,0x1ADAD47Du,0x6DDDE4EBu,0xF4D4B551u,0x83D385C7u,
    0x136C9856u,0x646BA8C0u,0xFD62F97Au,0x8A65C9ECu,0x14015C4Fu,0x63066CD9u,
    0xFA0F3D63u,0x8D080DF5u,0x3B6E20C8u,0x4C69105Eu,0xD56041E4u,0xA2677172u,
    0x3C03E4D1u,0x4B04D447u,0xD20D85FDu,0xA50AB56Bu,0x35B5A8FAu,0x42B2986Cu,
    0xDBBBC9D6u,0xACBCF940u,0x32D86CE3u,0x45DF5C75u,0xDCD60DCFu,0xABD13D59u,
    0x26D930ACu,0x51DE003Au,0xC8D75180u,0xBFD06116u,0x21B4F4B5u,0x56B3C423u,
    0xCFBA9599u,0xB8BDA50Fu,0x2802B89Eu,0x5F058808u,0xC60CD9B2u,0xB10BE924u,
    0x2F6F7C87u,0x58684C11u,0xC1611DABu,0xB6662D3Du,0x76DC4190u,0x01DB7106u,
    0x98D220BCu,0xEFD5102Au,0x71B18589u,0x06B6B51Fu,0x9FBFE4A5u,0xE8B8D433u,
    0x7807C9A2u,0x0F00F934u,0x9609A88Eu,0xE10E9818u,0x7F6A0DBBu,0x086D3D2Du,
    0x91646C97u,0xE6635C01u,0x6B6B51F4u,0x1C6C6162u,0x856530D8u,0xF262004Eu,
    0x6C0695EDu,0x1B01A57Bu,0x8208F4C1u,0xF50FC457u,0x65B0D9C6u,0x12B7E950u,
    0x8BBEB8EAu,0xFCB9887Cu,0x62DD1DDFu,0x15DA2D49u,0x8CD37CF3u,0xFBD44C65u,
    0x4DB26158u,0x3AB551CEu,0xA3BC0074u,0xD4BB30E2u,0x4ADFA541u,0x3DD895D7u,
    0xA4D1C46Du,0xD3D6F4FBu,0x4369E96Au,0x346ED9FCu,0xAD678846u,0xDA60B8D0u,
    0x44042D73u,0x33031DE5u,0xAA0A4C5Fu,0xDD0D7CC9u,0x5005713Cu,0x270241AAu,
    0xBE0B1010u,0xC90C2086u,0x5768B525u,0x206F85B3u,0xB966D409u,0xCE61E49Fu,
    0x5EDEF90Eu,0x29D9C998u,0xB0D09822u,0xC7D7A8B4u,0x59B33D17u,0x2EB40D81u,
    0xB7BD5C3Bu,0xC0BA6CADu,0xEDB88320u,0x9ABFB3B6u,0x03B6E20Cu,0x74B1D29Au,
    0xEAD54739u,0x9DD277AFu,0x04DB2615u,0x73DC1683u,0xE3630B12u,0x94643B84u,
    0x0D6D6A3Eu,0x7A6A5AA8u,0xE40ECF0Bu,0x9309FF9Du,0x0A00AE27u,0x7D079EB1u,
    0xF00F9344u,0x8708A3D2u,0x1E01F268u,0x6906C2FEu,0xF762575Du,0x806567CBu,
    0x196C3671u,0x6E6B06E7u,0xFED41B76u,0x89D32BE0u,0x10DA7A5Au,0x67DD4ACCu,
    0xF9B9DF6Fu,0x8EBEEFF9u,0x17B7BE43u,0x60B08ED5u,0xD6D6A3E8u,0xA1D1937Eu,
    0x38D8C2C4u,0x4FDFF252u,0xD1BB67F1u,0xA6BC5767u,0x3FB506DDu,0x48B2364Bu,
    0xD80D2BDAu,0xAF0A1B4Cu,0x36034AF6u,0x41047A60u,0xDF60EFC3u,0xA867DF55u,
    0x316E8EEFu,0x4669BE79u,0xCB61B38Cu,0xBC66831Au,0x256FD2A0u,0x5268E236u,
    0xCC0C7795u,0xBB0B4703u,0x220216B9u,0x5505262Fu,0xC5BA3BBEu,0xB2BD0B28u,
    0x2BB45A92u,0x5CB36A04u,0xC2D7FFA7u,0xB5D0CF31u,0x2CD99E8Bu,0x5BDEAE1Du,
    0x9B64C2B0u,0xEC63F226u,0x756AA39Cu,0x026D930Au,0x9C0906A9u,0xEB0E363Fu,
    0x72076785u,0x05005713u,0x95BF4A82u,0xE2B87A14u,0x7BB12BAEu,0x0CB61B38u,
    0x92D28E9Bu,0xE5D5BE0Du,0x7CDCEFB7u,0x0BDBDF21u,0x86D3D2D4u,0xF1D4E242u,
    0x68DDB3F8u,0x1FDA836Eu,0x81BE16CDu,0xF6B9265Bu,0x6FB077E1u,0x18B74777u,
    0x88085AE6u,0xFF0F6A70u,0x66063BCAu,0x11010B5Cu,0x8F659EFFu,0xF862AE69u,
    0x616BFFD3u,0x166CCF45u,0xA00AE278u,0xD70DD2EEu,0x4E048354u,0x3903B3C2u,
    0xA7672661u,0xD06016F7u,0x4969474Du,0x3E6E77DBu,0xAED16A4Au,0xD9D65ADCu,
    0x40DF0B66u,0x37D83BF0u,0xA9BCAE53u,0xDEBB9EC5u,0x47B2CF7Fu,0x30B5FFE9u,
    0xBDBDF21Cu,0xCABAC28Au,0x53B39330u,0x24B4A3A6u,0xBAD03605u,0xCDD70693u,
    0x54DE5729u,0x23D967BFu,0xB3667A2Eu,0xC4614AB8u,0x5D681B02u,0x2A6F2B94u,
    0xB40BBE37u,0xC30C8EA1u,0x5A05DF1Bu,0x2D02EF8Du,
},
{
    0x00000000u,0x191B3141u,0x32366282u,0x2B2D53C3u,0x646CC504u,0x7D77F445u,
    0x565AA786u,0x4F4196C7u,0xC8D98A08u,0xD1C2BB49u,0xFAEFE88Au,0xE3F4D9CBu,
    0xACB54F0Cu,0xB5AE7E4Du,0x9E832D8Eu,0x87981CCFu,0x4AC21251u,0x53D92310u,
    0x78F470D3u,0x61EF4192u,0x2EAED755u,0x37B5E614u,0x1C98B5D7u,0x05838496u,
    0x821B9859u,0x9B00A918u,0xB02DFADBu,0xA936CB9Au,0xE6775D5Du,0xFF6C6C1Cu,
    0xD4413FDFu,0xCD5A0E9Eu,0x958424A2u,0x8C9F15E3u,0xA7B24620u,0xBEA97761u,
    0xF1E8E1A6u,0xE8F3D0E7u,0xC3DE8324u,0xDAC5B265u,0x5D5DAEAAu,0x44469FEBu,
    0x6F6BCC28u,0x7670FD69u,0x39316BAEu,0x202A5AEFu,0x0B07092Cu,0x121C386Du,
    0xDF4636F3u,0xC65D07B2u,0xED705471u,0xF46B6530u,0xBB2AF3F7u,0xA231C2B6u,
    0x891C9175u,0x9007A034u,0x179FBCFBu,0x0E848DBAu,0x25A9DE79u,0x3CB2EF38u,
    0x73F379FFu,0x6AE848BEu,0x41C51B7Du,0x58DE2A3Cu,0xF0794F05u,0xE9627E44u,
    0xC24F2D87u,0xDB541CC6u,0x94158A01u,0x8D0EBB40u,0xA623E883u,0xBF38D9C2u,
    0x38A0C50Du,0x21BBF44Cu,0x0A96A78Fu,0x138D96CEu,0x5CCC0009u,0x45D73148u,
    0x6EFA628Bu,0x77E153CAu,0xBABB5D54u,0xA3A06C15u,0x888D3FD6u,0x91960E97u,
    0xDED79850u,0xC7CCA911u,0xECE1FAD2u,0xF5FACB93u,0x7262D75Cu,0x6B79E61Du,
    0x4054B5DEu,0x594F849Fu,0x160E1258u,0x0F152319u,0x243870DAu,0x3D23419Bu,
    0x65FD6BA7u,0x7CE65AE6u,0x57CB0925u,0x4ED03864u,0x0191AEA3u,0x188A9FE2u,
    0x33A7CC21u,0x2ABCFD60u,0xAD24E1AFu,0xB43FD0EEu,0x9F12832Du,0x8609B26Cu,
    0xC94824ABu,0xD05315EAu,0xFB7E4629u,0xE2657768u,0x2F3F79F6u,0x362448B7u,
    0x1D091B74u,0x04122A35u,0x4B53BCF2u,0x52488DB3u,0x7965DE70u,0x607EEF31u,
    0xE7E6F3FEu,0xFEFDC2BFu,0xD5D0917Cu,0xCCCBA03Du,0x838A36FAu,0x9A9107BBu,
    0xB1BC5478u,0xA8A76539u,0x3B83984Bu,0x2298A90Au,0x09B5FAC9u,0x10AECB88u,
    0x5FEF5D4Fu,0x46F46C0Eu,0x6DD93FCDu,0x74C20E8Cu,0xF35A1243u,0xEA412302u,
    0xC16C70C1u,0xD8774180u,0x9736D747u,0x8E2DE606u,0xA500B5C5u,0xBC1B8484u,
    0x71418A1Au,0x685ABB5Bu,0x4377E898u,0x5A6CD9D9u,0x152D4F1Eu,0x0C367E5Fu,
    0x271B2D9Cu,0x3E001CDDu,0xB9980012u,0xA0833153u,0x8BAE6290u,0x92B553D1u,
    0xDDF4C516u,0xC4EFF457u,0xEFC2A794u,0xF6D996D5u,0xAE07BCE9u,0xB71C8DA8u,
    0x9C31DE6Bu,0x852AEF2Au,0xCA6B79EDu,0xD37048ACu,0xF85D1B6Fu,0xE1462A2Eu,
    0x66DE36E1u,0x7FC507A0u,0x54E85463u,0x4DF36522u,0x02B2F3E5u,0x1BA9C2A4u,
    0x30849167u,0x299FA026u,0xE4C5AEB8u,0xFDDE9FF9u,0xD6F3CC3Au,0xCFE8FD7Bu,
    0x80A96BBCu,0x99B25AFDu,0xB29F093Eu,0xAB84387Fu,0x2C1C24B0u,0x350715F1u,
    0x1E2A4632u,0x07317773u,0x4870E1B4u,0x516BD0F5u,0x7A468336u,0x635DB277u,
    0xCBFAD74Eu,0xD2E1E60Fu,0xF9CCB5CCu,0xE0D7848Du,0xAF96124Au,0xB68D230Bu,
    0x9DA070C8u,0x84BB4189u,0x03235D46u,0x1A386C07u,0x31153FC4u,0x280E0E85u,
    0x674F9842u,0x7E54A903u,0x5579FAC0u,0x4C62CB81u,0x8138C51Fu,0x9823F45Eu,
    0xB30EA79Du,0xAA1596DCu,0xE554001Bu,0xFC4F315Au,0xD7626299u,0xCE7953D8u,
    0x49E14F17u,0x50FA7E56u,0x7BD72D95u,0x62CC1CD4u,0x2D8D8A13u,0x3496BB52u,
    0x1FBBE891u,0x06A0D9D0u,0x5E7EF3ECu,0x4765C2ADu,0x6C48916Eu,0x7553A02Fu,
    0x3A1236E8u,0x230907A9u,0x0824546Au,0x113F652Bu,0x96A779E4u,0x8FBC48A5u,
    0xA4911B66u,0xBD8A2A27u,0xF2CBBCE0u,0xEBD08DA1u,0xC0FDDE62u,0xD9E6EF23u,
    0x14BCE1BDu,0x0DA7D0FCu,0x268A833Fu,0x3F91B27Eu,0x70D024B9u,0x69CB15F8u,
    0x42E6463Bu,0x5BFD777Au,0xDC656BB5u,0xC57E5AF4u,0xEE530937u,0xF7483876u,
    0xB809AEB1u,0xA1129FF0u,0x8A3FCC33u,0x9324FD72u,
},
{
    0x00000000u,0x01C26A37u,0x0384D46Eu,0x0246BE59u,0x0709A8DCu,0x06CBC2EBu,
    0x048D7CB2u,0x054F1685u,0x0E1351B8u,0x0FD13B8Fu,0x0D9785D6u,0x0C55EFE1u,
    0x091AF964u,0x08D89353u,0x0A9E2D0Au,0x0B5C473Du,0x1C26A370u,0x1DE4C947u,
    0x1FA2771Eu,0x1E601D29u,0x1B2F0BACu,0x1AED619Bu,0x18ABDFC2u,0x1969B5F5u,
    0x1235F2C8u,0x13F798FFu,0x11B126A6u,0x10734C91u,0x153C5A14u,0x14FE3023u,
    0x16B88E7Au,0x177AE44Du,0x384D46E0u,0x398F2CD7u,0x3BC9928Eu,0x3A0BF8B9u,
    0x3F44EE3Cu,0x3E86840Bu,0x3CC03A52u,0x3D025065u,0x365E1758u,0x379C7D6Fu,
    0x35DAC336u,0x3418A901u,0x3157BF84u,0x3095D5B3u,0x32D36BEAu,0x331101DDu,
    0x246BE590u,0x25A98FA7u,0x27EF31FEu,0x262D5BC9u,0x23624D4Cu,0x22A0277Bu,
    0x20E69922u,0x2124F315u,0x2A78B428u,0x2BBADE1Fu,0x29FC6046u,0x283E0A71u,
    0x2D711CF4u,0x2CB376C3u,0x2EF5C89Au,0x2F37A2ADu,0x709A8DC0u,0x7158E7F7u,
    0x731E59AEu,0x72DC3399u,0x7793251Cu,0x76514F2Bu,0x7417F172u,0x75D59B45u,
    0x7E89DC78u,0x7F4BB64Fu,0x7D0D0816u,0x7CCF6221u,0x798074A4u,0x78421E93u,
    0x7A04A0CAu,0x7BC6CAFDu,0x6CBC2EB0u,0x6D7E4487u,0x6F38FADEu,0x6EFA90E9u,
    0x6BB5866Cu,0x6A77EC5Bu,0x68315202u,0x69F33835u,0x62AF7F08u,0x636D153Fu,
    0x612BAB66u,0x60E9C151u,0x65A6D7D4u,0x6464BDE3u,0x662203BAu,0x67E0698Du,
    0x48D7CB20u,0x4915A117u,0x4B531F4Eu,0x4A917579u,0x4FDE63FCu,0x4E1C09CBu,
    0x4C5AB792u,0x4D98DDA5u,0x46C49A98u,0x4706F0AFu,0x45404EF6u,0x448224C1u,
    0x41CD3244u,0x400F5873u,0x4249E62Au,0x438B8C1Du,0x54F16850u,0x55330267u,
    0x5775BC3Eu,0x56B7D609u,0x53F8C08Cu,0x523AAABBu,0x507C14E2u,0x51BE7ED5u,
    0x5AE239E8u,0x5B2053DFu,0x5966ED86u,0x58A487B1u,0x5DEB9134u,0x5C29FB03u,
    0x5E6F455Au,0x5FAD2F6Du,0xE1351B80u,0xE0F771B7u,0xE2B1CFEEu,0xE373A5D9u,
    0xE63CB35Cu,0xE7FED96Bu,0xE5B86732u,0xE47A0D05u,0xEF264A38u,0xEEE4200Fu,
    0xECA29E56u,0xED60F461u,0xE82FE2E4u,0xE9ED88D3u,0xEBAB368Au,0xEA695CBDu,
    0xFD13B8F0u,0xFCD1D2C7u,0xFE976C9Eu,0xFF5506A9u,0xFA1A102Cu,0xFBD87A1Bu,
    0xF99EC442u,0xF85CAE75u,0xF300E948u,0xF2C2837Fu,0xF0843D26u,0xF1465711u,
    0xF4094194u,0xF5CB2BA3u,0xF78D95FAu,0xF64FFFCDu,0xD9785D60u,0xD8BA3757u,
    0xDAFC890Eu,0xDB3EE339u,0xDE71F5BCu,0xDFB39F8Bu,0xDDF521D2u,0xDC374BE5u,
    0xD76B0CD8u,0xD6A966EFu,0xD4EFD8B6u,0xD52DB281u,0xD062A404u,0xD1A0CE33u,
    0xD3E6706Au,0xD2241A5Du,0xC55EFE10u,0xC49C9427u,0xC6DA2A7Eu,0xC7184049u,
    0xC25756CCu,0xC3953CFBu,0xC1D382A2u,0xC011E895u,0xCB4DAFA8u,0xCA8FC59Fu,
    0xC8C97BC6u,0xC90B11F1u,0xCC440774u,0xCD866D43u,0xCFC0D31Au,0xCE02B92Du,
    0x91AF9640u,0x906DFC77u,0x922B422Eu,0x93E92819u,0x96A63E9Cu,0x976454ABu,
    0x9522EAF2u,0x94E080C5u,0x9FBCC7F8u,0x9E7EADCFu,0x9C381396u,0x9DFA79A1u,
    0x98B56F24u,0x99770513u,0x9B31BB4Au,0x9AF3D17Du,0x8D893530u,0x8C4B5F07u,
    0x8E0DE15Eu,0x8FCF8B69u,0x8A809DECu,0x8B42F7DBu,0x89044982u,0x88C623B5u,
    0x839A6488u,0x82580EBFu,0x801EB0E6u,0x81DCDAD1u,0x8493CC54u,0x8551A663u,
    0x8717183Au,0x86D5720Du,0xA9E2D0A0u,0xA820BA97u,0xAA6604CEu,0xABA46EF9u,
    0xAEEB787Cu,0xAF29124Bu,0xAD6FAC12u,0xACADC625u,0xA7F18118u,0xA633EB2Fu,
    0xA4755576u,0xA5B73F41u,0xA0F829C4u,0xA13A43F3u,0xA37CFDAAu,0xA2BE979Du,
    0xB5C473D0u,0xB40619E7u,0xB640A7BEu,0xB782CD89u,0xB2CDDB0Cu,0xB30FB13Bu,
    0xB1490F62u,0xB08B6555u,0xBBD72268u,0xBA15485Fu,0xB853F606u,0xB9919C31u,
    0xBCDE8AB4u,0xBD1CE083u,0xBF5A5EDAu,0xBE9834EDu,
},
{
    0x00000000u,0xB8BC6765u,0xAA09C88Bu,0x12B5AFEEu,0x8F629757u,0x37DEF032u,
    0x256B5FDCu,0x9DD738B9u,0xC5B428EFu,0x7D084F8Au,0x6FBDE064u,0xD7018701u,
    0x4AD6BFB8u,0xF26AD8DDu,0xE0DF7733u,0x58631056u,0x5019579Fu,0xE8A530FAu,
    0xFA109F14u,0x42ACF871u,0xDF7BC0C8u,0x67C7A7ADu,0x75720843u,0xCDCE6F26u,
    0x95AD7F70u,0x2D111815u,0x3FA4B7FBu,0x8718D09Eu,0x1ACFE827u,0xA2738F42u,
    0xB0C620ACu,0x087A47C9u,0xA032AF3Eu,0x188EC85Bu,0x0A3B67B5u,0xB28700D0u,
    0x2F503869u,0x97EC5F0Cu,0x8559F0E2u,0x3DE59787u,0x658687D1u,0xDD3AE0B4u,
    0xCF8F4F5Au,0x7733283Fu,0xEAE41086u,0x525877E3u,0x40EDD80Du,0xF851BF68u,
    0xF02BF8A1u,0x48979FC4u,0x5A22302Au,0xE29E574Fu,0x7F496FF6u,0xC7F50893u,
    0xD540A77Du,0x6DFCC018u,0x359FD04Eu,0x8D23B72Bu,0x9F9618C5u,0x272A7FA0u,
    0xBAFD4719u,0x0241207Cu,0x10F48F92u,0xA848E8F7u,0x9B14583Du,0x23A83F58u,
    0x311D90B6u,0x89A1F7D3u,0x1476CF6Au,0xACCAA80Fu,0xBE7F07E1u,0x06C36084u,
    0x5EA070D2u,0xE61C17B7u,0xF4A9B859u,0x4C15DF3Cu,0xD1C2E785u,0x697E80E0u,
    0x7BCB2F0Eu,0xC377486Bu,0xCB0D0FA2u,0x73B168C7u,0x6104C729u,0xD9B8A04Cu,
    0x446F98F5u,0xFCD3FF90u,0xEE66507Eu,0x56DA371Bu,0x0EB9274Du,0xB6054028u,
    0xA4B0EFC6u,0x1C0C88A3u,0x81DBB01Au,0x3967D77Fu,0x2BD27891u,0x936E1FF4u,
    0x3B26F703u,0x839A9066u,0x912F3F88u,0x299358EDu,0xB4446054u,0x0CF80731u,
    0x1E4DA8DFu,0xA6F1CFBAu,0xFE92DFECu,0x462EB889u,0x549B1767u,0xEC277002u,
    0x71F048BBu,0xC94C2FDEu,0xDBF98030u,0x6345E755u,0x6B3FA09Cu,0xD383C7F9u,
    0xC1366817u,0x798A0F72u,0xE45D37CBu,0x5CE150AEu,0x4E54FF40u,0xF6E89825u,
    0xAE8B8873u,0x1637EF16u,0x048240F8u,0xBC3E279Du,0x21E91F24u,0x99557841u,
    0x8BE0D7AFu,0x335CB0CAu,0xED59B63Bu,0x55E5D15Eu,0x47507EB0u,0xFFEC19D5u,
    0x623B216Cu,0xDA874609u,0xC832E9E7u,0x708E8E82u,0x28ED9ED4u,0x9051F9B1u,
    0x82E4565Fu,0x3A58313Au,0xA78F0983u,0x1F336EE6u,0x0D86C108u,0xB53AA66Du,
    0xBD40E1A4u,0x05FC86C1u,0x1749292Fu,0xAFF54E4Au,0x322276F3u,0x8A9E1196u,
    0x982BBE78u,0x2097D91Du,0x78F4C94Bu,0xC048AE2Eu,0xD2FD01C0u,0x6A4166A5u,
    0xF7965E1Cu,0x4F2A3979u,0x5D9F9697u,0xE523F1F2u,0x4D6B1905u,0xF5D77E60u,
    0xE762D18Eu,0x5FDEB6EBu,0xC2098E52u,0x7AB5E937u,0x680046D9u,0xD0BC21BCu,
    0x88DF31EAu,0x3063568Fu,0x22D6F961u,0x9A6A9E04u,0x07BDA6BDu,0xBF01C1D8u,
    0xADB46E36u,0x15080953u,0x1D724E9Au,0xA5CE29FFu,0xB77B8611u,0x0FC7E174u,
    0x9210D9CDu,0x2AACBEA8u,0x38191146u,0x80A57623u,0xD8C66675u,0x607A0110u,
    0x72CFAEFEu,0xCA73C99Bu,0x57A4F122u,0xEF189647u,0xFDAD39A9u,0x45115ECCu,
    0x764DEE06u,0xCEF18963u,0xDC44268Du,0x64F841E8u,0xF92F7951u,0x41931E34u,
    0x5326B1DAu,0xEB9AD6BFu,0xB3F9C6E9u,0x0B45A18Cu,0x19F00E62u,0xA14C6907u,
    0x3C9B51BEu,0x842736DBu,0x96929935u,0x2E2EFE50u,0x2654B999u,0x9EE8DEFCu,
    0x8C5D7112u,0x34E11677u,0xA9362ECEu,0x118A49ABu,0x033FE645u,0xBB838120u,
    0xE3E09176u,0x5B5CF613u,0x49E959FDu,0xF1553E98u,0x6C820621u,0xD43E6144u,
    0xC68BCEAAu,0x7E37A9CFu,0xD67F4138u,0x6EC3265Du,0x7C7689B3u,0xC4CAEED6u,
    0x591DD66Fu,0xE1A1B10Au,0xF3141EE4u,0x4BA87981u,0x13CB69D7u,0xAB770EB2u,
    0xB9C2A15Cu,0x017EC639u,0x9CA9FE80u,0x241599E5u,0x36A0360Bu,0x8E1C516Eu,
    0x866616A7u,0x3EDA71C2u,0x2C6FDE2Cu,0x94D3B949u,0x090481F0u,0xB1B8E695u,
    0xA30D497Bu,0x1BB12E1Eu,0x43D23E48u,0xFB6E592Du,0xE9DBF6C3u,0x516791A6u,
    0xCCB0A91Fu,0x740CCE7Au,0x66B96194u,0xDE0506F1u,
},
{
    0x00000000u,0x3D6029B0u,0x7AC05360u,0x47A07AD0u,0xF580A6C0u,0xC8E08F70u,
    0x8F40F5A0u,0xB220DC10u,0x30704BC1u,0x0D106271u,0x4AB018A1u,0x77D03111u,
    0xC5F0ED01u,0xF890C4B1u,0xBF30BE61u,0x825097D1u,0x60E09782u,0x5D80BE32u,
    0x1A20C4E2u,0x2740ED52u,0x95603142u,0xA80018F2u,0xEFA06222u,0xD2C04B92u,
    0x5090DC43u,0x6DF0F5F3u,0x2A508F23u,0x1730A693u,0xA5107A83u,0x98705333u,
    0xDFD029E3u,0xE2B00053u,0xC1C12F04u,0xFCA106B4u,0xBB017C64u,0x866155D4u,
    0x344189C4u,0x0921A074u,0x4E81DAA4u,0x73E1F314u,0xF1B164C5u,0xCCD14D75u,
    0x8B7137A5u,0xB6111E15u,0x0431C205u,0x3951EBB5u,0x7EF19165u,0x4391B8D5u,
    0xA121B886u,0x9C419136u,0xDBE1EBE6u,0xE681C256u,0x54A11E46u,0x69C137F6u,
    0x2E614D26u,0x13016496u,0x9151F347u,0xAC31DAF7u,0xEB91A027u,0xD6F18997u,
    0x64D15587u,0x59B17C37u,0x1E1106E7u,0x23712F57u,0x58F35849u,0x659371F9u,
    0x22330B29u,0x1F532299u,0xAD73FE89u,0x9013D739u,0xD7B3ADE9u,0xEAD38459u,
    0x68831388u,0x55E33A38u,0x124340E8u,0x2F236958u,0x9D03B548u,0xA0639CF8u,
    0xE7C3E628u,0xDAA3CF98u,0x3813CFCBu,0x0573E67Bu,0x42D39CABu,0x7FB3B51Bu,
    0xCD93690Bu,0xF0F340BBu,0xB7533A6Bu,0x8A3313DBu,0x0863840Au,0x3503ADBAu,
    0x72A3D76Au,0x4FC3FEDAu,0xFDE322CAu,0xC0830B7Au,0x872371AAu,0xBA43581Au,
    0x9932774Du,0xA4525EFDu,0xE3F2242Du,0xDE920D9Du,0x6CB2D18Du,0x51D2F83Du,
    0x167282EDu,0x2B12AB5Du,0xA9423C8Cu,0x9422153Cu,0xD3826FECu,0xEEE2465Cu,
    0x5CC29A4Cu,0x61A2B3FCu,0x2602C92Cu,0x1B62E09Cu,0xF9D2E0CFu,0xC4B2C97Fu,
    0x8312B3AFu,0xBE729A1Fu,0x0C52460Fu,0x31326FBFu,0x7692156Fu,0x4BF23CDFu,
    0xC9A2AB0Eu,0xF4C282BEu,0xB362F86Eu,0x8E02D1DEu,0x3C220DCEu,0x0142247Eu,
    0x46E25EAEu,0x7B82771Eu,0xB1E6B092u,0x8C869922u,0xCB26E3F2u,0xF646CA42u,
    0x44661652u,0x79063FE2u,0x3EA64532u,0x03C66C82u,0x8196FB53u,0xBCF6D2E3u,
    0xFB56A833u,0xC6368183u,0x74165D93u,0x49767423u,0x0ED60EF3u,0x33B62743u,
    0xD1062710u,0xEC660EA0u,0xABC67470u,0x96A65DC0u,0x248681D0u,0x19E6A860u,
    0x5E46D2B0u,0x6326FB00u,0xE1766CD1u,0xDC164561u,0x9BB63FB1u,0xA6D61601u,
    0x14F6CA11u,0x2996E3A1u,0x6E369971u,0x5356B0C1u,0x70279F96u,0x4D47B626u,
    0x0AE7CCF6u,0x3787E546u,0x85A73956u,0xB8C710E6u,0xFF676A36u,0xC2074386u,
    0x4057D457u,0x7D37FDE7u,0x3A978737u,0x07F7AE87u,0xB5D77297u,0x88B75B27u,
    0xCF1721F7u,0xF2770847u,0x10C70814u,0x2DA721A4u,0x6A075B74u,0x576772C4u,
    0xE547AED4u,0xD8278764u,0x9F87FDB4u,0xA2E7D404u,0x20B743D5u,0x1DD76A65u,
    0x5A7710B5u,0x67173905u,0xD537E515u,0xE857CCA5u,0xAFF7B675u,0x92979FC5u,
    0xE915E8DBu,0xD475C16Bu,0x93D5BBBBu,0xAEB5920Bu,0x1C954E1Bu,0x21F567ABu,
    0x66551D7Bu,0x5B3534CBu,0xD965A31Au,0xE4058AAAu,0xA3A5F07Au,0x9EC5D9CAu,
    0x2CE505DAu,0x11852C6Au,0x562556BAu,0x6B457F0Au,0x89F57F59u,0xB49556E9u,
    0xF3352C39u,0xCE550589u,0x7C75D999u,0x4115F029u,0x06B58AF9u,0x3BD5A349u,
    0xB9853498u,0x84E51D28u,0xC34567F8u,0xFE254E48u,0x4C059258u,0x7165BBE8u,
    0x36C5C138u,0x0BA5E888u,0x28D4C7DFu,0x15B4EE6Fu,0x521494BFu,0x6F74BD0Fu,
    0xDD54611Fu,0xE03448AFu,0xA794327Fu,0x9AF41BCFu,0x18A48C1Eu,0x25C4A5AEu,
    0x6264DF7Eu,0x5F04F6CEu,0xED242ADEu,0xD044036Eu,0x97E479BEu,0xAA84500Eu,
    0x4834505Du,0x755479EDu,0x32F4033Du,0x0F942A8Du,0xBDB4F69Du,0x80D4DF2Du,
    0xC774A5FDu,0xFA148C4Du,0x78441B9Cu,0x4524322Cu,0x028448FCu,0x3FE4614Cu,
    0x8DC4BD5Cu,0xB0A494ECu,0xF704EE3Cu,0xCA64C78Cu,
},
{
    0x00000000u,0xCB5CD3A5u,0x4DC8A10Bu,0x869472AEu,0x9B914216u,0x50CD91B3u,
    0xD659E31Du,0x1D0530B8u,0xEC53826Du,0x270F51C8u,0xA19B2366u,0x6AC7F0C3u,
    0x77C2C07Bu,0xBC9E13DEu,0x3A0A6170u,0xF156B2D5u,0x03D6029Bu,0xC88AD13Eu,
    0x4E1EA390u,0x85427035u,0x9847408Du,0x531B9328u,0xD58FE186u,0x1ED33223u,
    0xEF8580F6u,0x24D95353u,0xA24D21FDu,0x6911F258u,0x7414C2E0u,0xBF481145u,
    0x39DC63EBu,0xF280B04Eu,0x07AC0536u,0xCCF0D693u,0x4A64A43Du,0x81387798u,
    0x9C3D4720u,0x57619485u,0xD1F5E62Bu,0x1AA9358Eu,0xEBFF875Bu,0x20A354FEu,
    0xA6372650u,0x6D6BF5F5u,0x706EC54Du,0xBB3216E8u,0x3DA66446u,0xF6FAB7E3u,
    0x047A07ADu,0xCF26D408u,0x49B2A6A6u,0x82EE7503u,0x9FEB45BBu,0x54B7961Eu,
    0xD223E4B0u,0x197F3715u,0xE82985C0u,0x23755665u,0xA5E124CBu,0x6EBDF76Eu,
    0x73B8C7D6u,0xB8E41473u,0x3E7066DDu,0xF52CB578u,0x0F580A6Cu,0xC404D9C9u,
    0x4290AB67u,0x89CC78C2u,0x94C9487Au,0x5F959BDFu,0xD901E971u,0x125D3AD4u,
    0xE30B8801u,0x28575BA4u,0xAEC3290Au,0x659FFAAFu,0x789ACA17u,0xB3C619B2u,
    0x35526B1Cu,0xFE0EB8B9u,0x0C8E08F7u,0xC7D2DB52u,0x4146A9FCu,0x8A1A7A59u,
    0x971F4AE1u,0x5C439944u,0xDAD7EBEAu,0x118B384Fu,0xE0DD8A9Au,0x2B81593Fu,
    0xAD152B91u,0x6649F834u,0x7B4CC88Cu,0xB0101B29u,0x36846987u,0xFDD8BA22u,
    0x08F40F5Au,0xC3A8DCFFu,0x453CAE51u,0x8E607DF4u,0x93654D4Cu,0x58399EE9u,
    0xDEADEC47u,0x15F13FE2u,0xE4A78D37u,0x2FFB5E92u,0xA96F2C3Cu,0x6233FF99u,
    0x7F36CF21u,0xB46A1C84u,0x32FE6E2Au,0xF9A2BD8Fu,0x0B220DC1u,0xC07EDE64u,
    0x46EAACCAu,0x8DB67F6Fu,0x90B34FD7u,0x5BEF9C72u,0xDD7BEEDCu,0x16273D79u,
    0xE7718FACu,0x2C2D5C09u,0xAAB92EA7u,0x61E5FD02u,0x7CE0CDBAu,0xB7BC1E1Fu,
    0x31286CB1u,0xFA74BF14u,0x1EB014D8u,0xD5ECC77Du,0x5378B5D3u,0x98246676u,
    0x852156CEu,0x4E7D856Bu,0xC8E9F7C5u,0x03B52460u,0xF2E396B5u,0x39BF4510u,
    0xBF2B37BEu,0x7477E41Bu,0x6972D4A3u,0xA22E0706u,0x24BA75A8u,0xEFE6A60Du,
    0x1D661643u,0xD63AC5E6u,0x50AEB748u,0x9BF264EDu,0x86F75455u,0x4DAB87F0u,
    0xCB3FF55Eu,0x006326FBu,0xF135942Eu,0x3A69478Bu,0xBCFD3525u,0x77A1E680u,
    0x6AA4D638u,0xA1F8059Du,0x276C7733u,0xEC30A496u,0x191C11EEu,0xD240C24Bu,
    0x54D4B0E5u,0x9F886340u,0x828D53F8u,0x49D1805Du,0xCF45F2F3u,0x04192156u,
    0xF54F9383u,0x3E134026u,0xB8873288u,0x73DBE12Du,0x6EDED195u,0xA5820230u,
    0x2316709Eu,0xE84AA33Bu,0x1ACA1375u,0xD196C0D0u,0x5702B27Eu,0x9C5E61DBu,
    0x815B5163u,0x4A0782C6u,0xCC93F068u,0x07CF23CDu,0xF6999118u,0x3DC542BDu,
    0xBB513013u,0x700DE3B6u,0x6D08D30Eu,0xA65400ABu,0x20C07205u,0xEB9CA1A0u,
    0x11E81EB4u,0xDAB4CD11u,0x5C20BFBFu,0x977C6C1Au,0x8A795CA2u,0x41258F07u,
    0xC7B1FDA9u,0x0CED2E0Cu,0xFDBB9CD9u,0x36E74F7Cu,0xB0733DD2u,0x7B2FEE77u,
    0x662ADECFu,0xAD760D6Au,0x2BE27FC4u,0xE0BEAC61u,0x123E1C2Fu,0xD962CF8Au,
    0x5FF6BD24u,0x94AA6E81u,0x89AF5E39u,0x42F38D9Cu,0xC467FF32u,0x0F3B2C97u,
    0xFE6D9E42u,0x35314DE7u,0xB3A53F49u,0x78F9ECECu,0x65FCDC54u,0xAEA00FF1u,
    0x28347D5Fu,0xE368AEFAu,0x16441B82u,0xDD18C827u,0x5B8CBA89u,0x90D0692Cu,
    0x8DD55994u,0x46898A31u,0xC01DF89Fu,0x0B412B3Au,0xFA1799EFu,0x314B4A4Au,
    0xB7DF38E4u,0x7C83EB41u,0x6186DBF9u,0xAADA085Cu,0x2C4E7AF2u,0xE712A957u,
    0x15921919u,0xDECECABCu,0x585AB812u,0x93066BB7u,0x8E035B0Fu,0x455F88AAu,
    0xC3CBFA04u,0x089729A1u,0xF9C19B74u,0x329D48D1u,0xB4093A7Fu,0x7F55E9DAu,
    0x6250D962u,0xA90C0AC7u,0x2F987869u,0xE4C4ABCCu,
},
{
    0x00000000u,0xA6770BB4u,0x979F1129u,0x31E81A9Du,0xF44F2413u,0x52382FA7u,
    0x63D0353Au,0xC5A73E8Eu,0x33EF4E67u,0x959845D3u,0xA4705F4Eu,0x020754FAu,
    0xC7A06A74u,0x61D761C0u,0x503F7B5Du,0xF64870E9u,0x67DE9CCEu,0xC1A9977Au,
    0xF0418DE7u,0x56368653u,0x9391B8DDu,0x35E6B369u,0x040EA9F4u,0xA279A240u,
    0x5431D2A9u,0xF246D91Du,0xC3AEC380u,0x65D9C834u,0xA07EF6BAu,0x0609FD0Eu,
    0x37E1E793u,0x9196EC27u,0xCFBD399Cu,0x69CA3228u,0x582228B5u,0xFE552301u,
    0x3BF21D8Fu,0x9D85163Bu,0xAC6D0CA6u,0x0A1A0712u,0xFC5277FBu,0x5A257C4Fu,
    0x6BCD66D2u,0xCDBA6D66u,0x081D53E8u,0xAE6A585Cu,0x9F8242C1u,0x39F54975u,
    0xA863A552u,0x0E14AEE6u,0x3FFCB47Bu,0x998BBFCFu,0x5C2C8141u,0xFA5B8AF5u,
    0xCBB39068u,0x6DC49BDCu,0x9B8CEB35u,0x3DFBE081u,0x0C13FA1Cu,0xAA64F1A8u,
    0x6FC3CF26u,0xC9B4C492u,0xF85CDE0Fu,0x5E2BD5BBu,0x440B7579u,0xE27C7ECDu,
    0xD3946450u,0x75E36FE4u,0xB044516Au,0x16335ADEu,0x27DB4043u,0x81AC4BF7u,
    0x77E43B1Eu,0xD19330AAu,0xE07B2A37u,0x460C2183u,0x83AB1F0Du,0x25DC14B9u,
    0x14340E24u,0xB2430590u,0x23D5E9B7u,0x85A2E203u,0xB44AF89Eu,0x123DF32Au,
    0xD79ACDA4u,0x71EDC610u,0x4005DC8Du,0xE672D739u,0x103AA7D0u,0xB64DAC64u,
    0x87A5B6F9u,0x21D2BD4Du,0xE47583C3u,0x42028877u,0x73EA92EAu,0xD59D995Eu,
    0x8BB64CE5u,0x2DC14751u,0x1C295DCCu,0xBA5E5678u,0x7FF968F6u,0xD98E6342u,
    0xE86679DFu,0x4E11726Bu,0xB8590282u,0x1E2E0936u,0x2FC613ABu,0x89B1181Fu,
    0x4C162691u,0xEA612D25u,0xDB8937B8u,0x7DFE3C0Cu,0xEC68D02Bu,0x4A1FDB9Fu,
    0x7BF7C102u,0xDD80CAB6u,0x1827F438u,0xBE50FF8Cu,0x8FB8E511u,0x29CFEEA5u,
    0xDF879E4Cu,0x79F095F8u,0x48188F65u,0xEE6F84D1u,0x2BC8BA5Fu,0x8DBFB1EBu,
    0xBC57AB76u,0x1A20A0C2u,0x8816EAF2u,0x2E61E146u,0x1F89FBDBu,0xB9FEF06Fu,
    0x7C59CEE1u,0xDA2EC555u,0xEBC6DFC8u,0x4DB1D47Cu,0xBBF9A495u,0x1D8EAF21u,
    0x2C66B5BCu,0x8A11BE08u,0x4FB68086u,0xE9C18B32u,0xD82991AFu,0x7E5E9A1Bu,
    0xEFC8763Cu,0x49BF7D88u,0x78576715u,0xDE206CA1u,0x1B87522Fu,0xBDF0599Bu,
    0x8C184306u,0x2A6F48B2u,0xDC27385Bu,0x7A5033EFu,0x4BB82972u,0xEDCF22C6u,
    0x28681C48u,0x8E1F17FCu,0xBFF70D61u,0x198006D5u,0x47ABD36Eu,0xE1DCD8DAu,
    0xD034C247u,0x7643C9F3u,0xB3E4F77Du,0x1593FCC9u,0x247BE654u,0x820CEDE0u,
    0x74449D09u,0xD23396BDu,0xE3DB8C20u,0x45AC8794u,0x800BB91Au,0x267CB2AEu,
    0x1794A833u,0xB1E3A387u,0x20754FA0u,0x86024414u,0xB7EA5E89u,0x119D553Du,
    0xD43A6BB3u,0x724D6007u,0x43A57A9Au,0xE5D2712Eu,0x139A01C7u,0xB5ED0A73u,
    0x840510EEu,0x22721B5Au,0xE7D525D4u,0x41A22E60u,0x704A34FDu,0xD63D3F49u,
    0xCC1D9F8Bu,0x6A6A943Fu,0x5B828EA2u,0xFDF58516u,0x3852BB98u,0x9E25B02Cu,
    0xAFCDAAB1u,0x09BAA105u,0xFFF2D1ECu,0x5985DA58u,0x686DC0C5u,0xCE1ACB71u,
    0x0BBDF5FFu,0xADCAFE4Bu,0x9C22E4D6u,0x3A55EF62u,0xABC30345u,0x0DB408F1u,
    0x3C5C126Cu,0x9A2B19D8u,0x5F8C2756u,0xF9FB2CE2u,0xC813367Fu,0x6E643DCBu,
    0x982C4D22u,0x3E5B4696u,0x0FB35C0Bu,0xA9C457BFu,0x6C636931u,0xCA146285u,
    0xFBFC7818u,0x5D8B73ACu,0x03A0A617u,0xA5D7ADA3u,0x943FB73Eu,0x3248BC8Au,
    0xF7EF8204u,0x519889B0u,0x6070932Du,0xC6079899u,0x304FE870u,0x9638E3C4u,
    0xA7D0F959u,0x01A7F2EDu,0xC400CC63u,0x6277C7D7u,0x539FDD4Au,0xF5E8D6FEu,
    0x647E3AD9u,0xC209316Du,0xF3E12BF0u,0x55962044u,0x90311ECAu,0x3646157Eu,
    0x07AE0FE3u,0xA1D90457u,0x579174BEu,0xF1E67F0Au,0xC00E6597u,0x66796E23u,
    0xA3DE50ADu,0x05A95B19u,0x34414184u,0x92364A30u,
},
{
    0x00000000u,0xCCAA009Eu,0x4225077Du,0x8E8F07E3u,0x844A0EFAu,0x48E00E64u,
    0xC66F0987u,0x0AC50919u,0xD3E51BB5u,0x1F4F1B2Bu,0x91C01CC8u,0x5D6A1C56u,
    0x57AF154Fu,0x9B0515D1u,0x158A1232u,0xD92012ACu,0x7CBB312Bu,0xB01131B5u,
    0x3E9E3656u,0xF23436C8u,0xF8F13FD1u,0x345B3F4Fu,0xBAD438ACu,0x767E3832u,
    0xAF5E2A9Eu,0x63F42A00u,0xED7B2DE3u,0x21D12D7Du,0x2B142464u,0xE7BE24FAu,
    0x69312319u,0xA59B2387u,0xF9766256u,0x35DC62C8u,0xBB53652Bu,0x77F965B5u,
    0x7D3C6CACu,0xB1966C32u,0x3F196BD1u,0xF3B36B4Fu,0x2A9379E3u,0xE639797Du,
    0x68B67E9Eu,0xA41C7E00u,0xAED97719u,0x62737787u,0xECFC7064u,0x205670FAu,
    0x85CD537Du,0x496753E3u,0xC7E85400u,0x0B42549Eu,0x01875D87u,0xCD2D5D19u,
    0x43A25AFAu,0x8F085A64u,0x562848C8u,0x9A824856u,0x140D4FB5u,0xD8A74F2Bu,
    0xD2624632u,0x1EC846ACu,0x9047414Fu,0x5CED41D1u,0x299DC2EDu,0xE537C273u,
    0x6BB8C590u,0xA712C50Eu,0xADD7CC17u,0x617DCC89u,0xEFF2CB6Au,0x2358CBF4u,
    0xFA78D958u,0x36D2D9C6u,0xB85DDE25u,0x74F7DEBBu,0x7E32D7A2u,0xB298D73Cu,
    0x3C17D0DFu,0xF0BDD041u,0x5526F3C6u,0x998CF358u,0x1703F4BBu,0xDBA9F425u,
    0xD16CFD3Cu,0x1DC6FDA2u,0x9349FA41u,0x5FE3FADFu,0x86C3E873u,0x4A69E8EDu,
    0xC4E6EF0Eu,0x084CEF90u,0x0289E689u,0xCE23E617u,0x40ACE1F4u,0x8C06E16Au,
    0xD0EBA0BBu,0x1C41A025u,0x92CEA7C6u,0x5E64A758u,0x54A1AE41u,0x980BAEDFu,
    0x1684A93Cu,0xDA2EA9A2u,0x030EBB0Eu,0xCFA4BB90u,0x412BBC73u,0x8D81BCEDu,
    0x8744B5F4u,0x4BEEB56Au,0xC561B289u,0x09CBB217u,0xAC509190u,0x60FA910Eu,
    0xEE7596EDu,0x22DF9673u,0x281A9F6Au,0xE4B09FF4u,0x6A3F9817u,0xA6959889u,
    0x7FB58A25u,0xB31F8ABBu,0x3D908D58u,0xF13A8DC6u,0xFBFF84DFu,0x37558441u,
    0xB9DA83A2u,0x7570833Cu,0x533B85DAu,0x9F918544u,0x111E82A7u,0xDDB48239u,
    0xD7718B20u,0x1BDB8BBEu,0x95548C5Du,0x59FE8CC3u,0x80DE9E6Fu,0x4C749EF1u,
    0xC2FB9912u,0x0E51998Cu,0x04949095u,0xC83E900Bu,0x46B197E8u,0x8A1B9776u,
    0x2F80B4F1u,0xE32AB46Fu,0x6DA5B38Cu,0xA10FB312u,0xABCABA0Bu,0x6760BA95u,
    0xE9EFBD76u,0x2545BDE8u,0xFC65AF44u,0x30CFAFDAu,0xBE40A839u,0x72EAA8A7u,
    0x782FA1BEu,0xB485A120u,0x3A0AA6C3u,0xF6A0A65Du,0xAA4DE78Cu,0x66E7E712u,
    0xE868E0F1u,0x24C2E06Fu,0x2E07E976u,0xE2ADE9E8u,0x6C22EE0Bu,0xA088EE95u,
    0x79A8FC39u,0xB502FCA7u,0x3B8DFB44u,0xF727FBDAu,0xFDE2F2C3u,0x3148F25Du,
    0xBFC7F5BEu,0x736DF520u,0xD6F6D6A7u,0x1A5CD639u,0x94D3D1DAu,0x5879D144u,
    0x52BCD85Du,0x9E16D8C3u,0x1099DF20u,0xDC33DFBEu,0x0513CD12u,0xC9B9CD8Cu,
    0x4736CA6Fu,0x8B9CCAF1u,0x8159C3E8u,0x4DF3C376u,0xC37CC495u,0x0FD6C40Bu,
    0x7AA64737u,0xB60C47A9u,0x3883404Au,0xF42940D4u,0xFEEC49CDu,0x32464953u,
    0xBCC94EB0u,0x70634E2Eu,0xA9435C82u,0x65E95C1Cu,0xEB665BFFu,0x27CC5B61u,
    0x2D095278u,0xE1A352E6u,0x6F2C5505u,0xA386559Bu,0x061D761Cu,0xCAB77682u,
    0x44387161u,0x889271FFu,0x825778E6u,0x4EFD7878u,0xC0727F9Bu,0x0CD87F05u,
    0xD5F86DA9u,0x19526D37u,0x97DD6AD4u,0x5B776A4Au,0x51B26353u,0x9D1863CDu,
    0x1397642Eu,0xDF3D64B0u,0x83D02561u,0x4F7A25FFu,0xC1F5221Cu,0x0D5F2282u,
    0x079A2B9Bu,0xCB302B05u,0x45BF2CE6u,0x89152C78u,0x50353ED4u,0x9C9F3E4Au,
    0x121039A9u,0xDEBA3937u,0xD47F302Eu,0x18D530B0u,0x965A3753u,0x5AF037CDu,
    0xFF6B144Au,0x33C114D4u,0xBD4E1337u,0x71E413A9u,0x7B211AB0u,0xB78B1A2Eu,
    0x39041DCDu,0xF5AE1D53u,0x2C8E0FFFu,0xE0240F61u,0x6EAB0882u,0xA201081Cu,
    0xA8C40105u,0x646E019Bu,0xEAE10678u,0x264B06E6u,
},
};

static const uint32_t crc32c_s8[8][256] = {
{
    0x00000000u,0xF26B8303u,0xE13B70F7u,0x1350F3F4u,0xC79A971Fu,0x35F1141Cu,
    0x26A1E7E8u,0xD4CA64EBu,0x8AD958CFu,0x78B2DBCCu,0x6BE22838u,0x9989AB3Bu,
    0x4D43CFD0u,0xBF284CD3u,0xAC78BF27u,0x5E133C24u,0x105EC76Fu,0xE235446Cu,
    0xF165B798u,0x030E349Bu,0xD7C45070u,0x25AFD373u,0x36FF2087u,0xC494A384u,
    0x9A879FA0u,0x68EC1CA3u,0x7BBCEF57u,0x89D76C54u,0x5D1D08BFu,0xAF768BBCu,
    0xBC267848u,0x4E4DFB4Bu,0x20BD8EDEu,0xD2D60DDDu,0xC186FE29u,0x33ED7D2Au,
    0xE72719C1u,0x154C9AC2u,0x061C6936u,0xF477EA35u,0xAA64D611u,0x580F5512u,
    0x4B5FA6E6u,0xB93425E5u,0x6DFE410Eu,0x9F95C20Du,0x8CC531F9u,0x7EAEB2FAu,
    0x30E349B1u,0xC288CAB2u,0xD1D83946u,0x23B3BA45u,0xF779DEAEu,0x05125DADu,
    0x1642AE59u,0xE4292D5Au,0xBA3A117Eu,0x4851927Du,0x5B016189u,0xA96AE28Au,
    0x7DA08661u,0x8FCB0562u,0x9C9BF696u,0x6EF07595u,0x417B1DBCu,0xB3109EBFu,
    0xA0406D4Bu,0x522BEE48u,0x86E18AA3u,0x748A09A0u,0x67DAFA54u,0x95B17957u,
    0xCBA24573u,0x39C9C670u,0x2A993584u,0xD8F2B687u,0x0C38D26Cu,0xFE53516Fu,
    0xED03A29Bu,0x1F682198u,0x5125DAD3u,0xA34E59D0u,0xB01EAA24u,0x42752927u,
    0x96BF4DCCu,0x64D4CECFu,0x77843D3Bu,0x85EFBE38u,0xDBFC821Cu,0x2997011Fu,
    0x3AC7F2EBu,0xC8AC71E8u,0x1C661503u,0xEE0D9600u,0xFD5D65F4u,0x0F36E6F7u,
    0x61C69362u,0x93AD1061u,0x80FDE395u,0x72966096u,0xA65C047Du,0x5437877Eu,
    0x4767748Au,0xB50CF789u,0xEB1FCBADu,0x197448AEu,0x0A24BB5Au,0xF84F3859u,
    0x2C855CB2u,0xDEEEDFB1u,0xCDBE2C45u,0x3FD5AF46u,0x7198540Du,0x83F3D70Eu,
    0x90A324FAu,0x62C8A7F9u,0xB602C312u,0x44694011u,0x5739B3E5u,0xA55230E6u,
    0xFB410CC2u,0x092A8FC1u,0x1A7A7C35u,0xE811FF36u,0x3CDB9BDDu,0xCEB018DEu,
    0xDDE0EB2Au,0x2F8B6829u,0x82F63B78u,0x709DB87Bu,0x63CD4B8Fu,0x91A6C88Cu,
    0x456CAC67u,0xB7072F64u,0xA457DC90u,0x563C5F93u,0x082F63B7u,0xFA44E0B4u,
    0xE9141340u,0x1B7F9043u,0xCFB5F4A8u,0x3DDE77ABu,0x2E8E845Fu,0xDCE5075Cu,
    0x92A8FC17u,0x60C37F14u,0x73938CE0u,0x81F80FE3u,0x55326B08u,0xA759E80Bu,
    0xB4091BFFu,0x466298FCu,0x1871A4D8u,0xEA1A27DBu,0xF94AD42Fu,0x0B21572Cu,
    0xDFEB33C7u,0x2D80B0C4u,0x3ED04330u,0xCCBBC033u,0xA24BB5A6u,0x502036A5u,
    0x4370C551u,0xB11B4652u,0x65D122B9u,0x97BAA1BAu,0x84EA524Eu,0x7681D14Du,
    0x2892ED69u,0xDAF96E6Au,0xC9A99D9Eu,0x3BC21E9Du,0xEF087A76u,0x1D63F975u,
    0x0E330A81u,0xFC588982u,0xB21572C9u,0x407EF1CAu,0x532E023Eu,0xA145813Du,
    0x758FE5D6u,0x87E466D5u,0x94B49521u,0x66DF1622u,0x38CC2A06u,0xCAA7A905u,
    0xD9F75AF1u,0x2B9CD9F2u,0xFF56BD19u,0x0D3D3E1Au,0x1E6DCDEEu,0xEC064EEDu,
    0xC38D26C4u,0x31E6A5C7u,0x22B65633u,0xD0DDD530u,0x0417B1DBu,0xF67C32D8u,
    0xE52CC12Cu,0x1747422Fu,0x49547E0Bu,0xBB3FFD08u,0xA86F0EFCu,0x5A048DFFu,
    0x8ECEE914u,0x7CA56A17u,0x6FF599E3u,0x9D9E1AE0u,0xD3D3E1ABu,0x21B862A8u,
    0x32E8915Cu,0xC083125Fu,0x144976B4u,0xE622F5B7u,0xF5720643u,0x07198540u,
    0x590AB964u,0xAB613A67u,0xB831C993u,0x4A5A4A90u,0x9E902E7Bu,0x6CFBAD78u,
    0x7FAB5E8Cu,0x8DC0DD8Fu,0xE330A81Au,0x115B2B19u,0x020BD8EDu,0xF0605BEEu,
    0x24AA3F05u,0xD6C1BC06u,0xC5914FF2u,0x37FACCF1u,0x69E9F0D5u,0x9B8273D6u,
    0x88D28022u,0x7AB90321u,0xAE7367CAu,0x5C18E4C9u,0x4F48173Du,0xBD23943Eu,
    0xF36E6F75u,0x0105EC76u,0x12551F82u,0xE03E9C81u,0x34F4F86Au,0xC69F7B69u,
    0xD5CF889Du,0x27A40B9Eu,0x79B737BAu,0x8BDCB4B9u,0x988C474Du,0x6AE7C44Eu,
    0xBE2DA0A5u,0x4C4623A6u,0x5F16D052u,0xAD7D5351u,
},
{
    0x00000000u,0x13A29877u,0x274530EEu,0x34E7A899u,0x4E8A61DCu,0x5D28F9ABu,
    0x69CF5132u,0x7A6DC945u,0x9D14C3B8u,0x8EB65BCFu,0xBA51F356u,0xA9F36B21u,
    0xD39EA264u,0xC03C3A13u,0xF4DB928Au,0xE7790AFDu,0x3FC5F181u,0x2C6769F6u,
    0x1880C16Fu,0x0B225918u,0x714F905Du,0x62ED082Au,0x560AA0B3u,0x45A838C4u,
    0xA2D13239u,0xB173AA4Eu,0x859402D7u,0x96369AA0u,0xEC5B53E5u,0xFFF9CB92u,
    0xCB1E630Bu,0xD8BCFB7Cu,0x7F8BE302u,0x6C297B75u,0x58CED3ECu,0x4B6C4B9Bu,
    0x310182DEu,0x22A31AA9u,0x1644B230u,0x05E62A47u,0xE29F20BAu,0xF13DB8CDu,
    0xC5DA1054u,0xD6788823u,0xAC154166u,0xBFB7D911u,0x8B507188u,0x98F2E9FFu,
    0x404E1283u,0x53EC8AF4u,0x670B226Du,0x74A9BA1Au,0x0EC4735Fu,0x1D66EB28u,
    0x298143B1u,0x3A23DBC6u,0xDD5AD13Bu,0xCEF8494Cu,0xFA1FE1D5u,0xE9BD79A2u,
    0x93D0B0E7u,0x80722890u,0xB4958009u,0xA737187Eu,0xFF17C604u,0xECB55E73u,
    0xD852F6EAu,0xCBF06E9Du,0xB19DA7D8u,0xA23F3FAFu,0x96D89736u,0x857A0F41u,
    0x620305BCu,0x71A19DCBu,0x45463552u,0x56E4AD25u,0x2C896460u,0x3F2BFC17u,
    0x0BCC548Eu,0x186ECCF9u,0xC0D23785u,0xD370AFF2u,0xE797076Bu,0xF4359F1Cu,
    0x8E585659u,0x9DFACE2Eu,0xA91D66B7u,0xBABFFEC0u,0x5DC6F43Du,0x4E646C4Au,
    0x7A83C4D3u,0x69215CA4u,0x134C95E1u,0x00EE0D96u,0x3409A50Fu,0x27AB3D78u,
    0x809C2506u,0x933EBD71u,0xA7D915E8u,0xB47B8D9Fu,0xCE1644DAu,0xDDB4DCADu,
    0xE9537434u,0xFAF1EC43u,0x1D88E6BEu,0x0E2A7EC9u,0x3ACDD650u,0x296F4E27u,
    0x53028762u,0x40A01F15u,0x7447B78Cu,0x67E52FFBu,0xBF59D487u,0xACFB4CF0u,
    0x981CE469u,0x8BBE7C1Eu,0xF1D3B55Bu,0xE2712D2Cu,0xD69685B5u,0xC5341DC2u,
    0x224D173Fu,0x31EF8F48u,0x050827D1u,0x16AABFA6u,0x6CC776E3u,0x7F65EE94u,
    0x4B82460Du,0x5820DE7Au,0xFBC3FAF9u,0xE861628Eu,0xDC86CA17u,0xCF245260u,
    0xB5499B25u,0xA6EB0352u,0x920CABCBu,0x81AE33BCu,0x66D73941u,0x7575A136u,
    0x419209AFu,0x523091D8u,0x285D589Du,0x3BFFC0EAu,0x0F186873u,0x1CBAF004u,
    0xC4060B78u,0xD7A4930Fu,0xE3433B96u,0xF0E1A3E1u,0x8A8C6AA4u,0x992EF2D3u,
    0xADC95A4Au,0xBE6BC23Du,0x5912C8C0u,0x4AB050B7u,0x7E57F82Eu,0x6DF56059u,
    0x1798A91Cu,0x043A316Bu,0x30DD99F2u,0x237F0185u,0x844819FBu,0x97EA818Cu,
    0xA30D2915u,0xB0AFB162u,0xCAC27827u,0xD960E050u,0xED8748C9u,0xFE25D0BEu,
    0x195CDA43u,0x0AFE4234u,0x3E19EAADu,0x2DBB72DAu,0x57D6BB9Fu,0x447423E8u,
    0x70938B71u,0x63311306u,0xBB8DE87Au,0xA82F700Du,0x9CC8D894u,0x8F6A40E3u,
    0xF50789A6u,0xE6A511D1u,0xD242B948u,0xC1E0213Fu,0x26992BC2u,0x353BB3B5u,
    0x01DC1B2Cu,0x127E835Bu,0x68134A1Eu,0x7BB1D269u,0x4F567AF0u,0x5CF4E287u,
    0x04D43CFDu,0x1776A48Au,0x23910C13u,0x30339464u,0x4A5E5D21u,0x59FCC556u,
    0x6D1B6DCFu,0x7EB9F5B8u,0x99C0FF45u,0x8A626732u,0xBE85CFABu,0xAD2757DCu,
    0xD74A9E99u,0xC4E806EEu,0xF00FAE77u,0xE3AD3600u,0x3B11CD7Cu,0x28B3550Bu,
    0x1C54FD92u,0x0FF665E5u,0x759BACA0u,0x663934D7u,0x52DE9C4Eu,0x417C0439u,
    0xA6050EC4u,0xB5A796B3u,0x81403E2Au,0x92E2A65Du,0xE88F6F18u,0xFB2DF76Fu,
    0xCFCA5FF6u,0xDC68C781u,0x7B5FDFFFu,0x68FD4788u,0x5C1AEF11u,0x4FB87766u,
    0x35D5BE23u,0x26772654u,0x12908ECDu,0x013216BAu,0xE64B1C47u,0xF5E98430u,
    0xC10E2CA9u,0xD2ACB4DEu,0xA8C17D9Bu,0xBB63E5ECu,0x8F844D75u,0x9C26D502u,
    0x449A2E7Eu,0x5738B609u,0x63DF1E90u,0x707D86E7u,0x0A104FA2u,0x19B2D7D5u,
    0x2D557F4Cu,0x3EF7E73Bu,0xD98EEDC6u,0xCA2C75B1u,0xFECBDD28u,0xED69455Fu,
    0x97048C1Au,0x84A6146Du,0xB041BCF4u,0xA3E32483u,
},
{
    0x00000000u,0xA541927Eu,0x4F6F520Du,0xEA2EC073u,0x9EDEA41Au,0x3B9F3664u,
    0xD1B1F617u,0x74F06469u,0x38513EC5u,0x9D10ACBBu,0x773E6CC8u,0xD27FFEB6u,
    0xA68F9ADFu,0x03CE08A1u,0xE9E0C8D2u,0x4CA15AACu,0x70A27D8Au,0xD5E3EFF4u,
    0x3FCD2F87u,0x9A8CBDF9u,0xEE7CD990u,0x4B3D4BEEu,0xA1138B9Du,0x045219E3u,
    0x48F3434Fu,0xEDB2D131u,0x079C1142u,0xA2DD833Cu,0xD62DE755u,0x736C752Bu,
    0x9942B558u,0x3C032726u,0xE144FB14u,0x4405696Au,0xAE2BA919u,0x0B6A3B67u,
    0x7F9A5F0Eu,0xDADBCD70u,0x30F50D03u,0x95B49F7Du,0xD915C5D1u,0x7C5457AFu,
    0x967A97DCu,0x333B05A2u,0x47CB61CBu,0xE28AF3B5u,0x08A433C6u,0xADE5A1B8u,
    0x91E6869Eu,0x34A714E0u,0xDE89D493u,0x7BC846EDu,0x0F382284u,0xAA79B0FAu,
    0x40577089u,0xE516E2F7u,0xA9B7B85Bu,0x0CF62A25u,0xE6D8EA56u,0x43997828u,
    0x37691C41u,0x92288E3Fu,0x78064E4Cu,0xDD47DC32u,0xC76580D9u,0x622412A7u,
    0x880AD2D4u,0x2D4B40AAu,0x59BB24C3u,0xFCFAB6BDu,0x16D476CEu,0xB395E4B0u,
    0xFF34BE1Cu,0x5A752C62u,0xB05BEC11u,0x151A7E6Fu,0x61EA1A06u,0xC4AB8878u,
    0x2E85480Bu,0x8BC4DA75u,0xB7C7FD53u,0x12866F2Du,0xF8A8AF5Eu,0x5DE93D20u,
    0x29195949u,0x8C58CB37u,0x66760B44u,0xC337993Au,0x8F96C396u,0x2AD751E8u,
    0xC0F9919Bu,0x65B803E5u,0x1148678Cu,0xB409F5F2u,0x5E273581u,0xFB66A7FFu,
    0x26217BCDu,0x8360E9B3u,0x694E29C0u,0xCC0FBBBEu,0xB8FFDFD7u,0x1DBE4DA9u,
    0xF7908DDAu,0x52D11FA4u,0x1E704508u,0xBB31D776u,0x511F1705u,0xF45E857Bu,
    0x80AEE112u,0x25EF736Cu,0xCFC1B31Fu,0x6A802161u,0x56830647u,0xF3C29439u,
    0x19EC544Au,0xBCADC634u,0xC85DA25Du,0x6D1C3023u,0x8732F050u,0x2273622Eu,
    0x6ED23882u,0xCB93AAFCu,0x21BD6A8Fu,0x84FCF8F1u,0xF00C9C98u,0x554D0EE6u,
    0xBF63CE95u,0x1A225CEBu,0x8B277743u,0x2E66E53Du,0xC448254Eu,0x6109B730u,
    0x15F9D359u,0xB0B84127u,0x5A968154u,0xFFD7132Au,0xB3764986u,0x1637DBF8u,
    0xFC191B8Bu,0x595889F5u,0x2DA8ED9Cu,0x88E97FE2u,0x62C7BF91u,0xC7862DEFu,
    0xFB850AC9u,0x5EC498B7u,0xB4EA58C4u,0x11ABCABAu,0x655BAED3u,0xC01A3CADu,
    0x2A34FCDEu,0x8F756EA0u,0xC3D4340Cu,0x6695A672u,0x8CBB6601u,0x29FAF47Fu,
    0x5D0A9016u,0xF84B0268u,0x1265C21Bu,0xB7245065u,0x6A638C57u,0xCF221E29u,
    0x250CDE5Au,0x804D4C24u,0xF4BD284Du,0x51FCBA33u,0xBBD27A40u,0x1E93E83Eu,
    0x5232B292u,0xF77320ECu,0x1D5DE09Fu,0xB81C72E1u,0xCCEC1688u,0x69AD84F6u,
    0x83834485u,0x26C2D6FBu,0x1AC1F1DDu,0xBF8063A3u,0x55AEA3D0u,0xF0EF31AEu,
    0x841F55C7u,0x215EC7B9u,0xCB7007CAu,0x6E3195B4u,0x2290CF18u,0x87D15D66u,
    0x6DFF9D15u,0xC8BE0F6Bu,0xBC4E6B02u,0x190FF97Cu,0xF321390Fu,0x5660AB71u,
    0x4C42F79Au,0xE90365E4u,0x032DA597u,0xA66C37E9u,0xD29C5380u,0x77DDC1FEu,
    0x9DF3018Du,0x38B293F3u,0x7413C95Fu,0xD1525B21u,0x3B7C9B52u,0x9E3D092Cu,
    0xEACD6D45u,0x4F8CFF3Bu,0xA5A23F48u,0x00E3AD36u,0x3CE08A10u,0x99A1186Eu,
    0x738FD81Du,0xD6CE4A63u,0xA23E2E0Au,0x077FBC74u,0xED517C07u,0x4810EE79u,
    0x04B1B4D5u,0xA1F026ABu,0x4BDEE6D8u,0xEE9F74A6u,0x9A6F10CFu,0x3F2E82B1u,
    0xD50042C2u,0x7041D0BCu,0xAD060C8Eu,0x08479EF0u,0xE2695E83u,0x4728CCFDu,
    0x33D8A894u,0x96993AEAu,0x7CB7FA99u,0xD9F668E7u,0x9557324Bu,0x3016A035u,
    0xDA386046u,0x7F79F238u,0x0B899651u,0xAEC8042Fu,0x44E6C45Cu,0xE1A75622u,
    0xDDA47104u,0x78E5E37Au,0x92CB2309u,0x378AB177u,0x437AD51Eu,0xE63B4760u,
    0x0C158713u,0xA954156Du,0xE5F54FC1u,0x40B4DDBFu,0xAA9A1DCCu,0x0FDB8FB2u,
    0x7B2BEBDBu,0xDE6A79A5u,0x3444B9D6u,0x91052BA8u,
},
{
    0x00000000u,0xDD45AAB8u,0xBF672381u,0x62228939u,0x7B2231F3u,0xA6679B4Bu,
    0xC4451272u,0x1900B8CAu,0xF64463E6u,0x2B01C95Eu,0x49234067u,0x9466EADFu,
    0x8D665215u,0x5023F8ADu,0x32017194u,0xEF44DB2Cu,0xE964B13Du,0x34211B85u,
    0x560392BCu,0x8B463804u,0x924680CEu,0x4F032A76u,0x2D21A34Fu,0xF06409F7u,
    0x1F20D2DBu,0xC2657863u,0xA047F15Au,0x7D025BE2u,0x6402E328u,0xB9474990u,
    0xDB65C0A9u,0x06206A11u,0xD725148Bu,0x0A60BE33u,0x6842370Au,0xB5079DB2u,
    0xAC072578u,0x71428FC0u,0x136006F9u,0xCE25AC41u,0x2161776Du,0xFC24DDD5u,
    0x9E0654ECu,0x4343FE54u,0x5A43469Eu,0x8706EC26u,0xE524651Fu,0x3861CFA7u,
    0x3E41A5B6u,0xE3040F0Eu,0x81268637u,0x5C632C8Fu,0x45639445u,0x98263EFDu,
    0xFA04B7C4u,0x27411D7Cu,0xC805C650u,0x15406CE8u,0x7762E5D1u,0xAA274F69u,
    0xB327F7A3u,0x6E625D1Bu,0x0C40D422u,0xD1057E9Au,0xABA65FE7u,0x76E3F55Fu,
    0x14C17C66u,0xC984D6DEu,0xD0846E14u,0x0DC1C4ACu,0x6FE34D95u,0xB2A6E72Du,
    0x5DE23C01u,0x80A796B9u,0xE2851F80u,0x3FC0B538u,0x26C00DF2u,0xFB85A74Au,
    0x99A72E73u,0x44E284CBu,0x42C2EEDAu,0x9F874462u,0xFDA5CD5Bu,0x20E067E3u,
    0x39E0DF29u,0xE4A57591u,0x8687FCA8u,0x5BC25610u,0xB4868D3Cu,0x69C32784u,
    0x0BE1AEBDu,0xD6A40405u,0xCFA4BCCFu,0x12E11677u,0x70C39F4Eu,0xAD8635F6u,
    0x7C834B6Cu,0xA1C6E1D4u,0xC3E468EDu,0x1EA1C255u,0x07A17A9Fu,0xDAE4D027u,
    0xB8C6591Eu,0x6583F3A6u,0x8AC7288Au,0x57828232u,0x35A00B0Bu,0xE8E5A1B3u,
    0xF1E51979u,0x2CA0B3C1u,0x4E823AF8u,0x93C79040u,0x95E7FA51u,0x48A250E9u,
    0x2A80D9D0u,0xF7C57368u,0xEEC5CBA2u,0x3380611Au,0x51A2E823u,0x8CE7429Bu,
    0x63A399B7u,0xBEE6330Fu,0xDCC4BA36u,0x0181108Eu,0x1881A844u,0xC5C402FCu,
    0xA7E68BC5u,0x7AA3217Du,0x52A0C93Fu,0x8FE56387u,0xEDC7EABEu,0x30824006u,
    0x2982F8CCu,0xF4C75274u,0x96E5DB4Du,0x4BA071F5u,0xA4E4AAD9u,0x79A10061u,
    0x1B838958u,0xC6C623E0u,0xDFC69B2Au,0x02833192u,0x60A1B8ABu,0xBDE41213u,
    0xBBC47802u,0x6681D2BAu,0x04A35B83u,0xD9E6F13Bu,0xC0E649F1u,0x1DA3E349u,
    0x7F816A70u,0xA2C4C0C8u,0x4D801BE4u,0x90C5B15Cu,0xF2E73865u,0x2FA292DDu,
    0x36A22A17u,0xEBE780AFu,0x89C50996u,0x5480A32Eu,0x8585DDB4u,0x58C0770Cu,
    0x3AE2FE35u,0xE7A7548Du,0xFEA7EC47u,0x23E246FFu,0x41C0CFC6u,0x9C85657Eu,
    0x73C1BE52u,0xAE8414EAu,0xCCA69DD3u,0x11E3376Bu,0x08E38FA1u,0xD5A62519u,
    0xB784AC20u,0x6AC10698u,0x6CE16C89u,0xB1A4C631u,0xD3864F08u,0x0EC3E5B0u,
    0x17C35D7Au,0xCA86F7C2u,0xA8A47EFBu,0x75E1D443u,0x9AA50F6Fu,0x47E0A5D7u,
    0x25C22CEEu,0xF8878656u,0xE1873E9Cu,0x3CC29424u,0x5EE01D1Du,0x83A5B7A5u,
    0xF90696D8u,0x24433C60u,0x4661B559u,0x9B241FE1u,0x8224A72Bu,0x5F610D93u,
    0x3D4384AAu,0xE0062E12u,0x0F42F53Eu,0xD2075F86u,0xB025D6BFu,0x6D607C07u,
    0x7460C4CDu,0xA9256E75u,0xCB07E74Cu,0x16424DF4u,0x106227E5u,0xCD278D5Du,
    0xAF050464u,0x7240AEDCu,0x6B401616u,0xB605BCAEu,0xD4273597u,0x09629F2Fu,
    0xE6264403u,0x3B63EEBBu,0x59416782u,0x8404CD3Au,0x9D0475F0u,0x4041DF48u,
    0x22635671u,0xFF26FCC9u,0x2E238253u,0xF36628EBu,0x9144A1D2u,0x4C010B6Au,
    0x5501B3A0u,0x88441918u,0xEA669021u,0x37233A99u,0xD867E1B5u,0x05224B0Du,
    0x6700C234u,0xBA45688Cu,0xA345D046u,0x7E007AFEu,0x1C22F3C7u,0xC167597Fu,
    0xC747336Eu,0x1A0299D6u,0x782010EFu,0xA565BA57u,0xBC65029Du,0x6120A825u,
    0x0302211Cu,0xDE478BA4u,0x31035088u,0xEC46FA30u,0x8E647309u,0x5321D9B1u,
    0x4A21617Bu,0x9764CBC3u,0xF54642FAu,0x2803E842u,
},
{
    0x00000000u,0x38116FACu,0x7022DF58u,0x4833B0F4u,0xE045BEB0u,0xD854D11Cu,
    0x906761E8u,0xA8760E44u,0xC5670B91u,0xFD76643Du,0xB545D4C9u,0x8D54BB65u,
    0x2522B521u,0x1D33DA8Du,0x55006A79u,0x6D1105D5u,0x8F2261D3u,0xB7330E7Fu,
    0xFF00BE8Bu,0xC711D127u,0x6F67DF63u,0x5776B0CFu,0x1F45003Bu,0x27546F97u,
    0x4A456A42u,0x725405EEu,0x3A67B51Au,0x0276DAB6u,0xAA00D4F2u,0x9211BB5Eu,
    0xDA220BAAu,0xE2336406u,0x1BA8B557u,0x23B9DAFBu,0x6B8A6A0Fu,0x539B05A3u,
    0xFBED0BE7u,0xC3FC644Bu,0x8BCFD4BFu,0xB3DEBB13u,0xDECFBEC6u,0xE6DED16Au,
    0xAEED619Eu,0x96FC0E32u,0x3E8A0076u,0x069B6FDAu,0x4EA8DF2Eu,0x76B9B082u,
    0x948AD484u,0xAC9BBB28u,0xE4A80BDCu,0xDCB96470u,0x74CF6A34u,0x4CDE0598u,
    0x04EDB56Cu,0x3CFCDAC0u,0x51EDDF15u,0x69FCB0B9u,0x21CF004Du,0x19DE6FE1u,
    0xB1A861A5u,0x89B90E09u,0xC18ABEFDu,0xF99BD151u,0x37516AAEu,0x0F400502u,
    0x4773B5F6u,0x7F62DA5Au,0xD714D41Eu,0xEF05BBB2u,0xA7360B46u,0x9F2764EAu,
    0xF236613Fu,0xCA270E93u,0x8214BE67u,0xBA05D1CBu,0x1273DF8Fu,0x2A62B023u,
    0x625100D7u,0x5A406F7Bu,0xB8730B7Du,0x806264D1u,0xC851D425u,0xF040BB89u,
    0x5836B5CDu,0x6027DA61u,0x28146A95u,0x10050539u,0x7D1400ECu,0x45056F40u,
    0x0D36DFB4u,0x3527B018u,0x9D51BE5Cu,0xA540D1F0u,0xED736104u,0xD5620EA8u,
    0x2CF9DFF9u,0x14E8B055u,0x5CDB00A1u,0x64CA6F0Du,0xCCBC6149u,0xF4AD0EE5u,
    0xBC9EBE11u,0x848FD1BDu,0xE99ED468u,0xD18FBBC4u,0x99BC0B30u,0xA1AD649Cu,
    0x09DB6AD8u,0x31CA0574u,0x79F9B580u,0x41E8DA2Cu,0xA3DBBE2Au,0x9BCAD186u,
    0xD3F96172u,0xEBE80EDEu,0x439E009Au,0x7B8F6F36u,0x33BCDFC2u,0x0BADB06Eu,
    0x66BCB5BBu,0x5EADDA17u,0x169E6AE3u,0x2E8F054Fu,0x86F90B0Bu,0xBEE864A7u,
    0xF6DBD453u,0xCECABBFFu,0x6EA2D55Cu,0x56B3BAF0u,0x1E800A04u,0x269165A8u,
    0x8EE76BECu,0xB6F60440u,0xFEC5B4B4u,0xC6D4DB18u,0xABC5DECDu,0x93D4B161u,
    0xDBE70195u,0xE3F66E39u,0x4B80607Du,0x73910FD1u,0x3BA2BF25u,0x03B3D089u,
    0xE180B48Fu,0xD991DB23u,0x91A26BD7u,0xA9B3047Bu,0x01C50A3Fu,0x39D46593u,
    0x71E7D567u,0x49F6BACBu,0x24E7BF1Eu,0x1CF6D0B2u,0x54C56046u,0x6CD40FEAu,
    0xC4A201AEu,0xFCB36E02u,0xB480DEF6u,0x8C91B15Au,0x750A600Bu,0x4D1B0FA7u,
    0x0528BF53u,0x3D39D0FFu,0x954FDEBBu,0xAD5EB117u,0xE56D01E3u,0xDD7C6E4Fu,
    0xB06D6B9Au,0x887C0436u,0xC04FB4C2u,0xF85EDB6Eu,0x5028D52Au,0x6839BA86u,
    0x200A0A72u,0x181B65DEu,0xFA2801D8u,0xC2396E74u,0x8A0ADE80u,0xB21BB12Cu,
    0x1A6DBF68u,0x227CD0C4u,0x6A4F6030u,0x525E0F9Cu,0x3F4F0A49u,0x075E65E5u,
    0x4F6DD511u,0x777CBABDu,0xDF0AB4F9u,0xE71BDB55u,0xAF286BA1u,0x9739040Du,
    0x59F3BFF2u,0x61E2D05Eu,0x29D160AAu,0x11C00F06u,0xB9B60142u,0x81A76EEEu,
    0xC994DE1Au,0xF185B1B6u,0x9C94B463u,0xA485DBCFu,0xECB66B3Bu,0xD4A70497u,
    0x7CD10AD3u,0x44C0657Fu,0x0CF3D58Bu,0x34E2BA27u,0xD6D1DE21u,0xEEC0B18Du,
    0xA6F30179u,0x9EE26ED5u,0x36946091u,0x0E850F3Du,0x46B6BFC9u,0x7EA7D065u,
    0x13B6D5B0u,0x2BA7BA1Cu,0x63940AE8u,0x5B856544u,0xF3F36B00u,0xCBE204ACu,
    0x83D1B458u,0xBBC0DBF4u,0x425B0AA5u,0x7A4A6509u,0x3279D5FDu,0x0A68BA51u,
    0xA21EB415u,0x9A0FDBB9u,0xD23C6B4Du,0xEA2D04E1u,0x873C0134u,0xBF2D6E98u,
    0xF71EDE6Cu,0xCF0FB1C0u,0x6779BF84u,0x5F68D028u,0x175B60DCu,0x2F4A0F70u,
    0xCD796B76u,0xF56804DAu,0xBD5BB42Eu,0x854ADB82u,0x2D3CD5C6u,0x152DBA6Au,
    0x5D1E0A9Eu,0x650F6532u,0x081E60E7u,0x300F0F4Bu,0x783CBFBFu,0x402DD013u,
    0xE85BDE57u,0xD04AB1FBu,0x9879010Fu,0xA0686EA3u,
},
{
    0x00000000u,0xEF306B19u,0xDB8CA0C3u,0x34BCCBDAu,0xB2F53777u,0x5DC55C6Eu,
    0x697997B4u,0x8649FCADu,0x6006181Fu,0x8F367306u,0xBB8AB8DCu,0x54BAD3C5u,
    0xD2F32F68u,0x3DC34471u,0x097F8FABu,0xE64FE4B2u,0xC00C303Eu,0x2F3C5B27u,
    0x1B8090FDu,0xF4B0FBE4u,0x72F90749u,0x9DC96C50u,0xA975A78Au,0x4645CC93u,
    0xA00A2821u,0x4F3A4338u,0x7B8688E2u,0x94B6E3FBu,0x12FF1F56u,0xFDCF744Fu,
    0xC973BF95u,0x2643D48Cu,0x85F4168Du,0x6AC47D94u,0x5E78B64Eu,0xB148DD57u,
    0x370121FAu,0xD8314AE3u,0xEC8D8139u,0x03BDEA20u,0xE5F20E92u,0x0AC2658Bu,
    0x3E7EAE51u,0xD14EC548u,0x570739E5u,0xB83752FCu,0x8C8B9926u,0x63BBF23Fu,
    0x45F826B3u,0xAAC84DAAu,0x9E748670u,0x7144ED69u,0xF70D11C4u,0x183D7ADDu,
    0x2C81B107u,0xC3B1DA1Eu,0x25FE3EACu,0xCACE55B5u,0xFE729E6Fu,0x1142F576u,
    0x970B09DBu,0x783B62C2u,0x4C87A918u,0xA3B7C201u,0x0E045BEBu,0xE13430F2u,
    0xD588FB28u,0x3AB89031u,0xBCF16C9Cu,0x53C10785u,0x677DCC5Fu,0x884DA746u,
    0x6E0243F4u,0x813228EDu,0xB58EE337u,0x5ABE882Eu,0xDCF77483u,0x33C71F9Au,
    0x077BD440u,0xE84BBF59u,0xCE086BD5u,0x213800CCu,0x1584CB16u,0xFAB4A00Fu,
    0x7CFD5CA2u,0x93CD37BBu,0xA771FC61u,0x48419778u,0xAE0E73CAu,0x413E18D3u,
    0x7582D309u,0x9AB2B810u,0x1CFB44BDu,0xF3CB2FA4u,0xC777E47Eu,0x28478F67u,
    0x8BF04D66u,0x64C0267Fu,0x507CEDA5u,0xBF4C86BCu,0x39057A11u,0xD6351108u,
    0xE289DAD2u,0x0DB9B1CBu,0xEBF65579u,0x04C63E60u,0x307AF5BAu,0xDF4A9EA3u,
    0x5903620Eu,0xB6330917u,0x828FC2CDu,0x6DBFA9D4u,0x4BFC7D58u,0xA4CC1641u,
    0x9070DD9Bu,0x7F40B682u,0xF9094A2Fu,0x16392136u,0x2285EAECu,0xCDB581F5u,
    0x2BFA6547u,0xC4CA0E5Eu,0xF076C584u,0x1F46AE9Du,0x990F5230u,0x763F3929u,
    0x4283F2F3u,0xADB399EAu,0x1C08B7D6u,0xF338DCCFu,0xC7841715u,0x28B47C0Cu,
    0xAEFD80A1u,0x41CDEBB8u,0x75712062u,0x9A414B7Bu,0x7C0EAFC9u,0x933EC4D0u,
    0xA7820F0Au,0x48B26413u,0xCEFB98BEu,0x21CBF3A7u,0x1577387Du,0xFA475364u,
    0xDC0487E8u,0x3334ECF1u,0x0788272Bu,0xE8B84C32u,0x6EF1B09Fu,0x81C1DB86u,
    0xB57D105Cu,0x5A4D7B45u,0xBC029FF7u,0x5332F4EEu,0x678E3F34u,0x88BE542Du,
    0x0EF7A880u,0xE1C7C399u,0xD57B0843u,0x3A4B635Au,0x99FCA15Bu,0x76CCCA42u,
    0x42700198u,0xAD406A81u,0x2B09962Cu,0xC439FD35u,0xF08536EFu,0x1FB55DF6u,
    0xF9FAB944u,0x16CAD25Du,0x22761987u,0xCD46729Eu,0x4B0F8E33u,0xA43FE52Au,
    0x90832EF0u,0x7FB345E9u,0x59F09165u,0xB6C0FA7Cu,0x827C31A6u,0x6D4C5ABFu,
    0xEB05A612u,0x0435CD0Bu,0x308906D1u,0xDFB96DC8u,0x39F6897Au,0xD6C6E263u,
    0xE27A29B9u,0x0D4A42A0u,0x8B03BE0Du,0x6433D514u,0x508F1ECEu,0xBFBF75D7u,
    0x120CEC3Du,0xFD3C8724u,0xC9804CFEu,0x26B027E7u,0xA0F9DB4Au,0x4FC9B053u,
    0x7B757B89u,0x94451090u,0x720AF422u,0x9D3A9F3Bu,0xA98654E1u,0x46B63FF8u,
    0xC0FFC355u,0x2FCFA84Cu,0x1B736396u,0xF443088Fu,0xD200DC03u,0x3D30B71Au,
    0x098C7CC0u,0xE6BC17D9u,0x60F5EB74u,0x8FC5806Du,0xBB794BB7u,0x544920AEu,
    0xB206C41Cu,0x5D36AF05u,0x698A64DFu,0x86BA0FC6u,0x00F3F36Bu,0xEFC39872u,
    0xDB7F53A8u,0x344F38B1u,0x97F8FAB0u,0x78C891A9u,0x4C745A73u,0xA344316Au,
    0x250DCDC7u,0xCA3DA6DEu,0xFE816D04u,0x11B1061Du,0xF7FEE2AFu,0x18CE89B6u,
    0x2C72426Cu,0xC3422975u,0x450BD5D8u,0xAA3BBEC1u,0x9E87751Bu,0x71B71E02u,
    0x57F4CA8Eu,0xB8C4A197u,0x8C786A4Du,0x63480154u,0xE501FDF9u,0x0A3196E0u,
    0x3E8D5D3Au,0xD1BD3623u,0x37F2D291u,0xD8C2B988u,0xEC7E7252u,0x034E194Bu,
    0x8507E5E6u,0x6A378EFFu,0x5E8B4525u,0xB1BB2E3Cu,
},
{
    0x00000000u,0x68032CC8u,0xD0065990u,0xB8057558u,0xA5E0C5D1u,0xCDE3E919u,
    0x75E69C41u,0x1DE5B089u,0x4E2DFD53u,0x262ED19Bu,0x9E2BA4C3u,0xF628880Bu,
    0xEBCD3882u,0x83CE144Au,0x3BCB6112u,0x53C84DDAu,0x9C5BFAA6u,0xF458D66Eu,
    0x4C5DA336u,0x245E8FFEu,0x39BB3F77u,0x51B813BFu,0xE9BD66E7u,0x81BE4A2Fu,
    0xD27607F5u,0xBA752B3Du,0x02705E65u,0x6A7372ADu,0x7796C224u,0x1F95EEECu,
    0xA7909BB4u,0xCF93B77Cu,0x3D5B83BDu,0x5558AF75u,0xED5DDA2Du,0x855EF6E5u,
    0x98BB466Cu,0xF0B86AA4u,0x48BD1FFCu,0x20BE3334u,0x73767EEEu,0x1B755226u,
    0xA370277Eu,0xCB730BB6u,0xD696BB3Fu,0xBE9597F7u,0x0690E2AFu,0x6E93CE67u,
    0xA100791Bu,0xC90355D3u,0x7106208Bu,0x19050C43u,0x04E0BCCAu,0x6CE39002u,
    0xD4E6E55Au,0xBCE5C992u,0xEF2D8448u,0x872EA880u,0x3F2BDDD8u,0x5728F110u,
    0x4ACD4199u,0x22CE6D51u,0x9ACB1809u,0xF2C834C1u,0x7AB7077Au,0x12B42BB2u,
    0xAAB15EEAu,0xC2B27222u,0xDF57C2ABu,0xB754EE63u,0x0F519B3Bu,0x6752B7F3u,
    0x349AFA29u,0x5C99D6E1u,0xE49CA3B9u,0x8C9F8F71u,0x917A3FF8u,0xF9791330u,
    0x417C6668u,0x297F4AA0u,0xE6ECFDDCu,0x8EEFD114u,0x36EAA44Cu,0x5EE98884u,
    0x430C380Du,0x2B0F14C5u,0x930A619Du,0xFB094D55u,0xA8C1008Fu,0xC0C22C47u,
    0x78C7591Fu,0x10C475D7u,0x0D21C55Eu,0x6522E996u,0xDD279CCEu,0xB524B006u,
    0x47EC84C7u,0x2FEFA80Fu,0x97EADD57u,0xFFE9F19Fu,0xE20C4116u,0x8A0F6DDEu,
    0x320A1886u,0x5A09344Eu,0x09C17994u,0x61C2555Cu,0xD9C72004u,0xB1C40CCCu,
    0xAC21BC45u,0xC422908Du,0x7C27E5D5u,0x1424C91Du,0xDBB77E61u,0xB3B452A9u,
    0x0BB127F1u,0x63B20B39u,0x7E57BBB0u,0x16549778u,0xAE51E220u,0xC652CEE8u,
    0x959A8332u,0xFD99AFFAu,0x459CDAA2u,0x2D9FF66Au,0x307A46E3u,0x58796A2Bu,
    0xE07C1F73u,0x887F33BBu,0xF56E0EF4u,0x9D6D223Cu,0x25685764u,0x4D6B7BACu,
    0x508ECB25u,0x388DE7EDu,0x808892B5u,0xE88BBE7Du,0xBB43F3A7u,0xD340DF6Fu,
    0x6B45AA37u,0x034686FFu,0x1EA33676u,0x76A01ABEu,0xCEA56FE6u,0xA6A6432Eu,
    0x6935F452u,0x0136D89Au,0xB933ADC2u,0xD130810Au,0xCCD53183u,0xA4D61D4Bu,
    0x1CD36813u,0x74D044DBu,0x27180901u,0x4F1B25C9u,0xF71E5091u,0x9F1D7C59u,
    0x82F8CCD0u,0xEAFBE018u,0x52FE9540u,0x3AFDB988u,0xC8358D49u,0xA036A181u,
    0x1833D4D9u,0x7030F811u,0x6DD54898u,0x05D66450u,0xBDD31108u,0xD5D03DC0u,
    0x8618701Au,0xEE1B5CD2u,0x561E298Au,0x3E1D0542u,0x23F8B5CBu,0x4BFB9903u,
    0xF3FEEC5Bu,0x9BFDC093u,0x546E77EFu,0x3C6D5B27u,0x84682E7Fu,0xEC6B02B7u,
    0xF18EB23Eu,0x998D9EF6u,0x2188EBAEu,0x498BC766u,0x1A438ABCu,0x7240A674u,
    0xCA45D32Cu,0xA246FFE4u,0xBFA34F6Du,0xD7A063A5u,0x6FA516FDu,0x07A63A35u,
    0x8FD9098Eu,0xE7DA2546u,0x5FDF501Eu,0x37DC7CD6u,0x2A39CC5Fu,0x423AE097u,
    0xFA3F95CFu,0x923CB907u,0xC1F4F4DDu,0xA9F7D815u,0x11F2AD4Du,0x79F18185u,
    0x6414310Cu,0x0C171DC4u,0xB412689Cu,0xDC114454u,0x1382F328u,0x7B81DFE0u,
    0xC384AAB8u,0xAB878670u,0xB66236F9u,0xDE611A31u,0x66646F69u,0x0E6743A1u,
    0x5DAF0E7Bu,0x35AC22B3u,0x8DA957EBu,0xE5AA7B23u,0xF84FCBAAu,0x904CE762u,
    0x2849923Au,0x404ABEF2u,0xB2828A33u,0xDA81A6FBu,0x6284D3A3u,0x0A87FF6Bu,
    0x17624FE2u,0x7F61632Au,0xC7641672u,0xAF673ABAu,0xFCAF7760u,0x94AC5BA8u,
    0x2CA92EF0u,0x44AA0238u,0x594FB2B1u,0x314C9E79u,0x8949EB21u,0xE14AC7E9u,
    0x2ED97095u,0x46DA5C5Du,0xFEDF2905u,0x96DC05CDu,0x8B39B544u,0xE33A998Cu,
    0x5B3FECD4u,0x333CC01Cu,0x60F48DC6u,0x08F7A10Eu,0xB0F2D456u,0xD8F1F89Eu,
    0xC5144817u,0xAD1764DFu,0x15121187u,0x7D113D4Fu,
},
{
    0x00000000u,0x493C7D27u,0x9278FA4Eu,0xDB448769u,0x211D826Du,0x6821FF4Au,
    0xB3657823u,0xFA590504u,0x423B04DAu,0x0B0779FDu,0xD043FE94u,0x997F83B3u,
    0x632686B7u,0x2A1AFB90u,0xF15E7CF9u,0xB86201DEu,0x847609B4u,0xCD4A7493u,
    0x160EF3FAu,0x5F328EDDu,0xA56B8BD9u,0xEC57F6FEu,0x37137197u,0x7E2F0CB0u,
    0xC64D0D6Eu,0x8F717049u,0x5435F720u,0x1D098A07u,0xE7508F03u,0xAE6CF224u,
    0x7528754Du,0x3C14086Au,0x0D006599u,0x443C18BEu,0x9F789FD7u,0xD644E2F0u,
    0x2C1DE7F4u,0x65219AD3u,0xBE651DBAu,0xF759609Du,0x4F3B6143u,0x06071C64u,
    0xDD439B0Du,0x947FE62Au,0x6E26E32Eu,0x271A9E09u,0xFC5E1960u,0xB5626447u,
    0x89766C2Du,0xC04A110Au,0x1B0E9663u,0x5232EB44u,0xA86BEE40u,0xE1579367u,
    0x3A13140Eu,0x732F6929u,0xCB4D68F7u,0x827115D0u,0x593592B9u,0x1009EF9Eu,
    0xEA50EA9Au,0xA36C97BDu,0x782810D4u,0x31146DF3u,0x1A00CB32u,0x533CB615u,
    0x8878317Cu,0xC1444C5Bu,0x3B1D495Fu,0x72213478u,0xA965B311u,0xE059CE36u,
    0x583BCFE8u,0x1107B2CFu,0xCA4335A6u,0x837F4881u,0x79264D85u,0x301A30A2u,
    0xEB5EB7CBu,0xA262CAECu,0x9E76C286u,0xD74ABFA1u,0x0C0E38C8u,0x453245EFu,
    0xBF6B40EBu,0xF6573DCCu,0x2D13BAA5u,0x642FC782u,0xDC4DC65Cu,0x9571BB7Bu,
    0x4E353C12u,0x07094135u,0xFD504431u,0xB46C3916u,0x6F28BE7Fu,0x2614C358u,
    0x1700AEABu,0x5E3CD38Cu,0x857854E5u,0xCC4429C2u,0x361D2CC6u,0x7F2151E1u,
    0xA465D688u,0xED59ABAFu,0x553BAA71u,0x1C07D756u,0xC743503Fu,0x8E7F2D18u,
    0x7426281Cu,0x3D1A553Bu,0xE65ED252u,0xAF62AF75u,0x9376A71Fu,0xDA4ADA38u,
    0x010E5D51u,0x48322076u,0xB26B2572u,0xFB575855u,0x2013DF3Cu,0x692FA21Bu,
    0xD14DA3C5u,0x9871DEE2u,0x4335598Bu,0x0A0924ACu,0xF05021A8u,0xB96C5C8Fu,
    0x6228DBE6u,0x2B14A6C1u,0x34019664u,0x7D3DEB43u,0xA6796C2Au,0xEF45110Du,
    0x151C1409u,0x5C20692Eu,0x8764EE47u,0xCE589360u,0x763A92BEu,0x3F06EF99u,
    0xE44268F0u,0xAD7E15D7u,0x572710D3u,0x1E1B6DF4u,0xC55FEA9Du,0x8C6397BAu,
    0xB0779FD0u,0xF94BE2F7u,0x220F659Eu,0x6B3318B9u,0x916A1DBDu,0xD856609Au,
    0x0312E7F3u,0x4A2E9AD4u,0xF24C9B0Au,0xBB70E62Du,0x60346144u,0x29081C63u,
    0xD3511967u,0x9A6D6440u,0x4129E329u,0x08159E0Eu,0x3901F3FDu,0x703D8EDAu,
    0xAB7909B3u,0xE2457494u,0x181C7190u,0x51200CB7u,0x8A648BDEu,0xC358F6F9u,
    0x7B3AF727u,0x32068A00u,0xE9420D69u,0xA07E704Eu,0x5A27754Au,0x131B086Du,
    0xC85F8F04u,0x8163F223u,0xBD77FA49u,0xF44B876Eu,0x2F0F0007u,0x66337D20u,
    0x9C6A7824u,0xD5560503u,0x0E12826Au,0x472EFF4Du,0xFF4CFE93u,0xB67083B4u,
    0x6D3404DDu,0x240879FAu,0xDE517CFEu,0x976D01D9u,0x4C2986B0u,0x0515FB97u,
    0x2E015D56u,0x673D2071u,0xBC79A718u,0xF545DA3Fu,0x0F1CDF3Bu,0x4620A21Cu,
    0x9D642575u,0xD4585852u,0x6C3A598Cu,0x250624ABu,0xFE42A3C2u,0xB77EDEE5u,
    0x4D27DBE1u,0x041BA6C6u,0xDF5F21AFu,0x96635C88u,0xAA7754E2u,0xE34B29C5u,
    0x380FAEACu,0x7133D38Bu,0x8B6AD68Fu,0xC256ABA8u,0x19122CC1u,0x502E51E6u,
    0xE84C5038u,0xA1702D1Fu,0x7A34AA76u,0x3308D751u,0xC951D255u,0x806DAF72u,
    0x5B29281Bu,0x1215553Cu,0x230138CFu,0x6A3D45E8u,0xB179C281u,0xF845BFA6u,
    0x021CBAA2u,0x4B20C785u,0x906440ECu,0xD9583DCBu,0x613A3C15u,0x28064132u,
    0xF342C65Bu,0xBA7EBB7Cu,0x4027BE78u,0x091BC35Fu,0xD25F4436u,0x9B633911u,
    0xA777317Bu,0xEE4B4C5Cu,0x350FCB35u,0x7C33B612u,0x866AB316u,0xCF56CE31u,
    0x14124958u,0x5D2E347Fu,0xE54C35A1u,0xAC704886u,0x7734CFEFu,0x3E08B2C8u,
    0xC451B7CCu,0x8D6DCAEBu,0x56294D82u,0x1F1530A5u,
},
};

/* Slice-by-8: XOR eight input bytes into the register at once and index eight
 * tables in parallel. The byte-at-a-time loop is a serial dependency chain --
 * one table lookup cannot start until the previous crc is known. This one has
 * eight independent loads per iteration and the adder tree is the only
 * dependency. */
static uint32_t dyn_crc32_slice8(const uint8_t *data, size_t len,
                                 const uint32_t (*t)[256])
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len >= 8) {
        uint32_t lo, hi;
        memcpy(&lo, data, 4);
        memcpy(&hi, data + 4, 4);
        lo ^= crc;
        crc = t[7][ lo        & 0xFFu] ^ t[6][(lo >>  8) & 0xFFu] ^
              t[5][(lo >> 16) & 0xFFu] ^ t[4][(lo >> 24) & 0xFFu] ^
              t[3][ hi        & 0xFFu] ^ t[2][(hi >>  8) & 0xFFu] ^
              t[1][(hi >> 16) & 0xFFu] ^ t[0][(hi >> 24) & 0xFFu];
        data += 8;
        len -= 8;
    }
    while (len--)
        crc = t[0][(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- hardware CRC-32C ---------------------------------------------------
 *
 * SSE4.2's crc32 instruction implements the CASTAGNOLI polynomial and only
 * that one, so this path serves dyn_crc32c and cannot serve dyn_crc32.
 *
 * The instruction has 3-cycle latency and 1-per-cycle throughput, so a single
 * dependent chain runs at a third of the achievable rate. Three chains over
 * three thirds saturate it; the thirds are recombined by advancing each partial
 * CRC over the bytes that followed it, a multiplication by x^(8*n) in
 * GF(2)[x]/P. The powers of x are derived from the polynomial at first use, not
 * hard-coded, so there is no magic-constant table to get wrong. */
/* Both hardware paths need the same GF(2)[x] recombination, so it lives above
 * the ISA split rather than being written twice. */
#if (defined(__x86_64__) && defined(__GNUC__)) || \
    (defined(__aarch64__) && defined(__ARM_FEATURE_CRC32))
#define CRC32C_HW 1
#endif

#ifdef CRC32C_HW

static uint32_t crc32_gf_mul(uint32_t a, uint32_t b, uint32_t poly)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < 32; i++) {
        if (b & 0x80000000u) r ^= a;
        b <<= 1;
        a = (a >> 1) ^ (poly & (0u - (a & 1u)));
    }
    return r;
}

/* x^(2^i) mod P, for i = 0..63.
 *
 * This USED to be filled lazily on first use, justified by "every entry is a
 * pure function of the polynomial, so two threads racing to fill it write
 * identical bytes". Identical bytes or not, that is a data race on
 * crc32c_pow[] and on the ready flag, and dyn_crc32c is reachable from any JS
 * thread -- serialize() via dyn_ser_finish, the dyna:compress dictionary
 * id, and the crc32c binding itself. It went unnoticed because the whole block
 * was #if'd out on arm64 until the ARMv8 path was added, so the dev host never
 * built it.
 *
 * A compile-time table has no initialiser to race on (CLAUDE.md sec.12: a
 * lazily-initialised static is the thing to avoid, not a table). Regenerate
 * with tools/gen-crc32c-pow.c; test_crc32c_hw.c checks the values these
 * produce against the bit-serial definition, so a wrong entry cannot ship. */
static const uint32_t crc32c_pow[64] = {
    0x40000000u, 0x20000000u, 0x08000000u, 0x00800000u, 0x00008000u, 0x82F63B78u,
    0x6EA2D55Cu, 0x18B8EA18u, 0x510AC59Au, 0xB82BE955u, 0xB8FDB1E7u, 0x88E56F72u,
    0x74C360A4u, 0xE4172B16u, 0x0D65762Au, 0x35D73A62u, 0x28461564u, 0xBF455269u,
    0xE2EA32DCu, 0xFE7740E6u, 0xF946610Bu, 0x3C204F8Fu, 0x538586E3u, 0x59726915u,
    0x734D5309u, 0xBC1AC763u, 0x7D0722CCu, 0xD289CABEu, 0xE94CA9BCu, 0x05B74F3Fu,
    0xA51E1F42u, 0x40000000u, 0x20000000u, 0x08000000u, 0x00800000u, 0x00008000u,
    0x82F63B78u, 0x6EA2D55Cu, 0x18B8EA18u, 0x510AC59Au, 0xB82BE955u, 0xB8FDB1E7u,
    0x88E56F72u, 0x74C360A4u, 0xE4172B16u, 0x0D65762Au, 0x35D73A62u, 0x28461564u,
    0xBF455269u, 0xE2EA32DCu, 0xFE7740E6u, 0xF946610Bu, 0x3C204F8Fu, 0x538586E3u,
    0x59726915u, 0x734D5309u, 0xBC1AC763u, 0x7D0722CCu, 0xD289CABEu, 0xE94CA9BCu,
    0x05B74F3Fu, 0xA51E1F42u, 0x40000000u, 0x20000000u,
};


/* Same construction for the IEEE 802.3 polynomial. ARMv8's CRC32 extension
 * supplies BOTH polynomials from the same feature bit -- crc32b/h/w/x for this
 * one and crc32cb/... for Castagnoli -- so wiring only the second left the more
 * commonly used of the two on the table path. SSE4.2 supplies Castagnoli only,
 * which is why this kernel is arm64-only. */
static const uint32_t crc32_ieee_pow[64] = {
    0x40000000u, 0x20000000u, 0x08000000u, 0x00800000u, 0x00008000u, 0xEDB88320u,
    0xB1E6B092u, 0xA06A2517u, 0xED627DAEu, 0x88D14467u, 0xD7BBFE6Au, 0xEC447F11u,
    0x8E7EA170u, 0x6427800Eu, 0x4D47BAE0u, 0x09FE548Fu, 0x83852D0Fu, 0x30362F1Au,
    0x7B5A9CC3u, 0x31FEC169u, 0x9FEC022Au, 0x6C8DEDC4u, 0x15D6874Du, 0x5FDE7A4Eu,
    0xBAD90E37u, 0x2E4E5EEFu, 0x4EABA214u, 0xA8A472C0u, 0x429A969Eu, 0x148D302Au,
    0xC40BA6D0u, 0xC4E22C3Cu, 0x40000000u, 0x20000000u, 0x08000000u, 0x00800000u,
    0x00008000u, 0xEDB88320u, 0xB1E6B092u, 0xA06A2517u, 0xED627DAEu, 0x88D14467u,
    0xD7BBFE6Au, 0xEC447F11u, 0x8E7EA170u, 0x6427800Eu, 0x4D47BAE0u, 0x09FE548Fu,
    0x83852D0Fu, 0x30362F1Au, 0x7B5A9CC3u, 0x31FEC169u, 0x9FEC022Au, 0x6C8DEDC4u,
    0x15D6874Du, 0x5FDE7A4Eu, 0xBAD90E37u, 0x2E4E5EEFu, 0x4EABA214u, 0xA8A472C0u,
    0x429A969Eu, 0x148D302Au, 0xC40BA6D0u, 0xC4E22C3Cu,
};

static uint32_t crc32_shift(uint32_t crc, size_t bytes,
                            const uint32_t *pow, uint32_t poly)
{
    uint32_t r = 0x80000000u;
    uint64_t k = (uint64_t)bytes * 8;
    int i = 0;
    while (k) {
        if (k & 1) r = crc32_gf_mul(r, pow[i], poly);
        k >>= 1;
        i++;
    }
    return crc32_gf_mul(crc, r, poly);
}

/* The per-ISA primitives. Everything below the split is written once. */
#if defined(__x86_64__)
#include <nmmintrin.h>
#define CRC32C_U64(c, v) _mm_crc32_u64((c), (v))
#define CRC32C_U32(c, v) _mm_crc32_u32((uint32_t)(c), (v))
#define CRC32C_U16(c, v) _mm_crc32_u16((uint32_t)(c), (v))
#define CRC32C_U8(c, b)  _mm_crc32_u8((uint32_t)(c), (b))
#define CRC32C_TARGET    __attribute__((target("sse4.2")))
#else
/* ARMv8 CRC32 extension. crc32cx has the same 3-cycle latency / 1-per-cycle
 * throughput as SSE4.2's, so the same three-chain split applies unchanged. */
#include <arm_acle.h>
#define CRC32C_U64(c, v) __crc32cd((uint32_t)(c), (v))
#define CRC32C_U32(c, v) __crc32cw((uint32_t)(c), (v))
#define CRC32C_U16(c, v) __crc32ch((uint32_t)(c), (v))
#define CRC32C_U8(c, b)  __crc32cb((uint32_t)(c), (b))
#define CRC32C_TARGET
#endif

CRC32C_TARGET
static uint32_t crc32c_hw(const uint8_t *d, size_t n)
{
    uint64_t c0 = 0xFFFFFFFFu;
    if (n >= 3 * 64) {
        size_t third = (n / 3) & ~(size_t)7;
        const uint8_t *p0 = d, *p1 = d + third, *p2 = d + 2 * third;
        uint64_t c1 = 0, c2 = 0;
        size_t i;
        for (i = 0; i < third; i += 8) {
            uint64_t v0, v1, v2;
            memcpy(&v0, p0 + i, 8);
            memcpy(&v1, p1 + i, 8);
            memcpy(&v2, p2 + i, 8);
            c0 = CRC32C_U64(c0, v0);
            c1 = CRC32C_U64(c1, v1);
            c2 = CRC32C_U64(c2, v2);
        }
        c0 = crc32_shift((uint32_t)c0, third, crc32c_pow, DYN_CRC32C_POLY) ^ c1;
        c0 = crc32_shift((uint32_t)c0, third, crc32c_pow, DYN_CRC32C_POLY) ^ c2;
        d += 3 * third;
        n -= 3 * third;
    }
    while (n >= 8) { uint64_t v; memcpy(&v, d, 8); c0 = CRC32C_U64(c0, v); d += 8; n -= 8; }
    /* Ladder, not a byte loop. Each instruction is a 3-cycle link in a serial
     * dependency chain, so a 7-byte tail costs seven of them; 4/2/1 costs
     * three. This is why n=12 used to measure SLOWER than n=16 -- more
     * instructions on less data. The words must stay in stream order, so no
     * overlapping re-read is possible; only the width can change. */
    if (n >= 4) { uint32_t v; memcpy(&v, d, 4); c0 = CRC32C_U32(c0, v); d += 4; n -= 4; }
    if (n >= 2) { uint16_t v; memcpy(&v, d, 2); c0 = CRC32C_U16(c0, v); d += 2; n -= 2; }
    if (n)      c0 = CRC32C_U8(c0, *d);
    return (uint32_t)c0 ^ 0xFFFFFFFFu;
}

/* IEEE 802.3, arm64 only: SSE4.2's crc32 instruction is Castagnoli-only, so
 * there is no x86 counterpart to write. Same three-chain structure and the
 * same tail ladder as the Castagnoli kernel above; only the instruction and
 * the power table differ. */
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define DYN_CRC32_IEEE_HW 1

static uint32_t crc32_ieee_hw(const uint8_t *d, size_t n)
{
    uint64_t c0 = 0xFFFFFFFFu;
    if (n >= 3 * 64) {
        size_t third = (n / 3) & ~(size_t)7;
        const uint8_t *p0 = d, *p1 = d + third, *p2 = d + 2 * third;
        uint64_t c1 = 0, c2 = 0;
        size_t i;
        for (i = 0; i < third; i += 8) {
            uint64_t v0, v1, v2;
            memcpy(&v0, p0 + i, 8);
            memcpy(&v1, p1 + i, 8);
            memcpy(&v2, p2 + i, 8);
            c0 = __crc32d((uint32_t)c0, v0);
            c1 = __crc32d((uint32_t)c1, v1);
            c2 = __crc32d((uint32_t)c2, v2);
        }
        c0 = crc32_shift((uint32_t)c0, third, crc32_ieee_pow, DYN_CRC32_POLY) ^ c1;
        c0 = crc32_shift((uint32_t)c0, third, crc32_ieee_pow, DYN_CRC32_POLY) ^ c2;
        d += 3 * third;
        n -= 3 * third;
    }
    while (n >= 8) { uint64_t v; memcpy(&v, d, 8); c0 = __crc32d((uint32_t)c0, v); d += 8; n -= 8; }
    if (n >= 4) { uint32_t v; memcpy(&v, d, 4); c0 = __crc32w((uint32_t)c0, v); d += 4; n -= 4; }
    if (n >= 2) { uint16_t v; memcpy(&v, d, 2); c0 = __crc32h((uint32_t)c0, v); d += 2; n -= 2; }
    if (n)      c0 = __crc32b((uint32_t)c0, *d);
    return (uint32_t)c0 ^ 0xFFFFFFFFu;
}
#endif

#if defined(__x86_64__)
/* SSE4.2 is not in the x86-64 baseline ABI, so this one genuinely has to be a
 * runtime probe. _Atomic because dyn_crc32c runs on any JS thread and a plain
 * static write from two of them is a race even when both write the same value
 * (CLAUDE.md sec.6). Relaxed: the value is a pure function of the CPU, so any
 * order of the racing writes gives the same answer, and nothing is published
 * through it -- crc32c_pow is now a compile-time constant. */
static int crc32c_hw_ok(void)
{
    static _Atomic int state;   /* 0 unknown, 1 yes, -1 no */
    int s = atomic_load_explicit(&state, memory_order_relaxed);
    if (!s) {
        __builtin_cpu_init();
        s = __builtin_cpu_supports("sse4.2") ? 1 : -1;
        atomic_store_explicit(&state, s, memory_order_relaxed);
    }
    return s == 1;
}
#else
/* __ARM_FEATURE_CRC32 is a compile-time guarantee from the target: nothing to
 * probe, so no state and no race. */
static int crc32c_hw_ok(void) { return 1; }
#endif
#else
static int crc32c_hw_ok(void) { return 0; }
static uint32_t crc32c_hw(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }
#endif

#ifndef DYN_CRC32C_HW_MIN
#define DYN_CRC32C_HW_MIN 8
#endif

/* Generic form, for a caller with a polynomial not known at compile time. */
uint32_t dyn_crc32_poly(const uint8_t *data, size_t len, uint32_t poly)
{
    /* A caller passing one of the two published constants gets the same answer
     * either way, so there is no reason to hand it the bit-serial loop. */
    if (poly == DYN_CRC32_POLY)
        return dyn_crc32_slice8(data, len, crc32_ieee_s8);
    if (poly == DYN_CRC32C_POLY)
        return dyn_crc32c(data, len);
    return dyn_crc32_bitwise(data, len, poly);
}

/* ==================================================================== *
 *  reuse + one-shot convenience over the streaming core                 *
 * ==================================================================== */

const dyn_hash_algo_t *dyn_hash_algo_by_id(dyn_hash_algo_id id)
{
    if ((unsigned)id >= (unsigned)DYN_HASH_ALGO_COUNT)
        return NULL;
    return &dyn_hash_algos[id];
}

void dyn_hash_reset(dyn_hash_ctx_t *c)
{
    dyn_hash_init(c, c->algo);
}

void dyn_hash_oneshot(const dyn_hash_algo_t *a, const uint8_t *data, size_t len,
                      uint8_t *out)
{
    dyn_hash_ctx_t c;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, out);
}

void dyn_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    dyn_hash_oneshot(&dyn_hash_algos[DYN_HASH_MD5], data, len, out);
}

void dyn_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    dyn_hash_oneshot(&dyn_hash_algos[DYN_HASH_SHA1], data, len, out);
}

void dyn_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    dyn_hash_oneshot(&dyn_hash_algos[DYN_HASH_SHA256], data, len, out);
}

void dyn_hmac(const dyn_hash_algo_t *a, const uint8_t *key, size_t keylen,
              const uint8_t *msg, size_t msglen, uint8_t *out)
{
    uint8_t k0[DYN_HASH_MAX_BLOCK];
    memset(k0, 0, sizeof(k0)); /* dyn_hmac_key0 requires a pre-zeroed k0 */
    dyn_hmac_key0(a, key, keylen, k0);
    dyn_hmac_finish(a, k0, msg, msglen, out);
}

uint32_t dyn_crc32(const uint8_t *data, size_t len)
{
#if defined(DYN_CRC32_IEEE_HW)
    /* Same gate and the same reason as dyn_crc32c: below one 64-bit step the
     * instruction is a pure dependent chain and the table wins. */
    if (len >= DYN_CRC32C_HW_MIN)
        return crc32_ieee_hw(data, len);
#endif
    return dyn_crc32_slice8(data, len, crc32_ieee_s8);
}

uint32_t dyn_crc32c(const uint8_t *data, size_t len)
{
    /* Below this length the hardware chain loses to a table lookup: the
     * instruction has 3-cycle latency and a short input is a pure dependent
     * chain with no room for the three-way split to hide it. Measured, not
     * guessed: 8 is where the path gets its first 64-bit step, and below it
     * crc32c_hw runs a pure byte chain with no room to hide the latency.
     * Measured either side -- n=4 loses 0.84x to the table, n=8 wins 1.17x.
     * Build with -DDYN_CRC32C_HW_MIN=0 for the threshold-0 control. */
    if (len >= DYN_CRC32C_HW_MIN && crc32c_hw_ok())
        return crc32c_hw(data, len);
    return dyn_crc32_slice8(data, len, crc32c_s8);
}

/* ==================================================================== *
 *  XXH64 -- xxHash spec v0.7.3, little-endian by definition
 * ==================================================================== */

#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

static uint64_t xxh_rotl(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

/* The format defines its lanes as little-endian, so they are assembled byte by
 * byte rather than loaded: a big-endian host must produce the same digest. */
static uint64_t xxh_read64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static uint32_t xxh_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t xxh_round(uint64_t acc, uint64_t input)
{
    acc += input * XXH_P2;
    acc = xxh_rotl(acc, 31);
    acc *= XXH_P1;
    return acc;
}

static uint64_t xxh_merge(uint64_t acc, uint64_t val)
{
    acc ^= xxh_round(0, val);
    return acc * XXH_P1 + XXH_P4;
}

uint64_t dyn_xxh64(const uint8_t *data, size_t len, uint64_t seed)
{
    const uint8_t *p = data, *end = data + len;
    uint64_t h;

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + XXH_P1 + XXH_P2;
        uint64_t v2 = seed + XXH_P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - XXH_P1;
        do {
            v1 = xxh_round(v1, xxh_read64(p));      p += 8;
            v2 = xxh_round(v2, xxh_read64(p));      p += 8;
            v3 = xxh_round(v3, xxh_read64(p));      p += 8;
            v4 = xxh_round(v4, xxh_read64(p));      p += 8;
        } while (p <= limit);
        h = xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) +
            xxh_rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + XXH_P5;
    }
    h += (uint64_t)len;

    while (end - p >= 8) {
        h ^= xxh_round(0, xxh_read64(p));
        h = xxh_rotl(h, 27) * XXH_P1 + XXH_P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)xxh_read32(p) * XXH_P1;
        h = xxh_rotl(h, 23) * XXH_P2 + XXH_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)*p * XXH_P5;
        h = xxh_rotl(h, 11) * XXH_P1;
        p++;
    }

    h ^= h >> 33;
    h *= XXH_P2;
    h ^= h >> 29;
    h *= XXH_P3;
    h ^= h >> 32;
    return h;
}

/* ---- streaming HMAC, key derivation, constant-time comparison ---------- */

void dyn_hmac_init(dyn_hmac_ctx_t *c, const dyn_hash_algo_t *a,
                   const uint8_t *key, size_t keylen)
{
    uint8_t k0[DYN_HASH_MAX_BLOCK], pad[DYN_HASH_MAX_BLOCK];
    unsigned i, bs = a->block_size;

    c->algo = a;
    memset(k0, 0, sizeof(k0));
    dyn_hmac_key0(a, key, keylen, k0);

    for (i = 0; i < bs; i++)
        pad[i] = k0[i] ^ 0x36;
    a->init(&c->istate);
    a->compress(&c->istate, pad);

    for (i = 0; i < bs; i++)
        pad[i] = k0[i] ^ 0x5c;
    a->init(&c->ostate);
    a->compress(&c->ostate, pad);

    /* k0 and both pads die here; nothing derived from the key survives except
     * the two chaining states. */
    memset(k0, 0, sizeof(k0));
    memset(pad, 0, sizeof(pad));

    dyn_hmac_reset(c);
}

void dyn_hmac_update(dyn_hmac_ctx_t *c, const uint8_t *data, size_t len)
{
    dyn_hash_update(&c->h, data, len);
}

void dyn_hmac_final(dyn_hmac_ctx_t *c, uint8_t *out)
{
    uint8_t inner[DYN_HASH_MAX_DIGEST];
    const dyn_hash_algo_t *a = c->algo;

    dyn_hash_final(&c->h, inner);

    /* Resume from the pre-absorbed opad state instead of replaying the block. */
    c->h.algo = a;
    c->h.st = c->ostate;
    c->h.buflen = 0;
    c->h.bytelen = a->block_size;
    dyn_hash_update(&c->h, inner, a->digest_size);
    dyn_hash_final(&c->h, out);

    dyn_hmac_reset(c);          /* finalise AND reset: the object stays usable */
}

void dyn_hmac_reset(dyn_hmac_ctx_t *c)
{
    c->h.algo = c->algo;
    c->h.st = c->istate;
    c->h.buflen = 0;
    c->h.bytelen = c->algo->block_size;
}

int dyn_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    /* `volatile` on the accumulator stops the compiler proving that a non-zero
     * difference can never become zero and inserting an early exit -- which is
     * the exact failure this function exists to prevent. Every byte is read for
     * any input, so the running time depends only on the length. */
    unsigned long diff = 0;
    size_t i;
    for (i = 0; i < len; i++)
        diff |= (unsigned long)(a[i] ^ b[i]);
    /* The accumulator stays in a register; the barrier is what stops the
     * compiler reasoning about its value and branching early. `volatile` on
     * the accumulator bought the same guarantee at the price of a store and a
     * reload per byte. */
    __asm__ __volatile__("" : "+r"(diff) : : "memory");
    return diff == 0;
}

int dyn_hkdf(const dyn_hash_algo_t *a, const uint8_t *ikm, size_t ikm_len,
             const uint8_t *salt, size_t salt_len,
             const uint8_t *info, size_t info_len,
             uint8_t *out, size_t out_len)
{
    uint8_t prk[DYN_HASH_MAX_DIGEST], t[DYN_HASH_MAX_DIGEST];
    uint8_t zero_salt[DYN_HASH_MAX_DIGEST];
    dyn_hmac_ctx_t hc;
    size_t hl, done = 0;
    unsigned counter = 1;

    if (!a)
        return -1;
    hl = a->digest_size;
    if (out_len > 255 * hl)
        return -1;
    if (!salt || salt_len == 0) {
        memset(zero_salt, 0, hl);        /* RFC 5869: a zero salt of hash length */
        salt = zero_salt;
        salt_len = hl;
    }
    dyn_hmac(a, salt, salt_len, ikm, ikm_len, prk);          /* extract */
    dyn_hmac_init(&hc, a, prk, hl);
    while (done < out_len) {                                  /* expand */
        uint8_t cb = (uint8_t)counter;
        size_t take;
        if (counter > 1)
            dyn_hmac_update(&hc, t, hl);
        if (info && info_len)
            dyn_hmac_update(&hc, info, info_len);
        dyn_hmac_update(&hc, &cb, 1);
        dyn_hmac_final(&hc, t);           /* finalises and resets for the next T */
        take = out_len - done < hl ? out_len - done : hl;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    return 0;
}

int dyn_pbkdf2(const dyn_hash_algo_t *a, const uint8_t *pw, size_t pw_len,
               const uint8_t *salt, size_t salt_len, uint32_t iters,
               uint8_t *out, size_t out_len)
{
    uint8_t u[DYN_HASH_MAX_DIGEST], acc[DYN_HASH_MAX_DIGEST];
    dyn_hmac_ctx_t hc;
    size_t hl, done = 0;
    uint32_t block = 1;

    if (!a || iters < 1)
        return -1;
    hl = a->digest_size;
    /* The key schedule depends only on the password, so it is derived ONCE and
     * the context reset per iteration. Re-deriving it inside the loop is most
     * of what makes a straight transcription of RFC 8018 slow, and it buys
     * nothing. */
    dyn_hmac_init(&hc, a, pw, pw_len);
    while (done < out_len) {
        uint8_t be[4];
        uint32_t it;
        size_t i, take;
        be[0] = (uint8_t)(block >> 24); be[1] = (uint8_t)(block >> 16);
        be[2] = (uint8_t)(block >> 8);  be[3] = (uint8_t)block;
        if (salt && salt_len)
            dyn_hmac_update(&hc, salt, salt_len);
        dyn_hmac_update(&hc, be, 4);
        dyn_hmac_final(&hc, u);
        memcpy(acc, u, hl);
        for (it = 1; it < iters; it++) {
            dyn_hmac_update(&hc, u, hl);
            dyn_hmac_final(&hc, u);
            for (i = 0; i < hl; i++)
                acc[i] ^= u[i];
        }
        take = out_len - done < hl ? out_len - done : hl;
        memcpy(out + done, acc, take);
        done += take;
        block++;
    }
    return 0;
}

/* ---- XXH32 (xxHash spec v0.7.3) ----
 *
 * A separate function rather than a truncation of XXH64: the two produce
 * different values, and this one exists because the LZ4 frame format specifies
 * it by name for the header and content checksums. Interop with the `lz4` tool
 * is the whole point, so the constants are the published ones. */
#define XXH32_P1 2654435761u
#define XXH32_P2 2246822519u
#define XXH32_P3 3266489917u
#define XXH32_P4 668265263u
#define XXH32_P5 374761393u

static uint32_t xxh32_rotl(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

static uint32_t xxh32_round(uint32_t acc, uint32_t input)
{
    acc += input * XXH32_P2;
    acc = xxh32_rotl(acc, 13);
    return acc * XXH32_P1;
}

uint32_t dyn_xxh32(const uint8_t *data, size_t len, uint32_t seed)
{
    const uint8_t *p = data, *end = data + len;
    uint32_t h;

    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + XXH32_P1 + XXH32_P2;
        uint32_t v2 = seed + XXH32_P2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XXH32_P1;
        do {
            v1 = xxh32_round(v1, xxh_read32(p)); p += 4;
            v2 = xxh32_round(v2, xxh_read32(p)); p += 4;
            v3 = xxh32_round(v3, xxh_read32(p)); p += 4;
            v4 = xxh32_round(v4, xxh_read32(p)); p += 4;
        } while (p <= limit);
        h = xxh32_rotl(v1, 1) + xxh32_rotl(v2, 7) + xxh32_rotl(v3, 12) +
            xxh32_rotl(v4, 18);
    } else {
        h = seed + XXH32_P5;
    }
    h += (uint32_t)len;

    while (end - p >= 4) {
        h += xxh_read32(p) * XXH32_P3;
        h = xxh32_rotl(h, 17) * XXH32_P4;
        p += 4;
    }
    while (p < end) {
        h += (uint32_t)*p * XXH32_P5;
        h = xxh32_rotl(h, 11) * XXH32_P1;
        p++;
    }

    h ^= h >> 15;
    h *= XXH32_P2;
    h ^= h >> 13;
    h *= XXH32_P3;
    h ^= h >> 16;
    return h;
}

uint64_t dyn_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}
