#include "../dyn-csvx.h"
#include "../dyn-par.h"

#include <stdlib.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define CSVX_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define CSVX_SSE2 1
#endif

#define CSVX_MIN_CHUNK ((size_t)1 << 20)
#define CSVX_MAX_WORKERS 64

typedef struct {
    uint64_t q, d, l, c;
} BlkMasks;

#if defined(CSVX_NEON)
static inline uint64_t neon_mask(uint8x16_t a, uint8x16_t b, uint8x16_t c, uint8x16_t d)
{
    static const uint8_t bitv[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8x16_t bit = vld1q_u8(bitv);
    uint8x16_t s0 = vpaddq_u8(vandq_u8(a, bit), vandq_u8(b, bit));
    uint8x16_t s1 = vpaddq_u8(vandq_u8(c, bit), vandq_u8(d, bit));
    s0 = vpaddq_u8(s0, s1);
    s0 = vpaddq_u8(s0, s0);
    return vgetq_lane_u64(vreinterpretq_u64_u8(s0), 0);
}

static inline void blk_masks(const uint8_t* p, uint8_t dch, uint8_t qch, BlkMasks* m)
{
    uint8x16_t v0 = vld1q_u8(p), v1 = vld1q_u8(p + 16), v2 = vld1q_u8(p + 32), v3 = vld1q_u8(p + 48);
    uint8x16_t q = vdupq_n_u8(qch), d = vdupq_n_u8(dch), l = vdupq_n_u8('\n'), c = vdupq_n_u8('\r');
    m->q = neon_mask(vceqq_u8(v0, q), vceqq_u8(v1, q), vceqq_u8(v2, q), vceqq_u8(v3, q));
    m->d = neon_mask(vceqq_u8(v0, d), vceqq_u8(v1, d), vceqq_u8(v2, d), vceqq_u8(v3, d));
    m->l = neon_mask(vceqq_u8(v0, l), vceqq_u8(v1, l), vceqq_u8(v2, l), vceqq_u8(v3, l));
    m->c = neon_mask(vceqq_u8(v0, c), vceqq_u8(v1, c), vceqq_u8(v2, c), vceqq_u8(v3, c));
}

static inline uint64_t blk_quotes(const uint8_t* p, uint8_t qch)
{
    uint8x16_t q = vdupq_n_u8(qch);
    return neon_mask(vceqq_u8(vld1q_u8(p), q), vceqq_u8(vld1q_u8(p + 16), q),
        vceqq_u8(vld1q_u8(p + 32), q), vceqq_u8(vld1q_u8(p + 48), q));
}
#elif defined(CSVX_SSE2)
static inline uint64_t sse_mask(const uint8_t* p, uint8_t ch)
{
    __m128i k = _mm_set1_epi8((char)ch);
    uint64_t a = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)p), k));
    uint64_t b = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(p + 16)), k));
    uint64_t c = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(p + 32)), k));
    uint64_t d = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(p + 48)), k));
    return a | (b << 16) | (c << 32) | (d << 48);
}

static inline void blk_masks(const uint8_t* p, uint8_t dch, uint8_t qch, BlkMasks* m)
{
    m->q = sse_mask(p, qch);
    m->d = sse_mask(p, dch);
    m->l = sse_mask(p, '\n');
    m->c = sse_mask(p, '\r');
}

static inline uint64_t blk_quotes(const uint8_t* p, uint8_t qch)
{
    return sse_mask(p, qch);
}
#else
static inline void blk_masks(const uint8_t* p, uint8_t dch, uint8_t qch, BlkMasks* m)
{
    int i;
    m->q = m->d = m->l = m->c = 0;
    for (i = 0; i < 64; i++) {
        uint64_t b = (uint64_t)1 << i;
        m->q |= p[i] == qch ? b : 0;
        m->d |= p[i] == dch ? b : 0;
        m->l |= p[i] == '\n' ? b : 0;
        m->c |= p[i] == '\r' ? b : 0;
    }
}

static inline uint64_t blk_quotes(const uint8_t* p, uint8_t qch)
{
    uint64_t m = 0;
    int i;
    for (i = 0; i < 64; i++)
        m |= p[i] == qch ? (uint64_t)1 << i : 0;
    return m;
}
#endif

static inline uint64_t prefix_xor(uint64_t x)
{
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    x ^= x << 32;
    return x;
}

static inline unsigned ctz64(uint64_t x)
{
    return (unsigned)__builtin_ctzll(x);
}

static inline unsigned pop64(uint64_t x)
{
    return (unsigned)__builtin_popcountll(x);
}

typedef struct {
    size_t lo, hi;
    uint32_t* sep;
    size_t n, cap;
    uint32_t* rf;
    uint32_t* rs;
    size_t nr, rcap;
    uint64_t quotes;
    int bad, oom, end_inq;
} Chunk;

static int chunk_grow_sep(Chunk* k, size_t need)
{
    size_t nc;
    uint32_t* p;
    if (need <= k->cap)
        return 0;
    nc = k->cap ? k->cap : 256;
    while (nc < need)
        nc *= 2;
    p = (uint32_t*)realloc(k->sep, nc * sizeof(uint32_t));
    if (!p)
        return -1;
    k->sep = p;
    k->cap = nc;
    return 0;
}

static int chunk_grow_rows(Chunk* k, size_t need)
{
    size_t nc;
    uint32_t *a, *b;
    if (need <= k->rcap)
        return 0;
    nc = k->rcap ? k->rcap : 64;
    while (nc < need)
        nc *= 2;
    a = (uint32_t*)realloc(k->rf, nc * sizeof(uint32_t));
    if (!a)
        return -1;
    k->rf = a;
    b = (uint32_t*)realloc(k->rs, nc * sizeof(uint32_t));
    if (!b)
        return -1;
    k->rs = b;
    k->rcap = nc;
    return 0;
}

typedef struct {
    const uint8_t* buf;
    size_t len;
    uint8_t delim, quote;
    Chunk* ck;
    unsigned nck;
    CsvxIndex* ix;
    size_t nsep_total, nrows_total;
    int oom;
} BuildCtx;

static void scan_chunk(const BuildCtx* bc, Chunk* k, int inq0, uint64_t c_sr, uint64_t c_close, uint64_t c_cr)
{
    const uint8_t* buf = bc->buf;
    size_t len = bc->len, pos;
    uint8_t pad[64];
    uint8_t padch = 0;
    uint64_t c_inq = inq0 ? ~(uint64_t)0 : 0, bad = 0;

    while (padch == bc->delim || padch == bc->quote || padch == '\n' || padch == '\r')
        padch++;
    if (chunk_grow_sep(k, (k->hi - k->lo) / 8 + 128) || chunk_grow_rows(k, (k->hi - k->lo) / 32 + 64)) {
        k->oom = 1;
        return;
    }
    for (pos = k->lo; pos < k->hi; pos += 64) {
        BlkMasks m;
        uint64_t valid = ~(uint64_t)0, inq, open, close, dr, cr, lr, sr, leff, nl, s, prev_sr, prev_close, prev_cr;
        size_t rem = k->hi - pos;
        if (rem >= 64) {
            blk_masks(buf + pos, bc->delim, bc->quote, &m);
        } else {
            memset(pad, padch, sizeof(pad));
            memcpy(pad, buf + pos, rem);
            blk_masks(pad, bc->delim, bc->quote, &m);
            valid = ((uint64_t)1 << rem) - 1;
        }
        inq = prefix_xor(m.q) ^ c_inq;
        c_inq = (uint64_t)((int64_t)inq >> 63);
        open = m.q & inq;
        close = m.q & ~inq;
        dr = m.d & ~inq;
        cr = m.c & ~inq;
        lr = m.l & ~inq;
        sr = dr | cr | lr;
        prev_sr = (sr << 1) | c_sr;
        prev_close = (close << 1) | c_close;
        prev_cr = (cr << 1) | c_cr;
        bad |= (open & ~(prev_sr | prev_close)) | (prev_close & ~(sr | open) & valid);
        leff = lr & ~prev_cr;
        nl = cr | leff;
        s = dr | nl;
        c_sr = sr >> 63;
        c_close = close >> 63;
        c_cr = cr >> 63;
        if (!s)
            continue;
        if (chunk_grow_sep(k, k->n + 72)) {
            k->oom = 1;
            return;
        }
        if (nl) {
            unsigned cnt = pop64(nl);
            if (chunk_grow_rows(k, k->nr + cnt)) {
                k->oom = 1;
                return;
            }
            do {
                unsigned b = ctz64(nl);
                size_t at = pos + b;
                size_t fi = k->n + pop64(s & (((uint64_t)1 << b) - 1));
                size_t next = at + 1;
                if (buf[at] == '\r' && next < len && buf[next] == '\n')
                    next++;
                k->rf[k->nr] = (uint32_t)(fi + 1);
                k->rs[k->nr] = (uint32_t)next;
                k->nr++;
                nl &= nl - 1;
            } while (nl);
        }
        {
            uint32_t* out = k->sep + k->n;
            uint32_t base = (uint32_t)pos;
            unsigned cnt = pop64(s);
            unsigned i = 0;
            while (s) {
                out[i++] = base + ctz64(s);
                s &= s - 1;
            }
            k->n += cnt;
        }
    }
    k->bad = bad != 0;
    k->end_inq = c_inq != 0;
}

static void count_chunk(const BuildCtx* bc, Chunk* k)
{
    size_t pos;
    uint64_t n = 0;
    for (pos = k->lo; pos + 64 <= k->hi; pos += 64)
        n += pop64(blk_quotes(bc->buf + pos, bc->quote));
    for (; pos < k->hi; pos++)
        n += bc->buf[pos] == bc->quote;
    k->quotes = n;
}

static void chunk_carries(const BuildCtx* bc, size_t lo, int parity, uint64_t* c_sr, uint64_t* c_close, uint64_t* c_cr)
{
    uint8_t b;
    *c_sr = *c_close = *c_cr = 0;
    if (lo == 0) {
        *c_sr = 1;
        return;
    }
    b = bc->buf[lo - 1];
    if (b == bc->quote) {
        *c_close = parity == 0;
        return;
    }
    if (parity == 0 && (b == bc->delim || b == '\n' || b == '\r')) {
        *c_sr = 1;
        *c_cr = b == '\r';
    }
}

static void build_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    BuildCtx* bc = (BuildCtx*)arg;
    unsigned i, j;
    for (i = w; i < bc->nck; i += nw)
        count_chunk(bc, &bc->ck[i]);
    dyn_par_barrier(par);
    for (i = w; i < bc->nck; i += nw) {
        uint64_t q = 0, c_sr, c_close, c_cr;
        for (j = 0; j < i; j++)
            q += bc->ck[j].quotes;
        chunk_carries(bc, bc->ck[i].lo, (int)(q & 1), &c_sr, &c_close, &c_cr);
        scan_chunk(bc, &bc->ck[i], (int)(q & 1), c_sr, c_close, c_cr);
    }
    dyn_par_barrier(par);
    if (w == 0) {
        size_t ns = 0, nr = 1;
        int bad = 0;
        for (i = 0; i < bc->nck; i++) {
            ns += bc->ck[i].n;
            nr += bc->ck[i].nr;
            bad |= bc->ck[i].bad | bc->ck[i].oom;
        }
        bc->nsep_total = ns;
        bc->nrows_total = nr;
        bc->oom = 0;
        if (!bad) {
            CsvxIndex* ix = bc->ix;
            ix->sep = (uint32_t*)malloc((ns + 1) * sizeof(uint32_t));
            ix->row_first = (uint32_t*)malloc((nr + 1) * sizeof(uint32_t));
            ix->row_start = (uint32_t*)malloc((nr + 1) * sizeof(uint32_t));
            if (!ix->sep || !ix->row_first || !ix->row_start)
                bc->oom = 1;
        }
    }
    dyn_par_barrier(par);
    if (!bc->ix->sep || !bc->ix->row_first || !bc->ix->row_start || bc->oom)
        return;
    for (i = w; i < bc->nck; i += nw) {
        size_t so = 0, ro = 1, r;
        Chunk* k = &bc->ck[i];
        for (j = 0; j < i; j++) {
            so += bc->ck[j].n;
            ro += bc->ck[j].nr;
        }
        memcpy(bc->ix->sep + so, k->sep, k->n * sizeof(uint32_t));
        for (r = 0; r < k->nr; r++) {
            bc->ix->row_first[ro + r] = (uint32_t)(k->rf[r] + so);
            bc->ix->row_start[ro + r] = k->rs[r];
        }
    }
}

int csvx_build(CsvxIndex* ix, const uint8_t* buf, size_t len, uint8_t delim,
    uint8_t quote, unsigned max_threads)
{
    BuildCtx bc;
    Chunk local[1];
    Chunk* ck;
    unsigned nck, i, nw;
    int rc = CSVX_OK;
    size_t nr, ns, r;

    memset(ix, 0, sizeof(*ix));
    ix->buf = buf;
    ix->len = len;
    ix->delim = delim;
    ix->quote = quote;
    if (len > CSVX_MAX_LEN)
        return CSVX_TOO_LARGE;
    nw = len >= 2 * CSVX_MIN_CHUNK ? dyn_par_plan(len, CSVX_MIN_CHUNK, max_threads) : 1;
    if (nw < 1)
        nw = 1;
    nck = nw;
    if (nck > CSVX_MAX_WORKERS)
        nck = CSVX_MAX_WORKERS;
    ck = nck == 1 ? local : (Chunk*)calloc(nck, sizeof(Chunk));
    if (!ck)
        return CSVX_OOM;
    memset(ck, 0, nck * sizeof(Chunk));
    for (i = 0; i < nck; i++) {
        ck[i].lo = (len / nck) * i & ~(size_t)63;
        ck[i].hi = i + 1 == nck ? len : ((len / nck) * (i + 1) & ~(size_t)63);
    }
    memset(&bc, 0, sizeof(bc));
    bc.buf = buf;
    bc.len = len;
    bc.delim = delim;
    bc.quote = quote;
    bc.ck = ck;
    bc.nck = nck;
    bc.ix = ix;
    dyn_par_run(nck, build_worker, &bc);
    for (i = 0; i < nck; i++) {
        if (ck[i].oom)
            rc = CSVX_OOM;
        else if (rc == CSVX_OK && ck[i].bad)
            rc = CSVX_IRREGULAR;
    }
    if (rc == CSVX_OK && nck && ck[nck - 1].end_inq)
        rc = CSVX_IRREGULAR;
    if (rc == CSVX_OK && (bc.oom || !ix->sep || !ix->row_first || !ix->row_start))
        rc = CSVX_OOM;
    for (i = 0; i < nck; i++) {
        free(ck[i].sep);
        free(ck[i].rf);
        free(ck[i].rs);
    }
    if (ck != local)
        free(ck);
    if (rc != CSVX_OK) {
        csvx_free(ix);
        ix->buf = buf;
        ix->len = len;
        return rc;
    }
    ns = bc.nsep_total;
    nr = bc.nrows_total;
    ix->row_first[0] = 0;
    ix->row_start[0] = 0;
    if (len == 0) {
        nr = 0;
    } else if (ix->row_start[nr - 1] == len) {
        nr--;
    } else {
        ix->sep[ns++] = (uint32_t)len;
        ix->row_first[nr] = (uint32_t)ns;
    }
    ix->nsep = ns;
    ix->nrows = nr;
    ix->width = nr ? ix->row_first[1] - ix->row_first[0] : 0;
    for (r = 1; r < nr; r++)
        if (ix->row_first[r + 1] - ix->row_first[r] != ix->width) {
            ix->ragged = 1;
            break;
        }
    return CSVX_OK;
}

void csvx_free(CsvxIndex* ix)
{
    free(ix->sep);
    free(ix->row_first);
    free(ix->row_start);
    ix->sep = NULL;
    ix->row_first = NULL;
    ix->row_start = NULL;
    ix->nsep = ix->nrows = 0;
}
