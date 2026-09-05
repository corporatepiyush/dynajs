/*
 * test_ds_btree_oom.c -- regression test for the B+tree split-OOM bug.
 *
 * The bug (fixed 2026-09-03): dyn_btree_set inserted into the leaf BEFORE
 * splitting, and the split allocated as it mutated. A failed allocation
 * mid-split returned -1 with the leaf resting at n == DYN_BTREE_ORDER, so the
 * NEXT set() shifted key[32]/val[32] out of bounds -- val[32] lands on the
 * leaf's next pointer (intra-object, ASan-blind), and further inserts cross
 * the allocation (ASan heap-buffer-overflow WRITE at the val-shift, observed
 * at dyn-ds.c:2607 under this very harness).
 *
 * The fix pre-stages every node the split cascade needs before mutating, and
 * dyn_btree_set rolls the insertion back on -1. This test FAILS the
 * allocation at every point in a fill sequence and asserts the documented
 * contract each time: "-1 leaves the container unchanged".
 *
 * Allocator interposition: the test TU defines malloc/calloc/realloc, so a
 * static link binds dyn-ds.c's allocations through them (verified under
 * ASan, whose interceptor sits above via RTLD_NEXT). Build/run: `make
 * test-ds-btree-oom` (same ASan+UBSan shape as test-ds-core).
 */
#include "dyn-ds.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- interposed allocator with an injection knob ------------------------ */
static long alloc_count = 0;
static long fail_from = -1;   /* allocations numbered >= fail_from return NULL */

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);
typedef void *(*realloc_fn)(void *, size_t);
typedef void *(*calloc_fn)(size_t, size_t);
static malloc_fn r_malloc;
static free_fn r_free;
static realloc_fn r_realloc;
static calloc_fn r_calloc;

static void resolve(void)
{
    if (!r_malloc) {
        r_malloc  = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
        r_free    = (free_fn)dlsym(RTLD_NEXT, "free");
        r_realloc = (realloc_fn)dlsym(RTLD_NEXT, "realloc");
        r_calloc  = (calloc_fn)dlsym(RTLD_NEXT, "calloc");
    }
}
void *malloc(size_t n)       { resolve(); if (fail_from >= 0 && ++alloc_count >= fail_from) return NULL; return r_malloc(n); }
void *calloc(size_t a, size_t b){ resolve(); if (fail_from >= 0 && ++alloc_count >= fail_from) return NULL; return r_calloc(a, b); }
void *realloc(void *p, size_t n){ resolve(); if (fail_from >= 0 && ++alloc_count >= fail_from) return NULL; return r_realloc(p, n); }
void free(void *p)           { resolve(); r_free(p); }

/* ---- harness ------------------------------------------------------------- */
static int failures = 0;
static long checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            if (++failures > 20) { printf("too many\n"); exit(1); }           \
        }                                                                     \
    } while (0)

static dyn_cell_t mkcell(uint64_t v)
{
    dyn_cell_t c;
    c.w[0] = v;
    c.w[1] = ~v;
    return c;
}

/* Iterate the whole tree; returns the entry count and checks strict order. */
static long iter_count_ordered(dyn_btree_t *t)
{
    dyn_btree_iter it;
    double k, prev = 0;
    const dyn_cell_t *v;
    long n = 0;
    int first = 1;
    if (!dyn_btree_iter_begin(t, &it))
        return 0;
    do {
        if (!dyn_btree_iter_get(&it, &k, &v))
            return -1;
        if (!first && !(k > prev))
            return -1;                  /* out of order or duplicate */
        prev = k;
        first = 0;
        n++;
    } while (dyn_btree_iter_next(&it));
    return n;
}

/* Fill to `nsets` keys with allocation failing from `from`; then verify the
 * contract: a -1 return left the container exactly as before the call. */
static void oom_at(long from, int nsets)
{
    dyn_btree_t *t = dyn_btree_new();
    dyn_cell_t old, c;
    int replaced, i, rcs;
    uint32_t size_before = 0;

    fail_from = -1;
    for (i = 0; i < nsets; i++) {
        c = mkcell((uint64_t)i);
        if (dyn_btree_set(t, (double)i, &c, &old, &replaced) < 0) {
            printf("pre-fill set failed at %d (fail disabled)\n", i);
            failures++;
            dyn_btree_free(t, NULL, NULL);
            return;
        }
    }
    size_before = dyn_btree_size(t);

    /* Fail from `from` onward and set one more key. */
    alloc_count = 0;
    fail_from = from;
    c = mkcell(0xABCD);
    rcs = dyn_btree_set(t, (double)nsets, &c, &old, &replaced);
    fail_from = -1;
    alloc_count = 0;

    if (rcs == 0) {
        /* The allocation window missed the split: fine, but the tree must
         * still be consistent. */
        CHECK(dyn_btree_size(t) == size_before + 1, "size grew by one");
    } else {
        CHECK(rcs == -1, "set returns exactly -1");
        /* THE contract: nothing changed. */
        CHECK(dyn_btree_size(t) == size_before,
              "fail_from=%ld nsets=%d: size %u != %u after -1 (rollback broken)",
              from, nsets, dyn_btree_size(t), size_before);
        CHECK(dyn_btree_get(t, (double)nsets) == NULL,
              "fail_from=%ld nsets=%d: key present after -1", from, nsets);
    }

    /* Memory restored: the tree must be fully usable and correct. */
    {
        long n = iter_count_ordered(t);
        CHECK(n == (long)dyn_btree_size(t),
              "fail_from=%ld nsets=%d: iter %ld != size %u", from, nsets, n,
              dyn_btree_size(t));
    }
    for (i = 0; i < nsets + 8; i++) {
        c = mkcell((uint64_t)0x1000 + (uint64_t)i);
        CHECK(dyn_btree_set(t, (double)(10000 + i), &c, &old, &replaced) == 0,
              "fail_from=%ld nsets=%d: post-OOM set(%d) failed", from, nsets, i);
        CHECK(dyn_btree_get(t, (double)(10000 + i)) != NULL,
              "fail_from=%ld nsets=%d: post-OOM get(%d) lost", from, nsets, i);
    }
    {
        long n = iter_count_ordered(t);
        CHECK(n == (long)dyn_btree_size(t),
              "fail_from=%ld nsets=%d: post-OOM iter %ld != size %u", from,
              nsets, n, dyn_btree_size(t));
    }
    dyn_btree_free(t, NULL, NULL);
}

/* Post-OOM inserts at the FRONT: the shift loop path that wrote key[32] in
 * the buggy version (insert position 0 forces the full-length shift). */
static void front_inserts_after_oom(void)
{
    dyn_btree_t *t = dyn_btree_new();
    dyn_cell_t old, c;
    int replaced, i;

    fail_from = -1;
    for (i = 0; i < 40; i++) {
        c = mkcell((uint64_t)i);
        dyn_btree_set(t, (double)i, &c, &old, &replaced);
    }
    alloc_count = 0;
    fail_from = 1;                     /* fail everything from the first alloc */
    c = mkcell(0);
    (void)dyn_btree_set(t, -1.0, &c, &old, &replaced);
    fail_from = -1;
    alloc_count = 0;

    for (i = -2; i > -12; i--) {
        c = mkcell((uint64_t)(100 - i));
        CHECK(dyn_btree_set(t, (double)i, &c, &old, &replaced) == 0,
              "front set(%d) failed", i);
    }
    {
        long n = iter_count_ordered(t);
        CHECK(n == (long)dyn_btree_size(t), "front iter %ld != size %u", n,
              dyn_btree_size(t));
        CHECK(dyn_btree_get(t, 0.0) != NULL, "front: key 0 lost");
    }
    dyn_btree_free(t, NULL, NULL);
}

int main(void)
{
    long f;
    int n;

    /* Every allocation index across three split boundaries (first split at
     * 32 keys; subsequent splits every 16) and one deep-fill point. */
    for (n = 31; n <= 34; n++)
        for (f = 1; f <= 120; f++)
            oom_at(f, n);
    for (f = 1; f <= 400; f += 7)
        oom_at(f, 1000);
    front_inserts_after_oom();

    printf("%s: %ld checks, %d failures\n",
           failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
