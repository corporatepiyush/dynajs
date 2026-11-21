#ifndef DS_INTERNAL_H
#define DS_INTERNAL_H

#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "ds-bitset.h"
#include "ds-btree.h"
#include "ds-cell.h"
#include "ds-common.h"
#include "ds-range-set.h"
#include "ds-rstar-tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#ifndef DYN_DS_MALLOC
#define DYN_DS_HOOKS 1
void* ds_hook_malloc(size_t n);
void* ds_hook_realloc(void* p, size_t n);
void ds_hook_free(void* p);
void* ds_hook_calloc(size_t n, size_t s);
#define DYN_DS_MALLOC(n) ds_hook_malloc(n)
#define DYN_DS_CALLOC(n, s) ds_hook_calloc(n, s)
#define DYN_DS_REALLOC(p, n) ds_hook_realloc(p, n)
#define DYN_DS_FREE(p) ds_hook_free(p)
#endif

#define DS_UNCONST(p) ((void*)(uintptr_t)(p))

#if defined(__has_attribute)
#if __has_attribute(uninitialized)
#define DS_SCRATCH __attribute__((uninitialized))
#endif
#endif

#ifndef DS_SCRATCH
#define DS_SCRATCH
#endif

#define DS_XXH_P1 0x9E3779B185EBCA87ULL
#define DS_XXH_P2 0xC2B2AE3D27D4EB4FULL
#define DS_XXH_P4 0x85EBCA77C2B2AE63ULL

static inline uint64_t ds_rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

uint64_t ds_xxh_round(uint64_t acc, uint64_t input);

uint64_t ds_xxh_merge(uint64_t acc, uint64_t val);

uint64_t dyn_ds_xxh64(const void* data, size_t len, uint64_t seed);

static inline uint64_t ds_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

#define DS_MIN_CAP 8u

static inline void* ds_calloc(size_t n, size_t sz)
{
    if (n == 0)
        n = 1;
    return DYN_DS_CALLOC(n, sz);
}

static inline uint64_t ds_u64_sat_add(uint64_t a, uint64_t b)
{
    uint64_t s = a + b;
    return s < a ? UINT64_MAX : s;
}

int ds_reserve(void** p, uint32_t* cap, uint32_t need, size_t esz);

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

extern atomic_uint_least64_t ds_seed0;

extern atomic_uint_least64_t ds_seed1;

uint64_t ds_ix_hash(const char* k, size_t n);

void ds_ix_free(dyn_ix* ix);

uint32_t ds_ix_find(const dyn_ix* ix, uint64_t h, const char* k, size_t n);

int ds_ix_reserve(dyn_ix* ix);

int ds_ix_insert(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx);

void ds_ix_erase(dyn_ix* ix, uint32_t pos);

void ds_ix_move(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx);

typedef struct {
    char* p;
    size_t n;
    uint64_t h;
} ds_key;

static inline int ds_key_init(ds_key* dst, const char* k, size_t n, uint64_t h)
{
    dst->p = (char*)DYN_DS_MALLOC(n ? n : 1);
    if (!dst->p)
        return -1;
    memcpy(dst->p, k, n);
    dst->n = n;
    dst->h = h;
    return 0;
}

#define DS_CV_CAP 128u

typedef struct {
    char* p;
    uint32_t n, start;
} ds_cv_chunk;

typedef struct {
    ds_cv_chunk* c;
    uint32_t nch, cch, len, hint;
} ds_cvec;

static inline uint32_t ds_cv_chunk_of(const ds_cvec* v, uint32_t i)
{
    uint32_t a = 0, b = v->nch, h = v->hint;
    if (h < v->nch) {
        if (i - v->c[h].start < v->c[h].n)
            return h;
        if (h + 1 < v->nch && i - v->c[h + 1].start < v->c[h + 1].n) {
            ((ds_cvec*)(uintptr_t)v)->hint = h + 1;
            return h + 1;
        }
    }
    while (b - a > 1) {
        uint32_t mid = a + (b - a) / 2;
        if (v->c[mid].start <= i)
            a = mid;
        else
            b = mid;
    }
    ((ds_cvec*)(uintptr_t)v)->hint = a;
    return a;
}

static inline void* ds_cv_at(const ds_cvec* v, size_t esz, uint32_t i)
{
    const ds_cv_chunk* c = &v->c[ds_cv_chunk_of(v, i)];
    return c->p + (size_t)(i - c->start) * esz;
}

int ds_cv_splice(ds_cvec* v, size_t esz, uint32_t at, uint32_t ndel,
    const void* ins, uint32_t nins);

uint32_t ds_cv_first_hi(const ds_cvec* v, size_t esz, double x, int strict);

void ds_cv_free(ds_cvec* v);

void ds_cv_clear(ds_cvec* v);

typedef struct {
    double lo, hi;
} ds_span;

struct dyn_rset {
    ds_cvec g;
};

uint32_t ds_rs_first_hi(const dyn_rset_t* s, double x, int strict);

#define DYN_HLL_NACC 8

_Static_assert((DYN_HLL_NACC & (DYN_HLL_NACC - 1)) == 0,
    "NACC must be a power of two: the bound is m & ~(NACC-1), and a "
    "non-power rounds UP, reading past the registers");

static inline int ds_ops_cmp(const dyn_ds_ops* o, const void* a, const void* b)
{
    return o->cmp ? o->cmp(a, b, o->ud) : memcmp(a, b, o->size);
}

static inline int ds_ops_cmp_f64_fast(const dyn_ds_ops* o, const void* a, const void* b)
{
    if (o->cmp == dyn_ds_cmp_f64) {
        double x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        return x < y ? -1 : x > y;
    }
    return ds_ops_cmp(o, a, b);
}

static inline void ds_ops_copy(const dyn_ds_ops* o, void* dst, const void* src)
{
    if (o->copy)
        o->copy(dst, src, o->ud);
    else if (o->size)
        memcpy(dst, src, o->size);
}

static inline void ds_ops_release(const dyn_ds_ops* o, void* e)
{
    if (o->release)
        o->release(e, o->ud);
}

static inline void ds_ops_init(dyn_ds_ops* dst, const dyn_ds_ops* src)
{
    if (src)
        *dst = *src;
    else
        memset(dst, 0, sizeof(*dst));
}

static inline size_t ds_align8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

typedef struct {
    double d;
    uint32_t v;
} ds_heap_item;

void ds_heap_push(ds_heap_item* h, size_t* n, ds_heap_item it);

ds_heap_item ds_heap_pop(ds_heap_item* h, size_t* n);

uint32_t ds_uf_find(uint32_t* p, uint32_t x);

#define DS_RS_M 16

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

typedef struct {
    const dyn_rstar_t* t;
    unsigned axis;
    int by_hi;
} ds_rs_sortctx;

#ifndef DYN_DS_NO_THREADS
#include <pthread.h>
#include <time.h>
#include <errno.h>
#endif

static inline uint64_t ds_splitmix64(uint64_t* x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline uint64_t ds_str_seed(void)
{
    return atomic_load_explicit(&ds_seed0, memory_order_relaxed);
}

uint64_t dyn_ds_strhash(const char* p, size_t n);

void dyn_ds_smap_free(dyn_ds_smap* m);

dyn_ds_sent* dyn_ds_smap_find(const dyn_ds_smap* m, const char* k, size_t n, uint64_t h);

dyn_ds_sent* dyn_ds_smap_insert(dyn_ds_smap* m, const char* k, size_t n, uint64_t h, uintptr_t v);

void dyn_ds_smap_remove(dyn_ds_smap* m, dyn_ds_sent* s);

void dyn_ds_imap_init(dyn_ds_imap* m);

void dyn_ds_imap_free(dyn_ds_imap* m);

dyn_ds_ient* dyn_ds_imap_find(const dyn_ds_imap* m, uint32_t k);

dyn_ds_ient* dyn_ds_imap_put(dyn_ds_imap* m, uint32_t k, uint32_t v);

void dyn_ds_imap_remove(dyn_ds_imap* m, dyn_ds_ient* s);

uint8_t* dyn_ds_bytes_dup(const char* p, size_t n);

#endif
