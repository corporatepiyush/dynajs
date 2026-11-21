/* Generic containers of dyn-ds.c (the counted B-tree ds-cbtree.c, which backs RBTree, WBTree and SkipList since
 * the red-black and weight-balanced node trees were deleted, and XArray) checked against brute-force references under random operation sequences. The library
 * is included as one translation unit with a counting allocator, so every test also proves the
 * containers free everything they allocate. (The second -DDYN_DS_NO_SIMD build left with
 * SwissMap, whose SIMD probe groups were the only code that flag selected.) The counted B-tree is also driven through an
 * allocation-failure sweep over its small array, bulk conversion and per-node allocations: a failed put leaves the map unchanged.
 * f64 keys take the sentinel path (unused key slots hold +Infinity so a read descent counts all 31 slots without a
 * branch): random, ascending, descending, two-ended, run and special-value (+-Infinity, subnormal, 2^53+1) sequences grow
 * to 33 sizes around every small-array, node and pool edge and drain back down, with dyn_cbt_check (which also checks
 * the sentinels and that only edge-spine leaves may hold fewer than 15 keys) and a sorted-model check of rank, at, get,
 * floorKey and ceilKey. Bytes per entry pin the layout: one allocation per node (no 32-node chunk slack, which cost
 * up to +60% at N=768) and edge-aware leaf splits (an append past the last key or before the first keeps the full
 * leaf whole, so ascending or descending fills stay near 30 of 31 slots instead of 15). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/dyn-xalloc.h"

static long ds_live_allocs;
static long ds_fail_after = -1;
static int ds_may_fail(void)
{
    if (ds_fail_after < 0)
        return 0;
    if (ds_fail_after == 0)
        return 1;
    ds_fail_after--;
    return 0;
}
static void* t_malloc(size_t n)
{
    void* p = ds_may_fail() ? NULL : malloc(n);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_calloc(size_t n, size_t s)
{
    void* p = ds_may_fail() ? NULL : calloc(n, s);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_realloc(void* p, size_t n)
{
    void* q = ds_may_fail() ? NULL : realloc(p, n);
    if (q && !p)
        ds_live_allocs++;
    return q;
}
static void t_free(void* p)
{
    if (p)
        ds_live_allocs--;
    free(p);
}
#define DYN_DS_MALLOC(n) t_malloc(n)
#define DYN_DS_CALLOC(n, s) t_calloc(n, s)
#define DYN_DS_REALLOC(p, n) t_realloc(p, n)
#define DYN_DS_FREE(p) t_free(p)
#include "ds-unity.c"

static int failures;
static long checks;
#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            if (++failures > 25)                        \
                exit(1);                                \
        }                                               \
    } while (0)

static uint64_t rng = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

typedef struct {
    uint64_t* k;
    uint64_t* v;
    size_t n, cap;
} ref_map;

static long ref_find(const ref_map* r, uint64_t k)
{
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (r->k[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < r->n && r->k[lo] == k ? (long)lo : -(long)lo - 1;
}
static int ref_put(ref_map* r, uint64_t k, uint64_t v)
{
    long i = ref_find(r, k);
    if (i >= 0) {
        r->v[i] = v;
        return 0;
    }
    i = -i - 1;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->k = xrealloc(r->k, r->cap * sizeof(uint64_t));
        r->v = xrealloc(r->v, r->cap * sizeof(uint64_t));
    }
    memmove(r->k + i + 1, r->k + i, (r->n - (size_t)i) * sizeof(uint64_t));
    memmove(r->v + i + 1, r->v + i, (r->n - (size_t)i) * sizeof(uint64_t));
    r->k[i] = k;
    r->v[i] = v;
    r->n++;
    return 1;
}
static int ref_del(ref_map* r, uint64_t k)
{
    long i = ref_find(r, k);
    if (i < 0)
        return 0;
    memmove(r->k + i, r->k + i + 1, (r->n - (size_t)i - 1) * sizeof(uint64_t));
    memmove(r->v + i, r->v + i + 1, (r->n - (size_t)i - 1) * sizeof(uint64_t));
    r->n--;
    return 1;
}

static int u64_cmp(const void* a, const void* b, void* ud)
{
    uint64_t x, y;
    (void)ud;
    memcpy(&x, a, 8);
    memcpy(&y, b, 8);
    return x < y ? -1 : x > y;
}

static const dyn_ds_ops U64 = { 8, NULL, NULL, u64_cmp, NULL, NULL, NULL };

static uint64_t pick_key(uint64_t range)
{
    return rnd() % range;
}

static void* tree_new(void)
{
    return dyn_cbt_new(&U64, &U64);
}
static int tree_put(void* t, uint64_t key, uint64_t v)
{
    return dyn_cbt_put(t, &key, &v);
}
static int tree_del(void* t, uint64_t key)
{
    return dyn_cbt_remove(t, &key);
}

typedef struct {
    const ref_map* r;
    size_t i;
    int bad;
} cbt_order;

static int cbt_order_visit(const void* k, void* v, void* ud)
{
    cbt_order* o = (cbt_order*)ud;
    if (o->i >= o->r->n || *(const uint64_t*)k != o->r->k[o->i] || *(const uint64_t*)v != o->r->v[o->i])
        o->bad++;
    o->i++;
    return 0;
}

static void tree_order_check(void* t, const ref_map* r)
{
    size_t i;
    cbt_order ord = { r, 0, 0 };
    dyn_cbt_walk(t, cbt_order_visit, &ord);
    CHECK(ord.bad == 0 && ord.i == r->n, "counted btree in-order walk (%zu of %zu)", ord.i, r->n);
    for (i = 0; i < r->n; i += 1 + r->n / 64) {
        const void* k;
        void* v;
        CHECK(dyn_cbt_at(t, i, &k, &v) == 0 && *(const uint64_t*)k == r->k[i] && *(uint64_t*)v == r->v[i], "counted btree at %zu", i);
        CHECK(dyn_cbt_rank(t, &r->k[i]) == i, "counted btree rank %zu", i);
        if (r->k[i] + 1 < (i + 1 < r->n ? r->k[i + 1] : UINT64_MAX)) {
            uint64_t gap = r->k[i] + 1;
            CHECK(dyn_cbt_rank(t, &gap) == i + 1, "counted btree rank of an absent key after %zu", i);
        }
    }
    CHECK(dyn_cbt_at(t, r->n, NULL, NULL) == -1, "counted btree at past the end");
    for (i = 0; i < 4000; i++) {
        uint64_t probe = rnd() % 32000;
        size_t lo = 0, hi = r->n;
        const uint64_t *fl, *ce;
        while (lo < hi) {
            size_t m = (lo + hi) / 2;
            if (r->k[m] < probe)
                lo = m + 1;
            else
                hi = m;
        }
        fl = (const uint64_t*)dyn_cbt_bound(t, &probe, 1);
        ce = (const uint64_t*)dyn_cbt_bound(t, &probe, 0);
        if (lo < r->n && r->k[lo] == probe)
            CHECK(fl && ce && *fl == probe && *ce == probe, "counted btree bound of a present key %llu", (unsigned long long)probe);
        else
            CHECK((lo ? fl && *fl == r->k[lo - 1] : !fl) && (lo < r->n ? ce && *ce == r->k[lo] : !ce), "counted btree floor/ceil of %llu", (unsigned long long)probe);
    }
}

static void test_tree(const char* name)
{
    void* t = tree_new();
    ref_map r = { 0 };
    long i;
    for (i = 0; i < 120000; i++) {
        uint64_t k = pick_key(i < 60000 ? 2000 : 30000), v = rnd();
        unsigned op = (unsigned)(rnd() % 10);
        if (op < 6) {
            int a = tree_put(t, k, v), b = ref_put(&r, k, v);
            CHECK(a == b, "%s put %d want %d", name, a, b);
        } else if (op < 9) {
            int a = tree_del(t, k), b = ref_del(&r, k);
            CHECK(a == b, "%s remove %d want %d", name, a, b);
        } else {
            uint64_t* got = dyn_cbt_get(t, &k);
            long j = ref_find(&r, k);
            CHECK((got != NULL) == (j >= 0) && (!got || *got == r.v[j]), "%s get", name);
        }
        CHECK(dyn_cbt_size(t) == r.n, "%s size", name);
        if (i % 4096 == 0)
            CHECK(dyn_cbt_check(t) == 0, "%s invariants at op %ld", name, i);
    }
    CHECK(dyn_cbt_check(t) == 0, "%s final invariants", name);
    tree_order_check(t, &r);
    if (r.n > 10) {
        uint64_t probe = r.k[3] + 1;
        const void* k = NULL;
        CHECK(dyn_cbt_at(t, dyn_cbt_rank(t, &probe), &k, NULL) == 0 && *(const uint64_t*)k == r.k[4], "counted btree seek via rank + at");
    }
    for (i = 0; i < (long)r.n; i += 2)
        tree_del(t, r.k[i]);
    CHECK(dyn_cbt_check(t) == 0, "%s invariants after deleting half", name);
    dyn_cbt_free(t);
    free(r.k);
    free(r.v);
    {
        void* s = tree_new();
        for (i = 0; i < 100000; i++)
            tree_put(s, (uint64_t)i, (uint64_t)i);
        CHECK(dyn_cbt_check(s) == 0, "%s sequential inserts stay balanced", name);
        for (i = 0; i < 100000; i += 3)
            tree_del(s, (uint64_t)i);
        CHECK(dyn_cbt_check(s) == 0 && dyn_cbt_size(s) == 66666, "%s sequential deletes", name);
        dyn_cbt_free(s);
    }
    printf("%s ok\n", name);
}

static void test_tree_small(void)
{
    long trial, i;
    size_t conv = 0;
    for (trial = 0; trial < 400; trial++) {
        void* t = tree_new();
        ref_map r = { 0 };
        uint64_t range = 1 + rnd() % 300;
        for (i = 0; i < 700; i++) {
            uint64_t k = pick_key(range), v = rnd();
            if (rnd() % 10 < 6)
                CHECK(tree_put(t, k, v) == ref_put(&r, k, v), "small put");
            else
                CHECK(tree_del(t, k) == ref_del(&r, k), "small remove");
            CHECK(dyn_cbt_size(t) == r.n && dyn_cbt_check(t) == 0, "small invariants at op %ld of trial %ld", i, trial);
            if (i % 50 == 0)
                tree_order_check(t, &r);
        }
        tree_order_check(t, &r);
        conv += r.n > 128;
        dyn_cbt_free(t);
        free(r.k);
        free(r.v);
    }
    printf("counted btree small maps ok (%zu of 400 ended above 128)\n", conv);
}

static void test_tree_oom(void)
{
    long k;
    for (k = 0; k < 200; k++) {
        void* t;
        uint8_t ok[3000];
        uint64_t i, n = 0;
        ds_fail_after = k;
        t = tree_new();
        if (!t) {
            ds_fail_after = -1;
            continue;
        }
        for (i = 0; i < 3000; i++)
            ok[(i * 7919) % 3000] = (uint8_t)(tree_put(t, (i * 7919) % 3000, i) == 1);
        ds_fail_after = -1;
        for (i = 0; i < 3000; i++) {
            uint64_t key = i, *got = dyn_cbt_get(t, &key);
            CHECK(ok[i] ? got != NULL : got == NULL, "tree oom %ld key %llu", k, (unsigned long long)i);
            n += ok[i];
        }
        CHECK(dyn_cbt_size(t) == n && dyn_cbt_check(t) == 0, "tree oom %ld invariants", k);
        for (i = 0; i < 3000; i++)
            if (!ok[i])
                CHECK(tree_put(t, i, i) == 1, "tree oom %ld refill", k);
        for (i = 0; i < 3000; i += 2)
            CHECK(tree_del(t, i) == 1, "tree oom %ld delete", k);
        CHECK(dyn_cbt_size(t) == 1500 && dyn_cbt_check(t) == 0, "tree oom %ld final", k);
        dyn_cbt_free(t);
    }
    printf("counted btree allocation failures ok\n");
}

static const dyn_ds_ops F64 = { 8, NULL, NULL, dyn_ds_cmp_f64, NULL, NULL, NULL };

typedef struct {
    double* k;
    size_t n, cap;
} fref;

static size_t fref_lb(const fref* r, double k)
{
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (r->k[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int fref_put(fref* r, double k)
{
    size_t i = fref_lb(r, k);
    if (i < r->n && r->k[i] == k)
        return 0;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->k = xrealloc(r->k, r->cap * sizeof(double));
    }
    memmove(r->k + i + 1, r->k + i, (r->n - i) * sizeof(double));
    r->k[i] = k;
    r->n++;
    return 1;
}

static int fref_del(fref* r, double k)
{
    size_t i = fref_lb(r, k);
    if (i >= r->n || r->k[i] != k)
        return 0;
    memmove(r->k + i, r->k + i + 1, (r->n - i - 1) * sizeof(double));
    r->n--;
    return 1;
}

static double f64_pick(int mode, long i, double* lo, double* hi)
{
    double u = (double)(rnd() % 1000003) / 7.0 - 50000.0;
    switch (mode) {
    case 0:
        return u;
    case 1:
        return *hi += 1.0 + (double)(rnd() % 5) / 4.0;
    case 2:
        return *lo -= 1.0 + (double)(rnd() % 5) / 4.0;
    case 3:
        return (i & 1) ? (*hi += 0.5 + (double)(rnd() % 3)) : (*lo -= 0.5 + (double)(rnd() % 3));
    case 4:
        if (rnd() % 97 == 0)
            *hi = u;
        return *hi += 1.0 / 64.0;
    default: {
        static const double sp[] = { 1e300, -1e300, 5e-324, -5e-324, 9007199254740993.0, -9007199254740992.0 };
        uint64_t c = rnd() % 16;
        if (c == 0)
            return INFINITY;
        if (c == 1)
            return -INFINITY;
        if (c < 8)
            return sp[c - 2];
        return u;
    }
    }
}

static void f64_verify(dyn_cbt_t* t, const fref* r, const char* what, size_t n)
{
    size_t i, step = r->n > 600 ? r->n / 300 : 1;
    long bad = 0;
    CHECK(dyn_cbt_size(t) == r->n && dyn_cbt_check(t) == 0, "f64 %s size %zu invariants", what, n);
    for (i = 0; i < r->n; i += step) {
        const void* k = NULL;
        if (dyn_cbt_at(t, i, &k, NULL) || *(const double*)k != r->k[i])
            bad++;
        if (dyn_cbt_rank(t, &r->k[i]) != i || !dyn_cbt_get(t, &r->k[i]))
            bad++;
    }
    for (i = 0; i < 64; i++) {
        double q = i < 2 ? (i ? INFINITY : -INFINITY) : (r->n && (i & 1) ? r->k[rnd() % r->n] + (i & 2 ? 1e-9 : -1e-9) : (double)(rnd() % 1000003) / 7.0 - 50000.0);
        size_t p = fref_lb(r, q);
        int hit = p < r->n && r->k[p] == q;
        const double* fl = dyn_cbt_bound(t, &q, 1);
        const double* ce = dyn_cbt_bound(t, &q, 0);
        if (dyn_cbt_rank(t, &q) != p || (dyn_cbt_get(t, &q) != NULL) != hit)
            bad++;
        if (hit ? !(fl && *fl == q && ce && *ce == q) : !((p ? fl && *fl == r->k[p - 1] : !fl) && (p < r->n ? ce && *ce == r->k[p] : !ce)))
            bad++;
    }
    CHECK(bad == 0, "f64 %s size %zu rank/at/get/bounds match the sorted model (%ld bad)", what, n, bad);
}

static void test_tree_f64(void)
{
    static const size_t sizes[] = { 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128, 129, 130, 160, 192, 255, 256, 257, 383, 511, 512, 513, 767, 769, 1023, 1024, 1025, 1399, 2048, 4093, 9973, 30011 };
    static const char* names[] = { "random", "ascending", "descending", "two-ended", "runs", "specials" };
    size_t si;
    int mode;
    for (mode = 0; mode < 6; mode++)
        for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            size_t n = sizes[si];
            dyn_cbt_t* t = dyn_cbt_new(&F64, &U64);
            fref r = { 0 };
            double lo = 0, hi = 0, v[1] = { 0 };
            long i, tries = 0;
            for (i = 0; r.n < n && tries < (long)n * 40 + 400; tries++, i++) {
                double k = f64_pick(mode, i, &lo, &hi);
                uint64_t val = (uint64_t)i;
                CHECK(dyn_cbt_put(t, &k, &val) == fref_put(&r, k), "f64 %s put", names[mode]);
                if (n <= 300 || (i & 1023) == 0)
                    CHECK(dyn_cbt_size(t) == r.n && dyn_cbt_check(t) == 0, "f64 %s invariants while growing to %zu", names[mode], n);
            }
            f64_verify(t, &r, names[mode], n);
            while (r.n) {
                double k = mode == 1 ? r.k[0] : mode == 2 ? r.k[r.n - 1] : r.k[rnd() % r.n];
                CHECK(dyn_cbt_remove(t, &k) == 1 && fref_del(&r, k) == 1, "f64 %s remove", names[mode]);
                if (r.n <= 300 || (r.n & 1023) == 0)
                    CHECK(dyn_cbt_size(t) == r.n && dyn_cbt_check(t) == 0, "f64 %s invariants while draining from %zu", names[mode], n);
                if (r.n == n / 2 || r.n == 129 || r.n == 128 || r.n == 31)
                    f64_verify(t, &r, names[mode], n);
            }
            (void)v;
            dyn_cbt_free(t);
            free(r.k);
        }
    printf("counted btree f64 keys ok\n");
}

static double tree_bpe(int order, size_t n)
{
    dyn_cbt_t* t = dyn_cbt_new(&U64, &U64);
    uint64_t* ks = xmalloc(n * sizeof(uint64_t));
    size_t i;
    double b;
    for (i = 0; i < n; i++)
        ks[i] = order == 2 ? (uint64_t)(n - i) * 3 : (uint64_t)i * 3;
    if (!order)
        for (i = n - 1; i > 0; i--) {
            size_t j = (size_t)(rnd() % (i + 1));
            uint64_t x = ks[i];
            ks[i] = ks[j];
            ks[j] = x;
        }
    for (i = 0; i < n; i++)
        dyn_cbt_put(t, &ks[i], &ks[i]);
    CHECK(dyn_cbt_check(t) == 0 && dyn_cbt_size(t) == n, "bytes probe %d %zu invariants", order, n);
    b = (double)dyn_cbt_bytes(t) / (double)n;
    dyn_cbt_free(t);
    free(ks);
    return b;
}

static void test_tree_bytes(void)
{
    static const size_t sizes[] = { 129, 192, 256, 384, 512, 768, 1000, 1400, 4096, 100000 };
    size_t si;
    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        double r = 0, a = tree_bpe(1, sizes[si]), d = tree_bpe(2, sizes[si]);
        int k;
        for (k = 0; k < 8; k++)
            r += tree_bpe(0, sizes[si]) / 8;
        printf("counted btree bytes/entry at %zu: random %.1f ascending %.1f descending %.1f\n", sizes[si], r, a, d);
        CHECK(r <= 34.0, "random inserts at %zu cost %.1f bytes/entry", sizes[si], r);
        CHECK(a <= (sizes[si] < 300 ? 34.0 : 23.0) && d <= (sizes[si] < 300 ? 34.0 : 23.0), "edge appends at %zu cost %.1f/%.1f bytes/entry", sizes[si], a, d);
    }
}

static void test_xarray(void)
{
    dyn_xarray_t* x = dyn_xarray_new(&U64);
    ref_map r = { 0 };
    long i;
    uint64_t specials[] = { 0, 1, 63, 64, 4095, 4096, 262143, 262144, (uint64_t)1 << 32, ((uint64_t)1 << 60) - 1, (uint64_t)1 << 60, ((uint64_t)1 << 63) + 5, UINT64_MAX - 1, UINT64_MAX };
    for (i = 0; i < 150000; i++) {
        uint64_t k, v = rnd();
        unsigned op = (unsigned)(rnd() % 10);
        if (rnd() % 50 == 0)
            k = specials[rnd() % (sizeof(specials) / sizeof(specials[0]))];
        else
            k = rnd() % (i < 75000 ? 3000 : 1000000);
        if (op < 5) {
            int a = dyn_xarray_store(x, k, &v), b = ref_put(&r, k, v);
            CHECK(a == b, "xarray store %d want %d at %llu", a, b, (unsigned long long)k);
        } else if (op < 8) {
            int a = dyn_xarray_erase(x, k), b = ref_del(&r, k);
            CHECK(a == b, "xarray erase %d want %d", a, b);
        } else {
            uint64_t* got = dyn_xarray_load(x, k);
            long j = ref_find(&r, k);
            CHECK((got != NULL) == (j >= 0) && (!got || *got == r.v[j]), "xarray load %llu", (unsigned long long)k);
        }
        CHECK(dyn_xarray_count(x) == r.n, "xarray count");
    }
    {
        uint64_t idx = 0;
        size_t seen = 0;
        void* v;
        while (dyn_xarray_find(x, &idx, UINT64_MAX, &v)) {
            CHECK(seen < r.n && idx == r.k[seen] && *(uint64_t*)v == r.v[seen], "xarray find yields keys in order (%zu)", seen);
            seen++;
            if (idx == UINT64_MAX)
                break;
            idx++;
        }
        CHECK(seen == r.n, "xarray find visits %zu of %zu", seen, r.n);
    }
    {
        size_t j, marked = 0;
        for (j = 0; j < r.n; j += 7) {
            CHECK(dyn_xarray_set_mark(x, r.k[j], 1, 1) == 0, "set mark");
            marked++;
        }
        {
            uint64_t idx = 0;
            size_t seen = 0;
            while (dyn_xarray_find_marked(x, &idx, UINT64_MAX, 1, NULL)) {
                CHECK(seen * 7 < r.n && idx == r.k[seen * 7], "find_marked order");
                CHECK(dyn_xarray_get_mark(x, idx, 1) == 1, "get_mark");
                seen++;
                if (idx == UINT64_MAX)
                    break;
                idx++;
            }
            CHECK(seen == marked, "find_marked visits %zu of %zu", seen, marked);
        }
        for (j = 0; j < r.n; j += 14)
            dyn_xarray_set_mark(x, r.k[j], 1, 0);
        {
            uint64_t idx = 0;
            size_t seen = 0;
            while (dyn_xarray_find_marked(x, &idx, UINT64_MAX, 1, NULL)) {
                seen++;
                if (idx == UINT64_MAX)
                    break;
                idx++;
            }
            CHECK(seen == marked - (r.n + 13) / 14, "clearing marks propagates (%zu)", seen);
        }
        CHECK(dyn_xarray_set_mark(x, 999999999999ULL, 0, 1) == -1, "marking an absent index fails");
    }
    {
        uint64_t idx = r.k[r.n / 2] + 1, want = r.k[r.n / 2 + 1];
        CHECK(dyn_xarray_find(x, &idx, want - 1, NULL) == 0, "find respects max");
        CHECK(dyn_xarray_find(x, &idx, want, NULL) == 1 && idx == want, "find reaches max exactly");
    }
    while (r.n) {
        CHECK(dyn_xarray_erase(x, r.k[r.n - 1]) == 1, "erase all");
        r.n--;
    }
    CHECK(dyn_xarray_count(x) == 0 && dyn_xarray_load(x, 0) == NULL, "empty after erasing all");
    {
        uint64_t one = 1;
        CHECK(dyn_xarray_store(x, UINT64_MAX, &one) == 1 && *(uint64_t*)dyn_xarray_load(x, UINT64_MAX) == 1, "store at UINT64_MAX in an emptied array");
    }
    dyn_xarray_free(x);
    free(r.k);
    free(r.v);
    printf("xarray ok\n");
}

int main(void)
{
    test_tree("counted btree");
    test_tree_small();
    test_tree_oom();
    test_tree_f64();
    test_tree_bytes();
    test_xarray();
    CHECK(ds_live_allocs == 0, "every container allocation was freed (%ld live)", ds_live_allocs);
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
