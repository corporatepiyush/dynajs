#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "dyn-ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#ifndef DYN_DS_MALLOC
typedef void* (*ds_malloc_t)(size_t);
typedef void* (*ds_realloc_t)(void*, size_t);
typedef void (*ds_free_t)(void*);
static _Atomic(ds_malloc_t) ds_malloc_fn = malloc;
static _Atomic(ds_realloc_t) ds_realloc_fn = realloc;
static _Atomic(ds_free_t) ds_free_fn = free;

static void* ds_hook_malloc(size_t n)
{
    return atomic_load_explicit(&ds_malloc_fn, memory_order_relaxed)(n);
}

static void* ds_hook_realloc(void* p, size_t n)
{
    return atomic_load_explicit(&ds_realloc_fn, memory_order_relaxed)(p, n);
}

static void ds_hook_free(void* p)
{
    atomic_load_explicit(&ds_free_fn, memory_order_relaxed)(p);
}

static void* ds_hook_calloc(size_t n, size_t s)
{
    void* p;
    if (s && n > SIZE_MAX / s)
        return NULL;
    p = ds_hook_malloc(n * s);
    if (p)
        memset(p, 0, n * s);
    return p;
}

void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*))
{
    atomic_store(&ds_malloc_fn, malloc_fn ? malloc_fn : malloc);
    atomic_store(&ds_realloc_fn, realloc_fn ? realloc_fn : realloc);
    atomic_store(&ds_free_fn, free_fn ? free_fn : free);
}

#define DYN_DS_MALLOC(n) ds_hook_malloc(n)
#define DYN_DS_CALLOC(n, s) ds_hook_calloc(n, s)
#define DYN_DS_REALLOC(p, n) ds_hook_realloc(p, n)
#define DYN_DS_FREE(p) ds_hook_free(p)
#else
void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*))
{
    (void)malloc_fn;
    (void)realloc_fn;
    (void)free_fn;
}
#endif

void* dyn_ds_malloc(size_t n)
{
    return DYN_DS_MALLOC(n);
}

void* dyn_ds_calloc(size_t n, size_t size)
{
    return DYN_DS_CALLOC(n, size);
}

void* dyn_ds_realloc(void* p, size_t n)
{
    return DYN_DS_REALLOC(p, n);
}

void dyn_ds_free(void* p)
{
    DYN_DS_FREE(p);
}

#define DS_UNCONST(p) ((void*)(uintptr_t)(p))
#if defined(__has_attribute)
#if __has_attribute(uninitialized)
#define DS_SCRATCH __attribute__((uninitialized))
#endif
#endif
#ifndef DS_SCRATCH
#define DS_SCRATCH
#endif
#define DS_PI 3.14159265358979323846

#define DS_XXH_P1 0x9E3779B185EBCA87ULL
#define DS_XXH_P2 0xC2B2AE3D27D4EB4FULL
#define DS_XXH_P3 0x165667B19E3779F9ULL
#define DS_XXH_P4 0x85EBCA77C2B2AE63ULL
#define DS_XXH_P5 0x27D4EB2F165667C5ULL

static uint64_t ds_rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static uint64_t ds_read64le(const uint8_t* p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t ds_read32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ds_xxh_round(uint64_t acc, uint64_t input)
{
    acc += input * DS_XXH_P2;
    acc = ds_rotl64(acc, 31);
    return acc * DS_XXH_P1;
}

static uint64_t ds_xxh_merge(uint64_t acc, uint64_t val)
{
    acc ^= ds_xxh_round(0, val);
    return acc * DS_XXH_P1 + DS_XXH_P4;
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

static uint64_t ds_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

#define DS_MIN_CAP 8u

static void* ds_calloc(size_t n, size_t sz)
{
    if (n == 0)
        n = 1;
    return DYN_DS_CALLOC(n, sz);
}

static uint64_t ds_u64_sat_add(uint64_t a, uint64_t b)
{
    uint64_t s = a + b;
    return s < a ? UINT64_MAX : s;
}

static int ds_reserve(void** p, uint32_t* cap, uint32_t need, size_t esz)
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

typedef struct {
    uint64_t h;
    const char* key;
    size_t klen;
    uint32_t idx;
} dyn_ixslot;

typedef struct {
    dyn_ixslot* slot;
    uint32_t cap;
    uint32_t used;
} dyn_ix;

static atomic_uint_least64_t ds_seed0 = 0x9E3779B97F4A7C15ULL;
static atomic_uint_least64_t ds_seed1 = 0xC2B2AE3D27D4EB4FULL;

void dyn_ds_hash_seed(uint64_t seed0, uint64_t seed1)
{
    static atomic_flag seeded = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set_explicit(&seeded, memory_order_relaxed))
        return;
    atomic_store_explicit(&ds_seed0, seed0, memory_order_relaxed);
    atomic_store_explicit(&ds_seed1, seed1, memory_order_relaxed);
}

static inline uint64_t ds_ix_mix(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    uint64_t lo = (a & 0xFFFFFFFFu) * (b & 0xFFFFFFFFu);
    uint64_t hi = (a >> 32) * (b >> 32);
    uint64_t m1 = (a >> 32) * (b & 0xFFFFFFFFu);
    uint64_t m2 = (a & 0xFFFFFFFFu) * (b >> 32);
    return (lo + (m1 << 32) + (m2 << 32)) ^ (hi + (m1 >> 32) + (m2 >> 32));
#endif
}

static uint64_t ds_ix_hash(const char* k, size_t n)
{
    const uint8_t* p = (const uint8_t*)k;
    uint64_t a, b, h;
    uint64_t s0 = atomic_load_explicit(&ds_seed0, memory_order_relaxed);
    uint64_t s1 = atomic_load_explicit(&ds_seed1, memory_order_relaxed);

    if (n >= 16) {
        h = dyn_ds_xxh64(p, n, s1);
        return h ? h : 1;
    }
    if (n >= 8) {
        memcpy(&a, p, 8);
        memcpy(&b, p + n - 8, 8);
    } else if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, p + n - 4, 4);
        a = x;
        b = y;
    } else if (n) {
        a = ((uint64_t)p[0] << 16) | ((uint64_t)p[n >> 1] << 8) | p[n - 1];
        b = 0;
    } else {
        a = b = 0;
    }
    h = ds_ix_mix(a ^ s0, b ^ s1 ^ (uint64_t)n);
    return h ? h : 1;
}

static void ds_ix_free(dyn_ix* ix)
{
    DYN_DS_FREE(ix->slot);
    ix->slot = NULL;
    ix->cap = ix->used = 0;
}

static void ds_ix_put_raw(dyn_ixslot* slot, uint32_t mask, uint64_t h,
    const char* k, size_t n, uint32_t idx)
{
    uint32_t i = (uint32_t)h & mask;
    while (slot[i].h)
        i = (i + 1) & mask;
    slot[i].h = h;
    slot[i].key = k;
    slot[i].klen = n;
    slot[i].idx = idx;
}

static int ds_ix_grow(dyn_ix* ix)
{
    uint32_t nc = ix->cap ? ix->cap * 2 : DS_MIN_CAP;
    dyn_ixslot* ns;
    uint32_t i;
    if (ix->cap > 0x40000000u)
        return -1;
    ns = (dyn_ixslot*)ds_calloc(nc, sizeof(*ns));
    if (!ns)
        return -1;
    for (i = 0; i < ix->cap; i++)
        if (ix->slot[i].h)
            ds_ix_put_raw(ns, nc - 1, ix->slot[i].h, ix->slot[i].key,
                ix->slot[i].klen, ix->slot[i].idx);
    DYN_DS_FREE(ix->slot);
    ix->slot = ns;
    ix->cap = nc;
    return 0;
}

static uint32_t ds_ix_find(const dyn_ix* ix, uint64_t h, const char* k, size_t n)
{
    uint32_t mask, i;
    if (!ix->cap)
        return UINT32_MAX;
    mask = ix->cap - 1;
    i = (uint32_t)h & mask;
    while (ix->slot[i].h) {
        if (ix->slot[i].h == h && ix->slot[i].klen == n && memcmp(ix->slot[i].key, k, n) == 0)
            return i;
        i = (i + 1) & mask;
    }
    return UINT32_MAX;
}

static int ds_ix_reserve(dyn_ix* ix)
{
    if ((ix->used + 1) * 10 >= ix->cap * 7)
        return ds_ix_grow(ix);
    return 0;
}

static int ds_ix_insert(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx)
{
    if (ds_ix_reserve(ix) < 0)
        return -1;
    ds_ix_put_raw(ix->slot, ix->cap - 1, h, k, n, idx);
    ix->used++;
    return 0;
}

static void ds_ix_erase(dyn_ix* ix, uint32_t pos)
{
    uint32_t mask = ix->cap - 1, i = pos, j;
    ix->slot[i].h = 0;
    ix->used--;
    j = i;
    for (;;) {
        uint32_t k;
        j = (j + 1) & mask;
        if (!ix->slot[j].h)
            break;
        k = (uint32_t)ix->slot[j].h & mask;
        if ((j > i) ? (k <= i || k > j) : (k <= i && k > j)) {
            ix->slot[i] = ix->slot[j];
            ix->slot[j].h = 0;
            i = j;
        }
    }
}

static void ds_ix_move(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx)
{
    uint32_t pos = ds_ix_find(ix, h, k, n);
    if (pos != UINT32_MAX)
        ix->slot[pos].idx = idx;
}

typedef struct {
    char* p;
    size_t n;
    uint64_t h;
} ds_key;

static int ds_key_init(ds_key* dst, const char* k, size_t n, uint64_t h)
{
    dst->p = (char*)DYN_DS_MALLOC(n ? n : 1);
    if (!dst->p)
        return -1;
    memcpy(dst->p, k, n);
    dst->n = n;
    dst->h = h;
    return 0;
}

typedef struct {
    ds_key key;
    uint64_t count;
} ds_mset_rec;

struct dyn_mset {
    ds_mset_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mset_t* dyn_mset_new(void)
{
    dyn_mset_t* m = (dyn_mset_t*)ds_calloc(1, sizeof(*m));
    return m;
}

void dyn_mset_clear(dyn_mset_t* m)
{
    uint32_t i;
    for (i = 0; i < m->len; i++)
        DYN_DS_FREE(m->rec[i].key.p);
    m->len = 0;
    m->total = 0;
    ds_ix_free(&m->ix);
}

void dyn_mset_free(dyn_mset_t* m)
{
    if (!m)
        return;
    dyn_mset_clear(m);
    DYN_DS_FREE(m->rec);
    DYN_DS_FREE(m);
}

static void ds_mset_erase(dyn_mset_t* m, uint32_t i, uint32_t pos)
{
    ds_ix_erase(&m->ix, pos);
    DYN_DS_FREE(m->rec[i].key.p);
    m->len--;
    if (i != m->len) {
        m->rec[i] = m->rec[m->len];
        ds_ix_move(&m->ix, m->rec[i].key.h, m->rec[i].key.p, m->rec[i].key.n, i);
    }
}

int dyn_mset_add(dyn_mset_t* m, const char* k, size_t n, int64_t delta,
    uint64_t* out)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    uint64_t cur = 0;

    if (pos != UINT32_MAX)
        cur = m->rec[m->ix.slot[pos].idx].count;

    if (delta < 0) {
        uint64_t sub = (uint64_t)(-(delta + 1)) + 1;
        uint64_t nv = sub >= cur ? 0 : cur - sub;
        m->total -= cur - nv;
        if (pos != UINT32_MAX) {
            uint32_t idx = m->ix.slot[pos].idx;
            if (nv == 0)
                ds_mset_erase(m, idx, pos);
            else
                m->rec[idx].count = nv;
        }
        if (out)
            *out = nv;
        return 0;
    }
    if (delta == 0) {
        if (out)
            *out = cur;
        return 0;
    }
    if (pos != UINT32_MAX) {
        uint32_t idx = m->ix.slot[pos].idx;
        uint64_t nc = ds_u64_sat_add(cur, (uint64_t)delta);
        m->total = ds_u64_sat_add(m->total, nc - cur);
        m->rec[idx].count = nc;
        if (out)
            *out = nc;
        return 0;
    }
    if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (ds_key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = (uint64_t)delta;
    if (ds_ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        DYN_DS_FREE(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total = ds_u64_sat_add(m->total, (uint64_t)delta);
    if (out)
        *out = (uint64_t)delta;
    return 0;
}

int dyn_mset_set_count(dyn_mset_t* m, const char* k, size_t n, uint64_t c)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    if (pos != UINT32_MAX) {
        uint32_t idx = m->ix.slot[pos].idx;
        m->total = ds_u64_sat_add(m->total - m->rec[idx].count, c);
        if (c == 0)
            ds_mset_erase(m, idx, pos);
        else
            m->rec[idx].count = c;
        return 0;
    }
    if (c == 0)
        return 0;
    if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (ds_key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = c;
    if (ds_ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        DYN_DS_FREE(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total = ds_u64_sat_add(m->total, c);
    return 0;
}

uint64_t dyn_mset_count(const dyn_mset_t* m, const char* k, size_t n)
{
    uint32_t pos = ds_ix_find(&m->ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? 0 : m->rec[m->ix.slot[pos].idx].count;
}

uint64_t dyn_mset_total(const dyn_mset_t* m) { return m->total; }
uint32_t dyn_mset_distinct(const dyn_mset_t* m) { return m->len; }

int dyn_mset_at(const dyn_mset_t* m, uint32_t i, const char** k, size_t* n,
    uint64_t* count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].count;
    return 1;
}

typedef struct {
    ds_key key;
    dyn_cell_t* val;
    uint32_t len, cap;
} ds_mmap_rec;

struct dyn_mmap {
    ds_mmap_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mmap_t* dyn_mmap_new(void)
{
    return (dyn_mmap_t*)ds_calloc(1, sizeof(dyn_mmap_t));
}

void dyn_mmap_free(dyn_mmap_t* m, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i, j;
    if (!m)
        return;
    for (i = 0; i < m->len; i++) {
        if (fn)
            for (j = 0; j < m->rec[i].len; j++)
                fn(ud, &m->rec[i].val[j]);
        DYN_DS_FREE(m->rec[i].val);
        DYN_DS_FREE(m->rec[i].key.p);
    }
    DYN_DS_FREE(m->rec);
    ds_ix_free(&m->ix);
    DYN_DS_FREE(m);
}

static ds_mmap_rec* ds_mmap_lookup(const dyn_mmap_t* m, const char* k, size_t n)
{
    uint32_t pos = ds_ix_find(&m->ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? NULL : &m->rec[m->ix.slot[pos].idx];
}

int dyn_mmap_put(dyn_mmap_t* m, const char* k, size_t n, const dyn_cell_t* v)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    ds_mmap_rec* r;

    if (pos == UINT32_MAX) {
        if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1,
                sizeof(*m->rec))
            < 0)
            return -1;
        r = &m->rec[m->len];
        memset(r, 0, sizeof(*r));
        if (ds_key_init(&r->key, k, n, h) < 0)
            return -1;
        if (ds_reserve((void**)&r->val, &r->cap, 1, sizeof(*r->val)) < 0) {
            DYN_DS_FREE(r->key.p);
            return -1;
        }
        if (ds_ix_insert(&m->ix, h, r->key.p, n, m->len) < 0) {
            DYN_DS_FREE(r->val);
            DYN_DS_FREE(r->key.p);
            return -1;
        }
        m->len++;
    } else {
        r = &m->rec[m->ix.slot[pos].idx];
        if (ds_reserve((void**)&r->val, &r->cap, r->len + 1,
                sizeof(*r->val))
            < 0)
            return -1;
    }
    r->val[r->len++] = *v;
    m->total++;
    return 0;
}

uint32_t dyn_mmap_count(const dyn_mmap_t* m, const char* k, size_t n)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    return r ? r->len : 0;
}

const dyn_cell_t* dyn_mmap_at(const dyn_mmap_t* m, const char* k, size_t n,
    uint32_t i)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    return (r && i < r->len) ? &r->val[i] : NULL;
}

static void ds_mmap_erase(dyn_mmap_t* m, uint32_t idx, uint32_t pos)
{
    ds_ix_erase(&m->ix, pos);
    DYN_DS_FREE(m->rec[idx].val);
    DYN_DS_FREE(m->rec[idx].key.p);
    m->len--;
    if (idx != m->len) {
        m->rec[idx] = m->rec[m->len];
        ds_ix_move(&m->ix, m->rec[idx].key.h, m->rec[idx].key.p,
            m->rec[idx].key.n, idx);
    }
}

int dyn_mmap_remove_at(dyn_mmap_t* m, const char* k, size_t n, uint32_t i,
    dyn_cell_t* out)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n), idx;
    ds_mmap_rec* r;

    if (pos == UINT32_MAX)
        return 0;
    idx = m->ix.slot[pos].idx;
    r = &m->rec[idx];
    if (i >= r->len)
        return 0;
    *out = r->val[i];
    memmove(&r->val[i], &r->val[i + 1],
        (size_t)(r->len - i - 1) * sizeof(*r->val));
    r->len--;
    m->total--;
    if (r->len == 0)
        ds_mmap_erase(m, idx, pos);
    return 1;
}

uint32_t dyn_mmap_remove_key(dyn_mmap_t* m, const char* k, size_t n,
    dyn_cell_free_fn fn, void* ud)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n), idx, i, cnt;

    if (pos == UINT32_MAX)
        return 0;
    idx = m->ix.slot[pos].idx;
    cnt = m->rec[idx].len;
    if (fn)
        for (i = 0; i < cnt; i++)
            fn(ud, &m->rec[idx].val[i]);
    m->total -= cnt;
    ds_mmap_erase(m, idx, pos);
    return cnt;
}

uint32_t dyn_mmap_keys(const dyn_mmap_t* m) { return m->len; }
uint64_t dyn_mmap_size(const dyn_mmap_t* m) { return m->total; }

int dyn_mmap_key_at(const dyn_mmap_t* m, uint32_t i, const char** k, size_t* n,
    uint32_t* count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].len;
    return 1;
}

const dyn_cell_t* dyn_mmap_cells_at(const dyn_mmap_t* m, uint32_t i,
    uint32_t* count)
{
    if (i >= m->len) {
        *count = 0;
        return NULL;
    }
    *count = m->rec[i].len;
    return m->rec[i].val;
}

const dyn_cell_t* dyn_mmap_cells_for(const dyn_mmap_t* m, const char* k,
    size_t n, uint32_t* count)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    *count = r ? r->len : 0;
    return r ? r->val : NULL;
}

typedef struct {
    ds_key key;
    ds_key val;
} ds_bimap_rec;

struct dyn_bimap {
    ds_bimap_rec* rec;
    uint32_t len, cap;
    dyn_ix fwd;
    dyn_ix rev;
};

dyn_bimap_t* dyn_bimap_new(void)
{
    return (dyn_bimap_t*)ds_calloc(1, sizeof(dyn_bimap_t));
}

void dyn_bimap_clear(dyn_bimap_t* b)
{
    uint32_t i;
    for (i = 0; i < b->len; i++) {
        DYN_DS_FREE(b->rec[i].key.p);
        DYN_DS_FREE(b->rec[i].val.p);
    }
    b->len = 0;
    ds_ix_free(&b->fwd);
    ds_ix_free(&b->rev);
}

void dyn_bimap_free(dyn_bimap_t* b)
{
    if (!b)
        return;
    dyn_bimap_clear(b);
    DYN_DS_FREE(b->rec);
    DYN_DS_FREE(b);
}

static void ds_bimap_erase(dyn_bimap_t* b, uint32_t idx)
{
    uint32_t fp = ds_ix_find(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
        b->rec[idx].key.n);
    uint32_t rp = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
        b->rec[idx].val.n);
    if (fp != UINT32_MAX)
        ds_ix_erase(&b->fwd, fp);
    if (rp != UINT32_MAX)
        ds_ix_erase(&b->rev, rp);
    DYN_DS_FREE(b->rec[idx].key.p);
    DYN_DS_FREE(b->rec[idx].val.p);
    b->len--;
    if (idx != b->len) {
        b->rec[idx] = b->rec[b->len];
        ds_ix_move(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
            b->rec[idx].key.n, idx);
        ds_ix_move(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n, idx);
    }
}

int dyn_bimap_put(dyn_bimap_t* b, const char* k, size_t kn,
    const char* v, size_t vn, int force)
{
    uint64_t kh = ds_ix_hash(k, kn), vh = ds_ix_hash(v, vn);
    uint32_t kp = ds_ix_find(&b->fwd, kh, k, kn);
    uint32_t vp = ds_ix_find(&b->rev, vh, v, vn);
    ds_bimap_rec* r;

    if (kp != UINT32_MAX && vp != UINT32_MAX && b->fwd.slot[kp].idx == b->rev.slot[vp].idx)
        return 0;
    if (vp != UINT32_MAX) {
        if (!force)
            return DYN_BIMAP_VALUE_TAKEN;
        if (kp != UINT32_MAX) {
            ds_key nv;
            if (ds_ix_reserve(&b->rev) < 0)
                return -1;
            if (ds_key_init(&nv, v, vn, vh) < 0)
                return -1;
            vp = ds_ix_find(&b->rev, vh, v, vn);
            ds_bimap_erase(b, b->rev.slot[vp].idx);
            kp = ds_ix_find(&b->fwd, kh, k, kn);
            {
                uint32_t idx = b->fwd.slot[kp].idx;
                uint32_t op = ds_ix_find(&b->rev, b->rec[idx].val.h,
                    b->rec[idx].val.p, b->rec[idx].val.n);
                if (op != UINT32_MAX)
                    ds_ix_erase(&b->rev, op);
                DYN_DS_FREE(b->rec[idx].val.p);
                b->rec[idx].val = nv;
                ds_ix_insert(&b->rev, vh, nv.p, vn, idx);
            }
            return 0;
        }
        r = NULL;
        if (ds_reserve((void**)&b->rec, &b->cap, b->len + 1,
                sizeof(*b->rec))
            < 0)
            return -1;
        {
            ds_bimap_rec tmp;
            tmp.key.p = NULL;
            tmp.val.p = NULL;
            if (ds_key_init(&tmp.key, k, kn, kh) < 0)
                return -1;
            if (ds_key_init(&tmp.val, v, vn, vh) < 0) {
                DYN_DS_FREE(tmp.key.p);
                return -1;
            }
            if (ds_ix_reserve(&b->fwd) < 0 || ds_ix_reserve(&b->rev) < 0) {
                DYN_DS_FREE(tmp.key.p);
                DYN_DS_FREE(tmp.val.p);
                return -1;
            }
            vp = ds_ix_find(&b->rev, vh, v, vn);
            ds_bimap_erase(b, b->rev.slot[vp].idx);
            b->rec[b->len] = tmp;
            ds_ix_insert(&b->fwd, kh, tmp.key.p, kn, b->len);
            ds_ix_insert(&b->rev, vh, tmp.val.p, vn, b->len);
            b->len++;
        }
        return 0;
    }
    if (kp != UINT32_MAX) {
        uint32_t idx = b->fwd.slot[kp].idx;
        uint32_t op = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n);
        ds_key nv;
        if (ds_ix_reserve(&b->rev) < 0)
            return -1;
        if (ds_key_init(&nv, v, vn, vh) < 0)
            return -1;
        op = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n);
        if (op != UINT32_MAX)
            ds_ix_erase(&b->rev, op);
        DYN_DS_FREE(b->rec[idx].val.p);
        b->rec[idx].val = nv;
        ds_ix_insert(&b->rev, vh, nv.p, vn, idx);
        return 0;
    }
    if (ds_reserve((void**)&b->rec, &b->cap, b->len + 1, sizeof(*b->rec)) < 0)
        return -1;
    r = &b->rec[b->len];
    if (ds_key_init(&r->key, k, kn, kh) < 0)
        return -1;
    if (ds_key_init(&r->val, v, vn, vh) < 0) {
        DYN_DS_FREE(r->key.p);
        return -1;
    }
    if (ds_ix_insert(&b->fwd, kh, r->key.p, kn, b->len) < 0) {
        DYN_DS_FREE(r->key.p);
        DYN_DS_FREE(r->val.p);
        return -1;
    }
    if (ds_ix_insert(&b->rev, vh, r->val.p, vn, b->len) < 0) {
        uint32_t fp = ds_ix_find(&b->fwd, kh, r->key.p, kn);
        if (fp != UINT32_MAX)
            ds_ix_erase(&b->fwd, fp);
        DYN_DS_FREE(r->key.p);
        DYN_DS_FREE(r->val.p);
        return -1;
    }
    b->len++;
    return 0;
}

const char* dyn_bimap_get(const dyn_bimap_t* b, const char* k, size_t kn,
    size_t* vn)
{
    uint32_t pos = ds_ix_find(&b->fwd, ds_ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return NULL;
    *vn = b->rec[b->fwd.slot[pos].idx].val.n;
    return b->rec[b->fwd.slot[pos].idx].val.p;
}

const char* dyn_bimap_key_for(const dyn_bimap_t* b, const char* v, size_t vn,
    size_t* kn)
{
    uint32_t pos = ds_ix_find(&b->rev, ds_ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return NULL;
    *kn = b->rec[b->rev.slot[pos].idx].key.n;
    return b->rec[b->rev.slot[pos].idx].key.p;
}

int dyn_bimap_remove(dyn_bimap_t* b, const char* k, size_t kn)
{
    uint32_t pos = ds_ix_find(&b->fwd, ds_ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return 0;
    ds_bimap_erase(b, b->fwd.slot[pos].idx);
    return 1;
}

int dyn_bimap_remove_value(dyn_bimap_t* b, const char* v, size_t vn)
{
    uint32_t pos = ds_ix_find(&b->rev, ds_ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return 0;
    ds_bimap_erase(b, b->rev.slot[pos].idx);
    return 1;
}

uint32_t dyn_bimap_size(const dyn_bimap_t* b) { return b->len; }

int dyn_bimap_at(const dyn_bimap_t* b, uint32_t i, const char** k, size_t* kn,
    const char** v, size_t* vn)
{
    if (i >= b->len)
        return 0;
    *k = b->rec[i].key.p;
    *kn = b->rec[i].key.n;
    *v = b->rec[i].val.p;
    *vn = b->rec[i].val.n;
    return 1;
}

typedef struct {
    ds_key row;
    ds_key col;
    ds_key pair;
    dyn_cell_t val;
} ds_table_rec;

typedef struct {
    uint32_t head, tail;
} ds_table_chain;

struct dyn_table {
    ds_table_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    dyn_ix row_ix, col_ix;
    ds_table_chain *row_chain, *col_chain;
    uint32_t row_chains, row_chain_cap, col_chains, col_chain_cap;
    uint32_t *row_next, *col_next;
    uint32_t slice_cap;
    uint32_t sliced;
    int slices_valid;
};

static void ds_table_drop_slices(dyn_table_t* t)
{
    ds_ix_free(&t->row_ix);
    ds_ix_free(&t->col_ix);
    DYN_DS_FREE(t->row_chain);
    DYN_DS_FREE(t->col_chain);
    DYN_DS_FREE(t->row_next);
    DYN_DS_FREE(t->col_next);
    t->row_chain = NULL;
    t->col_chain = NULL;
    t->row_next = NULL;
    t->col_next = NULL;
    t->row_chains = t->col_chains = 0;
    t->row_chain_cap = t->col_chain_cap = 0;
    t->slice_cap = 0;
    t->sliced = 0;
    t->slices_valid = 0;
}

static int ds_table_chain_add(dyn_ix* ix, ds_table_chain** chain, uint32_t* nchain,
    uint32_t* cap, uint32_t* next, const ds_key* k,
    uint32_t i)
{
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n);
    next[i] = UINT32_MAX;
    if (pos != UINT32_MAX) {
        ds_table_chain* c = &(*chain)[ix->slot[pos].idx];
        next[c->tail] = i;
        c->tail = i;
        return 0;
    }
    if (ds_reserve((void**)chain, cap, *nchain + 1, sizeof(**chain)) < 0)
        return -1;
    (*chain)[*nchain].head = i;
    (*chain)[*nchain].tail = i;
    if (ds_ix_insert(ix, k->h, k->p, k->n, *nchain) < 0)
        return -1;
    (*nchain)++;
    return 0;
}

static void ds_table_chain_unlink(dyn_table_t* t, int by_column, uint32_t i)
{
    dyn_ix* ix = by_column ? &t->col_ix : &t->row_ix;
    ds_table_chain* chain = by_column ? t->col_chain : t->row_chain;
    uint32_t* next = by_column ? t->col_next : t->row_next;
    const ds_key* k = by_column ? &t->rec[i].col : &t->rec[i].row;
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n), p;
    ds_table_chain* c;

    if (pos == UINT32_MAX)
        return;
    c = &chain[ix->slot[pos].idx];
    if (c->head == i) {
        if (c->tail == i) {
            ds_ix_erase(ix, pos);
            return;
        }
        c->head = next[i];
    } else {
        for (p = c->head; p != UINT32_MAX && next[p] != i; p = next[p])
            ;
        if (p == UINT32_MAX)
            return;
        next[p] = next[i];
        if (c->tail == i)
            c->tail = p;
    }
    ix->slot[pos].key = by_column ? t->rec[c->head].col.p
                                  : t->rec[c->head].row.p;
}

static void ds_table_chain_move(dyn_table_t* t, int by_column, uint32_t from,
    uint32_t to)
{
    dyn_ix* ix = by_column ? &t->col_ix : &t->row_ix;
    ds_table_chain* chain = by_column ? t->col_chain : t->row_chain;
    uint32_t* next = by_column ? t->col_next : t->row_next;
    const ds_key* k = by_column ? &t->rec[to].col : &t->rec[to].row;
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n), p;
    ds_table_chain* c;

    if (pos == UINT32_MAX)
        return;
    c = &chain[ix->slot[pos].idx];
    next[to] = next[from];
    if (c->head == from) {
        c->head = to;
    } else {
        for (p = c->head; p != UINT32_MAX && next[p] != from; p = next[p])
            ;
        if (p != UINT32_MAX)
            next[p] = to;
    }
    if (c->tail == from)
        c->tail = to;
}

static int ds_table_build_slices(dyn_table_t* t)
{
    uint32_t i;

    if (t->len > t->slice_cap) {
        uint32_t cap = t->slice_cap ? t->slice_cap : 16;
        uint32_t *rn, *cn;
        while (cap < t->len)
            cap *= 2;
        rn = (uint32_t*)DYN_DS_REALLOC(t->row_next, (size_t)cap * sizeof(uint32_t));
        if (!rn)
            return -1;
        t->row_next = rn;
        cn = (uint32_t*)DYN_DS_REALLOC(t->col_next, (size_t)cap * sizeof(uint32_t));
        if (!cn)
            return -1;
        t->col_next = cn;
        t->slice_cap = cap;
    }
    for (i = t->sliced; i < t->len; i++) {
        if (ds_table_chain_add(&t->row_ix, &t->row_chain, &t->row_chains,
                &t->row_chain_cap, t->row_next,
                &t->rec[i].row, i)
                < 0
            || ds_table_chain_add(&t->col_ix, &t->col_chain, &t->col_chains,
                   &t->col_chain_cap, t->col_next,
                   &t->rec[i].col, i)
                < 0) {
            ds_table_drop_slices(t);
            return -1;
        }
    }
    t->sliced = t->len;
    t->slices_valid = 1;
    return 0;
}

uint32_t dyn_table_slice_first(dyn_table_t* t, const char* k, size_t n,
    int by_column)
{
    const dyn_ix* ix;
    const ds_table_chain* chain;
    uint32_t pos;
    if ((!t->slices_valid || t->sliced != t->len) && ds_table_build_slices(t) < 0)
        return UINT32_MAX;
    ix = by_column ? &t->col_ix : &t->row_ix;
    chain = by_column ? t->col_chain : t->row_chain;
    pos = ds_ix_find(ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? UINT32_MAX : chain[ix->slot[pos].idx].head;
}

uint32_t dyn_table_slice_next(const dyn_table_t* t, uint32_t i, int by_column)
{
    const uint32_t* next = by_column ? t->col_next : t->row_next;
    if (!t->slices_valid || !next || i >= t->len)
        return UINT32_MAX;
    return next[i];
}

dyn_table_t* dyn_table_new(void)
{
    return (dyn_table_t*)ds_calloc(1, sizeof(dyn_table_t));
}

void dyn_table_free(dyn_table_t* t, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < t->len; i++) {
        if (fn)
            fn(ud, &t->rec[i].val);
        DYN_DS_FREE(t->rec[i].row.p);
        DYN_DS_FREE(t->rec[i].col.p);
        DYN_DS_FREE(t->rec[i].pair.p);
    }
    DYN_DS_FREE(t->rec);
    ds_ix_free(&t->ix);
    ds_table_drop_slices(t);
    DYN_DS_FREE(t);
}

#define DS_TABLE_PAIR_INLINE 192

static int ds_table_pair_into(char* inl, char** out, size_t* outn, const char* r,
    size_t rn, const char* c, size_t cn)
{
    size_t n;
    char* p;
    int i;
    if (rn > SIZE_MAX - 8 - cn)
        return -1;
    n = 8 + rn + cn;
    p = (inl && n <= DS_TABLE_PAIR_INLINE) ? inl : (char*)DYN_DS_MALLOC(n);
    if (!p)
        return -1;
    for (i = 0; i < 8; i++)
        p[i] = (char)((uint64_t)rn >> (i * 8));
    memcpy(p + 8, r, rn);
    memcpy(p + 8 + rn, c, cn);
    *out = p;
    *outn = n;
    return 0;
}

static int ds_table_pair(char** out, size_t* outn, const char* r, size_t rn,
    const char* c, size_t cn)
{
    return ds_table_pair_into(NULL, out, outn, r, rn, c, cn);
}

static void ds_table_pair_release(char* inl, char* p)
{
    if (p != inl)
        DYN_DS_FREE(p);
}

int dyn_table_put(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, const dyn_cell_t* v,
    dyn_cell_t* old)
{
    char* pk;
    size_t pn;
    uint64_t h;
    uint32_t pos;
    ds_table_rec* rec;

    if (ds_table_pair(&pk, &pn, r, rn, c, cn) < 0)
        return -1;
    h = ds_ix_hash(pk, pn);
    pos = ds_ix_find(&t->ix, h, pk, pn);
    if (pos != UINT32_MAX) {
        uint32_t idx = t->ix.slot[pos].idx;
        DYN_DS_FREE(pk);
        *old = t->rec[idx].val;
        t->rec[idx].val = *v;
        return 1;
    }
    if (ds_reserve((void**)&t->rec, &t->cap, t->len + 1,
            sizeof(*t->rec))
        < 0) {
        DYN_DS_FREE(pk);
        return -1;
    }
    rec = &t->rec[t->len];
    memset(rec, 0, sizeof(*rec));
    if (ds_key_init(&rec->row, r, rn, ds_ix_hash(r, rn)) < 0 || ds_key_init(&rec->col, c, cn, ds_ix_hash(c, cn)) < 0) {
        DYN_DS_FREE(rec->row.p);
        DYN_DS_FREE(pk);
        return -1;
    }
    rec->pair.p = pk;
    rec->pair.n = pn;
    rec->pair.h = h;
    rec->val = *v;
    if (ds_ix_insert(&t->ix, h, pk, pn, t->len) < 0) {
        DYN_DS_FREE(rec->row.p);
        DYN_DS_FREE(rec->col.p);
        DYN_DS_FREE(pk);
        return -1;
    }
    t->len++;
    if (t->slices_valid && ds_table_build_slices(t) < 0)
        ds_table_drop_slices(t);
    return 0;
}

const dyn_cell_t* dyn_table_get(const dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn)
{
    char pk_inl[DS_TABLE_PAIR_INLINE];
    char* pk;
    size_t pn;
    uint32_t pos;
    const dyn_cell_t* out = NULL;

    if (ds_table_pair_into(pk_inl, &pk, &pn, r, rn, c, cn) < 0)
        return NULL;
    pos = ds_ix_find(&t->ix, ds_ix_hash(pk, pn), pk, pn);
    if (pos != UINT32_MAX)
        out = &t->rec[t->ix.slot[pos].idx].val;
    ds_table_pair_release(pk_inl, pk);
    return out;
}

int dyn_table_remove(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, dyn_cell_t* out)
{
    char pk_inl[DS_TABLE_PAIR_INLINE];
    char* pk;
    size_t pn;
    uint32_t pos, idx;

    if (ds_table_pair_into(pk_inl, &pk, &pn, r, rn, c, cn) < 0)
        return 0;
    pos = ds_ix_find(&t->ix, ds_ix_hash(pk, pn), pk, pn);
    ds_table_pair_release(pk_inl, pk);
    if (pos == UINT32_MAX)
        return 0;
    idx = t->ix.slot[pos].idx;
    *out = t->rec[idx].val;
    ds_ix_erase(&t->ix, pos);
    if (t->slices_valid) {
        ds_table_chain_unlink(t, 0, idx);
        ds_table_chain_unlink(t, 1, idx);
    }
    DYN_DS_FREE(t->rec[idx].row.p);
    DYN_DS_FREE(t->rec[idx].col.p);
    DYN_DS_FREE(t->rec[idx].pair.p);
    t->len--;
    if (idx != t->len) {
        t->rec[idx] = t->rec[t->len];
        ds_ix_move(&t->ix, t->rec[idx].pair.h, t->rec[idx].pair.p,
            t->rec[idx].pair.n, idx);
        if (t->slices_valid) {
            ds_table_chain_move(t, 0, t->len, idx);
            ds_table_chain_move(t, 1, t->len, idx);
        }
    }
    if (t->slices_valid)
        t->sliced = t->len;
    return 1;
}

uint32_t dyn_table_size(const dyn_table_t* t) { return t->len; }

int dyn_table_at(const dyn_table_t* t, uint32_t i, const char** r, size_t* rn,
    const char** c, size_t* cn, const dyn_cell_t** v)
{
    if (i >= t->len)
        return 0;
    *r = t->rec[i].row.p;
    *rn = t->rec[i].row.n;
    *c = t->rec[i].col.p;
    *cn = t->rec[i].col.n;
    *v = &t->rec[i].val;
    return 1;
}

typedef struct {
    char* p;
    uint32_t len, cap, gs;
} ds_gapvec;

static inline void* ds_gv_at(const ds_gapvec* g, size_t esz, uint32_t i)
{
    return g->p + (size_t)(i < g->gs ? i : i + (g->cap - g->len)) * esz;
}

static void ds_gv_move_gap(ds_gapvec* g, size_t esz, uint32_t at)
{
    uint32_t gap = g->cap - g->len;
    if (gap && at < g->gs)
        memmove(g->p + (size_t)(at + gap) * esz, g->p + (size_t)at * esz,
            (size_t)(g->gs - at) * esz);
    else if (gap && at > g->gs)
        memmove(g->p + (size_t)g->gs * esz, g->p + (size_t)(g->gs + gap) * esz,
            (size_t)(at - g->gs) * esz);
    g->gs = at;
}

static int ds_gv_splice(ds_gapvec* g, size_t esz, uint32_t at, uint32_t ndel,
    const void* ins, uint32_t nins)
{
    uint32_t need = g->len - ndel + nins;
    if (need > g->cap) {
        ds_gv_move_gap(g, esz, g->len);
        if (ds_reserve((void**)&g->p, &g->cap, need, esz) < 0)
            return -1;
    }
    ds_gv_move_gap(g, esz, at);
    g->len -= ndel;
    if (nins)
        memcpy(g->p + (size_t)g->gs * esz, ins, (size_t)nins * esz);
    g->gs += nins;
    g->len += nins;
    return 0;
}

typedef struct {
    double lo, hi;
} ds_span;

struct dyn_rset {
    ds_gapvec g;
};

#define DS_RS(set, i) ((ds_span*)ds_gv_at(&(set)->g, sizeof(ds_span), (i)))

static uint32_t ds_rs_first_hi(const dyn_rset_t* s, double x, int strict)
{
    const ds_span* p = (const ds_span*)s->g.p;
    size_t off;
    uint32_t a, b;
    if (s->g.gs > 0 && (strict ? p[s->g.gs - 1].hi > x : p[s->g.gs - 1].hi >= x)) {
        a = 0;
        b = s->g.gs - 1;
        off = 0;
    } else {
        a = s->g.gs;
        b = s->g.len;
        off = s->g.cap - s->g.len;
    }
    while (a < b) {
        uint32_t mid = a + (b - a) / 2;
        if (strict ? p[off + mid].hi > x : p[off + mid].hi >= x)
            b = mid;
        else
            a = mid + 1;
    }
    return a;
}

dyn_rset_t* dyn_rset_new(void)
{
    return (dyn_rset_t*)ds_calloc(1, sizeof(dyn_rset_t));
}

void dyn_rset_free(dyn_rset_t* s)
{
    if (!s)
        return;
    DYN_DS_FREE(s->g.p);
    DYN_DS_FREE(s);
}

void dyn_rset_clear(dyn_rset_t* s)
{
    s->g.len = 0;
    s->g.gs = 0;
}

static uint32_t ds_rset_lower(const dyn_rset_t* s, double x)
{
    return ds_rs_first_hi(s, x, 1);
}

int dyn_rset_add(dyn_rset_t* s, double lo, double hi)
{
    uint32_t i, j;
    ds_span merged;

    if (!(hi > lo))
        return 0;
    i = ds_rs_first_hi(s, lo, 0);
    j = i;
    merged.lo = lo;
    merged.hi = hi;
    while (j < s->g.len && DS_RS(s, j)->lo <= merged.hi) {
        const ds_span* c = DS_RS(s, j);
        if (c->lo < merged.lo)
            merged.lo = c->lo;
        if (c->hi > merged.hi)
            merged.hi = c->hi;
        j++;
    }
    return ds_gv_splice(&s->g, sizeof(ds_span), i, j - i, &merged, 1);
}

int dyn_rset_remove(dyn_rset_t* s, double lo, double hi)
{
    uint32_t i, j, n = 0;
    ds_span keep[2];

    if (!(hi > lo))
        return 0;
    i = ds_rset_lower(s, lo);
    j = i;
    while (j < s->g.len && DS_RS(s, j)->lo < hi)
        j++;
    if (j == i)
        return 0;
    if (DS_RS(s, i)->lo < lo) {
        keep[n].lo = DS_RS(s, i)->lo;
        keep[n].hi = lo;
        n++;
    }
    if (DS_RS(s, j - 1)->hi > hi) {
        keep[n].lo = hi;
        keep[n].hi = DS_RS(s, j - 1)->hi;
        n++;
    }
    return ds_gv_splice(&s->g, sizeof(ds_span), i, j - i, keep, n);
}

int dyn_rset_contains(const dyn_rset_t* s, double x)
{
    uint32_t i = ds_rset_lower(s, x);
    return i < s->g.len && DS_RS(s, i)->lo <= x;
}

int dyn_rset_encloses(const dyn_rset_t* s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 1;
    i = ds_rset_lower(s, lo);
    return i < s->g.len && DS_RS(s, i)->lo <= lo && DS_RS(s, i)->hi >= hi;
}

int dyn_rset_intersects(const dyn_rset_t* s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 0;
    i = ds_rset_lower(s, lo);
    return i < s->g.len && DS_RS(s, i)->lo < hi;
}

uint32_t dyn_rset_count(const dyn_rset_t* s) { return s->g.len; }

int dyn_rset_at(const dyn_rset_t* s, uint32_t i, double* lo, double* hi)
{
    if (i >= s->g.len)
        return 0;
    *lo = DS_RS(s, i)->lo;
    *hi = DS_RS(s, i)->hi;
    return 1;
}

int dyn_rset_complement(const dyn_rset_t* s, double lo, double hi,
    dyn_rset_t* out)
{
    double cur = lo;
    uint32_t i;

    dyn_rset_clear(out);
    if (!(hi > lo))
        return 0;
    for (i = 0; i < s->g.len; i++) {
        const ds_span* c = DS_RS(s, i);
        if (c->hi <= cur)
            continue;
        if (c->lo >= hi)
            break;
        if (c->lo > cur && dyn_rset_add(out, cur, c->lo) < 0)
            return -1;
        if (c->hi > cur)
            cur = c->hi;
        if (cur >= hi)
            break;
    }
    if (cur < hi && dyn_rset_add(out, cur, hi) < 0)
        return -1;
    return 0;
}

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} ds_rmap_span;

struct dyn_rmap {
    ds_gapvec g;
};

#define DS_RM(map, i) ((ds_rmap_span*)ds_gv_at(&(map)->g, sizeof(ds_rmap_span), (i)))

static uint32_t ds_rm_first_hi_above(const dyn_rmap_t* m, double x)
{
    const ds_rmap_span* p = (const ds_rmap_span*)m->g.p;
    size_t off;
    uint32_t a, b;
    if (m->g.gs > 0 && p[m->g.gs - 1].hi > x) {
        a = 0;
        b = m->g.gs - 1;
        off = 0;
    } else {
        a = m->g.gs;
        b = m->g.len;
        off = m->g.cap - m->g.len;
    }
    while (a < b) {
        uint32_t mid = a + (b - a) / 2;
        if (p[off + mid].hi > x)
            b = mid;
        else
            a = mid + 1;
    }
    return a;
}

dyn_rmap_t* dyn_rmap_new(void)
{
    return (dyn_rmap_t*)ds_calloc(1, sizeof(dyn_rmap_t));
}

void dyn_rmap_free(dyn_rmap_t* m, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!m)
        return;
    if (fn)
        for (i = 0; i < m->g.len; i++)
            fn(ud, &DS_RM(m, i)->val);
    DYN_DS_FREE(m->g.p);
    DYN_DS_FREE(m);
}

static int ds_rmap_carve(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud,
    uint32_t* at)
{
    uint32_t i, j;

    i = ds_rm_first_hi_above(m, lo);
    j = i;
    while (j < m->g.len && DS_RM(m, j)->lo < hi)
        j++;
    *at = i;
    if (j == i)
        return 0;

    if (DS_RM(m, i)->lo < lo && DS_RM(m, j - 1)->hi > hi && j - i == 1) {
        ds_rmap_span right;
        right.lo = hi;
        right.hi = DS_RM(m, i)->hi;
        dup(ud, &DS_RM(m, i)->val, &right.val);
        if (ds_gv_splice(&m->g, sizeof(ds_rmap_span), i + 1, 0, &right, 1) < 0) {
            if (fn)
                fn(ud, &right.val);
            return -1;
        }
        DS_RM(m, i)->hi = lo;
        *at = i + 1;
        return 0;
    }
    if (DS_RM(m, i)->lo < lo) {
        DS_RM(m, i)->hi = lo;
        i++;
        *at = i;
    }
    if (j > i && DS_RM(m, j - 1)->hi > hi) {
        DS_RM(m, j - 1)->lo = hi;
        j--;
    }
    if (j > i) {
        uint32_t k;
        if (fn)
            for (k = i; k < j; k++)
                fn(ud, &DS_RM(m, k)->val);
        ds_gv_splice(&m->g, sizeof(ds_rmap_span), i, j - i, NULL, 0);
    }
    return 0;
}

int dyn_rmap_put(dyn_rmap_t* m, double lo, double hi, const dyn_cell_t* v,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud)
{
    uint32_t at;
    ds_rmap_span ns;
    if (!(hi > lo))
        return 0;
    if (ds_rmap_carve(m, lo, hi, dup, fn, ud, &at) < 0)
        return -1;
    ns.lo = lo;
    ns.hi = hi;
    ns.val = *v;
    return ds_gv_splice(&m->g, sizeof(ds_rmap_span), at, 0, &ns, 1);
}

int dyn_rmap_remove(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud)
{
    uint32_t at;
    if (!(hi > lo))
        return 0;
    return ds_rmap_carve(m, lo, hi, dup, fn, ud, &at);
}

const dyn_cell_t* dyn_rmap_get(const dyn_rmap_t* m, double x)
{
    uint32_t lo = ds_rm_first_hi_above(m, x);
    return (lo < m->g.len && DS_RM(m, lo)->lo <= x) ? &DS_RM(m, lo)->val : NULL;
}

uint32_t dyn_rmap_count(const dyn_rmap_t* m) { return m->g.len; }

int dyn_rmap_at(const dyn_rmap_t* m, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v)
{
    if (i >= m->g.len)
        return 0;
    *lo = DS_RM(m, i)->lo;
    *hi = DS_RM(m, i)->hi;
    *v = &DS_RM(m, i)->val;
    return 1;
}

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} ds_ival;

struct dyn_itree {
    ds_ival* v;
    uint32_t len, cap;
    double* mx;
    uint32_t sz;
    uint32_t mxcap;
    uint32_t built;
    int dirty;
    uint32_t qsince;
};

#define DS_ITREE_PENDING_MAX(built) (16u + (built) / 8u)

#define DS_ITREE_BURST_MIN 1024u

dyn_itree_t* dyn_itree_new(void)
{
    return (dyn_itree_t*)ds_calloc(1, sizeof(dyn_itree_t));
}

void dyn_itree_free(dyn_itree_t* t, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!t)
        return;
    if (fn)
        for (i = 0; i < t->len; i++)
            fn(ud, &t->v[i].val);
    DYN_DS_FREE(t->v);
    DYN_DS_FREE(t->mx);
    DYN_DS_FREE(t);
}

int dyn_itree_insert(dyn_itree_t* t, double lo, double hi,
    const dyn_cell_t* v)
{
    if (ds_reserve((void**)&t->v, &t->cap, t->len + 1, sizeof(*t->v)) < 0)
        return -1;
    t->v[t->len].lo = lo;
    t->v[t->len].hi = hi < lo ? lo : hi;
    t->v[t->len].val = *v;
    t->len++;
    t->dirty = 1;
    t->qsince = 0;
    return 0;
}

uint32_t dyn_itree_size(const dyn_itree_t* t) { return t->len; }

static int ds_ival_cmp(const void* a, const void* b)
{
    double x = ((const ds_ival*)a)->lo, y = ((const ds_ival*)b)->lo;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int ds_itree_build(dyn_itree_t* t)
{
    uint32_t sz = 1, i;
    if (!t->dirty)
        return 0;
    if (t->len == 0) {
        t->dirty = 0;
        t->sz = 0;
        t->built = 0;
        t->qsince = 0;
        return 0;
    }
    t->qsince++;
    if (t->built && (t->qsince < 2 || t->len - t->built < DS_ITREE_BURST_MIN) && t->len - t->built <= DS_ITREE_PENDING_MAX(t->built))
        return 0;
    qsort(t->v, t->len, sizeof(*t->v), ds_ival_cmp);
    while (sz < t->len)
        sz *= 2;
    if (ds_reserve((void**)&t->mx, &t->mxcap, 2 * sz, sizeof(*t->mx)) < 0)
        return -1;
    for (i = 0; i < sz; i++)
        t->mx[sz + i] = i < t->len ? t->v[i].hi : -(double)INFINITY;
    for (i = sz - 1; i >= 1; i--)
        t->mx[i] = t->mx[2 * i] > t->mx[2 * i + 1] ? t->mx[2 * i]
                                                   : t->mx[2 * i + 1];
    t->sz = sz;
    t->built = t->len;
    t->dirty = 0;
    t->qsince = 0;
    return 0;
}

uint32_t dyn_itree_query(dyn_itree_t* t, double lo, double hi,
    uint32_t* out, uint32_t cap)
{
    uint32_t stack[128], top = 0, found = 0, k, p;

    if (ds_itree_build(t) < 0 || t->len == 0)
        return 0;
    for (p = t->built; p < t->len; p++)
        if (t->v[p].lo <= hi && t->v[p].hi >= lo) {
            if (found < cap)
                out[found] = p;
            found++;
        }
    if (t->built == 0)
        return found;
    {
        uint32_t a = 0, b = t->built;
        while (a < b) {
            uint32_t mid = a + (b - a) / 2;
            if (t->v[mid].lo <= hi)
                a = mid + 1;
            else
                b = mid;
        }
        k = a;
    }
    if (k == 0)
        return found;

    stack[top++] = 1;
    while (top) {
        uint32_t n = stack[--top], lvl = 0, m = n, base, width;
        while (m > 1) {
            m >>= 1;
            lvl++;
        }
        width = t->sz >> lvl;
        base = (n - (1u << lvl)) * width;
        if (base >= k || t->mx[n] < lo)
            continue;
        if (width == 1) {
            if (found < cap)
                out[found] = base;
            found++;
            continue;
        }
        stack[top++] = 2 * n + 1;
        stack[top++] = 2 * n;
    }
    return found;
}

int dyn_itree_at(const dyn_itree_t* t, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v)
{
    if (i >= t->len)
        return 0;
    *lo = t->v[i].lo;
    *hi = t->v[i].hi;
    *v = &t->v[i].val;
    return 1;
}

int dyn_itree_remove_at(dyn_itree_t* t, uint32_t i, dyn_cell_t* out)
{
    if (i >= t->len)
        return 0;
    *out = t->v[i].val;
    t->len--;
    if (i != t->len)
        t->v[i] = t->v[t->len];
    t->built = 0;
    t->dirty = 1;
    return 1;
}

typedef struct {
    double pri;
    dyn_cell_t val;
} ds_mmnode;

struct dyn_mmheap {
    ds_mmnode* v;
    uint32_t len, cap;
};

dyn_mmheap_t* dyn_mmheap_new(void)
{
    return (dyn_mmheap_t*)ds_calloc(1, sizeof(dyn_mmheap_t));
}

void dyn_mmheap_free(dyn_mmheap_t* h, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!h)
        return;
    if (fn)
        for (i = 0; i < h->len; i++)
            fn(ud, &h->v[i].val);
    DYN_DS_FREE(h->v);
    DYN_DS_FREE(h);
}

uint32_t dyn_mmheap_size(const dyn_mmheap_t* h) { return h->len; }

const dyn_cell_t* dyn_mmheap_cell_at(const dyn_mmheap_t* h, uint32_t i)
{
    return i < h->len ? &h->v[i].val : NULL;
}

double dyn_mmheap_pri_at(const dyn_mmheap_t* h, uint32_t i)
{
    return i < h->len ? h->v[i].pri : 0.0;
}

static int ds_mm_is_min_level(uint32_t i)
{
#if defined(__GNUC__) || defined(__clang__)
    return (31 - __builtin_clz(i + 1)) % 2 == 0;
#else
    int d = 0;
    uint32_t n = i + 1;
    while (n > 1) {
        n >>= 1;
        d++;
    }
    return (d & 1) == 0;
#endif
}

static void ds_mm_swap(ds_mmnode* a, ds_mmnode* b)
{
    ds_mmnode t = *a;
    *a = *b;
    *b = t;
}

static void ds_mm_bubble_up_dir(dyn_mmheap_t* h, uint32_t i, int want_max)
{
    while (i >= 3) {
        uint32_t g = (i - 3) / 4;
        int better = want_max ? (h->v[i].pri > h->v[g].pri)
                              : (h->v[i].pri < h->v[g].pri);
        if (!better)
            return;
        ds_mm_swap(&h->v[i], &h->v[g]);
        i = g;
    }
}

static void ds_mm_bubble_up(dyn_mmheap_t* h, uint32_t i)
{
    uint32_t p;
    if (i == 0)
        return;
    p = (i - 1) / 2;
    if (ds_mm_is_min_level(i)) {
        if (h->v[i].pri > h->v[p].pri) {
            ds_mm_swap(&h->v[i], &h->v[p]);
            ds_mm_bubble_up_dir(h, p, 1);
        } else {
            ds_mm_bubble_up_dir(h, i, 0);
        }
    } else {
        if (h->v[i].pri < h->v[p].pri) {
            ds_mm_swap(&h->v[i], &h->v[p]);
            ds_mm_bubble_up_dir(h, p, 0);
        } else {
            ds_mm_bubble_up_dir(h, i, 1);
        }
    }
}

static uint32_t ds_mm_best_descendant(dyn_mmheap_t* h, uint32_t i, int want_max,
    int* is_gc)
{
    uint32_t best = UINT32_MAX, c, g;
    for (c = 2 * i + 1; c <= 2 * i + 2 && c < h->len; c++) {
        if (best == UINT32_MAX || (want_max ? h->v[c].pri > h->v[best].pri : h->v[c].pri < h->v[best].pri)) {
            best = c;
            *is_gc = 0;
        }
        for (g = 2 * c + 1; g <= 2 * c + 2 && g < h->len; g++)
            if (want_max ? h->v[g].pri > h->v[best].pri
                         : h->v[g].pri < h->v[best].pri) {
                best = g;
                *is_gc = 1;
            }
    }
    return best;
}

static void ds_mm_trickle_down_dir(dyn_mmheap_t* h, uint32_t i, int want_max)
{
    for (;;) {
        int is_gc = 0;
        uint32_t m = ds_mm_best_descendant(h, i, want_max, &is_gc);
        if (m == UINT32_MAX)
            return;
        if (!is_gc) {
            if (want_max ? h->v[m].pri > h->v[i].pri
                         : h->v[m].pri < h->v[i].pri)
                ds_mm_swap(&h->v[m], &h->v[i]);
            return;
        }
        if (!(want_max ? h->v[m].pri > h->v[i].pri
                       : h->v[m].pri < h->v[i].pri))
            return;
        ds_mm_swap(&h->v[m], &h->v[i]);
        {
            uint32_t p = (m - 1) / 2;
            if (want_max ? h->v[m].pri < h->v[p].pri
                         : h->v[m].pri > h->v[p].pri)
                ds_mm_swap(&h->v[m], &h->v[p]);
        }
        i = m;
    }
}

static void ds_mm_trickle_down(dyn_mmheap_t* h, uint32_t i)
{
    ds_mm_trickle_down_dir(h, i, !ds_mm_is_min_level(i));
}

int dyn_mmheap_push(dyn_mmheap_t* h, double pri, const dyn_cell_t* v)
{
    if (ds_reserve((void**)&h->v, &h->cap, h->len + 1, sizeof(*h->v)) < 0)
        return -1;
    h->v[h->len].pri = pri;
    h->v[h->len].val = *v;
    h->len++;
    ds_mm_bubble_up(h, h->len - 1);
    return 0;
}

static uint32_t ds_mm_max_index(const dyn_mmheap_t* h)
{
    if (h->len <= 1)
        return 0;
    if (h->len == 2)
        return 1;
    return h->v[1].pri >= h->v[2].pri ? 1 : 2;
}

static int ds_mm_pop_at(dyn_mmheap_t* h, uint32_t i, double* pri,
    dyn_cell_t* out)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[i].pri;
    *out = h->v[i].val;
    h->len--;
    if (i != h->len) {
        h->v[i] = h->v[h->len];
        ds_mm_trickle_down(h, i);
    }
    return 1;
}

int dyn_mmheap_pop_min(dyn_mmheap_t* h, double* pri, dyn_cell_t* out)
{
    return ds_mm_pop_at(h, 0, pri, out);
}

int dyn_mmheap_pop_max(dyn_mmheap_t* h, double* pri, dyn_cell_t* out)
{
    return ds_mm_pop_at(h, ds_mm_max_index(h), pri, out);
}

int dyn_mmheap_peek_min(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[0].pri;
    *v = &h->v[0].val;
    return 1;
}

int dyn_mmheap_peek_max(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v)
{
    uint32_t i;
    if (h->len == 0)
        return 0;
    i = ds_mm_max_index(h);
    *pri = h->v[i].pri;
    *v = &h->v[i].val;
    return 1;
}

struct dyn_cms {
    uint64_t* c;
    uint32_t width, depth;
    uint64_t total;
};

dyn_cms_t* dyn_cms_new(uint32_t width, uint32_t depth)
{
    dyn_cms_t* s;
    if (width == 0 || depth == 0)
        return NULL;
    if ((uint64_t)width * depth > (uint64_t)1 << 24)
        return NULL;
    s = (dyn_cms_t*)ds_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->c = (uint64_t*)ds_calloc((size_t)width * depth, sizeof(uint64_t));
    if (!s->c) {
        DYN_DS_FREE(s);
        return NULL;
    }
    s->width = width;
    s->depth = depth;
    return s;
}

void dyn_cms_free(dyn_cms_t* s)
{
    if (!s)
        return;
    DYN_DS_FREE(s->c);
    DYN_DS_FREE(s);
}

static void ds_cms_probe(const dyn_cms_t* s, const char* k, size_t n,
    uint64_t* h1, uint64_t* h2)
{
    (void)s;
    *h1 = dyn_ds_xxh64((const uint8_t*)k, n, 0);
    *h2 = ds_mix64(*h1 ^ 0x9E3779B97F4A7C15ULL) | 1;
}

void dyn_cms_add(dyn_cms_t* s, const char* k, size_t n, uint64_t count)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        s->c[(size_t)i * s->width + col] = ds_u64_sat_add(s->c[(size_t)i * s->width + col], count);
    }
    s->total = ds_u64_sat_add(s->total, count);
}

uint64_t dyn_cms_count(const dyn_cms_t* s, const char* k, size_t n)
{
    uint64_t h1, h2, best = UINT64_MAX;
    uint32_t i;
    ds_cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        uint64_t v = s->c[(size_t)i * s->width + col];
        if (v < best)
            best = v;
    }
    return best;
}

uint64_t dyn_cms_total(const dyn_cms_t* s) { return s->total; }
uint32_t dyn_cms_width(const dyn_cms_t* s) { return s->width; }
uint32_t dyn_cms_depth(const dyn_cms_t* s) { return s->depth; }

int dyn_cms_merge(dyn_cms_t* a, const dyn_cms_t* b)
{
    size_t i, n;
    if (a->width != b->width || a->depth != b->depth)
        return -1;
    n = (size_t)a->width * a->depth;
    for (i = 0; i < n; i++)
        a->c[i] = ds_u64_sat_add(a->c[i], b->c[i]);
    a->total = ds_u64_sat_add(a->total, b->total);
    return 0;
}

const uint64_t* dyn_cms_counters(const dyn_cms_t* s) { return s->c; }
uint64_t* dyn_cms_counters_mut(dyn_cms_t* s) { return s->c; }
void dyn_cms_set_total(dyn_cms_t* s, uint64_t total) { s->total = total; }

struct dyn_hll {
    uint8_t* reg;
    uint32_t p, m;
    double cached;
    int dirty;
};

dyn_hll_t* dyn_hll_new(uint32_t precision)
{
    dyn_hll_t* h;
    if (precision < DYN_HLL_MIN_PRECISION || precision > DYN_HLL_MAX_PRECISION)
        return NULL;
    h = (dyn_hll_t*)ds_calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->p = precision;
    h->m = 1u << precision;
    h->reg = (uint8_t*)ds_calloc(h->m, 1);
    if (!h->reg) {
        DYN_DS_FREE(h);
        return NULL;
    }
    return h;
}

void dyn_hll_free(dyn_hll_t* h)
{
    if (!h)
        return;
    DYN_DS_FREE(h->reg);
    DYN_DS_FREE(h);
}

void dyn_hll_add_hash(dyn_hll_t* h, uint64_t hash)
{
    uint32_t idx = (uint32_t)(hash >> (64 - h->p));
    uint64_t w = (hash << h->p) | ((uint64_t)1 << (h->p - 1));
    uint8_t rank = 1;
    while ((w & ((uint64_t)1 << 63)) == 0) {
        rank++;
        w <<= 1;
    }
    if (rank > h->reg[idx]) {
        h->reg[idx] = rank;
        h->dirty = 1;
    }
}

void dyn_hll_add(dyn_hll_t* h, const char* k, size_t n)
{
    dyn_hll_add_hash(h, dyn_ds_xxh64((const uint8_t*)k, n, 0));
}

static const double dyn_hll_inv_pow2[256] = {
    1.0 / (double)(1ULL << 0),
    1.0 / (double)(1ULL << 1),
    1.0 / (double)(1ULL << 2),
    1.0 / (double)(1ULL << 3),
    1.0 / (double)(1ULL << 4),
    1.0 / (double)(1ULL << 5),
    1.0 / (double)(1ULL << 6),
    1.0 / (double)(1ULL << 7),
    1.0 / (double)(1ULL << 8),
    1.0 / (double)(1ULL << 9),
    1.0 / (double)(1ULL << 10),
    1.0 / (double)(1ULL << 11),
    1.0 / (double)(1ULL << 12),
    1.0 / (double)(1ULL << 13),
    1.0 / (double)(1ULL << 14),
    1.0 / (double)(1ULL << 15),
    1.0 / (double)(1ULL << 16),
    1.0 / (double)(1ULL << 17),
    1.0 / (double)(1ULL << 18),
    1.0 / (double)(1ULL << 19),
    1.0 / (double)(1ULL << 20),
    1.0 / (double)(1ULL << 21),
    1.0 / (double)(1ULL << 22),
    1.0 / (double)(1ULL << 23),
    1.0 / (double)(1ULL << 24),
    1.0 / (double)(1ULL << 25),
    1.0 / (double)(1ULL << 26),
    1.0 / (double)(1ULL << 27),
    1.0 / (double)(1ULL << 28),
    1.0 / (double)(1ULL << 29),
    1.0 / (double)(1ULL << 30),
    1.0 / (double)(1ULL << 31),
    1.0 / (double)(1ULL << 32),
    1.0 / (double)(1ULL << 33),
    1.0 / (double)(1ULL << 34),
    1.0 / (double)(1ULL << 35),
    1.0 / (double)(1ULL << 36),
    1.0 / (double)(1ULL << 37),
    1.0 / (double)(1ULL << 38),
    1.0 / (double)(1ULL << 39),
    1.0 / (double)(1ULL << 40),
    1.0 / (double)(1ULL << 41),
    1.0 / (double)(1ULL << 42),
    1.0 / (double)(1ULL << 43),
    1.0 / (double)(1ULL << 44),
    1.0 / (double)(1ULL << 45),
    1.0 / (double)(1ULL << 46),
    1.0 / (double)(1ULL << 47),
    1.0 / (double)(1ULL << 48),
    1.0 / (double)(1ULL << 49),
    1.0 / (double)(1ULL << 50),
    1.0 / (double)(1ULL << 51),
    1.0 / (double)(1ULL << 52),
    1.0 / (double)(1ULL << 53),
    1.0 / (double)(1ULL << 54),
    1.0 / (double)(1ULL << 55),
    1.0 / (double)(1ULL << 56),
    1.0 / (double)(1ULL << 57),
    1.0 / (double)(1ULL << 58),
    1.0 / (double)(1ULL << 59),
    1.0 / (double)(1ULL << 60),
    1.0 / (double)(1ULL << 61),
    1.0 / (double)(1ULL << 62),
    1.0 / (double)(1ULL << 63),
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
};

#define DYN_HLL_NACC 8
_Static_assert((DYN_HLL_NACC & (DYN_HLL_NACC - 1)) == 0,
    "NACC must be a power of two: the bound is m & ~(NACC-1), and a "
    "non-power rounds UP, reading past the registers");

double dyn_hll_count(dyn_hll_t* h)
{
    double m = (double)h->m, sum = 0.0, alpha, est;
    uint32_t i, zeros = 0;
    double s[DYN_HLL_NACC];
    uint32_t z[DYN_HLL_NACC], lim, j;
    const uint8_t* reg = h->reg;

    if (!h->dirty)
        return h->cached;

    for (j = 0; j < DYN_HLL_NACC; j++) {
        s[j] = 0.0;
        z[j] = 0;
    }
    lim = h->m & ~(uint32_t)(DYN_HLL_NACC - 1);
    for (i = 0; i < lim; i += DYN_HLL_NACC)
        for (j = 0; j < DYN_HLL_NACC; j++) {
            s[j] += dyn_hll_inv_pow2[reg[i + j]];
            z[j] += (reg[i + j] == 0);
        }
    for (; i < h->m; i++) {
        s[0] += dyn_hll_inv_pow2[reg[i]];
        z[0] += (reg[i] == 0);
    }
    for (j = 0; j < DYN_HLL_NACC; j++) {
        sum += s[j];
        zeros += z[j];
    }
    if (h->m == 16)
        alpha = 0.673;
    else if (h->m == 32)
        alpha = 0.697;
    else if (h->m == 64)
        alpha = 0.709;
    else
        alpha = 0.7213 / (1.0 + 1.079 / m);

    est = alpha * m * m / sum;
    if (est <= 2.5 * m && zeros > 0)
        est = m * log(m / (double)zeros);
    ((dyn_hll_t*)h)->cached = est;
    ((dyn_hll_t*)h)->dirty = 0;
    return est;
}

uint32_t dyn_hll_precision(const dyn_hll_t* h) { return h->p; }
uint32_t dyn_hll_registers(const dyn_hll_t* h) { return h->m; }

int dyn_hll_merge(dyn_hll_t* a, const dyn_hll_t* b)
{
    uint32_t i;
    if (a->p != b->p)
        return -1;
    for (i = 0; i < a->m; i++)
        if (b->reg[i] > a->reg[i])
            a->reg[i] = b->reg[i];
    a->dirty = 1;
    return 0;
}

const uint8_t* dyn_hll_data(const dyn_hll_t* h) { return h->reg; }
uint8_t* dyn_hll_data_mut(dyn_hll_t* h)
{
    h->dirty = 1;
    return h->reg;
}

int dyn_hll_regs_valid(const dyn_hll_t* h)
{
    uint32_t i, max = 64 - h->p + 1;
    for (i = 0; i < h->m; i++)
        if (h->reg[i] > max)
            return -1;
    return 0;
}

typedef struct ds_btree_node {
    struct ds_btree_node* parent;
    uint16_t n;
    uint8_t leaf;
    double key[DYN_BTREE_ORDER];
    union {
        struct {
            dyn_cell_t val[DYN_BTREE_ORDER];
            struct ds_btree_node *next, *prev;
        } l;
        struct {
            struct ds_btree_node* kid[DYN_BTREE_ORDER + 1];
        } i;
    } u;
} ds_btree_node;

struct dyn_btree {
    ds_btree_node* root;
    ds_btree_node* head;
    uint32_t count;
};

static uint32_t ds_btree_lower(const ds_btree_node* nd, double k)
{
    uint32_t i = 0;
    while (i < nd->n && nd->key[i] < k)
        i++;
    return i;
}

static ds_btree_node* ds_btree_node_new(int leaf)
{
    ds_btree_node* nd = (ds_btree_node*)ds_calloc(1, sizeof(*nd));
    if (nd)
        nd->leaf = (uint8_t)leaf;
    return nd;
}

dyn_btree_t* dyn_btree_new(void)
{
    dyn_btree_t* t = (dyn_btree_t*)ds_calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->root = ds_btree_node_new(1);
    if (!t->root) {
        DYN_DS_FREE(t);
        return NULL;
    }
    t->head = t->root;
    return t;
}

static void dyn_btree_free_kids(ds_btree_node* nd, dyn_cell_free_fn fn, void* ud)
{
    if (nd->leaf) {
        uint32_t i;
        if (fn)
            for (i = 0; i < nd->n; i++)
                fn(ud, &nd->u.l.val[i]);
    } else {
        uint32_t i;
        for (i = 0; i <= nd->n; i++)
            if (nd->u.i.kid[i])
                dyn_btree_free_kids(nd->u.i.kid[i], fn, ud);
    }
    DYN_DS_FREE(nd);
}

void dyn_btree_free(dyn_btree_t* t, dyn_cell_free_fn fn, void* ud)
{
    ds_btree_node** st = NULL;
    uint32_t len = 0, cap = 0;
    if (!t)
        return;
    if (t->root && ds_reserve((void**)&st, &cap, 1, sizeof(*st)) == 0)
        st[len++] = t->root;
    while (len) {
        ds_btree_node* nd = st[--len];
        if (nd->leaf) {
            uint32_t i;
            if (fn)
                for (i = 0; i < nd->n; i++)
                    fn(ud, &nd->u.l.val[i]);
        } else {
            uint32_t i;
            for (i = 0; i <= nd->n; i++) {
                if (!nd->u.i.kid[i])
                    continue;
                if (ds_reserve((void**)&st, &cap, len + 1,
                        sizeof(*st))
                    < 0) {
                    dyn_btree_free_kids(nd->u.i.kid[i], fn, ud);
                    continue;
                }
                st[len++] = nd->u.i.kid[i];
            }
        }
        DYN_DS_FREE(nd);
    }
    DYN_DS_FREE(st);
    DYN_DS_FREE(t);
}

uint32_t dyn_btree_size(const dyn_btree_t* t) { return t->count; }

static ds_btree_node* ds_btree_find_leaf(const dyn_btree_t* t, double k)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf) {
        uint32_t i = ds_btree_lower(nd, k);
        if (i < nd->n && nd->key[i] == k)
            i++;
        nd = nd->u.i.kid[i];
    }
    return nd;
}

const dyn_cell_t* dyn_btree_get(const dyn_btree_t* t, double k)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    if (i < nd->n && nd->key[i] == k)
        return &nd->u.l.val[i];
    return NULL;
}

static void ds_btree_ins_internal(ds_btree_node* nd, uint32_t at, double key,
    ds_btree_node* kid)
{
    uint32_t j;
    for (j = nd->n; j > at; j--) {
        nd->key[j] = nd->key[j - 1];
        nd->u.i.kid[j + 1] = nd->u.i.kid[j];
    }
    nd->key[at] = key;
    nd->u.i.kid[at + 1] = kid;
    nd->n++;
    kid->parent = nd;
}

#define DYN_BTREE_MAX_CHAIN 16

static int ds_btree_split(dyn_btree_t* t, ds_btree_node* nd)
{
    ds_btree_node* pre[DYN_BTREE_MAX_CHAIN];
    ds_btree_node* new_root = NULL;
    uint32_t mid = DYN_BTREE_ORDER / 2, j, nn = 0, next_right = 0;
    ds_btree_node* p;

    if (nd->n != DYN_BTREE_ORDER)
        return 0;
    p = nd;
    for (;;) {
        if (nn >= DYN_BTREE_MAX_CHAIN)
            return -1;
        pre[nn] = ds_btree_node_new(p->leaf);
        if (!pre[nn])
            goto oom;
        nn++;
        if (!p->parent)
            break;
        p = p->parent;
        if (p->n != DYN_BTREE_ORDER - 1)
            break;
    }
    if (!p->parent) {
        new_root = ds_btree_node_new(0);
        if (!new_root)
            goto oom;
    }

    while (nd->n == DYN_BTREE_ORDER) {
        ds_btree_node *right = pre[next_right++], *par;
        double up;
        if (nd->leaf) {
            right->n = (uint16_t)(nd->n - mid);
            for (j = 0; j < right->n; j++) {
                right->key[j] = nd->key[mid + j];
                right->u.l.val[j] = nd->u.l.val[mid + j];
            }
            nd->n = (uint16_t)mid;
            right->u.l.next = nd->u.l.next;
            right->u.l.prev = nd;
            if (nd->u.l.next)
                nd->u.l.next->u.l.prev = right;
            nd->u.l.next = right;
            up = right->key[0];
        } else {
            right->n = (uint16_t)(nd->n - mid - 1);
            for (j = 0; j < right->n; j++)
                right->key[j] = nd->key[mid + 1 + j];
            for (j = 0; j <= right->n; j++) {
                right->u.i.kid[j] = nd->u.i.kid[mid + 1 + j];
                right->u.i.kid[j]->parent = right;
            }
            up = nd->key[mid];
            nd->n = (uint16_t)mid;
        }
        par = nd->parent;
        if (par == NULL) {
            par = new_root;
            par->n = 1;
            par->key[0] = up;
            par->u.i.kid[0] = nd;
            par->u.i.kid[1] = right;
            nd->parent = par;
            right->parent = par;
            t->root = par;
            return 0;
        }
        ds_btree_ins_internal(par, ds_btree_lower(par, up), up, right);
        nd = par;
    }
    while (next_right < nn)
        DYN_DS_FREE(pre[next_right++]);
    DYN_DS_FREE(new_root);
    return 0;
oom:
    while (nn > 0)
        DYN_DS_FREE(pre[--nn]);
    DYN_DS_FREE(new_root);
    return -1;
}

int dyn_btree_set(dyn_btree_t* t, double k, const dyn_cell_t* v,
    dyn_cell_t* old, int* replaced)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k), j;

    if (i < nd->n && nd->key[i] == k) {
        *old = nd->u.l.val[i];
        nd->u.l.val[i] = *v;
        *replaced = 1;
        return 0;
    }
    *replaced = 0;
    for (j = nd->n; j > i; j--) {
        nd->key[j] = nd->key[j - 1];
        nd->u.l.val[j] = nd->u.l.val[j - 1];
    }
    nd->key[i] = k;
    nd->u.l.val[i] = *v;
    nd->n++;
    t->count++;
    if (ds_btree_split(t, nd) < 0) {
        for (j = i; j + 1 < nd->n; j++) {
            nd->key[j] = nd->key[j + 1];
            nd->u.l.val[j] = nd->u.l.val[j + 1];
        }
        nd->n--;
        t->count--;
        return -1;
    }
    return 0;
}

int dyn_btree_del(dyn_btree_t* t, double k, dyn_cell_t* out)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k), j;

    if (i >= nd->n || nd->key[i] != k)
        return 0;
    *out = nd->u.l.val[i];
    for (j = i; j + 1 < nd->n; j++) {
        nd->key[j] = nd->key[j + 1];
        nd->u.l.val[j] = nd->u.l.val[j + 1];
    }
    nd->n--;
    t->count--;
    return 1;
}

static ds_btree_node* ds_btree_leftmost(const dyn_btree_t* t)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[0];
    return nd;
}

int dyn_btree_iter_begin(const dyn_btree_t* t, dyn_btree_iter* it)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    while (nd && nd->n == 0)
        nd = nd->u.l.next;
    it->leaf = nd;
    it->i = 0;
    return nd != NULL;
}

int dyn_btree_iter_seek(const dyn_btree_t* t, double k, dyn_btree_iter* it)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    while (nd && i >= nd->n) {
        nd = nd->u.l.next;
        i = 0;
        while (nd && nd->n == 0)
            nd = nd->u.l.next;
    }
    it->leaf = nd;
    it->i = i;
    return nd != NULL;
}

int dyn_btree_iter_get(const dyn_btree_iter* it, double* k,
    const dyn_cell_t** v)
{
    const ds_btree_node* nd = (const ds_btree_node*)it->leaf;
    if (!nd || it->i >= nd->n)
        return 0;
    *k = nd->key[it->i];
    *v = &nd->u.l.val[it->i];
    return 1;
}

int dyn_btree_iter_next(dyn_btree_iter* it)
{
    ds_btree_node* nd = (ds_btree_node*)it->leaf;
    if (!nd)
        return 0;
    it->i++;
    while (nd && it->i >= nd->n) {
        nd = nd->u.l.next;
        it->i = 0;
    }
    it->leaf = nd;
    return nd != NULL;
}

int dyn_btree_first(const dyn_btree_t* t, double* out)
{
    dyn_btree_iter it;
    const dyn_cell_t* v;
    if (!dyn_btree_iter_begin(t, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_last(const dyn_btree_t* t, double* out)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[nd->n];
    while (nd && nd->n == 0)
        nd = nd->u.l.prev;
    if (!nd)
        return 0;
    *out = nd->key[nd->n - 1];
    return 1;
}

uint32_t dyn_btree_get_keys(const dyn_btree_t* t, double* out)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    uint32_t n = 0;
    while (nd) {
        if (nd->n) {
            memcpy(out + n, nd->key, (size_t)nd->n * sizeof(double));
            n += nd->n;
        }
        nd = nd->u.l.next;
    }
    return n;
}

uint32_t dyn_btree_walk_leaves(const dyn_btree_t* t, dyn_btree_leaf_fn fn,
    void* ud)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    uint32_t n = 0;
    while (nd) {
        if (nd->n) {
            if (!fn(ud, nd->key, nd->u.l.val, nd->n))
                break;
            n += nd->n;
        }
        nd = nd->u.l.next;
    }
    return n;
}

int dyn_btree_ceil(const dyn_btree_t* t, double k, double* out)
{
    dyn_btree_iter it;
    const dyn_cell_t* v;
    if (!dyn_btree_iter_seek(t, k, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_floor(const dyn_btree_t* t, double k, double* out)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    if (i < nd->n && nd->key[i] == k) {
        *out = k;
        return 1;
    }
    while (nd && i == 0) {
        nd = nd->u.l.prev;
        i = nd ? nd->n : 0;
    }
    if (!nd || i == 0)
        return 0;
    *out = nd->key[i - 1];
    return 1;
}

static uint64_t ds_ops_hash(const dyn_ds_ops* o, const void* e)
{
    return o->hash ? o->hash(e, o->ud) : dyn_ds_xxh64(e, o->size, 0x51ED270B27C56C4BULL);
}

static int ds_ops_eq(const dyn_ds_ops* o, const void* a, const void* b)
{
    return o->eq ? o->eq(a, b, o->ud) : !memcmp(a, b, o->size);
}

static int ds_ops_cmp(const dyn_ds_ops* o, const void* a, const void* b)
{
    return o->cmp ? o->cmp(a, b, o->ud) : memcmp(a, b, o->size);
}

static void ds_ops_copy(const dyn_ds_ops* o, void* dst, const void* src)
{
    if (o->copy)
        o->copy(dst, src, o->ud);
    else if (o->size)
        memcpy(dst, src, o->size);
}

static void ds_ops_release(const dyn_ds_ops* o, void* e)
{
    if (o->release)
        o->release(e, o->ud);
}

static void ds_ops_init(dyn_ds_ops* dst, const dyn_ds_ops* src)
{
    if (src)
        *dst = *src;
    else
        memset(dst, 0, sizeof(*dst));
}

static size_t ds_align8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

#if !defined(DYN_DS_NO_SIMD) && defined(__aarch64__)
#include <arm_neon.h>
#define DS_GROUP 16
#define DS_LANE_SHIFT 2
static inline uint64_t ds_group_match(const uint8_t* g, uint8_t tag)
{
    uint8x16_t c = vceqq_u8(vld1q_u8(g), vdupq_n_u8(tag));
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(c), 4)), 0) & 0x8888888888888888ULL;
}
static inline uint64_t ds_group_empty(const uint8_t* g)
{
    return ds_group_match(g, 0x80);
}
static inline uint64_t ds_group_free(const uint8_t* g)
{
    uint8x16_t c = vcltq_s8(vreinterpretq_s8_u8(vld1q_u8(g)), vdupq_n_s8(0));
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(c), 4)), 0) & 0x8888888888888888ULL;
}
const char* dyn_swiss_group_impl(void)
{
    return "neon16";
}
#elif !defined(DYN_DS_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#define DS_GROUP 16
#define DS_LANE_SHIFT 0
static inline uint64_t ds_group_match(const uint8_t* g, uint8_t tag)
{
    __m128i v = _mm_loadu_si128((const __m128i*)(const void*)g);
    return (uint64_t)(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8((char)tag)));
}
static inline uint64_t ds_group_empty(const uint8_t* g)
{
    return ds_group_match(g, 0x80);
}
static inline uint64_t ds_group_free(const uint8_t* g)
{
    __m128i v = _mm_loadu_si128((const __m128i*)(const void*)g);
    return (uint64_t)(uint32_t)_mm_movemask_epi8(v);
}
const char* dyn_swiss_group_impl(void)
{
    return "sse2x16";
}
#else
#define DS_GROUP 8
#define DS_LANE_SHIFT 3
static inline uint64_t ds_group_load(const uint8_t* p)
{
    uint64_t w;
    memcpy(&w, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    w = __builtin_bswap64(w);
#endif
    return w;
}
static inline uint64_t ds_group_match(const uint8_t* g, uint8_t tag)
{
    uint64_t x = ds_group_load(g) ^ (0x0101010101010101ULL * tag);
    return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
}
static inline uint64_t ds_group_empty(const uint8_t* g)
{
    uint64_t w = ds_group_load(g);
    return w & ~(w << 6) & 0x8080808080808080ULL;
}
static inline uint64_t ds_group_free(const uint8_t* g)
{
    return ds_group_load(g) & 0x8080808080808080ULL;
}
const char* dyn_swiss_group_impl(void)
{
    return "swar8";
}
#endif

#define DS_CTRL_EMPTY 0x80
#define DS_CTRL_DELETED 0xFE

static inline unsigned ds_lowest_lane(uint64_t bits)
{
    return (unsigned)(__builtin_ctzll(bits) >> DS_LANE_SHIFT);
}

struct dyn_swiss {
    dyn_ds_ops kops, vops;
    size_t stride, voff;
    uint8_t* ctrl;
    uint8_t* slots;
    size_t mask;
    size_t size;
    size_t growth_left;
};

static size_t ds_swiss_cap_for(size_t n)
{
    size_t c = 16;
    while (c - c / 8 < n) {
        if (c > ((size_t)1 << 40))
            return 0;
        c <<= 1;
    }
    return c;
}

static int ds_swiss_alloc(dyn_swiss_t* m, size_t cap)
{
    size_t slot_bytes = cap * m->stride;
    m->ctrl = (uint8_t*)DYN_DS_MALLOC(cap + DS_GROUP);
    m->slots = (uint8_t*)DYN_DS_MALLOC(slot_bytes ? slot_bytes : 1);
    if (!m->ctrl || !m->slots) {
        DYN_DS_FREE(m->ctrl);
        DYN_DS_FREE(m->slots);
        m->ctrl = NULL;
        m->slots = NULL;
        return -1;
    }
    memset(m->ctrl, DS_CTRL_EMPTY, cap + DS_GROUP);
    m->mask = cap - 1;
    m->growth_left = cap - cap / 8;
    return 0;
}

static inline void ds_swiss_set_ctrl(dyn_swiss_t* m, size_t i, uint8_t v)
{
    m->ctrl[i] = v;
    if (i < DS_GROUP)
        m->ctrl[m->mask + 1 + i] = v;
}

static inline uint8_t* ds_swiss_slot(const dyn_swiss_t* m, size_t i)
{
    return m->slots + i * m->stride;
}

static size_t ds_swiss_find_free(const dyn_swiss_t* m, uint64_t h)
{
    size_t pos = (size_t)h & m->mask, step = 0;
    for (;;) {
        uint64_t bits = ds_group_free(m->ctrl + pos);
        if (bits)
            return (pos + ds_lowest_lane(bits)) & m->mask;
        step += DS_GROUP;
        pos = (pos + step) & m->mask;
    }
}

static long long ds_swiss_find(const dyn_swiss_t* m, const void* key, uint64_t h)
{
    size_t pos, step = 0;
    uint8_t tag = (uint8_t)(h >> 57);
    if (!m->ctrl)
        return -1;
    pos = (size_t)h & m->mask;
    for (;;) {
        const uint8_t* g = m->ctrl + pos;
        uint64_t bits = ds_group_match(g, tag);
        while (bits) {
            size_t i = (pos + ds_lowest_lane(bits)) & m->mask;
            if (ds_ops_eq(&m->kops, ds_swiss_slot(m, i), key))
                return (long long)i;
            bits &= bits - 1;
        }
        if (ds_group_empty(g))
            return -1;
        step += DS_GROUP;
        pos = (pos + step) & m->mask;
    }
}

static int ds_swiss_resize(dyn_swiss_t* m, size_t cap)
{
    uint8_t *octrl = m->ctrl, *oslots = m->slots;
    size_t ocap = m->ctrl ? m->mask + 1 : 0, i;
    if (ds_swiss_alloc(m, cap) < 0) {
        m->ctrl = octrl;
        m->slots = oslots;
        if (octrl)
            m->mask = ocap - 1;
        return -1;
    }
    for (i = 0; i < ocap; i++) {
        if (octrl[i] < 0x80) {
            uint8_t* src = oslots + i * m->stride;
            uint64_t h = ds_ops_hash(&m->kops, src);
            size_t j = ds_swiss_find_free(m, h);
            ds_swiss_set_ctrl(m, j, (uint8_t)(h >> 57));
            memcpy(ds_swiss_slot(m, j), src, m->stride);
            m->growth_left--;
        }
    }
    DYN_DS_FREE(octrl);
    DYN_DS_FREE(oslots);
    return 0;
}

dyn_swiss_t* dyn_swiss_new(const dyn_ds_ops* key, const dyn_ds_ops* val)
{
    dyn_swiss_t* m = (dyn_swiss_t*)DYN_DS_CALLOC(1, sizeof(*m));
    if (!m)
        return NULL;
    ds_ops_init(&m->kops, key);
    ds_ops_init(&m->vops, val);
    m->voff = ds_align8(m->kops.size);
    m->stride = ds_align8(m->voff + m->vops.size);
    if (!m->stride)
        m->stride = 8;
    return m;
}

void dyn_swiss_clear(dyn_swiss_t* m)
{
    size_t i, cap;
    if (!m->ctrl)
        return;
    cap = m->mask + 1;
    for (i = 0; i < cap; i++) {
        if (m->ctrl[i] < 0x80) {
            uint8_t* s = ds_swiss_slot(m, i);
            ds_ops_release(&m->kops, s);
            ds_ops_release(&m->vops, s + m->voff);
        }
    }
    memset(m->ctrl, DS_CTRL_EMPTY, cap + DS_GROUP);
    m->size = 0;
    m->growth_left = cap - cap / 8;
}

void dyn_swiss_free(dyn_swiss_t* m)
{
    if (!m)
        return;
    dyn_swiss_clear(m);
    DYN_DS_FREE(m->ctrl);
    DYN_DS_FREE(m->slots);
    DYN_DS_FREE(m);
}

size_t dyn_swiss_size(const dyn_swiss_t* m)
{
    return m->size;
}

void* dyn_swiss_get(const dyn_swiss_t* m, const void* key)
{
    long long i = ds_swiss_find(m, key, ds_ops_hash(&m->kops, key));
    return i < 0 ? NULL : ds_swiss_slot(m, (size_t)i) + m->voff;
}

int dyn_swiss_reserve(dyn_swiss_t* m, size_t n)
{
    size_t cap = ds_swiss_cap_for(n);
    if (!cap)
        return -1;
    if (m->ctrl && cap <= m->mask + 1)
        return 0;
    return ds_swiss_resize(m, cap);
}

void* dyn_swiss_upsert(dyn_swiss_t* m, const void* key, int* created)
{
    uint64_t h = ds_ops_hash(&m->kops, key);
    long long i = ds_swiss_find(m, key, h);
    size_t j;
    uint8_t* s;
    if (i >= 0) {
        if (created)
            *created = 0;
        return ds_swiss_slot(m, (size_t)i) + m->voff;
    }
    if (!m->ctrl || m->growth_left == 0) {
        size_t cap = m->ctrl ? m->mask + 1 : 16;
        if (m->ctrl && m->size >= (cap - cap / 8) / 2)
            cap <<= 1;
        if (ds_swiss_resize(m, cap) < 0)
            return NULL;
    }
    j = ds_swiss_find_free(m, h);
    if (m->ctrl[j] == DS_CTRL_EMPTY)
        m->growth_left--;
    ds_swiss_set_ctrl(m, j, (uint8_t)(h >> 57));
    s = ds_swiss_slot(m, j);
    ds_ops_copy(&m->kops, s, key);
    memset(s + m->voff, 0, m->vops.size);
    m->size++;
    if (created)
        *created = 1;
    return s + m->voff;
}

int dyn_swiss_put(dyn_swiss_t* m, const void* key, const void* val)
{
    int created;
    void* v = dyn_swiss_upsert(m, key, &created);
    if (!v)
        return -1;
    if (!created)
        ds_ops_release(&m->vops, v);
    ds_ops_copy(&m->vops, v, val);
    return created;
}

int dyn_swiss_remove(dyn_swiss_t* m, const void* key)
{
    long long i = ds_swiss_find(m, key, ds_ops_hash(&m->kops, key));
    uint8_t* s;
    if (i < 0)
        return 0;
    s = ds_swiss_slot(m, (size_t)i);
    ds_ops_release(&m->kops, s);
    ds_ops_release(&m->vops, s + m->voff);
    ds_swiss_set_ctrl(m, (size_t)i, DS_CTRL_DELETED);
    m->size--;
    return 1;
}

size_t dyn_swiss_next(const dyn_swiss_t* m, size_t pos, const void** key, void** val)
{
    size_t cap;
    if (!m->ctrl)
        return 0;
    cap = m->mask + 1;
    for (; pos < cap; pos++) {
        if (m->ctrl[pos] < 0x80) {
            uint8_t* s = ds_swiss_slot(m, pos);
            if (key)
                *key = s;
            if (val)
                *val = s + m->voff;
            return pos + 1;
        }
    }
    return 0;
}

struct dyn_rbnode {
    struct dyn_rbnode *left, *right, *parent;
    int red;
};

struct dyn_rbtree {
    dyn_ds_ops kops, vops;
    size_t koff, voff, nodesz;
    dyn_rbnode* root;
    size_t size;
};

#define DS_RB_KEY(t, n) ((void*)((uint8_t*)DS_UNCONST(n) + (t)->koff))
#define DS_RB_VAL(t, n) ((void*)((uint8_t*)DS_UNCONST(n) + (t)->voff))

dyn_rbtree_t* dyn_rbtree_new(const dyn_ds_ops* key, const dyn_ds_ops* val)
{
    dyn_rbtree_t* t = (dyn_rbtree_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    ds_ops_init(&t->kops, key);
    ds_ops_init(&t->vops, val);
    t->koff = ds_align8(sizeof(dyn_rbnode));
    t->voff = t->koff + ds_align8(t->kops.size);
    t->nodesz = t->voff + ds_align8(t->vops.size);
    return t;
}

static void ds_rb_free_rec(dyn_rbtree_t* t, dyn_rbnode* n)
{
    while (n) {
        dyn_rbnode* r = n->right;
        ds_rb_free_rec(t, n->left);
        ds_ops_release(&t->kops, DS_RB_KEY(t, n));
        ds_ops_release(&t->vops, DS_RB_VAL(t, n));
        DYN_DS_FREE(n);
        n = r;
    }
}

void dyn_rbtree_free(dyn_rbtree_t* t)
{
    if (!t)
        return;
    ds_rb_free_rec(t, t->root);
    DYN_DS_FREE(t);
}

size_t dyn_rbtree_size(const dyn_rbtree_t* t)
{
    return t->size;
}

static dyn_rbnode* ds_rb_find(const dyn_rbtree_t* t, const void* key)
{
    dyn_rbnode* n = t->root;
    while (n) {
        int c = ds_ops_cmp(&t->kops, key, DS_RB_KEY(t, n));
        if (c == 0)
            return n;
        n = c < 0 ? n->left : n->right;
    }
    return NULL;
}

void* dyn_rbtree_get(const dyn_rbtree_t* t, const void* key)
{
    dyn_rbnode* n = ds_rb_find(t, key);
    return n ? DS_RB_VAL(t, n) : NULL;
}

static void ds_rb_rotate_left(dyn_rbtree_t* t, dyn_rbnode* x)
{
    dyn_rbnode* y = x->right;
    x->right = y->left;
    if (y->left)
        y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        t->root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void ds_rb_rotate_right(dyn_rbtree_t* t, dyn_rbnode* x)
{
    dyn_rbnode* y = x->left;
    x->left = y->right;
    if (y->right)
        y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        t->root = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;
    y->right = x;
    x->parent = y;
}

int dyn_rbtree_put(dyn_rbtree_t* t, const void* key, const void* val)
{
    dyn_rbnode *p = NULL, *n = t->root, *z;
    int c = 0;
    while (n) {
        c = ds_ops_cmp(&t->kops, key, DS_RB_KEY(t, n));
        if (c == 0) {
            ds_ops_release(&t->vops, DS_RB_VAL(t, n));
            ds_ops_copy(&t->vops, DS_RB_VAL(t, n), val);
            return 0;
        }
        p = n;
        n = c < 0 ? n->left : n->right;
    }
    z = (dyn_rbnode*)DYN_DS_MALLOC(t->nodesz);
    if (!z)
        return -1;
    z->left = z->right = NULL;
    z->parent = p;
    z->red = 1;
    ds_ops_copy(&t->kops, DS_RB_KEY(t, z), key);
    ds_ops_copy(&t->vops, DS_RB_VAL(t, z), val);
    if (!p)
        t->root = z;
    else if (c < 0)
        p->left = z;
    else
        p->right = z;
    t->size++;
    while (z->parent && z->parent->red) {
        dyn_rbnode* g = z->parent->parent;
        if (z->parent == g->left) {
            dyn_rbnode* u = g->right;
            if (u && u->red) {
                z->parent->red = 0;
                u->red = 0;
                g->red = 1;
                z = g;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    ds_rb_rotate_left(t, z);
                }
                z->parent->red = 0;
                g->red = 1;
                ds_rb_rotate_right(t, g);
            }
        } else {
            dyn_rbnode* u = g->left;
            if (u && u->red) {
                z->parent->red = 0;
                u->red = 0;
                g->red = 1;
                z = g;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    ds_rb_rotate_right(t, z);
                }
                z->parent->red = 0;
                g->red = 1;
                ds_rb_rotate_left(t, g);
            }
        }
    }
    t->root->red = 0;
    return 1;
}

static dyn_rbnode* ds_rb_min(dyn_rbnode* n)
{
    while (n && n->left)
        n = n->left;
    return n;
}

static dyn_rbnode* ds_rb_max(dyn_rbnode* n)
{
    while (n && n->right)
        n = n->right;
    return n;
}

static void ds_rb_transplant(dyn_rbtree_t* t, dyn_rbnode* u, dyn_rbnode* v)
{
    if (!u->parent)
        t->root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;
    if (v)
        v->parent = u->parent;
}

int dyn_rbtree_remove(dyn_rbtree_t* t, const void* key)
{
    dyn_rbnode *z = ds_rb_find(t, key), *x, *xp, *y;
    int yred;
    if (!z)
        return 0;
    y = z;
    yred = y->red;
    if (!z->left) {
        x = z->right;
        xp = z->parent;
        ds_rb_transplant(t, z, z->right);
    } else if (!z->right) {
        x = z->left;
        xp = z->parent;
        ds_rb_transplant(t, z, z->left);
    } else {
        y = ds_rb_min(z->right);
        yred = y->red;
        x = y->right;
        if (y->parent == z) {
            xp = y;
        } else {
            xp = y->parent;
            ds_rb_transplant(t, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        ds_rb_transplant(t, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->red = z->red;
    }
    if (!yred) {
        while (x != t->root && (!x || !x->red)) {
            if (x == xp->left) {
                dyn_rbnode* w = xp->right;
                if (w->red) {
                    w->red = 0;
                    xp->red = 1;
                    ds_rb_rotate_left(t, xp);
                    w = xp->right;
                }
                if ((!w->left || !w->left->red) && (!w->right || !w->right->red)) {
                    w->red = 1;
                    x = xp;
                    xp = x->parent;
                } else {
                    if (!w->right || !w->right->red) {
                        w->left->red = 0;
                        w->red = 1;
                        ds_rb_rotate_right(t, w);
                        w = xp->right;
                    }
                    w->red = xp->red;
                    xp->red = 0;
                    if (w->right)
                        w->right->red = 0;
                    ds_rb_rotate_left(t, xp);
                    x = t->root;
                    xp = NULL;
                }
            } else {
                dyn_rbnode* w = xp->left;
                if (w->red) {
                    w->red = 0;
                    xp->red = 1;
                    ds_rb_rotate_right(t, xp);
                    w = xp->left;
                }
                if ((!w->right || !w->right->red) && (!w->left || !w->left->red)) {
                    w->red = 1;
                    x = xp;
                    xp = x->parent;
                } else {
                    if (!w->left || !w->left->red) {
                        w->right->red = 0;
                        w->red = 1;
                        ds_rb_rotate_left(t, w);
                        w = xp->left;
                    }
                    w->red = xp->red;
                    xp->red = 0;
                    if (w->left)
                        w->left->red = 0;
                    ds_rb_rotate_right(t, xp);
                    x = t->root;
                    xp = NULL;
                }
            }
        }
        if (x)
            x->red = 0;
    }
    ds_ops_release(&t->kops, DS_RB_KEY(t, z));
    ds_ops_release(&t->vops, DS_RB_VAL(t, z));
    DYN_DS_FREE(z);
    t->size--;
    return 1;
}

dyn_rbnode* dyn_rbtree_first(const dyn_rbtree_t* t)
{
    return ds_rb_min(t->root);
}

dyn_rbnode* dyn_rbtree_last(const dyn_rbtree_t* t)
{
    return ds_rb_max(t->root);
}

dyn_rbnode* dyn_rbtree_next(const dyn_rbnode* n)
{
    const dyn_rbnode* p;
    if (n->right)
        return ds_rb_min(n->right);
    p = n->parent;
    while (p && n == p->right) {
        n = p;
        p = p->parent;
    }
    return (dyn_rbnode*)DS_UNCONST(p);
}

dyn_rbnode* dyn_rbtree_prev(const dyn_rbnode* n)
{
    const dyn_rbnode* p;
    if (n->left)
        return ds_rb_max(n->left);
    p = n->parent;
    while (p && n == p->left) {
        n = p;
        p = p->parent;
    }
    return (dyn_rbnode*)DS_UNCONST(p);
}

dyn_rbnode* dyn_rbtree_lower_bound(const dyn_rbtree_t* t, const void* key)
{
    dyn_rbnode *n = t->root, *best = NULL;
    while (n) {
        if (ds_ops_cmp(&t->kops, DS_RB_KEY(t, n), key) >= 0) {
            best = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}

dyn_rbnode* dyn_rbtree_upper_bound(const dyn_rbtree_t* t, const void* key)
{
    dyn_rbnode *n = t->root, *best = NULL;
    while (n) {
        if (ds_ops_cmp(&t->kops, DS_RB_KEY(t, n), key) > 0) {
            best = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}

const void* dyn_rbtree_key(const dyn_rbtree_t* t, const dyn_rbnode* n)
{
    return (const uint8_t*)n + t->koff;
}

void* dyn_rbtree_val(const dyn_rbtree_t* t, const dyn_rbnode* n)
{
    return (uint8_t*)DS_UNCONST(n) + t->voff;
}

static int ds_rb_check_rec(const dyn_rbtree_t* t, const dyn_rbnode* n, const dyn_rbnode* parent, size_t* count)
{
    int lh, rh;
    if (!n)
        return 1;
    if (n->parent != parent)
        return -1;
    if (n->red && ((n->left && n->left->red) || (n->right && n->right->red)))
        return -1;
    if (n->left && ds_ops_cmp(&t->kops, DS_RB_KEY(t, n->left), DS_RB_KEY(t, n)) >= 0)
        return -1;
    if (n->right && ds_ops_cmp(&t->kops, DS_RB_KEY(t, n->right), DS_RB_KEY(t, n)) <= 0)
        return -1;
    lh = ds_rb_check_rec(t, n->left, n, count);
    rh = ds_rb_check_rec(t, n->right, n, count);
    if (lh < 0 || rh < 0 || lh != rh)
        return -1;
    (*count)++;
    return lh + !n->red;
}

int dyn_rbtree_check(const dyn_rbtree_t* t)
{
    size_t count = 0;
    if (t->root && t->root->red)
        return -1;
    if (ds_rb_check_rec(t, t->root, NULL, &count) < 0)
        return -1;
    return count == t->size ? 0 : -1;
}

typedef struct ds_wbnode {
    struct ds_wbnode *l, *r;
    size_t size;
} ds_wbnode;

struct dyn_wbtree {
    dyn_ds_ops kops, vops;
    size_t koff, voff, nodesz;
    ds_wbnode* root;
};

#define DS_WB_DELTA 3
#define DS_WB_RATIO 2
#define DS_WB_KEY(t, n) ((void*)((uint8_t*)DS_UNCONST(n) + (t)->koff))
#define DS_WB_VAL(t, n) ((void*)((uint8_t*)DS_UNCONST(n) + (t)->voff))

static size_t ds_wb_size(const ds_wbnode* n)
{
    return n ? n->size : 0;
}

static void ds_wb_fix(ds_wbnode* n)
{
    n->size = 1 + ds_wb_size(n->l) + ds_wb_size(n->r);
}

static ds_wbnode* ds_wb_rot_l(ds_wbnode* n)
{
    ds_wbnode* r = n->r;
    n->r = r->l;
    ds_wb_fix(n);
    r->l = n;
    ds_wb_fix(r);
    return r;
}

static ds_wbnode* ds_wb_rot_r(ds_wbnode* n)
{
    ds_wbnode* l = n->l;
    n->l = l->r;
    ds_wb_fix(n);
    l->r = n;
    ds_wb_fix(l);
    return l;
}

static ds_wbnode* ds_wb_balance(ds_wbnode* n)
{
    size_t sl = ds_wb_size(n->l), sr = ds_wb_size(n->r);
    ds_wb_fix(n);
    if (sl + sr <= 1)
        return n;
    if (sr + 1 > DS_WB_DELTA * (sl + 1)) {
        if (ds_wb_size(n->r->l) + 1 >= DS_WB_RATIO * (ds_wb_size(n->r->r) + 1))
            n->r = ds_wb_rot_r(n->r);
        return ds_wb_rot_l(n);
    }
    if (sl + 1 > DS_WB_DELTA * (sr + 1)) {
        if (ds_wb_size(n->l->r) + 1 >= DS_WB_RATIO * (ds_wb_size(n->l->l) + 1))
            n->l = ds_wb_rot_l(n->l);
        return ds_wb_rot_r(n);
    }
    return n;
}

dyn_wbtree_t* dyn_wbtree_new(const dyn_ds_ops* key, const dyn_ds_ops* val)
{
    dyn_wbtree_t* t = (dyn_wbtree_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    ds_ops_init(&t->kops, key);
    ds_ops_init(&t->vops, val);
    t->koff = ds_align8(sizeof(ds_wbnode));
    t->voff = t->koff + ds_align8(t->kops.size);
    t->nodesz = t->voff + ds_align8(t->vops.size);
    return t;
}

static void ds_wb_free_rec(dyn_wbtree_t* t, ds_wbnode* n)
{
    while (n) {
        ds_wbnode* r = n->r;
        ds_wb_free_rec(t, n->l);
        ds_ops_release(&t->kops, DS_WB_KEY(t, n));
        ds_ops_release(&t->vops, DS_WB_VAL(t, n));
        DYN_DS_FREE(n);
        n = r;
    }
}

void dyn_wbtree_free(dyn_wbtree_t* t)
{
    if (!t)
        return;
    ds_wb_free_rec(t, t->root);
    DYN_DS_FREE(t);
}

size_t dyn_wbtree_size(const dyn_wbtree_t* t)
{
    return ds_wb_size(t->root);
}

void* dyn_wbtree_get(const dyn_wbtree_t* t, const void* key)
{
    ds_wbnode* n = t->root;
    while (n) {
        int c = ds_ops_cmp(&t->kops, key, DS_WB_KEY(t, n));
        if (c == 0)
            return DS_WB_VAL(t, n);
        n = c < 0 ? n->l : n->r;
    }
    return NULL;
}

static ds_wbnode* ds_wb_insert(dyn_wbtree_t* t, ds_wbnode* n, const void* key, const void* val, int* res)
{
    int c;
    if (!n) {
        ds_wbnode* z = (ds_wbnode*)DYN_DS_MALLOC(t->nodesz);
        if (!z) {
            *res = -1;
            return NULL;
        }
        z->l = z->r = NULL;
        z->size = 1;
        ds_ops_copy(&t->kops, DS_WB_KEY(t, z), key);
        ds_ops_copy(&t->vops, DS_WB_VAL(t, z), val);
        *res = 1;
        return z;
    }
    c = ds_ops_cmp(&t->kops, key, DS_WB_KEY(t, n));
    if (c == 0) {
        ds_ops_release(&t->vops, DS_WB_VAL(t, n));
        ds_ops_copy(&t->vops, DS_WB_VAL(t, n), val);
        *res = 0;
        return n;
    }
    if (c < 0) {
        ds_wbnode* l = ds_wb_insert(t, n->l, key, val, res);
        if (*res < 0)
            return n;
        n->l = l;
    } else {
        ds_wbnode* r = ds_wb_insert(t, n->r, key, val, res);
        if (*res < 0)
            return n;
        n->r = r;
    }
    return *res ? ds_wb_balance(n) : n;
}

int dyn_wbtree_put(dyn_wbtree_t* t, const void* key, const void* val)
{
    int res = 0;
    t->root = ds_wb_insert(t, t->root, key, val, &res);
    return res;
}

static ds_wbnode* ds_wb_pop_min(ds_wbnode* n, ds_wbnode** out)
{
    if (!n->l) {
        *out = n;
        return n->r;
    }
    n->l = ds_wb_pop_min(n->l, out);
    return ds_wb_balance(n);
}

static ds_wbnode* ds_wb_pop_max(ds_wbnode* n, ds_wbnode** out)
{
    if (!n->r) {
        *out = n;
        return n->l;
    }
    n->r = ds_wb_pop_max(n->r, out);
    return ds_wb_balance(n);
}

static ds_wbnode* ds_wb_glue(ds_wbnode* l, ds_wbnode* r)
{
    ds_wbnode* m;
    if (!l)
        return r;
    if (!r)
        return l;
    if (l->size > r->size) {
        l = ds_wb_pop_max(l, &m);
    } else {
        r = ds_wb_pop_min(r, &m);
    }
    m->l = l;
    m->r = r;
    return ds_wb_balance(m);
}

static ds_wbnode* ds_wb_delete(dyn_wbtree_t* t, ds_wbnode* n, const void* key, int* res)
{
    int c;
    if (!n) {
        *res = 0;
        return NULL;
    }
    c = ds_ops_cmp(&t->kops, key, DS_WB_KEY(t, n));
    if (c == 0) {
        ds_wbnode* g = ds_wb_glue(n->l, n->r);
        ds_ops_release(&t->kops, DS_WB_KEY(t, n));
        ds_ops_release(&t->vops, DS_WB_VAL(t, n));
        DYN_DS_FREE(n);
        *res = 1;
        return g;
    }
    if (c < 0)
        n->l = ds_wb_delete(t, n->l, key, res);
    else
        n->r = ds_wb_delete(t, n->r, key, res);
    return *res ? ds_wb_balance(n) : n;
}

int dyn_wbtree_remove(dyn_wbtree_t* t, const void* key)
{
    int res = 0;
    t->root = ds_wb_delete(t, t->root, key, &res);
    return res;
}

size_t dyn_wbtree_rank(const dyn_wbtree_t* t, const void* key)
{
    ds_wbnode* n = t->root;
    size_t r = 0;
    while (n) {
        int c = ds_ops_cmp(&t->kops, key, DS_WB_KEY(t, n));
        if (c <= 0) {
            if (c == 0)
                return r + ds_wb_size(n->l);
            n = n->l;
        } else {
            r += ds_wb_size(n->l) + 1;
            n = n->r;
        }
    }
    return r;
}

int dyn_wbtree_select(const dyn_wbtree_t* t, size_t i, const void** key, void** val)
{
    ds_wbnode* n = t->root;
    while (n) {
        size_t sl = ds_wb_size(n->l);
        if (i < sl) {
            n = n->l;
        } else if (i == sl) {
            if (key)
                *key = DS_WB_KEY(t, n);
            if (val)
                *val = DS_WB_VAL(t, n);
            return 0;
        } else {
            i -= sl + 1;
            n = n->r;
        }
    }
    return -1;
}

static long ds_wb_check_rec(const dyn_wbtree_t* t, const ds_wbnode* n)
{
    long sl, sr;
    if (!n)
        return 0;
    if (n->l && ds_ops_cmp(&t->kops, DS_WB_KEY(t, n->l), DS_WB_KEY(t, n)) >= 0)
        return -1;
    if (n->r && ds_ops_cmp(&t->kops, DS_WB_KEY(t, n->r), DS_WB_KEY(t, n)) <= 0)
        return -1;
    sl = ds_wb_check_rec(t, n->l);
    sr = ds_wb_check_rec(t, n->r);
    if (sl < 0 || sr < 0 || (size_t)(sl + sr + 1) != n->size)
        return -1;
    if (sl + sr > 1 && ((size_t)sr + 1 > DS_WB_DELTA * ((size_t)sl + 1) || (size_t)sl + 1 > DS_WB_DELTA * ((size_t)sr + 1)))
        return -1;
    return sl + sr + 1;
}

static int ds_wb_walk_rec(const dyn_wbtree_t* t, const ds_wbnode* n, dyn_wbtree_visit fn, void* ud)
{
    int r;
    while (n) {
        if ((r = ds_wb_walk_rec(t, n->l, fn, ud)) != 0)
            return r;
        if ((r = fn(DS_WB_KEY(t, n), DS_WB_VAL(t, n), ud)) != 0)
            return r;
        n = n->r;
    }
    return 0;
}

int dyn_wbtree_walk(const dyn_wbtree_t* t, dyn_wbtree_visit fn, void* ud)
{
    return ds_wb_walk_rec(t, t->root, fn, ud);
}

int dyn_wbtree_check(const dyn_wbtree_t* t)
{
    return ds_wb_check_rec(t, t->root) < 0 ? -1 : 0;
}

#define DS_SL_MAXLEVEL 32

typedef struct {
    struct dyn_slnode* next;
    size_t span;
} ds_sllevel;

struct dyn_slnode {
    struct dyn_slnode* back;
    unsigned level;
    ds_sllevel lv[];
};

struct dyn_skiplist {
    dyn_ds_ops kops, vops;
    dyn_slnode* head;
    unsigned level;
    size_t size;
    uint64_t rng;
};

static size_t ds_sl_node_bytes(unsigned level)
{
    return ds_align8(offsetof(dyn_slnode, lv) + sizeof(ds_sllevel) * level);
}

#define DS_SL_KEY(n) ((void*)((uint8_t*)(n) + ds_sl_node_bytes((n)->level)))
#define DS_SL_VAL(l, n) ((void*)((uint8_t*)DS_SL_KEY(n) + ds_align8((l)->kops.size)))

static dyn_slnode* ds_sl_node_new(const dyn_skiplist_t* l, unsigned level)
{
    dyn_slnode* n = (dyn_slnode*)DYN_DS_MALLOC(ds_sl_node_bytes(level) + ds_align8(l->kops.size) + ds_align8(l->vops.size));
    unsigned i;
    if (!n)
        return NULL;
    n->level = level;
    n->back = NULL;
    for (i = 0; i < level; i++) {
        n->lv[i].next = NULL;
        n->lv[i].span = 0;
    }
    return n;
}

dyn_skiplist_t* dyn_skiplist_new(const dyn_ds_ops* key, const dyn_ds_ops* val, uint64_t seed)
{
    dyn_skiplist_t* l = (dyn_skiplist_t*)DYN_DS_CALLOC(1, sizeof(*l));
    if (!l)
        return NULL;
    ds_ops_init(&l->kops, key);
    ds_ops_init(&l->vops, val);
    l->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    l->level = 1;
    l->head = ds_sl_node_new(l, DS_SL_MAXLEVEL);
    if (!l->head) {
        DYN_DS_FREE(l);
        return NULL;
    }
    return l;
}

void dyn_skiplist_free(dyn_skiplist_t* l)
{
    dyn_slnode* n;
    if (!l)
        return;
    n = l->head->lv[0].next;
    while (n) {
        dyn_slnode* nx = n->lv[0].next;
        ds_ops_release(&l->kops, DS_SL_KEY(n));
        ds_ops_release(&l->vops, DS_SL_VAL(l, n));
        DYN_DS_FREE(n);
        n = nx;
    }
    DYN_DS_FREE(l->head);
    DYN_DS_FREE(l);
}

size_t dyn_skiplist_size(const dyn_skiplist_t* l)
{
    return l->size;
}

static unsigned ds_sl_random_level(dyn_skiplist_t* l)
{
    unsigned lv = 1;
    uint64_t x = l->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    l->rng = x;
    while (lv < DS_SL_MAXLEVEL && (x & 3) == 0) {
        lv++;
        x >>= 2;
    }
    return lv;
}

static dyn_slnode* ds_sl_find(const dyn_skiplist_t* l, const void* key, dyn_slnode** update, size_t* rank)
{
    dyn_slnode* x = l->head;
    size_t r = 0;
    int i;
    for (i = (int)l->level - 1; i >= 0; i--) {
        while (x->lv[i].next && ds_ops_cmp(&l->kops, DS_SL_KEY(x->lv[i].next), key) < 0) {
            r += x->lv[i].span;
            x = x->lv[i].next;
        }
        if (update)
            update[i] = x;
        if (rank)
            rank[i] = r;
    }
    x = x->lv[0].next;
    return x && ds_ops_cmp(&l->kops, DS_SL_KEY(x), key) == 0 ? x : NULL;
}

void* dyn_skiplist_get(const dyn_skiplist_t* l, const void* key)
{
    dyn_slnode* n = ds_sl_find(l, key, NULL, NULL);
    return n ? DS_SL_VAL(l, n) : NULL;
}

int dyn_skiplist_put(dyn_skiplist_t* l, const void* key, const void* val)
{
    dyn_slnode* update[DS_SL_MAXLEVEL];
    size_t rank[DS_SL_MAXLEVEL];
    dyn_slnode *x = ds_sl_find(l, key, update, rank), *n;
    unsigned lv, i;
    if (x) {
        ds_ops_release(&l->vops, DS_SL_VAL(l, x));
        ds_ops_copy(&l->vops, DS_SL_VAL(l, x), val);
        return 0;
    }
    lv = ds_sl_random_level(l);
    if (lv > l->level) {
        for (i = l->level; i < lv; i++) {
            rank[i] = 0;
            update[i] = l->head;
            update[i]->lv[i].span = l->size;
        }
        l->level = lv;
    }
    n = ds_sl_node_new(l, lv);
    if (!n)
        return -1;
    ds_ops_copy(&l->kops, DS_SL_KEY(n), key);
    ds_ops_copy(&l->vops, DS_SL_VAL(l, n), val);
    for (i = 0; i < lv; i++) {
        n->lv[i].next = update[i]->lv[i].next;
        update[i]->lv[i].next = n;
        n->lv[i].span = update[i]->lv[i].span - (rank[0] - rank[i]);
        update[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = lv; i < l->level; i++)
        update[i]->lv[i].span++;
    n->back = update[0] == l->head ? NULL : update[0];
    if (n->lv[0].next)
        n->lv[0].next->back = n;
    l->size++;
    return 1;
}

int dyn_skiplist_remove(dyn_skiplist_t* l, const void* key)
{
    dyn_slnode* update[DS_SL_MAXLEVEL];
    dyn_slnode* x = ds_sl_find(l, key, update, NULL);
    unsigned i;
    if (!x)
        return 0;
    for (i = 0; i < l->level; i++) {
        if (update[i]->lv[i].next == x) {
            update[i]->lv[i].span += x->lv[i].span - 1;
            update[i]->lv[i].next = x->lv[i].next;
        } else {
            update[i]->lv[i].span--;
        }
    }
    if (x->lv[0].next)
        x->lv[0].next->back = x->back;
    while (l->level > 1 && !l->head->lv[l->level - 1].next)
        l->level--;
    ds_ops_release(&l->kops, DS_SL_KEY(x));
    ds_ops_release(&l->vops, DS_SL_VAL(l, x));
    DYN_DS_FREE(x);
    l->size--;
    return 1;
}

size_t dyn_skiplist_rank(const dyn_skiplist_t* l, const void* key)
{
    size_t rank[DS_SL_MAXLEVEL];
    ds_sl_find(l, key, NULL, rank);
    return rank[0];
}

dyn_slnode* dyn_skiplist_at(const dyn_skiplist_t* l, size_t idx)
{
    dyn_slnode* x = l->head;
    size_t traversed = 0, want = idx + 1;
    int i;
    if (idx >= l->size)
        return NULL;
    for (i = (int)l->level - 1; i >= 0; i--) {
        while (x->lv[i].next && traversed + x->lv[i].span <= want) {
            traversed += x->lv[i].span;
            x = x->lv[i].next;
        }
        if (traversed == want)
            return x;
    }
    return NULL;
}

dyn_slnode* dyn_skiplist_first(const dyn_skiplist_t* l)
{
    return l->head->lv[0].next;
}

dyn_slnode* dyn_skiplist_seek(const dyn_skiplist_t* l, const void* key)
{
    dyn_slnode* x = l->head;
    int i;
    for (i = (int)l->level - 1; i >= 0; i--) {
        while (x->lv[i].next && ds_ops_cmp(&l->kops, DS_SL_KEY(x->lv[i].next), key) < 0)
            x = x->lv[i].next;
    }
    return x->lv[0].next;
}

dyn_slnode* dyn_skiplist_next(const dyn_slnode* n)
{
    return n->lv[0].next;
}

const void* dyn_skiplist_key(const dyn_skiplist_t* l, const dyn_slnode* n)
{
    (void)l;
    return DS_SL_KEY((dyn_slnode*)DS_UNCONST(n));
}

void* dyn_skiplist_val(const dyn_skiplist_t* l, const dyn_slnode* n)
{
    return DS_SL_VAL(l, (dyn_slnode*)DS_UNCONST(n));
}

int dyn_skiplist_check(const dyn_skiplist_t* l)
{
    unsigned i;
    size_t count = 0;
    dyn_slnode *x, *prev = NULL;
    for (x = l->head->lv[0].next; x; x = x->lv[0].next) {
        if (prev && ds_ops_cmp(&l->kops, DS_SL_KEY(prev), DS_SL_KEY(x)) >= 0)
            return -1;
        if (x->back != prev)
            return -1;
        prev = x;
        count++;
    }
    if (count != l->size)
        return -1;
    for (i = 0; i < l->level; i++) {
        size_t pos = 0, walked = 0;
        dyn_slnode* y = l->head;
        while (y->lv[i].next) {
            dyn_slnode* z;
            size_t steps = 0;
            for (z = y; z != y->lv[i].next; z = z->lv[0].next)
                steps++;
            if (steps != y->lv[i].span)
                return -1;
            walked += steps;
            y = y->lv[i].next;
        }
        (void)pos;
        if (walked > l->size)
            return -1;
    }
    return 0;
}

#define DS_XA_SHIFT 6
#define DS_XA_FANOUT 64
#define DS_XA_MARKS 3

typedef struct ds_xanode {
    uint8_t shift;
    uint64_t present;
    uint64_t marks[DS_XA_MARKS];
    void* slot[DS_XA_FANOUT];
} ds_xanode;

struct dyn_xarray {
    dyn_ds_ops vops;
    size_t vstride;
    ds_xanode* root;
    uint8_t height;
    size_t count;
};

static uint8_t* ds_xa_leaf_vals(ds_xanode* n)
{
    return (uint8_t*)n->slot[0];
}

static ds_xanode* ds_xa_node_new(dyn_xarray_t* x, uint8_t shift)
{
    ds_xanode* n = (ds_xanode*)DYN_DS_CALLOC(1, sizeof(*n));
    if (!n)
        return NULL;
    n->shift = shift;
    if (shift == 0) {
        size_t leaf_bytes = x->vstride * DS_XA_FANOUT;
        n->slot[0] = DYN_DS_MALLOC(leaf_bytes ? leaf_bytes : 1);
        if (!n->slot[0]) {
            DYN_DS_FREE(n);
            return NULL;
        }
    }
    return n;
}

dyn_xarray_t* dyn_xarray_new(const dyn_ds_ops* val)
{
    dyn_xarray_t* x = (dyn_xarray_t*)DYN_DS_CALLOC(1, sizeof(*x));
    if (!x)
        return NULL;
    ds_ops_init(&x->vops, val);
    x->vstride = ds_align8(x->vops.size);
    return x;
}

static void ds_xa_free_rec(dyn_xarray_t* x, ds_xanode* n)
{
    unsigned i;
    if (!n)
        return;
    if (n->shift == 0) {
        uint8_t* vals = ds_xa_leaf_vals(n);
        uint64_t p = n->present;
        while (p) {
            unsigned b = (unsigned)__builtin_ctzll(p);
            ds_ops_release(&x->vops, vals + (size_t)b * x->vstride);
            p &= p - 1;
        }
        DYN_DS_FREE(vals);
    } else {
        for (i = 0; i < DS_XA_FANOUT; i++)
            ds_xa_free_rec(x, (ds_xanode*)n->slot[i]);
    }
    DYN_DS_FREE(n);
}

void dyn_xarray_free(dyn_xarray_t* x)
{
    if (!x)
        return;
    ds_xa_free_rec(x, x->root);
    DYN_DS_FREE(x);
}

size_t dyn_xarray_count(const dyn_xarray_t* x)
{
    return x->count;
}

static uint64_t ds_xa_span(uint8_t height)
{
    unsigned bits = (unsigned)height * DS_XA_SHIFT;
    return bits >= 64 ? 0 : (uint64_t)1 << bits;
}

void* dyn_xarray_load(const dyn_xarray_t* x, uint64_t index)
{
    ds_xanode* n = x->root;
    uint64_t span = ds_xa_span(x->height);
    if (!n || (span && index >= span))
        return NULL;
    while (n->shift) {
        n = (ds_xanode*)n->slot[(index >> n->shift) & (DS_XA_FANOUT - 1)];
        if (!n)
            return NULL;
    }
    if (!(n->present >> (index & (DS_XA_FANOUT - 1)) & 1))
        return NULL;
    return ds_xa_leaf_vals(n) + (size_t)(index & (DS_XA_FANOUT - 1)) * x->vstride;
}

static int ds_xa_grow(dyn_xarray_t* x, uint64_t index)
{
    if (!x->root) {
        x->root = ds_xa_node_new(x, 0);
        if (!x->root)
            return -1;
        x->height = 1;
    }
    while (ds_xa_span(x->height) && index >= ds_xa_span(x->height)) {
        ds_xanode* r = ds_xa_node_new(x, (uint8_t)(x->height * DS_XA_SHIFT));
        unsigned m;
        if (!r)
            return -1;
        r->slot[0] = x->root;
        if (x->root->present)
            r->present = 1;
        for (m = 0; m < DS_XA_MARKS; m++)
            if (x->root->marks[m])
                r->marks[m] = 1;
        x->root = r;
        x->height++;
    }
    return 0;
}

int dyn_xarray_store(dyn_xarray_t* x, uint64_t index, const void* val)
{
    ds_xanode* path[11];
    ds_xanode* n;
    unsigned depth = 0, b;
    uint8_t* v;
    if (ds_xa_grow(x, index) < 0)
        return -1;
    n = x->root;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth++] = n;
        if (!n->slot[i]) {
            n->slot[i] = ds_xa_node_new(x, (uint8_t)(n->shift - DS_XA_SHIFT));
            if (!n->slot[i])
                return -1;
        }
        n = (ds_xanode*)n->slot[i];
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    v = ds_xa_leaf_vals(n) + (size_t)b * x->vstride;
    if (n->present >> b & 1) {
        ds_ops_release(&x->vops, v);
        ds_ops_copy(&x->vops, v, val);
        return 0;
    }
    ds_ops_copy(&x->vops, v, val);
    n->present |= (uint64_t)1 << b;
    while (depth--) {
        ds_xanode* p = path[depth];
        p->present |= (uint64_t)1 << ((index >> p->shift) & (DS_XA_FANOUT - 1));
    }
    x->count++;
    return 1;
}

static void ds_xa_refresh(ds_xanode* p, unsigned i)
{
    ds_xanode* c = (ds_xanode*)p->slot[i];
    unsigned m;
    uint64_t bit = (uint64_t)1 << i;
    if (!c || !c->present)
        p->present &= ~bit;
    else
        p->present |= bit;
    for (m = 0; m < DS_XA_MARKS; m++) {
        if (c && c->marks[m])
            p->marks[m] |= bit;
        else
            p->marks[m] &= ~bit;
    }
}

int dyn_xarray_erase(dyn_xarray_t* x, uint64_t index)
{
    ds_xanode* path[11];
    unsigned idx[11];
    ds_xanode* n = x->root;
    unsigned depth = 0, b, m;
    uint64_t span = ds_xa_span(x->height);
    if (!n || (span && index >= span))
        return 0;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth] = n;
        idx[depth++] = i;
        n = (ds_xanode*)n->slot[i];
        if (!n)
            return 0;
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    if (!(n->present >> b & 1))
        return 0;
    ds_ops_release(&x->vops, ds_xa_leaf_vals(n) + (size_t)b * x->vstride);
    n->present &= ~((uint64_t)1 << b);
    for (m = 0; m < DS_XA_MARKS; m++)
        n->marks[m] &= ~((uint64_t)1 << b);
    x->count--;
    while (depth--) {
        ds_xanode* p = path[depth];
        ds_xanode* c = (ds_xanode*)p->slot[idx[depth]];
        if (c && !c->present) {
            ds_xa_free_rec(x, c);
            p->slot[idx[depth]] = NULL;
        }
        ds_xa_refresh(p, idx[depth]);
    }
    if (x->count == 0) {
        ds_xa_free_rec(x, x->root);
        x->root = NULL;
        x->height = 0;
    }
    return 1;
}

static int ds_xa_find_rec(const dyn_xarray_t* x, ds_xanode* n, uint64_t base, uint64_t from, uint64_t max, int mark, uint64_t* out)
{
    uint64_t width = (uint64_t)1 << n->shift;
    unsigned start = 0, i;
    uint64_t bits = mark < 0 ? n->present : n->marks[mark];
    if (from > base)
        start = (unsigned)((from - base) >> n->shift);
    if (start >= DS_XA_FANOUT)
        return 0;
    bits &= start ? ~(((uint64_t)1 << start) - 1) : ~(uint64_t)0;
    while (bits) {
        uint64_t child_base;
        i = (unsigned)__builtin_ctzll(bits);
        child_base = base + (uint64_t)i * width;
        if (child_base > max)
            return 0;
        if (n->shift == 0) {
            *out = child_base;
            return 1;
        }
        if (ds_xa_find_rec(x, (ds_xanode*)n->slot[i], child_base, from > child_base ? from : child_base, max, mark, out))
            return 1;
        bits &= bits - 1;
    }
    return 0;
}

static int ds_xa_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, int mark, void** val)
{
    uint64_t span = ds_xa_span(x->height), found;
    if (!x->root || (span && *index >= span) || *index > max)
        return 0;
    if (!ds_xa_find_rec(x, x->root, 0, *index, max, mark, &found))
        return 0;
    *index = found;
    if (val)
        *val = dyn_xarray_load(x, found);
    return 1;
}

int dyn_xarray_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, void** val)
{
    return ds_xa_find(x, index, max, -1, val);
}

int dyn_xarray_find_marked(const dyn_xarray_t* x, uint64_t* index, uint64_t max, unsigned mark, void** val)
{
    if (mark >= DS_XA_MARKS)
        return 0;
    return ds_xa_find(x, index, max, (int)mark, val);
}

int dyn_xarray_set_mark(dyn_xarray_t* x, uint64_t index, unsigned mark, int on)
{
    ds_xanode* path[11];
    unsigned idx[11];
    ds_xanode* n = x->root;
    unsigned depth = 0, b;
    uint64_t span = ds_xa_span(x->height);
    if (mark >= DS_XA_MARKS || !n || (span && index >= span))
        return -1;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth] = n;
        idx[depth++] = i;
        n = (ds_xanode*)n->slot[i];
        if (!n)
            return -1;
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    if (!(n->present >> b & 1))
        return -1;
    if (on)
        n->marks[mark] |= (uint64_t)1 << b;
    else
        n->marks[mark] &= ~((uint64_t)1 << b);
    while (depth--)
        ds_xa_refresh(path[depth], idx[depth]);
    return 0;
}

int dyn_xarray_get_mark(const dyn_xarray_t* x, uint64_t index, unsigned mark)
{
    ds_xanode* n = x->root;
    uint64_t span = ds_xa_span(x->height);
    if (mark >= DS_XA_MARKS || !n || (span && index >= span))
        return 0;
    while (n->shift) {
        n = (ds_xanode*)n->slot[(index >> n->shift) & (DS_XA_FANOUT - 1)];
        if (!n)
            return 0;
    }
    return (int)(n->marks[mark] >> (index & (DS_XA_FANOUT - 1)) & 1);
}

#define DS_SA_EMPTY UINT32_MAX

static int ds_sa_type_s(const uint8_t* t, uint32_t i)
{
    return (t[i >> 3] >> (i & 7)) & 1;
}

static void ds_sa_set_type(uint8_t* t, uint32_t i, int s)
{
    if (s)
        t[i >> 3] |= (uint8_t)(1u << (i & 7));
    else
        t[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static int ds_sa_is_lms(const uint8_t* t, uint32_t i)
{
    return i > 0 && ds_sa_type_s(t, i) && !ds_sa_type_s(t, i - 1);
}

static void ds_sa_buckets(const uint32_t* s, uint32_t n, uint32_t k, uint32_t* bkt, int end)
{
    uint32_t i, sum = 0;
    memset(bkt, 0, sizeof(uint32_t) * k);
    for (i = 0; i < n; i++)
        bkt[s[i]]++;
    for (i = 0; i < k; i++) {
        sum += bkt[i];
        bkt[i] = end ? sum : sum - bkt[i];
    }
}

static void ds_sa_induce(const uint32_t* s, uint32_t* sa, const uint8_t* t, uint32_t n, uint32_t k, uint32_t* bkt)
{
    uint32_t i;
    ds_sa_buckets(s, n, k, bkt, 0);
    for (i = 0; i < n; i++) {
        uint32_t j = sa[i];
        if (j != DS_SA_EMPTY && j > 0 && !ds_sa_type_s(t, j - 1))
            sa[bkt[s[j - 1]]++] = j - 1;
    }
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = n; i-- > 0;) {
        uint32_t j = sa[i];
        if (j != DS_SA_EMPTY && j > 0 && ds_sa_type_s(t, j - 1))
            sa[--bkt[s[j - 1]]] = j - 1;
    }
}

static int ds_sa_is(const uint32_t* s, uint32_t* sa, uint32_t n, uint32_t k)
{
    uint8_t* t = (uint8_t*)DYN_DS_CALLOC((n >> 3) + 1, 1);
    uint32_t* bkt = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (k ? k : 1));
    uint32_t i, j, n1 = 0, name = 0, prev = DS_SA_EMPTY;
    uint32_t *s1 = NULL, *sa1 = NULL, *names = NULL;
    int rc = -1;
    if (!t || !bkt)
        goto out;
    if (n == 1) {
        sa[0] = 0;
        rc = 0;
        goto out;
    }
    ds_sa_set_type(t, n - 1, 1);
    for (i = n - 1; i-- > 0;)
        ds_sa_set_type(t, i, s[i] < s[i + 1] || (s[i] == s[i + 1] && ds_sa_type_s(t, i + 1)));
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = 0; i < n; i++)
        sa[i] = DS_SA_EMPTY;
    for (i = 1; i < n; i++)
        if (ds_sa_is_lms(t, i))
            sa[--bkt[s[i]]] = i;
    ds_sa_induce(s, sa, t, n, k, bkt);
    for (i = 0; i < n; i++)
        if (sa[i] != DS_SA_EMPTY && ds_sa_is_lms(t, sa[i]))
            sa[n1++] = sa[i];
    names = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * n);
    if (!names)
        goto out;
    for (i = 0; i < n; i++)
        names[i] = DS_SA_EMPTY;
    for (i = 0; i < n1; i++) {
        uint32_t pos = sa[i];
        int diff = 0;
        if (prev == DS_SA_EMPTY) {
            diff = 1;
        } else {
            uint32_t d;
            for (d = 0;; d++) {
                if (pos + d >= n || prev + d >= n || s[pos + d] != s[prev + d] || ds_sa_type_s(t, pos + d) != ds_sa_type_s(t, prev + d)) {
                    diff = 1;
                    break;
                }
                if (d > 0 && ds_sa_is_lms(t, pos + d))
                    break;
            }
        }
        if (diff) {
            name++;
            prev = pos;
        }
        names[pos] = name - 1;
    }
    s1 = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n1 ? n1 : 1));
    sa1 = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n1 ? n1 : 1));
    if (!s1 || !sa1)
        goto out;
    for (i = 0, j = 0; i < n; i++)
        if (names[i] != DS_SA_EMPTY)
            s1[j++] = names[i];
    if (name < n1) {
        if (ds_sa_is(s1, sa1, n1, name) < 0)
            goto out;
    } else {
        for (i = 0; i < n1; i++)
            sa1[s1[i]] = i;
    }
    for (i = 1, j = 0; i < n; i++)
        if (ds_sa_is_lms(t, i))
            s1[j++] = i;
    for (i = 0; i < n1; i++)
        sa1[i] = s1[sa1[i]];
    for (i = 0; i < n; i++)
        sa[i] = DS_SA_EMPTY;
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = n1; i-- > 0;) {
        j = sa1[i];
        sa[--bkt[s[j]]] = j;
    }
    ds_sa_induce(s, sa, t, n, k, bkt);
    rc = 0;
out:
    DYN_DS_FREE(t);
    DYN_DS_FREE(bkt);
    DYN_DS_FREE(names);
    DYN_DS_FREE(s1);
    DYN_DS_FREE(sa1);
    return rc;
}

struct dyn_sa {
    uint8_t* text;
    uint32_t n;
    uint32_t* sa;
    uint32_t* lcp;
};

dyn_sa_t* dyn_sa_build(const uint8_t* text, size_t n)
{
    dyn_sa_t* r;
    uint32_t *s, *tmp, i;
    if (n >= UINT32_MAX - 1)
        return NULL;
    r = (dyn_sa_t*)DYN_DS_CALLOC(1, sizeof(*r));
    if (!r)
        return NULL;
    r->n = (uint32_t)n;
    r->text = (uint8_t*)DYN_DS_MALLOC(n ? n : 1);
    r->sa = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n ? n : 1));
    s = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n + 1));
    tmp = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n + 1));
    if (!r->text || !r->sa || !s || !tmp)
        goto fail;
    if (n)
        memcpy(r->text, text, n);
    for (i = 0; i < n; i++)
        s[i] = (uint32_t)text[i] + 1;
    s[n] = 0;
    if (ds_sa_is(s, tmp, (uint32_t)n + 1, 257) < 0)
        goto fail;
    if (n)
        memcpy(r->sa, tmp + 1, sizeof(uint32_t) * n);
    DYN_DS_FREE(s);
    DYN_DS_FREE(tmp);
    return r;
fail:
    DYN_DS_FREE(s);
    DYN_DS_FREE(tmp);
    dyn_sa_free(r);
    return NULL;
}

void dyn_sa_free(dyn_sa_t* sa)
{
    if (!sa)
        return;
    DYN_DS_FREE(sa->text);
    DYN_DS_FREE(sa->sa);
    DYN_DS_FREE(sa->lcp);
    DYN_DS_FREE(sa);
}

size_t dyn_sa_len(const dyn_sa_t* sa)
{
    return sa->n;
}

const uint32_t* dyn_sa_array(const dyn_sa_t* sa)
{
    return sa->sa;
}

const uint8_t* dyn_sa_text(const dyn_sa_t* sa)
{
    return sa->text;
}

const uint32_t* dyn_sa_lcp(dyn_sa_t* sa)
{
    uint32_t *rank, i, h = 0;
    if (sa->lcp)
        return sa->lcp;
    sa->lcp = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (sa->n ? sa->n : 1));
    rank = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (sa->n ? sa->n : 1));
    if (!sa->lcp || !rank) {
        DYN_DS_FREE(sa->lcp);
        DYN_DS_FREE(rank);
        sa->lcp = NULL;
        return NULL;
    }
    for (i = 0; i < sa->n; i++)
        rank[sa->sa[i]] = i;
    for (i = 0; i < sa->n; i++) {
        if (rank[i] == 0) {
            sa->lcp[0] = 0;
            h = 0;
            continue;
        }
        {
            uint32_t j = sa->sa[rank[i] - 1];
            while (i + h < sa->n && j + h < sa->n && sa->text[i + h] == sa->text[j + h])
                h++;
            sa->lcp[rank[i]] = h;
            if (h)
                h--;
        }
    }
    DYN_DS_FREE(rank);
    return sa->lcp;
}

static int ds_sa_cmp_prefix(const dyn_sa_t* sa, uint32_t pos, const uint8_t* pat, size_t m)
{
    size_t avail = sa->n - pos, len = avail < m ? avail : m;
    int c = len ? memcmp(sa->text + pos, pat, len) : 0;
    if (c)
        return c;
    return avail < m ? -1 : 0;
}

size_t dyn_sa_find(const dyn_sa_t* sa, const uint8_t* pat, size_t m, size_t* first)
{
    size_t lo = 0, hi = sa->n, start;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ds_sa_cmp_prefix(sa, sa->sa[mid], pat, m) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    start = lo;
    hi = sa->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ds_sa_cmp_prefix(sa, sa->sa[mid], pat, m) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (first)
        *first = start;
    return lo - start;
}

size_t dyn_sa_longest_repeat(dyn_sa_t* sa, size_t* pos)
{
    const uint32_t* lcp = dyn_sa_lcp(sa);
    uint32_t i, best = 0, at = 0;
    if (!lcp)
        return 0;
    for (i = 1; i < sa->n; i++) {
        if (lcp[i] > best) {
            best = lcp[i];
            at = sa->sa[i];
        }
    }
    if (pos)
        *pos = at;
    return best;
}

typedef struct ds_ptnode {
    struct ds_ptnode* c[2];
    uint8_t* key;
    uint32_t bits;
    uint32_t has_val;
} ds_ptnode;

struct dyn_patricia {
    dyn_ds_ops vops;
    size_t voff;
    ds_ptnode* root;
    size_t size;
};

#define DS_PT_VAL(t, n) ((void*)((uint8_t*)(n) + (t)->voff))

static int ds_pt_bit(const uint8_t* key, uint32_t i)
{
    return (key[i >> 3] >> (7 - (i & 7))) & 1;
}

static uint32_t ds_pt_common(const uint8_t* a, uint32_t abits, const uint8_t* b, uint32_t bbits)
{
    uint32_t lim = abits < bbits ? abits : bbits, i = 0;
    while (i + 8 <= lim && a[i >> 3] == b[i >> 3])
        i += 8;
    while (i < lim && ds_pt_bit(a, i) == ds_pt_bit(b, i))
        i++;
    return i;
}

static ds_ptnode* ds_pt_node(dyn_patricia_t* t, const uint8_t* key, uint32_t bits)
{
    size_t kb = (bits + 7) / 8;
    ds_ptnode* n = (ds_ptnode*)DYN_DS_CALLOC(1, t->voff + ds_align8(t->vops.size));
    if (!n)
        return NULL;
    n->key = (uint8_t*)DYN_DS_MALLOC(kb ? kb : 1);
    if (!n->key) {
        DYN_DS_FREE(n);
        return NULL;
    }
    if (kb) {
        memcpy(n->key, key, kb);
        if (bits & 7)
            n->key[kb - 1] &= (uint8_t)(0xFF << (8 - (bits & 7)));
    }
    n->bits = bits;
    return n;
}

static void ds_pt_node_free(dyn_patricia_t* t, ds_ptnode* n)
{
    if (n->has_val)
        ds_ops_release(&t->vops, DS_PT_VAL(t, n));
    DYN_DS_FREE(n->key);
    DYN_DS_FREE(n);
}

dyn_patricia_t* dyn_patricia_new(const dyn_ds_ops* val)
{
    dyn_patricia_t* t = (dyn_patricia_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    ds_ops_init(&t->vops, val);
    t->voff = ds_align8(sizeof(ds_ptnode));
    return t;
}

static void ds_pt_free_rec(dyn_patricia_t* t, ds_ptnode* n)
{
    if (!n)
        return;
    ds_pt_free_rec(t, n->c[0]);
    ds_pt_free_rec(t, n->c[1]);
    ds_pt_node_free(t, n);
}

void dyn_patricia_free(dyn_patricia_t* t)
{
    if (!t)
        return;
    ds_pt_free_rec(t, t->root);
    DYN_DS_FREE(t);
}

size_t dyn_patricia_size(const dyn_patricia_t* t)
{
    return t->size;
}

int dyn_patricia_put(dyn_patricia_t* t, const uint8_t* key, uint32_t bits, const void* val)
{
    ds_ptnode **link = &t->root, *n;
    for (;;) {
        uint32_t cpl;
        n = *link;
        if (!n) {
            n = ds_pt_node(t, key, bits);
            if (!n)
                return -1;
            ds_ops_copy(&t->vops, DS_PT_VAL(t, n), val);
            n->has_val = 1;
            *link = n;
            t->size++;
            return 1;
        }
        cpl = ds_pt_common(n->key, n->bits, key, bits);
        if (cpl < n->bits) {
            ds_ptnode* mid = ds_pt_node(t, key, cpl);
            if (!mid)
                return -1;
            mid->c[ds_pt_bit(n->key, cpl)] = n;
            if (cpl == bits) {
                ds_ops_copy(&t->vops, DS_PT_VAL(t, mid), val);
                mid->has_val = 1;
            } else {
                ds_ptnode* leaf = ds_pt_node(t, key, bits);
                if (!leaf) {
                    DYN_DS_FREE(mid->key);
                    DYN_DS_FREE(mid);
                    return -1;
                }
                ds_ops_copy(&t->vops, DS_PT_VAL(t, leaf), val);
                leaf->has_val = 1;
                mid->c[ds_pt_bit(key, cpl)] = leaf;
            }
            *link = mid;
            t->size++;
            return 1;
        }
        if (bits == n->bits) {
            if (n->has_val) {
                ds_ops_release(&t->vops, DS_PT_VAL(t, n));
                ds_ops_copy(&t->vops, DS_PT_VAL(t, n), val);
                return 0;
            }
            ds_ops_copy(&t->vops, DS_PT_VAL(t, n), val);
            n->has_val = 1;
            t->size++;
            return 1;
        }
        link = &n->c[ds_pt_bit(key, n->bits)];
    }
}

void* dyn_patricia_get(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits)
{
    ds_ptnode* n = t->root;
    while (n) {
        uint32_t cpl = ds_pt_common(n->key, n->bits, key, bits);
        if (cpl < n->bits)
            return NULL;
        if (n->bits == bits)
            return n->has_val ? DS_PT_VAL(t, n) : NULL;
        n = n->c[ds_pt_bit(key, n->bits)];
    }
    return NULL;
}

void* dyn_patricia_longest_prefix(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits, uint32_t* matched_bits)
{
    ds_ptnode *n = t->root, *best = NULL;
    while (n) {
        uint32_t cpl = ds_pt_common(n->key, n->bits, key, bits);
        if (cpl < n->bits)
            break;
        if (n->has_val)
            best = n;
        if (n->bits == bits)
            break;
        n = n->c[ds_pt_bit(key, n->bits)];
    }
    if (!best)
        return NULL;
    if (matched_bits)
        *matched_bits = best->bits;
    return DS_PT_VAL(t, best);
}

static ds_ptnode* ds_pt_remove_rec(dyn_patricia_t* t, ds_ptnode* n, const uint8_t* key, uint32_t bits, int* removed)
{
    uint32_t cpl;
    if (!n)
        return NULL;
    cpl = ds_pt_common(n->key, n->bits, key, bits);
    if (cpl < n->bits)
        return n;
    if (n->bits == bits) {
        if (!n->has_val)
            return n;
        ds_ops_release(&t->vops, DS_PT_VAL(t, n));
        n->has_val = 0;
        *removed = 1;
    } else {
        int b = ds_pt_bit(key, n->bits);
        n->c[b] = ds_pt_remove_rec(t, n->c[b], key, bits, removed);
        if (!*removed)
            return n;
    }
    if (n->has_val)
        return n;
    if (!n->c[0] && !n->c[1]) {
        ds_pt_node_free(t, n);
        return NULL;
    }
    if (!n->c[0] || !n->c[1]) {
        ds_ptnode* child = n->c[0] ? n->c[0] : n->c[1];
        ds_pt_node_free(t, n);
        return child;
    }
    return n;
}

int dyn_patricia_remove(dyn_patricia_t* t, const uint8_t* key, uint32_t bits)
{
    int removed = 0;
    t->root = ds_pt_remove_rec(t, t->root, key, bits, &removed);
    if (removed)
        t->size--;
    return removed;
}

static int ds_pt_walk_rec(const dyn_patricia_t* t, ds_ptnode* n, dyn_patricia_visit fn, void* ud)
{
    int r;
    if (!n)
        return 0;
    if (n->has_val && (r = fn(n->key, n->bits, DS_PT_VAL(t, n), ud)) != 0)
        return r;
    if ((r = ds_pt_walk_rec(t, n->c[0], fn, ud)) != 0)
        return r;
    return ds_pt_walk_rec(t, n->c[1], fn, ud);
}

int dyn_patricia_walk(const dyn_patricia_t* t, const uint8_t* prefix, uint32_t bits, dyn_patricia_visit fn, void* ud)
{
    ds_ptnode* n = t->root;
    while (n) {
        uint32_t cpl = ds_pt_common(n->key, n->bits, prefix, bits);
        if (cpl == bits)
            return ds_pt_walk_rec(t, n, fn, ud);
        if (cpl < n->bits)
            return 0;
        n = n->c[ds_pt_bit(prefix, n->bits)];
    }
    return 0;
}

struct dyn_csr {
    uint32_t n;
    size_t m;
    uint32_t* row;
    uint32_t* col;
    double* w;
    int directed;
};

dyn_csr_t* dyn_csr_build(uint32_t nodes, const dyn_csr_edge* edges, size_t nedges, int directed)
{
    dyn_csr_t* g;
    uint32_t* cur = NULL;
    size_t i, m;
    if (nodes == UINT32_MAX)
        return NULL;
    for (i = 0; i < nedges; i++)
        if (edges[i].src >= nodes || edges[i].dst >= nodes || edges[i].w != edges[i].w)
            return NULL;
    m = directed ? nedges : nedges * 2;
    if (m > UINT32_MAX)
        return NULL;
    g = (dyn_csr_t*)DYN_DS_CALLOC(1, sizeof(*g));
    if (!g)
        return NULL;
    g->n = nodes;
    g->m = m;
    g->directed = directed;
    g->row = (uint32_t*)DYN_DS_CALLOC((size_t)nodes + 1, sizeof(uint32_t));
    g->col = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (m ? m : 1));
    g->w = (double*)DYN_DS_MALLOC(sizeof(double) * (m ? m : 1));
    cur = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)nodes + 1));
    if (!g->row || !g->col || !g->w || !cur)
        goto fail;
    for (i = 0; i < nedges; i++) {
        g->row[edges[i].src + 1]++;
        if (!directed)
            g->row[edges[i].dst + 1]++;
    }
    for (i = 0; i < nodes; i++)
        g->row[i + 1] += g->row[i];
    memcpy(cur, g->row, sizeof(uint32_t) * ((size_t)nodes + 1));
    for (i = 0; i < nedges; i++) {
        uint32_t p = cur[edges[i].src]++;
        g->col[p] = edges[i].dst;
        g->w[p] = edges[i].w;
        if (!directed) {
            p = cur[edges[i].dst]++;
            g->col[p] = edges[i].src;
            g->w[p] = edges[i].w;
        }
    }
    DYN_DS_FREE(cur);
    return g;
fail:
    DYN_DS_FREE(cur);
    dyn_csr_free(g);
    return NULL;
}

void dyn_csr_free(dyn_csr_t* g)
{
    if (!g)
        return;
    DYN_DS_FREE(g->row);
    DYN_DS_FREE(g->col);
    DYN_DS_FREE(g->w);
    DYN_DS_FREE(g);
}

uint32_t dyn_csr_nodes(const dyn_csr_t* g)
{
    return g->n;
}

size_t dyn_csr_edges(const dyn_csr_t* g)
{
    return g->m;
}

uint32_t dyn_csr_degree(const dyn_csr_t* g, uint32_t v)
{
    return v < g->n ? g->row[v + 1] - g->row[v] : 0;
}

const uint32_t* dyn_csr_neighbors(const dyn_csr_t* g, uint32_t v, const double** w, uint32_t* count)
{
    if (v >= g->n) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = g->row[v + 1] - g->row[v];
    if (w)
        *w = g->w + g->row[v];
    return g->col + g->row[v];
}

int dyn_csr_bfs(const dyn_csr_t* g, uint32_t src, int32_t* dist)
{
    uint32_t *q, head = 0, tail = 0, i;
    if (src >= g->n)
        return -1;
    q = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (g->n ? g->n : 1));
    if (!q)
        return -1;
    for (i = 0; i < g->n; i++)
        dist[i] = -1;
    dist[src] = 0;
    q[tail++] = src;
    while (head < tail) {
        uint32_t u = q[head++], e;
        for (e = g->row[u]; e < g->row[u + 1]; e++) {
            uint32_t v = g->col[e];
            if (dist[v] < 0) {
                dist[v] = dist[u] + 1;
                q[tail++] = v;
            }
        }
    }
    DYN_DS_FREE(q);
    return 0;
}

typedef struct {
    double d;
    uint32_t v;
} ds_heap_item;

static void ds_heap_push(ds_heap_item* h, size_t* n, ds_heap_item it)
{
    size_t i = (*n)++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h[p].d <= it.d)
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = it;
}

static ds_heap_item ds_heap_pop(ds_heap_item* h, size_t* n)
{
    ds_heap_item top = h[0], last = h[--(*n)];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= *n)
            break;
        if (c + 1 < *n && h[c + 1].d < h[c].d)
            c++;
        if (last.d <= h[c].d)
            break;
        h[i] = h[c];
        i = c;
    }
    if (*n)
        h[i] = last;
    return top;
}

int dyn_csr_dijkstra(const dyn_csr_t* g, uint32_t src, double* dist, int32_t* pred)
{
    ds_heap_item* h;
    size_t hn = 0, i;
    if (src >= g->n)
        return -1;
    for (i = 0; i < g->m; i++)
        if (g->w[i] < 0)
            return -1;
    h = (ds_heap_item*)DYN_DS_MALLOC(sizeof(ds_heap_item) * (g->m + 1));
    if (!h)
        return -1;
    for (i = 0; i < g->n; i++) {
        dist[i] = (double)INFINITY;
        if (pred)
            pred[i] = -1;
    }
    dist[src] = 0;
    ds_heap_push(h, &hn, (ds_heap_item) { 0, src });
    while (hn) {
        ds_heap_item it = ds_heap_pop(h, &hn);
        uint32_t e;
        if (it.d > dist[it.v])
            continue;
        for (e = g->row[it.v]; e < g->row[it.v + 1]; e++) {
            uint32_t v = g->col[e];
            double nd = it.d + g->w[e];
            if (nd < dist[v]) {
                dist[v] = nd;
                if (pred)
                    pred[v] = (int32_t)it.v;
                ds_heap_push(h, &hn, (ds_heap_item) { nd, v });
            }
        }
    }
    DYN_DS_FREE(h);
    return 0;
}

int dyn_csr_pagerank(const dyn_csr_t* g, double damping, uint32_t iters, double tol, double* rank)
{
    double *next, inv;
    uint32_t it, i;
    if (g->n == 0)
        return 0;
    if (!(damping >= 0 && damping <= 1))
        return -1;
    next = (double*)DYN_DS_MALLOC(sizeof(double) * g->n);
    if (!next)
        return -1;
    inv = 1.0 / g->n;
    for (i = 0; i < g->n; i++)
        rank[i] = inv;
    for (it = 0; it < iters; it++) {
        double dangling = 0, diff = 0;
        for (i = 0; i < g->n; i++)
            next[i] = 0;
        for (i = 0; i < g->n; i++) {
            uint32_t deg = g->row[i + 1] - g->row[i], e;
            if (!deg) {
                dangling += rank[i];
                continue;
            }
            for (e = g->row[i]; e < g->row[i + 1]; e++)
                next[g->col[e]] += rank[i] / deg;
        }
        for (i = 0; i < g->n; i++) {
            double r = (1 - damping) * inv + damping * (next[i] + dangling * inv);
            diff += fabs(r - rank[i]);
            rank[i] = r;
        }
        if (diff < tol)
            break;
    }
    DYN_DS_FREE(next);
    return (int)it;
}

static uint32_t ds_uf_find(uint32_t* p, uint32_t x)
{
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

uint32_t dyn_csr_components(const dyn_csr_t* g, uint32_t* comp)
{
    uint32_t *p = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (g->n ? g->n : 1)), i, k = 0, e;
    if (!p)
        return UINT32_MAX;
    for (i = 0; i < g->n; i++)
        p[i] = i;
    for (i = 0; i < g->n; i++)
        for (e = g->row[i]; e < g->row[i + 1]; e++) {
            uint32_t a = ds_uf_find(p, i), b = ds_uf_find(p, g->col[e]);
            if (a != b)
                p[a < b ? b : a] = a < b ? a : b;
        }
    for (i = 0; i < g->n; i++)
        comp[i] = UINT32_MAX;
    for (i = 0; i < g->n; i++) {
        uint32_t r = ds_uf_find(p, i);
        if (comp[r] == UINT32_MAX)
            comp[r] = k++;
        comp[i] = comp[r];
    }
    DYN_DS_FREE(p);
    return k;
}

#define DS_RS_M 16
#define DS_RS_MIN 6
#define DS_RS_REINSERT 5

typedef struct ds_rsnode ds_rsnode;
typedef struct {
    double b[2 * DYN_RSTAR_MAX_DIMS];
    union {
        ds_rsnode* child;
        uint64_t id;
    } u;
} ds_rsentry;

struct ds_rsnode {
    unsigned level;
    unsigned count;
    ds_rsentry e[DS_RS_M + 1];
};

struct dyn_rstar {
    unsigned dims;
    ds_rsnode* root;
    size_t size;
};

static void ds_rs_box_of(const dyn_rstar_t* t, const ds_rsnode* n, double* b)
{
    unsigned i, d;
    for (d = 0; d < t->dims; d++) {
        b[d] = (double)INFINITY;
        b[t->dims + d] = -(double)INFINITY;
    }
    for (i = 0; i < n->count; i++)
        for (d = 0; d < t->dims; d++) {
            if (n->e[i].b[d] < b[d])
                b[d] = n->e[i].b[d];
            if (n->e[i].b[t->dims + d] > b[t->dims + d])
                b[t->dims + d] = n->e[i].b[t->dims + d];
        }
}

static double ds_rs_area(const dyn_rstar_t* t, const double* b)
{
    double a = 1;
    unsigned d;
    for (d = 0; d < t->dims; d++)
        a *= b[t->dims + d] - b[d];
    return a;
}

static double ds_rs_margin(const dyn_rstar_t* t, const double* b)
{
    double m = 0;
    unsigned d;
    for (d = 0; d < t->dims; d++)
        m += b[t->dims + d] - b[d];
    return m;
}

static double ds_rs_overlap(const dyn_rstar_t* t, const double* a, const double* b)
{
    double o = 1;
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        double lo = a[d] > b[d] ? a[d] : b[d];
        double hi = a[t->dims + d] < b[t->dims + d] ? a[t->dims + d] : b[t->dims + d];
        if (hi <= lo)
            return 0;
        o *= hi - lo;
    }
    return o;
}

static void ds_rs_union(const dyn_rstar_t* t, double* dst, const double* a, const double* b)
{
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        dst[d] = a[d] < b[d] ? a[d] : b[d];
        dst[t->dims + d] = a[t->dims + d] > b[t->dims + d] ? a[t->dims + d] : b[t->dims + d];
    }
}

static int ds_rs_intersects(const dyn_rstar_t* t, const double* a, const double* b)
{
    unsigned d;
    for (d = 0; d < t->dims; d++)
        if (a[t->dims + d] < b[d] || b[t->dims + d] < a[d])
            return 0;
    return 1;
}

static int ds_rs_contains(const dyn_rstar_t* t, const double* outer, const double* inner)
{
    unsigned d;
    for (d = 0; d < t->dims; d++)
        if (inner[d] < outer[d] || inner[t->dims + d] > outer[t->dims + d])
            return 0;
    return 1;
}

dyn_rstar_t* dyn_rstar_new(unsigned dims)
{
    dyn_rstar_t* t;
    if (dims < 1 || dims > DYN_RSTAR_MAX_DIMS)
        return NULL;
    t = (dyn_rstar_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    t->dims = dims;
    t->root = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
    if (!t->root) {
        DYN_DS_FREE(t);
        return NULL;
    }
    return t;
}

static void ds_rs_free_rec(ds_rsnode* n)
{
    unsigned i;
    if (n->level)
        for (i = 0; i < n->count; i++)
            ds_rs_free_rec(n->e[i].u.child);
    DYN_DS_FREE(n);
}

void dyn_rstar_free(dyn_rstar_t* t)
{
    if (!t)
        return;
    ds_rs_free_rec(t->root);
    DYN_DS_FREE(t);
}

size_t dyn_rstar_size(const dyn_rstar_t* t)
{
    return t->size;
}

unsigned dyn_rstar_height(const dyn_rstar_t* t)
{
    return t->root->level + 1;
}

static unsigned ds_rs_choose(const dyn_rstar_t* t, const ds_rsnode* n, const double* b)
{
    unsigned i, j, best = 0;
    double bu[2 * DYN_RSTAR_MAX_DIMS];
    if (n->level == 1) {
        double best_ov = (double)INFINITY, best_ae = (double)INFINITY, best_a = (double)INFINITY;
        for (i = 0; i < n->count; i++) {
            double ov = 0, a = ds_rs_area(t, n->e[i].b), ae;
            ds_rs_union(t, bu, n->e[i].b, b);
            ae = ds_rs_area(t, bu) - a;
            for (j = 0; j < n->count; j++)
                if (j != i)
                    ov += ds_rs_overlap(t, bu, n->e[j].b) - ds_rs_overlap(t, n->e[i].b, n->e[j].b);
            if (ov < best_ov || (ov == best_ov && (ae < best_ae || (ae == best_ae && a < best_a)))) {
                best_ov = ov;
                best_ae = ae;
                best_a = a;
                best = i;
            }
        }
    } else {
        double best_ae = (double)INFINITY, best_a = (double)INFINITY;
        for (i = 0; i < n->count; i++) {
            double a = ds_rs_area(t, n->e[i].b), ae;
            ds_rs_union(t, bu, n->e[i].b, b);
            ae = ds_rs_area(t, bu) - a;
            if (ae < best_ae || (ae == best_ae && a < best_a)) {
                best_ae = ae;
                best_a = a;
                best = i;
            }
        }
    }
    return best;
}

typedef struct {
    const dyn_rstar_t* t;
    unsigned axis;
    int by_hi;
} ds_rs_sortctx;

static void ds_rs_sort(const dyn_rstar_t* t, ds_rsentry* e, unsigned n, unsigned axis, int by_hi)
{
    unsigned i, j;
    unsigned col = by_hi ? t->dims + axis : axis;
    for (i = 1; i < n; i++) {
        ds_rsentry x = e[i];
        double k = x.b[col];
        j = i;
        while (j > 0 && e[j - 1].b[col] > k) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = x;
    }
}

static void ds_rs_group_box(const dyn_rstar_t* t, const ds_rsentry* e, unsigned from, unsigned to, double* b)
{
    unsigned i, d;
    for (d = 0; d < t->dims; d++) {
        b[d] = (double)INFINITY;
        b[t->dims + d] = -(double)INFINITY;
    }
    for (i = from; i < to; i++)
        for (d = 0; d < t->dims; d++) {
            if (e[i].b[d] < b[d])
                b[d] = e[i].b[d];
            if (e[i].b[t->dims + d] > b[t->dims + d])
                b[t->dims + d] = e[i].b[t->dims + d];
        }
}

static ds_rsnode* ds_rs_split(dyn_rstar_t* t, ds_rsnode* n)
{
    unsigned total = n->count, axis, best_axis = 0, k, best_k = DS_RS_MIN;
    int best_hi = 0, hi;
    double best_margin = (double)INFINITY, best_ov = (double)INFINITY, best_area = (double)INFINITY;
    double b1[2 * DYN_RSTAR_MAX_DIMS], b2[2 * DYN_RSTAR_MAX_DIMS];
    ds_rsnode* nn;
    for (axis = 0; axis < t->dims; axis++) {
        double margin = 0;
        for (hi = 0; hi < 2; hi++) {
            ds_rs_sort(t, n->e, total, axis, hi);
            for (k = DS_RS_MIN; k <= total - DS_RS_MIN; k++) {
                ds_rs_group_box(t, n->e, 0, k, b1);
                ds_rs_group_box(t, n->e, k, total, b2);
                margin += ds_rs_margin(t, b1) + ds_rs_margin(t, b2);
            }
        }
        if (margin < best_margin) {
            best_margin = margin;
            best_axis = axis;
        }
    }
    for (hi = 0; hi < 2; hi++) {
        ds_rs_sort(t, n->e, total, best_axis, hi);
        for (k = DS_RS_MIN; k <= total - DS_RS_MIN; k++) {
            double ov, area;
            ds_rs_group_box(t, n->e, 0, k, b1);
            ds_rs_group_box(t, n->e, k, total, b2);
            ov = ds_rs_overlap(t, b1, b2);
            area = ds_rs_area(t, b1) + ds_rs_area(t, b2);
            if (ov < best_ov || (ov == best_ov && area < best_area)) {
                best_ov = ov;
                best_area = area;
                best_k = k;
                best_hi = hi;
            }
        }
    }
    ds_rs_sort(t, n->e, total, best_axis, best_hi);
    nn = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
    if (!nn)
        return NULL;
    nn->level = n->level;
    nn->count = total - best_k;
    memcpy(nn->e, n->e + best_k, sizeof(ds_rsentry) * nn->count);
    n->count = best_k;
    return nn;
}

static int ds_rs_insert_at(dyn_rstar_t* t, const ds_rsentry* in, unsigned level, uint8_t* reinserted);

static int ds_rs_overflow(dyn_rstar_t* t, ds_rsnode** path, unsigned* idx, unsigned depth, uint8_t* reinserted)
{
    for (;;) {
        ds_rsnode* n = path[depth];
        if (n->count <= DS_RS_M)
            break;
        if (n != t->root && !reinserted[n->level]) {
            double center[DYN_RSTAR_MAX_DIMS], nb[2 * DYN_RSTAR_MAX_DIMS], dist[DS_RS_M + 1];
            ds_rsentry out[DS_RS_REINSERT];
            unsigned i, j, d, level = n->level;
            reinserted[level] = 1;
            ds_rs_box_of(t, n, nb);
            for (d = 0; d < t->dims; d++)
                center[d] = (nb[d] + nb[t->dims + d]) / 2;
            for (i = 0; i < n->count; i++) {
                double s = 0;
                for (d = 0; d < t->dims; d++) {
                    double c = (n->e[i].b[d] + n->e[i].b[t->dims + d]) / 2 - center[d];
                    s += c * c;
                }
                dist[i] = s;
            }
            for (j = 0; j < DS_RS_REINSERT; j++) {
                unsigned far = 0;
                for (i = 1; i < n->count; i++)
                    if (dist[i] > dist[far])
                        far = i;
                out[j] = n->e[far];
                n->e[far] = n->e[n->count - 1];
                dist[far] = dist[n->count - 1];
                n->count--;
            }
            for (i = depth; i-- > 0;)
                ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
            for (j = DS_RS_REINSERT; j-- > 0;)
                if (ds_rs_insert_at(t, &out[j], level, reinserted) < 0)
                    return -1;
            return 0;
        } else {
            ds_rsnode* nn = ds_rs_split(t, n);
            if (!nn)
                return -1;
            if (n == t->root) {
                ds_rsnode* r = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
                if (!r) {
                    DYN_DS_FREE(nn);
                    return -1;
                }
                r->level = n->level + 1;
                r->count = 2;
                r->e[0].u.child = n;
                ds_rs_box_of(t, n, r->e[0].b);
                r->e[1].u.child = nn;
                ds_rs_box_of(t, nn, r->e[1].b);
                t->root = r;
                return 0;
            }
            {
                ds_rsnode* p = path[depth - 1];
                ds_rs_box_of(t, n, p->e[idx[depth - 1]].b);
                p->e[p->count].u.child = nn;
                ds_rs_box_of(t, nn, p->e[p->count].b);
                p->count++;
                depth--;
                {
                    unsigned i;
                    for (i = depth; i-- > 0;)
                        ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
                }
            }
        }
    }
    return 0;
}

static int ds_rs_insert_at(dyn_rstar_t* t, const ds_rsentry* in, unsigned level, uint8_t* reinserted)
{
    ds_rsnode* path[64];
    unsigned idx[64], depth = 0, i;
    ds_rsnode* n = t->root;
    path[0] = n;
    while (n->level > level) {
        unsigned c = ds_rs_choose(t, n, in->b);
        idx[depth] = c;
        ds_rs_union(t, n->e[c].b, n->e[c].b, in->b);
        n = n->e[c].u.child;
        path[++depth] = n;
    }
    n->e[n->count++] = *in;
    for (i = depth; i-- > 0;)
        ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
    return ds_rs_overflow(t, path, idx, depth, reinserted);
}

int dyn_rstar_insert(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id)
{
    ds_rsentry e;
    uint8_t reinserted[64];
    unsigned d;
    memset(&e, 0, sizeof(e));
    for (d = 0; d < t->dims; d++) {
        if (!(lo[d] <= hi[d]))
            return -1;
        e.b[d] = lo[d];
        e.b[t->dims + d] = hi[d];
    }
    e.u.id = id;
    memset(reinserted, 0, sizeof(reinserted));
    if (ds_rs_insert_at(t, &e, 0, reinserted) < 0)
        return -1;
    t->size++;
    return 0;
}

static int ds_rs_find_leaf(const dyn_rstar_t* t, ds_rsnode* n, const double* b, uint64_t id, ds_rsnode** path, unsigned* idx, unsigned depth, unsigned* out_depth, unsigned* out_i)
{
    unsigned i;
    path[depth] = n;
    if (n->level == 0) {
        for (i = 0; i < n->count; i++) {
            unsigned d, same = 1;
            if (n->e[i].u.id != id)
                continue;
            for (d = 0; d < 2 * t->dims; d++)
                if (n->e[i].b[d] != b[d])
                    same = 0;
            if (same) {
                *out_depth = depth;
                *out_i = i;
                return 1;
            }
        }
        return 0;
    }
    for (i = 0; i < n->count; i++) {
        if (!ds_rs_contains(t, n->e[i].b, b))
            continue;
        idx[depth] = i;
        if (ds_rs_find_leaf(t, n->e[i].u.child, b, id, path, idx, depth + 1, out_depth, out_i))
            return 1;
    }
    return 0;
}

static int ds_rs_collect(ds_rsnode* n, ds_rsentry** buf, unsigned** levels, size_t* cnt, size_t* cap)
{
    unsigned i;
    for (i = 0; i < n->count; i++) {
        if (*cnt == *cap) {
            size_t nc = *cap ? *cap * 2 : 32;
            ds_rsentry* nb = (ds_rsentry*)DYN_DS_REALLOC(*buf, nc * sizeof(ds_rsentry));
            unsigned* nl;
            if (!nb)
                return -1;
            *buf = nb;
            nl = (unsigned*)DYN_DS_REALLOC(*levels, nc * sizeof(unsigned));
            if (!nl)
                return -1;
            *levels = nl;
            *cap = nc;
        }
        (*buf)[*cnt] = n->e[i];
        (*levels)[*cnt] = n->level;
        (*cnt)++;
    }
    return 0;
}

int dyn_rstar_remove(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id)
{
    ds_rsnode* path[64];
    unsigned idx[64], depth = 0, i, d;
    double b[2 * DYN_RSTAR_MAX_DIMS];
    ds_rsentry* orphans = NULL;
    unsigned* levels = NULL;
    size_t norph = 0, cap = 0, k;
    int rc = 1;
    for (d = 0; d < t->dims; d++) {
        b[d] = lo[d];
        b[t->dims + d] = hi[d];
    }
    if (!ds_rs_find_leaf(t, t->root, b, id, path, idx, 0, &depth, &i))
        return 0;
    {
        ds_rsnode* leaf = path[depth];
        leaf->e[i] = leaf->e[leaf->count - 1];
        leaf->count--;
    }
    while (depth > 0) {
        ds_rsnode *n = path[depth], *p = path[depth - 1];
        if (n->count < DS_RS_MIN) {
            if (ds_rs_collect(n, &orphans, &levels, &norph, &cap) < 0) {
                rc = -1;
                break;
            }
            p->e[idx[depth - 1]] = p->e[p->count - 1];
            p->count--;
            DYN_DS_FREE(n);
        } else {
            ds_rs_box_of(t, n, p->e[idx[depth - 1]].b);
        }
        depth--;
    }
    t->size--;
    while (t->root->level > 0 && t->root->count == 1) {
        ds_rsnode* old = t->root;
        t->root = old->e[0].u.child;
        DYN_DS_FREE(old);
    }
    if (t->root->level > 0 && t->root->count == 0) {
        t->root->level = 0;
    }
    for (k = 0; k < norph && rc >= 0; k++) {
        uint8_t reinserted[64];
        memset(reinserted, 1, sizeof(reinserted));
        if (levels[k] == 0 || levels[k] <= t->root->level) {
            if (ds_rs_insert_at(t, &orphans[k], levels[k], reinserted) < 0)
                rc = -1;
        } else {
            ds_rsnode* sub = orphans[k].u.child;
            if (ds_rs_collect(sub, &orphans, &levels, &norph, &cap) < 0)
                rc = -1;
            DYN_DS_FREE(sub);
        }
    }
    DYN_DS_FREE(orphans);
    DYN_DS_FREE(levels);
    return rc;
}

static size_t ds_rs_search_rec(const dyn_rstar_t* t, const ds_rsnode* n, const double* b, dyn_rstar_visit fn, void* ud, int* stop)
{
    size_t hits = 0;
    unsigned i;
    for (i = 0; i < n->count && !*stop; i++) {
        if (!ds_rs_intersects(t, n->e[i].b, b))
            continue;
        if (n->level == 0) {
            hits++;
            if (fn && fn(n->e[i].b, n->e[i].b + t->dims, n->e[i].u.id, ud))
                *stop = 1;
        } else {
            hits += ds_rs_search_rec(t, n->e[i].u.child, b, fn, ud, stop);
        }
    }
    return hits;
}

size_t dyn_rstar_search(const dyn_rstar_t* t, const double* lo, const double* hi, dyn_rstar_visit fn, void* ud)
{
    double b[2 * DYN_RSTAR_MAX_DIMS];
    unsigned d;
    int stop = 0;
    for (d = 0; d < t->dims; d++) {
        b[d] = lo[d];
        b[t->dims + d] = hi[d];
    }
    return ds_rs_search_rec(t, t->root, b, fn, ud, &stop);
}

static double ds_rs_mindist2(const dyn_rstar_t* t, const double* b, const double* p)
{
    double s = 0;
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        double c = 0;
        if (p[d] < b[d])
            c = b[d] - p[d];
        else if (p[d] > b[t->dims + d])
            c = p[d] - b[t->dims + d];
        s += c * c;
    }
    return s;
}

typedef struct {
    double d;
    const ds_rsnode* n;
    uint64_t id;
} ds_rs_qitem;

size_t dyn_rstar_nearest(const dyn_rstar_t* t, const double* point, size_t k, uint64_t* ids, double* dist2)
{
    ds_rs_qitem* q;
    size_t qn = 0, qcap = 64, found = 0;
    if (!k || !t->size)
        return 0;
    q = (ds_rs_qitem*)DYN_DS_MALLOC(sizeof(ds_rs_qitem) * qcap);
    if (!q)
        return 0;
    q[qn++] = (ds_rs_qitem) { 0, t->root, 0 };
    while (qn && found < k) {
        ds_rs_qitem top = q[0], last = q[--qn];
        size_t i = 0;
        for (;;) {
            size_t c = 2 * i + 1;
            if (c >= qn)
                break;
            if (c + 1 < qn && q[c + 1].d < q[c].d)
                c++;
            if (last.d <= q[c].d)
                break;
            q[i] = q[c];
            i = c;
        }
        if (qn)
            q[i] = last;
        if (!top.n) {
            ids[found] = top.id;
            if (dist2)
                dist2[found] = top.d;
            found++;
            continue;
        }
        {
            unsigned e;
            for (e = 0; e < top.n->count; e++) {
                ds_rs_qitem it;
                size_t j;
                it.d = ds_rs_mindist2(t, top.n->e[e].b, point);
                it.n = top.n->level ? top.n->e[e].u.child : NULL;
                it.id = top.n->level ? 0 : top.n->e[e].u.id;
                if (qn == qcap) {
                    ds_rs_qitem* nq = (ds_rs_qitem*)DYN_DS_REALLOC(q, sizeof(ds_rs_qitem) * qcap * 2);
                    if (!nq) {
                        DYN_DS_FREE(q);
                        return found;
                    }
                    q = nq;
                    qcap *= 2;
                }
                j = qn++;
                while (j > 0) {
                    size_t p = (j - 1) / 2;
                    if (q[p].d <= it.d)
                        break;
                    q[j] = q[p];
                    j = p;
                }
                q[j] = it;
            }
        }
    }
    DYN_DS_FREE(q);
    return found;
}

static long ds_rs_check_rec(const dyn_rstar_t* t, const ds_rsnode* n, int is_root)
{
    unsigned i, d;
    long total = 0;
    if (!is_root && (n->count < DS_RS_MIN || n->count > DS_RS_M))
        return -1;
    if (n->count > DS_RS_M)
        return -1;
    if (n->level == 0)
        return n->count;
    for (i = 0; i < n->count; i++) {
        double b[2 * DYN_RSTAR_MAX_DIMS];
        long c;
        if (n->e[i].u.child->level != n->level - 1)
            return -1;
        ds_rs_box_of(t, n->e[i].u.child, b);
        for (d = 0; d < 2 * t->dims; d++)
            if (b[d] != n->e[i].b[d])
                return -1;
        c = ds_rs_check_rec(t, n->e[i].u.child, 0);
        if (c < 0)
            return -1;
        total += c;
    }
    return total;
}

int dyn_rstar_check(const dyn_rstar_t* t)
{
    long c = ds_rs_check_rec(t, t->root, 1);
    if (c < 0 || (size_t)c != t->size)
        return -1;
    if (t->root->level > 0 && t->root->count < 2)
        return -1;
    return 0;
}

#ifndef DYN_DS_NO_THREADS
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define DS_PARK_BUCKETS 512

typedef struct ds_parker {
    const void* addr;
    pthread_cond_t cv;
    int woken;
    struct ds_parker *prev, *next;
} ds_parker;

typedef struct {
    pthread_mutex_t mu;
    ds_parker *head, *tail;
    char pad[64];
} ds_park_bucket;

static ds_park_bucket ds_park_tab[DS_PARK_BUCKETS];
static pthread_once_t ds_park_once = PTHREAD_ONCE_INIT;

static void ds_park_init(void)
{
    unsigned i;
    for (i = 0; i < DS_PARK_BUCKETS; i++) {
        pthread_mutex_init(&ds_park_tab[i].mu, NULL);
        ds_park_tab[i].head = ds_park_tab[i].tail = NULL;
    }
}

static ds_park_bucket* ds_park_bucket_of(const void* addr)
{
    pthread_once(&ds_park_once, ds_park_init);
    return &ds_park_tab[(ds_mix64((uint64_t)(uintptr_t)addr) >> 32) & (DS_PARK_BUCKETS - 1)];
}

static void ds_park_unlink(ds_park_bucket* b, ds_parker* p)
{
    if (p->prev)
        p->prev->next = p->next;
    else
        b->head = p->next;
    if (p->next)
        p->next->prev = p->prev;
    else
        b->tail = p->prev;
    p->prev = p->next = NULL;
}

static int64_t ds_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int dyn_park_wait(const void* addr, dyn_park_validate validate, void* ud, int64_t timeout_ns)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker me;
    int64_t deadline = timeout_ns >= 0 ? ds_mono_ns() + timeout_ns : 0;
    int result = 0;
#if !defined(__APPLE__)
    pthread_condattr_t attr;
#endif
    pthread_mutex_lock(&b->mu);
    if (validate && !validate(ud)) {
        pthread_mutex_unlock(&b->mu);
        return -1;
    }
    me.addr = addr;
    me.woken = 0;
    me.next = NULL;
    me.prev = b->tail;
#if defined(__APPLE__)
    pthread_cond_init(&me.cv, NULL);
#else
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&me.cv, &attr);
    pthread_condattr_destroy(&attr);
#endif
    if (b->tail)
        b->tail->next = &me;
    else
        b->head = &me;
    b->tail = &me;
    while (!me.woken) {
        if (timeout_ns < 0) {
            pthread_cond_wait(&me.cv, &b->mu);
        } else {
            int64_t left = deadline - ds_mono_ns();
            struct timespec ts;
            if (left <= 0) {
                result = 1;
                break;
            }
#if defined(__APPLE__)
            ts.tv_sec = (time_t)(left / 1000000000);
            ts.tv_nsec = (long)(left % 1000000000);
            pthread_cond_timedwait_relative_np(&me.cv, &b->mu, &ts);
#else
            ts.tv_sec = (time_t)(deadline / 1000000000);
            ts.tv_nsec = (long)(deadline % 1000000000);
            pthread_cond_timedwait(&me.cv, &b->mu, &ts);
#endif
        }
    }
    if (!me.woken)
        ds_park_unlink(b, &me);
    pthread_mutex_unlock(&b->mu);
    pthread_cond_destroy(&me.cv);
    return me.woken ? 0 : result;
}

unsigned dyn_park_wake(const void* addr, unsigned count)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker *p, *next;
    unsigned n = 0;
    pthread_mutex_lock(&b->mu);
    for (p = b->head; p && n < count; p = next) {
        next = p->next;
        if (p->addr != addr)
            continue;
        ds_park_unlink(b, p);
        p->woken = 1;
        pthread_cond_signal(&p->cv);
        n++;
    }
    pthread_mutex_unlock(&b->mu);
    return n;
}

unsigned dyn_park_waiters(const void* addr)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker* p;
    unsigned n = 0;
    pthread_mutex_lock(&b->mu);
    for (p = b->head; p; p = p->next)
        n += p->addr == addr;
    pthread_mutex_unlock(&b->mu);
    return n;
}

struct dyn_ring {
    size_t esz, stride, mask;
    uint8_t* buf;
    char pad0[64];
    atomic_size_t enq;
    char pad1[64];
    atomic_size_t deq;
    char pad2[64];
};

#define DS_RING_SEQ(r, i) ((atomic_size_t*)(void*)((r)->buf + ((i) & (r)->mask) * (r)->stride))
#define DS_RING_ELEM(r, i) ((void*)((r)->buf + ((i) & (r)->mask) * (r)->stride + sizeof(atomic_size_t)))

dyn_ring_t* dyn_ring_new(size_t elem_size, size_t capacity)
{
    dyn_ring_t* r;
    size_t cap = 2, i;
    if (!elem_size || capacity > ((size_t)1 << 40))
        return NULL;
    while (cap < capacity)
        cap <<= 1;
    r = (dyn_ring_t*)DYN_DS_CALLOC(1, sizeof(*r));
    if (!r)
        return NULL;
    r->esz = elem_size;
    r->stride = ds_align8(sizeof(atomic_size_t) + elem_size);
    r->mask = cap - 1;
    r->buf = (uint8_t*)DYN_DS_MALLOC(cap * r->stride);
    if (!r->buf) {
        DYN_DS_FREE(r);
        return NULL;
    }
    for (i = 0; i < cap; i++)
        atomic_init(DS_RING_SEQ(r, i), i);
    atomic_init(&r->enq, 0);
    atomic_init(&r->deq, 0);
    return r;
}

void dyn_ring_free(dyn_ring_t* r)
{
    if (!r)
        return;
    DYN_DS_FREE(r->buf);
    DYN_DS_FREE(r);
}

size_t dyn_ring_capacity(const dyn_ring_t* r)
{
    return r->mask + 1;
}

int dyn_ring_try_push(dyn_ring_t* r, const void* elem)
{
    size_t pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
    for (;;) {
        atomic_size_t* seq = DS_RING_SEQ(r, pos);
        size_t s = atomic_load_explicit(seq, memory_order_acquire);
        intptr_t dif = (intptr_t)s - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->enq, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                memcpy(DS_RING_ELEM(r, pos), elem, r->esz);
                atomic_store_explicit(seq, pos + 1, memory_order_release);
                return 0;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
        }
    }
}

int dyn_ring_try_pop(dyn_ring_t* r, void* out)
{
    size_t pos = atomic_load_explicit(&r->deq, memory_order_relaxed);
    for (;;) {
        atomic_size_t* seq = DS_RING_SEQ(r, pos);
        size_t s = atomic_load_explicit(seq, memory_order_acquire);
        intptr_t dif = (intptr_t)s - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->deq, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                memcpy(out, DS_RING_ELEM(r, pos), r->esz);
                atomic_store_explicit(seq, pos + r->mask + 1, memory_order_release);
                return 0;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = atomic_load_explicit(&r->deq, memory_order_relaxed);
        }
    }
}

size_t dyn_ring_size_approx(const dyn_ring_t* r)
{
    size_t e = atomic_load_explicit(&r->enq, memory_order_relaxed);
    size_t d = atomic_load_explicit(&r->deq, memory_order_relaxed);
    return e > d ? e - d : 0;
}

typedef struct ds_wsarray {
    int64_t size;
    struct ds_wsarray* retired;
    _Atomic uint64_t buf[];
} ds_wsarray;

struct dyn_wsdeque {
    char pad0[64];
    _Atomic int64_t top;
    char pad1[64];
    _Atomic int64_t bottom;
    char pad2[64];
    _Atomic(ds_wsarray*) array;
    ds_wsarray* retired;
};

static ds_wsarray* ds_ws_array_new(int64_t size)
{
    ds_wsarray* a = (ds_wsarray*)DYN_DS_MALLOC(sizeof(ds_wsarray) + sizeof(_Atomic uint64_t) * (size_t)size);
    int64_t i;
    if (!a)
        return NULL;
    a->size = size;
    a->retired = NULL;
    for (i = 0; i < size; i++)
        atomic_init(&a->buf[i], 0);
    return a;
}

dyn_wsdeque_t* dyn_wsdeque_new(size_t initial_capacity)
{
    dyn_wsdeque_t* q;
    int64_t size = 16;
    ds_wsarray* a;
    while ((size_t)size < initial_capacity && size < ((int64_t)1 << 40))
        size <<= 1;
    q = (dyn_wsdeque_t*)DYN_DS_CALLOC(1, sizeof(*q));
    if (!q)
        return NULL;
    a = ds_ws_array_new(size);
    if (!a) {
        DYN_DS_FREE(q);
        return NULL;
    }
    atomic_init(&q->top, 0);
    atomic_init(&q->bottom, 0);
    atomic_init(&q->array, a);
    return q;
}

void dyn_wsdeque_free(dyn_wsdeque_t* q)
{
    ds_wsarray *a, *r;
    if (!q)
        return;
    a = atomic_load_explicit(&q->array, memory_order_relaxed);
    DYN_DS_FREE(a);
    r = q->retired;
    while (r) {
        ds_wsarray* nx = r->retired;
        DYN_DS_FREE(r);
        r = nx;
    }
    DYN_DS_FREE(q);
}

int dyn_wsdeque_push(dyn_wsdeque_t* q, uint64_t item)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_relaxed);
    if (b - t > a->size - 1) {
        ds_wsarray* na = ds_ws_array_new(a->size * 2);
        int64_t i;
        if (!na)
            return -1;
        for (i = t; i < b; i++)
            atomic_store_explicit(&na->buf[i & (na->size - 1)], atomic_load_explicit(&a->buf[i & (a->size - 1)], memory_order_relaxed), memory_order_relaxed);
        a->retired = q->retired;
        q->retired = a;
        atomic_store_explicit(&q->array, na, memory_order_release);
        a = na;
    }
    atomic_store_explicit(&a->buf[b & (a->size - 1)], item, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return 0;
}

int dyn_wsdeque_pop(dyn_wsdeque_t* q, uint64_t* item)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;
    ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_relaxed);
    int64_t t;
    int ok = 0;
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    t = atomic_load_explicit(&q->top, memory_order_relaxed);
    if (t <= b) {
        *item = atomic_load_explicit(&a->buf[b & (a->size - 1)], memory_order_relaxed);
        ok = 1;
        if (t == b) {
            if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
                ok = 0;
            atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    }
    return ok ? 0 : -1;
}

int dyn_wsdeque_steal(dyn_wsdeque_t* q, uint64_t* item)
{
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    int64_t b;
    atomic_thread_fence(memory_order_seq_cst);
    b = atomic_load_explicit(&q->bottom, memory_order_acquire);
    if (t < b) {
        ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_acquire);
        uint64_t x = atomic_load_explicit(&a->buf[t & (a->size - 1)], memory_order_relaxed);
        if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
            return 1;
        *item = x;
        return 0;
    }
    return -1;
}

size_t dyn_wsdeque_size_approx(const dyn_wsdeque_t* q)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    return b > t ? (size_t)(b - t) : 0;
}
#endif

static uint64_t ds_splitmix64(uint64_t* x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t ds_str_seed(void)
{
    return atomic_load_explicit(&ds_seed0, memory_order_relaxed);
}

uint64_t dyn_ds_strhash(const char* p, size_t n)
{
    return dyn_ds_xxh64((const uint8_t*)p, n, ds_str_seed());
}

static uint64_t dyn_ds_strhash_fixed(const char* p, size_t n, uint64_t salt)
{
    return dyn_ds_xxh64((const uint8_t*)p, n, DYN_DS_FIXED_SEED ^ salt);
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

static dyn_ds_ient* dyn_ds_imap_put(dyn_ds_imap* m, uint32_t k, uint32_t v)
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

static void dyn_ds_imap_remove(dyn_ds_imap* m, dyn_ds_ient* s)
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

#define DS_R4(x) x, x, x, x
static const uint64_t dyn_roar_ones[DYN_ROAR_WORDS] = { DS_R4(DS_R4(DS_R4(DS_R4(DS_R4(~0ULL))))) };
#undef DS_R4

static const uint64_t* dyn_roar_words(const dyn_roar_ct* c)
{
    return c->type == DYN_ROAR_FULL ? dyn_roar_ones : (const uint64_t*)c->data;
}

static void dyn_roar_ct_free(dyn_roar_ct* c)
{
    if (c->type != DYN_ROAR_FULL)
        DYN_DS_FREE(c->data);
    c->data = NULL;
    c->type = 0;
    c->card = 0;
}

void dyn_roar_clear(dyn_roar_t* r)
{
    uint32_t i;
    for (i = 0; i < r->n; i++)
        dyn_roar_ct_free(&r->c[i]);
    DYN_DS_FREE(r->c);
    r->c = NULL;
    r->n = r->cap = 0;
}

static int dyn_roar_find(const dyn_roar_t* r, uint16_t key, uint32_t* pos)
{
    uint32_t lo = 0, hi = r->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (r->c[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos = lo;
    return lo < r->n && r->c[lo].key == key;
}

int dyn_roar_reserve(dyn_roar_t* r, uint32_t need)
{
    uint32_t ncap;
    dyn_roar_ct* nc;
    if (need <= r->cap)
        return 0;
    ncap = r->cap ? r->cap * 2 : 4;
    while (ncap < need)
        ncap *= 2;
    nc = (dyn_roar_ct*)DYN_DS_REALLOC(r->c, sizeof(dyn_roar_ct) * ncap);
    if (!nc)
        return -1;
    r->c = nc;
    r->cap = ncap;
    return 0;
}

static dyn_roar_ct* dyn_roar_insert_at(dyn_roar_t* r, uint32_t pos, uint16_t key)
{
    if (dyn_roar_reserve(r, r->n + 1) < 0)
        return NULL;
    memmove(&r->c[pos + 1], &r->c[pos], sizeof(dyn_roar_ct) * (r->n - pos));
    r->n++;
    memset(&r->c[pos], 0, sizeof(dyn_roar_ct));
    r->c[pos].key = key;
    return &r->c[pos];
}

static void dyn_roar_remove_at(dyn_roar_t* r, uint32_t pos)
{
    dyn_roar_ct_free(&r->c[pos]);
    memmove(&r->c[pos], &r->c[pos + 1], sizeof(dyn_roar_ct) * (r->n - pos - 1));
    r->n--;
}

uint32_t dyn_roar_popcnt(const uint64_t* w)
{
    uint32_t i, c = 0;
    for (i = 0; i < DYN_ROAR_WORDS; i++)
        c += (uint32_t)__builtin_popcountll(w[i]);
    return c;
}

static void dyn_roar_arr_to_words(const dyn_roar_ct* c, uint64_t* w)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t i;
    memset(w, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
    for (i = 0; i < c->card; i++)
        w[a[i] >> 6] |= 1ULL << (a[i] & 63);
}

static const uint64_t* dyn_roar_words_of(const dyn_roar_ct* c, uint64_t* tmp)
{
    if (!c) {
        memset(tmp, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
        return tmp;
    }
    if (c->type == DYN_ROAR_ARR) {
        dyn_roar_arr_to_words(c, tmp);
        return tmp;
    }
    return dyn_roar_words(c);
}

static int dyn_roar_adopt_words(dyn_roar_ct* out, uint64_t* w, uint32_t card)
{
    out->card = card;
    if (card == 0) {
        DYN_DS_FREE(w);
        out->type = 0;
        out->data = NULL;
        return 0;
    }
    if (card == 65536) {
        DYN_DS_FREE(w);
        out->type = DYN_ROAR_FULL;
        out->data = NULL;
        return 0;
    }
    if (card <= DYN_ROAR_ARR_MAX) {
        uint16_t* a = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * card);
        uint32_t i, k = 0;
        if (!a) {
            DYN_DS_FREE(w);
            return -1;
        }
        for (i = 0; i < DYN_ROAR_WORDS; i++) {
            uint64_t x = w[i];
            while (x) {
                a[k++] = (uint16_t)(i * 64 + __builtin_ctzll(x));
                x &= x - 1;
            }
        }
        DYN_DS_FREE(w);
        out->type = DYN_ROAR_ARR;
        out->data = a;
        out->cap = card;
        return 0;
    }
    out->type = DYN_ROAR_BMP;
    out->data = w;
    out->cap = 0;
    return 0;
}

static int dyn_roar_ct_copy(dyn_roar_ct* out, const dyn_roar_ct* src)
{
    out->key = src->key;
    out->type = src->type;
    out->card = src->card;
    out->cap = 0;
    out->data = NULL;
    if (src->type == DYN_ROAR_ARR) {
        out->data = DYN_DS_MALLOC(sizeof(uint16_t) * src->card);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint16_t) * src->card);
        out->cap = src->card;
    } else if (src->type == DYN_ROAR_BMP) {
        out->data = DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint64_t) * DYN_ROAR_WORDS);
    }
    return 0;
}

static int dyn_roar_arr_has(const dyn_roar_ct* c, uint16_t lo, uint32_t* pos)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t l = 0, h = c->card;
    while (l < h) {
        uint32_t m = (l + h) >> 1;
        if (a[m] < lo)
            l = m + 1;
        else
            h = m;
    }
    if (pos)
        *pos = l;
    return l < c->card && a[l] == lo;
}

static int dyn_roar_ct_has(const dyn_roar_ct* c, uint16_t lo)
{
    if (c->type == DYN_ROAR_ARR)
        return dyn_roar_arr_has(c, lo, NULL);
    return (dyn_roar_words(c)[lo >> 6] >> (lo & 63)) & 1;
}

int dyn_roar_add(dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos;
    dyn_roar_ct* c;
    if (!dyn_roar_find(r, hi, &pos)) {
        c = dyn_roar_insert_at(r, pos, hi);
        if (!c)
            return -1;
        c->data = DYN_DS_MALLOC(sizeof(uint16_t) * 4);
        if (!c->data) {
            memmove(&r->c[pos], &r->c[pos + 1], sizeof(dyn_roar_ct) * (r->n - pos - 1));
            r->n--;
            return -1;
        }
        c->type = DYN_ROAR_ARR;
        c->cap = 4;
        c->card = 1;
        ((uint16_t*)c->data)[0] = lo;
        return 1;
    }
    c = &r->c[pos];
    if (c->type == DYN_ROAR_FULL)
        return 0;
    if (c->type == DYN_ROAR_BMP) {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (w[lo >> 6] & b)
            return 0;
        w[lo >> 6] |= b;
        if (++c->card == 65536) {
            DYN_DS_FREE(c->data);
            c->data = NULL;
            c->type = DYN_ROAR_FULL;
        }
        return 1;
    }
    {
        uint16_t* a = (uint16_t*)c->data;
        uint32_t p;
        if (c->card && a[c->card - 1] < lo)
            p = c->card;
        else if (dyn_roar_arr_has(c, lo, &p))
            return 0;
        if (c->card == DYN_ROAR_ARR_MAX) {
            uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
            if (!w)
                return -1;
            dyn_roar_arr_to_words(c, w);
            w[lo >> 6] |= 1ULL << (lo & 63);
            DYN_DS_FREE(c->data);
            c->data = w;
            c->type = DYN_ROAR_BMP;
            c->card++;
            return 1;
        }
        if (c->card == c->cap) {
            uint32_t ncap = c->cap * 2 > DYN_ROAR_ARR_MAX ? DYN_ROAR_ARR_MAX : c->cap * 2;
            uint16_t* na = (uint16_t*)DYN_DS_REALLOC(a, sizeof(uint16_t) * ncap);
            if (!na)
                return -1;
            c->data = a = na;
            c->cap = ncap;
        }
        memmove(&a[p + 1], &a[p], sizeof(uint16_t) * (c->card - p));
        a[p] = lo;
        c->card++;
        return 1;
    }
}

int dyn_roar_remove(dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos, p;
    dyn_roar_ct* c;
    if (!dyn_roar_find(r, hi, &pos))
        return 0;
    c = &r->c[pos];
    if (c->type == DYN_ROAR_ARR) {
        uint16_t* a = (uint16_t*)c->data;
        if (!dyn_roar_arr_has(c, lo, &p))
            return 0;
        memmove(&a[p], &a[p + 1], sizeof(uint16_t) * (c->card - p - 1));
        if (--c->card == 0)
            dyn_roar_remove_at(r, pos);
        return 1;
    }
    if (c->type == DYN_ROAR_FULL) {
        uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        memcpy(w, dyn_roar_ones, sizeof(dyn_roar_ones));
        c->data = w;
        c->type = DYN_ROAR_BMP;
    }
    {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (!(w[lo >> 6] & b))
            return 0;
        w[lo >> 6] &= ~b;
        c->card--;
        if (c->card <= DYN_ROAR_ARR_MAX) {
            dyn_roar_ct t = *c;
            c->data = NULL;
            if (dyn_roar_adopt_words(&t, w, c->card) < 0) {
                c->data = NULL;
                dyn_roar_remove_at(r, pos);
                return -1;
            }
            *c = t;
        }
        return 1;
    }
}

int dyn_roar_has(const dyn_roar_t* r, uint32_t x)
{
    uint32_t pos;
    if (!dyn_roar_find(r, (uint16_t)(x >> 16), &pos))
        return 0;
    return dyn_roar_ct_has(&r->c[pos], (uint16_t)x);
}

int dyn_roar_range(dyn_roar_t* r, uint64_t lo, uint64_t hi, int add)
{
    uint64_t k;
    if (lo >= hi)
        return 0;
    for (k = lo >> 16; k <= (hi - 1) >> 16; k++) {
        uint32_t a = k == (lo >> 16) ? (uint32_t)(lo & 0xFFFF) : 0;
        uint32_t b = k == ((hi - 1) >> 16) ? (uint32_t)((hi - 1) & 0xFFFF) : 0xFFFF;
        uint32_t pos;
        int found = dyn_roar_find(r, (uint16_t)k, &pos);
        dyn_roar_ct* c;
        uint64_t* w;
        uint32_t i;
        if (a == 0 && b == 0xFFFF) {
            if (!add) {
                if (found)
                    dyn_roar_remove_at(r, pos);
                continue;
            }
            if (!found && !dyn_roar_insert_at(r, pos, (uint16_t)k))
                return -1;
            dyn_roar_ct_free(&r->c[pos]);
            r->c[pos].type = DYN_ROAR_FULL;
            r->c[pos].card = 65536;
            continue;
        }
        if (!found && !add)
            continue;
        w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        if (found) {
            const uint64_t* src = dyn_roar_words_of(&r->c[pos], w);
            if (src != w)
                memcpy(w, src, sizeof(uint64_t) * DYN_ROAR_WORDS);
        } else {
            memset(w, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
        }
        for (i = a; i <= b;) {
            if ((i & 63) == 0 && i + 63 <= b) {
                w[i >> 6] = add ? ~0ULL : 0;
                i += 64;
            } else {
                if (add)
                    w[i >> 6] |= 1ULL << (i & 63);
                else
                    w[i >> 6] &= ~(1ULL << (i & 63));
                i++;
            }
        }
        if (!found) {
            c = dyn_roar_insert_at(r, pos, (uint16_t)k);
            if (!c) {
                DYN_DS_FREE(w);
                return -1;
            }
        } else {
            c = &r->c[pos];
            dyn_roar_ct_free(c);
        }
        if (dyn_roar_adopt_words(c, w, dyn_roar_popcnt(w)) < 0) {
            dyn_roar_remove_at(r, pos);
            return -1;
        }
        if (c->card == 0)
            dyn_roar_remove_at(r, pos);
    }
    return 0;
}

uint64_t dyn_roar_card(const dyn_roar_t* r)
{
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n; i++)
        s += r->c[i].card;
    return s;
}

static int dyn_roar_ct_words_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp);

static uint32_t ds_roar_merge_and(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o)
{
    uint32_t p = 0, q = 0, card = 0;
    if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)xv;
            card += xv == yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv == yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    return card;
}

static uint32_t ds_roar_merge_andnot(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o)
{
    uint32_t p = 0, q = 0, card = 0;
    if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)xv;
            card += xv < yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv < yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    if (o && p < na)
        memcpy(o + card, x + p, sizeof(uint16_t) * (na - p));
    return card + (na - p);
}

static uint32_t ds_roar_merge_or(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o, int xor_)
{
    uint32_t p = 0, q = 0, card = 0;
    if (o && xor_)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)(xv < yv ? xv : yv);
            card += xv != yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)(xv < yv ? xv : yv);
            card++;
            p += xv <= yv;
            q += yv <= xv;
        }
    else if (xor_)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv != yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card++;
            p += xv <= yv;
            q += yv <= xv;
        }
    if (o && p < na)
        memcpy(o + card, x + p, sizeof(uint16_t) * (na - p));
    if (o && q < nb)
        memcpy(o + card, y + q, sizeof(uint16_t) * (nb - q));
    return card + (na - p) + (nb - q);
}

static uint32_t ds_roar_merge(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, int op, uint16_t* o)
{
    if (op == DYN_ROAR_AND)
        return ds_roar_merge_and(x, na, y, nb, o);
    if (op == DYN_ROAR_ANDNOT)
        return ds_roar_merge_andnot(x, na, y, nb, o);
    return ds_roar_merge_or(x, na, y, nb, o, op == DYN_ROAR_XOR);
}

static int dyn_roar_ct_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp)
{
    uint32_t i, card = 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->key = a ? a->key : b->key;
    }
    if (!a || !b) {
        const dyn_roar_ct* s = NULL;
        if (op == DYN_ROAR_AND || (op == DYN_ROAR_ANDNOT && !a))
            s = NULL;
        else
            s = a ? a : b;
        if (!s) {
            *cardp = 0;
            return 0;
        }
        *cardp = s->card;
        return out ? dyn_roar_ct_copy(out, s) : 0;
    }
    if (a->type == DYN_ROAR_ARR && b->type == DYN_ROAR_ARR) {
        const uint16_t *x = (const uint16_t*)a->data, *y = (const uint16_t*)b->data;
        uint32_t cap = op == DYN_ROAR_AND ? (a->card < b->card ? a->card : b->card) : op == DYN_ROAR_ANDNOT ? a->card
                                                                                                            : a->card + b->card;
        uint16_t* o = NULL;
        if (out && cap) {
            o = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * cap);
            if (!o)
                return -1;
        }
        card = ds_roar_merge(x, a->card, y, b->card, op, o);
        *cardp = card;
        if (!out)
            return 0;
        if (card == 0) {
            DYN_DS_FREE(o);
            return 0;
        }
        if (card <= DYN_ROAR_ARR_MAX) {
            out->type = DYN_ROAR_ARR;
            out->data = o;
            out->cap = cap;
            out->card = card;
            return 0;
        }
        {
            uint64_t* w = (uint64_t*)DYN_DS_CALLOC(DYN_ROAR_WORDS, sizeof(uint64_t));
            if (!w) {
                DYN_DS_FREE(o);
                return -1;
            }
            for (i = 0; i < card; i++)
                w[o[i] >> 6] |= 1ULL << (o[i] & 63);
            DYN_DS_FREE(o);
            return dyn_roar_adopt_words(out, w, card);
        }
    }
    if ((op == DYN_ROAR_AND || op == DYN_ROAR_ANDNOT) && a->type == DYN_ROAR_ARR) {
        const uint16_t* x = (const uint16_t*)a->data;
        const uint64_t* yw = dyn_roar_words(b);
        uint16_t* o = NULL;
        if (out) {
            o = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * a->card);
            if (!o)
                return -1;
        }
        for (i = 0; i < a->card; i++) {
            int in = (int)((yw[x[i] >> 6] >> (x[i] & 63)) & 1);
            if (in == (op == DYN_ROAR_AND)) {
                if (o)
                    o[card] = x[i];
                card++;
            }
        }
        *cardp = card;
        if (!out)
            return 0;
        if (!card) {
            DYN_DS_FREE(o);
            return 0;
        }
        out->type = DYN_ROAR_ARR;
        out->data = o;
        out->cap = a->card;
        out->card = card;
        return 0;
    }
    if (op == DYN_ROAR_AND && b->type == DYN_ROAR_ARR)
        return dyn_roar_ct_op(b, a, op, out, cardp);
    return dyn_roar_ct_words_op(a, b, op, out, cardp);
}

static int dyn_roar_ct_words_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp)
{
    uint64_t ta[DYN_ROAR_WORDS] DS_SCRATCH, tb[DYN_ROAR_WORDS] DS_SCRATCH;
    const uint64_t *wa, *wb;
    uint32_t i, card = 0;
    wa = dyn_roar_words_of(a, ta);
    wb = dyn_roar_words_of(b, tb);
    if (!out) {
        for (i = 0; i < DYN_ROAR_WORDS; i++) {
            uint64_t v = op == DYN_ROAR_AND ? wa[i] & wb[i] : op == DYN_ROAR_OR ? wa[i] | wb[i]
                : op == DYN_ROAR_XOR                                            ? wa[i] ^ wb[i]
                                                                                : wa[i] & ~wb[i];
            card += (uint32_t)__builtin_popcountll(v);
        }
        *cardp = card;
        return 0;
    }
    {
        uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        for (i = 0; i < DYN_ROAR_WORDS; i++) {
            uint64_t v = op == DYN_ROAR_AND ? wa[i] & wb[i] : op == DYN_ROAR_OR ? wa[i] | wb[i]
                : op == DYN_ROAR_XOR                                            ? wa[i] ^ wb[i]
                                                                                : wa[i] & ~wb[i];
            w[i] = v;
            card += (uint32_t)__builtin_popcountll(v);
        }
        *cardp = card;
        return dyn_roar_adopt_words(out, w, card);
    }
}

int dyn_roar_op(dyn_roar_t* a, const dyn_roar_t* b, int op)
{
    dyn_roar_t nr = { NULL, 0, 0 };
    uint32_t i = 0, j = 0;
    if (a == b) {
        if (op == DYN_ROAR_XOR || op == DYN_ROAR_ANDNOT)
            dyn_roar_clear(a);
        return 0;
    }
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        dyn_roar_ct out;
        uint64_t card;
        if (op == DYN_ROAR_AND && (i >= a->n || j >= b->n))
            break;
        if (op == DYN_ROAR_ANDNOT && i >= a->n)
            break;
        if (ka < kb && op != DYN_ROAR_AND) {
            if (dyn_roar_reserve(&nr, nr.n + 1) < 0)
                goto oom;
            nr.c[nr.n++] = a->c[i];
            a->c[i].data = NULL;
            a->c[i].type = 0;
            i++;
            continue;
        }
        if (ka < kb) {
            i++;
            continue;
        }
        if (kb < ka && (op == DYN_ROAR_AND || op == DYN_ROAR_ANDNOT)) {
            j++;
            continue;
        }
        if (dyn_roar_ct_op(ka == kb ? &a->c[i] : NULL, &b->c[j], op, &out, &card) < 0)
            goto oom;
        if (ka == kb)
            i++;
        j++;
        if (card) {
            if (dyn_roar_reserve(&nr, nr.n + 1) < 0) {
                dyn_roar_ct_free(&out);
                goto oom;
            }
            nr.c[nr.n++] = out;
        }
    }
    dyn_roar_clear(a);
    *a = nr;
    return 0;
oom:
    dyn_roar_clear(&nr);
    return -1;
}

uint64_t dyn_roar_op_card(const dyn_roar_t* a, const dyn_roar_t* b, int op)
{
    uint32_t i = 0, j = 0;
    uint64_t s = 0;
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        uint64_t card = 0;
        if (ka == kb) {
            dyn_roar_ct_op(&a->c[i], &b->c[j], op, NULL, &card);
            i++;
            j++;
        } else if (ka < kb) {
            dyn_roar_ct_op(&a->c[i], NULL, op, NULL, &card);
            i++;
        } else {
            dyn_roar_ct_op(NULL, &b->c[j], op, NULL, &card);
            j++;
        }
        s += card;
    }
    return s;
}

uint64_t dyn_roar_rank(const dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n && r->c[i].key < hi; i++)
        s += r->c[i].card;
    if (i < r->n && r->c[i].key == hi) {
        const dyn_roar_ct* c = &r->c[i];
        if (c->type == DYN_ROAR_FULL)
            s += (uint32_t)lo + 1;
        else if (c->type == DYN_ROAR_ARR) {
            uint32_t p;
            s += dyn_roar_arr_has(c, lo, &p) ? p + 1 : p;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t k;
            for (k = 0; k < (uint32_t)(lo >> 6); k++)
                s += (uint64_t)__builtin_popcountll(w[k]);
            s += (uint64_t)__builtin_popcountll(w[lo >> 6] & (((lo & 63) == 63) ? ~0ULL : ((2ULL << (lo & 63)) - 1)));
        }
    }
    return s;
}

int dyn_roar_select(const dyn_roar_t* r, uint64_t i, uint32_t* out)
{
    uint32_t k;
    for (k = 0; k < r->n; k++) {
        const dyn_roar_ct* c = &r->c[k];
        if (i >= c->card) {
            i -= c->card;
            continue;
        }
        if (c->type == DYN_ROAR_FULL)
            *out = ((uint32_t)c->key << 16) | (uint32_t)i;
        else if (c->type == DYN_ROAR_ARR)
            *out = ((uint32_t)c->key << 16) | ((const uint16_t*)c->data)[i];
        else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < DYN_ROAR_WORDS; j++) {
                uint32_t pc = (uint32_t)__builtin_popcountll(w[j]);
                if (i < pc) {
                    uint64_t x = w[j];
                    while (i--)
                        x &= x - 1;
                    *out = ((uint32_t)c->key << 16) | (j * 64 + (uint32_t)__builtin_ctzll(x));
                    return 1;
                }
                i -= pc;
            }
            return 0;
        }
        return 1;
    }
    return 0;
}

void dyn_roar_fill(const dyn_roar_t* r, uint32_t* out)
{
    uint32_t k, n = 0;
    for (k = 0; k < r->n; k++) {
        const dyn_roar_ct* c = &r->c[k];
        uint32_t base = (uint32_t)c->key << 16, i;
        if (c->type == DYN_ROAR_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            for (i = 0; i < c->card; i++)
                out[n++] = base | a[i];
        } else if (c->type == DYN_ROAR_FULL) {
            for (i = 0; i < 65536; i++)
                out[n++] = base | i;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            for (i = 0; i < DYN_ROAR_WORDS; i++) {
                uint64_t x = w[i];
                while (x) {
                    out[n++] = base | (i * 64 + (uint32_t)__builtin_ctzll(x));
                    x &= x - 1;
                }
            }
        }
    }
}

int dyn_roar_clone(dyn_roar_t* dst, const dyn_roar_t* src)
{
    uint32_t i;
    dst->c = NULL;
    dst->n = dst->cap = 0;
    if (dyn_roar_reserve(dst, src->n) < 0)
        return -1;
    for (i = 0; i < src->n; i++) {
        if (dyn_roar_ct_copy(&dst->c[i], &src->c[i]) < 0) {
            dst->n = i;
            dyn_roar_clear(dst);
            return -1;
        }
        dst->n = i + 1;
    }
    return 0;
}

int dyn_roar_u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

int dyn_tdigest_init(dyn_tdigest_t* t, double delta)
{
    memset(t, 0, sizeof(*t));
    t->delta = delta;
    t->cap = (uint32_t)(delta * 6) + 64;
    t->c = (dyn_tdigest_cent*)DYN_DS_MALLOC(sizeof(dyn_tdigest_cent) * t->cap);
    t->min = HUGE_VAL;
    t->max = -HUGE_VAL;
    return t->c ? 0 : -1;
}

void dyn_tdigest_free(dyn_tdigest_t* t)
{
    DYN_DS_FREE(t->c);
    t->c = NULL;
}

static int dyn_tdigest_cent_cmp(const void* a, const void* b)
{
    double x = ((const dyn_tdigest_cent*)a)->m, y = ((const dyn_tdigest_cent*)b)->m;
    return (x > y) - (x < y);
}

static double dyn_tdigest_k(double q, double d)
{
    return d / (2 * DS_PI) * asin(2 * q - 1);
}

static double dyn_tdigest_kinv(double k, double d)
{
    if (k <= -d / 4)
        return 0;
    if (k >= d / 4)
        return 1;
    return (sin(k * 2 * DS_PI / d) + 1) / 2;
}

void dyn_tdigest_compress(dyn_tdigest_t* t)
{
    uint32_t n = t->n + t->nbuf, i, out = 0;
    double total = 0, so_far = 0, qlimit;
    dyn_tdigest_cent cur;
    if (t->nbuf == 0)
        return;
    qsort(t->c, n, sizeof(dyn_tdigest_cent), dyn_tdigest_cent_cmp);
    for (i = 0; i < n; i++)
        total += t->c[i].w;
    cur = t->c[0];
    qlimit = dyn_tdigest_kinv(dyn_tdigest_k(0, t->delta) + 1, t->delta);
    for (i = 1; i < n; i++) {
        dyn_tdigest_cent nx = t->c[i];
        double q = (so_far + cur.w + nx.w) / total;
        if (q <= qlimit) {
            double w = cur.w + nx.w;
            cur.m += (nx.m - cur.m) * nx.w / w;
            cur.w = w;
        } else {
            t->c[out++] = cur;
            so_far += cur.w;
            qlimit = dyn_tdigest_kinv(dyn_tdigest_k(so_far / total, t->delta) + 1, t->delta);
            cur = nx;
        }
    }
    t->c[out++] = cur;
    t->n = out;
    t->nbuf = 0;
    t->total = total;
}

void dyn_tdigest_add(dyn_tdigest_t* t, double x, double w)
{
    if (t->n + t->nbuf >= t->cap)
        dyn_tdigest_compress(t);
    if (t->n + t->nbuf >= t->cap)
        return;
    t->c[t->n + t->nbuf].m = x;
    t->c[t->n + t->nbuf].w = w;
    t->nbuf++;
    t->total += w;
    t->sum += x * w;
    if (x < t->min)
        t->min = x;
    if (x > t->max)
        t->max = x;
}

static double dyn_tdigest_wavg(double x1, double w1, double x2, double w2)
{
    double lo = x1 < x2 ? x1 : x2, hi = x1 < x2 ? x2 : x1;
    double r;
    if (!(w1 + w2 > 0))
        return (x1 + x2) / 2;
    r = (x1 * w1 + x2 * w2) / (w1 + w2);
    return r < lo ? lo : r > hi ? hi
                                : r;
}

double dyn_tdigest_quantile(dyn_tdigest_t* t, double q)
{
    double W, index, so_far;
    uint32_t i, n;
    dyn_tdigest_cent* c;
    dyn_tdigest_compress(t);
    n = t->n;
    c = t->c;
    if (n == 0)
        return (double)NAN;
    if (n == 1)
        return c[0].m;
    W = t->total;
    index = q * W;
    if (index < 1)
        return t->min;
    if (c[0].w > 1 && index < c[0].w / 2)
        return t->min + (index - 1) / (c[0].w / 2 - 1) * (c[0].m - t->min);
    if (index > W - 1)
        return t->max;
    if (c[n - 1].w > 1 && W - index <= c[n - 1].w / 2)
        return t->max - (W - index - 1) / (c[n - 1].w / 2 - 1) * (t->max - c[n - 1].m);
    so_far = c[0].w / 2;
    for (i = 0; i + 1 < n; i++) {
        double dw = (c[i].w + c[i + 1].w) / 2;
        if (so_far + dw > index) {
            double lu = 0, ru = 0, z1, z2;
            if (c[i].w == 1) {
                if (index - so_far < 0.5)
                    return c[i].m;
                lu = 0.5;
            }
            if (c[i + 1].w == 1) {
                if (so_far + dw - index <= 0.5)
                    return c[i + 1].m;
                ru = 0.5;
            }
            z1 = index - so_far - lu;
            z2 = so_far + dw - index - ru;
            return dyn_tdigest_wavg(c[i].m, z2, c[i + 1].m, z1);
        }
        so_far += dw;
    }
    {
        double z1 = index - so_far, z2 = c[n - 1].w / 2 - z1;
        if (z1 < 0)
            z1 = 0;
        if (z2 < 0)
            z2 = 0;
        if (z1 + z2 == 0)
            return c[n - 1].m;
        return dyn_tdigest_wavg(c[n - 1].m, z2, t->max, z1);
    }
}

double dyn_tdigest_cdf(dyn_tdigest_t* t, double x)
{
    uint32_t i, n;
    dyn_tdigest_cent* c;
    double cum = 0, px, py;
    dyn_tdigest_compress(t);
    n = t->n;
    c = t->c;
    if (n == 0)
        return (double)NAN;
    if (x < t->min)
        return 0;
    if (x >= t->max)
        return 1;
    if (n == 1)
        return t->max - t->min > 0 ? (x - t->min) / (t->max - t->min) : 0.5;
    px = t->min;
    py = 0;
    for (i = 0; i < n; i++) {
        double kx = c[i].m, ky = cum + c[i].w / 2;
        if (x < kx) {
            if (kx <= px)
                return ky / t->total;
            return (py + (ky - py) * (x - px) / (kx - px)) / t->total;
        }
        px = kx;
        py = ky;
        cum += c[i].w;
    }
    if (t->max <= px)
        return 1;
    return (py + (t->total - py) * (x - px) / (t->max - px)) / t->total;
}

static void dyn_topk_swap(dyn_topk_t* t, uint32_t a, uint32_t b)
{
    uint32_t x = t->heap[a];
    t->heap[a] = t->heap[b];
    t->heap[b] = x;
    t->s[t->heap[a]].hpos = a;
    t->s[t->heap[b]].hpos = b;
}

static int dyn_topk_less(const dyn_topk_t* t, uint32_t a, uint32_t b)
{
    return t->s[t->heap[a]].count < t->s[t->heap[b]].count;
}

static void dyn_topk_down(dyn_topk_t* t, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && dyn_topk_less(t, l, m))
            m = l;
        if (r < t->n && dyn_topk_less(t, r, m))
            m = r;
        if (m == i)
            return;
        dyn_topk_swap(t, i, m);
        i = m;
    }
}

void dyn_topk_up(dyn_topk_t* t, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!dyn_topk_less(t, i, p))
            return;
        dyn_topk_swap(t, i, p);
        i = p;
    }
}

void dyn_topk_free(dyn_topk_t* t)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        DYN_DS_FREE(t->s[i].key);
    DYN_DS_FREE(t->s);
    DYN_DS_FREE(t->heap);
    dyn_ds_smap_free(&t->idx);
}

int dyn_topk_add(dyn_topk_t* t, const char* k, size_t n, double c, char** evicted, size_t* evn)
{
    uint64_t h = dyn_ds_strhash(k, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&t->idx, k, n, h);
    *evicted = NULL;
    t->total += c;
    if (e) {
        dyn_topk_slot* s = &t->s[e->v];
        s->count += c;
        dyn_topk_down(t, s->hpos);
        return 0;
    }
    if (n > UINT32_MAX - 1)
        return -1;
    if (t->n < t->k) {
        uint32_t i = t->n;
        dyn_topk_slot* s = &t->s[i];
        s->key = (char*)dyn_ds_bytes_dup(k, n);
        if (!s->key)
            return -1;
        s->klen = (uint32_t)n;
        s->count = c;
        s->err = 0;
        if (!dyn_ds_smap_insert(&t->idx, s->key, n, h, i)) {
            DYN_DS_FREE(s->key);
            return -1;
        }
        t->heap[i] = i;
        s->hpos = i;
        t->n++;
        dyn_topk_up(t, i);
        return 0;
    }
    {
        uint32_t si = t->heap[0];
        dyn_topk_slot* s = &t->s[si];
        char* nk = (char*)dyn_ds_bytes_dup(k, n);
        dyn_ds_sent* old;
        if (!nk)
            return -1;
        old = dyn_ds_smap_find(&t->idx, s->key, s->klen, dyn_ds_strhash(s->key, s->klen));
        if (old)
            dyn_ds_smap_remove(&t->idx, old);
        if (!dyn_ds_smap_insert(&t->idx, nk, n, h, si)) {
            dyn_ds_smap_insert(&t->idx, s->key, s->klen, dyn_ds_strhash(s->key, s->klen), si);
            DYN_DS_FREE(nk);
            return -1;
        }
        *evicted = s->key;
        *evn = s->klen;
        s->key = nk;
        s->klen = (uint32_t)n;
        s->err = s->count;
        s->count += c;
        dyn_topk_down(t, 0);
        return 0;
    }
}

static uint32_t dyn_cuckoo_alt(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    return (i ^ (uint32_t)(((uint64_t)fp * 0x5bd1e995u) >> 0)) & c->mask;
}

void dyn_cuckoo_hash(const dyn_cuckoo_t* c, const char* k, size_t n, uint32_t* i, uint16_t* fp)
{
    uint64_t h = dyn_ds_strhash_fixed(k, n, 0xCF);
    uint16_t f = (uint16_t)(h >> 48);
    if (!f)
        f = 1;
    *fp = f;
    *i = (uint32_t)h & c->mask;
}

static int dyn_cuckoo_put(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (!c->t[i * DYN_CUCKOO_SLOTS + s]) {
            c->t[i * DYN_CUCKOO_SLOTS + s] = fp;
            return 1;
        }
    return 0;
}

static int dyn_cuckoo_has_in(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (c->t[i * DYN_CUCKOO_SLOTS + s] == fp)
            return 1;
    return 0;
}

static int dyn_cuckoo_del_in(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (c->t[i * DYN_CUCKOO_SLOTS + s] == fp) {
            c->t[i * DYN_CUCKOO_SLOTS + s] = 0;
            return 1;
        }
    return 0;
}

int dyn_cuckoo_insert(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp), k;
    if (c->has_victim)
        return 0;
    if (dyn_cuckoo_put(c, i, fp) || dyn_cuckoo_put(c, i2, fp)) {
        c->count++;
        return 1;
    }
    i = (ds_splitmix64(&c->rng) & 1) ? i : i2;
    for (k = 0; k < DYN_CUCKOO_MAX_KICKS; k++) {
        uint32_t s = (uint32_t)(ds_splitmix64(&c->rng) % DYN_CUCKOO_SLOTS);
        uint16_t old = c->t[i * DYN_CUCKOO_SLOTS + s];
        c->t[i * DYN_CUCKOO_SLOTS + s] = fp;
        fp = old;
        i = dyn_cuckoo_alt(c, i, fp);
        if (dyn_cuckoo_put(c, i, fp)) {
            c->count++;
            return 1;
        }
    }
    c->victim_fp = fp;
    c->victim_i = i;
    c->has_victim = 1;
    c->count++;
    return 1;
}

int dyn_cuckoo_contains(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp);
    if (dyn_cuckoo_has_in(c, i, fp) || dyn_cuckoo_has_in(c, i2, fp))
        return 1;
    return c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2);
}

int dyn_cuckoo_delete(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp);
    if (dyn_cuckoo_del_in(c, i, fp) || dyn_cuckoo_del_in(c, i2, fp)) {
        c->count--;
        if (c->has_victim) {
            uint16_t vf = c->victim_fp;
            uint32_t vi = c->victim_i;
            c->has_victim = 0;
            c->count--;
            dyn_cuckoo_insert(c, vi, vf);
        }
        return 1;
    }
    if (c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2)) {
        c->has_victim = 0;
        c->count--;
        return 1;
    }
    return 0;
}

static int dyn_iheap_before(const dyn_iheap_t* q, uint32_t a, uint32_t b)
{
    const dyn_iheap_ent *x = &q->h[a], *y = &q->h[b];
    if (x->p != y->p)
        return q->max ? x->p > y->p : x->p < y->p;
    return x->id < y->id;
}

static void dyn_iheap_swap(dyn_iheap_t* q, uint32_t a, uint32_t b)
{
    dyn_iheap_ent t = q->h[a];
    dyn_ds_ient* s;
    q->h[a] = q->h[b];
    q->h[b] = t;
    s = dyn_ds_imap_find(&q->pos, q->h[a].id);
    if (s)
        s->v = a;
    s = dyn_ds_imap_find(&q->pos, q->h[b].id);
    if (s)
        s->v = b;
}

static uint32_t dyn_iheap_up(dyn_iheap_t* q, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!dyn_iheap_before(q, i, p))
            break;
        dyn_iheap_swap(q, i, p);
        i = p;
    }
    return i;
}

static void dyn_iheap_down(dyn_iheap_t* q, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < q->n && dyn_iheap_before(q, l, m))
            m = l;
        if (r < q->n && dyn_iheap_before(q, r, m))
            m = r;
        if (m == i)
            return;
        dyn_iheap_swap(q, i, m);
        i = m;
    }
}

int dyn_iheap_push(dyn_iheap_t* q, uint32_t id, double p)
{
    dyn_ds_ient* s = dyn_ds_imap_find(&q->pos, id);
    if (s) {
        uint32_t i = s->v;
        if (q->h[i].p == p)
            return 0;
        q->h[i].p = p;
        dyn_iheap_down(q, dyn_iheap_up(q, i));
        return 0;
    }
    if (q->n == q->cap) {
        uint32_t nc = q->cap ? q->cap * 2 : 16;
        dyn_iheap_ent* nh;
        if (nc > DYN_DS_MAX_ITEMS)
            return -1;
        nh = (dyn_iheap_ent*)DYN_DS_REALLOC(q->h, sizeof(dyn_iheap_ent) * nc);
        if (!nh)
            return -1;
        q->h = nh;
        q->cap = nc;
    }
    if (!dyn_ds_imap_put(&q->pos, id, q->n))
        return -1;
    q->h[q->n].p = p;
    q->h[q->n].id = id;
    q->n++;
    dyn_iheap_up(q, q->n - 1);
    return 0;
}

void dyn_iheap_remove_at(dyn_iheap_t* q, uint32_t i)
{
    dyn_ds_ient* s = dyn_ds_imap_find(&q->pos, q->h[i].id);
    uint32_t last = q->n - 1;
    if (s)
        dyn_ds_imap_remove(&q->pos, s);
    q->n--;
    if (i == last)
        return;
    q->h[i] = q->h[last];
    s = dyn_ds_imap_find(&q->pos, q->h[i].id);
    if (s)
        s->v = i;
    dyn_iheap_down(q, dyn_iheap_up(q, i));
}

static double dyn_kdtree_coord(const dyn_kdtree_t* t, uint32_t idx, uint32_t dim)
{
    return t->pts[(size_t)idx * t->d + dim];
}

static void dyn_kdtree_select(dyn_kdtree_t* t, uint32_t lo, uint32_t hi, uint32_t k, uint32_t dim)
{
    uint32_t* p = t->perm;
    while (hi - lo > 1) {
        uint32_t piv = lo + (uint32_t)(ds_splitmix64(&t->rng) % (hi - lo));
        double pv = dyn_kdtree_coord(t, p[piv], dim);
        uint32_t lt = lo, i = lo, gt = hi;
        while (i < gt) {
            double v = dyn_kdtree_coord(t, p[i], dim);
            if (v < pv) {
                uint32_t x = p[lt];
                p[lt++] = p[i];
                p[i++] = x;
            } else if (v > pv) {
                uint32_t x = p[--gt];
                p[gt] = p[i];
                p[i] = x;
            } else
                i++;
        }
        if (k < lt)
            hi = lt;
        else if (k >= gt)
            lo = gt;
        else
            return;
    }
}

static int32_t dyn_kdtree_build(dyn_kdtree_t* t, uint32_t lo, uint32_t hi)
{
    int32_t id;
    dyn_kdtree_node* nd;
    if (t->nn == t->ncap) {
        uint32_t nc = t->ncap ? t->ncap * 2 : 64;
        dyn_kdtree_node* nn = (dyn_kdtree_node*)DYN_DS_REALLOC(t->nodes, sizeof(dyn_kdtree_node) * nc);
        if (!nn)
            return -2;
        t->nodes = nn;
        t->ncap = nc;
    }
    id = (int32_t)t->nn++;
    nd = &t->nodes[id];
    nd->lo = lo;
    nd->hi = hi;
    nd->l = nd->r = -1;
    nd->dim = 0;
    nd->split = 0;
    if (hi - lo > DYN_KDTREE_LEAF) {
        uint32_t best = 0, dim, i, mid = lo + (hi - lo) / 2;
        double spread = -1;
        int32_t l, r;
        for (dim = 0; dim < t->d; dim++) {
            double mn = HUGE_VAL, mx = -HUGE_VAL;
            for (i = lo; i < hi; i++) {
                double v = dyn_kdtree_coord(t, t->perm[i], dim);
                if (v < mn)
                    mn = v;
                if (v > mx)
                    mx = v;
            }
            if (mx - mn > spread) {
                spread = mx - mn;
                best = dim;
            }
        }
        if (spread <= 0)
            return id;
        dyn_kdtree_select(t, lo, hi, mid, best);
        t->nodes[id].dim = best;
        t->nodes[id].split = dyn_kdtree_coord(t, t->perm[mid], best);
        l = dyn_kdtree_build(t, lo, mid);
        if (l == -2)
            return -2;
        r = dyn_kdtree_build(t, mid, hi);
        if (r == -2)
            return -2;
        t->nodes[id].l = l;
        t->nodes[id].r = r;
    }
    return id;
}

void dyn_kdtree_free(dyn_kdtree_t* t)
{
    DYN_DS_FREE(t->pts);
    DYN_DS_FREE(t->perm);
    DYN_DS_FREE(t->nodes);
    memset(t, 0, sizeof(*t));
}

int dyn_kdtree_init(dyn_kdtree_t* t, double* pts, uint32_t n, uint32_t d)
{
    uint32_t i;
    memset(t, 0, sizeof(*t));
    t->d = d;
    t->n = n;
    t->pts = pts;
    t->rng = DYN_DS_FIXED_SEED;
    t->perm = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)n + 1));
    if (!t->perm)
        return -1;
    for (i = 0; i < n; i++)
        t->perm[i] = i;
    if (n && dyn_kdtree_build(t, 0, n) == -2)
        return -1;
    return 0;
}

static int dyn_kdtree_hit_worse(const dyn_kdtree_hit* a, const dyn_kdtree_hit* b)
{
    return a->d2 > b->d2 || (a->d2 == b->d2 && a->i > b->i);
}

static void dyn_kdtree_knn_push(dyn_kdtree_knn_ctx* c, double d2, uint32_t idx)
{
    dyn_kdtree_hit x = { d2, idx };
    uint32_t i;
    if (c->n < c->k) {
        i = c->n++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!dyn_kdtree_hit_worse(&x, &c->h[p]))
                break;
            c->h[i] = c->h[p];
            i = p;
        }
        c->h[i] = x;
        return;
    }
    if (!dyn_kdtree_hit_worse(&c->h[0], &x))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const dyn_kdtree_hit* mv = &x;
        if (l < c->n && dyn_kdtree_hit_worse(&c->h[l], mv)) {
            m = l;
            mv = &c->h[l];
        }
        if (r < c->n && dyn_kdtree_hit_worse(&c->h[r], mv))
            m = r;
        if (m == i)
            break;
        c->h[i] = c->h[m];
        i = m;
    }
    c->h[i] = x;
}

static double dyn_kdtree_d2(const dyn_kdtree_t* t, const double* q, uint32_t idx)
{
    const double* p = &t->pts[(size_t)idx * t->d];
    double s = 0;
    uint32_t j;
    for (j = 0; j < t->d; j++) {
        double x = p[j] - q[j];
        s += x * x;
    }
    return s;
}

void dyn_kdtree_knn(dyn_kdtree_knn_ctx* c, int32_t ni)
{
    const dyn_kdtree_node* nd = &c->t->nodes[ni];
    double diff;
    int32_t first, second;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            uint32_t idx = c->t->perm[i];
            dyn_kdtree_knn_push(c, dyn_kdtree_d2(c->t, c->q, idx), idx);
        }
        return;
    }
    diff = c->q[nd->dim] - nd->split;
    first = diff < 0 ? nd->l : nd->r;
    second = diff < 0 ? nd->r : nd->l;
    dyn_kdtree_knn(c, first);
    if (c->n < c->k || diff * diff <= c->h[0].d2)
        dyn_kdtree_knn(c, second);
}

int dyn_kdtree_hit_cmp(const void* a, const void* b)
{
    const dyn_kdtree_hit *x = (const dyn_kdtree_hit*)a, *y = (const dyn_kdtree_hit*)b;
    if (x->d2 != y->d2)
        return x->d2 < y->d2 ? -1 : 1;
    return (x->i > y->i) - (x->i < y->i);
}

static void dyn_kdtree_list_add(dyn_kdtree_list* l, double d2, uint32_t i)
{
    if (l->oom)
        return;
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 32;
        dyn_kdtree_hit* nh = (dyn_kdtree_hit*)DYN_DS_REALLOC(l->h, sizeof(dyn_kdtree_hit) * nc);
        if (!nh) {
            l->oom = 1;
            return;
        }
        l->h = nh;
        l->cap = nc;
    }
    l->h[l->n].d2 = d2;
    l->h[l->n].i = i;
    l->n++;
}

void dyn_kdtree_radius(const dyn_kdtree_t* t, int32_t ni, const double* q, double r2, dyn_kdtree_list* out)
{
    const dyn_kdtree_node* nd = &t->nodes[ni];
    double diff;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            double d2 = dyn_kdtree_d2(t, q, t->perm[i]);
            if (d2 <= r2)
                dyn_kdtree_list_add(out, d2, t->perm[i]);
        }
        return;
    }
    diff = q[nd->dim] - nd->split;
    if (diff <= 0 || diff * diff <= r2)
        dyn_kdtree_radius(t, nd->l, q, r2, out);
    if (diff >= 0 || diff * diff <= r2)
        dyn_kdtree_radius(t, nd->r, q, r2, out);
}

void dyn_kdtree_box(const dyn_kdtree_t* t, int32_t ni, const double* lo, const double* hi, dyn_kdtree_list* out)
{
    const dyn_kdtree_node* nd = &t->nodes[ni];
    if (nd->l < 0) {
        uint32_t i, j;
        for (i = nd->lo; i < nd->hi; i++) {
            const double* p = &t->pts[(size_t)t->perm[i] * t->d];
            for (j = 0; j < t->d; j++)
                if (p[j] < lo[j] || p[j] > hi[j])
                    break;
            if (j == t->d)
                dyn_kdtree_list_add(out, (double)t->perm[i], t->perm[i]);
        }
        return;
    }
    if (lo[nd->dim] <= nd->split)
        dyn_kdtree_box(t, nd->l, lo, hi, out);
    if (hi[nd->dim] >= nd->split)
        dyn_kdtree_box(t, nd->r, lo, hi, out);
}

static _Thread_local const dyn_chash_t* ds_ch_sort_ctx;

static int dyn_chash_pt_cmp(const void* a, const void* b)
{
    const dyn_chash_pt *x = (const dyn_chash_pt*)a, *y = (const dyn_chash_pt*)b;
    const dyn_chash_node *nx, *ny;
    uint32_t m;
    int c;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    nx = &ds_ch_sort_ctx->nodes[x->node];
    ny = &ds_ch_sort_ctx->nodes[y->node];
    m = nx->len < ny->len ? nx->len : ny->len;
    c = m ? memcmp(nx->name, ny->name, m) : 0;
    return c ? c : (nx->len > ny->len) - (nx->len < ny->len);
}

static int dyn_chash_rebuild(dyn_chash_t* c)
{
    uint64_t total = 0;
    uint32_t i, r, k = 0;
    dyn_chash_pt* ring;
    for (i = 0; i < c->nn; i++)
        total += (uint64_t)c->replicas * c->nodes[i].weight;
    if (total > DYN_CHASH_MAX_POINTS)
        return -2;
    ring = (dyn_chash_pt*)DYN_DS_MALLOC(sizeof(dyn_chash_pt) * ((size_t)total + 1));
    if (!ring)
        return -1;
    for (i = 0; i < c->nn; i++) {
        size_t len = c->nodes[i].len;
        char* buf = (char*)DYN_DS_MALLOC(len + 16);
        if (!buf) {
            DYN_DS_FREE(ring);
            return -1;
        }
        if (len)
            memcpy(buf, c->nodes[i].name, len);
        buf[len] = '#';
        for (r = 0; r < c->replicas * c->nodes[i].weight; r++) {
            int dn = snprintf(buf + len + 1, 15, "%u", (unsigned)r);
            ring[k].h = dyn_ds_xxh64((const uint8_t*)buf, len + 1 + (size_t)dn, 0);
            ring[k].node = i;
            k++;
        }
        DYN_DS_FREE(buf);
    }
    ds_ch_sort_ctx = c;
    qsort(ring, k, sizeof(dyn_chash_pt), dyn_chash_pt_cmp);
    ds_ch_sort_ctx = NULL;
    DYN_DS_FREE(c->ring);
    c->ring = ring;
    c->nr = k;
    return 0;
}

uint32_t dyn_chash_locate(const dyn_chash_t* c, uint64_t h)
{
    uint32_t lo = 0, hi = c->nr;
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (c->ring[m].h < h)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == c->nr ? 0 : lo;
}

void dyn_chash_free(dyn_chash_t* c)
{
    uint32_t i;
    for (i = 0; i < c->nn; i++)
        DYN_DS_FREE(c->nodes[i].name);
    DYN_DS_FREE(c->nodes);
    DYN_DS_FREE(c->ring);
    dyn_ds_smap_free(&c->idx);
}

int dyn_chash_set_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight)
{
    uint64_t h = dyn_ds_strhash(name, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&c->idx, name, n, h);
    int rc;
    if (e) {
        uint32_t old = c->nodes[e->v].weight;
        c->nodes[e->v].weight = weight;
        rc = dyn_chash_rebuild(c);
        if (rc < 0)
            c->nodes[e->v].weight = old;
        return rc;
    }
    if (n > UINT32_MAX - 1)
        return -1;
    if (c->nn == c->ncap) {
        uint32_t nc = c->ncap ? c->ncap * 2 : 8;
        dyn_chash_node* nn = (dyn_chash_node*)DYN_DS_REALLOC(c->nodes, sizeof(dyn_chash_node) * nc);
        if (!nn)
            return -1;
        c->nodes = nn;
        c->ncap = nc;
    }
    c->nodes[c->nn].name = (char*)dyn_ds_bytes_dup(name, n);
    if (!c->nodes[c->nn].name)
        return -1;
    c->nodes[c->nn].len = (uint32_t)n;
    c->nodes[c->nn].weight = weight;
    if (!dyn_ds_smap_insert(&c->idx, c->nodes[c->nn].name, n, h, c->nn)) {
        DYN_DS_FREE(c->nodes[c->nn].name);
        return -1;
    }
    c->nn++;
    rc = dyn_chash_rebuild(c);
    if (rc < 0) {
        c->nn--;
        e = dyn_ds_smap_find(&c->idx, name, n, h);
        if (e)
            dyn_ds_smap_remove(&c->idx, e);
        DYN_DS_FREE(c->nodes[c->nn].name);
    }
    return rc;
}

int dyn_chash_remove_node(dyn_chash_t* c, const char* name, size_t n)
{
    uint64_t h = dyn_ds_strhash(name, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&c->idx, name, n, h);
    uint32_t i, last;
    if (!e)
        return 0;
    i = (uint32_t)e->v;
    dyn_ds_smap_remove(&c->idx, e);
    last = c->nn - 1;
    DYN_DS_FREE(c->nodes[i].name);
    if (i != last) {
        c->nodes[i] = c->nodes[last];
        e = dyn_ds_smap_find(&c->idx, c->nodes[i].name, c->nodes[i].len, dyn_ds_strhash(c->nodes[i].name, c->nodes[i].len));
        if (e)
            e->v = i;
    }
    c->nn--;
    if (dyn_chash_rebuild(c) < 0) {
        DYN_DS_FREE(c->ring);
        c->ring = NULL;
        c->nr = 0;
        return -1;
    }
    return 1;
}

static int dyn_invidx_word_byte(const unsigned char* s, size_t n, size_t i, size_t* skip)
{
    unsigned char c = s[i];
    *skip = 1;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
        return 1;
    if (c < 0x80)
        return 0;
    if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF && s[i + 1] != 0xAA && s[i + 1] != 0xB5 && s[i + 1] != 0xBA) {
        *skip = 2;
        return 0;
    }
    if (c == 0xE2 && i + 2 < n && (s[i + 1] == 0x80 || s[i + 1] == 0x81)) {
        *skip = 3;
        return 0;
    }
    if (c == 0xE3 && i + 2 < n && s[i + 1] == 0x80) {
        *skip = 3;
        return 0;
    }
    if (c == 0xEF && i + 2 < n && ((s[i + 1] == 0xBC && s[i + 2] >= 0x81 && s[i + 2] <= 0x8F) || (s[i + 1] == 0xBD && s[i + 2] >= 0x9B))) {
        *skip = 3;
        return 0;
    }
    return 1;
}

int dyn_invidx_tokenize(const char* str, size_t n, ii_tok_fn fn, void* ud)
{
    const unsigned char* s = (const unsigned char*)str;
    char buf[DYN_INVIDX_TOK_MAX];
    size_t i = 0, skip;
    while (i < n) {
        size_t len = 0;
        int trunc = 0;
        while (i < n && !dyn_invidx_word_byte(s, n, i, &skip))
            i += skip;
        while (i < n && dyn_invidx_word_byte(s, n, i, &skip)) {
            unsigned char c = s[i];
            if (len < DYN_INVIDX_TOK_MAX)
                buf[len++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            else
                trunc = 1;
            i++;
        }
        if (trunc) {
            size_t k = len;
            while (k > 0 && ((unsigned char)buf[k - 1] & 0xC0) == 0x80)
                k--;
            if (k > 0) {
                unsigned char lead = (unsigned char)buf[k - 1];
                size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                    : lead >= 0xC0                            ? 2
                                                              : 1;
                if (len - (k - 1) < need)
                    len = k - 1;
            }
        }
        if (len) {
            int rc = fn(ud, buf, len);
            if (rc)
                return rc;
        }
    }
    return 0;
}

static int dyn_invidx_is_stop(const dyn_invidx_t* x, const char* t, size_t n)
{
    return x->nstop && dyn_ds_smap_find(&x->stop, t, n, dyn_ds_strhash(t, n)) != NULL;
}

int64_t dyn_invidx_term_get(dyn_invidx_t* x, const char* t, size_t n, int create)
{
    uint64_t h = dyn_ds_strhash(t, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&x->tidx, t, n, h);
    dyn_invidx_term* tm;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (x->nterms == x->tcap) {
        uint32_t nc = x->tcap ? x->tcap * 2 : 64;
        dyn_invidx_term* nt;
        if (nc > DYN_DS_MAX_ITEMS)
            return -2;
        nt = (dyn_invidx_term*)DYN_DS_REALLOC(x->terms, sizeof(dyn_invidx_term) * nc);
        if (!nt)
            return -2;
        x->terms = nt;
        x->tcap = nc;
    }
    tm = &x->terms[x->nterms];
    memset(tm, 0, sizeof(*tm));
    tm->s = (char*)dyn_ds_bytes_dup(t, n);
    if (!tm->s)
        return -2;
    tm->len = (uint32_t)n;
    if (!dyn_ds_smap_insert(&x->tidx, tm->s, n, h, x->nterms)) {
        DYN_DS_FREE(tm->s);
        return -2;
    }
    return (int64_t)x->nterms++;
}

int dyn_invidx_tscratch(dyn_invidx_t* x)
{
    if (x->tscap < x->nterms) {
        uint32_t nc = x->tcap;
        uint32_t* a = (uint32_t*)DYN_DS_REALLOC(x->tstamp, sizeof(uint32_t) * nc);
        uint32_t* b;
        if (!a)
            return -1;
        x->tstamp = a;
        b = (uint32_t*)DYN_DS_REALLOC(x->tcount, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->tcount = b;
        memset(x->tstamp + x->tscap, 0, sizeof(uint32_t) * (nc - x->tscap));
        x->tscap = nc;
    }
    return 0;
}

static int dyn_invidx_add_tok(void* ud, const char* t, size_t n)
{
    dyn_invidx_add_ctx* c = (dyn_invidx_add_ctx*)ud;
    dyn_invidx_t* x = c->x;
    int64_t ti;
    if (dyn_invidx_is_stop(x, t, n))
        return 0;
    ti = dyn_invidx_term_get(x, t, n, 1);
    if (ti < 0 || dyn_invidx_tscratch(x) < 0) {
        c->oom = 1;
        return 1;
    }
    c->len++;
    if (x->tstamp[ti] == x->tepoch) {
        c->list[x->tcount[ti]].tf++;
        return 0;
    }
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 16;
        dyn_invidx_dt* nl = (dyn_invidx_dt*)DYN_DS_REALLOC(c->list, sizeof(dyn_invidx_dt) * nc);
        if (!nl) {
            c->oom = 1;
            return 1;
        }
        c->list = nl;
        c->cap = nc;
    }
    x->tstamp[ti] = x->tepoch;
    x->tcount[ti] = c->n;
    c->list[c->n].term = (uint32_t)ti;
    c->list[c->n].tf = 1;
    c->n++;
    return 0;
}

void dyn_invidx_bump_tepoch(dyn_invidx_t* x)
{
    if (++x->tepoch == 0) {
        if (x->tstamp)
            memset(x->tstamp, 0, sizeof(uint32_t) * x->tscap);
        x->tepoch = 1;
    }
}

static int dyn_invidx_post_push(dyn_invidx_term* tm, uint32_t slot, uint32_t tf)
{
    if (tm->n == tm->cap) {
        uint32_t nc = tm->cap ? tm->cap * 2 : 4;
        dyn_invidx_post* np = (dyn_invidx_post*)DYN_DS_REALLOC(tm->p, sizeof(dyn_invidx_post) * nc);
        if (!np)
            return -1;
        tm->p = np;
        tm->cap = nc;
    }
    tm->p[tm->n].doc = slot;
    tm->p[tm->n].tf = tf;
    tm->n++;
    return 0;
}

static void dyn_invidx_remove_slot(dyn_invidx_t* x, uint32_t slot)
{
    dyn_invidx_doc* d = &x->docs[slot];
    uint32_t i, j;
    for (i = 0; i < d->nt; i++) {
        dyn_invidx_term* tm = &x->terms[d->t[i].term];
        for (j = 0; j < tm->n; j++)
            if (tm->p[j].doc == slot) {
                tm->p[j] = tm->p[--tm->n];
                break;
            }
    }
    x->total_len -= d->len;
    DYN_DS_FREE(d->t);
    d->t = NULL;
    d->nt = 0;
    d->alive = 0;
    x->live--;
    if (x->nfree == x->fcap) {
        uint32_t nc = x->fcap ? x->fcap * 2 : 16;
        uint32_t* nf = (uint32_t*)DYN_DS_REALLOC(x->freel, sizeof(uint32_t) * nc);
        if (!nf)
            return;
        x->freel = nf;
        x->fcap = nc;
    }
    x->freel[x->nfree++] = slot;
}

int dyn_invidx_remove(dyn_invidx_t* x, uint32_t id)
{
    dyn_ds_ient* e = dyn_ds_imap_find(&x->didx, id);
    uint32_t slot;
    if (!e)
        return 0;
    slot = e->v;
    dyn_ds_imap_remove(&x->didx, e);
    dyn_invidx_remove_slot(x, slot);
    return 1;
}

static int dyn_invidx_slot_alloc(dyn_invidx_t* x, uint32_t* out)
{
    if (x->nfree) {
        *out = x->freel[--x->nfree];
        return 0;
    }
    if (x->nslots == x->dcap) {
        uint32_t nc = x->dcap ? x->dcap * 2 : 64;
        dyn_invidx_doc* nd;
        if (nc > DYN_DS_MAX_ITEMS)
            return -1;
        nd = (dyn_invidx_doc*)DYN_DS_REALLOC(x->docs, sizeof(dyn_invidx_doc) * nc);
        if (!nd)
            return -1;
        x->docs = nd;
        x->dcap = nc;
    }
    *out = x->nslots++;
    return 0;
}

int dyn_invidx_install(dyn_invidx_t* x, uint32_t id, dyn_invidx_dt* list, uint32_t nt, uint32_t len)
{
    uint32_t slot, i;
    dyn_invidx_doc* d;
    if (dyn_invidx_slot_alloc(x, &slot) < 0)
        return -1;
    if (!dyn_ds_imap_put(&x->didx, id, slot)) {
        if (x->nfree < x->fcap)
            x->freel[x->nfree++] = slot;
        else if (slot == x->nslots - 1)
            x->nslots--;
        return -1;
    }
    d = &x->docs[slot];
    d->id = id;
    d->len = len;
    d->nt = nt;
    d->t = list;
    d->alive = 1;
    x->live++;
    x->total_len += len;
    for (i = 0; i < nt; i++)
        if (dyn_invidx_post_push(&x->terms[list[i].term], slot, list[i].tf) < 0) {
            uint32_t j;
            for (j = 0; j < i; j++)
                x->terms[list[j].term].n--;
            d->nt = 0;
            d->t = NULL;
            if (dyn_ds_imap_find(&x->didx, id))
                dyn_ds_imap_remove(&x->didx, dyn_ds_imap_find(&x->didx, id));
            d->alive = 0;
            x->live--;
            x->total_len -= len;
            if (x->nfree < x->fcap)
                x->freel[x->nfree++] = slot;
            return -1;
        }
    return 0;
}

int dyn_invidx_add(dyn_invidx_t* x, uint32_t id, const char* text, size_t n)
{
    dyn_invidx_add_ctx c;
    memset(&c, 0, sizeof(c));
    c.x = x;
    dyn_invidx_remove(x, id);
    dyn_invidx_bump_tepoch(x);
    dyn_invidx_tokenize(text, n, dyn_invidx_add_tok, &c);
    if (c.oom || dyn_invidx_install(x, id, c.list, c.n, c.len) < 0) {
        DYN_DS_FREE(c.list);
        return -1;
    }
    return 0;
}

void dyn_invidx_free(dyn_invidx_t* x)
{
    uint32_t i;
    for (i = 0; i < x->nterms; i++) {
        DYN_DS_FREE(x->terms[i].s);
        DYN_DS_FREE(x->terms[i].p);
    }
    DYN_DS_FREE(x->terms);
    for (i = 0; i < x->nslots; i++)
        DYN_DS_FREE(x->docs[i].t);
    DYN_DS_FREE(x->docs);
    DYN_DS_FREE(x->freel);
    dyn_ds_smap_free(&x->tidx);
    dyn_ds_imap_free(&x->didx);
    for (i = 0; i < x->nstop; i++)
        DYN_DS_FREE(x->stopw[i]);
    DYN_DS_FREE(x->stopw);
    dyn_ds_smap_free(&x->stop);
    DYN_DS_FREE(x->acc);
    DYN_DS_FREE(x->stamp);
    DYN_DS_FREE(x->mask);
    DYN_DS_FREE(x->touched);
    DYN_DS_FREE(x->tstamp);
    DYN_DS_FREE(x->tcount);
}

int dyn_invidx_add_stop(dyn_invidx_t* x, const char* t, size_t n)
{
    char** ns;
    char* s;
    if (dyn_ds_smap_find(&x->stop, t, n, dyn_ds_strhash(t, n)))
        return 0;
    ns = (char**)DYN_DS_REALLOC(x->stopw, sizeof(char*) * (x->nstop + 1));
    if (!ns)
        return -1;
    x->stopw = ns;
    s = (char*)dyn_ds_bytes_dup(t, n);
    if (!s)
        return -1;
    if (!dyn_ds_smap_insert(&x->stop, s, n, dyn_ds_strhash(t, n), x->nstop)) {
        DYN_DS_FREE(s);
        return -1;
    }
    x->stopw[x->nstop++] = s;
    return 0;
}

int dyn_invidx_stop_tok(void* ud, const char* t, size_t n)
{
    return dyn_invidx_add_stop((dyn_invidx_t*)ud, t, n) < 0 ? -1 : 0;
}

static int dyn_invidx_q_tok(void* ud, const char* t, size_t n)
{
    dyn_invidx_q_ctx* q = (dyn_invidx_q_ctx*)ud;
    uint32_t i;
    if (dyn_invidx_is_stop(q->x, t, n))
        return 0;
    for (i = 0; i < q->n; i++)
        if (q->lens[i] == n && memcmp(q->toks[i], t, n) == 0)
            return 0;
    if (q->n == DYN_INVIDX_MAX_GROUPS)
        return 1;
    memcpy(q->toks[q->n], t, n);
    q->lens[q->n++] = (uint32_t)n;
    return 0;
}

static int dyn_invidx_scratch(dyn_invidx_t* x)
{
    if (x->scap < x->nslots) {
        uint32_t nc = x->dcap;
        double* a = (double*)DYN_DS_REALLOC(x->acc, sizeof(double) * nc);
        uint32_t *b, *c, *d;
        if (!a)
            return -1;
        x->acc = a;
        b = (uint32_t*)DYN_DS_REALLOC(x->stamp, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->stamp = b;
        c = (uint32_t*)DYN_DS_REALLOC(x->mask, sizeof(uint32_t) * nc);
        if (!c)
            return -1;
        x->mask = c;
        d = (uint32_t*)DYN_DS_REALLOC(x->touched, sizeof(uint32_t) * nc);
        if (!d)
            return -1;
        x->touched = d;
        memset(x->stamp + x->scap, 0, sizeof(uint32_t) * (nc - x->scap));
        x->scap = nc;
    }
    if (++x->epoch == 0) {
        memset(x->stamp, 0, sizeof(uint32_t) * x->scap);
        x->epoch = 1;
    }
    return 0;
}

static int dyn_invidx_hit_worse(const dyn_invidx_hit* a, const dyn_invidx_hit* b)
{
    return a->s < b->s || (a->s == b->s && a->id > b->id);
}

static int dyn_invidx_hit_cmp(const void* a, const void* b)
{
    const dyn_invidx_hit *x = (const dyn_invidx_hit*)a, *y = (const dyn_invidx_hit*)b;
    if (x->s != y->s)
        return x->s > y->s ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

static void dyn_invidx_heap_push(dyn_invidx_hit* h, uint32_t* n, uint32_t k, dyn_invidx_hit v)
{
    uint32_t i;
    if (*n < k) {
        i = (*n)++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!dyn_invidx_hit_worse(&v, &h[p]))
                break;
            h[i] = h[p];
            i = p;
        }
        h[i] = v;
        return;
    }
    if (!dyn_invidx_hit_worse(&h[0], &v))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const dyn_invidx_hit* mv = &v;
        if (l < *n && dyn_invidx_hit_worse(&h[l], mv)) {
            m = l;
            mv = &h[l];
        }
        if (r < *n && dyn_invidx_hit_worse(&h[r], mv))
            m = r;
        if (m == i)
            break;
        h[i] = h[m];
        i = m;
    }
    h[i] = v;
}

static void dyn_invidx_score_term(dyn_invidx_t* x, uint32_t ti, uint32_t g, double avg, uint32_t* ntouched)
{
    dyn_invidx_term* tm = &x->terms[ti];
    double N = x->live, df = tm->n, idf, k1 = x->k1, b = x->b;
    uint32_t j;
    if (!tm->n)
        return;
    idf = log(1 + (N - df + 0.5) / (df + 0.5));
    for (j = 0; j < tm->n; j++) {
        uint32_t slot = tm->p[j].doc;
        double tf = tm->p[j].tf, dl = x->docs[slot].len;
        double s = idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * (avg > 0 ? dl / avg : 0)));
        if (x->stamp[slot] != x->epoch) {
            x->stamp[slot] = x->epoch;
            x->acc[slot] = 0;
            x->mask[slot] = 0;
            x->touched[(*ntouched)++] = slot;
        }
        x->acc[slot] += s;
        x->mask[slot] |= 1u << g;
    }
}

int dyn_invidx_search(dyn_invidx_t* x, const char* q, size_t qn, int and_mode, int prefix, uint32_t limit, dyn_invidx_hit** out, uint32_t* nout)
{
    dyn_invidx_q_ctx* qc;
    uint32_t g, ntouched = 0, i, nh = 0, full;
    double avg;
    dyn_invidx_hit* h;
    *out = NULL;
    *nout = 0;
    qc = (dyn_invidx_q_ctx*)DYN_DS_MALLOC(sizeof(*qc));
    if (!qc)
        return -1;
    qc->x = x;
    qc->n = 0;
    dyn_invidx_tokenize(q, qn, dyn_invidx_q_tok, qc);
    if (!qc->n || !x->live || !limit) {
        DYN_DS_FREE(qc);
        return 0;
    }
    if (dyn_invidx_scratch(x) < 0) {
        DYN_DS_FREE(qc);
        return -1;
    }
    avg = (double)x->total_len / x->live;
    for (g = 0; g < qc->n; g++) {
        if (prefix && g == qc->n - 1) {
            for (i = 0; i < x->nterms; i++)
                if (x->terms[i].len >= qc->lens[g] && memcmp(x->terms[i].s, qc->toks[g], qc->lens[g]) == 0)
                    dyn_invidx_score_term(x, i, g, avg, &ntouched);
        } else {
            int64_t ti = dyn_invidx_term_get(x, qc->toks[g], qc->lens[g], 0);
            if (ti >= 0)
                dyn_invidx_score_term(x, (uint32_t)ti, g, avg, &ntouched);
        }
    }
    full = qc->n == 32 ? 0xFFFFFFFFu : (1u << qc->n) - 1;
    DYN_DS_FREE(qc);
    if (limit > ntouched)
        limit = ntouched;
    h = (dyn_invidx_hit*)DYN_DS_MALLOC(sizeof(dyn_invidx_hit) * ((size_t)limit + 1));
    if (!h)
        return -1;
    for (i = 0; i < ntouched && limit; i++) {
        uint32_t slot = x->touched[i];
        dyn_invidx_hit v;
        if (and_mode && x->mask[slot] != full)
            continue;
        v.s = x->acc[slot];
        v.id = x->docs[slot].id;
        dyn_invidx_heap_push(h, &nh, limit, v);
    }
    qsort(h, nh, sizeof(dyn_invidx_hit), dyn_invidx_hit_cmp);
    *out = h;
    *nout = nh;
    return 0;
}

static const char* dyn_interner_store(dyn_interner_t* t, const char* p, size_t n)
{
    dyn_interner_chunk* c = t->chunks;
    char* d;
    if (!c || c->cap - c->used < n + 1) {
        size_t cap = n + 1 > DYN_INTERNER_CHUNK ? n + 1 : DYN_INTERNER_CHUNK;
        dyn_interner_chunk* nc = (dyn_interner_chunk*)DYN_DS_MALLOC(sizeof(dyn_interner_chunk) + cap);
        if (!nc)
            return NULL;
        nc->used = 0;
        nc->cap = cap;
        if (c && n + 1 > DYN_INTERNER_CHUNK) {
            nc->next = c->next;
            c->next = nc;
        } else {
            nc->next = c;
            t->chunks = nc;
        }
        c = nc;
    }
    d = c->data + c->used;
    if (n)
        memcpy(d, p, n);
    d[n] = 0;
    c->used += n + 1;
    t->bytes += n + 1;
    return d;
}

int64_t dyn_interner_intern(dyn_interner_t* t, const char* p, size_t n, int create)
{
    uint64_t h = dyn_ds_strhash(p, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&t->idx, p, n, h);
    const char* d;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (n > UINT32_MAX - 1 || t->n >= 0x7FFFFFFFu)
        return -2;
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 64;
        dyn_interner_str* ns = (dyn_interner_str*)DYN_DS_REALLOC(t->s, sizeof(dyn_interner_str) * nc);
        if (!ns)
            return -2;
        t->s = ns;
        t->cap = nc;
    }
    d = dyn_interner_store(t, p, n);
    if (!d)
        return -2;
    if (!dyn_ds_smap_insert(&t->idx, d, n, h, t->n))
        return -2;
    t->s[t->n].p = d;
    t->s[t->n].len = (uint32_t)n;
    return (int64_t)t->n++;
}

void dyn_interner_free(dyn_interner_t* t)
{
    dyn_interner_chunk* c = t->chunks;
    while (c) {
        dyn_interner_chunk* nx = c->next;
        DYN_DS_FREE(c);
        c = nx;
    }
    DYN_DS_FREE(t->s);
    dyn_ds_smap_free(&t->idx);
    memset(t, 0, sizeof(*t));
}

int dyn_zsl_cmp(const dyn_zsl_node* x, double s, const char* k, uint32_t n)
{
    uint32_t m;
    int c;
    if (x->score < s)
        return -1;
    if (x->score > s)
        return 1;
    m = x->klen < n ? x->klen : n;
    c = m ? memcmp(x->key, k, m) : 0;
    if (c)
        return c;
    return (x->klen > n) - (x->klen < n);
}

static dyn_zsl_node* ds_zsl_new_node(dyn_zsl_t* s, double score, const char* k, size_t n)
{
    uint64_t r = ds_splitmix64(&s->rng);
    uint32_t lvl = 1;
    dyn_zsl_node* x;
    while ((r & 3) == 0 && lvl < DYN_ZSL_MAX) {
        lvl++;
        r >>= 2;
    }
    if (n > UINT32_MAX - 1)
        return NULL;
    x = (dyn_zsl_node*)DYN_DS_MALLOC(sizeof(dyn_zsl_node) + lvl * sizeof(dyn_zsl_level) + n + 1);
    if (!x)
        return NULL;
    x->score = score;
    memset(&x->val, 0, sizeof(x->val));
    x->back = NULL;
    x->lvl = lvl;
    x->klen = (uint32_t)n;
    x->key = (char*)&x->lv[lvl];
    if (n)
        memcpy(x->key, k, n);
    x->key[n] = 0;
    return x;
}

int dyn_zsl_init(dyn_zsl_t* s, dyn_cell_free_fn release, void* ud)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->release = release;
    s->ud = ud;
    s->level = 1;
    s->rng = ds_str_seed() ^ (uint64_t)(uintptr_t)s;
    s->head = (dyn_zsl_node*)DYN_DS_MALLOC(sizeof(dyn_zsl_node) + DYN_ZSL_MAX * sizeof(dyn_zsl_level) + 1);
    if (!s->head)
        return -1;
    memset(s->head, 0, sizeof(dyn_zsl_node));
    memset(&s->head->val, 0, sizeof(s->head->val));
    s->head->lvl = DYN_ZSL_MAX;
    s->head->key = (char*)&s->head->lv[DYN_ZSL_MAX];
    s->head->key[0] = 0;
    for (i = 0; i < DYN_ZSL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    return 0;
}

void dyn_zsl_link(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_zsl_node* update[DYN_ZSL_MAX];
    uint32_t rank[DYN_ZSL_MAX];
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        rank[i] = i == s->level - 1 ? 0 : rank[i + 1];
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0) {
            rank[i] += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        update[i] = p;
    }
    if ((int)x->lvl > s->level) {
        for (i = s->level; i < (int)x->lvl; i++) {
            rank[i] = 0;
            update[i] = s->head;
            update[i]->lv[i].span = s->len;
        }
        s->level = (int)x->lvl;
    }
    for (i = 0; i < (int)x->lvl; i++) {
        x->lv[i].fwd = update[i]->lv[i].fwd;
        update[i]->lv[i].fwd = x;
        x->lv[i].span = update[i]->lv[i].span - (rank[0] - rank[i]);
        update[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = (int)x->lvl; i < s->level; i++)
        update[i]->lv[i].span++;
    x->back = update[0] == s->head ? NULL : update[0];
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x;
    else
        s->tail = x;
    s->len++;
}

void dyn_zsl_unlink(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_zsl_node* update[DYN_ZSL_MAX];
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd != x && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0)
            p = p->lv[i].fwd;
        update[i] = p;
    }
    for (i = 0; i < s->level; i++) {
        if (update[i]->lv[i].fwd == x) {
            update[i]->lv[i].span += x->lv[i].span - 1;
            update[i]->lv[i].fwd = x->lv[i].fwd;
        } else {
            update[i]->lv[i].span -= 1;
        }
    }
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x->back;
    else
        s->tail = x->back;
    while (s->level > 1 && s->head->lv[s->level - 1].fwd == NULL)
        s->level--;
    s->len--;
}

uint32_t dyn_zsl_rank(const dyn_zsl_t* s, const dyn_zsl_node* x)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) <= 0) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (p == x)
            return r;
    }
    return 0;
}

dyn_zsl_node* dyn_zsl_at(const dyn_zsl_t* s, uint32_t rank)
{
    uint32_t t = 0;
    dyn_zsl_node* p = s->head;
    int i;
    if (rank == 0 || rank > s->len)
        return NULL;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && t + p->lv[i].span <= rank) {
            t += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (t == rank)
            return p;
    }
    return NULL;
}

dyn_zsl_node* dyn_zsl_lt(const dyn_zsl_t* s, double sc, const char* k, uint32_t n, int le, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, sc, k, n) < le) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

dyn_zsl_node* dyn_zsl_first_ge_score(const dyn_zsl_t* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score < sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r + 1;
    return p->lv[0].fwd;
}

dyn_zsl_node* dyn_zsl_last_le_score(const dyn_zsl_t* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score <= sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

dyn_zsl_node* dyn_zsl_find(const dyn_zsl_t* s, const char* k, size_t n)
{
    dyn_ds_sent* e = dyn_ds_smap_find(&s->idx, k, n, dyn_ds_strhash(k, n));
    return e ? (dyn_zsl_node*)e->v : NULL;
}

void dyn_zsl_drop(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_ds_sent* e = dyn_ds_smap_find(&s->idx, x->key, x->klen, dyn_ds_strhash(x->key, x->klen));
    if (e)
        dyn_ds_smap_remove(&s->idx, e);
    dyn_zsl_unlink(s, x);
    if (s->release)
        s->release(s->ud, &x->val);
    DYN_DS_FREE(x);
}

int dyn_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, const dyn_cell_t* val, dyn_zsl_node** out)
{
    dyn_zsl_node* x = ds_zsl_new_node(s, score, k, n);
    if (!x)
        return -1;
    if (!dyn_ds_smap_insert(&s->idx, x->key, x->klen, dyn_ds_strhash(x->key, x->klen), (uintptr_t)x)) {
        DYN_DS_FREE(x);
        return -1;
    }
    x->val = *val;
    dyn_zsl_link(s, x);
    if (out)
        *out = x;
    return 0;
}

void dyn_zsl_clear(dyn_zsl_t* s)
{
    dyn_zsl_node* x = s->head->lv[0].fwd;
    int i;
    while (x) {
        dyn_zsl_node* nx = x->lv[0].fwd;
        if (s->release)
            s->release(s->ud, &x->val);
        DYN_DS_FREE(x);
        x = nx;
    }
    for (i = 0; i < DYN_ZSL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    s->tail = NULL;
    s->len = 0;
    s->level = 1;
    dyn_ds_smap_free(&s->idx);
}

void dyn_zsl_destroy(dyn_zsl_t* s)
{
    if (!s->head)
        return;
    dyn_zsl_clear(s);
    DYN_DS_FREE(s->head);
    s->head = NULL;
}

static _Thread_local const dyn_topk_t* ds_topk_sort_ctx;

static int ds_topk_cmp_desc(const void* a, const void* b)
{
    const dyn_topk_slot* x = &ds_topk_sort_ctx->s[*(const uint32_t*)a];
    const dyn_topk_slot* y = &ds_topk_sort_ctx->s[*(const uint32_t*)b];
    uint32_t m;
    int c;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    m = x->klen < y->klen ? x->klen : y->klen;
    c = m ? memcmp(x->key, y->key, m) : 0;
    return c ? c : (x->klen > y->klen) - (x->klen < y->klen);
}

void dyn_topk_sorted(const dyn_topk_t* t, uint32_t* ord)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        ord[i] = i;
    ds_topk_sort_ctx = t;
    qsort(ord, t->n, sizeof(uint32_t), ds_topk_cmp_desc);
    ds_topk_sort_ctx = NULL;
}

int dyn_topk_init(dyn_topk_t* t, uint32_t k)
{
    memset(t, 0, sizeof(*t));
    t->k = k;
    t->s = (dyn_topk_slot*)DYN_DS_CALLOC(k ? k : 1, sizeof(dyn_topk_slot));
    t->heap = (uint32_t*)DYN_DS_CALLOC(k ? k : 1, sizeof(uint32_t));
    if (!t->s || !t->heap) {
        dyn_topk_free(t);
        return -1;
    }
    return 0;
}

int dyn_cuckoo_init(dyn_cuckoo_t* c, uint32_t nb)
{
    memset(c, 0, sizeof(*c));
    c->nb = nb;
    c->mask = nb - 1;
    c->rng = DYN_DS_FIXED_SEED;
    c->t = (uint16_t*)DYN_DS_CALLOC((size_t)nb * DYN_CUCKOO_SLOTS, sizeof(uint16_t));
    return c->t ? 0 : -1;
}

void dyn_cuckoo_free(dyn_cuckoo_t* c)
{
    DYN_DS_FREE(c->t);
    c->t = NULL;
}

void dyn_iheap_init(dyn_iheap_t* q, int max)
{
    memset(q, 0, sizeof(*q));
    q->max = max;
    dyn_ds_imap_init(&q->pos);
}

void dyn_iheap_free(dyn_iheap_t* q)
{
    DYN_DS_FREE(q->h);
    q->h = NULL;
    dyn_ds_imap_free(&q->pos);
}

void dyn_invidx_init(dyn_invidx_t* x, double k1, double b)
{
    memset(x, 0, sizeof(*x));
    x->k1 = k1;
    x->b = b;
    x->tepoch = 1;
    dyn_ds_imap_init(&x->didx);
}

void dyn_chash_init(dyn_chash_t* c, uint32_t replicas)
{
    memset(c, 0, sizeof(*c));
    c->replicas = replicas;
}

void dyn_bitset_init(dyn_bitset_t* b)
{
    b->words = NULL;
    b->nwords = 0;
}

void dyn_bitset_free(dyn_bitset_t* b)
{
    DYN_DS_FREE(b->words);
    b->words = NULL;
    b->nwords = 0;
}

int dyn_bitset_reserve(dyn_bitset_t* b, uint32_t wi)
{
    uint32_t ncap;
    uint64_t* nw;
    if (wi < b->nwords)
        return 0;
    ncap = b->nwords ? b->nwords : 4;
    while (ncap <= wi) {
        if (ncap > UINT32_MAX / 2)
            return -1;
        ncap *= 2;
    }
    nw = (uint64_t*)DYN_DS_REALLOC(b->words, (size_t)ncap * sizeof(uint64_t));
    if (!nw)
        return -1;
    memset(nw + b->nwords, 0, (size_t)(ncap - b->nwords) * sizeof(uint64_t));
    b->words = nw;
    b->nwords = ncap;
    return 0;
}

uint64_t dyn_bitset_count(const dyn_bitset_t* b)
{
    uint64_t total = 0;
    uint32_t i;
    for (i = 0; i < b->nwords; i++)
        total += (uint64_t)__builtin_popcountll(b->words[i]);
    return total;
}

int64_t dyn_bitset_next_set(const dyn_bitset_t* b, uint32_t from)
{
    uint32_t wi = from >> 6;
    uint64_t w;
    if (wi >= b->nwords)
        return -1;
    w = b->words[wi] & (~(uint64_t)0 << (from & 63));
    for (;;) {
        if (w)
            return (int64_t)wi * 64 + __builtin_ctzll(w);
        if (++wi >= b->nwords)
            return -1;
        w = b->words[wi];
    }
}

int dyn_bitset_combine(dyn_bitset_t* a, const dyn_bitset_t* o, int op)
{
    uint32_t i;
    if (op != DYN_BITSET_AND && o->nwords > a->nwords && dyn_bitset_reserve(a, o->nwords - 1) < 0)
        return -1;
    for (i = 0; i < a->nwords; i++) {
        uint64_t ow = i < o->nwords ? o->words[i] : 0;
        if (op == DYN_BITSET_AND)
            a->words[i] &= ow;
        else if (op == DYN_BITSET_OR)
            a->words[i] |= ow;
        else
            a->words[i] ^= ow;
    }
    return 0;
}

int dyn_uf_init(dyn_uf_t* u, uint32_t n)
{
    uint32_t i;
    u->n = n;
    u->sets = n;
    u->parent = n ? (uint32_t*)DYN_DS_MALLOC((size_t)n * sizeof(uint32_t)) : NULL;
    u->rank = n ? (uint8_t*)DYN_DS_CALLOC(n, 1) : NULL;
    if (n && (!u->parent || !u->rank)) {
        dyn_uf_free(u);
        return -1;
    }
    for (i = 0; i < n; i++)
        u->parent[i] = i;
    return 0;
}

void dyn_uf_free(dyn_uf_t* u)
{
    DYN_DS_FREE(u->parent);
    DYN_DS_FREE(u->rank);
    u->parent = NULL;
    u->rank = NULL;
    u->n = u->sets = 0;
}

int dyn_uf_union(dyn_uf_t* u, uint32_t x, uint32_t y)
{
    uint32_t rx = dyn_uf_find(u, x), ry = dyn_uf_find(u, y);
    if (rx == ry)
        return 0;
    if (u->rank[rx] < u->rank[ry]) {
        uint32_t t = rx;
        rx = ry;
        ry = t;
    }
    u->parent[ry] = rx;
    if (u->rank[rx] == u->rank[ry])
        u->rank[rx]++;
    u->sets--;
    return 1;
}

int dyn_uf_validate(const dyn_uf_t* u, uint32_t* nroots)
{
    uint32_t n = u->n, i, roots = 0;
    uint8_t* st = (uint8_t*)DYN_DS_CALLOC(n ? n : 1, 1);
    if (!st)
        return -1;
    for (i = 0; i < n; i++) {
        uint32_t x = i, y;
        int cyc = 0;
        while (st[x] == 0 && u->parent[x] != x) {
            st[x] = 1;
            x = u->parent[x];
        }
        if (st[x] == 1 && u->parent[x] != x)
            cyc = 1;
        y = i;
        while (st[y] == 1) {
            st[y] = cyc ? 0 : 2;
            y = u->parent[y];
        }
        if (cyc) {
            DYN_DS_FREE(st);
            return 1;
        }
    }
    for (i = 0; i < n; i++)
        if (u->parent[i] == i)
            roots++;
    DYN_DS_FREE(st);
    *nroots = roots;
    return 0;
}

int dyn_fenwick_init(dyn_fenwick_t* f, uint32_t n)
{
    f->n = n;
    f->tree = (double*)DYN_DS_CALLOC((size_t)n + 1, sizeof(double));
    return f->tree ? 0 : -1;
}

void dyn_fenwick_free(dyn_fenwick_t* f)
{
    DYN_DS_FREE(f->tree);
    f->tree = NULL;
    f->n = 0;
}

double dyn_fenwick_range(const dyn_fenwick_t* f, uint32_t lo, uint32_t hi)
{
    uint32_t idx;
    double s = 0.0;
    if (lo > hi)
        return 0.0;
    for (idx = hi + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    for (idx = lo; idx > 0; idx -= idx & (~idx + 1))
        s -= f->tree[idx];
    return s;
}

double dyn_segtree_identity(int op)
{
    if (op == DYN_SEG_MIN)
        return HUGE_VAL;
    if (op == DYN_SEG_MAX)
        return -HUGE_VAL;
    return 0.0;
}

int dyn_segtree_init(dyn_segtree_t* s, uint32_t n, int op)
{
    uint32_t i;
    double idv = dyn_segtree_identity(op);
    s->n = n;
    s->op = op;
    s->tree = (double*)DYN_DS_MALLOC((size_t)2 * n * sizeof(double));
    if (!s->tree)
        return -1;
    for (i = 0; i < 2 * n; i++)
        s->tree[i] = idv;
    return 0;
}

void dyn_segtree_free(dyn_segtree_t* s)
{
    DYN_DS_FREE(s->tree);
    s->tree = NULL;
    s->n = 0;
}

void dyn_segtree_set(dyn_segtree_t* s, uint32_t i, double v)
{
    uint32_t pos = i + s->n;
    s->tree[pos] = v;
    switch (s->op) {
    case DYN_SEG_MIN:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a < b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    case DYN_SEG_MAX:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a > b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    default:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            s->tree[pos] = s->tree[2 * pos] + s->tree[2 * pos + 1];
            if (pos == 1)
                break;
        }
        break;
    }
}

double dyn_segtree_query(const dyn_segtree_t* s, uint32_t lo, uint32_t hi)
{
    double res = dyn_segtree_identity(s->op);
    uint32_t l, r;
    if (lo > hi)
        return res;
    l = lo + s->n;
    r = hi + 1 + s->n;
    switch (s->op) {
    case DYN_SEG_MIN:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res < x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res < x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    case DYN_SEG_MAX:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res > x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res > x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    default:
        while (l < r) {
            if (l & 1)
                res += s->tree[l++];
            if (r & 1)
                res += s->tree[--r];
            l >>= 1;
            r >>= 1;
        }
        break;
    }
    return res;
}

int dyn_circbuf_init(dyn_circbuf_t* c, uint32_t cap, dyn_cell_free_fn release, void* ud)
{
    c->buf = (dyn_cell_t*)DYN_DS_MALLOC((size_t)(cap ? cap : 1) * sizeof(dyn_cell_t));
    c->cap = cap;
    c->head = c->count = 0;
    c->release = release;
    c->ud = ud;
    return c->buf ? 0 : -1;
}

void dyn_circbuf_free(dyn_circbuf_t* c)
{
    uint32_t i;
    if (c->release)
        for (i = 0; i < c->count; i++)
            c->release(c->ud, dyn_circbuf_at(c, i));
    DYN_DS_FREE(c->buf);
    c->buf = NULL;
    c->count = 0;
}

void dyn_deque_init(dyn_deque_t* d, dyn_cell_free_fn release, void* ud)
{
    d->buf = NULL;
    d->cap = d->head = d->count = 0;
    d->release = release;
    d->ud = ud;
}

void dyn_deque_free(dyn_deque_t* d)
{
    uint32_t i;
    if (d->release)
        for (i = 0; i < d->count; i++)
            d->release(d->ud, dyn_deque_at(d, i));
    DYN_DS_FREE(d->buf);
    d->buf = NULL;
    d->cap = d->head = d->count = 0;
}

static int ds_deque_resize(dyn_deque_t* d, uint32_t ncap)
{
    dyn_cell_t* nb = (dyn_cell_t*)DYN_DS_MALLOC((size_t)ncap * sizeof(dyn_cell_t));
    if (!nb)
        return -1;
    if (d->count) {
        uint32_t first = d->cap - d->head;
        if (first > d->count)
            first = d->count;
        memcpy(nb, d->buf + d->head, (size_t)first * sizeof(dyn_cell_t));
        if (first < d->count)
            memcpy(nb + first, d->buf, (size_t)(d->count - first) * sizeof(dyn_cell_t));
    }
    DYN_DS_FREE(d->buf);
    d->buf = nb;
    d->cap = ncap;
    d->head = 0;
    return 0;
}

int dyn_deque_grow(dyn_deque_t* d)
{
    if (d->count < d->cap)
        return 0;
    if (d->cap > UINT32_MAX / 2 || d->cap > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t) / 2)
        return -1;
    return ds_deque_resize(d, d->cap ? d->cap * 2 : 8);
}

void dyn_deque_shrink(dyn_deque_t* d)
{
    if (d->cap < 64 || d->count > d->cap / 4)
        return;
    (void)ds_deque_resize(d, d->cap / 2);
}

static void ds_bloom_hashes(const char* s, size_t n, uint64_t* h1, uint64_t* h2)
{
    uint64_t a = 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL;
    uint64_t c = ~a;
    size_t i;
    for (i = 0; i < n; i++) {
        a ^= (uint64_t)(unsigned char)s[i];
        a *= 1099511628211ULL;
        c = (c ^ (uint64_t)(unsigned char)s[i]) * 1099511628211ULL + 0x9E3779B9ULL;
    }
    *h1 = a;
    *h2 = c | 1;
}

static inline void ds_bloom_probe_init(uint32_t m, uint64_t h1, uint64_t h2,
    uint32_t* bit, uint32_t* step,
    uint32_t* wmod)
{
    uint64_t w;
    *bit = (uint32_t)(h1 % m);
    *step = (uint32_t)(h2 % m);
    if (!*step)
        *step = 1;
    w = (UINT64_MAX % m) + 1;
    *wmod = (uint32_t)(w == m ? 0 : w);
}

static inline uint32_t ds_bloom_probe_next(uint32_t m, uint64_t* acc,
    uint64_t h2, uint32_t bit,
    uint32_t step, uint32_t wmod)
{
    uint64_t next = *acc + h2;
    uint64_t r = (uint64_t)bit + step;
    if (r >= m)
        r -= m;
    if (next < *acc) {
        r = r + m - wmod;
        if (r >= m)
            r -= m;
    }
    *acc = next;
    return (uint32_t)r;
}

int dyn_bloom_init(dyn_bloom_t* b, uint32_t nbits, uint32_t k)
{
    b->nbits = nbits;
    b->k = k;
    b->nwords = (nbits + 63) >> 6;
    b->words = (uint64_t*)DYN_DS_CALLOC(b->nwords ? b->nwords : 1, sizeof(uint64_t));
    return b->words ? 0 : -1;
}

void dyn_bloom_free(dyn_bloom_t* b)
{
    DYN_DS_FREE(b->words);
    b->words = NULL;
}

void dyn_bloom_add(dyn_bloom_t* b, const void* key, size_t n)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_bloom_hashes((const char*)key, n, &h1, &h2);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        ds_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
            bit = ds_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
}

int dyn_bloom_may_contain(const dyn_bloom_t* b, const void* key, size_t n)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_bloom_hashes((const char*)key, n, &h1, &h2);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return 0;
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        ds_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return 0;
            bit = ds_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
    return 1;
}

static uint8_t* ds_trie_bytes(dyn_trie_t* t, const uint8_t* src, uint32_t n)
{
    dyn_trie_bytes_t* b = t->bytes;
    uint8_t* p;
    if (!n)
        return NULL;
    if (!b || b->cap - b->used < n) {
        uint32_t cap = n > DYN_TRIE_BYTES_MIN ? n : DYN_TRIE_BYTES_MIN;
        b = (dyn_trie_bytes_t*)DYN_DS_MALLOC(sizeof(*b) + cap);
        if (!b)
            return NULL;
        b->next = t->bytes;
        b->used = 0;
        b->cap = cap;
        t->bytes = b;
    }
    p = DYN_TRIE_BYTES(b) + b->used;
    memcpy(p, src, n);
    b->used += n;
    return p;
}

static dyn_trie_node_t* ds_trie_new_node(dyn_trie_t* t)
{
    dyn_trie_chunk_t* c = t->chunks;
    dyn_trie_node_t* n;
    if (!c || c->used == c->cap) {
        uint32_t cap = c ? (c->cap < DYN_TRIE_CHUNK_MAX ? c->cap * 2
                                                        : DYN_TRIE_CHUNK_MAX)
                         : 32;
        c = (dyn_trie_chunk_t*)DYN_DS_MALLOC(sizeof(*c) + (size_t)cap * sizeof(dyn_trie_node_t));
        if (!c)
            return NULL;
        c->next = t->chunks;
        c->used = 0;
        c->cap = cap;
        t->chunks = c;
    }
    n = DYN_TRIE_CHUNK_NODES(c) + c->used++;
    memset(n, 0, sizeof(*n));
    return n;
}

dyn_trie_node_t* dyn_trie_child(dyn_trie_node_t* node, uint8_t b)
{
    dyn_trie_node_t* c;
    for (c = node->child; c; c = c->sibling)
        if (c->byte == b)
            return c;
    return NULL;
}

dyn_trie_node_t* dyn_trie_walk(dyn_trie_t* t, const char* key, size_t klen,
    int* on_tail)
{
    dyn_trie_node_t* node = t->root;
    size_t i = 0;
    *on_tail = 0;
    while (i < klen) {
        if (node->tail) {
            size_t rem = klen - i;
            if (rem == node->tail_len && memcmp(node->tail, key + i, rem) == 0) {
                *on_tail = 1;
                return node;
            }
            return NULL;
        }
        node = dyn_trie_child(node, (uint8_t)key[i]);
        if (!node)
            return NULL;
        i++;
    }
    return node;
}

static int ds_trie_expand(dyn_trie_t* t, dyn_trie_node_t** pnode, uint32_t keep)
{
    dyn_trie_node_t *node = *pnode, *c;
    uint8_t* tail = node->tail;
    uint32_t tlen = node->tail_len, j;
    dyn_trie_node_t** tmp;
    uint32_t nneed = keep + (keep < tlen ? 1 : 0);

    tmp = (dyn_trie_node_t**)DYN_DS_MALLOC((size_t)nneed * sizeof(*tmp));
    if (!tmp)
        return -1;
    for (j = 0; j < nneed; j++) {
        tmp[j] = ds_trie_new_node(t);
        if (!tmp[j]) {
            DYN_DS_FREE(tmp);
            return -1;
        }
    }
    node->tail = NULL;
    node->tail_len = 0;
    for (j = 0; j < keep; j++) {
        c = tmp[j];
        c->byte = tail[j];
        c->sibling = node->child;
        node->child = c;
        node = c;
    }
    if (keep == tlen) {
        node->is_end = 1;
    } else {
        c = tmp[keep];
        c->byte = tail[keep];
        c->sibling = node->child;
        node->child = c;
        if (keep + 1 == tlen) {
            c->is_end = 1;
        } else {
            c->tail = tail + keep + 1;
            c->tail_len = tlen - keep - 1;
        }
    }
    *pnode = node;
    DYN_DS_FREE(tmp);
    return 0;
}

int dyn_trie_insert_at(dyn_trie_t* t, dyn_trie_node_t* node,
    const char* key, size_t klen, size_t from,
    dyn_trie_node_t** path, size_t* plen)
{
    size_t i = from;

    for (;;) {
        if (path)
            *plen = i;
        if (i == klen) {
            if (!node->is_end) {
                node->is_end = 1;
                t->count++;
            }
            return 0;
        }
        if (node->tail) {
            uint32_t rem = (uint32_t)(klen - i), lim, L = 0;
            lim = rem < node->tail_len ? rem : node->tail_len;
            while (L < lim && node->tail[L] == (uint8_t)key[i + L])
                L++;
            if (L == node->tail_len && L == rem)
                return 0;
            if (path) {
                dyn_trie_node_t* walk = node;
                uint32_t j;
                if (ds_trie_expand(t, &node, L) < 0)
                    return -1;
                for (j = 0; j < L; j++) {
                    walk = walk->child;
                    path[i + j] = walk;
                }
                i += L;
                continue;
            }
            if (ds_trie_expand(t, &node, L) < 0)
                return -1;
            i += L;
            continue;
        }
        if (!node->child) {
            node->tail = ds_trie_bytes(t, (const uint8_t*)key + i,
                (uint32_t)(klen - i));
            if (!node->tail)
                return -1;
            node->tail_len = (uint32_t)(klen - i);
            t->count++;
            return 0;
        }
        {
            dyn_trie_node_t* c = dyn_trie_child(node, (uint8_t)key[i]);
            if (!c) {
                c = ds_trie_new_node(t);
                if (!c)
                    return -1;
                c->byte = (uint8_t)key[i];
                c->sibling = node->child;
                node->child = c;
            }
            if (path)
                path[i] = c;
            node = c;
            i++;
        }
    }
}

int dyn_trie_insert(dyn_trie_t* t, const char* key, size_t klen)
{
    return dyn_trie_insert_at(t, t->root, key, klen, 0, NULL, NULL);
}

void dyn_trie_order_children(dyn_trie_node_t** v, size_t k)
{
    size_t i, j;
    if (k <= 32) {
        for (i = 1; i < k; i++) {
            dyn_trie_node_t* x = v[i];
            for (j = i; j && v[j - 1]->byte > x->byte; j--)
                v[j] = v[j - 1];
            v[j] = x;
        }
    } else {
        dyn_trie_node_t* slot[256];
        memset(slot, 0, sizeof(slot));
        for (i = 0; i < k; i++)
            slot[v[i]->byte] = v[i];
        for (i = 0, k = 0; i < 256; i++)
            if (slot[i])
                v[k++] = slot[i];
    }
}

int dyn_trie_init(dyn_trie_t* t)
{
    t->chunks = NULL;
    t->bytes = NULL;
    t->count = 0;
    t->root = ds_trie_new_node(t);
    return t->root ? 0 : -1;
}

void dyn_trie_free(dyn_trie_t* t)
{
    dyn_trie_chunk_t *c, *nx;
    dyn_trie_bytes_t *b, *bn;
    for (c = t->chunks; c; c = nx) {
        nx = c->next;
        DYN_DS_FREE(c);
    }
    for (b = t->bytes; b; b = bn) {
        bn = b->next;
        DYN_DS_FREE(b);
    }
    t->chunks = NULL;
    t->bytes = NULL;
    t->root = NULL;
    t->count = 0;
}

int dyn_trie_has(const dyn_trie_t* t, const char* key, size_t klen)
{
    int on_tail;
    dyn_trie_node_t* node = dyn_trie_walk(DS_UNCONST(t), key, klen, &on_tail);
    return node && (on_tail || node->is_end);
}

int dyn_trie_delete(dyn_trie_t* t, const char* key, size_t klen)
{
    int on_tail;
    dyn_trie_node_t* node = dyn_trie_walk(t, key, klen, &on_tail);
    if (node && on_tail) {
        node->tail = NULL;
        node->tail_len = 0;
        t->count--;
        return 1;
    }
    if (node && node->is_end) {
        node->is_end = 0;
        t->count--;
        return 1;
    }
    return 0;
}

size_t dyn_trie_longest_prefix(const dyn_trie_t* t, const char* s, size_t slen)
{
    dyn_trie_node_t* node = t->root;
    size_t i, best = 0;
    for (i = 0; i < slen; i++) {
        if (node->tail) {
            size_t rem = slen - i;
            if (rem >= node->tail_len && memcmp(node->tail, s + i, node->tail_len) == 0)
                best = i + node->tail_len;
            break;
        }
        node = dyn_trie_child(node, (uint8_t)s[i]);
        if (!node)
            break;
        if (node->is_end)
            best = i + 1;
    }
    if (node && node->tail && slen >= i && best < slen) {
        size_t rem = slen - i;
        if (rem == node->tail_len && memcmp(node->tail, s + i, rem) == 0)
            best = slen;
    }
    return best;
}

void dyn_heap_init(dyn_heap_t* h)
{
    h->items = NULL;
    h->count = 0;
    h->cap = 0;
}

void dyn_heap_free(dyn_heap_t* h, dyn_cell_free_fn release, void* ud)
{
    uint32_t i;
    if (release)
        for (i = 0; i < h->count; i++)
            release(ud, &h->items[i]);
    DYN_DS_FREE(h->items);
    dyn_heap_init(h);
}

int dyn_heap_reserve(dyn_heap_t* h, uint32_t need)
{
    uint32_t ncap;
    dyn_cell_t* ni;
    if (need <= h->cap)
        return 0;
    if (need > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t))
        return -1;
    ncap = h->cap ? h->cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2 || ncap > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t) / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    ni = (dyn_cell_t*)DYN_DS_REALLOC(h->items, (size_t)ncap * sizeof(dyn_cell_t));
    if (!ni)
        return -1;
    h->items = ni;
    h->cap = ncap;
    return 0;
}

void dyn_list_init(dyn_list_t* l, dyn_cell_free_fn release, void* ud)
{
    l->head = l->tail = NULL;
    l->length = 0;
    l->release = release;
    l->ud = ud;
}

void dyn_list_free(dyn_list_t* l)
{
    dyn_list_node_t *n, *next;
    for (n = l->head; n; n = next) {
        next = n->next;
        if (l->release)
            l->release(l->ud, &n->value);
        DYN_DS_FREE(n);
    }
    l->head = l->tail = NULL;
    l->length = 0;
}

int dyn_list_push_front(dyn_list_t* l, const dyn_cell_t* v)
{
    dyn_list_node_t* n = (dyn_list_node_t*)DYN_DS_MALLOC(sizeof(*n));
    if (!n)
        return -1;
    n->value = *v;
    n->prev = NULL;
    n->next = l->head;
    if (l->head)
        l->head->prev = n;
    else
        l->tail = n;
    l->head = n;
    l->length++;
    return 0;
}

int dyn_list_push_back(dyn_list_t* l, const dyn_cell_t* v)
{
    dyn_list_node_t* n = (dyn_list_node_t*)DYN_DS_MALLOC(sizeof(*n));
    if (!n)
        return -1;
    n->value = *v;
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
    l->length++;
    return 0;
}

int dyn_list_pop_front(dyn_list_t* l, dyn_cell_t* out)
{
    dyn_list_node_t* n = l->head;
    if (!n)
        return -1;
    l->head = n->next;
    if (l->head)
        l->head->prev = NULL;
    else
        l->tail = NULL;
    l->length--;
    *out = n->value;
    DYN_DS_FREE(n);
    return 0;
}

int dyn_list_pop_back(dyn_list_t* l, dyn_cell_t* out)
{
    dyn_list_node_t* n = l->tail;
    if (!n)
        return -1;
    l->tail = n->prev;
    if (l->tail)
        l->tail->next = NULL;
    else
        l->head = NULL;
    l->length--;
    *out = n->value;
    DYN_DS_FREE(n);
    return 0;
}

static uint64_t ds_lru_hash(const char* s, size_t n)
{
    uint64_t h = 14695981039346656037ULL ^ ds_str_seed();
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int dyn_lru_init(dyn_lru_t* L, uint32_t cap, dyn_cell_free_fn release, void* ud)
{
    memset(L, 0, sizeof(*L));
    L->n_buckets = 16;
    L->cap = cap;
    L->release = release;
    L->ud = ud;
    L->buckets = (dyn_lru_node_t**)DYN_DS_CALLOC(L->n_buckets, sizeof(*L->buckets));
    return L->buckets ? 0 : -1;
}

void dyn_lru_node_free(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (L->release)
        L->release(L->ud, &n->value);
    DYN_DS_FREE(n);
}

dyn_cell_t dyn_lru_node_take(dyn_lru_node_t* n)
{
    dyn_cell_t v = n->value;
    DYN_DS_FREE(n);
    return v;
}

void dyn_lru_free(dyn_lru_t* L)
{
    dyn_lru_node_t *n, *next;
    for (n = L->head; n; n = next) {
        next = n->next;
        dyn_lru_node_free(L, n);
    }
    DYN_DS_FREE(L->buckets);
    L->buckets = NULL;
    L->head = L->tail = NULL;
    L->count = 0;
}

uint64_t dyn_lru_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t dyn_lru_expiry(uint64_t ttl)
{
    uint64_t now;
    if (!ttl)
        return 0;
    now = dyn_lru_now_ms();
    if (ttl > (uint64_t)INT64_MAX - now)
        return (uint64_t)INT64_MAX;
    return now + ttl;
}

int dyn_lru_expired(const dyn_lru_node_t* n, uint64_t now)
{
    return n->expires != 0 && now >= n->expires;
}

static void ds_lru_rehash(dyn_lru_t* L, uint32_t nb)
{
    dyn_lru_node_t** nbk = (dyn_lru_node_t**)DYN_DS_CALLOC(nb, sizeof(*nbk));
    dyn_lru_node_t* n;
    if (!nbk)
        return;
    for (n = L->head; n; n = n->next) {
        uint32_t b = (uint32_t)(n->hash & (nb - 1));
        n->hnext = nbk[b];
        nbk[b] = n;
    }
    DYN_DS_FREE(L->buckets);
    L->buckets = nbk;
    L->n_buckets = nb;
}

static dyn_lru_node_t* ds_lru_find_h(const dyn_lru_t* L, const char* key, size_t klen, uint64_t h)
{
    dyn_lru_node_t* n;
    for (n = L->buckets[h & (L->n_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && n->klen == klen && !memcmp(n->key, key, klen))
            return n;
    return NULL;
}

dyn_lru_node_t* dyn_lru_find(const dyn_lru_t* L, const char* key, size_t klen)
{
    return ds_lru_find_h(L, key, klen, ds_lru_hash(key, klen));
}

static void ds_lru_unlink(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        L->head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        L->tail = n->prev;
}

static void ds_lru_push_head(dyn_lru_t* L, dyn_lru_node_t* n)
{
    n->prev = NULL;
    n->next = L->head;
    if (L->head)
        L->head->prev = n;
    else
        L->tail = n;
    L->head = n;
}

void dyn_lru_touch(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (L->head != n) {
        ds_lru_unlink(L, n);
        ds_lru_push_head(L, n);
    }
}

void dyn_lru_detach(dyn_lru_t* L, dyn_lru_node_t* n)
{
    dyn_lru_node_t** pp = &L->buckets[n->hash & (L->n_buckets - 1)];
    ds_lru_unlink(L, n);
    while (*pp && *pp != n)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = n->hnext;
    L->count--;
}

int dyn_lru_set(dyn_lru_t* L, const char* key, size_t klen, const dyn_cell_t* v, uint64_t ttl, dyn_lru_node_t** evicted)
{
    uint64_t h = ds_lru_hash(key, klen);
    dyn_lru_node_t* n = ds_lru_find_h(L, key, klen, h);
    uint32_t b;
    *evicted = NULL;
    if (n) {
        if (L->release)
            L->release(L->ud, &n->value);
        n->value = *v;
        n->expires = dyn_lru_expiry(ttl);
        dyn_lru_touch(L, n);
        return 0;
    }
    n = (dyn_lru_node_t*)DYN_DS_MALLOC(sizeof(*n) + klen + 1);
    if (!n)
        return -1;
    memcpy(n->key, key, klen);
    n->key[klen] = '\0';
    n->klen = klen;
    n->hash = h;
    n->expires = dyn_lru_expiry(ttl);
    n->value = *v;
    if (L->count + 1 > L->n_buckets && L->n_buckets < DYN_LRU_MAX_BUCKETS)
        ds_lru_rehash(L, L->n_buckets * 2);
    b = (uint32_t)(h & (L->n_buckets - 1));
    n->hnext = L->buckets[b];
    L->buckets[b] = n;
    ds_lru_push_head(L, n);
    L->count++;
    if (L->count > L->cap) {
        dyn_lru_node_t* victim = L->tail;
        dyn_lru_detach(L, victim);
        L->evictions++;
        *evicted = victim;
    }
    return 0;
}

dyn_csr_t* dyn_csr_alloc(uint32_t nodes, size_t arcs, uint32_t** row, uint32_t** col, double** w)
{
    dyn_csr_t* g;
    if (nodes == UINT32_MAX || arcs > UINT32_MAX)
        return NULL;
    g = (dyn_csr_t*)DYN_DS_CALLOC(1, sizeof(*g));
    if (!g)
        return NULL;
    g->n = nodes;
    g->m = arcs;
    g->directed = 1;
    g->row = (uint32_t*)DYN_DS_CALLOC((size_t)nodes + 1, sizeof(uint32_t));
    g->col = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (arcs ? arcs : 1));
    g->w = (double*)DYN_DS_MALLOC(sizeof(double) * (arcs ? arcs : 1));
    if (!g->row || !g->col || !g->w) {
        dyn_csr_free(g);
        return NULL;
    }
    *row = g->row;
    *col = g->col;
    *w = g->w;
    return g;
}
