/*
 * test_ds_core.c -- randomised differential test of src/core/dyn-ds.c against
 * brute-force models, in pure C so it runs under ASan/UBSan without the engine.
 *
 * The interesting containers here are the ones whose invariants are easy to
 * state and hard to implement: a coalescing RangeSet, a Guava-semantics
 * RangeMap (newest put wins, overlapping spans trimmed), an interval tree that
 * must enumerate EVERY overlap, and a min-max heap whose two ends are
 * maintained in one array. Each is checked against an O(n) model over millions
 * of random operations, which is the only way to find the case where a boundary
 * is off by one endpoint.
 *
 *   cc -Isrc/core tests/test_ds_core.c src/core/dyn-ds.c src/core/dyn-hash.c
 */
#include "dyn-ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t rng_state = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}
static uint32_t rnd_n(uint32_t n) { return (uint32_t)(rnd() % n); }

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

static dyn_cell_t cell(uint64_t v)
{
    dyn_cell_t c;
    c.w[0] = v;
    c.w[1] = ~v;
    return c;
}

/* ------------------------------------------------------------------ Multiset */

#define MS_KEYS 64

static void test_multiset(void)
{
    uint64_t model[MS_KEYS];
    dyn_mset_t *m = dyn_mset_new();
    char kb[32];
    uint32_t it, i;
    uint64_t total = 0;

    memset(model, 0, sizeof(model));
    for (it = 0; it < 400000; it++) {
        uint32_t k = rnd_n(MS_KEYS);
        int64_t d = (int64_t)(rnd_n(21)) - 10;
        uint64_t got, want;
        size_t kl = (size_t)snprintf(kb, sizeof(kb), "k%u", k);

        if (rnd_n(20) == 0) {
            uint64_t c = rnd_n(5);
            total = total - model[k] + c;
            model[k] = c;
            CHECK(dyn_mset_set_count(m, kb, kl, c) == 0, "set_count oom");
        } else {
            if (d < 0) {
                uint64_t sub = (uint64_t)(-d);
                uint64_t nv = sub >= model[k] ? 0 : model[k] - sub;
                total -= model[k] - nv;
                model[k] = nv;
            } else {
                model[k] += (uint64_t)d;
                total += (uint64_t)d;
            }
            CHECK(dyn_mset_add(m, kb, kl, d, &got) == 0, "add oom");
            CHECK(got == model[k], "add count k=%u got %llu want %llu",
                  k, (unsigned long long)got, (unsigned long long)model[k]);
        }
        if ((it & 1023) == 0) {
            uint32_t distinct = 0;
            for (i = 0; i < MS_KEYS; i++) {
                size_t l = (size_t)snprintf(kb, sizeof(kb), "k%u", i);
                want = model[i];
                CHECK(dyn_mset_count(m, kb, l) == want,
                      "count k=%u got %llu want %llu", i,
                      (unsigned long long)dyn_mset_count(m, kb, l),
                      (unsigned long long)want);
                if (want)
                    distinct++;
            }
            CHECK(dyn_mset_distinct(m) == distinct, "distinct %u vs %u",
                  dyn_mset_distinct(m), distinct);
            CHECK(dyn_mset_total(m) == total, "total %llu vs %llu",
                  (unsigned long long)dyn_mset_total(m),
                  (unsigned long long)total);
        }
    }
    /* Dense iteration must visit each live key exactly once. */
    {
        uint64_t seen[MS_KEYS];
        const char *kp;
        size_t kl;
        uint64_t c;
        memset(seen, 0, sizeof(seen));
        for (i = 0; dyn_mset_at(m, i, &kp, &kl, &c); i++) {
            /* Stored keys are byte strings with no NUL: copy before atoi. */
            char tmp[32];
            uint32_t k;
            CHECK(kl < sizeof(tmp), "key too long");
            memcpy(tmp, kp, kl);
            tmp[kl] = 0;
            k = (uint32_t)atoi(tmp + 1);
            CHECK(k < MS_KEYS && seen[k] == 0, "dup or bad key in iteration");
            seen[k] = c;
        }
        for (i = 0; i < MS_KEYS; i++)
            CHECK(seen[i] == model[i], "iteration k=%u %llu vs %llu", i,
                  (unsigned long long)seen[i], (unsigned long long)model[i]);
    }
    dyn_mset_free(m);
    printf("multiset ok\n");
}

/* ------------------------------------------------------------------ RangeSet */

#define RS_UNIV 200

static void test_rangeset(void)
{
    unsigned char model[RS_UNIV];
    dyn_rset_t *s = dyn_rset_new();
    uint32_t it, i;

    memset(model, 0, sizeof(model));
    for (it = 0; it < 200000; it++) {
        uint32_t a = rnd_n(RS_UNIV), b = rnd_n(RS_UNIV);
        uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
        int add = rnd_n(2);

        if (add) {
            for (i = lo; i < hi; i++) model[i] = 1;
            CHECK(dyn_rset_add(s, lo, hi) == 0, "rset add oom");
        } else {
            for (i = lo; i < hi; i++) model[i] = 0;
            CHECK(dyn_rset_remove(s, lo, hi) == 0, "rset remove oom");
        }
        if ((it & 255) == 0) {
            uint32_t n = dyn_rset_count(s), spans = 0;
            double plo = -1e300, phi = -1e300;
            for (i = 0; i < RS_UNIV; i++) {
                CHECK(dyn_rset_contains(s, i) == model[i],
                      "contains %u it=%u", i, it);
                if (model[i] && (i == 0 || !model[i - 1]))
                    spans++;
            }
            CHECK(n == spans, "span count %u vs %u at it=%u", n, spans, it);
            /* Spans must be sorted, disjoint AND non-adjacent (coalesced). */
            for (i = 0; i < n; i++) {
                double l, h;
                dyn_rset_at(s, i, &l, &h);
                CHECK(h > l, "empty span");
                if (i)
                    CHECK(l > phi, "spans not coalesced: %g..%g then %g..%g",
                          plo, phi, l, h);
                plo = l; phi = h;
            }
            /* encloses / intersects against the model */
            for (i = 0; i < 40; i++) {
                uint32_t x = rnd_n(RS_UNIV), y = rnd_n(RS_UNIV);
                uint32_t l = x < y ? x : y, h = x < y ? y : x;
                int enc = 1, isect = 0, j;
                for (j = (int)l; j < (int)h; j++) {
                    if (model[j]) isect = 1; else enc = 0;
                }
                if (h == l) { enc = 1; isect = 0; }
                CHECK(dyn_rset_encloses(s, l, h) == enc, "encloses %u..%u", l, h);
                CHECK(dyn_rset_intersects(s, l, h) == isect,
                      "intersects %u..%u", l, h);
            }
        }
    }
    /* complement within [0, RS_UNIV) */
    {
        dyn_rset_t *c = dyn_rset_new();
        CHECK(dyn_rset_complement(s, 0, RS_UNIV, c) == 0, "complement oom");
        for (i = 0; i < RS_UNIV; i++)
            CHECK(dyn_rset_contains(c, i) == !model[i], "complement %u", i);
        dyn_rset_free(c);
    }
    dyn_rset_free(s);
    printf("rangeset ok\n");
}

/* ------------------------------------------------------------------ RangeMap */

static int rm_live[1 << 20];
static void rm_freed(void *ud, const dyn_cell_t *c)
{
    (void)ud;
    rm_live[c->w[0]]--;
}
static void rm_dup(void *ud, const dyn_cell_t *in, dyn_cell_t *out)
{
    (void)ud;
    rm_live[in->w[0]]++;
    *out = *in;
}

static void test_rangemap(void)
{
    int model[RS_UNIV];         /* -1 = unmapped, else the value id */
    dyn_rmap_t *m = dyn_rmap_new();
    uint32_t it, i;
    uint32_t next_id = 1;

    for (i = 0; i < RS_UNIV; i++) model[i] = -1;
    memset(rm_live, 0, sizeof(rm_live));

    for (it = 0; it < 120000; it++) {
        uint32_t a = rnd_n(RS_UNIV), b = rnd_n(RS_UNIV);
        uint32_t lo = a < b ? a : b, hi = a < b ? b : a;

        if (rnd_n(4) == 0) {
            for (i = lo; i < hi; i++) model[i] = -1;
            CHECK(dyn_rmap_remove(m, lo, hi, rm_dup, rm_freed, NULL) == 0,
                  "rmap remove oom");
        } else {
            uint32_t id = next_id++;
            dyn_cell_t c = cell(id);
            if (hi > lo) {
                rm_live[id] = 1;
                for (i = lo; i < hi; i++) model[i] = (int)id;
            }
            CHECK(dyn_rmap_put(m, lo, hi, &c, rm_dup, rm_freed, NULL) == 0,
                  "rmap put oom");
        }
        if ((it & 255) == 0) {
            uint32_t n = dyn_rmap_count(m);
            double phi = -1e300;
            for (i = 0; i < RS_UNIV; i++) {
                const dyn_cell_t *g = dyn_rmap_get(m, i);
                if (model[i] < 0)
                    CHECK(g == NULL, "rmap %u should be unmapped at it=%u",
                          i, it);
                else
                    CHECK(g && g->w[0] == (uint64_t)model[i],
                          "rmap %u got %lld want %d at it=%u", i,
                          g ? (long long)g->w[0] : -1LL, model[i], it);
            }
            for (i = 0; i < n; i++) {
                double l, h;
                const dyn_cell_t *v;
                dyn_rmap_at(m, i, &l, &h, &v);
                CHECK(h > l, "rmap empty span");
                CHECK(l >= phi, "rmap spans overlap/unsorted");
                phi = h;
            }
        }
    }
    /* Every live reference must be released exactly once at teardown, and the
     * split path must have dup'd exactly as often as it created a second
     * span. */
    dyn_rmap_free(m, rm_freed, NULL);
    {
        uint32_t leaked = 0, over = 0;
        for (i = 0; i < next_id; i++) {
            if (rm_live[i] > 0) leaked++;
            if (rm_live[i] < 0) over++;
        }
        CHECK(leaked == 0, "%u rangemap cells leaked", leaked);
        CHECK(over == 0, "%u rangemap cells over-released", over);
    }
    printf("rangemap ok\n");
}

/* -------------------------------------------------------------- IntervalTree */

static void test_itree(void)
{
    struct { double lo, hi; uint64_t id; } model[400];
    uint32_t nmodel = 0;
    dyn_itree_t *t = dyn_itree_new();
    uint32_t it, i, j;
    uint32_t out[512];

    for (it = 0; it < 30000; it++) {
        if (nmodel < 400 && rnd_n(3) != 0) {
            double lo = (double)rnd_n(1000);
            double hi = lo + (double)rnd_n(50);
            dyn_cell_t c = cell(it);
            CHECK(dyn_itree_insert(t, lo, hi, &c) == 0, "itree oom");
            model[nmodel].lo = lo;
            model[nmodel].hi = hi;
            model[nmodel].id = it;
            nmodel++;
        }
        if ((it & 63) == 0 && nmodel) {
            double qlo = (double)rnd_n(1000);
            double qhi = qlo + (double)rnd_n(80);
            uint32_t want = 0, got;
            uint64_t want_mask = 0, got_mask = 0;
            for (i = 0; i < nmodel; i++)
                if (model[i].lo <= qhi && model[i].hi >= qlo) {
                    want++;
                    want_mask += model[i].id;
                }
            got = dyn_itree_query(t, qlo, qhi, out, 512);
            CHECK(got == want, "itree query %g..%g got %u want %u",
                  qlo, qhi, got, want);
            for (j = 0; j < got && j < 512; j++) {
                double l, h;
                const dyn_cell_t *v;
                CHECK(dyn_itree_at(t, out[j], &l, &h, &v), "itree at");
                CHECK(l <= qhi && h >= qlo, "itree returned non-overlap");
                got_mask += v->w[0];
            }
            if (got <= 512)
                CHECK(got_mask == want_mask, "itree wrong members");
        }
        CHECK(dyn_itree_size(t) == nmodel, "itree size");
    }
    dyn_itree_free(t, NULL, NULL);
    printf("itree ok (%u intervals)\n", nmodel);
}

/* --------------------------------------------------------------- MinMaxHeap */

static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void test_mmheap(void)
{
    double model[2048];
    uint32_t nmodel = 0;
    dyn_mmheap_t *h = dyn_mmheap_new();
    uint32_t it;

    for (it = 0; it < 300000; it++) {
        double pri;
        dyn_cell_t c;
        if (nmodel == 0 || (nmodel < 2048 && rnd_n(3) != 0)) {
            pri = (double)(int32_t)(rnd() & 0xffff) - 32768.0;
            c = cell(it);
            CHECK(dyn_mmheap_push(h, pri, &c) == 0, "mmheap oom");
            model[nmodel++] = pri;
        } else {
            double got;
            uint32_t k;
            qsort(model, nmodel, sizeof(double), dcmp);
            if (rnd_n(2)) {
                CHECK(dyn_mmheap_pop_min(h, &got, &c), "pop_min empty");
                CHECK(got == model[0], "pop_min %g want %g (n=%u)",
                      got, model[0], nmodel);
                for (k = 0; k + 1 < nmodel; k++) model[k] = model[k + 1];
            } else {
                CHECK(dyn_mmheap_pop_max(h, &got, &c), "pop_max empty");
                CHECK(got == model[nmodel - 1], "pop_max %g want %g (n=%u)",
                      got, model[nmodel - 1], nmodel);
            }
            nmodel--;
        }
        CHECK(dyn_mmheap_size(h) == nmodel, "mmheap size");
        if ((it & 511) == 0 && nmodel) {
            double pmin, pmax;
            const dyn_cell_t *v;
            qsort(model, nmodel, sizeof(double), dcmp);
            CHECK(dyn_mmheap_peek_min(h, &pmin, &v) && pmin == model[0],
                  "peek_min");
            CHECK(dyn_mmheap_peek_max(h, &pmax, &v) &&
                  pmax == model[nmodel - 1], "peek_max");
        }
    }
    dyn_mmheap_free(h, NULL, NULL);
    printf("mmheap ok\n");
}

/* -------------------------------------------------------------------- BiMap */

static void test_bimap(void)
{
    int kv[64], vk[64];    /* model: key i -> value kv[i], or -1 */
    dyn_bimap_t *b = dyn_bimap_new();
    char ks[32], vs[32];
    uint32_t it, i;

    for (i = 0; i < 64; i++) { kv[i] = -1; vk[i] = -1; }
    for (it = 0; it < 200000; it++) {
        uint32_t k = rnd_n(64), v = rnd_n(64);
        size_t kl = (size_t)snprintf(ks, sizeof(ks), "k%u", k);
        size_t vl = (size_t)snprintf(vs, sizeof(vs), "v%u", v);
        uint32_t op = rnd_n(10);

        if (op < 5) {                       /* put, non-forcing */
            int r = dyn_bimap_put(b, ks, kl, vs, vl, 0);
            if (vk[v] >= 0 && vk[v] != (int)k) {
                CHECK(r == DYN_BIMAP_VALUE_TAKEN, "put should refuse");
            } else {
                CHECK(r == 0, "put failed %d", r);
                if (kv[k] >= 0) vk[kv[k]] = -1;
                kv[k] = (int)v;
                vk[v] = (int)k;
            }
        } else if (op < 8) {                /* forcePut */
            CHECK(dyn_bimap_put(b, ks, kl, vs, vl, 1) == 0, "forcePut failed");
            if (vk[v] >= 0 && vk[v] != (int)k) kv[vk[v]] = -1;
            if (kv[k] >= 0) vk[kv[k]] = -1;
            kv[k] = (int)v;
            vk[v] = (int)k;
        } else if (op == 8) {               /* remove by key */
            int r = dyn_bimap_remove(b, ks, kl);
            CHECK(r == (kv[k] >= 0), "remove by key");
            if (kv[k] >= 0) { vk[kv[k]] = -1; kv[k] = -1; }
        } else {                            /* remove by value */
            int r = dyn_bimap_remove_value(b, vs, vl);
            CHECK(r == (vk[v] >= 0), "remove by value");
            if (vk[v] >= 0) { kv[vk[v]] = -1; vk[v] = -1; }
        }

        if ((it & 511) == 0) {
            uint32_t live = 0;
            for (i = 0; i < 64; i++) {
                size_t l = (size_t)snprintf(ks, sizeof(ks), "k%u", i), gn;
                const char *g = dyn_bimap_get(b, ks, l, &gn);
                if (kv[i] < 0) {
                    CHECK(g == NULL, "bimap k%u should be absent", i);
                } else {
                    char want[32];
                    size_t wl = (size_t)snprintf(want, sizeof(want), "v%d",
                                                 kv[i]);
                    CHECK(g && gn == wl && memcmp(g, want, wl) == 0,
                          "bimap k%u -> %.*s want %s", i, (int)gn,
                          g ? g : "", want);
                    live++;
                }
                l = (size_t)snprintf(vs, sizeof(vs), "v%u", i);
                g = dyn_bimap_key_for(b, vs, l, &gn);
                CHECK((g != NULL) == (vk[i] >= 0), "bimap inverse v%u", i);
            }
            CHECK(dyn_bimap_size(b) == live, "bimap size %u vs %u",
                  dyn_bimap_size(b), live);
        }
    }
    dyn_bimap_free(b);
    printf("bimap ok\n");
}

/* -------------------------------------------------------------------- Table */

static void test_table(void)
{
    int model[16][16];
    dyn_table_t *t = dyn_table_new();
    char rs[16], cs[16];
    uint32_t it, r, c;
    uint32_t live = 0;

    memset(model, 0, sizeof(model));
    for (it = 1; it < 200000; it++) {
        uint32_t rr = rnd_n(16), cc = rnd_n(16);
        size_t rl = (size_t)snprintf(rs, sizeof(rs), "r%u", rr);
        size_t cl = (size_t)snprintf(cs, sizeof(cs), "c%u", cc);
        dyn_cell_t v = cell(it), old;

        if (rnd_n(4) == 0) {
            int had = dyn_table_remove(t, rs, rl, cs, cl, &old);
            CHECK(had == (model[rr][cc] != 0), "table remove");
            if (had) {
                CHECK(old.w[0] == (uint64_t)model[rr][cc], "table removed val");
                live--;
            }
            model[rr][cc] = 0;
        } else {
            int rep = dyn_table_put(t, rs, rl, cs, cl, &v, &old);
            CHECK(rep >= 0, "table put oom");
            CHECK(rep == (model[rr][cc] != 0), "table put replace flag");
            if (rep)
                CHECK(old.w[0] == (uint64_t)model[rr][cc], "table old val");
            else
                live++;
            model[rr][cc] = (int)it;
        }
        if ((it & 511) == 0) {
            CHECK(dyn_table_size(t) == live, "table size %u vs %u",
                  dyn_table_size(t), live);
            for (r = 0; r < 16; r++)
                for (c = 0; c < 16; c++) {
                    const dyn_cell_t *g;
                    size_t l1 = (size_t)snprintf(rs, sizeof(rs), "r%u", r);
                    size_t l2 = (size_t)snprintf(cs, sizeof(cs), "c%u", c);
                    g = dyn_table_get(t, rs, l1, cs, l2);
                    if (model[r][c] == 0)
                        CHECK(g == NULL, "table (%u,%u) absent", r, c);
                    else
                        CHECK(g && g->w[0] == (uint64_t)model[r][c],
                              "table (%u,%u)", r, c);
                }
        }
    }
    dyn_table_free(t, NULL, NULL);
    printf("table ok\n");
}

/* ----------------------------------------------------------------- Multimap */

static int mm_live_count;
static void mm_freed(void *ud, const dyn_cell_t *c)
{
    (void)ud; (void)c;
    mm_live_count--;
}

static void test_multimap(void)
{
    dyn_mmap_t *m = dyn_mmap_new();
    uint64_t model[16][64];
    uint32_t mlen[16];
    uint32_t it, i, j;

    memset(mlen, 0, sizeof(mlen));
    mm_live_count = 0;
    for (it = 1; it < 200000; it++) {
        uint32_t k = rnd_n(16);
        char kb[16];
        size_t kl = (size_t)snprintf(kb, sizeof(kb), "k%u", k);
        uint32_t op = rnd_n(10);

        if (op < 7 && mlen[k] < 64) {
            dyn_cell_t c = cell(it);
            CHECK(dyn_mmap_put(m, kb, kl, &c) == 0, "mmap oom");
            model[k][mlen[k]++] = it;
            mm_live_count++;
        } else if (op < 9 && mlen[k]) {
            uint32_t idx = rnd_n(mlen[k]);
            dyn_cell_t out;
            CHECK(dyn_mmap_remove_at(m, kb, kl, idx, &out) == 1, "remove_at");
            CHECK(out.w[0] == model[k][idx], "remove_at value");
            for (i = idx; i + 1 < mlen[k]; i++) model[k][i] = model[k][i + 1];
            mlen[k]--;
            mm_live_count--;
        } else {
            uint32_t n = dyn_mmap_remove_key(m, kb, kl, mm_freed, NULL);
            CHECK(n == mlen[k], "remove_key %u vs %u", n, mlen[k]);
            mlen[k] = 0;
        }
        if ((it & 511) == 0) {
            uint64_t total = 0;
            uint32_t keys = 0;
            for (i = 0; i < 16; i++) {
                char b2[16];
                size_t l = (size_t)snprintf(b2, sizeof(b2), "k%u", i);
                CHECK(dyn_mmap_count(m, b2, l) == mlen[i], "mmap count k%u", i);
                for (j = 0; j < mlen[i]; j++) {
                    const dyn_cell_t *c = dyn_mmap_at(m, b2, l, j);
                    CHECK(c && c->w[0] == model[i][j], "mmap at k%u[%u]", i, j);
                }
                total += mlen[i];
                if (mlen[i]) keys++;
            }
            CHECK(dyn_mmap_size(m) == total, "mmap size");
            CHECK(dyn_mmap_keys(m) == keys, "mmap keys");
        }
    }
    dyn_mmap_free(m, mm_freed, NULL);
    CHECK(mm_live_count == 0, "multimap leaked %d cells", mm_live_count);
    printf("multimap ok\n");
}

/* ---------------------------------------------------- sketches (statistical) */

static void test_sketches(void)
{
    dyn_cms_t *s = dyn_cms_new(2048, 5);
    dyn_hll_t *h = dyn_hll_new(14);
    char kb[32];
    uint32_t i;
    double est, err;

    /* CountMinSketch never under-counts, by construction. */
    for (i = 0; i < 20000; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "item%u", i % 500);
        dyn_cms_add(s, kb, l, 1);
    }
    for (i = 0; i < 500; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "item%u", i);
        CHECK(dyn_cms_count(s, kb, l) >= 40, "cms under-counted item%u", i);
    }
    CHECK(dyn_cms_total(s) == 20000, "cms total");

    /* Merge must be equivalent to adding into one sketch. */
    {
        dyn_cms_t *a = dyn_cms_new(512, 4), *b = dyn_cms_new(512, 4);
        dyn_cms_t *both = dyn_cms_new(512, 4);
        for (i = 0; i < 1000; i++) {
            size_t l = (size_t)snprintf(kb, sizeof(kb), "x%u", i);
            dyn_cms_add(i & 1 ? a : b, kb, l, i);
            dyn_cms_add(both, kb, l, i);
        }
        CHECK(dyn_cms_merge(a, b) == 0, "cms merge");
        CHECK(memcmp(dyn_cms_counters(a), dyn_cms_counters(both),
                     512 * 4 * sizeof(uint64_t)) == 0, "cms merge != direct");
        CHECK(dyn_cms_merge(a, s) == -1, "cms merge dimension mismatch");
        dyn_cms_free(a); dyn_cms_free(b); dyn_cms_free(both);
    }

    /* HyperLogLog at p=14 has a standard error of 1.04/sqrt(2^14) = 0.81%;
     * 3 sigma is 2.4%, and 5% is a generous non-flaky bound. */
    for (i = 0; i < 100000; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "uniq-%u", i);
        dyn_hll_add(h, kb, l);
    }
    est = dyn_hll_count(h);
    err = fabs(est - 100000.0) / 100000.0;
    CHECK(err < 0.05, "hll estimate %.0f for 100000 (err %.3f)", est, err);
    printf("hll(100000) = %.0f, relative error %.4f\n", est, err);

    /* Small cardinality is the linear-counting regime. */
    {
        dyn_hll_t *g = dyn_hll_new(14);
        for (i = 0; i < 100; i++) {
            size_t l = (size_t)snprintf(kb, sizeof(kb), "s-%u", i);
            dyn_hll_add(g, kb, l);
        }
        est = dyn_hll_count(g);
        CHECK(fabs(est - 100.0) / 100.0 < 0.05, "hll small %.1f", est);
        printf("hll(100) = %.1f\n", est);
        dyn_hll_free(g);
    }
    /* Merge = union of the two key sets. */
    {
        dyn_hll_t *a = dyn_hll_new(12), *b = dyn_hll_new(12);
        for (i = 0; i < 30000; i++) {
            size_t l = (size_t)snprintf(kb, sizeof(kb), "m-%u", i);
            dyn_hll_add(i < 20000 ? a : b, kb, l);
        }
        CHECK(dyn_hll_merge(a, b) == 0, "hll merge");
        est = dyn_hll_count(a);
        CHECK(fabs(est - 30000.0) / 30000.0 < 0.06, "hll merged %.0f", est);
        CHECK(dyn_hll_merge(a, h) == -1, "hll merge precision mismatch");
        dyn_hll_free(a); dyn_hll_free(b);
    }
    CHECK(dyn_hll_new(3) == NULL && dyn_hll_new(19) == NULL,
          "hll precision bounds");
    dyn_cms_free(s);
    dyn_hll_free(h);
    printf("sketches ok\n");
}

int main(void)
{
    test_multiset();
    test_multimap();
    test_bimap();
    test_table();
    test_rangeset();
    test_rangemap();
    test_itree();
    test_mmheap();
    test_sketches();
    printf("%s: %ld checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
