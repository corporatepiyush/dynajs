#include "ds-internal.h"
#include "ds-chunk.h"
#include "ds-cbtree.h"

#define CBT_T 16
#define CBT_MAXK (2 * CBT_T - 1)
#define CBT_MAXC (2 * CBT_T)
#define CBT_DEPTH 64
#define CBT_SMALL 128
#define CBT_FILL 24
#define CBT_LF 0x80000000u
#if defined(__GNUC__) || defined(__clang__)
#define CBT_INLINE inline __attribute__((always_inline))
#else
#define CBT_INLINE inline
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define CBT_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define CBT_SSE2 1
#endif

typedef struct {
    uint32_t n, leaf;
} cbt_hdr;

struct dyn_cbt {
    dyn_ds_ops kops, vops;
    size_t ks, vs, kidoff, cntoff, voff, lvoff;
    int f64;
    uint32_t esz, scap;
    dyn_chunk_t nd, lf;
    cbt_hdr* sm;
    uint32_t root;
    size_t size;
};

static CBT_INLINE cbt_hdr* cbt_n(const dyn_cbt_t* t, uint32_t s)
{
    if (s & CBT_LF)
        return (cbt_hdr*)(void*)t->lf.ch[s & ~CBT_LF];
    return (cbt_hdr*)(void*)t->nd.ch[s];
}

static uint8_t* cbt_k(const dyn_cbt_t* t, cbt_hdr* x, uint32_t i)
{
    return (uint8_t*)x + sizeof(cbt_hdr) + (size_t)i * t->ks;
}

static uint8_t* cbt_v(const dyn_cbt_t* t, cbt_hdr* x, uint32_t i)
{
    return (uint8_t*)x + (x->leaf ? t->lvoff : t->voff) + (size_t)i * t->vs;
}

static uint8_t* cbt_sv(const dyn_cbt_t* t, uint32_t i)
{
    return (uint8_t*)t->sm + sizeof(cbt_hdr) + (size_t)t->scap * t->ks + (size_t)i * t->vs;
}

static uint32_t* cbt_kid(const dyn_cbt_t* t, cbt_hdr* x)
{
    return (uint32_t*)((uint8_t*)x + t->kidoff);
}

static uint32_t* cbt_cnt(const dyn_cbt_t* t, cbt_hdr* x)
{
    return (uint32_t*)((uint8_t*)x + t->cntoff);
}

static int cbt_cmp(const dyn_cbt_t* t, const void* a, const void* b)
{
    return ds_ops_cmp_f64_fast(&t->kops, a, b);
}

static uint32_t cbt_lb_gen(const dyn_cbt_t* t, cbt_hdr* x, const void* key, int* found)
{
    uint32_t lo = 0, hi = x->n;
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (cbt_cmp(t, cbt_k(t, x, m), key) < 0)
            lo = m + 1;
        else
            hi = m;
    }
    *found = lo < x->n && cbt_cmp(t, cbt_k(t, x, lo), key) == 0;
    return lo;
}

static inline const double* cbt_kf(const cbt_hdr* x)
{
    return (const double*)(const void*)((const uint8_t*)x + sizeof(cbt_hdr));
}

static CBT_INLINE uint32_t cbt_lb(const dyn_cbt_t* t, cbt_hdr* x, const void* key, int* found)
{
    if (t->f64) {
        const double* kv = cbt_kf(x);
        double k;
        uint32_t i = 0, j;
        memcpy(&k, key, 8);
        for (j = 0; j < CBT_MAXK; j++)
            i += kv[j] < k;
        *found = i < x->n && kv[i] == k;
        return i;
    }
    return cbt_lb_gen(t, x, key, found);
}

static CBT_INLINE uint32_t cbt_lbw(const dyn_cbt_t* t, cbt_hdr* x, const void* key, int* found)
{
    if (t->f64) {
        const double* kv = cbt_kf(x);
        double k;
        uint32_t i = 0, n = x->n;
        memcpy(&k, key, 8);
        while (i < n && kv[i] < k)
            i++;
        *found = i < n && kv[i] == k;
        return i;
    }
    return cbt_lb_gen(t, x, key, found);
}

static uint32_t cbt_slb(const dyn_cbt_t* t, cbt_hdr* x, const void* key, int* found)
{
    if (t->f64) {
        const double* kv = cbt_kf(x);
        double k;
        uint32_t i = 0, n = x->n;
        memcpy(&k, key, 8);
        while (i + 16 <= n && kv[i + 15] < k)
            i += 16;
        while (i < n && kv[i] < k)
            i++;
        *found = i < n && kv[i] == k;
        return i;
    }
    return cbt_lb_gen(t, x, key, found);
}

static CBT_INLINE uint32_t cbt_psum(const uint32_t* c, uint32_t i)
{
#if defined(CBT_NEON)
    const uint32x4_t lim = vdupq_n_u32(i), step = vdupq_n_u32(4);
    uint32x4_t ix = { 0, 1, 2, 3 }, a = vdupq_n_u32(0);
    uint32_t j;
    for (j = 0; j < CBT_MAXC; j += 4) {
        a = vaddq_u32(a, vandq_u32(vld1q_u32(c + j), vcltq_u32(ix, lim)));
        ix = vaddq_u32(ix, step);
    }
    return vaddvq_u32(a);
#elif defined(CBT_SSE2)
    const __m128i lim = _mm_set1_epi32((int)i), step = _mm_set1_epi32(4);
    __m128i ix = _mm_setr_epi32(0, 1, 2, 3), a = _mm_setzero_si128();
    uint32_t j;
    for (j = 0; j < CBT_MAXC; j += 4) {
        a = _mm_add_epi32(a, _mm_and_si128(_mm_loadu_si128((const __m128i*)(const void*)(c + j)), _mm_cmplt_epi32(ix, lim)));
        ix = _mm_add_epi32(ix, step);
    }
    a = _mm_add_epi32(a, _mm_shuffle_epi32(a, 0x4E));
    a = _mm_add_epi32(a, _mm_shuffle_epi32(a, 0xB1));
    return (uint32_t)_mm_cvtsi128_si32(a);
#else
    uint32_t a = 0, j;
    for (j = 0; j < i; j++)
        a += c[j];
    return a;
#endif
}

static CBT_INLINE void cbt_shrink(const dyn_cbt_t* t, cbt_hdr* x, uint32_t n)
{
    uint32_t j = n, e = x->n;
    x->n = n;
    if (t->f64) {
        double* kv = (double*)(void*)((uint8_t*)x + sizeof(cbt_hdr));
        for (; j < e; j++)
            kv[j] = HUGE_VAL;
    }
}

static uint32_t cbt_node(dyn_cbt_t* t, int leaf)
{
    uint32_t s = dyn_chunk_alloc(leaf ? &t->lf : &t->nd);
    cbt_hdr* x;
    if (s == DYN_CHUNK_NIL)
        return s;
    if (leaf)
        s |= CBT_LF;
    x = cbt_n(t, s);
    x->n = CBT_MAXK;
    x->leaf = (uint32_t)leaf;
    cbt_shrink(t, x, 0);
    return s;
}

static void cbt_drop(dyn_cbt_t* t, uint32_t s)
{
    dyn_chunk_release((s & CBT_LF) ? &t->lf : &t->nd, s & ~CBT_LF);
}

static size_t cbt_sub(const dyn_cbt_t* t, cbt_hdr* x)
{
    size_t s = x->n;
    uint32_t i;
    if (!x->leaf)
        for (i = 0; i <= x->n; i++)
            s += cbt_cnt(t, x)[i];
    return s;
}

dyn_cbt_t* dyn_cbt_new(const dyn_ds_ops* key, const dyn_ds_ops* val)
{
    dyn_cbt_t* t = (dyn_cbt_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    ds_ops_init(&t->kops, key);
    ds_ops_init(&t->vops, val);
    t->ks = ds_align8(t->kops.size);
    t->vs = ds_align8(t->vops.size);
    t->f64 = t->kops.cmp == dyn_ds_cmp_f64 && t->kops.size == 8;
    t->kidoff = sizeof(cbt_hdr) + (size_t)CBT_MAXK * t->ks;
    t->kidoff = (t->kidoff + 3) & ~(size_t)3;
    t->cntoff = t->kidoff + CBT_MAXC * sizeof(uint32_t);
    t->voff = ds_align8(t->cntoff + CBT_MAXC * sizeof(uint32_t));
    t->esz = (uint32_t)(t->voff + (size_t)CBT_MAXK * t->vs);
    t->lvoff = ds_align8(sizeof(cbt_hdr) + (size_t)CBT_MAXK * t->ks);
    t->root = DYN_CHUNK_NIL;
    return t;
}

static int cbt_pool_new(dyn_cbt_t* t)
{
    dyn_chunk_init(&t->nd, t->esz, 0, 1, 1);
    dyn_chunk_init(&t->lf, t->lvoff + (size_t)CBT_MAXK * t->vs, 0, 1, 1);
    return 0;
}

static void cbt_release_all(dyn_cbt_t* t, uint32_t s)
{
    cbt_hdr* x = cbt_n(t, s);
    uint32_t i;
    for (i = 0; i < x->n; i++) {
        ds_ops_release(&t->kops, cbt_k(t, x, i));
        ds_ops_release(&t->vops, cbt_v(t, x, i));
    }
    if (!x->leaf)
        for (i = 0; i <= x->n; i++)
            cbt_release_all(t, cbt_kid(t, x)[i]);
}

void dyn_cbt_free(dyn_cbt_t* t)
{
    if (!t)
        return;
    if (t->sm) {
        uint32_t i;
        for (i = 0; i < t->sm->n; i++) {
            ds_ops_release(&t->kops, cbt_k(t, t->sm, i));
            ds_ops_release(&t->vops, cbt_sv(t, i));
        }
        DYN_DS_FREE(t->sm);
    }
    if (t->lf.ch) {
        if (t->root != DYN_CHUNK_NIL && (t->kops.release || t->vops.release))
            cbt_release_all(t, t->root);
        dyn_chunk_destroy(&t->nd);
        dyn_chunk_destroy(&t->lf);
    }
    DYN_DS_FREE(t);
}

size_t dyn_cbt_size(const dyn_cbt_t* t)
{
    return t->size;
}

size_t dyn_cbt_bytes(const dyn_cbt_t* t)
{
    return sizeof(*t) + (t->sm ? sizeof(cbt_hdr) + (size_t)t->scap * (t->ks + t->vs) : 0) + dyn_chunk_bytes(&t->nd) + dyn_chunk_bytes(&t->lf);
}

void* dyn_cbt_get(const dyn_cbt_t* t, const void* key)
{
    uint32_t s = t->root;
    if (!t->lf.ch) {
        int f = 0;
        uint32_t i = t->sm ? cbt_slb(t, t->sm, key, &f) : 0;
        return f ? cbt_sv(t, i) : NULL;
    }
    while (s != DYN_CHUNK_NIL) {
        cbt_hdr* x = cbt_n(t, s);
        int f;
        uint32_t i = cbt_lb(t, x, key, &f);
        if (f)
            return cbt_v(t, x, i);
        if (x->leaf)
            return NULL;
        s = cbt_kid(t, x)[i];
    }
    return NULL;
}

static void cbt_move(const dyn_cbt_t* t, cbt_hdr* dst, uint32_t di, cbt_hdr* src, uint32_t si, uint32_t n)
{
    if (!n)
        return;
    memmove(cbt_k(t, dst, di), cbt_k(t, src, si), (size_t)n * t->ks);
    memmove(cbt_v(t, dst, di), cbt_v(t, src, si), (size_t)n * t->vs);
}

static void cbt_move_kids(const dyn_cbt_t* t, cbt_hdr* dst, uint32_t di, cbt_hdr* src, uint32_t si, uint32_t n)
{
    if (!n)
        return;
    memmove(cbt_kid(t, dst) + di, cbt_kid(t, src) + si, (size_t)n * sizeof(uint32_t));
    memmove(cbt_cnt(t, dst) + di, cbt_cnt(t, src) + si, (size_t)n * sizeof(uint32_t));
}

static int cbt_split(dyn_cbt_t* t, uint32_t xs, uint32_t i, uint32_t m)
{
    uint32_t zs, ys, rn = CBT_MAXK - 1 - m;
    cbt_hdr *x, *y, *z;
    size_t zc;
    ys = cbt_kid(t, cbt_n(t, xs))[i];
    zs = cbt_node(t, (int)cbt_n(t, ys)->leaf);
    if (zs == DYN_CHUNK_NIL)
        return -1;
    x = cbt_n(t, xs);
    y = cbt_n(t, ys);
    z = cbt_n(t, zs);
    cbt_move(t, z, 0, y, m + 1, rn);
    if (!y->leaf)
        cbt_move_kids(t, z, 0, y, m + 1, rn + 1);
    z->n = rn;
    zc = cbt_sub(t, z);
    cbt_move_kids(t, x, i + 2, x, i + 1, x->n - i);
    cbt_move(t, x, i + 1, x, i, x->n - i);
    cbt_move(t, x, i, y, m, 1);
    cbt_shrink(t, y, m);
    cbt_kid(t, x)[i + 1] = zs;
    cbt_cnt(t, x)[i + 1] = (uint32_t)zc;
    cbt_cnt(t, x)[i] = (uint32_t)(cbt_cnt(t, x)[i] - zc - 1);
    x->n++;
    return 0;
}

static CBT_INLINE uint32_t cbt_edge(uint32_t edge, uint32_t i, uint32_t n)
{
    return ((edge & 1) && !i) | ((uint32_t)((edge & 2) && i == n) << 1);
}

static uint32_t cbt_cut(const dyn_cbt_t* t, cbt_hdr* y, const void* key, uint32_t edge)
{
    if (y->leaf) {
        if ((edge & 2) && cbt_cmp(t, key, cbt_k(t, y, CBT_MAXK - 1)) > 0)
            return CBT_MAXK - 1;
        if ((edge & 1) && cbt_cmp(t, key, cbt_k(t, y, 0)) < 0)
            return 0;
    }
    return CBT_T - 1;
}

static int cbt_small_grow(dyn_cbt_t* t)
{
    uint32_t nc = t->scap ? t->scap * 2 : 2, n = t->sm ? t->sm->n : 0;
    cbt_hdr* q;
    if (nc > CBT_SMALL)
        nc = CBT_SMALL;
    q = (cbt_hdr*)DYN_DS_REALLOC(t->sm, sizeof(cbt_hdr) + (size_t)nc * (t->ks + t->vs));
    if (!q)
        return -1;
    if (!t->sm) {
        q->n = 0;
        q->leaf = 1;
    }
    memmove((uint8_t*)q + sizeof(cbt_hdr) + (size_t)nc * t->ks, (uint8_t*)q + sizeof(cbt_hdr) + (size_t)t->scap * t->ks, (size_t)n * t->vs);
    t->sm = q;
    t->scap = nc;
    return 0;
}

static int cbt_to_tree(dyn_cbt_t* t)
{
    uint32_t n = t->sm->n, m = (n + 1 + CBT_FILL) / (CBT_FILL + 1), keys = n - (m - 1), p = 0, j, rs, c;
    cbt_hdr* r;
    if (cbt_pool_new(t) < 0)
        return -1;
    rs = cbt_node(t, 0);
    if (rs == DYN_CHUNK_NIL)
        goto fail;
    for (j = 0; j < m; j++) {
        uint32_t ls = cbt_node(t, 1);
        cbt_hdr* l;
        if (ls == DYN_CHUNK_NIL)
            goto fail;
        c = keys / m + (j < keys % m);
        l = cbt_n(t, ls);
        memcpy(cbt_k(t, l, 0), cbt_k(t, t->sm, p), (size_t)c * t->ks);
        memcpy(cbt_v(t, l, 0), cbt_sv(t, p), (size_t)c * t->vs);
        l->n = c;
        p += c;
        r = cbt_n(t, rs);
        cbt_kid(t, r)[j] = ls;
        cbt_cnt(t, r)[j] = c;
        if (j + 1 < m) {
            memcpy(cbt_k(t, r, j), cbt_k(t, t->sm, p), t->ks);
            memcpy(cbt_v(t, r, j), cbt_sv(t, p), t->vs);
            p++;
        }
    }
    cbt_n(t, rs)->n = m - 1;
    t->root = rs;
    DYN_DS_FREE(t->sm);
    t->sm = NULL;
    t->scap = 0;
    return 0;
fail:
    dyn_chunk_destroy(&t->nd);
    dyn_chunk_destroy(&t->lf);
    return -1;
}

static int cbt_small_put(dyn_cbt_t* t, const void* key, const void* val)
{
    cbt_hdr* x = t->sm;
    int f = 0;
    uint32_t i = x ? cbt_slb(t, x, key, &f) : 0;
    if (f) {
        ds_ops_release(&t->vops, cbt_sv(t, i));
        ds_ops_copy(&t->vops, cbt_sv(t, i), val);
        return 0;
    }
    if (!x || x->n == t->scap) {
        if (t->scap == CBT_SMALL)
            return cbt_to_tree(t) < 0 ? -1 : 2;
        if (cbt_small_grow(t) < 0)
            return -1;
        x = t->sm;
    }
    memmove(cbt_k(t, x, i + 1), cbt_k(t, x, i), (size_t)(x->n - i) * t->ks);
    memmove(cbt_sv(t, i + 1), cbt_sv(t, i), (size_t)(x->n - i) * t->vs);
    ds_ops_copy(&t->kops, cbt_k(t, x, i), key);
    ds_ops_copy(&t->vops, cbt_sv(t, i), val);
    x->n++;
    t->size++;
    return 1;
}

static int cbt_small_remove(dyn_cbt_t* t, const void* key)
{
    cbt_hdr* x = t->sm;
    int f = 0;
    uint32_t i = x ? cbt_slb(t, x, key, &f) : 0;
    if (!f)
        return 0;
    ds_ops_release(&t->kops, cbt_k(t, x, i));
    ds_ops_release(&t->vops, cbt_sv(t, i));
    memmove(cbt_k(t, x, i), cbt_k(t, x, i + 1), (size_t)(x->n - i - 1) * t->ks);
    memmove(cbt_sv(t, i), cbt_sv(t, i + 1), (size_t)(x->n - i - 1) * t->vs);
    x->n--;
    t->size--;
    if (!x->n) {
        DYN_DS_FREE(x);
        t->sm = NULL;
        t->scap = 0;
    }
    return 1;
}

int dyn_cbt_put(dyn_cbt_t* t, const void* key, const void* val)
{
    uint32_t ps[CBT_DEPTH], pi[CBT_DEPTH], depth = 0, s, d, edge = 3;
    if (!t->lf.ch) {
        int r = cbt_small_put(t, key, val);
        if (r != 2)
            return r;
    }
    if (t->root == DYN_CHUNK_NIL) {
        t->root = cbt_node(t, 1);
        if (t->root == DYN_CHUNK_NIL)
            return -1;
    }
    if (cbt_n(t, t->root)->n == CBT_MAXK) {
        uint32_t r = t->root, ns = cbt_node(t, 0);
        cbt_hdr* x;
        if (ns == DYN_CHUNK_NIL)
            return -1;
        x = cbt_n(t, ns);
        cbt_kid(t, x)[0] = r;
        cbt_cnt(t, x)[0] = (uint32_t)t->size;
        if (cbt_split(t, ns, 0, cbt_cut(t, cbt_n(t, r), key, 3)) < 0) {
            cbt_drop(t, ns);
            return -1;
        }
        t->root = ns;
    }
    s = t->root;
    for (;;) {
        cbt_hdr* x = cbt_n(t, s);
        int f;
        uint32_t i = cbt_lbw(t, x, key, &f);
        if (f) {
            ds_ops_release(&t->vops, cbt_v(t, x, i));
            ds_ops_copy(&t->vops, cbt_v(t, x, i), val);
            return 0;
        }
        if (x->leaf) {
            cbt_move(t, x, i + 1, x, i, x->n - i);
            ds_ops_copy(&t->kops, cbt_k(t, x, i), key);
            ds_ops_copy(&t->vops, cbt_v(t, x, i), val);
            x->n++;
            break;
        }
        if (cbt_n(t, cbt_kid(t, x)[i])->n == CBT_MAXK) {
            int c;
            if (cbt_split(t, s, i, cbt_cut(t, cbt_n(t, cbt_kid(t, x)[i]), key, cbt_edge(edge, i, x->n))) < 0)
                return -1;
            x = cbt_n(t, s);
            c = cbt_cmp(t, key, cbt_k(t, x, i));
            if (c == 0) {
                ds_ops_release(&t->vops, cbt_v(t, x, i));
                ds_ops_copy(&t->vops, cbt_v(t, x, i), val);
                return 0;
            }
            if (c > 0)
                i++;
        }
        edge = cbt_edge(edge, i, x->n);
        ps[depth] = s;
        pi[depth++] = i;
        s = cbt_kid(t, x)[i];
    }
    for (d = 0; d < depth; d++)
        cbt_cnt(t, cbt_n(t, ps[d]))[pi[d]]++;
    t->size++;
    return 1;
}

static void cbt_merge(dyn_cbt_t* t, uint32_t xs, uint32_t i)
{
    cbt_hdr* x = cbt_n(t, xs);
    uint32_t ys = cbt_kid(t, x)[i], zs = cbt_kid(t, x)[i + 1];
    cbt_hdr *y = cbt_n(t, ys), *z = cbt_n(t, zs);
    uint32_t c = cbt_cnt(t, x)[i] + cbt_cnt(t, x)[i + 1] + 1;
    cbt_move(t, y, y->n, x, i, 1);
    cbt_move(t, y, y->n + 1, z, 0, z->n);
    if (!y->leaf)
        cbt_move_kids(t, y, y->n + 1, z, 0, z->n + 1);
    y->n += z->n + 1;
    cbt_move(t, x, i, x, i + 1, x->n - i - 1);
    cbt_move_kids(t, x, i + 1, x, i + 2, x->n - i - 1);
    cbt_cnt(t, x)[i] = c;
    cbt_shrink(t, x, x->n - 1);
    cbt_drop(t, zs);
}

static uint32_t cbt_fix(dyn_cbt_t* t, uint32_t xs, uint32_t i)
{
    cbt_hdr* x = cbt_n(t, xs);
    cbt_hdr* c = cbt_n(t, cbt_kid(t, x)[i]);
    if (c->n >= CBT_T)
        return i;
    if (i > 0 && cbt_n(t, cbt_kid(t, x)[i - 1])->n >= CBT_T) {
        cbt_hdr* l = cbt_n(t, cbt_kid(t, x)[i - 1]);
        uint32_t moved = 1;
        cbt_move(t, c, 1, c, 0, c->n);
        cbt_move(t, c, 0, x, i - 1, 1);
        if (!c->leaf) {
            cbt_move_kids(t, c, 1, c, 0, c->n + 1);
            cbt_kid(t, c)[0] = cbt_kid(t, l)[l->n];
            cbt_cnt(t, c)[0] = cbt_cnt(t, l)[l->n];
            moved += cbt_cnt(t, c)[0];
        }
        cbt_move(t, x, i - 1, l, l->n - 1, 1);
        cbt_shrink(t, l, l->n - 1);
        c->n++;
        cbt_cnt(t, x)[i - 1] -= moved;
        cbt_cnt(t, x)[i] += moved;
        return i;
    }
    if (i < x->n && cbt_n(t, cbt_kid(t, x)[i + 1])->n >= CBT_T) {
        cbt_hdr* r = cbt_n(t, cbt_kid(t, x)[i + 1]);
        uint32_t moved = 1;
        cbt_move(t, c, c->n, x, i, 1);
        if (!c->leaf) {
            cbt_kid(t, c)[c->n + 1] = cbt_kid(t, r)[0];
            cbt_cnt(t, c)[c->n + 1] = cbt_cnt(t, r)[0];
            moved += cbt_cnt(t, r)[0];
            cbt_move_kids(t, r, 0, r, 1, r->n);
        }
        cbt_move(t, x, i, r, 0, 1);
        cbt_move(t, r, 0, r, 1, r->n - 1);
        cbt_shrink(t, r, r->n - 1);
        c->n++;
        cbt_cnt(t, x)[i + 1] -= moved;
        cbt_cnt(t, x)[i] += moved;
        return i;
    }
    if (i < x->n) {
        cbt_merge(t, xs, i);
        return i;
    }
    cbt_merge(t, xs, i - 1);
    return i - 1;
}

static int cbt_collapse(dyn_cbt_t* t, uint32_t* xs)
{
    cbt_hdr* x = cbt_n(t, *xs);
    if (*xs != t->root || x->n || x->leaf)
        return 0;
    t->root = cbt_kid(t, x)[0];
    cbt_drop(t, *xs);
    *xs = t->root;
    return 1;
}

static void cbt_pop(dyn_cbt_t* t, uint32_t ys, int max, uint8_t* kout, uint8_t* vout)
{
    for (;;) {
        cbt_hdr* y = cbt_n(t, ys);
        uint32_t i;
        if (y->leaf) {
            i = max ? y->n - 1 : 0;
            memcpy(kout, cbt_k(t, y, i), t->ks);
            memcpy(vout, cbt_v(t, y, i), t->vs);
            if (!max)
                cbt_move(t, y, 0, y, 1, y->n - 1);
            cbt_shrink(t, y, y->n - 1);
            return;
        }
        i = cbt_fix(t, ys, max ? y->n : 0);
        y = cbt_n(t, ys);
        cbt_cnt(t, y)[i]--;
        ys = cbt_kid(t, y)[i];
    }
}

int dyn_cbt_remove(dyn_cbt_t* t, const void* key)
{
    uint32_t ps[CBT_DEPTH], pi[CBT_DEPTH], depth = 0, s, d;
    int known = 0;
    if (!t->lf.ch)
        return cbt_small_remove(t, key);
    if (t->root == DYN_CHUNK_NIL)
        return 0;
    s = t->root;
    for (;;) {
        cbt_hdr* x = cbt_n(t, s);
        int f;
        uint32_t i = cbt_lbw(t, x, key, &f);
        if (x->leaf) {
            if (!f)
                return 0;
            ds_ops_release(&t->kops, cbt_k(t, x, i));
            ds_ops_release(&t->vops, cbt_v(t, x, i));
            cbt_move(t, x, i, x, i + 1, x->n - i - 1);
            cbt_shrink(t, x, x->n - 1);
            break;
        }
        if (f && !known) {
            for (d = 0; d < depth; d++)
                cbt_cnt(t, cbt_n(t, ps[d]))[pi[d]]--;
            depth = 0;
            known = 1;
        }
        if (f) {
            if (cbt_n(t, cbt_kid(t, x)[i])->n >= CBT_T) {
                ds_ops_release(&t->kops, cbt_k(t, x, i));
                ds_ops_release(&t->vops, cbt_v(t, x, i));
                cbt_cnt(t, x)[i]--;
                cbt_pop(t, cbt_kid(t, x)[i], 1, cbt_k(t, x, i), cbt_v(t, x, i));
                t->size--;
                return 1;
            }
            if (cbt_n(t, cbt_kid(t, x)[i + 1])->n >= CBT_T) {
                ds_ops_release(&t->kops, cbt_k(t, x, i));
                ds_ops_release(&t->vops, cbt_v(t, x, i));
                cbt_cnt(t, x)[i + 1]--;
                cbt_pop(t, cbt_kid(t, x)[i + 1], 0, cbt_k(t, x, i), cbt_v(t, x, i));
                t->size--;
                return 1;
            }
            cbt_merge(t, s, i);
        } else {
            i = cbt_fix(t, s, i);
        }
        if (cbt_collapse(t, &s))
            continue;
        x = cbt_n(t, s);
        if (known) {
            cbt_cnt(t, x)[i]--;
        } else {
            ps[depth] = s;
            pi[depth++] = i;
        }
        s = cbt_kid(t, x)[i];
    }
    for (d = 0; d < depth; d++)
        cbt_cnt(t, cbt_n(t, ps[d]))[pi[d]]--;
    t->size--;
    return 1;
}

size_t dyn_cbt_rank(const dyn_cbt_t* t, const void* key)
{
    size_t r = 0;
    uint32_t s = t->root;
    if (!t->lf.ch) {
        int f;
        return t->sm ? cbt_slb(t, t->sm, key, &f) : 0;
    }
    while (s != DYN_CHUNK_NIL) {
        cbt_hdr* x = cbt_n(t, s);
        int f;
        uint32_t i = cbt_lb(t, x, key, &f);
        r += i;
        if (x->leaf)
            return r;
        r += cbt_psum(cbt_cnt(t, x), i);
        if (f)
            return r + cbt_cnt(t, x)[i];
        s = cbt_kid(t, x)[i];
    }
    return r;
}

int dyn_cbt_at(const dyn_cbt_t* t, size_t idx, const void** key, void** val)
{
    uint32_t s = t->root, j;
    if (idx >= t->size)
        return -1;
    if (!t->lf.ch) {
        if (key)
            *key = cbt_k(t, t->sm, (uint32_t)idx);
        if (val)
            *val = cbt_sv(t, (uint32_t)idx);
        return 0;
    }
    while (s != DYN_CHUNK_NIL) {
        cbt_hdr* x = cbt_n(t, s);
        if (x->leaf) {
            j = (uint32_t)idx;
            goto hit;
        }
        for (j = 0; j <= x->n; j++) {
            if (idx < cbt_cnt(t, x)[j])
                break;
            idx -= cbt_cnt(t, x)[j];
            if (j < x->n) {
                if (!idx)
                    goto hit;
                idx--;
            }
        }
        s = cbt_kid(t, x)[j];
        continue;
    hit:
        if (key)
            *key = cbt_k(t, x, j);
        if (val)
            *val = cbt_v(t, x, j);
        return 0;
    }
    return -1;
}

static int cbt_walk(const dyn_cbt_t* t, uint32_t s, dyn_cbt_visit_fn fn, void* ud)
{
    cbt_hdr* x = cbt_n(t, s);
    uint32_t i;
    for (i = 0; i <= x->n; i++) {
        int rc;
        if (!x->leaf && (rc = cbt_walk(t, cbt_kid(t, x)[i], fn, ud)) != 0)
            return rc;
        if (i < x->n && (rc = fn(cbt_k(t, x, i), cbt_v(t, x, i), ud)) != 0)
            return rc;
    }
    return 0;
}

int dyn_cbt_walk(const dyn_cbt_t* t, dyn_cbt_visit_fn fn, void* ud)
{
    if (!t->lf.ch) {
        uint32_t i;
        int rc;
        for (i = 0; t->sm && i < t->sm->n; i++)
            if ((rc = fn(cbt_k(t, t->sm, i), cbt_sv(t, i), ud)) != 0)
                return rc;
        return 0;
    }
    if (t->root == DYN_CHUNK_NIL)
        return 0;
    return cbt_walk(t, t->root, fn, ud);
}

static long cbt_check_node(const dyn_cbt_t* t, uint32_t s, const void* lo, const void* hi, int depth, int* leafd, int root, uint32_t edge)
{
    cbt_hdr* x = cbt_n(t, s);
    uint32_t i;
    long total = x->n;
    if (x->n > CBT_MAXK || (!root && x->n < (x->leaf && edge ? 1u : CBT_T - 1)) || (root && !x->leaf && !x->n))
        return -1;
    if (t->f64)
        for (i = x->n; i < CBT_MAXK; i++)
            if (((const double*)(const void*)((const uint8_t*)x + sizeof(cbt_hdr)))[i] != HUGE_VAL)
                return -1;
    for (i = 0; i < x->n; i++) {
        if (i && cbt_cmp(t, cbt_k(t, x, i - 1), cbt_k(t, x, i)) >= 0)
            return -1;
        if ((lo && cbt_cmp(t, cbt_k(t, x, i), lo) <= 0) || (hi && cbt_cmp(t, cbt_k(t, x, i), hi) >= 0))
            return -1;
    }
    if (x->leaf) {
        if (*leafd < 0)
            *leafd = depth;
        return *leafd == depth ? total : -1;
    }
    for (i = 0; i <= x->n; i++) {
        long c = cbt_check_node(t, cbt_kid(t, x)[i], i ? cbt_k(t, x, i - 1) : lo, i < x->n ? cbt_k(t, x, i) : hi, depth + 1, leafd, 0, cbt_edge(edge, i, x->n));
        if (c < 0 || (uint32_t)c != cbt_cnt(t, x)[i])
            return -1;
        total += c;
    }
    return total;
}

int dyn_cbt_check(const dyn_cbt_t* t)
{
    int leafd = -1;
    long n;
    if (!t->lf.ch) {
        uint32_t i;
        if (!t->sm)
            return t->size || t->scap ? -1 : 0;
        if (t->sm->n != t->size || !t->sm->n || t->sm->n > t->scap || t->scap > CBT_SMALL)
            return -1;
        for (i = 1; i < t->sm->n; i++)
            if (cbt_cmp(t, cbt_k(t, t->sm, i - 1), cbt_k(t, t->sm, i)) >= 0)
                return -1;
        return 0;
    }
    if (t->root == DYN_CHUNK_NIL)
        return t->size ? -1 : 0;
    n = cbt_check_node(t, t->root, NULL, NULL, 0, &leafd, 1, 3);
    if (n < 0 || (size_t)n != t->size)
        return -1;
    return 0;
}

const void* dyn_cbt_bound(const dyn_cbt_t* t, const void* key, int floor)
{
    const void* best = NULL;
    uint32_t s = t->root;
    if (!t->lf.ch) {
        int f;
        uint32_t i;
        if (!t->sm)
            return NULL;
        i = cbt_slb(t, t->sm, key, &f);
        if (f)
            return cbt_k(t, t->sm, i);
        return floor ? (i ? cbt_k(t, t->sm, i - 1) : NULL) : (i < t->sm->n ? cbt_k(t, t->sm, i) : NULL);
    }
    while (s != DYN_CHUNK_NIL) {
        cbt_hdr* x = cbt_n(t, s);
        int f;
        uint32_t i = cbt_lb(t, x, key, &f);
        if (f)
            return cbt_k(t, x, i);
        if (floor ? i > 0 : i < x->n)
            best = cbt_k(t, x, floor ? i - 1 : i);
        if (x->leaf)
            break;
        s = cbt_kid(t, x)[i];
    }
    return best;
}
