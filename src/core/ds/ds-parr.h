#ifndef DS_PARR_H
#define DS_PARR_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_PARR_MAX_FIELDS 8
#define DYN_PARR_NIL UINT32_MAX
#define DYN_PARR_HNIL UINT64_MAX

enum { DYN_PARR_AUTO = 0,
    DYN_PARR_FLAT = 1,
    DYN_PARR_TABLE = 2 };

enum { DYN_PARR_GROW_AUTO = 0,
    DYN_PARR_GROW_DOUBLE = 1,
    DYN_PARR_GROW_LINEAR = 2 };

enum { DYN_PARR_REUSE_LOWEST = 0,
    DYN_PARR_REUSE_LIFO = 1 };

enum { DYN_PARR_M_GEN = 0,
    DYN_PARR_M_OCC = 1,
    DYN_PARR_M_TOMB = 2,
    DYN_PARR_M_FREE = 3,
    DYN_PARR_M_VM = 4,
    DYN_PARR_M_OPEN = 4,
    DYN_PARR_M_OPEN2 = 5,
    DYN_PARR_M_ANY = 6,
    DYN_PARR_M_COUNT = 7 };

typedef void (*dyn_parr_release_fn)(void* ud, uint32_t slot);
typedef void (*dyn_parr_move_fn)(void* ud, uint32_t from, uint32_t to);

typedef struct {
    uint32_t partition;
    uint32_t initial_capacity;
    uint32_t max_capacity;
    uint32_t flat_small_bytes;
    uint32_t grow_step;
    double compact_at;
    int32_t key_field;
    uint8_t mode;
    uint8_t growth;
    uint8_t soft_delete;
    uint8_t no_vm;
    uint8_t reuse;
    dyn_parr_release_fn on_release;
    dyn_parr_move_fn on_move;
    void* ud;
} dyn_parr_opts_t;

typedef struct {
    uint8_t* p;
    size_t cap;
    size_t res;
} dyn_parr_seg_t;

typedef struct {
    uint8_t* base[DYN_PARR_MAX_FIELDS];
    uintptr_t* pt[DYN_PARR_MAX_FIELDS];
    uint32_t* gen;
    uint64_t* occ;
    uint64_t* tomb;
    uint64_t* open;
    uint64_t* open2;
    uint64_t* any;
    uint32_t* fstack;
    uint32_t esz[DYN_PARR_MAX_FIELDS];
    uint32_t foff[DYN_PARR_MAX_FIELDS];
    uint32_t nfields;
    uint32_t pshift;
    uint32_t pmask;
    uint32_t block;
    uint32_t cap;
    uint32_t npart;
    uint32_t maxpart;
    uint32_t ptcap;
    uint32_t live;
    uint32_t tombs;
    uint32_t lo;
    uint32_t nfree;
    uint32_t hw;
    uint32_t depth;
    uint32_t* ht;
    uint32_t htcap;
    uint32_t htn;
    uint8_t flat;
    uint8_t vm_failed;
    uint8_t slow;
    uint8_t lifo;
    dyn_parr_seg_t data[DYN_PARR_MAX_FIELDS];
    dyn_parr_seg_t meta[DYN_PARR_M_COUNT];
    uint8_t* vm;
    size_t vmsize;
    dyn_parr_opts_t o;
} dyn_parr_t;

void dyn_parr_opts_default(dyn_parr_opts_t* o);

int dyn_parr_vm_supported(void);

int dyn_parr_init(dyn_parr_t* p, const dyn_parr_opts_t* o, unsigned nfields, const uint32_t* esz);

void dyn_parr_destroy(dyn_parr_t* p);

int dyn_parr_reserve(dyn_parr_t* p, uint32_t n);

uint32_t dyn_parr_alloc_slow(dyn_parr_t* p);

void dyn_parr_note_full(dyn_parr_t* p, uint32_t c, uint32_t k);

int dyn_parr_release_slow(dyn_parr_t* p, uint32_t s);

uint32_t dyn_parr_next(const dyn_parr_t* p, uint32_t from);

uint32_t dyn_parr_next_free(const dyn_parr_t* p);

int dyn_parr_is_dense(const dyn_parr_t* p);

int dyn_parr_needs_compact(const dyn_parr_t* p);

int64_t dyn_parr_compact(dyn_parr_t* p);

int dyn_parr_key_put(dyn_parr_t* p, uint32_t s);

uint32_t dyn_parr_key_find(const dyn_parr_t* p, uint64_t key);

size_t dyn_parr_bytes(const dyn_parr_t* p);

static inline void* dyn_parr_at(const dyn_parr_t* p, unsigned f, uint32_t s)
{
    if (p->flat)
        return p->base[f] + (size_t)s * p->esz[f];
    return (void*)(p->pt[f][s >> p->pshift] + (uintptr_t)s * p->esz[f]);
}

#define DYN_PARR_FLAT_REF(p, f, s, T) ((T*)(p)->base[f] + (s))
#define DYN_PARR_TABLE_REF(p, f, s, T) ((T*)((p)->pt[f][(uint32_t)(s) >> (p)->pshift] + (uintptr_t)(s) * sizeof(T)))
#define DYN_PARR_REF(p, f, s, T) ((p)->flat ? DYN_PARR_FLAT_REF(p, f, s, T) : DYN_PARR_TABLE_REF(p, f, s, T))

#define DYN_PARR_FOREACH(p, s)                                                               \
    for (uint32_t s##_w = 0, s##_nw = ((p)->cap + 63) >> 6; s##_w < s##_nw; s##_w++)         \
        for (uint64_t s##_b = (p)->occ[s##_w]; s##_b; s##_b &= s##_b - 1)                    \
            for (uint32_t s = (s##_w << 6) + (uint32_t)__builtin_ctzll(s##_b), s##_once = 1; \
                s##_once; s##_once = 0)

#define DYN_PARR_EACH(p, s, ...)                                                  \
    do {                                                                          \
        const uint64_t* s##_occ = (p)->occ;                                       \
        uint32_t s##_nw = ((p)->cap + 63) >> 6;                                   \
        for (uint32_t s##_w = 0; s##_w < s##_nw; s##_w++) {                       \
            uint64_t s##_b = s##_occ[s##_w];                                      \
            if (s##_b == ~(uint64_t)0) {                                          \
                for (uint32_t s = s##_w << 6; s < (s##_w << 6) + 64; s++) {       \
                    __VA_ARGS__                                                   \
                }                                                                 \
            } else {                                                              \
                for (; s##_b; s##_b &= s##_b - 1) {                               \
                    uint32_t s = (s##_w << 6) + (uint32_t)__builtin_ctzll(s##_b); \
                    __VA_ARGS__                                                   \
                }                                                                 \
            }                                                                     \
        }                                                                         \
    } while (0)

static inline int dyn_parr_live(const dyn_parr_t* p, uint32_t s)
{
    return s < p->cap && ((p->occ[s >> 6] >> (s & 63)) & 1);
}

static inline uint64_t dyn_parr_handle(const dyn_parr_t* p, uint32_t s)
{
    return ((uint64_t)p->gen[s] << 32) | s;
}

static inline uint32_t dyn_parr_resolve(const dyn_parr_t* p, uint64_t h)
{
    uint32_t s = (uint32_t)h, g = (uint32_t)(h >> 32);
    if (s >= p->cap || !(g & 1) || p->gen[s] != g)
        return DYN_PARR_NIL;
    return s;
}

static inline uint32_t dyn_parr_run(const dyn_parr_t* p, uint32_t from, uint32_t* end)
{
    uint32_t nw = (p->cap + 63) >> 6, w = from >> 6, s, e;
    uint64_t b, x;
    if (from >= p->cap)
        return DYN_PARR_NIL;
    b = p->occ[w] & (~(uint64_t)0 << (from & 63));
    while (!b) {
        if (++w >= nw)
            return DYN_PARR_NIL;
        b = p->occ[w];
    }
    s = (w << 6) + (uint32_t)__builtin_ctzll(b);
    x = ~(b >> (s & 63));
    e = s + (x ? (uint32_t)__builtin_ctzll(x) : 64u);
    if (e & 63) {
        *end = e;
        return s;
    }
    while (++w < nw && p->occ[w] == ~(uint64_t)0)
        e += 64;
    if (w < nw)
        e += (uint32_t)__builtin_ctzll(~p->occ[w]);
    *end = e;
    return s;
}

static inline uint64_t dyn_parr_pmask(const dyn_parr_t* p, uint32_t c)
{
    if (p->pshift >= 6)
        return ~(uint64_t)0;
    return (((uint64_t)1 << (1u << p->pshift)) - 1) << ((c << p->pshift) & 63);
}

static inline uint32_t dyn_parr_alloc(dyn_parr_t* p)
{
    uint32_t c = p->lo, w, k, n, s;
    uint64_t m, b;
    if (p->lifo) {
        if (p->nfree)
            s = p->fstack[--p->nfree];
        else if (p->hw < p->cap)
            s = p->hw++;
        else
            return dyn_parr_alloc_slow(p);
        c = s >> p->pshift;
        p->occ[s >> 6] |= (uint64_t)1 << (s & 63);
        if (!((p->any[c >> 6] >> (c & 63)) & 1))
            p->any[c >> 6] |= (uint64_t)1 << (c & 63);
        p->gen[s]++;
        p->live++;
        return s;
    }
    if (c >= p->npart || !((p->open[c >> 6] >> (c & 63)) & 1))
        return dyn_parr_alloc_slow(p);
    m = dyn_parr_pmask(p, c);
    w = (c << p->pshift) >> 6;
    n = p->pshift >= 6 ? 1u << (p->pshift - 6) : 1u;
    for (k = 0;; k++) {
        if (k == n)
            return dyn_parr_alloc_slow(p);
        b = ~(p->tomb ? p->occ[w + k] | p->tomb[w + k] : p->occ[w + k]) & m;
        if (b)
            break;
    }
    s = ((w + k) << 6) + (uint32_t)__builtin_ctzll(b);
    p->occ[w + k] |= b & (0 - b);
    if (!((p->any[c >> 6] >> (c & 63)) & 1))
        p->any[c >> 6] |= (uint64_t)1 << (c & 63);
    p->gen[s]++;
    p->live++;
    if (!(b & (b - 1)))
        dyn_parr_note_full(p, c, k);
    return s;
}

static inline int dyn_parr_release_live(dyn_parr_t* p, uint32_t s)
{
    uint32_t c = s >> p->pshift, w = s >> 6, cw = c >> 6;
    uint64_t bit = (uint64_t)1 << (s & 63), cb = (uint64_t)1 << (c & 63);
    if (p->slow || (p->occ[w] & dyn_parr_pmask(p, c)) == bit)
        return dyn_parr_release_slow(p, s);
    p->occ[w] &= ~bit;
    p->gen[s]++;
    p->live--;
    if (p->lifo) {
        p->fstack[p->nfree++] = s;
        return 0;
    }
    if (!(p->open[cw] & cb)) {
        if (!p->open[cw])
            p->open2[c >> 12] |= (uint64_t)1 << (cw & 63);
        p->open[cw] |= cb;
    }
    if (c < p->lo)
        p->lo = c;
    return 0;
}

static inline int dyn_parr_release(dyn_parr_t* p, uint32_t s)
{
    if (!dyn_parr_live(p, s))
        return -1;
    return dyn_parr_release_live(p, s);
}

static inline int dyn_parr_free(dyn_parr_t* p, uint64_t h)
{
    uint32_t s = dyn_parr_resolve(p, h);
    if (s == DYN_PARR_NIL)
        return -1;
    return dyn_parr_release_live(p, s);
}

static inline uint32_t dyn_parr_count(const dyn_parr_t* p) { return p->live; }

static inline uint32_t dyn_parr_capacity(const dyn_parr_t* p) { return p->cap; }

#ifdef __cplusplus
}
#endif

#endif
