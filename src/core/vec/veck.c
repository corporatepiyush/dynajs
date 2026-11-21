#include <math.h>
#include "veck-int.h"

static uint64_t vk_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

int dyn_vec_init(dyn_vec_t* v, const dyn_vec_params_t* p)
{
    memset(v, 0, sizeof(*v));
    v->dim = p->dim;
    v->metric = p->metric;
    v->kind = p->kind;
    v->M = p->M;
    v->M0 = p->M * 2;
    v->efc = p->efc;
    v->efs = p->efs;
    v->heuristic = p->heuristic ? 1 : 0;
    v->exact_fallback = p->exact_fallback ? 1 : 0;
    v->rng = vk_mix(p->seed);
    v->ml = p->M > 1 ? 1.0 / log((double)p->M) : 1.0;
    v->top = -1;
    v->entry = DYN_VEC_NONE;
    v->quant = p->quant;
    v->keep = p->quant == DYN_VEC_F32 || p->rerank ? 1 : 0;
    v->pool = p->quant == DYN_VEC_F32 || !v->keep ? 0 : p->pool ? p->pool : p->quant == DYN_VEC_BINARY ? DYN_VEC_BINARY_POOL : 1;
    v->words = (p->dim + 63) / 64;
    v->cb = p->quant == DYN_VEC_SQ8 ? p->dim : p->quant == DYN_VEC_BINARY ? v->words * 8 : 0;
    v->qbuf = (float*)malloc((size_t)p->dim * sizeof(float));
    if (!v->qbuf)
        return DYN_VEC_ENOMEM;
    if (p->quant != DYN_VEC_F32) {
        v->qcbuf = (int8_t*)calloc((size_t)v->words * 8 + v->cb, 1);
        if (!v->qcbuf)
            return DYN_VEC_ENOMEM;
    }
    if (p->quant == DYN_VEC_SQ8) {
        v->lo = (double*)calloc(p->dim, sizeof(double));
        v->hi = (double*)calloc(p->dim, sizeof(double));
        v->mid = (double*)calloc(p->dim, sizeof(double));
        if (!v->lo || !v->hi || !v->mid)
            return DYN_VEC_ENOMEM;
    }
    return DYN_VEC_OK;
}

static int vk_in_pool(const dyn_vec_t* v, const uint32_t* p)
{
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)v->lpool;
    return v->lpool && a >= b && a < b + (uintptr_t)(v->lwords * sizeof(uint32_t));
}

void dyn_vec_free(dyn_vec_t* v)
{
    uint32_t i;
    if (v->links)
        for (i = 0; i < v->n; i++)
            if (!vk_in_pool(v, v->links[i]))
                free(v->links[i]);
    free(v->links);
    free(v->lpool);
    if (!v->data_borrowed)
        free(v->data);
    free(v->ext);
    free(v->dead);
    free(v->hkey);
    free(v->hval);
    free(v->lvl);
    free(v->vis);
    free(v->cand);
    free(v->res);
    free(v->tmp);
    free(v->qbuf);
    vk_quant_free(v);
    memset(v, 0, sizeof(*v));
}

static inline uint32_t vk_hash(uint64_t id, uint32_t mask)
{
    return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

static void vk_hput(uint64_t* key, uint32_t* val, uint32_t cap, uint64_t id, uint32_t slot)
{
    uint32_t m = cap - 1, i = vk_hash(id, m);
    while (key[i] != DYN_VEC_NOID)
        i = (i + 1) & m;
    key[i] = id;
    val[i] = slot;
}

uint32_t dyn_vec_find(const dyn_vec_t* v, uint64_t id)
{
    uint32_t m, i;
    if (!v->hcap || id == DYN_VEC_NOID)
        return DYN_VEC_NONE;
    m = v->hcap - 1;
    for (i = vk_hash(id, m);; i = (i + 1) & m) {
        if (v->hkey[i] == id)
            return v->hval[i];
        if (v->hkey[i] == DYN_VEC_NOID)
            return DYN_VEC_NONE;
    }
}

int vk_rehash(dyn_vec_t* v, uint32_t ncap)
{
    uint32_t hc = 16, i, *val;
    uint64_t* k;
    while (hc < ncap * 2u && hc < 0x80000000u)
        hc <<= 1;
    if (hc <= v->hcap)
        return DYN_VEC_OK;
    k = (uint64_t*)malloc((size_t)hc * sizeof(uint64_t));
    val = (uint32_t*)malloc((size_t)hc * sizeof(uint32_t));
    if (!k || !val) {
        free(k);
        free(val);
        return DYN_VEC_ENOMEM;
    }
    memset(k, 0xFF, (size_t)hc * sizeof(uint64_t));
    for (i = 0; i < v->n; i++)
        if (!v->dead[i])
            vk_hput(k, val, hc, v->ext[i], i);
    free(v->hkey);
    free(v->hval);
    v->hkey = k;
    v->hval = val;
    v->hcap = hc;
    return DYN_VEC_OK;
}

int dyn_vec_reserve(dyn_vec_t* v, uint32_t extra)
{
    uint64_t need = (uint64_t)v->n + extra, nc;
    void* p;
    if (need > DYN_VEC_MAX_SIZE)
        return DYN_VEC_EFULL;
    if (need <= v->cap)
        return DYN_VEC_OK;
    nc = v->cap ? (uint64_t)v->cap * 2 : 16;
    if (nc < need)
        nc = need;
    if (nc > DYN_VEC_MAX_SIZE)
        nc = DYN_VEC_MAX_SIZE;
    if (v->keep) {
        if (v->data_borrowed) {
            p = malloc((size_t)nc * v->dim * sizeof(float));
            if (p && v->n)
                memcpy(p, v->data, (size_t)v->n * v->dim * sizeof(float));
        } else {
            p = realloc(v->data, (size_t)nc * v->dim * sizeof(float));
        }
        if (!p)
            return DYN_VEC_ENOMEM;
        v->data = (float*)p;
        v->data_borrowed = 0;
    }
    if (vk_quant_alloc(v, (uint32_t)nc))
        return DYN_VEC_ENOMEM;
    p = realloc(v->ext, (size_t)nc * sizeof(uint64_t));
    if (!p)
        return DYN_VEC_ENOMEM;
    v->ext = (uint64_t*)p;
    p = realloc(v->dead, (size_t)nc);
    if (!p)
        return DYN_VEC_ENOMEM;
    v->dead = (uint8_t*)p;
    memset(v->dead + v->cap, 0, (size_t)(nc - v->cap));
    if (v->kind == DYN_VEC_HNSW) {
        p = realloc(v->lvl, (size_t)nc);
        if (!p)
            return DYN_VEC_ENOMEM;
        v->lvl = (uint8_t*)p;
        p = realloc(v->links, (size_t)nc * sizeof(uint32_t*));
        if (!p)
            return DYN_VEC_ENOMEM;
        v->links = (uint32_t**)p;
        p = realloc(v->vis, (size_t)nc * sizeof(uint32_t));
        if (!p)
            return DYN_VEC_ENOMEM;
        v->vis = (uint32_t*)p;
        memset(v->vis + v->cap, 0, (size_t)(nc - v->cap) * sizeof(uint32_t));
    }
    if (vk_rehash(v, (uint32_t)nc))
        return DYN_VEC_ENOMEM;
    v->cap = (uint32_t)nc;
    return DYN_VEC_OK;
}

static double vk_sumsq(const float* x, uint32_t d)
{
    uint32_t i;
    double s = 0;
    for (i = 0; i < d; i++) {
        double t = (double)x[i] * (double)x[i];
        s = s + t;
    }
    return s;
}

int dyn_vec_check(const dyn_vec_t* v, const float* x)
{
    uint32_t i;
    for (i = 0; i < v->dim; i++)
        if (!isfinite(x[i]))
            return DYN_VEC_ENONFINITE;
    if (v->metric == DYN_VEC_COSINE && !(vk_sumsq(x, v->dim) > 0))
        return DYN_VEC_EZERO;
    return DYN_VEC_OK;
}

static void vk_store(const dyn_vec_t* v, float* dst, const float* x)
{
    uint32_t i;
    if (v->metric == DYN_VEC_COSINE) {
        double inv = 1.0 / sqrt(vk_sumsq(x, v->dim));
        for (i = 0; i < v->dim; i++)
            dst[i] = (float)((double)x[i] * inv);
    } else {
        memcpy(dst, x, (size_t)v->dim * sizeof(float));
    }
}

int dyn_vec_add(dyn_vec_t* v, uint64_t id, const float* x)
{
    int rc;
    uint32_t slot;
    if (id == DYN_VEC_NOID || dyn_vec_find(v, id) != DYN_VEC_NONE)
        return DYN_VEC_EDUP;
    rc = dyn_vec_check(v, x);
    if (rc)
        return rc;
    rc = dyn_vec_reserve(v, 1);
    if (rc)
        return rc;
    slot = v->n;
    {
        float* dst = v->keep ? v->data + (size_t)slot * v->dim : v->qbuf;
        vk_store(v, dst, x);
        if (v->quant == DYN_VEC_SQ8 && (rc = vk_sq8_cover(v, dst, 1)))
            return rc;
        vk_encode(v, slot, dst);
    }
    v->ext[slot] = id;
    v->dead[slot] = 0;
    if (v->kind == DYN_VEC_HNSW) {
        rc = vk_hnsw_insert(v, slot);
        if (rc)
            return rc;
    }
    vk_hput(v->hkey, v->hval, v->hcap, id, slot);
    v->n = slot + 1;
    v->live++;
    return DYN_VEC_OK;
}

int dyn_vec_train(dyn_vec_t* v, const float* x, uint32_t nb)
{
    uint32_t i, j, d = v->dim;
    double *blo, *bhi;
    int rc, any = 0;
    if (v->quant != DYN_VEC_SQ8 || nb == 0)
        return DYN_VEC_OK;
    blo = (double*)malloc((size_t)d * sizeof(double));
    bhi = (double*)malloc((size_t)d * sizeof(double));
    if (!blo || !bhi) {
        free(blo);
        free(bhi);
        return DYN_VEC_ENOMEM;
    }
    for (j = 0; j < d; j++) {
        blo[j] = HUGE_VAL;
        bhi[j] = -HUGE_VAL;
    }
    for (i = 0; i < nb; i++) {
        if (dyn_vec_check(v, x + (size_t)i * d))
            continue;
        any = 1;
        vk_store(v, v->qbuf, x + (size_t)i * d);
        for (j = 0; j < d; j++) {
            double t = (double)v->qbuf[j];
            if (t < blo[j])
                blo[j] = t;
            if (t > bhi[j])
                bhi[j] = t;
        }
    }
    rc = any ? vk_sq8_widen(v, blo, bhi) : DYN_VEC_OK;
    free(blo);
    free(bhi);
    return rc;
}

int dyn_vec_remove(dyn_vec_t* v, uint64_t id)
{
    uint32_t m, i, j, slot = dyn_vec_find(v, id);
    if (slot == DYN_VEC_NONE)
        return 0;
    m = v->hcap - 1;
    for (i = vk_hash(id, m); v->hkey[i] != id; i = (i + 1) & m)
        ;
    for (j = (i + 1) & m; v->hkey[j] != DYN_VEC_NOID; j = (j + 1) & m) {
        uint32_t h = vk_hash(v->hkey[j], m);
        if (((j - h) & m) >= ((j - i) & m)) {
            v->hkey[i] = v->hkey[j];
            v->hval[i] = v->hval[j];
            i = j;
        }
    }
    v->hkey[i] = DYN_VEC_NOID;
    v->dead[slot] = 1;
    v->live--;
    return 1;
}

uint64_t dyn_vec_filter_card(const dyn_vec_filter_t* f)
{
    if (!f)
        return 0;
    if (f->ids)
        return f->nids;
    return f->roar ? dyn_roar_card(f->roar) : dyn_bitset_count(f->bits);
}

static int vk_hit_cmp(const void* a, const void* b)
{
    const dyn_vec_hit_t* x = (const dyn_vec_hit_t*)a;
    const dyn_vec_hit_t* y = (const dyn_vec_hit_t*)b;
    return vk_lt(*x, *y) ? -1 : vk_lt(*y, *x) ? 1 : 0;
}

static void vk_topk_offer(dyn_vec_t* v, uint32_t k, uint32_t* nr, uint32_t slot)
{
    dyn_vec_hit_t h;
    h.d = vk_qd(v, slot);
    h.s = v->ext[slot];
    if (*nr < k) {
        vk_maxpush(v->res, nr, h);
    } else if (vk_lt(h, v->res[0])) {
        vk_maxpop(v->res, nr);
        vk_maxpush(v->res, nr, h);
    }
}

static void vk_drain(dyn_vec_t* v, uint32_t nr, dyn_vec_hit_t* out, uint32_t* nout)
{
    uint32_t i = nr;
    *nout = nr;
    while (i)
        out[--i] = vk_maxpop(v->res, &nr);
}

static int vk_exact(dyn_vec_t* v, uint32_t k, const dyn_vec_filter_t* f,
    dyn_vec_hit_t* out, uint32_t* nout)
{
    uint32_t nr = 0, i, slot;
    uint64_t card;
    if (vk_grow(&v->res, &v->rcap, k + 1))
        return DYN_VEC_ENOMEM;
    card = f ? dyn_vec_filter_card(f) : 0;
    if (f && card < v->live) {
        if (f->ids) {
            uint64_t x;
            for (x = 0; x < f->nids; x++)
                if ((slot = dyn_vec_find(v, f->ids[x])) != DYN_VEC_NONE)
                    vk_topk_offer(v, k, &nr, slot);
        } else if (f->roar) {
            uint32_t* ids = (uint32_t*)malloc((size_t)(card ? card : 1) * sizeof(uint32_t));
            if (!ids)
                return DYN_VEC_ENOMEM;
            dyn_roar_fill(f->roar, ids);
            for (i = 0; i < (uint32_t)card; i++)
                if ((slot = dyn_vec_find(v, ids[i])) != DYN_VEC_NONE)
                    vk_topk_offer(v, k, &nr, slot);
            free(ids);
        } else {
            int64_t b;
            for (b = dyn_bitset_next_set(f->bits, 0); b >= 0;) {
                if ((slot = dyn_vec_find(v, (uint32_t)b)) != DYN_VEC_NONE)
                    vk_topk_offer(v, k, &nr, slot);
                if ((uint64_t)b >= 0xFFFFFFFEu)
                    break;
                b = dyn_bitset_next_set(f->bits, (uint32_t)b + 1);
            }
        }
    } else {
        for (slot = 0; slot < v->n; slot++)
            if (vk_admit(v, f, slot))
                vk_topk_offer(v, k, &nr, slot);
    }
    vk_drain(v, nr, out, nout);
    return DYN_VEC_OK;
}

static void vk_rerank(dyn_vec_t* v, dyn_vec_hit_t* h, uint32_t nh, int by_ext)
{
    uint32_t i;
    if (v->quant == DYN_VEC_F32 || !v->keep)
        return;
    for (i = 0; i < nh; i++) {
        uint32_t slot = by_ext ? dyn_vec_find(v, h[i].s) : (uint32_t)h[i].s;
        h[i].d = vk_dist(v, v->qbuf, vk_vec(v, slot));
    }
}

static uint32_t vk_pool(const dyn_vec_t* v, uint32_t ef)
{
    uint64_t r = (uint64_t)ef * (v->pool ? v->pool : 1), cap = v->n > ef ? v->n : ef;
    return (uint32_t)(r < cap ? r : cap);
}

static int vk_exact_ranked(dyn_vec_t* v, uint32_t k, uint32_t ef, const dyn_vec_filter_t* f,
    dyn_vec_hit_t* out, uint32_t* nout)
{
    uint32_t r = v->quant == DYN_VEC_F32 ? k : ef, nr;
    int rc;
    if (r == k)
        return vk_exact(v, k, f, out, nout);
    if (vk_grow(&v->tmp, &v->tcap, r + 1))
        return DYN_VEC_ENOMEM;
    rc = vk_exact(v, r, f, v->tmp, &nr);
    if (rc)
        return rc;
    vk_rerank(v, v->tmp, nr, 1);
    qsort(v->tmp, nr, sizeof(dyn_vec_hit_t), vk_hit_cmp);
    if (nr > k)
        nr = k;
    memcpy(out, v->tmp, (size_t)nr * sizeof(dyn_vec_hit_t));
    *nout = nr;
    return DYN_VEC_OK;
}

int dyn_vec_search(dyn_vec_t* v, const float* q, uint32_t k, uint32_t ef,
    const dyn_vec_filter_t* f, dyn_vec_hit_t* out, uint32_t* nout)
{
    int rc;
    uint32_t nr, i;
    *nout = 0;
    rc = dyn_vec_check(v, q);
    if (rc)
        return rc;
    vk_store(v, v->qbuf, q);
    if (v->live == 0 || k == 0)
        return DYN_VEC_OK;
    vk_query_vec(v, v->qbuf);
    if (ef < k)
        ef = k;
    ef = vk_pool(v, ef);
    if (v->kind == DYN_VEC_FLAT)
        return vk_exact_ranked(v, k, ef, f, out, nout);
    if (f && v->exact_fallback) {
        uint64_t card = dyn_vec_filter_card(f);
        if (card <= ef || card * 16 <= v->live)
            return vk_exact_ranked(v, k, ef, f, out, nout);
    }
    rc = vk_hnsw_search(v, ef, f, &nr);
    if (rc)
        return rc;
    if (vk_grow(&v->tmp, &v->tcap, nr + 1))
        return DYN_VEC_ENOMEM;
    for (i = 0; i < nr; i++) {
        v->tmp[i] = v->res[i];
    }
    vk_rerank(v, v->tmp, nr, 0);
    for (i = 0; i < nr; i++)
        v->tmp[i].s = v->ext[v->tmp[i].s];
    qsort(v->tmp, nr, sizeof(dyn_vec_hit_t), vk_hit_cmp);
    if (nr > k)
        nr = k;
    memcpy(out, v->tmp, (size_t)nr * sizeof(dyn_vec_hit_t));
    *nout = nr;
    return DYN_VEC_OK;
}
