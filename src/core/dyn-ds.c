/*
 * dyn-ds -- see dyn-ds.h for the ownership contract and why there is no
 * per-element vtable.
 *
 * Every string-keyed container shares one open-addressing index (`dyn_ix`) over
 * a dense record array. Linear probing with backward-shift deletion, so there
 * are no tombstones to accumulate and no "deleted" state to get wrong. The
 * index borrows the key pointer from the dense record: removals move a record
 * with memmove/struct assignment, which does not move the key BUFFER, so only
 * the stored dense index needs fixing up.
 */
#include "dyn-ds.h"
#include "dyn-hash.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DS_MIN_CAP 8u

static void *ds_calloc(size_t n, size_t sz)
{
    if (n == 0)
        n = 1;
    return calloc(n, sz);
}

/* Grow a dense array to hold at least `need` elements of `esz`. */
static int ds_reserve(void **p, uint32_t *cap, uint32_t need, size_t esz)
{
    uint32_t nc;
    void *np;
    if (need <= *cap)
        return 0;
    nc = *cap ? *cap * 2 : DS_MIN_CAP;
    while (nc < need) {
        if (nc > 0x40000000u)
            return -1;
        nc *= 2;
    }
    np = realloc(*p, (size_t)nc * esz);
    if (!np)
        return -1;
    *p = np;
    *cap = nc;
    return 0;
}

/* ===================================================================== *
 *  Shared byte-string index
 * ===================================================================== */

typedef struct {
    uint64_t h;        /* 0 marks an empty slot */
    const char *key;   /* borrowed from the owner's dense record */
    size_t klen;
    uint32_t idx;      /* position in the owner's dense array */
} dyn_ixslot;

typedef struct {
    dyn_ixslot *slot;
    uint32_t cap;      /* power of two, or 0 when unallocated */
    uint32_t used;
} dyn_ix;

/* Index hash. NOT dyn_xxh64: that is a published algorithm whose output is an
 * API, and this value never leaves the process -- it is not written by any
 * codec and not compared across builds, so it is free to be whatever hashes
 * best. Container keys are overwhelmingly short (identifiers, field names,
 * small integers formatted as text), and xxh64's per-call setup and
 * finalisation dominate at that length: measured at 3.79 ns, which was 39% of
 * a whole lookup.
 *
 * Short inputs are folded with one 64x64->128 multiply. The 4..16 byte case
 * reads the first and last words, which overlap for n < 16 -- that covers
 * every length in one shape with no loop and no branch per byte. Longer keys
 * keep xxh64, where its schedule amortises and its quality is worth having.
 *
 * Byte order deliberately unhandled: the value is in-memory only, so a
 * big-endian host simply gets a different (equally good) hash. */
#define IX_SECRET0 0x9E3779B97F4A7C15ULL
#define IX_SECRET1 0xC2B2AE3D27D4EB4FULL

static inline uint64_t ix_mix(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    /* 32x32 partial products; same fold, no 128-bit type needed. */
    uint64_t lo = (a & 0xFFFFFFFFu) * (b & 0xFFFFFFFFu);
    uint64_t hi = (a >> 32) * (b >> 32);
    uint64_t m1 = (a >> 32) * (b & 0xFFFFFFFFu);
    uint64_t m2 = (a & 0xFFFFFFFFu) * (b >> 32);
    return (lo + (m1 << 32) + (m2 << 32)) ^ (hi + (m1 >> 32) + (m2 >> 32));
#endif
}

static uint64_t ix_hash(const char *k, size_t n)
{
    const uint8_t *p = (const uint8_t *)k;
    uint64_t a, b, h;

    if (n >= 16) {
        h = dyn_xxh64(p, n, 0);
        return h ? h : 1;
    }
    if (n >= 8) {
        memcpy(&a, p, 8);
        memcpy(&b, p + n - 8, 8);       /* overlaps a when n < 16 */
    } else if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, p + n - 4, 4);
        a = x; b = y;
    } else if (n) {
        /* first, middle and last byte: distinguishes all 1..3 byte strings */
        a = ((uint64_t)p[0] << 16) | ((uint64_t)p[n >> 1] << 8) | p[n - 1];
        b = 0;
    } else {
        a = b = 0;
    }
    h = ix_mix(a ^ IX_SECRET0, b ^ IX_SECRET1 ^ (uint64_t)n);
    return h ? h : 1;   /* 0 is the empty marker */
}

static void ix_free(dyn_ix *ix)
{
    free(ix->slot);
    ix->slot = NULL;
    ix->cap = ix->used = 0;
}

static void ix_put_raw(dyn_ixslot *slot, uint32_t mask, uint64_t h,
                       const char *k, size_t n, uint32_t idx)
{
    uint32_t i = (uint32_t)h & mask;
    while (slot[i].h)
        i = (i + 1) & mask;
    slot[i].h = h;
    slot[i].key = k;
    slot[i].klen = n;
    slot[i].idx = idx;
}

static int ix_grow(dyn_ix *ix)
{
    uint32_t nc = ix->cap ? ix->cap * 2 : DS_MIN_CAP;
    dyn_ixslot *ns = (dyn_ixslot *)ds_calloc(nc, sizeof(*ns));
    uint32_t i;
    if (!ns)
        return -1;
    for (i = 0; i < ix->cap; i++)
        if (ix->slot[i].h)
            ix_put_raw(ns, nc - 1, ix->slot[i].h, ix->slot[i].key,
                       ix->slot[i].klen, ix->slot[i].idx);
    free(ix->slot);
    ix->slot = ns;
    ix->cap = nc;
    return 0;
}

/* Slot position of `k`, or UINT32_MAX. */
static uint32_t ix_find(const dyn_ix *ix, uint64_t h, const char *k, size_t n)
{
    uint32_t mask, i;
    if (!ix->cap)
        return UINT32_MAX;
    mask = ix->cap - 1;
    i = (uint32_t)h & mask;
    while (ix->slot[i].h) {
        if (ix->slot[i].h == h && ix->slot[i].klen == n &&
            memcmp(ix->slot[i].key, k, n) == 0)
            return i;
        i = (i + 1) & mask;
    }
    return UINT32_MAX;
}

/* Make room for one more entry. Separated from ix_insert so a caller that must
 * erase-then-insert can take the only failing step FIRST and leave the
 * container untouched on OOM. */
static int ix_reserve(dyn_ix *ix)
{
    if ((ix->used + 1) * 10 >= ix->cap * 7)
        return ix_grow(ix);
    return 0;
}

static int ix_insert(dyn_ix *ix, uint64_t h, const char *k, size_t n,
                     uint32_t idx)
{
    if (ix_reserve(ix) < 0)
        return -1;
    ix_put_raw(ix->slot, ix->cap - 1, h, k, n, idx);
    ix->used++;
    return 0;
}

/* Backward-shift deletion: no tombstones, so probe chains stay short. */
static void ix_erase(dyn_ix *ix, uint32_t pos)
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
        /* Is k cyclically within (i, j]? If not, slot j may move to i. */
        if ((j > i) ? (k <= i || k > j) : (k <= i && k > j)) {
            ix->slot[i] = ix->slot[j];
            ix->slot[j].h = 0;
            i = j;
        }
    }
}

/* Repoint the entry for `k` at a new dense index (after a swap-with-last). */
static void ix_move(dyn_ix *ix, uint64_t h, const char *k, size_t n,
                    uint32_t idx)
{
    uint32_t pos = ix_find(ix, h, k, n);
    if (pos != UINT32_MAX)
        ix->slot[pos].idx = idx;
}

/* A dense record's key: owned buffer plus its hash, so removal can re-find it
 * in the index without recomputing. */
typedef struct {
    char *p;
    size_t n;
    uint64_t h;
} ds_key;

static int key_init(ds_key *dst, const char *k, size_t n, uint64_t h)
{
    dst->p = (char *)malloc(n ? n : 1);
    if (!dst->p)
        return -1;
    memcpy(dst->p, k, n);
    dst->n = n;
    dst->h = h;
    return 0;
}

/* ===================================================================== *
 *  Multiset
 * ===================================================================== */

typedef struct {
    ds_key key;
    uint64_t count;
} mset_rec;

struct dyn_mset {
    mset_rec *rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mset_t *dyn_mset_new(void)
{
    dyn_mset_t *m = (dyn_mset_t *)ds_calloc(1, sizeof(*m));
    return m;
}

void dyn_mset_clear(dyn_mset_t *m)
{
    uint32_t i;
    for (i = 0; i < m->len; i++)
        free(m->rec[i].key.p);
    m->len = 0;
    m->total = 0;
    ix_free(&m->ix);
}

void dyn_mset_free(dyn_mset_t *m)
{
    if (!m)
        return;
    dyn_mset_clear(m);
    free(m->rec);
    free(m);
}

/* Drop dense record `i`, filling the hole from the end. */
static void mset_erase(dyn_mset_t *m, uint32_t i, uint32_t pos)
{
    ix_erase(&m->ix, pos);
    free(m->rec[i].key.p);
    m->len--;
    if (i != m->len) {
        m->rec[i] = m->rec[m->len];
        ix_move(&m->ix, m->rec[i].key.h, m->rec[i].key.p, m->rec[i].key.n, i);
    }
}

int dyn_mset_add(dyn_mset_t *m, const char *k, size_t n, int64_t delta,
                 uint64_t *out)
{
    uint64_t h = ix_hash(k, n);
    uint32_t pos = ix_find(&m->ix, h, k, n);
    uint64_t cur = 0;

    if (pos != UINT32_MAX)
        cur = m->rec[m->ix.slot[pos].idx].count;

    if (delta < 0) {
        uint64_t sub = (uint64_t)(-(delta + 1)) + 1;   /* no INT64_MIN UB */
        uint64_t nv = sub >= cur ? 0 : cur - sub;
        m->total -= cur - nv;
        if (pos != UINT32_MAX) {
            uint32_t idx = m->ix.slot[pos].idx;
            if (nv == 0)
                mset_erase(m, idx, pos);
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
        m->rec[m->ix.slot[pos].idx].count = cur + (uint64_t)delta;
        m->total += (uint64_t)delta;
        if (out)
            *out = cur + (uint64_t)delta;
        return 0;
    }
    if (ds_reserve((void **)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = (uint64_t)delta;
    if (ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        free(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total += (uint64_t)delta;
    if (out)
        *out = (uint64_t)delta;
    return 0;
}

int dyn_mset_set_count(dyn_mset_t *m, const char *k, size_t n, uint64_t c)
{
    uint64_t h = ix_hash(k, n);
    uint32_t pos = ix_find(&m->ix, h, k, n);
    if (pos != UINT32_MAX) {
        uint32_t idx = m->ix.slot[pos].idx;
        m->total = m->total - m->rec[idx].count + c;
        if (c == 0)
            mset_erase(m, idx, pos);
        else
            m->rec[idx].count = c;
        return 0;
    }
    if (c == 0)
        return 0;
    if (ds_reserve((void **)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = c;
    if (ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        free(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total += c;
    return 0;
}

uint64_t dyn_mset_count(const dyn_mset_t *m, const char *k, size_t n)
{
    uint32_t pos = ix_find(&m->ix, ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? 0 : m->rec[m->ix.slot[pos].idx].count;
}

uint64_t dyn_mset_total(const dyn_mset_t *m) { return m->total; }
uint32_t dyn_mset_distinct(const dyn_mset_t *m) { return m->len; }

int dyn_mset_at(const dyn_mset_t *m, uint32_t i, const char **k, size_t *n,
                uint64_t *count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].count;
    return 1;
}

/* ===================================================================== *
 *  Multimap
 * ===================================================================== */

typedef struct {
    ds_key key;
    dyn_cell_t *val;
    uint32_t len, cap;
} mmap_rec;

struct dyn_mmap {
    mmap_rec *rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mmap_t *dyn_mmap_new(void)
{
    return (dyn_mmap_t *)ds_calloc(1, sizeof(dyn_mmap_t));
}

void dyn_mmap_free(dyn_mmap_t *m, dyn_cell_free_fn fn, void *ud)
{
    uint32_t i, j;
    if (!m)
        return;
    for (i = 0; i < m->len; i++) {
        if (fn)
            for (j = 0; j < m->rec[i].len; j++)
                fn(ud, &m->rec[i].val[j]);
        free(m->rec[i].val);
        free(m->rec[i].key.p);
    }
    free(m->rec);
    ix_free(&m->ix);
    free(m);
}

static mmap_rec *mmap_lookup(const dyn_mmap_t *m, const char *k, size_t n)
{
    uint32_t pos = ix_find(&m->ix, ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? NULL : &m->rec[m->ix.slot[pos].idx];
}

int dyn_mmap_put(dyn_mmap_t *m, const char *k, size_t n, const dyn_cell_t *v)
{
    uint64_t h = ix_hash(k, n);
    uint32_t pos = ix_find(&m->ix, h, k, n);
    mmap_rec *r;

    if (pos == UINT32_MAX) {
        if (ds_reserve((void **)&m->rec, &m->cap, m->len + 1,
                       sizeof(*m->rec)) < 0)
            return -1;
        r = &m->rec[m->len];
        memset(r, 0, sizeof(*r));
        if (key_init(&r->key, k, n, h) < 0)
            return -1;
        if (ix_insert(&m->ix, h, r->key.p, n, m->len) < 0) {
            free(r->key.p);
            return -1;
        }
        m->len++;
    } else {
        r = &m->rec[m->ix.slot[pos].idx];
    }
    if (ds_reserve((void **)&r->val, &r->cap, r->len + 1, sizeof(*r->val)) < 0)
        return -1;
    r->val[r->len++] = *v;
    m->total++;
    return 0;
}

uint32_t dyn_mmap_count(const dyn_mmap_t *m, const char *k, size_t n)
{
    mmap_rec *r = mmap_lookup(m, k, n);
    return r ? r->len : 0;
}

const dyn_cell_t *dyn_mmap_at(const dyn_mmap_t *m, const char *k, size_t n,
                              uint32_t i)
{
    mmap_rec *r = mmap_lookup(m, k, n);
    return (r && i < r->len) ? &r->val[i] : NULL;
}

/* Drop dense record `idx`, whose index entry sits at `pos`. */
static void mmap_erase(dyn_mmap_t *m, uint32_t idx, uint32_t pos)
{
    ix_erase(&m->ix, pos);
    free(m->rec[idx].val);
    free(m->rec[idx].key.p);
    m->len--;
    if (idx != m->len) {
        m->rec[idx] = m->rec[m->len];
        ix_move(&m->ix, m->rec[idx].key.h, m->rec[idx].key.p,
                m->rec[idx].key.n, idx);
    }
}

int dyn_mmap_remove_at(dyn_mmap_t *m, const char *k, size_t n, uint32_t i,
                       dyn_cell_t *out)
{
    uint64_t h = ix_hash(k, n);
    uint32_t pos = ix_find(&m->ix, h, k, n), idx;
    mmap_rec *r;

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
        mmap_erase(m, idx, pos);
    return 1;
}

uint32_t dyn_mmap_remove_key(dyn_mmap_t *m, const char *k, size_t n,
                             dyn_cell_free_fn fn, void *ud)
{
    uint64_t h = ix_hash(k, n);
    uint32_t pos = ix_find(&m->ix, h, k, n), idx, i, cnt;

    if (pos == UINT32_MAX)
        return 0;
    idx = m->ix.slot[pos].idx;
    cnt = m->rec[idx].len;
    if (fn)
        for (i = 0; i < cnt; i++)
            fn(ud, &m->rec[idx].val[i]);
    m->total -= cnt;
    mmap_erase(m, idx, pos);
    return cnt;
}

uint32_t dyn_mmap_keys(const dyn_mmap_t *m) { return m->len; }
uint64_t dyn_mmap_size(const dyn_mmap_t *m) { return m->total; }

int dyn_mmap_key_at(const dyn_mmap_t *m, uint32_t i, const char **k, size_t *n,
                    uint32_t *count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].len;
    return 1;
}

const dyn_cell_t *dyn_mmap_cells_at(const dyn_mmap_t *m, uint32_t i,
                                    uint32_t *count)
{
    if (i >= m->len) {
        *count = 0;
        return NULL;
    }
    *count = m->rec[i].len;
    return m->rec[i].val;
}

const dyn_cell_t *dyn_mmap_cells_for(const dyn_mmap_t *m, const char *k,
                                     size_t n, uint32_t *count)
{
    mmap_rec *r = mmap_lookup(m, k, n);
    *count = r ? r->len : 0;
    return r ? r->val : NULL;
}

/* ===================================================================== *
 *  BiMap
 * ===================================================================== */

typedef struct {
    ds_key key;
    ds_key val;
} bimap_rec;

struct dyn_bimap {
    bimap_rec *rec;
    uint32_t len, cap;
    dyn_ix fwd;   /* key -> record */
    dyn_ix rev;   /* value -> record */
};

dyn_bimap_t *dyn_bimap_new(void)
{
    return (dyn_bimap_t *)ds_calloc(1, sizeof(dyn_bimap_t));
}

void dyn_bimap_clear(dyn_bimap_t *b)
{
    uint32_t i;
    for (i = 0; i < b->len; i++) {
        free(b->rec[i].key.p);
        free(b->rec[i].val.p);
    }
    b->len = 0;
    ix_free(&b->fwd);
    ix_free(&b->rev);
}

void dyn_bimap_free(dyn_bimap_t *b)
{
    if (!b)
        return;
    dyn_bimap_clear(b);
    free(b->rec);
    free(b);
}

static void bimap_erase(dyn_bimap_t *b, uint32_t idx)
{
    uint32_t fp = ix_find(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
                          b->rec[idx].key.n);
    uint32_t rp = ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
                          b->rec[idx].val.n);
    if (fp != UINT32_MAX)
        ix_erase(&b->fwd, fp);
    if (rp != UINT32_MAX)
        ix_erase(&b->rev, rp);
    free(b->rec[idx].key.p);
    free(b->rec[idx].val.p);
    b->len--;
    if (idx != b->len) {
        b->rec[idx] = b->rec[b->len];
        ix_move(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
                b->rec[idx].key.n, idx);
        ix_move(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
                b->rec[idx].val.n, idx);
    }
}

int dyn_bimap_put(dyn_bimap_t *b, const char *k, size_t kn,
                  const char *v, size_t vn, int force)
{
    uint64_t kh = ix_hash(k, kn), vh = ix_hash(v, vn);
    uint32_t kp = ix_find(&b->fwd, kh, k, kn);
    uint32_t vp = ix_find(&b->rev, vh, v, vn);
    bimap_rec *r;

    if (kp != UINT32_MAX && vp != UINT32_MAX &&
        b->fwd.slot[kp].idx == b->rev.slot[vp].idx)
        return 0;                                  /* the pair already holds */
    if (vp != UINT32_MAX) {
        if (!force)
            return DYN_BIMAP_VALUE_TAKEN;
        bimap_erase(b, b->rev.slot[vp].idx);
        kp = ix_find(&b->fwd, kh, k, kn);          /* indices may have moved */
    }
    if (kp != UINT32_MAX) {
        /* Rebind an existing key to a new value: the old value's reverse entry
         * goes away, the record keeps its dense slot. */
        uint32_t idx = b->fwd.slot[kp].idx;
        uint32_t op = ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
                              b->rec[idx].val.n);
        ds_key nv;
        /* Both failing steps happen before anything is destroyed. */
        if (ix_reserve(&b->rev) < 0)
            return -1;
        if (key_init(&nv, v, vn, vh) < 0)
            return -1;
        op = ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
                     b->rec[idx].val.n);   /* ix_reserve may have rehashed */
        if (op != UINT32_MAX)
            ix_erase(&b->rev, op);
        free(b->rec[idx].val.p);
        b->rec[idx].val = nv;
        ix_insert(&b->rev, vh, nv.p, vn, idx);
        return 0;
    }
    if (ds_reserve((void **)&b->rec, &b->cap, b->len + 1, sizeof(*b->rec)) < 0)
        return -1;
    r = &b->rec[b->len];
    if (key_init(&r->key, k, kn, kh) < 0)
        return -1;
    if (key_init(&r->val, v, vn, vh) < 0) {
        free(r->key.p);
        return -1;
    }
    if (ix_insert(&b->fwd, kh, r->key.p, kn, b->len) < 0) {
        free(r->key.p);
        free(r->val.p);
        return -1;
    }
    if (ix_insert(&b->rev, vh, r->val.p, vn, b->len) < 0) {
        /* fwd already holds a BORROWED pointer to r->key.p: freeing the key
           without erasing that entry leaves the index dangling into freed
           memory. */
        uint32_t fp = ix_find(&b->fwd, kh, r->key.p, kn);
        if (fp != UINT32_MAX)
            ix_erase(&b->fwd, fp);
        free(r->key.p);
        free(r->val.p);
        return -1;
    }
    b->len++;
    return 0;
}

const char *dyn_bimap_get(const dyn_bimap_t *b, const char *k, size_t kn,
                          size_t *vn)
{
    uint32_t pos = ix_find(&b->fwd, ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return NULL;
    *vn = b->rec[b->fwd.slot[pos].idx].val.n;
    return b->rec[b->fwd.slot[pos].idx].val.p;
}

const char *dyn_bimap_key_for(const dyn_bimap_t *b, const char *v, size_t vn,
                              size_t *kn)
{
    uint32_t pos = ix_find(&b->rev, ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return NULL;
    *kn = b->rec[b->rev.slot[pos].idx].key.n;
    return b->rec[b->rev.slot[pos].idx].key.p;
}

int dyn_bimap_remove(dyn_bimap_t *b, const char *k, size_t kn)
{
    uint32_t pos = ix_find(&b->fwd, ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return 0;
    bimap_erase(b, b->fwd.slot[pos].idx);
    return 1;
}

int dyn_bimap_remove_value(dyn_bimap_t *b, const char *v, size_t vn)
{
    uint32_t pos = ix_find(&b->rev, ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return 0;
    bimap_erase(b, b->rev.slot[pos].idx);
    return 1;
}

uint32_t dyn_bimap_size(const dyn_bimap_t *b) { return b->len; }

int dyn_bimap_at(const dyn_bimap_t *b, uint32_t i, const char **k, size_t *kn,
                 const char **v, size_t *vn)
{
    if (i >= b->len)
        return 0;
    *k = b->rec[i].key.p;
    *kn = b->rec[i].key.n;
    *v = b->rec[i].val.p;
    *vn = b->rec[i].val.n;
    return 1;
}

/* ===================================================================== *
 *  Table -- (row, col) -> cell
 *
 *  Dense cell array plus one index over the row/column pair. row(r) and
 *  column(c) are linear scans of the dense array: this is a sparse cell store,
 *  not a materialised grid, and the API documents the complexity rather than
 *  paying for two more indices that most callers never query.
 * ===================================================================== */

typedef struct {
    ds_key row;
    ds_key col;
    ds_key pair;   /* rowlen-prefixed row||col, the index key */
    dyn_cell_t val;
} table_rec;

typedef struct { uint32_t head, tail; } table_chain;

struct dyn_table {
    table_rec *rec;
    uint32_t len, cap;
    dyn_ix ix;
    /* row()/column() used to scan every cell. These chain the records sharing
     * a row or a column. Each chain keeps a TAIL so a new record appends in
     * O(1) and the chain stays in record order -- dropping the whole index on
     * every put made put-then-slice slower than the scan it replaced. */
    dyn_ix row_ix, col_ix;
    table_chain *row_chain, *col_chain;
    uint32_t row_chains, row_chain_cap, col_chains, col_chain_cap;
    uint32_t *row_next, *col_next;
    uint32_t slice_cap;    /* allocation size of row_next/col_next */
    uint32_t sliced;       /* records already chained; < len means extend */
    int slices_valid;
};

static void table_drop_slices(dyn_table_t *t)
{
    ix_free(&t->row_ix);
    ix_free(&t->col_ix);
    free(t->row_chain);
    free(t->col_chain);
    free(t->row_next);
    free(t->col_next);
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

/* Append record `i` to the chain for `k`, creating the chain if new. */
static int table_chain_add(dyn_ix *ix, table_chain **chain, uint32_t *nchain,
                           uint32_t *cap, uint32_t *next, const ds_key *k,
                           uint32_t i)
{
    uint32_t pos = ix_find(ix, k->h, k->p, k->n);
    next[i] = UINT32_MAX;
    if (pos != UINT32_MAX) {
        table_chain *c = &(*chain)[ix->slot[pos].idx];
        next[c->tail] = i;
        c->tail = i;
        return 0;
    }
    if (ds_reserve((void **)chain, cap, *nchain + 1, sizeof(**chain)) < 0)
        return -1;
    (*chain)[*nchain].head = i;
    (*chain)[*nchain].tail = i;
    if (ix_insert(ix, k->h, k->p, k->n, *nchain) < 0)
        return -1;
    (*nchain)++;
    return 0;
}

/* Unlink record `i`; O(chain), the row's width rather than the table's.
 *
 * An index slot BORROWS its key from the record that created the chain. When
 * that record's key is about to be freed the slot must be re-pointed at a
 * survivor holding the same bytes, or the next lookup reads freed memory. */
static void table_chain_unlink(dyn_table_t *t, int by_column, uint32_t i)
{
    dyn_ix *ix = by_column ? &t->col_ix : &t->row_ix;
    table_chain *chain = by_column ? t->col_chain : t->row_chain;
    uint32_t *next = by_column ? t->col_next : t->row_next;
    const ds_key *k = by_column ? &t->rec[i].col : &t->rec[i].row;
    uint32_t pos = ix_find(ix, k->h, k->p, k->n), p;
    table_chain *c;

    if (pos == UINT32_MAX)
        return;
    c = &chain[ix->slot[pos].idx];
    if (c->head == i) {
        if (c->tail == i) {
            ix_erase(ix, pos);           /* chain empty; the slot goes too */
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

/* A swap-remove moves the last record, so its chain entry is renumbered. The
 * key pointer travels with the record, so no re-pointing is needed here. */
static void table_chain_move(dyn_table_t *t, int by_column, uint32_t from,
                             uint32_t to)
{
    dyn_ix *ix = by_column ? &t->col_ix : &t->row_ix;
    table_chain *chain = by_column ? t->col_chain : t->row_chain;
    uint32_t *next = by_column ? t->col_next : t->row_next;
    const ds_key *k = by_column ? &t->rec[to].col : &t->rec[to].row;
    uint32_t pos = ix_find(ix, k->h, k->p, k->n), p;
    table_chain *c;

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

/* Chain records [t->sliced, t->len). A full build is sliced == 0. */
static int table_build_slices(dyn_table_t *t)
{
    uint32_t i;

    if (t->len > t->slice_cap) {
        uint32_t cap = t->slice_cap ? t->slice_cap : 16;
        uint32_t *rn, *cn;
        while (cap < t->len)
            cap *= 2;
        rn = (uint32_t *)realloc(t->row_next, (size_t)cap * sizeof(uint32_t));
        if (!rn)
            return -1;
        t->row_next = rn;
        cn = (uint32_t *)realloc(t->col_next, (size_t)cap * sizeof(uint32_t));
        if (!cn)
            return -1;
        t->col_next = cn;
        t->slice_cap = cap;
    }
    for (i = t->sliced; i < t->len; i++) {
        if (table_chain_add(&t->row_ix, &t->row_chain, &t->row_chains,
                            &t->row_chain_cap, t->row_next,
                            &t->rec[i].row, i) < 0 ||
            table_chain_add(&t->col_ix, &t->col_chain, &t->col_chains,
                            &t->col_chain_cap, t->col_next,
                            &t->rec[i].col, i) < 0) {
            table_drop_slices(t);
            return -1;
        }
    }
    t->sliced = t->len;
    t->slices_valid = 1;
    return 0;
}

uint32_t dyn_table_slice_first(dyn_table_t *t, const char *k, size_t n,
                               int by_column)
{
    const dyn_ix *ix;
    const table_chain *chain;
    uint32_t pos;
    if ((!t->slices_valid || t->sliced != t->len) &&
        table_build_slices(t) < 0)
        return UINT32_MAX;
    ix = by_column ? &t->col_ix : &t->row_ix;
    chain = by_column ? t->col_chain : t->row_chain;
    pos = ix_find(ix, ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? UINT32_MAX : chain[ix->slot[pos].idx].head;
}

uint32_t dyn_table_slice_next(const dyn_table_t *t, uint32_t i, int by_column)
{
    const uint32_t *next = by_column ? t->col_next : t->row_next;
    if (!t->slices_valid || !next || i >= t->len)
        return UINT32_MAX;
    return next[i];
}

dyn_table_t *dyn_table_new(void)
{
    return (dyn_table_t *)ds_calloc(1, sizeof(dyn_table_t));
}

void dyn_table_free(dyn_table_t *t, dyn_cell_free_fn fn, void *ud)
{
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < t->len; i++) {
        if (fn)
            fn(ud, &t->rec[i].val);
        free(t->rec[i].row.p);
        free(t->rec[i].col.p);
        free(t->rec[i].pair.p);
    }
    free(t->rec);
    ix_free(&t->ix);
    table_drop_slices(t);
    free(t);
}

/* Length-prefixed concatenation, so ("ab","c") and ("a","bc") differ. */
static int table_pair(char **out, size_t *outn, const char *r, size_t rn,
                      const char *c, size_t cn)
{
    size_t n = 8 + rn + cn;
    char *p = (char *)malloc(n);
    int i;
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

int dyn_table_put(dyn_table_t *t, const char *r, size_t rn,
                  const char *c, size_t cn, const dyn_cell_t *v,
                  dyn_cell_t *old)
{
    char *pk;
    size_t pn;
    uint64_t h;
    uint32_t pos;
    table_rec *rec;

    if (table_pair(&pk, &pn, r, rn, c, cn) < 0)
        return -1;
    h = ix_hash(pk, pn);
    pos = ix_find(&t->ix, h, pk, pn);
    if (pos != UINT32_MAX) {
        uint32_t idx = t->ix.slot[pos].idx;
        free(pk);
        *old = t->rec[idx].val;
        t->rec[idx].val = *v;
        return 1;
    }
    if (ds_reserve((void **)&t->rec, &t->cap, t->len + 1,
                   sizeof(*t->rec)) < 0) {
        free(pk);
        return -1;
    }
    rec = &t->rec[t->len];
    memset(rec, 0, sizeof(*rec));
    if (key_init(&rec->row, r, rn, ix_hash(r, rn)) < 0 ||
        key_init(&rec->col, c, cn, ix_hash(c, cn)) < 0) {
        free(rec->row.p);
        free(pk);
        return -1;
    }
    rec->pair.p = pk;
    rec->pair.n = pn;
    rec->pair.h = h;
    rec->val = *v;
    if (ix_insert(&t->ix, h, pk, pn, t->len) < 0) {
        free(rec->row.p);
        free(rec->col.p);
        free(pk);
        return -1;
    }
    t->len++;
    /* Extend the chains rather than dropping them: the new record is the
     * highest index, so appending keeps every chain in record order. */
    if (t->slices_valid && table_build_slices(t) < 0)
        table_drop_slices(t);
    return 0;
}

const dyn_cell_t *dyn_table_get(const dyn_table_t *t, const char *r, size_t rn,
                                const char *c, size_t cn)
{
    char *pk;
    size_t pn;
    uint32_t pos;
    const dyn_cell_t *out = NULL;

    if (table_pair(&pk, &pn, r, rn, c, cn) < 0)
        return NULL;
    pos = ix_find(&t->ix, ix_hash(pk, pn), pk, pn);
    if (pos != UINT32_MAX)
        out = &t->rec[t->ix.slot[pos].idx].val;
    free(pk);
    return out;
}

int dyn_table_remove(dyn_table_t *t, const char *r, size_t rn,
                     const char *c, size_t cn, dyn_cell_t *out)
{
    char *pk;
    size_t pn;
    uint32_t pos, idx;

    if (table_pair(&pk, &pn, r, rn, c, cn) < 0)
        return 0;
    pos = ix_find(&t->ix, ix_hash(pk, pn), pk, pn);
    free(pk);
    if (pos == UINT32_MAX)
        return 0;
    idx = t->ix.slot[pos].idx;
    *out = t->rec[idx].val;
    ix_erase(&t->ix, pos);
    /* Repair the chains rather than dropping them: a full rebuild per delete
     * cost more than the scan the index replaced. Unlink BEFORE the keys are
     * freed -- the chain is found by them. */
    if (t->slices_valid) {
        table_chain_unlink(t, 0, idx);
        table_chain_unlink(t, 1, idx);
    }
    free(t->rec[idx].row.p);
    free(t->rec[idx].col.p);
    free(t->rec[idx].pair.p);
    t->len--;
    if (idx != t->len) {
        t->rec[idx] = t->rec[t->len];
        ix_move(&t->ix, t->rec[idx].pair.h, t->rec[idx].pair.p,
                t->rec[idx].pair.n, idx);
        if (t->slices_valid) {
            table_chain_move(t, 0, t->len, idx);
            table_chain_move(t, 1, t->len, idx);
        }
    }
    if (t->slices_valid)
        t->sliced = t->len;
    return 1;
}

uint32_t dyn_table_size(const dyn_table_t *t) { return t->len; }

int dyn_table_at(const dyn_table_t *t, uint32_t i, const char **r, size_t *rn,
                 const char **c, size_t *cn, const dyn_cell_t **v)
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

/* ===================================================================== *
 *  RangeSet -- sorted, disjoint, coalesced half-open spans
 * ===================================================================== */

typedef struct { double lo, hi; } span;

struct dyn_rset {
    span *s;
    uint32_t len, cap;
};

dyn_rset_t *dyn_rset_new(void)
{
    return (dyn_rset_t *)ds_calloc(1, sizeof(dyn_rset_t));
}

void dyn_rset_free(dyn_rset_t *s)
{
    if (!s)
        return;
    free(s->s);
    free(s);
}

void dyn_rset_clear(dyn_rset_t *s) { s->len = 0; }

/* First index whose hi > x (i.e. the first span that could contain x). */
static uint32_t rset_lower(const dyn_rset_t *s, double x)
{
    uint32_t lo = 0, hi = s->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->s[mid].hi > x)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static int rset_splice(dyn_rset_t *s, uint32_t at, uint32_t ndel,
                       const span *ins, uint32_t nins)
{
    uint32_t nlen = s->len - ndel + nins;
    if (ds_reserve((void **)&s->s, &s->cap, nlen, sizeof(*s->s)) < 0)
        return -1;
    memmove(&s->s[at + nins], &s->s[at + ndel],
            (size_t)(s->len - at - ndel) * sizeof(*s->s));
    if (nins)
        memcpy(&s->s[at], ins, (size_t)nins * sizeof(*s->s));
    s->len = nlen;
    return 0;
}

int dyn_rset_add(dyn_rset_t *s, double lo, double hi)
{
    uint32_t i, j;
    span merged;

    if (!(hi > lo))
        return 0;
    /* Everything touching or overlapping [lo,hi) coalesces, so the work starts
     * at the first span whose hi >= lo. Spans are disjoint and ascending, so
     * `hi` is strictly increasing and that index bisects -- a scan was O(n). */
    {
        uint32_t a = 0, b = s->len;
        while (a < b) {
            uint32_t mid = a + (b - a) / 2;
            if (s->s[mid].hi < lo)
                a = mid + 1;
            else
                b = mid;
        }
        i = a;
    }
    j = i;
    merged.lo = lo;
    merged.hi = hi;
    while (j < s->len && s->s[j].lo <= hi) {
        if (s->s[j].lo < merged.lo)
            merged.lo = s->s[j].lo;
        if (s->s[j].hi > merged.hi)
            merged.hi = s->s[j].hi;
        j++;
    }
    return rset_splice(s, i, j - i, &merged, 1);
}

int dyn_rset_remove(dyn_rset_t *s, double lo, double hi)
{
    uint32_t i, j, n = 0;
    span keep[2];

    if (!(hi > lo))
        return 0;
    i = 0;
    while (i < s->len && s->s[i].hi <= lo)
        i++;
    j = i;
    while (j < s->len && s->s[j].lo < hi)
        j++;
    if (j == i)
        return 0;
    if (s->s[i].lo < lo) {
        keep[n].lo = s->s[i].lo;
        keep[n].hi = lo;
        n++;
    }
    if (s->s[j - 1].hi > hi) {
        keep[n].lo = hi;
        keep[n].hi = s->s[j - 1].hi;
        n++;
    }
    return rset_splice(s, i, j - i, keep, n);
}

int dyn_rset_contains(const dyn_rset_t *s, double x)
{
    uint32_t i = rset_lower(s, x);
    return i < s->len && s->s[i].lo <= x;
}

int dyn_rset_encloses(const dyn_rset_t *s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 1;
    i = rset_lower(s, lo);
    return i < s->len && s->s[i].lo <= lo && s->s[i].hi >= hi;
}

int dyn_rset_intersects(const dyn_rset_t *s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 0;
    i = rset_lower(s, lo);
    return i < s->len && s->s[i].lo < hi;
}

uint32_t dyn_rset_count(const dyn_rset_t *s) { return s->len; }

int dyn_rset_at(const dyn_rset_t *s, uint32_t i, double *lo, double *hi)
{
    if (i >= s->len)
        return 0;
    *lo = s->s[i].lo;
    *hi = s->s[i].hi;
    return 1;
}

int dyn_rset_complement(const dyn_rset_t *s, double lo, double hi,
                        dyn_rset_t *out)
{
    double cur = lo;
    uint32_t i;

    out->len = 0;
    if (!(hi > lo))
        return 0;
    for (i = 0; i < s->len; i++) {
        if (s->s[i].hi <= cur)
            continue;
        if (s->s[i].lo >= hi)
            break;
        if (s->s[i].lo > cur && dyn_rset_add(out, cur, s->s[i].lo) < 0)
            return -1;
        if (s->s[i].hi > cur)
            cur = s->s[i].hi;
        if (cur >= hi)
            break;
    }
    if (cur < hi && dyn_rset_add(out, cur, hi) < 0)
        return -1;
    return 0;
}

/* ===================================================================== *
 *  RangeMap -- sorted, disjoint, NOT coalesced; the newest put wins
 * ===================================================================== */

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} rmap_span;

struct dyn_rmap {
    rmap_span *s;
    uint32_t len, cap;
};

dyn_rmap_t *dyn_rmap_new(void)
{
    return (dyn_rmap_t *)ds_calloc(1, sizeof(dyn_rmap_t));
}

void dyn_rmap_free(dyn_rmap_t *m, dyn_cell_free_fn fn, void *ud)
{
    uint32_t i;
    if (!m)
        return;
    if (fn)
        for (i = 0; i < m->len; i++)
            fn(ud, &m->s[i].val);
    free(m->s);
    free(m);
}

/* Carve [lo,hi) out of the map, handing every fully-covered cell to `fn` and
 * duplicating a cell whose span is split in two. Leaves a hole at the insertion
 * point, whose index is stored in *at. */
static int rmap_carve(dyn_rmap_t *m, double lo, double hi,
                      dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void *ud,
                      uint32_t *at)
{
    uint32_t i, j;

    /* Disjoint ascending spans, so the first one that can survive to the left
     * of [lo,hi) bisects. The j loop below is bounded by the overlap count. */
    {
        uint32_t a = 0, b = m->len;
        while (a < b) {
            uint32_t mid = a + (b - a) / 2;
            if (m->s[mid].hi <= lo)
                a = mid + 1;
            else
                b = mid;
        }
        i = a;
    }
    j = i;
    while (j < m->len && m->s[j].lo < hi)
        j++;
    *at = i;
    if (j == i)
        return 0;

    /* One span strictly enclosing [lo,hi) becomes two, so its cell is dup'd. */
    if (m->s[i].lo < lo && m->s[j - 1].hi > hi && j - i == 1) {
        rmap_span right;
        if (ds_reserve((void **)&m->s, &m->cap, m->len + 1, sizeof(*m->s)) < 0)
            return -1;
        right.lo = hi;
        right.hi = m->s[i].hi;
        dup(ud, &m->s[i].val, &right.val);
        m->s[i].hi = lo;
        memmove(&m->s[i + 2], &m->s[i + 1],
                (size_t)(m->len - i - 1) * sizeof(*m->s));
        m->s[i + 1] = right;
        m->len++;
        *at = i + 1;
        return 0;
    }
    if (m->s[i].lo < lo) {          /* trim the left survivor, keep its cell */
        m->s[i].hi = lo;
        i++;
        *at = i;
    }
    if (j > i && m->s[j - 1].hi > hi) {   /* trim the right survivor */
        m->s[j - 1].lo = hi;
        j--;
    }
    if (j > i) {
        uint32_t k;
        if (fn)
            for (k = i; k < j; k++)
                fn(ud, &m->s[k].val);
        memmove(&m->s[i], &m->s[j], (size_t)(m->len - j) * sizeof(*m->s));
        m->len -= j - i;
    }
    return 0;
}

int dyn_rmap_put(dyn_rmap_t *m, double lo, double hi, const dyn_cell_t *v,
                 dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void *ud)
{
    uint32_t at;
    if (!(hi > lo))
        return 0;
    if (rmap_carve(m, lo, hi, dup, fn, ud, &at) < 0)
        return -1;
    if (ds_reserve((void **)&m->s, &m->cap, m->len + 1, sizeof(*m->s)) < 0)
        return -1;
    memmove(&m->s[at + 1], &m->s[at], (size_t)(m->len - at) * sizeof(*m->s));
    m->s[at].lo = lo;
    m->s[at].hi = hi;
    m->s[at].val = *v;
    m->len++;
    return 0;
}

int dyn_rmap_remove(dyn_rmap_t *m, double lo, double hi,
                    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void *ud)
{
    uint32_t at;
    if (!(hi > lo))
        return 0;
    return rmap_carve(m, lo, hi, dup, fn, ud, &at);
}

const dyn_cell_t *dyn_rmap_get(const dyn_rmap_t *m, double x)
{
    uint32_t lo = 0, hi = m->len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m->s[mid].hi > x)
            hi = mid;
        else
            lo = mid + 1;
    }
    return (lo < m->len && m->s[lo].lo <= x) ? &m->s[lo].val : NULL;
}

uint32_t dyn_rmap_count(const dyn_rmap_t *m) { return m->len; }

int dyn_rmap_at(const dyn_rmap_t *m, uint32_t i, double *lo, double *hi,
                const dyn_cell_t **v)
{
    if (i >= m->len)
        return 0;
    *lo = m->s[i].lo;
    *hi = m->s[i].hi;
    *v = &m->s[i].val;
    return 1;
}

/* ===================================================================== *
 *  IntervalTree -- closed intervals, overlap enumeration
 *
 *  Dense array sorted by `lo`, with a max-of-`hi` segment tree over it. The
 *  index is rebuilt lazily on the first query after a mutation, so a bulk load
 *  pays one sort rather than one per insert. A query descends the tree and
 *  prunes any subtree whose max hi is below the query lo -- a BOSCC in the
 *  CLAUDE.md section 4 sense: a cheap summary that bypasses a whole subtree.
 * ===================================================================== */

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} ival;

struct dyn_itree {
    ival *v;
    uint32_t len, cap;
    double *mx;      /* segment tree, 2*sz entries, leaves at [sz, sz+len) */
    uint32_t sz;     /* power of two >= len */
    uint32_t mxcap;
    /* v[0..built) is sorted and covered by mx; v[built..len) was appended
     * since and is scanned linearly. Re-sorting on every query made an
     * interleaved insert/query workload O(n) per call. */
    uint32_t built;
    int dirty;
};

/* Rebuild once the unindexed tail is a fixed fraction of what is indexed, so
 * inserts amortise to O(1) and a query never scans more than that fraction. */
#define ITREE_PENDING_MAX(built) (16u + (built) / 8u)

dyn_itree_t *dyn_itree_new(void)
{
    return (dyn_itree_t *)ds_calloc(1, sizeof(dyn_itree_t));
}

void dyn_itree_free(dyn_itree_t *t, dyn_cell_free_fn fn, void *ud)
{
    uint32_t i;
    if (!t)
        return;
    if (fn)
        for (i = 0; i < t->len; i++)
            fn(ud, &t->v[i].val);
    free(t->v);
    free(t->mx);
    free(t);
}

int dyn_itree_insert(dyn_itree_t *t, double lo, double hi,
                     const dyn_cell_t *v)
{
    if (ds_reserve((void **)&t->v, &t->cap, t->len + 1, sizeof(*t->v)) < 0)
        return -1;
    t->v[t->len].lo = lo;
    t->v[t->len].hi = hi < lo ? lo : hi;
    t->v[t->len].val = *v;
    t->len++;
    t->dirty = 1;
    return 0;
}

uint32_t dyn_itree_size(const dyn_itree_t *t) { return t->len; }

static int ival_cmp(const void *a, const void *b)
{
    double x = ((const ival *)a)->lo, y = ((const ival *)b)->lo;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int itree_build(dyn_itree_t *t)
{
    uint32_t sz = 1, i;
    if (!t->dirty)
        return 0;
    if (t->len == 0) {
        t->dirty = 0;
        t->sz = 0;
        t->built = 0;
        return 0;
    }
    /* Leave a short tail unindexed; the query scans it. */
    if (t->built && t->len - t->built <= ITREE_PENDING_MAX(t->built))
        return 0;
    qsort(t->v, t->len, sizeof(*t->v), ival_cmp);
    while (sz < t->len)
        sz *= 2;
    if (ds_reserve((void **)&t->mx, &t->mxcap, 2 * sz, sizeof(*t->mx)) < 0)
        return -1;
    for (i = 0; i < sz; i++)
        t->mx[sz + i] = i < t->len ? t->v[i].hi : -INFINITY;
    for (i = sz - 1; i >= 1; i--)
        t->mx[i] = t->mx[2 * i] > t->mx[2 * i + 1] ? t->mx[2 * i]
                                                   : t->mx[2 * i + 1];
    t->sz = sz;
    t->built = t->len;
    t->dirty = 0;
    return 0;
}

uint32_t dyn_itree_query(dyn_itree_t *t, double lo, double hi,
                         uint32_t *out, uint32_t cap)
{
    uint32_t stack[128], top = 0, found = 0, k, p;

    if (itree_build(t) < 0 || t->len == 0)
        return 0;
    /* The unindexed tail is short by construction, so a linear pass over it
     * costs less than the rebuild it replaces. */
    for (p = t->built; p < t->len; p++)
        if (t->v[p].lo <= hi && t->v[p].hi >= lo) {
            if (found < cap)
                out[found] = p;
            found++;
        }
    if (t->built == 0)
        return found;
    /* k = number of intervals whose lo <= hi (the query's upper bound). */
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
        return 0;

    /* Descend, pruning any subtree whose max hi is below the query lo or whose
     * leaf range starts at or past k. Node n covers [n*span - sz, ...). */
    stack[top++] = 1;
    while (top) {
        uint32_t n = stack[--top], lvl = 0, m = n, base, width;
        while (m > 1) { m >>= 1; lvl++; }
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

int dyn_itree_at(const dyn_itree_t *t, uint32_t i, double *lo, double *hi,
                 const dyn_cell_t **v)
{
    if (i >= t->len)
        return 0;
    *lo = t->v[i].lo;
    *hi = t->v[i].hi;
    *v = &t->v[i].val;
    return 1;
}

int dyn_itree_remove_at(dyn_itree_t *t, uint32_t i, dyn_cell_t *out)
{
    if (i >= t->len)
        return 0;
    *out = t->v[i].val;
    t->len--;
    if (i != t->len)
        t->v[i] = t->v[t->len];
    /* The swap drops an unsorted element into the sorted prefix, so the whole
     * index has to go rather than just the tail. */
    t->built = 0;
    t->dirty = 1;
    return 1;
}

/* ===================================================================== *
 *  MinMaxHeap -- Atkinson/Sack/Santoro/Strothotte min-max heap
 *
 *  Even depths are min levels, odd depths are max levels, so the minimum is at
 *  the root and the maximum is the larger of its two children. Both ends are
 *  O(log n) with one array and no second structure.
 * ===================================================================== */

typedef struct {
    double pri;
    dyn_cell_t val;
} mmnode;

struct dyn_mmheap {
    mmnode *v;
    uint32_t len, cap;
};

dyn_mmheap_t *dyn_mmheap_new(void)
{
    return (dyn_mmheap_t *)ds_calloc(1, sizeof(dyn_mmheap_t));
}

void dyn_mmheap_free(dyn_mmheap_t *h, dyn_cell_free_fn fn, void *ud)
{
    uint32_t i;
    if (!h)
        return;
    if (fn)
        for (i = 0; i < h->len; i++)
            fn(ud, &h->v[i].val);
    free(h->v);
    free(h);
}

uint32_t dyn_mmheap_size(const dyn_mmheap_t *h) { return h->len; }

const dyn_cell_t *dyn_mmheap_cell_at(const dyn_mmheap_t *h, uint32_t i)
{
    return i < h->len ? &h->v[i].val : NULL;
}

double dyn_mmheap_pri_at(const dyn_mmheap_t *h, uint32_t i)
{
    return i < h->len ? h->v[i].pri : 0.0;
}

/* Depth of node i (0-based index): floor(log2(i+1)). Even = min level. */
static int mm_is_min_level(uint32_t i)
{
    int d = 0;
    uint32_t n = i + 1;
    while (n > 1) { n >>= 1; d++; }
    return (d & 1) == 0;
}

static void mm_swap(mmnode *a, mmnode *b)
{
    mmnode t = *a;
    *a = *b;
    *b = t;
}

static void mm_bubble_up_dir(dyn_mmheap_t *h, uint32_t i, int want_max)
{
    while (i >= 3) {
        uint32_t g = (i - 3) / 4;   /* grandparent of i */
        int better = want_max ? (h->v[i].pri > h->v[g].pri)
                              : (h->v[i].pri < h->v[g].pri);
        if (!better)
            return;
        mm_swap(&h->v[i], &h->v[g]);
        i = g;
    }
}

static void mm_bubble_up(dyn_mmheap_t *h, uint32_t i)
{
    uint32_t p;
    if (i == 0)
        return;
    p = (i - 1) / 2;
    if (mm_is_min_level(i)) {
        if (h->v[i].pri > h->v[p].pri) {
            mm_swap(&h->v[i], &h->v[p]);
            mm_bubble_up_dir(h, p, 1);
        } else {
            mm_bubble_up_dir(h, i, 0);
        }
    } else {
        if (h->v[i].pri < h->v[p].pri) {
            mm_swap(&h->v[i], &h->v[p]);
            mm_bubble_up_dir(h, p, 0);
        } else {
            mm_bubble_up_dir(h, i, 1);
        }
    }
}

/* Index of the best (min or max) among the children and grandchildren of i,
 * or UINT32_MAX if i is a leaf. `is_gc` reports whether it is a grandchild. */
static uint32_t mm_best_descendant(dyn_mmheap_t *h, uint32_t i, int want_max,
                                   int *is_gc)
{
    uint32_t best = UINT32_MAX, c, g;
    for (c = 2 * i + 1; c <= 2 * i + 2 && c < h->len; c++) {
        if (best == UINT32_MAX ||
            (want_max ? h->v[c].pri > h->v[best].pri
                      : h->v[c].pri < h->v[best].pri)) {
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

static void mm_trickle_down_dir(dyn_mmheap_t *h, uint32_t i, int want_max)
{
    for (;;) {
        int is_gc = 0;
        uint32_t m = mm_best_descendant(h, i, want_max, &is_gc);
        if (m == UINT32_MAX)
            return;
        if (!is_gc) {
            if (want_max ? h->v[m].pri > h->v[i].pri
                         : h->v[m].pri < h->v[i].pri)
                mm_swap(&h->v[m], &h->v[i]);
            return;
        }
        if (!(want_max ? h->v[m].pri > h->v[i].pri
                       : h->v[m].pri < h->v[i].pri))
            return;
        mm_swap(&h->v[m], &h->v[i]);
        {
            uint32_t p = (m - 1) / 2;
            if (want_max ? h->v[m].pri < h->v[p].pri
                         : h->v[m].pri > h->v[p].pri)
                mm_swap(&h->v[m], &h->v[p]);
        }
        i = m;
    }
}

static void mm_trickle_down(dyn_mmheap_t *h, uint32_t i)
{
    mm_trickle_down_dir(h, i, !mm_is_min_level(i));
}

int dyn_mmheap_push(dyn_mmheap_t *h, double pri, const dyn_cell_t *v)
{
    if (ds_reserve((void **)&h->v, &h->cap, h->len + 1, sizeof(*h->v)) < 0)
        return -1;
    h->v[h->len].pri = pri;
    h->v[h->len].val = *v;
    h->len++;
    mm_bubble_up(h, h->len - 1);
    return 0;
}

/* Index of the maximum: the root when n<=1, else the larger child. */
static uint32_t mm_max_index(const dyn_mmheap_t *h)
{
    if (h->len <= 1)
        return 0;
    if (h->len == 2)
        return 1;
    return h->v[1].pri >= h->v[2].pri ? 1 : 2;
}

static int mm_pop_at(dyn_mmheap_t *h, uint32_t i, double *pri,
                     dyn_cell_t *out)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[i].pri;
    *out = h->v[i].val;
    h->len--;
    if (i != h->len) {
        h->v[i] = h->v[h->len];
        mm_trickle_down(h, i);
    }
    return 1;
}

int dyn_mmheap_pop_min(dyn_mmheap_t *h, double *pri, dyn_cell_t *out)
{
    return mm_pop_at(h, 0, pri, out);
}

int dyn_mmheap_pop_max(dyn_mmheap_t *h, double *pri, dyn_cell_t *out)
{
    return mm_pop_at(h, mm_max_index(h), pri, out);
}

int dyn_mmheap_peek_min(const dyn_mmheap_t *h, double *pri,
                        const dyn_cell_t **v)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[0].pri;
    *v = &h->v[0].val;
    return 1;
}

int dyn_mmheap_peek_max(const dyn_mmheap_t *h, double *pri,
                        const dyn_cell_t **v)
{
    uint32_t i;
    if (h->len == 0)
        return 0;
    i = mm_max_index(h);
    *pri = h->v[i].pri;
    *v = &h->v[i].val;
    return 1;
}

/* ===================================================================== *
 *  CountMinSketch
 * ===================================================================== */

struct dyn_cms {
    uint64_t *c;       /* depth rows of width counters */
    uint32_t width, depth;
    uint64_t total;
};

dyn_cms_t *dyn_cms_new(uint32_t width, uint32_t depth)
{
    dyn_cms_t *s;
    if (width == 0 || depth == 0)
        return NULL;
    if ((uint64_t)width * depth > (uint64_t)1 << 28)
        return NULL;
    s = (dyn_cms_t *)ds_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->c = (uint64_t *)ds_calloc((size_t)width * depth, sizeof(uint64_t));
    if (!s->c) {
        free(s);
        return NULL;
    }
    s->width = width;
    s->depth = depth;
    return s;
}

void dyn_cms_free(dyn_cms_t *s)
{
    if (!s)
        return;
    free(s->c);
    free(s);
}

/* Kirsch-Mitzenmacher double hashing: two hashes give d independent-enough
 * probes without d passes over the key. */
static void cms_probe(const dyn_cms_t *s, const char *k, size_t n,
                      uint64_t *h1, uint64_t *h2)
{
    (void)s;
    *h1 = dyn_xxh64((const uint8_t *)k, n, 0);
    *h2 = dyn_mix64(*h1 ^ 0x9E3779B97F4A7C15ULL) | 1;
}

void dyn_cms_add(dyn_cms_t *s, const char *k, size_t n, uint64_t count)
{
    uint64_t h1, h2;
    uint32_t i;
    cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        s->c[(size_t)i * s->width + col] += count;
    }
    s->total += count;
}

uint64_t dyn_cms_count(const dyn_cms_t *s, const char *k, size_t n)
{
    uint64_t h1, h2, best = UINT64_MAX;
    uint32_t i;
    cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        uint64_t v = s->c[(size_t)i * s->width + col];
        if (v < best)
            best = v;
    }
    return best == UINT64_MAX ? 0 : best;
}

uint64_t dyn_cms_total(const dyn_cms_t *s) { return s->total; }
uint32_t dyn_cms_width(const dyn_cms_t *s) { return s->width; }
uint32_t dyn_cms_depth(const dyn_cms_t *s) { return s->depth; }

int dyn_cms_merge(dyn_cms_t *a, const dyn_cms_t *b)
{
    size_t i, n;
    if (a->width != b->width || a->depth != b->depth)
        return -1;
    n = (size_t)a->width * a->depth;
    for (i = 0; i < n; i++)
        a->c[i] += b->c[i];
    a->total += b->total;
    return 0;
}

const uint64_t *dyn_cms_counters(const dyn_cms_t *s) { return s->c; }
uint64_t *dyn_cms_counters_mut(dyn_cms_t *s) { return s->c; }
void dyn_cms_set_total(dyn_cms_t *s, uint64_t total) { s->total = total; }

/* ===================================================================== *
 *  HyperLogLog
 * ===================================================================== */

struct dyn_hll {
    uint8_t *reg;
    uint32_t p, m;
    /* The estimate is O(m) and a caller that asks twice pays twice. Set by
     * every writer of reg -- add, merge, and data_mut, which the deserializer
     * fills through. Missing one reports a full sketch as its stale value. */
    double cached;
    int dirty;
};

dyn_hll_t *dyn_hll_new(uint32_t precision)
{
    dyn_hll_t *h;
    if (precision < DYN_HLL_MIN_PRECISION || precision > DYN_HLL_MAX_PRECISION)
        return NULL;
    h = (dyn_hll_t *)ds_calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->p = precision;
    h->m = 1u << precision;
    h->reg = (uint8_t *)ds_calloc(h->m, 1);
    if (!h->reg) {
        free(h);
        return NULL;
    }
    /* calloc leaves cached=0 dirty=0, and an all-zero sketch's estimate IS
     * exactly 0 (zeros==m, so m*log(m/m)). Setting dirty here too would make
     * data_mut's invalidation redundant, and neither could then be tested. */
    return h;
}

void dyn_hll_free(dyn_hll_t *h)
{
    if (!h)
        return;
    free(h->reg);
    free(h);
}

void dyn_hll_add_hash(dyn_hll_t *h, uint64_t hash)
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

void dyn_hll_add(dyn_hll_t *h, const char *k, size_t n)
{
    dyn_hll_add_hash(h, dyn_xxh64((const uint8_t *)k, n, 0));
}

/* 2^-r for every rank a register can hold. Replacing the division measured
 * NULL on its own -- the loop was never division-bound -- but it costs nothing
 * and the values are all known at compile time. */
/* 256 entries, not 64: the index is a uint8_t register and dyn_hll_data_mut
 * hands a caller the array, so a 64-entry table is a global-buffer-overflow
 * read away. 2^-r is zero in a double past 63, so the tail is exact. */
static const double dyn_hll_inv_pow2[256] = {
    1.0/(double)(1ULL<<0), 1.0/(double)(1ULL<<1), 1.0/(double)(1ULL<<2),
    1.0/(double)(1ULL<<3), 1.0/(double)(1ULL<<4), 1.0/(double)(1ULL<<5),
    1.0/(double)(1ULL<<6), 1.0/(double)(1ULL<<7), 1.0/(double)(1ULL<<8),
    1.0/(double)(1ULL<<9), 1.0/(double)(1ULL<<10), 1.0/(double)(1ULL<<11),
    1.0/(double)(1ULL<<12), 1.0/(double)(1ULL<<13), 1.0/(double)(1ULL<<14),
    1.0/(double)(1ULL<<15), 1.0/(double)(1ULL<<16), 1.0/(double)(1ULL<<17),
    1.0/(double)(1ULL<<18), 1.0/(double)(1ULL<<19), 1.0/(double)(1ULL<<20),
    1.0/(double)(1ULL<<21), 1.0/(double)(1ULL<<22), 1.0/(double)(1ULL<<23),
    1.0/(double)(1ULL<<24), 1.0/(double)(1ULL<<25), 1.0/(double)(1ULL<<26),
    1.0/(double)(1ULL<<27), 1.0/(double)(1ULL<<28), 1.0/(double)(1ULL<<29),
    1.0/(double)(1ULL<<30), 1.0/(double)(1ULL<<31), 1.0/(double)(1ULL<<32),
    1.0/(double)(1ULL<<33), 1.0/(double)(1ULL<<34), 1.0/(double)(1ULL<<35),
    1.0/(double)(1ULL<<36), 1.0/(double)(1ULL<<37), 1.0/(double)(1ULL<<38),
    1.0/(double)(1ULL<<39), 1.0/(double)(1ULL<<40), 1.0/(double)(1ULL<<41),
    1.0/(double)(1ULL<<42), 1.0/(double)(1ULL<<43), 1.0/(double)(1ULL<<44),
    1.0/(double)(1ULL<<45), 1.0/(double)(1ULL<<46), 1.0/(double)(1ULL<<47),
    1.0/(double)(1ULL<<48), 1.0/(double)(1ULL<<49), 1.0/(double)(1ULL<<50),
    1.0/(double)(1ULL<<51), 1.0/(double)(1ULL<<52), 1.0/(double)(1ULL<<53),
    1.0/(double)(1ULL<<54), 1.0/(double)(1ULL<<55), 1.0/(double)(1ULL<<56),
    1.0/(double)(1ULL<<57), 1.0/(double)(1ULL<<58), 1.0/(double)(1ULL<<59),
    1.0/(double)(1ULL<<60), 1.0/(double)(1ULL<<61), 1.0/(double)(1ULL<<62),
    1.0/(double)(1ULL<<63), 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0, 0.0, 0.0,
    0.0,
};

/* One accumulator makes the loop latency-bound on the f64-add chain, not on
 * the term. 8 independent chains saturate; 16 compiles identical. Reassociates
 * the sum -- exact enough for an estimator, and the same order on both sides. */
#define DYN_HLL_NACC 8
_Static_assert((DYN_HLL_NACC & (DYN_HLL_NACC - 1)) == 0,
               "NACC must be a power of two: the bound is m & ~(NACC-1), and a "
               "non-power rounds UP, reading past the registers");

double dyn_hll_count(const dyn_hll_t *h)
{
    double m = (double)h->m, sum = 0.0, alpha, est;
    uint32_t i, zeros = 0;
    double s[DYN_HLL_NACC];
    uint32_t z[DYN_HLL_NACC], lim, j;
    const uint8_t *reg = h->reg;

    if (!h->dirty)
        return h->cached;

    for (j = 0; j < DYN_HLL_NACC; j++) { s[j] = 0.0; z[j] = 0; }
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
    for (j = 0; j < DYN_HLL_NACC; j++) { sum += s[j]; zeros += z[j]; }
    if (h->m == 16)      alpha = 0.673;
    else if (h->m == 32) alpha = 0.697;
    else if (h->m == 64) alpha = 0.709;
    else                 alpha = 0.7213 / (1.0 + 1.079 / m);

    est = alpha * m * m / sum;
    /* Small-range: the raw estimator is biased low once many registers are
     * still zero, and linear counting is exact there. There is no large-range
     * correction because the hash is 64-bit -- the 2^32 wraparound the original
     * paper corrects for cannot occur. */
    if (est <= 2.5 * m && zeros > 0)
        est = m * log(m / (double)zeros);
    /* The only mutation through the const pointer, and an exact function of
     * the registers: recomputing yields the same double, so the two paths are
     * indistinguishable to a reader. */
    ((dyn_hll_t *)h)->cached = est;
    ((dyn_hll_t *)h)->dirty = 0;
    return est;
}

uint32_t dyn_hll_precision(const dyn_hll_t *h) { return h->p; }
uint32_t dyn_hll_registers(const dyn_hll_t *h) { return h->m; }

int dyn_hll_merge(dyn_hll_t *a, const dyn_hll_t *b)
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

const uint8_t *dyn_hll_data(const dyn_hll_t *h) { return h->reg; }
/* Handing out a writable register pointer means the caller may change any
 * rank, so the cached estimate has to be dropped HERE -- the deserializer
 * memcpys through this and would otherwise report a full sketch as zero. */
uint8_t *dyn_hll_data_mut(dyn_hll_t *h) { h->dirty = 1; return h->reg; }

/* A rank counts leading zeros of a word whose bit p-1 is set, so it can never
 * exceed 64 - p + 1. A record claiming more was not produced by add(). */
int dyn_hll_regs_valid(const dyn_hll_t *h)
{
    uint32_t i, max = 64 - h->p + 1;
    for (i = 0; i < h->m; i++)
        if (h->reg[i] > max)
            return -1;
    return 0;
}

/* =====================================================================
 *  B+tree -- ordered map on double keys, values in linked leaves
 * ===================================================================== */

typedef struct btree_node {
    struct btree_node *parent;
    uint16_t n;              /* keys in use */
    uint8_t leaf;
    double key[DYN_BTREE_ORDER];
    union {
        struct {                                   /* leaf */
            dyn_cell_t val[DYN_BTREE_ORDER];
            struct btree_node *next, *prev;
        } l;
        struct {                                   /* internal */
            struct btree_node *kid[DYN_BTREE_ORDER + 1];
        } i;
    } u;
} btree_node;

struct dyn_btree {
    btree_node *root;
    btree_node *head;        /* leftmost leaf, for iteration */
    uint32_t count;
};

/* NaN would make every comparison false and the descent would not terminate
 * at a well-defined place; the binding refuses it before reaching here. */
static uint32_t btree_lower(const btree_node *nd, double k)
{
    uint32_t i = 0;
    while (i < nd->n && nd->key[i] < k)
        i++;
    return i;
}

static btree_node *btree_node_new(int leaf)
{
    btree_node *nd = (btree_node *)ds_calloc(1, sizeof(*nd));
    if (nd)
        nd->leaf = (uint8_t)leaf;
    return nd;
}

dyn_btree_t *dyn_btree_new(void)
{
    dyn_btree_t *t = (dyn_btree_t *)ds_calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->root = btree_node_new(1);
    if (!t->root) {
        free(t);
        return NULL;
    }
    t->head = t->root;
    return t;
}

void dyn_btree_free(dyn_btree_t *t, dyn_cell_free_fn fn, void *ud)
{
    btree_node **st = NULL;
    size_t len = 0, cap = 0;
    if (!t)
        return;
    if (t->root && ds_reserve((void **)&st, (uint32_t *)&cap, 1,
                              sizeof(*st)) == 0)
        st[len++] = t->root;
    while (len) {
        btree_node *nd = st[--len];
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
                if (ds_reserve((void **)&st, (uint32_t *)&cap, len + 1,
                               sizeof(*st)) < 0)
                    break;
                st[len++] = nd->u.i.kid[i];
            }
        }
        free(nd);
    }
    free(st);
    free(t);
}

uint32_t dyn_btree_size(const dyn_btree_t *t) { return t->count; }

static btree_node *btree_find_leaf(const dyn_btree_t *t, double k)
{
    btree_node *nd = t->root;
    while (!nd->leaf) {
        uint32_t i = btree_lower(nd, k);
        /* Keys equal to the separator live in the RIGHT child, matching the
         * lower-bound descent used by the leaf search. */
        if (i < nd->n && nd->key[i] == k)
            i++;
        nd = nd->u.i.kid[i];
    }
    return nd;
}

const dyn_cell_t *dyn_btree_get(const dyn_btree_t *t, double k)
{
    btree_node *nd = btree_find_leaf(t, k);
    uint32_t i = btree_lower(nd, k);
    if (i < nd->n && nd->key[i] == k)
        return &nd->u.l.val[i];
    return NULL;
}

/* Put `key`/`kid` into the internal node `nd` at slot `at`. */
static void btree_ins_internal(btree_node *nd, uint32_t at, double key,
                               btree_node *kid)
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

/* Split `nd`, propagating up. Returns -1 on allocation failure. */
static int btree_split(dyn_btree_t *t, btree_node *nd)
{
    while (nd->n == DYN_BTREE_ORDER) {
        uint32_t mid = DYN_BTREE_ORDER / 2, j;
        btree_node *right = btree_node_new(nd->leaf), *par;
        double up;
        if (!right)
            return -1;
        if (nd->leaf) {
            /* A leaf split COPIES the separator up: the key stays in the
             * right leaf, because leaves hold every key. */
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
            /* An internal split MOVES the separator up; it is not repeated. */
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
        if (!par) {
            par = btree_node_new(0);
            if (!par) {
                free(right);
                return -1;
            }
            par->n = 1;
            par->key[0] = up;
            par->u.i.kid[0] = nd;
            par->u.i.kid[1] = right;
            nd->parent = par;
            right->parent = par;
            t->root = par;
            return 0;
        }
        btree_ins_internal(par, btree_lower(par, up), up, right);
        nd = par;
    }
    return 0;
}

int dyn_btree_set(dyn_btree_t *t, double k, const dyn_cell_t *v,
                  dyn_cell_t *old, int *replaced)
{
    btree_node *nd = btree_find_leaf(t, k);
    uint32_t i = btree_lower(nd, k), j;

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
    return btree_split(t, nd);
}

/* Deletion leaves nodes under-full rather than rebalancing. The tree stays
 * correct and bounded by the keys ever inserted; a delete-heavy workload
 * trades space for never touching the parent chain. */
int dyn_btree_del(dyn_btree_t *t, double k, dyn_cell_t *out)
{
    btree_node *nd = btree_find_leaf(t, k);
    uint32_t i = btree_lower(nd, k), j;

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

static btree_node *btree_leftmost(const dyn_btree_t *t)
{
    btree_node *nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[0];
    return nd;
}

int dyn_btree_iter_begin(const dyn_btree_t *t, dyn_btree_iter *it)
{
    btree_node *nd = btree_leftmost(t);
    while (nd && nd->n == 0)
        nd = nd->u.l.next;
    it->leaf = nd;
    it->i = 0;
    return nd != NULL;
}

int dyn_btree_iter_seek(const dyn_btree_t *t, double k, dyn_btree_iter *it)
{
    btree_node *nd = btree_find_leaf(t, k);
    uint32_t i = btree_lower(nd, k);
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

int dyn_btree_iter_get(const dyn_btree_iter *it, double *k,
                       const dyn_cell_t **v)
{
    const btree_node *nd = (const btree_node *)it->leaf;
    if (!nd || it->i >= nd->n)
        return 0;
    *k = nd->key[it->i];
    *v = &nd->u.l.val[it->i];
    return 1;
}

int dyn_btree_iter_next(dyn_btree_iter *it)
{
    btree_node *nd = (btree_node *)it->leaf;
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

int dyn_btree_first(const dyn_btree_t *t, double *out)
{
    dyn_btree_iter it;
    const dyn_cell_t *v;
    if (!dyn_btree_iter_begin(t, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_last(const dyn_btree_t *t, double *out)
{
    btree_node *nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[nd->n];
    while (nd && nd->n == 0)
        nd = nd->u.l.prev;
    if (!nd)
        return 0;
    *out = nd->key[nd->n - 1];
    return 1;
}

int dyn_btree_ceil(const dyn_btree_t *t, double k, double *out)
{
    dyn_btree_iter it;
    const dyn_cell_t *v;
    if (!dyn_btree_iter_seek(t, k, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_floor(const dyn_btree_t *t, double k, double *out)
{
    btree_node *nd = btree_find_leaf(t, k);
    uint32_t i = btree_lower(nd, k);
    /* lower_bound lands on the first key >= k, so the floor is one before it,
     * possibly in the previous leaf. */
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
