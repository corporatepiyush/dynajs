#ifndef VECK_INT_H
#define VECK_INT_H

#include <stdlib.h>
#include <string.h>
#include "veck.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

static inline int vk_lt(dyn_vec_hit_t a, dyn_vec_hit_t b)
{
    return a.d < b.d || (a.d == b.d && a.s < b.s);
}

static inline double vk_dist(const dyn_vec_t* v, const float* a, const float* b)
{
    uint32_t i, d = v->dim;
    double s = 0;
    if (v->metric == DYN_VEC_L2) {
        for (i = 0; i < d; i++) {
            double t = (double)a[i] - (double)b[i];
            double t2 = t * t;
            s = s + t2;
        }
        return s;
    }
    for (i = 0; i < d; i++) {
        double p = (double)a[i] * (double)b[i];
        s = s + p;
    }
    return v->metric == DYN_VEC_COSINE ? 1.0 - s : -s;
}

static inline const float* vk_vec(const dyn_vec_t* v, uint32_t slot)
{
    return v->data + (size_t)slot * v->dim;
}

static inline const int8_t* vk_sq(const dyn_vec_t* v, uint32_t slot)
{
    return (const int8_t*)v->codes + (size_t)slot * v->cb;
}

static inline const uint64_t* vk_bits(const dyn_vec_t* v, uint32_t slot)
{
    return (const uint64_t*)(const void*)(v->codes + (size_t)slot * v->cb);
}

static inline int64_t vk_sq8_l2i(const int8_t* a, const int8_t* b, uint32_t d)
{
    int64_t s = 0;
    uint32_t i = 0;
    while (i < d) {
        uint32_t e = d - i > 32768u ? i + 32768u : d;
        int32_t t = 0;
        for (; i < e; i++) {
            int32_t x = (int32_t)a[i] - (int32_t)b[i];
            t += x * x;
        }
        s += t;
    }
    return s;
}

static inline int64_t vk_sq8_doti(const int8_t* a, const int8_t* b, uint32_t d)
{
    int32_t t = 0;
    uint32_t i;
    for (i = 0; i < d; i++)
        t += (int32_t)a[i] * (int32_t)b[i];
    return t;
}

static inline double vk_sq8_dist(const dyn_vec_t* v, const int8_t* a, double ca, const int8_t* b, double cb)
{
    double s2 = v->scale * v->scale, dot;
    if (v->metric == DYN_VEC_L2)
        return s2 * (double)vk_sq8_l2i(a, b, v->dim);
    dot = v->msq + v->scale * (ca + cb) + s2 * (double)vk_sq8_doti(a, b, v->dim);
    return v->metric == DYN_VEC_COSINE ? 1.0 - dot : -dot;
}

static inline double vk_ham(const uint64_t* a, const uint64_t* b, uint32_t w)
{
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < w; i++)
        s += (uint64_t)__builtin_popcountll(a[i] ^ b[i]);
    return (double)s;
}

static inline double vk_l2f(const dyn_vec_t* v, const float* a, const float* b)
{
    uint32_t i;
    double s = 0;
    for (i = 0; i < v->dim; i++) {
        double t = (double)a[i] - (double)b[i];
        double t2 = t * t;
        s = s + t2;
    }
    return s;
}

static inline double vk_qd(const dyn_vec_t* v, uint32_t s)
{
    if (v->qmode == 0)
        return vk_dist(v, v->qf, vk_vec(v, s));
    if (v->quant == DYN_VEC_SQ8)
        return vk_sq8_dist(v, v->qc, v->qcorr, vk_sq(v, s), v->corr ? v->corr[s] : 0);
    return vk_ham(v->qb, vk_bits(v, s), v->words);
}

static inline double vk_pd(const dyn_vec_t* v, uint32_t a, uint32_t b)
{
    if (v->keep)
        return vk_dist(v, vk_vec(v, a), vk_vec(v, b));
    if (v->quant == DYN_VEC_SQ8)
        return vk_sq8_dist(v, vk_sq(v, a), v->corr ? v->corr[a] : 0, vk_sq(v, b), v->corr ? v->corr[b] : 0);
    return vk_ham(vk_bits(v, a), vk_bits(v, b), v->words);
}

static inline double vk_pl2(const dyn_vec_t* v, uint32_t a, uint32_t b)
{
    if (v->keep)
        return vk_l2f(v, vk_vec(v, a), vk_vec(v, b));
    if (v->quant == DYN_VEC_SQ8)
        return v->scale * v->scale * (double)vk_sq8_l2i(vk_sq(v, a), vk_sq(v, b), v->dim);
    return vk_ham(vk_bits(v, a), vk_bits(v, b), v->words);
}

static inline void vk_prefetch(const dyn_vec_t* v, uint32_t s)
{
    if (v->qmode == 0)
        __builtin_prefetch(vk_vec(v, s));
    else
        __builtin_prefetch(v->codes + (size_t)s * v->cb);
}

static inline int vk_grow(dyn_vec_hit_t** p, uint32_t* cap, uint32_t need)
{
    uint32_t nc;
    dyn_vec_hit_t* q;
    if (need <= *cap)
        return 0;
    nc = *cap ? *cap : 64;
    while (nc < need)
        nc = nc > 0x7FFFFFFFu ? need : nc * 2;
    q = (dyn_vec_hit_t*)realloc(*p, (size_t)nc * sizeof(*q));
    if (!q)
        return -1;
    *p = q;
    *cap = nc;
    return 0;
}

static inline void vk_minpush(dyn_vec_hit_t* h, uint32_t* n, dyn_vec_hit_t x)
{
    uint32_t i = (*n)++;
    while (i > 0) {
        uint32_t p = (i - 1) >> 1;
        if (!vk_lt(x, h[p]))
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
}

static inline dyn_vec_hit_t vk_minpop(dyn_vec_hit_t* h, uint32_t* n)
{
    dyn_vec_hit_t top = h[0], x = h[--(*n)];
    uint32_t i = 0, m = *n;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= m)
            break;
        if (c + 1 < m && vk_lt(h[c + 1], h[c]))
            c++;
        if (!vk_lt(h[c], x))
            break;
        h[i] = h[c];
        i = c;
    }
    if (m)
        h[i] = x;
    return top;
}

static inline void vk_maxpush(dyn_vec_hit_t* h, uint32_t* n, dyn_vec_hit_t x)
{
    uint32_t i = (*n)++;
    while (i > 0) {
        uint32_t p = (i - 1) >> 1;
        if (!vk_lt(h[p], x))
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
}

static inline dyn_vec_hit_t vk_maxpop(dyn_vec_hit_t* h, uint32_t* n)
{
    dyn_vec_hit_t top = h[0], x = h[--(*n)];
    uint32_t i = 0, m = *n;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= m)
            break;
        if (c + 1 < m && vk_lt(h[c], h[c + 1]))
            c++;
        if (!vk_lt(x, h[c]))
            break;
        h[i] = h[c];
        i = c;
    }
    if (m)
        h[i] = x;
    return top;
}

static inline int vk_in_ids(const uint64_t* a, uint64_t n, uint64_t id)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + ((hi - lo) >> 1);
        if (a[mid] < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && a[lo] == id;
}

static inline int vk_in_filter(const dyn_vec_filter_t* f, uint64_t id)
{
    if (f->ids)
        return vk_in_ids(f->ids, f->nids, id);
    if (id > 0xFFFFFFFFull)
        return 0;
    if (f->roar)
        return dyn_roar_has(f->roar, (uint32_t)id);
    return dyn_bitset_get(f->bits, (uint32_t)id);
}

static inline int vk_admit(const dyn_vec_t* v, const dyn_vec_filter_t* f, uint32_t slot)
{
    if (v->dead[slot])
        return 0;
    return !f || vk_in_filter(f, v->ext[slot]);
}

uint32_t* vk_links(const dyn_vec_t* v, uint32_t slot, int level);

int vk_hnsw_insert(dyn_vec_t* v, uint32_t slot);

int vk_hnsw_search(dyn_vec_t* v, uint32_t ef, const dyn_vec_filter_t* f, uint32_t* nres);

uint32_t vk_next_tag(dyn_vec_t* v);

int vk_rehash(dyn_vec_t* v, uint32_t ncap);

int vk_encode(dyn_vec_t* v, uint32_t slot, const float* x);

void vk_query_slot(dyn_vec_t* v, uint32_t slot);

int vk_query_vec(dyn_vec_t* v, const float* q);

int vk_quant_alloc(dyn_vec_t* v, uint32_t nc);

void vk_quant_free(dyn_vec_t* v);

int vk_sq8_refresh(dyn_vec_t* v);

int vk_sq8_widen(dyn_vec_t* v, const double* blo, const double* bhi);

int vk_sq8_cover(dyn_vec_t* v, const float* x, uint32_t nb);

#endif
