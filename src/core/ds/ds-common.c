#include "ds-internal.h"
#include "ds-heap.h"

#define DS_XXH_P3 0x165667B19E3779F9ULL
#define DS_XXH_P5 0x27D4EB2F165667C5ULL

static uint64_t ds_read64le(const uint8_t* p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t ds_read32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t dyn_ds_xxh64(const void* data, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t*)data, *end = p + len;
    uint64_t h;

    if (len >= 32) {
        const uint8_t* limit = end - 32;
        uint64_t v1 = seed + DS_XXH_P1 + DS_XXH_P2;
        uint64_t v2 = seed + DS_XXH_P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - DS_XXH_P1;
        do {
            v1 = ds_xxh_round(v1, ds_read64le(p));
            v2 = ds_xxh_round(v2, ds_read64le(p + 8));
            v3 = ds_xxh_round(v3, ds_read64le(p + 16));
            v4 = ds_xxh_round(v4, ds_read64le(p + 24));
            p += 32;
        } while (p <= limit);
        h = ds_rotl64(v1, 1) + ds_rotl64(v2, 7) + ds_rotl64(v3, 12) + ds_rotl64(v4, 18);
        h = ds_xxh_merge(h, v1);
        h = ds_xxh_merge(h, v2);
        h = ds_xxh_merge(h, v3);
        h = ds_xxh_merge(h, v4);
    } else {
        h = seed + DS_XXH_P5;
    }
    h += (uint64_t)len;
    while (end - p >= 8) {
        h ^= ds_xxh_round(0, ds_read64le(p));
        h = ds_rotl64(h, 27) * DS_XXH_P1 + DS_XXH_P4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= (uint64_t)ds_read32le(p) * DS_XXH_P1;
        h = ds_rotl64(h, 23) * DS_XXH_P2 + DS_XXH_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)*p * DS_XXH_P5;
        h = ds_rotl64(h, 11) * DS_XXH_P1;
        p++;
    }
    h ^= h >> 33;
    h *= DS_XXH_P2;
    h ^= h >> 29;
    h *= DS_XXH_P3;
    h ^= h >> 32;
    return h;
}

int ds_reserve(void** p, uint32_t* cap, uint32_t need, size_t esz)
{
    uint32_t nc;
    void* np;
    if (need <= *cap)
        return 0;
    if ((size_t)need > SIZE_MAX / esz)
        return -1;
    nc = *cap ? *cap * 2 : DS_MIN_CAP;
    while (nc < need) {
        if (nc > 0x40000000u)
            return -1;
        nc *= 2;
    }
    np = DYN_DS_REALLOC(*p, (size_t)nc * esz);
    if (!np)
        return -1;
    *p = np;
    *cap = nc;
    return 0;
}

atomic_uint_least64_t ds_seed0 = 0x9E3779B97F4A7C15ULL;

atomic_uint_least64_t ds_seed1 = 0xC2B2AE3D27D4EB4FULL;

void dyn_ds_hash_seed(uint64_t seed0, uint64_t seed1)
{
    static atomic_flag seeded = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set_explicit(&seeded, memory_order_relaxed))
        return;
    atomic_store_explicit(&ds_seed0, seed0, memory_order_relaxed);
    atomic_store_explicit(&ds_seed1, seed1, memory_order_relaxed);
}

static void ds_cv_restart(ds_cvec* v, uint32_t from)
{
    uint32_t k, s = from ? v->c[from - 1].start + v->c[from - 1].n : 0;
    for (k = from; k < v->nch; k++) {
        v->c[k].start = s;
        s += v->c[k].n;
    }
}

static void ds_cv_drop(ds_cvec* v, uint32_t k)
{
    DYN_DS_FREE(v->c[k].p);
    memmove(v->c + k, v->c + k + 1, (size_t)(v->nch - k - 1) * sizeof *v->c);
    v->nch--;
}

static int ds_cv_open(ds_cvec* v, size_t esz, uint32_t k)
{
    char* p;
    if (ds_reserve((void**)&v->c, &v->cch, v->nch + 1, sizeof *v->c) < 0)
        return -1;
    p = (char*)DYN_DS_MALLOC(DS_CV_CAP * esz);
    if (!p)
        return -1;
    memmove(v->c + k + 1, v->c + k, (size_t)(v->nch - k) * sizeof *v->c);
    v->c[k].p = p;
    v->c[k].n = 0;
    v->c[k].start = 0;
    v->nch++;
    return 0;
}

static void ds_cv_merge(ds_cvec* v, size_t esz, uint32_t k)
{
    if (k + 1 < v->nch && v->c[k].n + v->c[k + 1].n <= DS_CV_CAP / 2) {
        memcpy(v->c[k].p + (size_t)v->c[k].n * esz, v->c[k + 1].p, (size_t)v->c[k + 1].n * esz);
        v->c[k].n += v->c[k + 1].n;
        ds_cv_drop(v, k + 1);
    }
}

static int ds_cv_insert1(ds_cvec* v, size_t esz, uint32_t at, const void* e)
{
    uint32_t k, o;
    ds_cv_chunk* c;
    if (v->nch == 0 && ds_cv_open(v, esz, 0) < 0)
        return -1;
    if (at >= v->len) {
        k = v->nch - 1;
        o = v->c[k].n;
    } else {
        k = ds_cv_chunk_of(v, at);
        o = at - v->c[k].start;
    }
    if (v->c[k].n == DS_CV_CAP && o == DS_CV_CAP && k + 1 == v->nch) {
        if (ds_cv_open(v, esz, k + 1) < 0)
            return -1;
        k++;
        o = 0;
    } else if (v->c[k].n == DS_CV_CAP) {
        uint32_t half = DS_CV_CAP / 2;
        if (ds_cv_open(v, esz, k + 1) < 0)
            return -1;
        memcpy(v->c[k + 1].p, v->c[k].p + (size_t)half * esz, (size_t)(DS_CV_CAP - half) * esz);
        v->c[k + 1].n = DS_CV_CAP - half;
        v->c[k].n = half;
        if (o > half) {
            o -= half;
            k++;
        }
    }
    c = &v->c[k];
    memmove(c->p + (size_t)(o + 1) * esz, c->p + (size_t)o * esz, (size_t)(c->n - o) * esz);
    memcpy(c->p + (size_t)o * esz, e, esz);
    c->n++;
    v->len++;
    ds_cv_restart(v, k);
    return 0;
}

int ds_cv_splice(ds_cvec* v, size_t esz, uint32_t at, uint32_t ndel,
    const void* ins, uint32_t nins)
{
    uint32_t i;
    if (ndel) {
        uint32_t k = ds_cv_chunk_of(v, at), o = at - v->c[k].start, left = ndel, first = k;
        while (left && k < v->nch) {
            ds_cv_chunk* c = &v->c[k];
            uint32_t take = c->n - o < left ? c->n - o : left;
            memmove(c->p + (size_t)o * esz, c->p + (size_t)(o + take) * esz, (size_t)(c->n - o - take) * esz);
            c->n -= take;
            left -= take;
            if (c->n == 0)
                ds_cv_drop(v, k);
            else
                k++;
            o = 0;
        }
        v->len -= ndel;
        if (first > 0)
            first--;
        if (first < v->nch)
            ds_cv_merge(v, esz, first);
        if (first + 1 < v->nch)
            ds_cv_merge(v, esz, first + 1);
        if (v->nch)
            ds_cv_restart(v, first < v->nch ? first : v->nch - 1);
    }
    for (i = 0; i < nins; i++)
        if (ds_cv_insert1(v, esz, at + i, (const char*)ins + (size_t)i * esz) < 0)
            return -1;
    return 0;
}

static inline int ds_cv_last_ok(const ds_cvec* v, size_t esz, uint32_t k, double x, int strict)
{
    double h;
    memcpy(&h, v->c[k].p + (size_t)(v->c[k].n - 1) * esz + sizeof(double), sizeof h);
    return strict ? h > x : h >= x;
}

uint32_t ds_cv_first_hi(const ds_cvec* v, size_t esz, double x, int strict)
{
    uint32_t a = 0, b = v->nch, lo, hi, hk = v->hint;
    const ds_cv_chunk* c;
    if (v->nch == 0 || !ds_cv_last_ok(v, esz, v->nch - 1, x, strict))
        return v->len;
    if (hk < v->nch && ds_cv_last_ok(v, esz, hk, x, strict) && (hk == 0 || !ds_cv_last_ok(v, esz, hk - 1, x, strict))) {
        a = hk;
        b = hk;
    }
    while (a < b) {
        uint32_t mid = a + (b - a) / 2;
        double h;
        memcpy(&h, v->c[mid].p + (size_t)(v->c[mid].n - 1) * esz + sizeof(double), sizeof h);
        if (strict ? h > x : h >= x)
            b = mid;
        else
            a = mid + 1;
    }
    if (a == v->nch)
        return v->len;
    ((ds_cvec*)(uintptr_t)v)->hint = a;
    c = &v->c[a];
    lo = 0;
    hi = c->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        double h;
        memcpy(&h, c->p + (size_t)mid * esz + sizeof(double), sizeof h);
        if (strict ? h > x : h >= x)
            hi = mid;
        else
            lo = mid + 1;
    }
    return c->start + lo;
}

void ds_cv_clear(ds_cvec* v)
{
    uint32_t k;
    for (k = 0; k < v->nch; k++)
        DYN_DS_FREE(v->c[k].p);
    v->nch = 0;
    v->len = 0;
}

void ds_cv_free(ds_cvec* v)
{
    ds_cv_clear(v);
    DYN_DS_FREE(v->c);
    v->c = NULL;
    v->cch = 0;
}

int dyn_ds_cmp_f64(const void* a, const void* b, void* ud)
{
    double x, y;
    (void)ud;
    memcpy(&x, a, 8);
    memcpy(&y, b, 8);
    return x < y ? -1 : x > y;
}

uint64_t dyn_ds_strhash(const char* p, size_t n)
{
    return dyn_ds_xxh64((const uint8_t*)p, n, ds_str_seed());
}

static uint64_t dyn_ds_mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

void dyn_ds_smap_free(dyn_ds_smap* m)
{
    DYN_DS_FREE(m->e);
    m->e = NULL;
    m->cap = m->n = 0;
}

static int dyn_ds_smap_grow(dyn_ds_smap* m)
{
    uint32_t ncap = m->cap ? m->cap * 2 : 16, i;
    dyn_ds_sent* ne;
    if (ncap > (1u << 30))
        return -1;
    ne = (dyn_ds_sent*)DYN_DS_CALLOC(ncap, sizeof(*ne));
    if (!ne)
        return -1;
    for (i = 0; i < m->cap; i++) {
        dyn_ds_sent* s = &m->e[i];
        uint32_t j;
        if (!s->used)
            continue;
        j = (uint32_t)s->h & (ncap - 1);
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = *s;
    }
    DYN_DS_FREE(m->e);
    m->e = ne;
    m->cap = ncap;
    return 0;
}

dyn_ds_sent* dyn_ds_smap_find(const dyn_ds_smap* m, const char* k, size_t n, uint64_t h)
{
    uint32_t j;
    if (!m->cap)
        return NULL;
    j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].used) {
        dyn_ds_sent* s = &m->e[j];
        if (s->h == h && s->n == n && (n == 0 || memcmp(s->k, k, n) == 0))
            return s;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

dyn_ds_sent* dyn_ds_smap_insert(dyn_ds_smap* m, const char* k, size_t n, uint64_t h, uintptr_t v)
{
    uint32_t j;
    if (n > UINT32_MAX)
        return NULL;
    if ((m->n + 1) * 4 > m->cap * 3 && dyn_ds_smap_grow(m) < 0)
        return NULL;
    j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].used)
        j = (j + 1) & (m->cap - 1);
    m->e[j].h = h;
    m->e[j].k = k;
    m->e[j].n = (uint32_t)n;
    m->e[j].used = 1;
    m->e[j].v = v;
    m->n++;
    return &m->e[j];
}

void dyn_ds_smap_remove(dyn_ds_smap* m, dyn_ds_sent* s)
{
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(s - m->e), j = i;
    m->e[i].used = 0;
    m->n--;
    for (;;) {
        uint32_t home;
        j = (j + 1) & mask;
        if (!m->e[j].used)
            break;
        home = (uint32_t)m->e[j].h & mask;
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            m->e[i] = m->e[j];
            m->e[j].used = 0;
            i = j;
        }
    }
}

static uint32_t dyn_ds_imap_hash(const dyn_ds_imap* m, uint32_t k)
{
    return (uint32_t)dyn_ds_mix64((uint64_t)k ^ m->seed);
}

void dyn_ds_imap_init(dyn_ds_imap* m)
{
    m->e = NULL;
    m->cap = m->n = 0;
    m->seed = ds_str_seed();
}

void dyn_ds_imap_free(dyn_ds_imap* m)
{
    DYN_DS_FREE(m->e);
    m->e = NULL;
    m->cap = m->n = 0;
}

static int dyn_ds_imap_grow(dyn_ds_imap* m)
{
    uint32_t ncap = m->cap ? m->cap * 2 : 16, i;
    dyn_ds_ient* ne;
    if (ncap > (1u << 30))
        return -1;
    ne = (dyn_ds_ient*)DYN_DS_CALLOC(ncap, sizeof(*ne));
    if (!ne)
        return -1;
    for (i = 0; i < m->cap; i++) {
        uint32_t j;
        if (!m->e[i].used)
            continue;
        j = dyn_ds_imap_hash(m, m->e[i].k) & (ncap - 1);
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = m->e[i];
    }
    DYN_DS_FREE(m->e);
    m->e = ne;
    m->cap = ncap;
    return 0;
}

dyn_ds_ient* dyn_ds_imap_find(const dyn_ds_imap* m, uint32_t k)
{
    uint32_t j;
    if (!m->cap)
        return NULL;
    j = dyn_ds_imap_hash(m, k) & (m->cap - 1);
    while (m->e[j].used) {
        if (m->e[j].k == k)
            return &m->e[j];
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

dyn_ds_ient* dyn_ds_imap_put(dyn_ds_imap* m, uint32_t k, uint32_t v)
{
    dyn_ds_ient* f = dyn_ds_imap_find(m, k);
    uint32_t j;
    if (f) {
        f->v = v;
        return f;
    }
    if ((m->n + 1) * 4 > m->cap * 3 && dyn_ds_imap_grow(m) < 0)
        return NULL;
    j = dyn_ds_imap_hash(m, k) & (m->cap - 1);
    while (m->e[j].used)
        j = (j + 1) & (m->cap - 1);
    m->e[j].k = k;
    m->e[j].v = v;
    m->e[j].used = 1;
    m->n++;
    return &m->e[j];
}

void dyn_ds_imap_remove(dyn_ds_imap* m, dyn_ds_ient* s)
{
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(s - m->e), j = i;
    m->e[i].used = 0;
    m->n--;
    for (;;) {
        uint32_t home;
        j = (j + 1) & mask;
        if (!m->e[j].used)
            break;
        home = dyn_ds_imap_hash(m, m->e[j].k) & mask;
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            m->e[i] = m->e[j];
            m->e[j].used = 0;
            i = j;
        }
    }
}

uint8_t* dyn_ds_bytes_dup(const char* p, size_t n)
{
    uint8_t* d = (uint8_t*)DYN_DS_MALLOC(n + 1);
    if (d) {
        if (n)
            memcpy(d, p, n);
        d[n] = 0;
    }
    return d;
}
