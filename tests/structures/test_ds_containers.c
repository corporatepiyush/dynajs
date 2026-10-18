/* Generic containers of dyn-ds.c (Swiss map, red-black tree, weight-balanced tree, skip list,
 * XArray) checked against brute-force references under random operation sequences. The library
 * is included as one translation unit with a counting allocator, so every test also proves the
 * containers free everything they allocate. Build twice: default (NEON/SSE2 groups) and
 * -DDYN_DS_NO_SIMD (portable SWAR groups). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ds_live_allocs;
static void* t_malloc(size_t n)
{
    void* p = malloc(n);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_calloc(size_t n, size_t s)
{
    void* p = calloc(n, s);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_realloc(void* p, size_t n)
{
    void* q = realloc(p, n);
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
#include "dyn-ds.c"

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
        r->k = realloc(r->k, r->cap * sizeof(uint64_t));
        r->v = realloc(r->v, r->cap * sizeof(uint64_t));
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

static long owned_live;
static void str_dup(void* dst, const void* src, void* ud)
{
    const char* s;
    char* c;
    (void)ud;
    memcpy(&s, src, sizeof(s));
    c = strdup(s);
    owned_live++;
    memcpy(dst, &c, sizeof(c));
}
static void str_free(void* e, void* ud)
{
    char* s;
    (void)ud;
    memcpy(&s, e, sizeof(s));
    free(s);
    owned_live--;
}
static uint64_t str_hash(const void* e, void* ud)
{
    const char* s;
    (void)ud;
    memcpy(&s, e, sizeof(s));
    return dyn_ds_xxh64(s, strlen(s), 7);
}
static int str_eq(const void* a, const void* b, void* ud)
{
    const char *x, *y;
    (void)ud;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return strcmp(x, y) == 0;
}
static const dyn_ds_ops STR = { sizeof(char*), str_hash, str_eq, NULL, str_free, str_dup, NULL };

static uint64_t pick_key(uint64_t range)
{
    return rnd() % range;
}

static void test_swiss(void)
{
    dyn_swiss_t* m = dyn_swiss_new(&U64, &U64);
    ref_map r = { 0 };
    long i;
    printf("swiss group implementation: %s\n", dyn_swiss_group_impl());
    for (i = 0; i < 200000; i++) {
        uint64_t k = pick_key(i < 100000 ? 5000 : 60000), v = rnd();
        unsigned op = (unsigned)(rnd() % 10);
        if (op < 5) {
            int a = dyn_swiss_put(m, &k, &v), b = ref_put(&r, k, v);
            CHECK(a == b, "swiss put created %d want %d", a, b);
        } else if (op < 8) {
            int a = dyn_swiss_remove(m, &k), b = ref_del(&r, k);
            CHECK(a == b, "swiss remove %d want %d", a, b);
        } else {
            uint64_t* got = dyn_swiss_get(m, &k);
            long j = ref_find(&r, k);
            CHECK((got != NULL) == (j >= 0) && (!got || *got == r.v[j]), "swiss get key %llu", (unsigned long long)k);
        }
        CHECK(dyn_swiss_size(m) == r.n, "swiss size %zu want %zu", dyn_swiss_size(m), r.n);
        if (i == 150000) {
            dyn_swiss_clear(m);
            r.n = 0;
        }
    }
    {
        size_t it = 0, seen = 0;
        const void* k;
        void* v;
        while ((it = dyn_swiss_next(m, it, &k, &v)) != 0) {
            uint64_t kk;
            long j;
            memcpy(&kk, k, 8);
            j = ref_find(&r, kk);
            CHECK(j >= 0 && r.v[j] == *(uint64_t*)v, "swiss iteration yields a live pair");
            seen++;
        }
        CHECK(seen == r.n, "swiss iteration visits %zu of %zu", seen, r.n);
    }
    CHECK(dyn_swiss_reserve(m, 100000) == 0, "swiss reserve");
    CHECK(dyn_swiss_size(m) == r.n, "reserve keeps the size");
    dyn_swiss_free(m);
    free(r.k);
    free(r.v);
    {
        dyn_swiss_t* s = dyn_swiss_new(&STR, &U64);
        char buf[32];
        uint64_t v;
        for (i = 0; i < 5000; i++) {
            const char* p = buf;
            snprintf(buf, sizeof(buf), "key-%ld", i % 3000);
            v = (uint64_t)i;
            dyn_swiss_put(s, &p, &v);
        }
        CHECK(dyn_swiss_size(s) == 3000 && owned_live == 3000, "owned string keys: %zu entries, %ld live copies", dyn_swiss_size(s), owned_live);
        for (i = 0; i < 1500; i++) {
            const char* p = buf;
            snprintf(buf, sizeof(buf), "key-%ld", i);
            CHECK(dyn_swiss_remove(s, &p) == 1, "remove owned key");
        }
        CHECK(owned_live == 1500, "removing frees the owned keys (%ld live)", owned_live);
        {
            const char* p = "key-1999";
            uint64_t* got = dyn_swiss_get(s, &p);
            CHECK(got && *got == 4999, "a later put replaced the value");
            p = "key-2999";
            got = dyn_swiss_get(s, &p);
            CHECK(got && *got == 2999, "a key written once keeps its value");
        }
        dyn_swiss_free(s);
        CHECK(owned_live == 0, "freeing the map frees every owned key (%ld live)", owned_live);
    }
    printf("swiss ok\n");
}

typedef enum { T_RB,
    T_WB,
    T_SL } tree_kind;

static void* tree_new(tree_kind k)
{
    if (k == T_RB)
        return dyn_rbtree_new(&U64, &U64);
    if (k == T_WB)
        return dyn_wbtree_new(&U64, &U64);
    return dyn_skiplist_new(&U64, &U64, 12345);
}
static int tree_put(tree_kind k, void* t, uint64_t key, uint64_t v)
{
    return k == T_RB ? dyn_rbtree_put(t, &key, &v) : k == T_WB ? dyn_wbtree_put(t, &key, &v)
                                                               : dyn_skiplist_put(t, &key, &v);
}
static int tree_del(tree_kind k, void* t, uint64_t key)
{
    return k == T_RB ? dyn_rbtree_remove(t, &key) : k == T_WB ? dyn_wbtree_remove(t, &key)
                                                              : dyn_skiplist_remove(t, &key);
}
static uint64_t* tree_get(tree_kind k, void* t, uint64_t key)
{
    return k == T_RB ? dyn_rbtree_get(t, &key) : k == T_WB ? dyn_wbtree_get(t, &key)
                                                           : dyn_skiplist_get(t, &key);
}
static size_t tree_size(tree_kind k, void* t)
{
    return k == T_RB ? dyn_rbtree_size(t) : k == T_WB ? dyn_wbtree_size(t)
                                                      : dyn_skiplist_size(t);
}
static int tree_check(tree_kind k, void* t)
{
    return k == T_RB ? dyn_rbtree_check(t) : k == T_WB ? dyn_wbtree_check(t)
                                                       : dyn_skiplist_check(t);
}
static void tree_free(tree_kind k, void* t)
{
    if (k == T_RB)
        dyn_rbtree_free(t);
    else if (k == T_WB)
        dyn_wbtree_free(t);
    else
        dyn_skiplist_free(t);
}

static void tree_order_check(tree_kind kind, void* t, const ref_map* r)
{
    size_t i = 0;
    if (kind == T_RB) {
        dyn_rbnode* n;
        for (n = dyn_rbtree_first(t); n; n = dyn_rbtree_next(n), i++)
            CHECK(i < r->n && *(const uint64_t*)dyn_rbtree_key(t, n) == r->k[i], "rb in-order %zu", i);
        CHECK(i == r->n, "rb forward walk length");
        i = r->n;
        for (n = dyn_rbtree_last(t); n; n = dyn_rbtree_prev(n))
            CHECK(i > 0 && *(const uint64_t*)dyn_rbtree_key(t, n) == r->k[--i], "rb reverse order");
        CHECK(i == 0, "rb reverse walk length");
        i = r->n;
    } else if (kind == T_WB) {
        for (i = 0; i < r->n; i++) {
            const void* k;
            void* v;
            CHECK(dyn_wbtree_select(t, i, &k, &v) == 0 && *(const uint64_t*)k == r->k[i] && *(uint64_t*)v == r->v[i], "wb select %zu", i);
            CHECK(dyn_wbtree_rank(t, &r->k[i]) == i, "wb rank %zu", i);
        }
        CHECK(dyn_wbtree_select(t, r->n, NULL, NULL) == -1, "wb select past the end");
    } else {
        dyn_slnode* n;
        for (n = dyn_skiplist_first(t); n; n = dyn_skiplist_next(n), i++)
            CHECK(i < r->n && *(const uint64_t*)dyn_skiplist_key(t, n) == r->k[i], "skiplist order %zu", i);
        for (i = 0; i < r->n; i += 1 + r->n / 64) {
            dyn_slnode* a = dyn_skiplist_at(t, i);
            CHECK(a && *(const uint64_t*)dyn_skiplist_key(t, a) == r->k[i], "skiplist at %zu", i);
            CHECK(dyn_skiplist_rank(t, &r->k[i]) == i, "skiplist rank %zu", i);
        }
        CHECK(dyn_skiplist_at(t, r->n) == NULL, "skiplist at past the end");
    }
    CHECK(i == r->n || kind == T_SL, "order walk length");
}

static void test_tree(tree_kind kind, const char* name)
{
    void* t = tree_new(kind);
    ref_map r = { 0 };
    long i;
    for (i = 0; i < 120000; i++) {
        uint64_t k = pick_key(i < 60000 ? 2000 : 30000), v = rnd();
        unsigned op = (unsigned)(rnd() % 10);
        if (op < 6) {
            int a = tree_put(kind, t, k, v), b = ref_put(&r, k, v);
            CHECK(a == b, "%s put %d want %d", name, a, b);
        } else if (op < 9) {
            int a = tree_del(kind, t, k), b = ref_del(&r, k);
            CHECK(a == b, "%s remove %d want %d", name, a, b);
        } else {
            uint64_t* got = tree_get(kind, t, k);
            long j = ref_find(&r, k);
            CHECK((got != NULL) == (j >= 0) && (!got || *got == r.v[j]), "%s get", name);
        }
        CHECK(tree_size(kind, t) == r.n, "%s size", name);
        if (i % 4096 == 0)
            CHECK(tree_check(kind, t) == 0, "%s invariants at op %ld", name, i);
    }
    CHECK(tree_check(kind, t) == 0, "%s final invariants", name);
    tree_order_check(kind, t, &r);
    if (kind == T_RB && r.n > 10) {
        uint64_t probe = r.k[5] + 1;
        dyn_rbnode* lb = dyn_rbtree_lower_bound(t, &probe);
        dyn_rbnode* ub = dyn_rbtree_upper_bound(t, &r.k[5]);
        CHECK(lb && *(const uint64_t*)dyn_rbtree_key(t, lb) == r.k[6], "rb lower_bound");
        CHECK(ub && *(const uint64_t*)dyn_rbtree_key(t, ub) == r.k[6], "rb upper_bound");
    }
    if (kind == T_SL && r.n > 10) {
        uint64_t probe = r.k[3] + 1;
        dyn_slnode* s = dyn_skiplist_seek(t, &probe);
        CHECK(s && *(const uint64_t*)dyn_skiplist_key(t, s) == r.k[4], "skiplist seek");
    }
    for (i = 0; i < (long)r.n; i += 2)
        tree_del(kind, t, r.k[i]);
    CHECK(tree_check(kind, t) == 0, "%s invariants after deleting half", name);
    tree_free(kind, t);
    free(r.k);
    free(r.v);
    {
        void* s = tree_new(kind);
        for (i = 0; i < 100000; i++)
            tree_put(kind, s, (uint64_t)i, (uint64_t)i);
        CHECK(tree_check(kind, s) == 0, "%s sequential inserts stay balanced", name);
        for (i = 0; i < 100000; i += 3)
            tree_del(kind, s, (uint64_t)i);
        CHECK(tree_check(kind, s) == 0 && tree_size(kind, s) == 66666, "%s sequential deletes", name);
        tree_free(kind, s);
    }
    printf("%s ok\n", name);
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
    test_swiss();
    test_tree(T_RB, "rbtree");
    test_tree(T_WB, "wbtree");
    test_tree(T_SL, "skiplist");
    test_xarray();
    CHECK(ds_live_allocs == 0, "every container allocation was freed (%ld live)", ds_live_allocs);
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
