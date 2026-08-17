/* BLAKE3, BLAKE2b/2s and Murmur3-128 for dyna:hash (design 26).
   BLAKE3 is a Merkle tree of 1 KiB chunks, not a serial hash: the chunk
   counter is part of every compression, so a chunk hashed at the wrong index
   gives a clean-looking wrong digest. Full API: see the module header. */

#define BL3_CHUNK_START         1u
#define BL3_CHUNK_END           2u
#define BL3_PARENT              4u
#define BL3_ROOT                8u
#define BL3_BLOCK               64
#define BL3_CHUNK               1024

static const uint32_t BL3_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint8_t BL3_PERM[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

static uint32_t bl3_rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t bl3_load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bl3_store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void bl3_g(uint32_t *s, int a, int b, int c, int d,
                  uint32_t mx, uint32_t my)
{
    s[a] = s[a] + s[b] + mx;
    s[d] = bl3_rotr(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = bl3_rotr(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + my;
    s[d] = bl3_rotr(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = bl3_rotr(s[b] ^ s[c], 7);
}

static void bl3_round(uint32_t *s, const uint32_t *m)
{
    bl3_g(s, 0, 4,  8, 12, m[0],  m[1]);
    bl3_g(s, 1, 5,  9, 13, m[2],  m[3]);
    bl3_g(s, 2, 6, 10, 14, m[4],  m[5]);
    bl3_g(s, 3, 7, 11, 15, m[6],  m[7]);
    bl3_g(s, 0, 5, 10, 15, m[8],  m[9]);
    bl3_g(s, 1, 6, 11, 12, m[10], m[11]);
    bl3_g(s, 2, 7,  8, 13, m[12], m[13]);
    bl3_g(s, 3, 4,  9, 14, m[14], m[15]);
}

/* The full 16-word state: the low half is the chaining value, the high half is
   what an extendable output reads. */
static void bl3_compress(const uint32_t cv[8], const uint8_t block[64],
                         uint64_t counter, uint32_t block_len, uint32_t flags,
                         uint32_t out[16])
{
    uint32_t m[16], s[16];
    int i, r;

    for (i = 0; i < 16; i++)
        m[i] = bl3_load32(block + i * 4);
    for (i = 0; i < 8; i++)
        s[i] = cv[i];
    s[8]  = BL3_IV[0]; s[9]  = BL3_IV[1];
    s[10] = BL3_IV[2]; s[11] = BL3_IV[3];
    s[12] = (uint32_t)counter;
    s[13] = (uint32_t)(counter >> 32);
    s[14] = block_len;
    s[15] = flags;
    for (r = 0; r < 7; r++) {
        bl3_round(s, m);
        if (r < 6) {
            uint32_t p[16];
            for (i = 0; i < 16; i++)
                p[i] = m[BL3_PERM[i]];
            memcpy(m, p, sizeof m);
        }
    }
    for (i = 0; i < 8; i++) {
        s[i] ^= s[i + 8];
        s[i + 8] ^= cv[i];
    }
    memcpy(out, s, sizeof s);
}

/* A node whose root bytes are not yet demanded: kept unfinalised because the
   ROOT flag is only legal on the last node, and only once. */
typedef struct {
    uint32_t cv[8];
    uint8_t  block[BL3_BLOCK];
    uint32_t block_len, flags;
    uint64_t counter;
} bl3_out_t;

static void bl3_out_cv(const bl3_out_t *o, uint32_t cv[8])
{
    uint32_t s[16];
    bl3_compress(o->cv, o->block, o->counter, o->block_len, o->flags, s);
    memcpy(cv, s, 8 * sizeof(uint32_t));
}

static void bl3_out_root(const bl3_out_t *o, uint8_t *out, size_t out_len)
{
    uint64_t blk = 0;
    size_t i;

    while (out_len) {
        uint32_t s[16];
        uint8_t wide[64];
        size_t take = out_len < 64 ? out_len : 64;
        bl3_compress(o->cv, o->block, blk, o->block_len,
                     o->flags | BL3_ROOT, s);
        for (i = 0; i < 16; i++)
            bl3_store32(wide + i * 4, s[i]);
        memcpy(out, wide, take);
        out += take;
        out_len -= take;
        blk++;
    }
}

typedef struct {
    uint32_t cv[8];
    uint64_t chunk_counter;
    uint8_t  buf[BL3_BLOCK];
    uint32_t buf_len, blocks, flags;
} bl3_chunk_t;

static void bl3_chunk_init(bl3_chunk_t *c, const uint32_t key[8],
                           uint64_t counter, uint32_t flags)
{
    memcpy(c->cv, key, 8 * sizeof(uint32_t));
    c->chunk_counter = counter;
    memset(c->buf, 0, BL3_BLOCK);       /* compress reads all 64 bytes: the
                                           tail past block_len must be zero */
    c->buf_len = 0;
    c->blocks = 0;
    c->flags = flags;
}

static uint32_t bl3_chunk_start(const bl3_chunk_t *c)
{
    return c->blocks == 0 ? BL3_CHUNK_START : 0;
}

static void bl3_chunk_update(bl3_chunk_t *c, const uint8_t *p, size_t n)
{
    while (n) {
        size_t take;
        if (c->buf_len == BL3_BLOCK) {      /* full, and more follows: not END */
            uint32_t s[16];
            bl3_compress(c->cv, c->buf, c->chunk_counter, BL3_BLOCK,
                         c->flags | bl3_chunk_start(c), s);
            memcpy(c->cv, s, 8 * sizeof(uint32_t));
            c->blocks++;
            memset(c->buf, 0, BL3_BLOCK);
            c->buf_len = 0;
        }
        take = (size_t)(BL3_BLOCK - c->buf_len);
        if (take > n)
            take = n;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += (uint32_t)take;
        p += take;
        n -= take;
    }
}

static void bl3_chunk_out(const bl3_chunk_t *c, bl3_out_t *o)
{
    memcpy(o->cv, c->cv, 8 * sizeof(uint32_t));
    memcpy(o->block, c->buf, BL3_BLOCK);
    o->block_len = c->buf_len;
    o->counter = c->chunk_counter;
    o->flags = c->flags | bl3_chunk_start(c) | BL3_CHUNK_END;
}

static void bl3_parent_out(const uint32_t left[8], const uint32_t right[8],
                           const uint32_t key[8], uint32_t flags, bl3_out_t *o)
{
    memcpy(o->block, left, 8 * sizeof(uint32_t));
    memcpy(o->block + 32, right, 8 * sizeof(uint32_t));
    memcpy(o->cv, key, 8 * sizeof(uint32_t));
    o->block_len = BL3_BLOCK;
    o->counter = 0;                     /* a parent has no chunk index */
    o->flags = flags | BL3_PARENT;
}

/* ---- BL3_LANES independent chunks, one per lane ----
   Single-chunk SIMD cannot win: the schedule permutes ACROSS lanes, costing 112
   lane inserts per compression (measured 2x slower). Chunks are independent, so
   in this layout the message words load as vectors and nothing crosses a lane. */

#if !defined(BL3_NO_SIMD) && \
    (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  if defined(__aarch64__) || defined(_M_ARM64)
#    include <arm_neon.h>
#    define BL3_LANES 4
typedef uint32x4_t bl3_vec;
#    define bl3_v_add(a, b)  vaddq_u32((a), (b))
#    define bl3_v_xor(a, b)  veorq_u32((a), (b))
#    define bl3_v_set1(x)    vdupq_n_u32((uint32_t)(x))
#    define bl3_v_loadu(p)   vreinterpretq_u32_u8(vld1q_u8((const uint8_t *)(p)))
#    define bl3_v_store(p, v) vst1q_u32((uint32_t *)(p), (v))
/* SRI keeps the destination's top n bits, so shifting left by 32-n first makes
   this a rotate in two instructions rather than shift/shift/or. */
#    define BL3_ROT(x, n)    vsriq_n_u32(vshlq_n_u32((x), 32 - (n)), (x), (n))
#    define BL3_TRANSPOSE(a0, a1, a2, a3, r0, r1, r2, r3) do {               \
        uint32x4_t t0_ = vtrn1q_u32((a0), (a1)), t1_ = vtrn2q_u32((a0), (a1)); \
        uint32x4_t t2_ = vtrn1q_u32((a2), (a3)), t3_ = vtrn2q_u32((a2), (a3)); \
        uint64x2_t u0_ = vreinterpretq_u64_u32(t0_), u1_ = vreinterpretq_u64_u32(t1_); \
        uint64x2_t u2_ = vreinterpretq_u64_u32(t2_), u3_ = vreinterpretq_u64_u32(t3_); \
        (r0) = vreinterpretq_u32_u64(vtrn1q_u64(u0_, u2_));                  \
        (r1) = vreinterpretq_u32_u64(vtrn1q_u64(u1_, u3_));                  \
        (r2) = vreinterpretq_u32_u64(vtrn2q_u64(u0_, u2_));                  \
        (r3) = vreinterpretq_u32_u64(vtrn2q_u64(u1_, u3_));                  \
    } while (0)
#  elif defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#    include <emmintrin.h>
#    define BL3_LANES 4
typedef __m128i bl3_vec;
#    define bl3_v_add(a, b)  _mm_add_epi32((a), (b))
#    define bl3_v_xor(a, b)  _mm_xor_si128((a), (b))
#    define bl3_v_set1(x)    _mm_set1_epi32((int)(uint32_t)(x))
#    define bl3_v_loadu(p)   _mm_loadu_si128((const __m128i *)(const void *)(p))
#    define bl3_v_store(p, v) _mm_storeu_si128((__m128i *)(void *)(p), (v))
#    define BL3_ROT(x, n)    _mm_or_si128(_mm_srli_epi32((x), (n)),          \
                                          _mm_slli_epi32((x), 32 - (n)))
#    define BL3_TRANSPOSE(a0, a1, a2, a3, r0, r1, r2, r3) do {               \
        __m128i t0_ = _mm_unpacklo_epi32((a0), (a1));                        \
        __m128i t1_ = _mm_unpackhi_epi32((a0), (a1));                        \
        __m128i t2_ = _mm_unpacklo_epi32((a2), (a3));                        \
        __m128i t3_ = _mm_unpackhi_epi32((a2), (a3));                        \
        (r0) = _mm_unpacklo_epi64(t0_, t2_);                                 \
        (r1) = _mm_unpackhi_epi64(t0_, t2_);                                 \
        (r2) = _mm_unpacklo_epi64(t1_, t3_);                                 \
        (r3) = _mm_unpackhi_epi64(t1_, t3_);                                 \
    } while (0)
#  endif
#endif

#ifdef BL3_LANES

#define BL3_G(a, b, c, d, mx, my) do {                                       \
    (a) = bl3_v_add(bl3_v_add((a), (b)), (mx));                              \
    (d) = BL3_ROT(bl3_v_xor((d), (a)), 16);                                  \
    (c) = bl3_v_add((c), (d));                                               \
    (b) = BL3_ROT(bl3_v_xor((b), (c)), 12);                                  \
    (a) = bl3_v_add(bl3_v_add((a), (b)), (my));                              \
    (d) = BL3_ROT(bl3_v_xor((d), (a)), 8);                                   \
    (c) = bl3_v_add((c), (d));                                               \
    (b) = BL3_ROT(bl3_v_xor((b), (c)), 7);                                   \
} while (0)

/* The schedule as literal indices: a runtime table made every message word a
   dependent load and measured 763 -> 593 MiB/s on the scalar path. */
#define BL3_ROUND(v, m, s0, s1, s2, s3, s4, s5, s6, s7,                      \
                        s8, s9, sa, sb, sc, sd, se, sf) do {                 \
    BL3_G((v)[0], (v)[4], (v)[8],  (v)[12], (m)[s0], (m)[s1]);               \
    BL3_G((v)[1], (v)[5], (v)[9],  (v)[13], (m)[s2], (m)[s3]);               \
    BL3_G((v)[2], (v)[6], (v)[10], (v)[14], (m)[s4], (m)[s5]);               \
    BL3_G((v)[3], (v)[7], (v)[11], (v)[15], (m)[s6], (m)[s7]);               \
    BL3_G((v)[0], (v)[5], (v)[10], (v)[15], (m)[s8], (m)[s9]);               \
    BL3_G((v)[1], (v)[6], (v)[11], (v)[12], (m)[sa], (m)[sb]);               \
    BL3_G((v)[2], (v)[7], (v)[8],  (v)[13], (m)[sc], (m)[sd]);               \
    BL3_G((v)[3], (v)[4], (v)[9],  (v)[14], (m)[se], (m)[sf]);               \
} while (0)

#define BL3_ROUNDS(v, m) do {                                                \
    BL3_ROUND(v, m,  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15);        \
    BL3_ROUND(v, m,  2, 6, 3,10, 7, 0, 4,13, 1,11,12, 5, 9,14,15, 8);        \
    BL3_ROUND(v, m,  3, 4,10,12,13, 2, 7,14, 6, 5, 9, 0,11,15, 8, 1);        \
    BL3_ROUND(v, m, 10, 7,12, 9,14, 3,13,15, 4, 0,11, 2, 5, 8, 1, 6);        \
    BL3_ROUND(v, m, 12,13, 9,11,15,10,14, 8, 7, 2, 5, 3, 0, 1, 6, 4);        \
    BL3_ROUND(v, m,  9,14,11, 5, 8,12,15, 1,13, 3, 0,10, 2, 6, 4, 7);        \
    BL3_ROUND(v, m, 11,15, 5, 0, 1, 9, 8, 6,14,10, 2,12, 3, 4, 7,13);        \
} while (0)

#define BL3_CHUNK_BLOCKS (BL3_CHUNK / BL3_BLOCK)

/* BL3_LANES complete chunks, contiguous at p. Every lane runs the same block
   index, so START/END flags and block_len are uniform; only the counter differs. */
static void bl3_chunks_wide(const uint8_t *p, uint64_t counter, uint32_t flags,
                            const uint32_t key[8], uint32_t out[][8])
{
    bl3_vec h[8], ctr_lo, ctr_hi;
    uint32_t lo[BL3_LANES], hi[BL3_LANES], flat[8][BL3_LANES];
    int i, b, l;

    for (i = 0; i < 8; i++)
        h[i] = bl3_v_set1(key[i]);
    for (l = 0; l < BL3_LANES; l++) {
        lo[l] = (uint32_t)(counter + (uint64_t)l);
        hi[l] = (uint32_t)((counter + (uint64_t)l) >> 32);
    }
    ctr_lo = bl3_v_loadu(lo);
    ctr_hi = bl3_v_loadu(hi);

    for (b = 0; b < BL3_CHUNK_BLOCKS; b++) {
        bl3_vec m[16], v[16];
        uint32_t bf = flags;

        if (b == 0)
            bf |= BL3_CHUNK_START;
        if (b == BL3_CHUNK_BLOCKS - 1)
            bf |= BL3_CHUNK_END;

        for (i = 0; i < 16; i += 4) {
            const uint8_t *q = p + (size_t)b * BL3_BLOCK + (size_t)i * 4;
            bl3_vec a0 = bl3_v_loadu(q);
            bl3_vec a1 = bl3_v_loadu(q + BL3_CHUNK);
            bl3_vec a2 = bl3_v_loadu(q + 2 * BL3_CHUNK);
            bl3_vec a3 = bl3_v_loadu(q + 3 * BL3_CHUNK);
            BL3_TRANSPOSE(a0, a1, a2, a3, m[i], m[i + 1], m[i + 2], m[i + 3]);
        }

        for (i = 0; i < 8; i++)
            v[i] = h[i];
        v[8]  = bl3_v_set1(BL3_IV[0]); v[9]  = bl3_v_set1(BL3_IV[1]);
        v[10] = bl3_v_set1(BL3_IV[2]); v[11] = bl3_v_set1(BL3_IV[3]);
        v[12] = ctr_lo;                v[13] = ctr_hi;
        v[14] = bl3_v_set1(BL3_BLOCK); v[15] = bl3_v_set1(bf);

        BL3_ROUNDS(v, m);

        for (i = 0; i < 8; i++)
            h[i] = bl3_v_xor(v[i], v[i + 8]);
    }

    for (i = 0; i < 8; i++)
        bl3_v_store(flat[i], h[i]);
    for (l = 0; l < BL3_LANES; l++)
        for (i = 0; i < 8; i++)
            out[l][i] = flat[i][l];
}
#endif  /* BL3_LANES */

typedef struct {
    bl3_chunk_t chunk;
    uint32_t    key[8];
    uint32_t    stack[54][8];
    size_t      stack_len;
    uint32_t    flags;
} bl3_hasher_t;

static void bl3_init(bl3_hasher_t *h)
{
    memcpy(h->key, BL3_IV, sizeof h->key);
    bl3_chunk_init(&h->chunk, h->key, 0, 0);
    h->stack_len = 0;
    h->flags = 0;
}

/* The stack depth after N complete chunks is popcount(N): every 1 bit is one
   subtree waiting for a sibling. Merging down to that depth builds the
   left-balanced tree, and the left subtree is always the larger power of two. */
static void bl3_merge(bl3_hasher_t *h, uint64_t total_chunks)
{
    size_t want = 0;
    uint64_t t = total_chunks;

    while (t) { want += (size_t)(t & 1); t >>= 1; }
    while (h->stack_len > want) {
        bl3_out_t o;
        uint32_t cv[8];
        bl3_parent_out(h->stack[h->stack_len - 2], h->stack[h->stack_len - 1],
                       h->key, h->flags, &o);
        bl3_out_cv(&o, cv);
        h->stack_len -= 2;
        memcpy(h->stack[h->stack_len], cv, sizeof cv);
        h->stack_len++;
    }
}

static void bl3_update(bl3_hasher_t *h, const uint8_t *p, size_t n)
{
    while (n) {
        size_t have = (size_t)h->chunk.blocks * BL3_BLOCK + h->chunk.buf_len;
        size_t take;
#ifdef BL3_LANES
        /* Strictly more than a batch left, so the final chunk always reaches the
           serial path: bl3_final needs a current chunk it can still flag ROOT. */
        if (have == 0 && n > (size_t)BL3_LANES * BL3_CHUNK) {
            uint32_t cvs[BL3_LANES][8];
            uint64_t base = h->chunk.chunk_counter;
            int l;
            bl3_chunks_wide(p, base, h->flags, h->key, cvs);
            for (l = 0; l < BL3_LANES; l++) {
                memcpy(h->stack[h->stack_len], cvs[l], sizeof cvs[l]);
                h->stack_len++;
                bl3_merge(h, base + (uint64_t)l + 1);
            }
            p += (size_t)BL3_LANES * BL3_CHUNK;
            n -= (size_t)BL3_LANES * BL3_CHUNK;
            bl3_chunk_init(&h->chunk, h->key, base + BL3_LANES, h->flags);
            continue;
        }
#endif
        if (have == BL3_CHUNK) {        /* chunk complete: fold it into the tree */
            bl3_out_t o;
            uint32_t cv[8];
            uint64_t done = h->chunk.chunk_counter + 1;
            bl3_chunk_out(&h->chunk, &o);
            bl3_out_cv(&o, cv);
            memcpy(h->stack[h->stack_len], cv, sizeof cv);
            h->stack_len++;
            bl3_merge(h, done);      /* AFTER the push: merging first leaves the
                                        first two chunks unpaired */
            bl3_chunk_init(&h->chunk, h->key, done, h->flags);
            have = 0;
        }
        take = BL3_CHUNK - have;
        if (take > n)
            take = n;
        bl3_chunk_update(&h->chunk, p, take);
        p += take;
        n -= take;
    }
}

static void bl3_final(const bl3_hasher_t *h, uint8_t *out, size_t out_len)
{
    bl3_out_t o;
    size_t left = h->stack_len;

    bl3_chunk_out(&h->chunk, &o);
    while (left) {
        uint32_t cv[8];
        bl3_out_t parent;
        bl3_out_cv(&o, cv);
        left--;
        bl3_parent_out(h->stack[left], cv, h->key, h->flags, &parent);
        o = parent;
    }
    bl3_out_root(&o, out, out_len);
}

/* ---- BLAKE2b and BLAKE2s (RFC 7693) ---- */

static const uint8_t BL2_SIGMA[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

static const uint64_t BL2B_IV[8] = {
    0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL,
    0xA54FF53A5F1D36F1ULL, 0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
    0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL,
};

static uint64_t bl2_rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static void bl2b_g(uint64_t *v, int a, int b, int c, int d,
                   uint64_t x, uint64_t y)
{
    v[a] = v[a] + v[b] + x; v[d] = bl2_rotr64(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];     v[b] = bl2_rotr64(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y; v[d] = bl2_rotr64(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];     v[b] = bl2_rotr64(v[b] ^ v[c], 63);
}

static void bl2b_compress(uint64_t h[8], const uint8_t block[128],
                          uint64_t t, int last)
{
    uint64_t v[16], m[16];
    int i, r;

    for (i = 0; i < 16; i++) {
        int k;
        uint64_t w = 0;
        for (k = 0; k < 8; k++)
            w |= (uint64_t)block[i * 8 + k] << (8 * k);
        m[i] = w;
    }
    for (i = 0; i < 8; i++) v[i] = h[i];
    for (i = 0; i < 8; i++) v[8 + i] = BL2B_IV[i];
    v[12] ^= t;
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 12; r++) {
        const uint8_t *s = BL2_SIGMA[r];
        bl2b_g(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
        bl2b_g(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
        bl2b_g(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
        bl2b_g(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
        bl2b_g(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
        bl2b_g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        bl2b_g(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        bl2b_g(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

static void bl2b_hash(const uint8_t *p, size_t n, uint8_t *out, size_t out_len)
{
    uint64_t h[8];
    uint8_t block[128];
    uint64_t t = 0;
    size_t i;

    memcpy(h, BL2B_IV, sizeof h);
    h[0] ^= 0x01010000ULL ^ (uint64_t)out_len;   /* no key, fanout 1, depth 1 */
    while (n > 128) {
        t += 128;
        bl2b_compress(h, p, t, 0);
        p += 128;
        n -= 128;
    }
    memset(block, 0, sizeof block);
    memcpy(block, p, n);
    t += n;
    bl2b_compress(h, block, t, 1);
    for (i = 0; i < out_len; i++)
        out[i] = (uint8_t)(h[i / 8] >> (8 * (i % 8)));
}

static void bl2s_g(uint32_t *v, int a, int b, int c, int d,
                   uint32_t x, uint32_t y)
{
    v[a] = v[a] + v[b] + x; v[d] = bl3_rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];     v[b] = bl3_rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y; v[d] = bl3_rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];     v[b] = bl3_rotr(v[b] ^ v[c], 7);
}

static void bl2s_compress(uint32_t h[8], const uint8_t block[64],
                          uint64_t t, int last)
{
    uint32_t v[16], m[16];
    int i, r;

    for (i = 0; i < 16; i++)
        m[i] = bl3_load32(block + i * 4);
    for (i = 0; i < 8; i++) v[i] = h[i];
    for (i = 0; i < 8; i++) v[8 + i] = BL3_IV[i];
    v[12] ^= (uint32_t)t;
    v[13] ^= (uint32_t)(t >> 32);
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 10; r++) {          /* BLAKE2s is 10 rounds, not 12 */
        const uint8_t *s = BL2_SIGMA[r];
        bl2s_g(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
        bl2s_g(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
        bl2s_g(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
        bl2s_g(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
        bl2s_g(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
        bl2s_g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        bl2s_g(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        bl2s_g(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

static void bl2s_hash(const uint8_t *p, size_t n, uint8_t *out, size_t out_len)
{
    uint32_t h[8];
    uint8_t block[64];
    uint64_t t = 0;
    size_t i;

    memcpy(h, BL3_IV, sizeof h);
    h[0] ^= 0x01010000u ^ (uint32_t)out_len;
    while (n > 64) {
        t += 64;
        bl2s_compress(h, p, t, 0);
        p += 64;
        n -= 64;
    }
    memset(block, 0, sizeof block);
    memcpy(block, p, n);
    t += n;
    bl2s_compress(h, block, t, 1);
    for (i = 0; i < out_len; i++)
        out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

/* ---- Murmur3 x64 128-bit ---- */

static uint64_t mm3_rotl(uint64_t x, int n)
{
    return (x << n) | (x >> (64 - n));
}

static uint64_t mm3_fmix(uint64_t k)
{
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

static uint64_t mm3_load64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void mm3_128(const uint8_t *p, size_t n, uint32_t seed, uint8_t out[16])
{
    const uint64_t c1 = 0x87C37B91114253D5ULL, c2 = 0x4CF5AD432745937FULL;
    uint64_t h1 = seed, h2 = seed, k1 = 0, k2 = 0;
    size_t blocks = n / 16, i;
    const uint8_t *tail;

    for (i = 0; i < blocks; i++) {
        uint64_t a = mm3_load64(p + i * 16), b = mm3_load64(p + i * 16 + 8);
        a *= c1; a = mm3_rotl(a, 31); a *= c2; h1 ^= a;
        h1 = mm3_rotl(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52DCE729;
        b *= c2; b = mm3_rotl(b, 33); b *= c1; h2 ^= b;
        h2 = mm3_rotl(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495AB5;
    }
    tail = p + blocks * 16;
    switch (n & 15) {
    case 15: k2 ^= (uint64_t)tail[14] << 48;   /* fallthrough */
    case 14: k2 ^= (uint64_t)tail[13] << 40;   /* fallthrough */
    case 13: k2 ^= (uint64_t)tail[12] << 32;   /* fallthrough */
    case 12: k2 ^= (uint64_t)tail[11] << 24;   /* fallthrough */
    case 11: k2 ^= (uint64_t)tail[10] << 16;   /* fallthrough */
    case 10: k2 ^= (uint64_t)tail[9] << 8;     /* fallthrough */
    case  9: k2 ^= (uint64_t)tail[8];
             k2 *= c2; k2 = mm3_rotl(k2, 33); k2 *= c1; h2 ^= k2;
             /* fallthrough */
    case  8: k1 ^= (uint64_t)tail[7] << 56;    /* fallthrough */
    case  7: k1 ^= (uint64_t)tail[6] << 48;    /* fallthrough */
    case  6: k1 ^= (uint64_t)tail[5] << 40;    /* fallthrough */
    case  5: k1 ^= (uint64_t)tail[4] << 32;    /* fallthrough */
    case  4: k1 ^= (uint64_t)tail[3] << 24;    /* fallthrough */
    case  3: k1 ^= (uint64_t)tail[2] << 16;    /* fallthrough */
    case  2: k1 ^= (uint64_t)tail[1] << 8;     /* fallthrough */
    case  1: k1 ^= (uint64_t)tail[0];
             k1 *= c1; k1 = mm3_rotl(k1, 31); k1 *= c2; h1 ^= k1;
             break;
    default: break;
    }
    h1 ^= (uint64_t)n; h2 ^= (uint64_t)n;
    h1 += h2; h2 += h1;
    h1 = mm3_fmix(h1); h2 = mm3_fmix(h2);
    h1 += h2; h2 += h1;
    for (i = 0; i < 8; i++) out[i] = (uint8_t)(h1 >> (8 * i));
    for (i = 0; i < 8; i++) out[8 + i] = (uint8_t)(h2 >> (8 * i));
}

/* magic: 0 BLAKE3, 1 BLAKE2b, 2 BLAKE2s, 3 Murmur3_128; +8 for the hex form */
static JSValue dyn_blake(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic)
{
    static const struct { size_t def, max; const char *name; } K[] = {
        { 32,  1u << 20, "BLAKE3" }, { 64, 64, "BLAKE2b" },
        { 32,  32,       "BLAKE2s" }, { 16, 16, "Murmur3_128" },
    };
    int kind = magic & 7, hex = (magic & 8) != 0;
    const uint8_t *data;
    const char *owned;
    size_t n, out_len = K[kind].def;
    uint32_t seed = 0;
    uint8_t *out;
    JSValue result;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(data): data is required", K[kind].name);
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        int64_t v;
        if (JS_ToInt64(ctx, &v, argv[1]) < 0)
            return JS_EXCEPTION;
        if (kind == 3) {                /* Murmur3's second argument is a seed */
            seed = (uint32_t)v;
        } else if (v < 1 || (size_t)v > K[kind].max) {
            return JS_ThrowRangeError(ctx, "%s(data, length): length is 1 to %zu "
                                      "bytes", K[kind].name, K[kind].max);
        } else {
            out_len = (size_t)v;
        }
    }
    if (dyn_crypto_data(ctx, argv[0], &data, &n, &owned) < 0)
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(out_len);
    if (!out) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    switch (kind) {
    case 0: {
        bl3_hasher_t h;
        bl3_init(&h);
        bl3_update(&h, data, n);
        bl3_final(&h, out, out_len);
        break;
    }
    case 1: bl2b_hash(data, n, out, out_len); break;
    case 2: bl2s_hash(data, n, out, out_len); break;
    default: mm3_128(data, n, seed, out); break;
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    if (hex) {
        char *h = (char *)malloc(out_len * 2 + 1);
        size_t i;
        if (!h) { free(out); return JS_ThrowOutOfMemory(ctx); }
        for (i = 0; i < out_len; i++) {
            h[i * 2] = "0123456789abcdef"[out[i] >> 4];
            h[i * 2 + 1] = "0123456789abcdef"[out[i] & 15];
        }
        result = JS_NewStringLen(ctx, h, out_len * 2);
        free(h);
    } else {
        result = dyn_crypto_u8array(ctx, out, out_len);
    }
    free(out);
    return result;
}
