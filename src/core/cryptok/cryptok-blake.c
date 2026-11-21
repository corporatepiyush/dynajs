#include "cryptok.h"

#define BL3_CHUNK_START 1u
#define BL3_CHUNK_END 2u
#define BL3_PARENT 4u
#define BL3_ROOT 8u
#define BL3_CHUNK 1024

typedef struct {
    uint32_t cv[8];
    uint8_t block[BL3_BLOCK];
    uint32_t block_len, flags;
    uint64_t counter;
} bl3_out_t;

const uint32_t BL3_IV[8] = {
    0x6A09E667u,
    0xBB67AE85u,
    0x3C6EF372u,
    0xA54FF53Au,
    0x510E527Fu,
    0x9B05688Cu,
    0x1F83D9ABu,
    0x5BE0CD19u,
};

static const uint8_t BL3_PERM[16] = {
    2,
    6,
    3,
    10,
    7,
    0,
    4,
    13,
    1,
    11,
    12,
    5,
    9,
    14,
    15,
    8,
};

static uint32_t bl3_rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void bl3_g(uint32_t* s, int a, int b, int c, int d,
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

static void bl3_round(uint32_t* s, const uint32_t* m)
{
    bl3_g(s, 0, 4, 8, 12, m[0], m[1]);
    bl3_g(s, 1, 5, 9, 13, m[2], m[3]);
    bl3_g(s, 2, 6, 10, 14, m[4], m[5]);
    bl3_g(s, 3, 7, 11, 15, m[6], m[7]);
    bl3_g(s, 0, 5, 10, 15, m[8], m[9]);
    bl3_g(s, 1, 6, 11, 12, m[10], m[11]);
    bl3_g(s, 2, 7, 8, 13, m[12], m[13]);
    bl3_g(s, 3, 4, 9, 14, m[14], m[15]);
}

static void bl3_compress(const uint32_t cv[8], const uint8_t block[64],
    uint64_t counter, uint32_t block_len, uint32_t flags,
    uint32_t out[16])
{
    uint32_t m[16], s[16];
    int i, r;

    for (i = 0; i < 16; i++)
        m[i] = dyn_ld32le(block + i * 4);
    for (i = 0; i < 8; i++)
        s[i] = cv[i];
    s[8] = BL3_IV[0];
    s[9] = BL3_IV[1];
    s[10] = BL3_IV[2];
    s[11] = BL3_IV[3];
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

static void bl3_out_cv(const bl3_out_t* o, uint32_t cv[8])
{
    uint32_t s[16];
    bl3_compress(o->cv, o->block, o->counter, o->block_len, o->flags, s);
    memcpy(cv, s, 8 * sizeof(uint32_t));
}

static void bl3_out_root(const bl3_out_t* o, uint8_t* out, size_t out_len)
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
            dyn_st32le(wide + i * 4, s[i]);
        memcpy(out, wide, take);
        out += take;
        out_len -= take;
        blk++;
    }
}

static void bl3_chunk_init(bl3_chunk_t* c, const uint32_t key[8],
    uint64_t counter, uint32_t flags)
{
    memcpy(c->cv, key, 8 * sizeof(uint32_t));
    c->chunk_counter = counter;
    memset(c->buf, 0, BL3_BLOCK);
    c->buf_len = 0;
    c->blocks = 0;
    c->flags = flags;
}

static uint32_t bl3_chunk_start(const bl3_chunk_t* c)
{
    return c->blocks == 0 ? BL3_CHUNK_START : 0;
}

static void bl3_chunk_update(bl3_chunk_t* c, const uint8_t* p, size_t n)
{
    while (n) {
        size_t take;
        if (c->buf_len == BL3_BLOCK) {
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

static void bl3_chunk_out(const bl3_chunk_t* c, bl3_out_t* o)
{
    memcpy(o->cv, c->cv, 8 * sizeof(uint32_t));
    memcpy(o->block, c->buf, BL3_BLOCK);
    o->block_len = c->buf_len;
    o->counter = c->chunk_counter;
    o->flags = c->flags | bl3_chunk_start(c) | BL3_CHUNK_END;
}

static void bl3_parent_out(const uint32_t left[8], const uint32_t right[8],
    const uint32_t key[8], uint32_t flags, bl3_out_t* o)
{
    memcpy(o->block, left, 8 * sizeof(uint32_t));
    memcpy(o->block + 32, right, 8 * sizeof(uint32_t));
    memcpy(o->cv, key, 8 * sizeof(uint32_t));
    o->block_len = BL3_BLOCK;
    o->counter = 0;
    o->flags = flags | BL3_PARENT;
}

#ifdef BL3_LANES

#define BL3_G(a, b, c, d, mx, my)                   \
    do {                                            \
        (a) = bl3_v_add(bl3_v_add((a), (b)), (mx)); \
        (d) = BL3_ROT(bl3_v_xor((d), (a)), 16);     \
        (c) = bl3_v_add((c), (d));                  \
        (b) = BL3_ROT(bl3_v_xor((b), (c)), 12);     \
        (a) = bl3_v_add(bl3_v_add((a), (b)), (my)); \
        (d) = BL3_ROT(bl3_v_xor((d), (a)), 8);      \
        (c) = bl3_v_add((c), (d));                  \
        (b) = BL3_ROT(bl3_v_xor((b), (c)), 7);      \
    } while (0)

#define BL3_ROUND(v, m, s0, s1, s2, s3, s4, s5, s6, s7,            \
    s8, s9, sa, sb, sc, sd, se, sf)                                \
    do {                                                           \
        BL3_G((v)[0], (v)[4], (v)[8], (v)[12], (m)[s0], (m)[s1]);  \
        BL3_G((v)[1], (v)[5], (v)[9], (v)[13], (m)[s2], (m)[s3]);  \
        BL3_G((v)[2], (v)[6], (v)[10], (v)[14], (m)[s4], (m)[s5]); \
        BL3_G((v)[3], (v)[7], (v)[11], (v)[15], (m)[s6], (m)[s7]); \
        BL3_G((v)[0], (v)[5], (v)[10], (v)[15], (m)[s8], (m)[s9]); \
        BL3_G((v)[1], (v)[6], (v)[11], (v)[12], (m)[sa], (m)[sb]); \
        BL3_G((v)[2], (v)[7], (v)[8], (v)[13], (m)[sc], (m)[sd]);  \
        BL3_G((v)[3], (v)[4], (v)[9], (v)[14], (m)[se], (m)[sf]);  \
    } while (0)

#define BL3_ROUNDS(v, m)                                                       \
    do {                                                                       \
        BL3_ROUND(v, m, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); \
        BL3_ROUND(v, m, 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8); \
        BL3_ROUND(v, m, 3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1); \
        BL3_ROUND(v, m, 10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6); \
        BL3_ROUND(v, m, 12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4); \
        BL3_ROUND(v, m, 9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7); \
        BL3_ROUND(v, m, 11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13); \
    } while (0)

#define BL3_CHUNK_BLOCKS (BL3_CHUNK / BL3_BLOCK)

static void bl3_chunks_wide(const uint8_t* p, uint64_t counter, uint32_t flags,
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
            const uint8_t* q = p + (size_t)b * BL3_BLOCK + (size_t)i * 4;
            bl3_vec a0 = bl3_v_loadu(q);
            bl3_vec a1 = bl3_v_loadu(q + BL3_CHUNK);
            bl3_vec a2 = bl3_v_loadu(q + 2 * BL3_CHUNK);
            bl3_vec a3 = bl3_v_loadu(q + 3 * BL3_CHUNK);
            BL3_TRANSPOSE(a0, a1, a2, a3, m[i], m[i + 1], m[i + 2], m[i + 3]);
        }

        for (i = 0; i < 8; i++)
            v[i] = h[i];
        v[8] = bl3_v_set1(BL3_IV[0]);
        v[9] = bl3_v_set1(BL3_IV[1]);
        v[10] = bl3_v_set1(BL3_IV[2]);
        v[11] = bl3_v_set1(BL3_IV[3]);
        v[12] = ctr_lo;
        v[13] = ctr_hi;
        v[14] = bl3_v_set1(BL3_BLOCK);
        v[15] = bl3_v_set1(bf);

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
#endif

void bl3_init(bl3_hasher_t* h)
{
    memcpy(h->key, BL3_IV, sizeof h->key);
    bl3_chunk_init(&h->chunk, h->key, 0, 0);
    h->stack_len = 0;
    h->flags = 0;
}

static void bl3_init_key_words(bl3_hasher_t* h, const uint32_t key[8],
    uint32_t flags)
{
    memcpy(h->key, key, sizeof h->key);
    bl3_chunk_init(&h->chunk, h->key, 0, flags);
    h->stack_len = 0;
    h->flags = flags;
}

void bl3_init_flags(bl3_hasher_t* h, uint32_t flags)
{
    bl3_init_key_words(h, BL3_IV, flags);
}

void bl3_init_keyed(bl3_hasher_t* h, const uint8_t key[32],
    uint32_t flags)
{
    uint32_t kw[8];
    int i;
    for (i = 0; i < 8; i++)
        kw[i] = dyn_ld32le(key + 4 * i);
    bl3_init_key_words(h, kw, flags);
}

void bl3_ctx_adopt(const char* cs, size_t n, const uint8_t** ctxb,
    size_t* ctxn)
{
    *ctxb = (const uint8_t*)cs;
    *ctxn = n;
}

static void bl3_merge(bl3_hasher_t* h, uint64_t total_chunks)
{
    size_t want = 0;
    uint64_t t = total_chunks;

    while (t) {
        want += (size_t)(t & 1);
        t >>= 1;
    }
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

#define BL3_PAR_PART_CHUNKS 64u
#define BL3_PAR_MIN_PARTS 2u
#define BL3_PAR_MAX_SUBTREES 64

typedef struct {
    const uint8_t* p;
    const uint32_t* key;
    uint32_t flags;
    uint64_t counter;
    size_t parts;
    uint32_t (*cvs)[8];
} bl3_par_t;

static void bl3_part_cv(const uint8_t* p, uint64_t counter, const uint32_t key[8],
    uint32_t flags, uint32_t cv[8])
{
    uint32_t stack[24][8];
    size_t sl = 0;
    uint32_t k = 0;

    while (k < BL3_PAR_PART_CHUNKS) {
        uint32_t cvs[8][8];
        uint32_t nl = 1, l;
#ifdef BL3_LANES
        if (BL3_PAR_PART_CHUNKS - k >= BL3_LANES) {
            bl3_chunks_wide(p + (size_t)k * BL3_CHUNK, counter + k, flags, key, cvs);
            nl = BL3_LANES;
        } else
#endif
        {
            bl3_chunk_t ch;
            bl3_out_t o;
            bl3_chunk_init(&ch, key, counter + k, flags);
            bl3_chunk_update(&ch, p + (size_t)k * BL3_CHUNK, BL3_CHUNK);
            bl3_chunk_out(&ch, &o);
            bl3_out_cv(&o, cvs[0]);
        }
        for (l = 0; l < nl; l++) {
            uint32_t t;
            memcpy(stack[sl++], cvs[l], sizeof cvs[l]);
            k++;
            for (t = k; (t & 1) == 0; t >>= 1) {
                bl3_out_t o;
                bl3_parent_out(stack[sl - 2], stack[sl - 1], key, flags, &o);
                sl--;
                bl3_out_cv(&o, stack[sl - 1]);
            }
        }
    }
    memcpy(cv, stack[0], sizeof stack[0]);
}

static void bl3_par_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    bl3_par_t* x = (bl3_par_t*)arg;
    size_t i;
    (void)par;
    for (i = w; i < x->parts; i += nw)
        bl3_part_cv(x->p + i * (size_t)BL3_PAR_PART_CHUNKS * BL3_CHUNK,
            x->counter + (uint64_t)i * BL3_PAR_PART_CHUNKS, x->key, x->flags,
            x->cvs[i]);
}

static size_t bl3_par_consume(bl3_hasher_t* h, const uint8_t* p, size_t n)
{
    uint64_t c = h->chunk.chunk_counter, sizes[BL3_PAR_MAX_SUBTREES];
    uint64_t total = 0, avail = n / BL3_CHUNK;
    size_t nsub = 0, parts, i, k;
    unsigned nw;
    bl3_par_t x;

    if (c % BL3_PAR_PART_CHUNKS)
        return 0;
    while (nsub < BL3_PAR_MAX_SUBTREES) {
        uint64_t at = c + total, s = BL3_PAR_PART_CHUNKS;
        if ((total + s) * BL3_CHUNK >= n)
            break;
        while (at % (s * 2) == 0 && (total + s * 2) * BL3_CHUNK < n && total + s * 2 <= avail)
            s *= 2;
        sizes[nsub++] = s;
        total += s;
    }
    parts = (size_t)(total / BL3_PAR_PART_CHUNKS);
    if (parts < BL3_PAR_MIN_PARTS)
        return 0;
    nw = dyn_par_plan(parts, BL3_PAR_MIN_PARTS / 2, 0);
    if (nw < 2)
        return 0;
    x.p = p;
    x.key = h->key;
    x.flags = h->flags;
    x.counter = c;
    x.parts = parts;
    x.cvs = (uint32_t (*)[8])malloc(parts * sizeof(*x.cvs));
    if (!x.cvs)
        return 0;
    dyn_par_run(nw, bl3_par_worker, &x);
    k = 0;
    for (i = 0; i < nsub; i++) {
        size_t m = (size_t)(sizes[i] / BL3_PAR_PART_CHUNKS), j, step;
        for (step = 1; step < m; step *= 2)
            for (j = 0; j + step < m; j += 2 * step) {
                bl3_out_t o;
                bl3_parent_out(x.cvs[k + j], x.cvs[k + j + step], h->key, h->flags, &o);
                bl3_out_cv(&o, x.cvs[k + j]);
            }
        memcpy(h->stack[h->stack_len], x.cvs[k], sizeof x.cvs[k]);
        h->stack_len++;
        k += m;
        bl3_merge(h, c + (uint64_t)k * BL3_PAR_PART_CHUNKS);
    }
    free(x.cvs);
    bl3_chunk_init(&h->chunk, h->key, c + total, h->flags);
    return (size_t)total * BL3_CHUNK;
}

void bl3_update(bl3_hasher_t* h, const uint8_t* p, size_t n)
{
    while (n) {
        size_t have = (size_t)h->chunk.blocks * BL3_BLOCK + h->chunk.buf_len;
        size_t take;
        if (have == 0 && n > (size_t)BL3_PAR_MIN_PARTS * BL3_PAR_PART_CHUNKS * BL3_CHUNK) {
            size_t mis = (size_t)(h->chunk.chunk_counter % BL3_PAR_PART_CHUNKS), used;
            if (mis && n > (BL3_PAR_PART_CHUNKS - mis + (size_t)BL3_PAR_MIN_PARTS * BL3_PAR_PART_CHUNKS) * BL3_CHUNK) {
                size_t pre = (BL3_PAR_PART_CHUNKS - mis) * BL3_CHUNK;
                bl3_update(h, p, pre);
                p += pre;
                n -= pre;
                continue;
            }
            used = bl3_par_consume(h, p, n);
            if (used) {
                p += used;
                n -= used;
                continue;
            }
        }
#ifdef BL3_LANES
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
        if (have == BL3_CHUNK) {
            bl3_out_t o;
            uint32_t cv[8];
            uint64_t done = h->chunk.chunk_counter + 1;
            bl3_chunk_out(&h->chunk, &o);
            bl3_out_cv(&o, cv);
            memcpy(h->stack[h->stack_len], cv, sizeof cv);
            h->stack_len++;
            bl3_merge(h, done);
            bl3_chunk_init(&h->chunk, h->key, done, h->flags);
            continue;
        }
        take = BL3_CHUNK - have;
        if (take > n)
            take = n;
        bl3_chunk_update(&h->chunk, p, take);
        p += take;
        n -= take;
    }
}

void bl3_final(const bl3_hasher_t* h, uint8_t* out, size_t out_len)
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

static const uint8_t BL2_SIGMA[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
};

const uint64_t BL2B_IV[8] = {
    0x6A09E667F3BCC908ULL,
    0xBB67AE8584CAA73BULL,
    0x3C6EF372FE94F82BULL,
    0xA54FF53A5F1D36F1ULL,
    0x510E527FADE682D1ULL,
    0x9B05688C2B3E6C1FULL,
    0x1F83D9ABFB41BD6BULL,
    0x5BE0CD19137E2179ULL,
};

static uint64_t bl2_rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static void bl2b_g(uint64_t* v, int a, int b, int c, int d,
    uint64_t x, uint64_t y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = bl2_rotr64(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];
    v[b] = bl2_rotr64(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y;
    v[d] = bl2_rotr64(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = bl2_rotr64(v[b] ^ v[c], 63);
}

void bl2b_compress(uint64_t h[8], const uint8_t block[128],
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
    for (i = 0; i < 8; i++)
        v[i] = h[i];
    for (i = 0; i < 8; i++)
        v[8 + i] = BL2B_IV[i];
    v[12] ^= t;
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 12; r++) {
        const uint8_t* s = BL2_SIGMA[r];
        bl2b_g(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        bl2b_g(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        bl2b_g(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        bl2b_g(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        bl2b_g(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        bl2b_g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        bl2b_g(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        bl2b_g(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

void bl2b_hash_key(const uint8_t* p, size_t n, uint8_t* out,
    size_t out_len, const uint8_t* key, size_t keylen)
{
    uint64_t h[8];
    uint8_t block[128];
    uint64_t t = 0;
    size_t i;

    memcpy(h, BL2B_IV, sizeof h);
    h[0] ^= 0x01010000ULL ^ (uint64_t)out_len ^ ((uint64_t)keylen << 8);
    if (keylen) {
        memset(block, 0, sizeof block);
        memcpy(block, key, keylen);
        t = 128;
        if (n == 0) {
            bl2b_compress(h, block, t, 1);
            for (i = 0; i < out_len; i++)
                out[i] = (uint8_t)(h[i / 8] >> (8 * (i % 8)));
            return;
        }
        bl2b_compress(h, block, t, 0);
    }
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

static void bl2s_g(uint32_t* v, int a, int b, int c, int d,
    uint32_t x, uint32_t y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = bl3_rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = bl3_rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = bl3_rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = bl3_rotr(v[b] ^ v[c], 7);
}

void bl2s_compress(uint32_t h[8], const uint8_t block[64],
    uint64_t t, int last)
{
    uint32_t v[16], m[16];
    int i, r;

    for (i = 0; i < 16; i++)
        m[i] = dyn_ld32le(block + i * 4);
    for (i = 0; i < 8; i++)
        v[i] = h[i];
    for (i = 0; i < 8; i++)
        v[8 + i] = BL3_IV[i];
    v[12] ^= (uint32_t)t;
    v[13] ^= (uint32_t)(t >> 32);
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 10; r++) {
        const uint8_t* s = BL2_SIGMA[r];
        bl2s_g(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        bl2s_g(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        bl2s_g(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        bl2s_g(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        bl2s_g(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        bl2s_g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        bl2s_g(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        bl2s_g(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

void bl2s_hash_key(const uint8_t* p, size_t n, uint8_t* out,
    size_t out_len, const uint8_t* key, size_t keylen)
{
    uint32_t h[8];
    uint8_t block[64];
    uint64_t t = 0;
    size_t i;

    memcpy(h, BL3_IV, sizeof h);
    h[0] ^= 0x01010000u ^ (uint32_t)out_len ^ ((uint32_t)keylen << 8);
    if (keylen) {
        memset(block, 0, sizeof block);
        memcpy(block, key, keylen);
        t = 64;
        if (n == 0) {
            bl2s_compress(h, block, t, 1);
            for (i = 0; i < out_len; i++)
                out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
            return;
        }
        bl2s_compress(h, block, t, 0);
    }
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
