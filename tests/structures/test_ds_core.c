#include "dyn-ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t rng_state = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}
static uint32_t rnd_n(uint32_t n) { return (uint32_t)(rnd() % n); }

static int failures = 0;
static long checks = 0;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            if (++failures > 20) {                      \
                printf("too many\n");                   \
                exit(1);                                \
            }                                           \
        }                                               \
    } while (0)

static dyn_cell_t cell(uint64_t v)
{
    dyn_cell_t c;
    c.w[0] = v;
    c.w[1] = ~v;
    return c;
}

#define MS_KEYS 64

static void test_multiset(void)
{
    uint64_t model[MS_KEYS];
    dyn_mset_t* m = dyn_mset_new();
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
    {
        uint64_t seen[MS_KEYS];
        const char* kp;
        size_t kl;
        uint64_t c;
        memset(seen, 0, sizeof(seen));
        for (i = 0; dyn_mset_at(m, i, &kp, &kl, &c); i++) {
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

#define RS_UNIV 200

static void test_rangeset(void)
{
    unsigned char model[RS_UNIV];
    dyn_rset_t* s = dyn_rset_new();
    uint32_t it, i;

    memset(model, 0, sizeof(model));
    for (it = 0; it < 200000; it++) {
        uint32_t a = rnd_n(RS_UNIV), b = rnd_n(RS_UNIV);
        uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
        int add = rnd_n(2);

        if (add) {
            for (i = lo; i < hi; i++)
                model[i] = 1;
            CHECK(dyn_rset_add(s, lo, hi) == 0, "rset add oom");
        } else {
            for (i = lo; i < hi; i++)
                model[i] = 0;
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
            for (i = 0; i < n; i++) {
                double l, h;
                dyn_rset_at(s, i, &l, &h);
                CHECK(h > l, "empty span");
                if (i)
                    CHECK(l > phi, "spans not coalesced: %g..%g then %g..%g",
                        plo, phi, l, h);
                plo = l;
                phi = h;
            }
            for (i = 0; i < 40; i++) {
                uint32_t x = rnd_n(RS_UNIV), y = rnd_n(RS_UNIV);
                uint32_t l = x < y ? x : y, h = x < y ? y : x;
                int enc = 1, isect = 0, j;
                for (j = (int)l; j < (int)h; j++) {
                    if (model[j])
                        isect = 1;
                    else
                        enc = 0;
                }
                if (h == l) {
                    enc = 1;
                    isect = 0;
                }
                CHECK(dyn_rset_encloses(s, l, h) == enc, "encloses %u..%u", l, h);
                CHECK(dyn_rset_intersects(s, l, h) == isect,
                    "intersects %u..%u", l, h);
            }
        }
    }
    {
        dyn_rset_t* c = dyn_rset_new();
        CHECK(dyn_rset_complement(s, 0, RS_UNIV, c) == 0, "complement oom");
        for (i = 0; i < RS_UNIV; i++)
            CHECK(dyn_rset_contains(c, i) == !model[i], "complement %u", i);
        dyn_rset_free(c);
    }
    dyn_rset_free(s);
    printf("rangeset ok\n");
}

static int rm_live[1 << 20];
static void rm_freed(void* ud, const dyn_cell_t* c)
{
    (void)ud;
    rm_live[c->w[0]]--;
}
static void rm_dup(void* ud, const dyn_cell_t* in, dyn_cell_t* out)
{
    (void)ud;
    rm_live[in->w[0]]++;
    *out = *in;
}

static void test_rangemap(void)
{
    int model[RS_UNIV];
    dyn_rmap_t* m = dyn_rmap_new();
    uint32_t it, i;
    uint32_t next_id = 1;

    for (i = 0; i < RS_UNIV; i++)
        model[i] = -1;
    memset(rm_live, 0, sizeof(rm_live));

    for (it = 0; it < 120000; it++) {
        uint32_t a = rnd_n(RS_UNIV), b = rnd_n(RS_UNIV);
        uint32_t lo = a < b ? a : b, hi = a < b ? b : a;

        if (rnd_n(4) == 0) {
            for (i = lo; i < hi; i++)
                model[i] = -1;
            CHECK(dyn_rmap_remove(m, lo, hi, rm_dup, rm_freed, NULL) == 0,
                "rmap remove oom");
        } else {
            uint32_t id = next_id++;
            dyn_cell_t c = cell(id);
            if (hi > lo) {
                rm_live[id] = 1;
                for (i = lo; i < hi; i++)
                    model[i] = (int)id;
            }
            CHECK(dyn_rmap_put(m, lo, hi, &c, rm_dup, rm_freed, NULL) == 0,
                "rmap put oom");
        }
        if ((it & 255) == 0) {
            uint32_t n = dyn_rmap_count(m);
            double phi = -1e300;
            for (i = 0; i < RS_UNIV; i++) {
                const dyn_cell_t* g = dyn_rmap_get(m, i);
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
                const dyn_cell_t* v;
                dyn_rmap_at(m, i, &l, &h, &v);
                CHECK(h > l, "rmap empty span");
                CHECK(l >= phi, "rmap spans overlap/unsorted");
                phi = h;
            }
        }
    }
    dyn_rmap_free(m, rm_freed, NULL);
    {
        uint32_t leaked = 0, over = 0;
        for (i = 0; i < next_id; i++) {
            if (rm_live[i] > 0)
                leaked++;
            if (rm_live[i] < 0)
                over++;
        }
        CHECK(leaked == 0, "%u rangemap cells leaked", leaked);
        CHECK(over == 0, "%u rangemap cells over-released", over);
    }
    printf("rangemap ok\n");
}

static void test_itree(void)
{
    struct {
        double lo, hi;
        uint64_t id;
    } model[400];
    uint32_t nmodel = 0;
    dyn_itree_t* t = dyn_itree_new();
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
                const dyn_cell_t* v;
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

static int dcmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void test_mmheap(void)
{
    double model[2048];
    uint32_t nmodel = 0;
    dyn_mmheap_t* h = dyn_mmheap_new();
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
                for (k = 0; k + 1 < nmodel; k++)
                    model[k] = model[k + 1];
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
            const dyn_cell_t* v;
            qsort(model, nmodel, sizeof(double), dcmp);
            CHECK(dyn_mmheap_peek_min(h, &pmin, &v) && pmin == model[0],
                "peek_min");
            CHECK(dyn_mmheap_peek_max(h, &pmax, &v) && pmax == model[nmodel - 1], "peek_max");
        }
    }
    dyn_mmheap_free(h, NULL, NULL);
    printf("mmheap ok\n");
}

static void test_bimap(void)
{
    int kv[64], vk[64];
    dyn_bimap_t* b = dyn_bimap_new();
    char ks[32], vs[32];
    uint32_t it, i;

    for (i = 0; i < 64; i++) {
        kv[i] = -1;
        vk[i] = -1;
    }
    for (it = 0; it < 200000; it++) {
        uint32_t k = rnd_n(64), v = rnd_n(64);
        size_t kl = (size_t)snprintf(ks, sizeof(ks), "k%u", k);
        size_t vl = (size_t)snprintf(vs, sizeof(vs), "v%u", v);
        uint32_t op = rnd_n(10);

        if (op < 5) {
            int r = dyn_bimap_put(b, ks, kl, vs, vl, 0);
            if (vk[v] >= 0 && vk[v] != (int)k) {
                CHECK(r == DYN_BIMAP_VALUE_TAKEN, "put should refuse");
            } else {
                CHECK(r == 0, "put failed %d", r);
                if (kv[k] >= 0)
                    vk[kv[k]] = -1;
                kv[k] = (int)v;
                vk[v] = (int)k;
            }
        } else if (op < 8) {
            CHECK(dyn_bimap_put(b, ks, kl, vs, vl, 1) == 0, "forcePut failed");
            if (vk[v] >= 0 && vk[v] != (int)k)
                kv[vk[v]] = -1;
            if (kv[k] >= 0)
                vk[kv[k]] = -1;
            kv[k] = (int)v;
            vk[v] = (int)k;
        } else if (op == 8) {
            int r = dyn_bimap_remove(b, ks, kl);
            CHECK(r == (kv[k] >= 0), "remove by key");
            if (kv[k] >= 0) {
                vk[kv[k]] = -1;
                kv[k] = -1;
            }
        } else {
            int r = dyn_bimap_remove_value(b, vs, vl);
            CHECK(r == (vk[v] >= 0), "remove by value");
            if (vk[v] >= 0) {
                kv[vk[v]] = -1;
                vk[v] = -1;
            }
        }

        if ((it & 511) == 0) {
            uint32_t live = 0;
            for (i = 0; i < 64; i++) {
                size_t l = (size_t)snprintf(ks, sizeof(ks), "k%u", i), gn;
                const char* g = dyn_bimap_get(b, ks, l, &gn);
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

static void test_table(void)
{
    int model[16][16];
    dyn_table_t* t = dyn_table_new();
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
                    const dyn_cell_t* g;
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

static int mm_live_count;
static void mm_freed(void* ud, const dyn_cell_t* c)
{
    (void)ud;
    (void)c;
    mm_live_count--;
}

static void test_multimap(void)
{
    dyn_mmap_t* m = dyn_mmap_new();
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
            for (i = idx; i + 1 < mlen[k]; i++)
                model[k][i] = model[k][i + 1];
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
                    const dyn_cell_t* c = dyn_mmap_at(m, b2, l, j);
                    CHECK(c && c->w[0] == model[i][j], "mmap at k%u[%u]", i, j);
                }
                total += mlen[i];
                if (mlen[i])
                    keys++;
            }
            CHECK(dyn_mmap_size(m) == total, "mmap size");
            CHECK(dyn_mmap_keys(m) == keys, "mmap keys");
        }
    }
    dyn_mmap_free(m, mm_freed, NULL);
    CHECK(mm_live_count == 0, "multimap leaked %d cells", mm_live_count);
    printf("multimap ok\n");
}

static void test_sketches(void)
{
    dyn_cms_t* s = dyn_cms_new(2048, 5);
    dyn_hll_t* h = dyn_hll_new(14);
    char kb[32];
    uint32_t i;
    double est, err;

    for (i = 0; i < 20000; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "item%u", i % 500);
        dyn_cms_add(s, kb, l, 1);
    }
    for (i = 0; i < 500; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "item%u", i);
        CHECK(dyn_cms_count(s, kb, l) >= 40, "cms under-counted item%u", i);
    }
    CHECK(dyn_cms_total(s) == 20000, "cms total");

    {
        dyn_cms_t *a = dyn_cms_new(512, 4), *b = dyn_cms_new(512, 4);
        dyn_cms_t* both = dyn_cms_new(512, 4);
        for (i = 0; i < 1000; i++) {
            size_t l = (size_t)snprintf(kb, sizeof(kb), "x%u", i);
            dyn_cms_add(i & 1 ? a : b, kb, l, i);
            dyn_cms_add(both, kb, l, i);
        }
        CHECK(dyn_cms_merge(a, b) == 0, "cms merge");
        CHECK(memcmp(dyn_cms_counters(a), dyn_cms_counters(both),
                  512 * 4 * sizeof(uint64_t))
                == 0,
            "cms merge != direct");
        CHECK(dyn_cms_merge(a, s) == -1, "cms merge dimension mismatch");
        dyn_cms_free(a);
        dyn_cms_free(b);
        dyn_cms_free(both);
    }

    for (i = 0; i < 100000; i++) {
        size_t l = (size_t)snprintf(kb, sizeof(kb), "uniq-%u", i);
        dyn_hll_add(h, kb, l);
    }
    est = dyn_hll_count(h);
    err = fabs(est - 100000.0) / 100000.0;
    CHECK(err < 0.05, "hll estimate %.0f for 100000 (err %.3f)", est, err);
    printf("hll(100000) = %.0f, relative error %.4f\n", est, err);

    {
        dyn_hll_t* g = dyn_hll_new(14);
        for (i = 0; i < 100; i++) {
            size_t l = (size_t)snprintf(kb, sizeof(kb), "s-%u", i);
            dyn_hll_add(g, kb, l);
        }
        est = dyn_hll_count(g);
        CHECK(fabs(est - 100.0) / 100.0 < 0.05, "hll small %.1f", est);
        printf("hll(100) = %.1f\n", est);
        dyn_hll_free(g);
    }
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
        dyn_hll_free(a);
        dyn_hll_free(b);
    }
    CHECK(dyn_hll_new(3) == NULL && dyn_hll_new(19) == NULL,
        "hll precision bounds");
    dyn_cms_free(s);
    dyn_hll_free(h);
    printf("sketches ok\n");
}

/* XXH64 vectors: Cyan4973/xxHash tests/sanity_test_vectors.h (XSUM_XXH64_testdata, every length
 * 0..259 at seeds 0 and PRIME32), over the buffer tests/sanity_test.c fillTestBuffer() builds. */
static const struct {
    size_t len;
    uint64_t seed;
    uint64_t want;
} xxh64_vectors[] = {
    { 0, 0x0000000000000000ULL, 0xEF46DB3751D8E999ULL },
    { 0, 0x000000009E3779B1ULL, 0xAC75FDA2929B17EFULL },
    { 1, 0x0000000000000000ULL, 0xE934A84ADB052768ULL },
    { 1, 0x000000009E3779B1ULL, 0x5014607643A9B4C3ULL },
    { 2, 0x0000000000000000ULL, 0x5D48CD60A77E23FFULL },
    { 2, 0x000000009E3779B1ULL, 0x9E93152232D54A39ULL },
    { 3, 0x0000000000000000ULL, 0xFF7E1959CB50794AULL },
    { 3, 0x000000009E3779B1ULL, 0xAA8584E83660F7D1ULL },
    { 4, 0x0000000000000000ULL, 0x9136A0DCA57457EEULL },
    { 4, 0x000000009E3779B1ULL, 0xCAAB286BD8E9FDB5ULL },
    { 5, 0x0000000000000000ULL, 0x9B046FB1397F09A5ULL },
    { 5, 0x000000009E3779B1ULL, 0x2AF5249930F984ECULL },
    { 6, 0x0000000000000000ULL, 0xC72565B7154268A8ULL },
    { 6, 0x000000009E3779B1ULL, 0xCA4C6723580E8EF6ULL },
    { 7, 0x0000000000000000ULL, 0x6C83909A9F01ED25ULL },
    { 7, 0x000000009E3779B1ULL, 0xF98D03B1AD6F9293ULL },
    { 8, 0x0000000000000000ULL, 0xCDBCF538E71D1348ULL },
    { 8, 0x000000009E3779B1ULL, 0xFE0C047A5353CDACULL },
    { 9, 0x0000000000000000ULL, 0x554B1AE991EDA6B6ULL },
    { 9, 0x000000009E3779B1ULL, 0x7908265248F6D73FULL },
    { 10, 0x0000000000000000ULL, 0x5D00E7351392EA84ULL },
    { 10, 0x000000009E3779B1ULL, 0x2A8AE16B86CD2F12ULL },
    { 11, 0x0000000000000000ULL, 0x6345D5746F35DA70ULL },
    { 11, 0x000000009E3779B1ULL, 0xEAA08A8C8BE3CCCFULL },
    { 12, 0x0000000000000000ULL, 0x0723BF50086EAD9AULL },
    { 12, 0x000000009E3779B1ULL, 0x8252819F4E506951ULL },
    { 13, 0x0000000000000000ULL, 0xC2E5013E3C40BCF7ULL },
    { 13, 0x000000009E3779B1ULL, 0x4DF437A291CB1039ULL },
    { 14, 0x0000000000000000ULL, 0x8282DCC4994E35C8ULL },
    { 14, 0x000000009E3779B1ULL, 0xC3BD6BF63DEB6DF0ULL },
    { 15, 0x0000000000000000ULL, 0x180719316D622D84ULL },
    { 15, 0x000000009E3779B1ULL, 0xD61105C20E91F99FULL },
    { 16, 0x0000000000000000ULL, 0x98C90B57FDFCB55CULL },
    { 16, 0x000000009E3779B1ULL, 0xC900AD2D536B607EULL },
    { 17, 0x0000000000000000ULL, 0x0D39A2D051A30C2CULL },
    { 17, 0x000000009E3779B1ULL, 0x495CD68A647C7A22ULL },
    { 18, 0x0000000000000000ULL, 0x33E84A4333B2B2EBULL },
    { 18, 0x000000009E3779B1ULL, 0x2325A30CCA1A66DDULL },
    { 19, 0x0000000000000000ULL, 0xE91C6EF31FC08F82ULL },
    { 19, 0x000000009E3779B1ULL, 0x06809662799B7D6FULL },
    { 20, 0x0000000000000000ULL, 0x5F8C68355769439EULL },
    { 20, 0x000000009E3779B1ULL, 0x97218696C2D29602ULL },
    { 21, 0x0000000000000000ULL, 0x42B0B8EE353AC461ULL },
    { 21, 0x000000009E3779B1ULL, 0x7FC0BB451B83A633ULL },
    { 22, 0x0000000000000000ULL, 0x65C935C6978098B1ULL },
    { 22, 0x000000009E3779B1ULL, 0xC4A0DD14BF835C13ULL },
    { 23, 0x0000000000000000ULL, 0xD2460ECC840B74DDULL },
    { 23, 0x000000009E3779B1ULL, 0x4B44E8DE7A396773ULL },
    { 24, 0x0000000000000000ULL, 0xF75A6DEA42DC5BF4ULL },
    { 24, 0x000000009E3779B1ULL, 0x8B7C67EB59778E22ULL },
    { 25, 0x0000000000000000ULL, 0x52FAA43C3F20B994ULL },
    { 25, 0x000000009E3779B1ULL, 0xC4FEC92EAC2C3B8AULL },
    { 26, 0x0000000000000000ULL, 0x8DB7831EC345F9A3ULL },
    { 26, 0x000000009E3779B1ULL, 0x2C2A80BCAD321466ULL },
    { 27, 0x0000000000000000ULL, 0x88945AA08051FC2DULL },
    { 27, 0x000000009E3779B1ULL, 0x3401AF8EF28FD410ULL },
    { 28, 0x0000000000000000ULL, 0x64CD9E8C96A9E2DDULL },
    { 28, 0x000000009E3779B1ULL, 0x8160FB8C20B48287ULL },
    { 29, 0x0000000000000000ULL, 0x8C8F345B634AC2B9ULL },
    { 29, 0x000000009E3779B1ULL, 0x5A327C78E4AD6678ULL },
    { 30, 0x0000000000000000ULL, 0xE2677241D4C46CAFULL },
    { 30, 0x000000009E3779B1ULL, 0xB1B2B51C93AF4866ULL },
    { 31, 0x0000000000000000ULL, 0x299B39A290E6D783ULL },
    { 31, 0x000000009E3779B1ULL, 0xDA673D5FEB5C1D79ULL },
    { 32, 0x0000000000000000ULL, 0x18B216492BB44B70ULL },
    { 32, 0x000000009E3779B1ULL, 0xB3F33BDF93ADE409ULL },
    { 33, 0x0000000000000000ULL, 0x55C8DC3E578F5B59ULL },
    { 33, 0x000000009E3779B1ULL, 0xE92C292F64BC3071ULL },
    { 34, 0x0000000000000000ULL, 0xD0CE7CBAE371BEB2ULL },
    { 34, 0x000000009E3779B1ULL, 0xC509C460280BBB12ULL },
    { 35, 0x0000000000000000ULL, 0xCB0AA8E2A7A29707ULL },
    { 35, 0x000000009E3779B1ULL, 0xECE3515C236504C7ULL },
    { 36, 0x0000000000000000ULL, 0x0AE67584084DC6A4ULL },
    { 36, 0x000000009E3779B1ULL, 0x57C73F9E3A6FC2C6ULL },
    { 37, 0x0000000000000000ULL, 0x292F5332C92B73CEULL },
    { 37, 0x000000009E3779B1ULL, 0x7762425B2E89DAD3ULL },
    { 38, 0x0000000000000000ULL, 0x5F398E2A8478BED3ULL },
    { 38, 0x000000009E3779B1ULL, 0xE842D306DC53E621ULL },
    { 39, 0x0000000000000000ULL, 0xB888CAF07592B1B3ULL },
    { 39, 0x000000009E3779B1ULL, 0x602E74D43465AC3DULL },
    { 40, 0x0000000000000000ULL, 0x23B3810D7D8B1731ULL },
    { 40, 0x000000009E3779B1ULL, 0x5A57F246772BC540ULL },
    { 41, 0x0000000000000000ULL, 0x5342C966C7F3D5F4ULL },
    { 41, 0x000000009E3779B1ULL, 0xB213FFB25FEB1CCDULL },
    { 42, 0x0000000000000000ULL, 0x77454B74F1DBBF63ULL },
    { 42, 0x000000009E3779B1ULL, 0xAED0CE7EA4202741ULL },
    { 43, 0x0000000000000000ULL, 0x9904C9350F597190ULL },
    { 43, 0x000000009E3779B1ULL, 0xA071200735E57A6AULL },
    { 44, 0x0000000000000000ULL, 0x24EC2B3D0E442641ULL },
    { 44, 0x000000009E3779B1ULL, 0x831B7452E64EB328ULL },
    { 45, 0x0000000000000000ULL, 0x4EC307EBE0A650A4ULL },
    { 45, 0x000000009E3779B1ULL, 0x66BDA694A1FD3023ULL },
    { 46, 0x0000000000000000ULL, 0x7DD1C5514B2240EFULL },
    { 46, 0x000000009E3779B1ULL, 0xD238E617805EA9BBULL },
    { 47, 0x0000000000000000ULL, 0x84A42E167CBD1FEFULL },
    { 47, 0x000000009E3779B1ULL, 0xA474B31A5132A541ULL },
    { 48, 0x0000000000000000ULL, 0xFD0FEEAC7A939933ULL },
    { 48, 0x000000009E3779B1ULL, 0x6FFE2F43A24C2302ULL },
    { 49, 0x0000000000000000ULL, 0xA91BFF78A7185B66ULL },
    { 49, 0x000000009E3779B1ULL, 0xE858B13C918498ECULL },
    { 50, 0x0000000000000000ULL, 0xDFFE9F293DDD101DULL },
    { 50, 0x000000009E3779B1ULL, 0x46521A048859CE7AULL },
    { 51, 0x0000000000000000ULL, 0x2332585C18D7483AULL },
    { 51, 0x000000009E3779B1ULL, 0x788846B44826A5D1ULL },
    { 52, 0x0000000000000000ULL, 0x5A0056511F9DD653ULL },
    { 52, 0x000000009E3779B1ULL, 0x1C0081F84C6D32DBULL },
    { 53, 0x0000000000000000ULL, 0x4426A2F5C8D59469ULL },
    { 53, 0x000000009E3779B1ULL, 0x4270188315F2CC31ULL },
    { 54, 0x0000000000000000ULL, 0x39EAF8893520636AULL },
    { 54, 0x000000009E3779B1ULL, 0xB6D6E8B68C7EF7A2ULL },
    { 55, 0x0000000000000000ULL, 0xAFEA98CC4E92F7E1ULL },
    { 55, 0x000000009E3779B1ULL, 0xE6D5C88B9135E579ULL },
    { 56, 0x0000000000000000ULL, 0xB7A6414B8D3E2597ULL },
    { 56, 0x000000009E3779B1ULL, 0xB6BD2CBFB6A64017ULL },
    { 57, 0x0000000000000000ULL, 0xF33D64569CD4B1C0ULL },
    { 57, 0x000000009E3779B1ULL, 0xB9B235447E24CA28ULL },
    { 58, 0x0000000000000000ULL, 0x93FA072F0E8FE7CAULL },
    { 58, 0x000000009E3779B1ULL, 0xB8D0D6211B80EA15ULL },
    { 59, 0x0000000000000000ULL, 0x4F2890576949D6EDULL },
    { 59, 0x000000009E3779B1ULL, 0x11E586B470B7224EULL },
    { 60, 0x0000000000000000ULL, 0x3378C44F459C290BULL },
    { 60, 0x000000009E3779B1ULL, 0xA7C18F722446E5C8ULL },
    { 61, 0x0000000000000000ULL, 0x40D486FDDDAD5CB7ULL },
    { 61, 0x000000009E3779B1ULL, 0x980BDE3212E2F025ULL },
    { 62, 0x0000000000000000ULL, 0x21664C60C551CC87ULL },
    { 62, 0x000000009E3779B1ULL, 0x128AC6F704B8A0F7ULL },
    { 63, 0x0000000000000000ULL, 0xA9EFBE0FA0F3F4E7ULL },
    { 63, 0x000000009E3779B1ULL, 0x6C911FADB05B6FC2ULL },
    { 64, 0x0000000000000000ULL, 0xEF558F8ACAC2B5CDULL },
    { 64, 0x000000009E3779B1ULL, 0xB5EEBA99264CC44FULL },
    { 65, 0x0000000000000000ULL, 0xDE0F20DC2631AF7AULL },
    { 65, 0x000000009E3779B1ULL, 0xD3F6FF3941E310CAULL },
    { 66, 0x0000000000000000ULL, 0xCF1E52EDE1C505C4ULL },
    { 66, 0x000000009E3779B1ULL, 0xBCE7B3488A29EFB1ULL },
    { 67, 0x0000000000000000ULL, 0x0965DF7219D2E741ULL },
    { 67, 0x000000009E3779B1ULL, 0x1862E884ABDC704CULL },
    { 68, 0x0000000000000000ULL, 0x1B8378E923B247A7ULL },
    { 68, 0x000000009E3779B1ULL, 0x25E6F985E36492EFULL },
    { 69, 0x0000000000000000ULL, 0x43F2B606AD9BA362ULL },
    { 69, 0x000000009E3779B1ULL, 0x81F00574A346F668ULL },
    { 70, 0x0000000000000000ULL, 0xD9C3413222F1DEA4ULL },
    { 70, 0x000000009E3779B1ULL, 0xE01652426AA1774CULL },
    { 71, 0x0000000000000000ULL, 0x3D6ECAB2BCFBE3FFULL },
    { 71, 0x000000009E3779B1ULL, 0xAAB5C313DFAC5A44ULL },
    { 72, 0x0000000000000000ULL, 0xEA8573B60D5A8800ULL },
    { 72, 0x000000009E3779B1ULL, 0xBDEE6BA43947EB30ULL },
    { 73, 0x0000000000000000ULL, 0xE372599E31F8CFFDULL },
    { 73, 0x000000009E3779B1ULL, 0xF1597384DE9FBD93ULL },
    { 74, 0x0000000000000000ULL, 0x02B86794A00BDBE8ULL },
    { 74, 0x000000009E3779B1ULL, 0x5FB940B90D39A1E6ULL },
    { 75, 0x0000000000000000ULL, 0xCA1A21FB24979810ULL },
    { 75, 0x000000009E3779B1ULL, 0x76214D8E3B169CF5ULL },
    { 76, 0x0000000000000000ULL, 0x60DFF17BEC07766DULL },
    { 76, 0x000000009E3779B1ULL, 0x9A8133A1DDAF07DBULL },
    { 77, 0x0000000000000000ULL, 0xF4AF74828DE6862DULL },
    { 77, 0x000000009E3779B1ULL, 0xD73AEC54F785E384ULL },
    { 78, 0x0000000000000000ULL, 0x828E4C8C5257CC15ULL },
    { 78, 0x000000009E3779B1ULL, 0xD2317E57923E664CULL },
    { 79, 0x0000000000000000ULL, 0x91F7F9D6008299A0ULL },
    { 79, 0x000000009E3779B1ULL, 0xE863995625F7AD52ULL },
    { 80, 0x0000000000000000ULL, 0x99BD5D25EB211099ULL },
    { 80, 0x000000009E3779B1ULL, 0x5281D5357D0B8AC4ULL },
    { 81, 0x0000000000000000ULL, 0xCB329A8F102F05BCULL },
    { 81, 0x000000009E3779B1ULL, 0x7E77B483A01E4AEDULL },
    { 82, 0x0000000000000000ULL, 0x993A0B6E6D5DF0CBULL },
    { 82, 0x000000009E3779B1ULL, 0x6DBD4234F13A5DC1ULL },
    { 83, 0x0000000000000000ULL, 0x818F4CEC407D8CF9ULL },
    { 83, 0x000000009E3779B1ULL, 0x0367C5363E62AEC3ULL },
    { 84, 0x0000000000000000ULL, 0xAF57F8CD340FB1B6ULL },
    { 84, 0x000000009E3779B1ULL, 0xC7623B4C4E6844C1ULL },
    { 85, 0x0000000000000000ULL, 0x50789F0F60EBAD3AULL },
    { 85, 0x000000009E3779B1ULL, 0xB88DA3150AAF44BEULL },
    { 86, 0x0000000000000000ULL, 0xF3214750C1A6455EULL },
    { 86, 0x000000009E3779B1ULL, 0x56C1A7FB96DE0E82ULL },
    { 87, 0x0000000000000000ULL, 0x8DBE8913BE346D20ULL },
    { 87, 0x000000009E3779B1ULL, 0x899E381BD46D1409ULL },
    { 88, 0x0000000000000000ULL, 0xA130814F81F87E43ULL },
    { 88, 0x000000009E3779B1ULL, 0xA3BF96F3D593C759ULL },
    { 89, 0x0000000000000000ULL, 0x0023276D258EDA58ULL },
    { 89, 0x000000009E3779B1ULL, 0x6779DE8C67483444ULL },
    { 90, 0x0000000000000000ULL, 0x0A0571F769B40C93ULL },
    { 90, 0x000000009E3779B1ULL, 0x9C915D4DA4BE05A6ULL },
    { 91, 0x0000000000000000ULL, 0x14A2F5051E8893C8ULL },
    { 91, 0x000000009E3779B1ULL, 0x46F62B0B3D1B66AFULL },
    { 92, 0x0000000000000000ULL, 0x4FBBE74E062367B0ULL },
    { 92, 0x000000009E3779B1ULL, 0x3190419611427263ULL },
    { 93, 0x0000000000000000ULL, 0x4B375CEF503586FCULL },
    { 93, 0x000000009E3779B1ULL, 0x821D5495DDDC7C82ULL },
    { 94, 0x0000000000000000ULL, 0x6AD4B96E4286EB70ULL },
    { 94, 0x000000009E3779B1ULL, 0x553DDDB89F3CB523ULL },
    { 95, 0x0000000000000000ULL, 0xFF9F46BDCC644624ULL },
    { 95, 0x000000009E3779B1ULL, 0x72E75A560EF624A3ULL },
    { 96, 0x0000000000000000ULL, 0x105064E743EDD1D9ULL },
    { 96, 0x000000009E3779B1ULL, 0x8FF0B4ABEE6F03CCULL },
    { 97, 0x0000000000000000ULL, 0x097B16E4E9B0A2E3ULL },
    { 97, 0x000000009E3779B1ULL, 0x9704E5147FDCC4F4ULL },
    { 98, 0x0000000000000000ULL, 0x06662519E8393FF2ULL },
    { 98, 0x000000009E3779B1ULL, 0xB2889379EAD9C5CFULL },
    { 99, 0x0000000000000000ULL, 0xB51CB5BA2957AB0BULL },
    { 99, 0x000000009E3779B1ULL, 0x5C10E54DEC4B0FA9ULL },
    { 100, 0x0000000000000000ULL, 0x4BFE019CD91D9EA4ULL },
    { 100, 0x000000009E3779B1ULL, 0x4853706DC9625CAEULL },
    { 101, 0x0000000000000000ULL, 0xA51EA2101B2D114CULL },
    { 101, 0x000000009E3779B1ULL, 0x6B38A5BBBA1F049CULL },
    { 102, 0x0000000000000000ULL, 0x26259500AD475ACFULL },
    { 102, 0x000000009E3779B1ULL, 0x6CDC4C9926C04118ULL },
    { 103, 0x0000000000000000ULL, 0x74A97F547BB8C845ULL },
    { 103, 0x000000009E3779B1ULL, 0x6A027096FB146E74ULL },
    { 104, 0x0000000000000000ULL, 0x23ED252F132DFA0FULL },
    { 104, 0x000000009E3779B1ULL, 0x1683C40FE4F3EC6EULL },
    { 105, 0x0000000000000000ULL, 0x1E03CFB52F4E43F2ULL },
    { 105, 0x000000009E3779B1ULL, 0x8399AE3FAC887392ULL },
    { 106, 0x0000000000000000ULL, 0xB3C7FBBF1E2E9E9FULL },
    { 106, 0x000000009E3779B1ULL, 0x205158F71E4AD7DAULL },
    { 107, 0x0000000000000000ULL, 0xC85AF48447573841ULL },
    { 107, 0x000000009E3779B1ULL, 0x42BE4CF154BDCB6FULL },
    { 108, 0x0000000000000000ULL, 0x395022FA4E7C679DULL },
    { 108, 0x000000009E3779B1ULL, 0x76577A7F0C793723ULL },
    { 109, 0x0000000000000000ULL, 0x931CE3A962248CB1ULL },
    { 109, 0x000000009E3779B1ULL, 0x3F3AFDE1CBFB2D78ULL },
    { 110, 0x0000000000000000ULL, 0x46A7A7126CE7D4E6ULL },
    { 110, 0x000000009E3779B1ULL, 0x6CC264DBC30D3EDCULL },
    { 111, 0x0000000000000000ULL, 0xB7AFFC461C9ECE18ULL },
    { 111, 0x000000009E3779B1ULL, 0x7D83E9E9160317ECULL },
    { 112, 0x0000000000000000ULL, 0xE5752EA6E2B34417ULL },
    { 112, 0x000000009E3779B1ULL, 0xEECB8B56A5BD00DBULL },
    { 113, 0x0000000000000000ULL, 0xFDA17FD84B50AD25ULL },
    { 113, 0x000000009E3779B1ULL, 0x47E8C074796FD671ULL },
    { 114, 0x0000000000000000ULL, 0x8751DAC0314381B6ULL },
    { 114, 0x000000009E3779B1ULL, 0xB880FBDD4FB6B274ULL },
    { 115, 0x0000000000000000ULL, 0xF7CB3E3DFCE93157ULL },
    { 115, 0x000000009E3779B1ULL, 0x55033F2004F0E78CULL },
    { 116, 0x0000000000000000ULL, 0x0A29AECEA5D8240FULL },
    { 116, 0x000000009E3779B1ULL, 0xD85DE10D5AD4E0A2ULL },
    { 117, 0x0000000000000000ULL, 0x6731FD15B033D86FULL },
    { 117, 0x000000009E3779B1ULL, 0xFC63C52FB7E3C2F5ULL },
    { 118, 0x0000000000000000ULL, 0x00BB7740D233CCA9ULL },
    { 118, 0x000000009E3779B1ULL, 0x13B679C13A3D0514ULL },
    { 119, 0x0000000000000000ULL, 0xC9577B1D5E99EADDULL },
    { 119, 0x000000009E3779B1ULL, 0xF935B21C4FB17FA0ULL },
    { 120, 0x0000000000000000ULL, 0x35E51D65936602F0ULL },
    { 120, 0x000000009E3779B1ULL, 0x8F28106B8EDD0B62ULL },
    { 121, 0x0000000000000000ULL, 0xD11761D6683C7490ULL },
    { 121, 0x000000009E3779B1ULL, 0xE48065BAFEE225FFULL },
    { 122, 0x0000000000000000ULL, 0xEC6AFA468D61B584ULL },
    { 122, 0x000000009E3779B1ULL, 0x562C3376F70E4A29ULL },
    { 123, 0x0000000000000000ULL, 0x147F4024677727ECULL },
    { 123, 0x000000009E3779B1ULL, 0x4980EF3E9B77DC0CULL },
    { 124, 0x0000000000000000ULL, 0x47FD33A91C14F035ULL },
    { 124, 0x000000009E3779B1ULL, 0x89EEA88E5DFFEBBFULL },
    { 125, 0x0000000000000000ULL, 0x7B385D8B837FAA3EULL },
    { 125, 0x000000009E3779B1ULL, 0xFE949E5EF83C19B5ULL },
    { 126, 0x0000000000000000ULL, 0xA0E5EC3ED6068228ULL },
    { 126, 0x000000009E3779B1ULL, 0x27EDDF566EA1935CULL },
    { 127, 0x0000000000000000ULL, 0x3C7A21119AA662B0ULL },
    { 127, 0x000000009E3779B1ULL, 0xB0D6DC189C06CEEDULL },
    { 128, 0x0000000000000000ULL, 0x90CA021457D96DC5ULL },
    { 128, 0x000000009E3779B1ULL, 0xED9340A202BCD1CFULL },
    { 129, 0x0000000000000000ULL, 0x41C280132D697ABAULL },
    { 129, 0x000000009E3779B1ULL, 0x1668B87489935FF5ULL },
    { 130, 0x0000000000000000ULL, 0xFCEB19904FA5852EULL },
    { 130, 0x000000009E3779B1ULL, 0x531E7902F11E5233ULL },
    { 131, 0x0000000000000000ULL, 0xA855AB9C545E3AA0ULL },
    { 131, 0x000000009E3779B1ULL, 0x38EEF63349D48351ULL },
    { 132, 0x0000000000000000ULL, 0x92FF5FE9DF03B47EULL },
    { 132, 0x000000009E3779B1ULL, 0x25499A366A4F582AULL },
    { 133, 0x0000000000000000ULL, 0xBEBABB79841EEFE3ULL },
    { 133, 0x000000009E3779B1ULL, 0x086D173652FF2C7BULL },
    { 134, 0x0000000000000000ULL, 0x83484B9261651B21ULL },
    { 134, 0x000000009E3779B1ULL, 0xF6DF65E34A988A05ULL },
    { 135, 0x0000000000000000ULL, 0x1FA13853B96095B6ULL },
    { 135, 0x000000009E3779B1ULL, 0xAE76A9DBD7A050B5ULL },
    { 136, 0x0000000000000000ULL, 0xDED15F1F9BA92FEFULL },
    { 136, 0x000000009E3779B1ULL, 0xE0DC505D8B6C72AAULL },
    { 137, 0x0000000000000000ULL, 0x413F6647912DD2EAULL },
    { 137, 0x000000009E3779B1ULL, 0x49402EC5661AF438ULL },
    { 138, 0x0000000000000000ULL, 0xBF93F15916B89700ULL },
    { 138, 0x000000009E3779B1ULL, 0x5C8B85C8837C3FF6ULL },
    { 139, 0x0000000000000000ULL, 0x9B4FD36F05521437ULL },
    { 139, 0x000000009E3779B1ULL, 0x1A2A4E33EEA8A6CCULL },
    { 140, 0x0000000000000000ULL, 0xAF1F8B36AFE6DF4FULL },
    { 140, 0x000000009E3779B1ULL, 0xA001BF8A9FA78B49ULL },
    { 141, 0x0000000000000000ULL, 0x6948F778B10726CBULL },
    { 141, 0x000000009E3779B1ULL, 0x357064D8FE34DE45ULL },
    { 142, 0x0000000000000000ULL, 0x57DE58AC79A084E8ULL },
    { 142, 0x000000009E3779B1ULL, 0xBF0781313D72D5E8ULL },
    { 143, 0x0000000000000000ULL, 0xD2CA0115A4F272AAULL },
    { 143, 0x000000009E3779B1ULL, 0x7529563A8C6AAAD6ULL },
    { 144, 0x0000000000000000ULL, 0x47999F1F28EC88A6ULL },
    { 144, 0x000000009E3779B1ULL, 0x2221C4C0807FE0F4ULL },
    { 145, 0x0000000000000000ULL, 0x18F519801BB62003ULL },
    { 145, 0x000000009E3779B1ULL, 0x98022684CD4C5A22ULL },
    { 146, 0x0000000000000000ULL, 0x15C761F415F97578ULL },
    { 146, 0x000000009E3779B1ULL, 0xB193B77D0F63741CULL },
    { 147, 0x0000000000000000ULL, 0x4BA2A072E1FF588FULL },
    { 147, 0x000000009E3779B1ULL, 0xBF8E502C905E18F7ULL },
    { 148, 0x0000000000000000ULL, 0xF2E4CB94F60F07D2ULL },
    { 148, 0x000000009E3779B1ULL, 0x7838B9B817007E06ULL },
    { 149, 0x0000000000000000ULL, 0xAFBD86DEE66EFE56ULL },
    { 149, 0x000000009E3779B1ULL, 0xC39859A0DE79A89BULL },
    { 150, 0x0000000000000000ULL, 0xC1B4E21FDB2FECE6ULL },
    { 150, 0x000000009E3779B1ULL, 0xF1B1B1CDDB38FFC7ULL },
    { 151, 0x0000000000000000ULL, 0x4DDEA4A0F2AA9CF8ULL },
    { 151, 0x000000009E3779B1ULL, 0xDA1F611EB1C2BE56ULL },
    { 152, 0x0000000000000000ULL, 0x62A15F4D530A7EA5ULL },
    { 152, 0x000000009E3779B1ULL, 0x2F0F73C639D3BAB3ULL },
    { 153, 0x0000000000000000ULL, 0x9B84F22525275E62ULL },
    { 153, 0x000000009E3779B1ULL, 0x335D15302C3E1B23ULL },
    { 154, 0x0000000000000000ULL, 0x2EEB0441C9451B1FULL },
    { 154, 0x000000009E3779B1ULL, 0x97C17A5309E528DDULL },
    { 155, 0x0000000000000000ULL, 0x1DE7F80736C5FD1FULL },
    { 155, 0x000000009E3779B1ULL, 0x122D55162B11AA70ULL },
    { 156, 0x0000000000000000ULL, 0x73745EC8FDABF21CULL },
    { 156, 0x000000009E3779B1ULL, 0x4B29B9C80C11731EULL },
    { 157, 0x0000000000000000ULL, 0x8A2890AE42E4DBC0ULL },
    { 157, 0x000000009E3779B1ULL, 0x606C98DA0FFF5E99ULL },
    { 158, 0x0000000000000000ULL, 0x6B4F62CFF8A43B80ULL },
    { 158, 0x000000009E3779B1ULL, 0x71974C55ACE70505ULL },
    { 159, 0x0000000000000000ULL, 0x5E02EFAF7ACDF19CULL },
    { 159, 0x000000009E3779B1ULL, 0x0981A09CBBBEADFDULL },
    { 160, 0x0000000000000000ULL, 0x5B11EFE8DE350A3BULL },
    { 160, 0x000000009E3779B1ULL, 0x96C22CBA2A6BC16FULL },
    { 161, 0x0000000000000000ULL, 0xAD813CA7B0D54781ULL },
    { 161, 0x000000009E3779B1ULL, 0xF782F5A5F3183FC5ULL },
    { 162, 0x0000000000000000ULL, 0xD2EC094A8B3CA48FULL },
    { 162, 0x000000009E3779B1ULL, 0x6DE21A60EE5C4ED9ULL },
    { 163, 0x0000000000000000ULL, 0x4BF3D48D5ACBF6ACULL },
    { 163, 0x000000009E3779B1ULL, 0x3A4037A4F77B80F7ULL },
    { 164, 0x0000000000000000ULL, 0xE41F05B876F996E6ULL },
    { 164, 0x000000009E3779B1ULL, 0xDF59454C354263C0ULL },
    { 165, 0x0000000000000000ULL, 0x46C8500698B33143ULL },
    { 165, 0x000000009E3779B1ULL, 0xE61470FE106DAEB6ULL },
    { 166, 0x0000000000000000ULL, 0x1FF837B89DFF74C6ULL },
    { 166, 0x000000009E3779B1ULL, 0x35EF16A3CAB6DBD8ULL },
    { 167, 0x0000000000000000ULL, 0x8A9FE53D0989B9DBULL },
    { 167, 0x000000009E3779B1ULL, 0xF09AA4A5D0C8946AULL },
    { 168, 0x0000000000000000ULL, 0x1AED20C477BA590AULL },
    { 168, 0x000000009E3779B1ULL, 0xC4AF5415867E8187ULL },
    { 169, 0x0000000000000000ULL, 0x5678A561F39E4B7FULL },
    { 169, 0x000000009E3779B1ULL, 0x79174EB2FCB8E1B5ULL },
    { 170, 0x0000000000000000ULL, 0x07BC9808A99E91CCULL },
    { 170, 0x000000009E3779B1ULL, 0xA34F995ABD6E23E9ULL },
    { 171, 0x0000000000000000ULL, 0x1A1E7EFCFC5A377FULL },
    { 171, 0x000000009E3779B1ULL, 0xD47735EC5421D191ULL },
    { 172, 0x0000000000000000ULL, 0x625449E505BB4352ULL },
    { 172, 0x000000009E3779B1ULL, 0xCBB1E8F887B1A5E1ULL },
    { 173, 0x0000000000000000ULL, 0x16D6028DF84F9C3FULL },
    { 173, 0x000000009E3779B1ULL, 0xB0635E32B423501EULL },
    { 174, 0x0000000000000000ULL, 0x10902519949F206EULL },
    { 174, 0x000000009E3779B1ULL, 0xC39AFB66FBA93BE8ULL },
    { 175, 0x0000000000000000ULL, 0x8B8F04B20E8F9322ULL },
    { 175, 0x000000009E3779B1ULL, 0xD54B025F1F25CBA5ULL },
    { 176, 0x0000000000000000ULL, 0xDF9F79B4EA272F55ULL },
    { 176, 0x000000009E3779B1ULL, 0xBEB6CC6C04783A4DULL },
    { 177, 0x0000000000000000ULL, 0x05E8B1B61DBD0B71ULL },
    { 177, 0x000000009E3779B1ULL, 0xD71F05251A958C88ULL },
    { 178, 0x0000000000000000ULL, 0xA046B4F8050FF19FULL },
    { 178, 0x000000009E3779B1ULL, 0xF01A6ADA9BB9BB7FULL },
    { 179, 0x0000000000000000ULL, 0xC3C9B9967B78F65EULL },
    { 179, 0x000000009E3779B1ULL, 0x1766738809D8F120ULL },
    { 180, 0x0000000000000000ULL, 0x07557C6E01438898ULL },
    { 180, 0x000000009E3779B1ULL, 0xFFE5FEAB5B3B9BFDULL },
    { 181, 0x0000000000000000ULL, 0x8EB6B9EA889592E4ULL },
    { 181, 0x000000009E3779B1ULL, 0x1CB0DBF962768311ULL },
    { 182, 0x0000000000000000ULL, 0xB563B680EAF93D1CULL },
    { 182, 0x000000009E3779B1ULL, 0x8096F4E5CDAB3BA8ULL },
    { 183, 0x0000000000000000ULL, 0xDC0A598546EEF8D5ULL },
    { 183, 0x000000009E3779B1ULL, 0x6FE275B936FF8655ULL },
    { 184, 0x0000000000000000ULL, 0x3DEAE67F2AF3D038ULL },
    { 184, 0x000000009E3779B1ULL, 0xBF37C0A167A2A840ULL },
    { 185, 0x0000000000000000ULL, 0xDFF0E8388895A23EULL },
    { 185, 0x000000009E3779B1ULL, 0x36B6AF6C692E8CB9ULL },
    { 186, 0x0000000000000000ULL, 0x984EF85C5A5328F2ULL },
    { 186, 0x000000009E3779B1ULL, 0x2245AB8648BF626AULL },
    { 187, 0x0000000000000000ULL, 0xFDAD135D4A190C32ULL },
    { 187, 0x000000009E3779B1ULL, 0xD5C57A08C7C4BD87ULL },
    { 188, 0x0000000000000000ULL, 0x891447ACCCE59C8AULL },
    { 188, 0x000000009E3779B1ULL, 0xFC5497CD2A840F45ULL },
    { 189, 0x0000000000000000ULL, 0x3C44BB5E8E0B8C55ULL },
    { 189, 0x000000009E3779B1ULL, 0x918834CE94A648BAULL },
    { 190, 0x0000000000000000ULL, 0xA6479C9DDC0E5E00ULL },
    { 190, 0x000000009E3779B1ULL, 0x6A01F0D349AAD4D3ULL },
    { 191, 0x0000000000000000ULL, 0xF8329DA3AD4249E8ULL },
    { 191, 0x000000009E3779B1ULL, 0xE9CCF0337B2F9F6CULL },
    { 192, 0x0000000000000000ULL, 0xB76A434AC1E4768BULL },
    { 192, 0x000000009E3779B1ULL, 0xC12D08F273988FD5ULL },
    { 193, 0x0000000000000000ULL, 0x2CDF52488C4A2F81ULL },
    { 193, 0x000000009E3779B1ULL, 0x863D55C54A8D3ED1ULL },
    { 194, 0x0000000000000000ULL, 0x46A382CC7B5D8281ULL },
    { 194, 0x000000009E3779B1ULL, 0xFD7645CA691DFB7DULL },
    { 195, 0x0000000000000000ULL, 0x52B73ECDB3EF30E4ULL },
    { 195, 0x000000009E3779B1ULL, 0x9159A6288CD2ED9CULL },
    { 196, 0x0000000000000000ULL, 0xEB9ACBEFC9258D5CULL },
    { 196, 0x000000009E3779B1ULL, 0x935F16E68F6BD8EDULL },
    { 197, 0x0000000000000000ULL, 0x3356A9D81CFB5929ULL },
    { 197, 0x000000009E3779B1ULL, 0xDDAB613134C2333BULL },
    { 198, 0x0000000000000000ULL, 0xE781C3C6402BEA01ULL },
    { 198, 0x000000009E3779B1ULL, 0xF453BBAFAC16D837ULL },
    { 199, 0x0000000000000000ULL, 0xE37FEF4496509A87ULL },
    { 199, 0x000000009E3779B1ULL, 0xCC30E66C6A06D8F7ULL },
    { 200, 0x0000000000000000ULL, 0x4D863378A2052D65ULL },
    { 200, 0x000000009E3779B1ULL, 0x254AF7CA5C98A77EULL },
    { 201, 0x0000000000000000ULL, 0xF24E8A1EFDFF290CULL },
    { 201, 0x000000009E3779B1ULL, 0xB8D8C9E545CE2F59ULL },
    { 202, 0x0000000000000000ULL, 0x5C98EE8DFA4EFCBFULL },
    { 202, 0x000000009E3779B1ULL, 0x46517A8D9B9B7E59ULL },
    { 203, 0x0000000000000000ULL, 0x836063377C48824AULL },
    { 203, 0x000000009E3779B1ULL, 0x190AF984AAAAF457ULL },
    { 204, 0x0000000000000000ULL, 0xAAB407230B8C829CULL },
    { 204, 0x000000009E3779B1ULL, 0x8649C4337A9C12D8ULL },
    { 205, 0x0000000000000000ULL, 0xAFA6E4AA2AE03314ULL },
    { 205, 0x000000009E3779B1ULL, 0xFDB87A419B4094E3ULL },
    { 206, 0x0000000000000000ULL, 0xB258FED114D4FBE6ULL },
    { 206, 0x000000009E3779B1ULL, 0x2D3CA3EE5D2578C4ULL },
    { 207, 0x0000000000000000ULL, 0xC7054B982C73EB13ULL },
    { 207, 0x000000009E3779B1ULL, 0x4E0E7EA5A365B668ULL },
    { 208, 0x0000000000000000ULL, 0x9EB4B1067F96C311ULL },
    { 208, 0x000000009E3779B1ULL, 0x0DD3A0B6D0D0B379ULL },
    { 209, 0x0000000000000000ULL, 0x7344195C54279951ULL },
    { 209, 0x000000009E3779B1ULL, 0xFBB6E34768AF7349ULL },
    { 210, 0x0000000000000000ULL, 0x84812698B4B8BF44ULL },
    { 210, 0x000000009E3779B1ULL, 0xC18DC56C45886E5DULL },
    { 211, 0x0000000000000000ULL, 0x821B3108CD99A5A8ULL },
    { 211, 0x000000009E3779B1ULL, 0x33019E48E1DAFCCCULL },
    { 212, 0x0000000000000000ULL, 0x8E8E68151D5E419DULL },
    { 212, 0x000000009E3779B1ULL, 0x6366BDDFF5532071ULL },
    { 213, 0x0000000000000000ULL, 0xFD0B5E2A5921C14AULL },
    { 213, 0x000000009E3779B1ULL, 0xA064370AB08A06A3ULL },
    { 214, 0x0000000000000000ULL, 0x87FA20EB6126D0CCULL },
    { 214, 0x000000009E3779B1ULL, 0xC98AF5F597B2D97EULL },
    { 215, 0x0000000000000000ULL, 0xA38BDC1F221B4DDCULL },
    { 215, 0x000000009E3779B1ULL, 0x6A97D20B094B06CAULL },
    { 216, 0x0000000000000000ULL, 0xD997AAAE88AE9C5FULL },
    { 216, 0x000000009E3779B1ULL, 0xCAAB4027894BCCB2ULL },
    { 217, 0x0000000000000000ULL, 0x8E031800E9DE75D5ULL },
    { 217, 0x000000009E3779B1ULL, 0x5C20B9E6594A579EULL },
    { 218, 0x0000000000000000ULL, 0x2EE2208AE332E79DULL },
    { 218, 0x000000009E3779B1ULL, 0xAAF5F7238DDF1EE6ULL },
    { 219, 0x0000000000000000ULL, 0x21B874601A4DAC92ULL },
    { 219, 0x000000009E3779B1ULL, 0x0DD13A547AAB12B7ULL },
    { 220, 0x0000000000000000ULL, 0xB44A85B423E04207ULL },
    { 220, 0x000000009E3779B1ULL, 0x959091592F4A6B26ULL },
    { 221, 0x0000000000000000ULL, 0x7C8CB92ECFE982DCULL },
    { 221, 0x000000009E3779B1ULL, 0xBE6B813AF6B8657CULL },
    { 222, 0x0000000000000000ULL, 0xB641AE8CB691C174ULL },
    { 222, 0x000000009E3779B1ULL, 0x20CB8AB7AE10C14AULL },
    { 223, 0x0000000000000000ULL, 0x3204FA238CB7A119ULL },
    { 223, 0x000000009E3779B1ULL, 0x6976AE81215D0A31ULL },
    { 224, 0x0000000000000000ULL, 0x560A22F68D44BAA4ULL },
    { 224, 0x000000009E3779B1ULL, 0xCE6601FBB696A3CBULL },
    { 225, 0x0000000000000000ULL, 0x26034ED8D4CAE4DCULL },
    { 225, 0x000000009E3779B1ULL, 0x7721221845330B35ULL },
    { 226, 0x0000000000000000ULL, 0x6C44B98E3A56766FULL },
    { 226, 0x000000009E3779B1ULL, 0xBA55D8AAECB6D3F7ULL },
    { 227, 0x0000000000000000ULL, 0x84D9943F84B3F48CULL },
    { 227, 0x000000009E3779B1ULL, 0xA1EA6FD991AB6CAFULL },
    { 228, 0x0000000000000000ULL, 0x07FBF61C952C73A0ULL },
    { 228, 0x000000009E3779B1ULL, 0x2D1ADCE3A026DB2AULL },
    { 229, 0x0000000000000000ULL, 0x7650089C595D1589ULL },
    { 229, 0x000000009E3779B1ULL, 0x39EABB22FCAE07B7ULL },
    { 230, 0x0000000000000000ULL, 0xA6E2C789AE523788ULL },
    { 230, 0x000000009E3779B1ULL, 0x75964501197A7C29ULL },
    { 231, 0x0000000000000000ULL, 0x1678FC0F21A2D189ULL },
    { 231, 0x000000009E3779B1ULL, 0xC90DE1DD55859773ULL },
    { 232, 0x0000000000000000ULL, 0x08837FC18CF9DC21ULL },
    { 232, 0x000000009E3779B1ULL, 0x7ED72B5DAD65C52FULL },
    { 233, 0x0000000000000000ULL, 0x2B8CBB3C1BE4B134ULL },
    { 233, 0x000000009E3779B1ULL, 0x52A41D95CE99F400ULL },
    { 234, 0x0000000000000000ULL, 0x48502CC375AEDE41ULL },
    { 234, 0x000000009E3779B1ULL, 0x2D3525D1960F2D21ULL },
    { 235, 0x0000000000000000ULL, 0xC9136721158E47F6ULL },
    { 235, 0x000000009E3779B1ULL, 0x777C310AFC011284ULL },
    { 236, 0x0000000000000000ULL, 0x6252A7317AF95FD4ULL },
    { 236, 0x000000009E3779B1ULL, 0xD4B0539A256E0DB4ULL },
    { 237, 0x0000000000000000ULL, 0x6FD0AAE0666C5A27ULL },
    { 237, 0x000000009E3779B1ULL, 0x23E7D04D7E99A2B7ULL },
    { 238, 0x0000000000000000ULL, 0x0C2CE719985CD28CULL },
    { 238, 0x000000009E3779B1ULL, 0xCD34C05EA95F2F61ULL },
    { 239, 0x0000000000000000ULL, 0xB93713464372DA03ULL },
    { 239, 0x000000009E3779B1ULL, 0x4BFC609CBFAEED9BULL },
    { 240, 0x0000000000000000ULL, 0xB81838D483BAEE53ULL },
    { 240, 0x000000009E3779B1ULL, 0xA4B3F965B6FE67F8ULL },
    { 241, 0x0000000000000000ULL, 0x95D76C8B4D8FC4D6ULL },
    { 241, 0x000000009E3779B1ULL, 0x19D5AD5F4BD6CB9FULL },
    { 242, 0x0000000000000000ULL, 0xF90D9317A58AACCAULL },
    { 242, 0x000000009E3779B1ULL, 0x2B3D64CA68F954F6ULL },
    { 243, 0x0000000000000000ULL, 0xDA027E06C3DAAC3FULL },
    { 243, 0x000000009E3779B1ULL, 0xD36ED0285CC2C704ULL },
    { 244, 0x0000000000000000ULL, 0x3DA619F5D6E8F158ULL },
    { 244, 0x000000009E3779B1ULL, 0x10CD0F2DB5B8EDFFULL },
    { 245, 0x0000000000000000ULL, 0x711082146B76E24FULL },
    { 245, 0x000000009E3779B1ULL, 0xD24594468B28F2D1ULL },
    { 246, 0x0000000000000000ULL, 0x598FBCF46F00187FULL },
    { 246, 0x000000009E3779B1ULL, 0xBE543D6D5301F479ULL },
    { 247, 0x0000000000000000ULL, 0x9E1B0082378B40CDULL },
    { 247, 0x000000009E3779B1ULL, 0x1CC20CEC10C69CE0ULL },
    { 248, 0x0000000000000000ULL, 0x6C8D0BD0802CEE09ULL },
    { 248, 0x000000009E3779B1ULL, 0xE4E00E9C08E9D9BEULL },
    { 249, 0x0000000000000000ULL, 0x902EECCB98DB38D7ULL },
    { 249, 0x000000009E3779B1ULL, 0x826F17162AAE3546ULL },
    { 250, 0x0000000000000000ULL, 0x98733C2D429FF9EEULL },
    { 250, 0x000000009E3779B1ULL, 0xC00A5A94ADD5C591ULL },
    { 251, 0x0000000000000000ULL, 0xD278B54D9FEC3B13ULL },
    { 251, 0x000000009E3779B1ULL, 0x22E6DA603138549CULL },
    { 252, 0x0000000000000000ULL, 0x502316D885591F05ULL },
    { 252, 0x000000009E3779B1ULL, 0x365C19466B1CE6F6ULL },
    { 253, 0x0000000000000000ULL, 0x4F27F34D5F7363B4ULL },
    { 253, 0x000000009E3779B1ULL, 0x6598472FCB3C9FDDULL },
    { 254, 0x0000000000000000ULL, 0xA8FA70BEAF15A0D5ULL },
    { 254, 0x000000009E3779B1ULL, 0xA3D08118E33A36FBULL },
    { 255, 0x0000000000000000ULL, 0xA80F35BB0DC8E3A7ULL },
    { 255, 0x000000009E3779B1ULL, 0x8E7A59762640754FULL },
    { 256, 0x0000000000000000ULL, 0x5E3F5BF94D574981ULL },
    { 256, 0x000000009E3779B1ULL, 0x34733CBD9CC1B0D5ULL },
    { 257, 0x0000000000000000ULL, 0xD36CF327DCF12221ULL },
    { 257, 0x000000009E3779B1ULL, 0x0E59955420F8807BULL },
    { 258, 0x0000000000000000ULL, 0x800D25DAF5B3E79DULL },
    { 258, 0x000000009E3779B1ULL, 0xE72AEBE1FEAA93EEULL },
    { 259, 0x0000000000000000ULL, 0x2099FA654952427CULL },
    { 259, 0x000000009E3779B1ULL, 0xBB868FAB515A5C72ULL },
};

static void test_xxh64(void)
{
    uint8_t buf[260];
    uint64_t gen = 2654435761U;
    size_t i;
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(gen >> 56);
        gen *= 11400714785074694797ULL;
    }
    for (i = 0; i < sizeof(xxh64_vectors) / sizeof(xxh64_vectors[0]); i++) {
        uint64_t got = dyn_ds_xxh64(buf, xxh64_vectors[i].len, xxh64_vectors[i].seed);
        CHECK(got == xxh64_vectors[i].want, "xxh64 len %zu seed %llx: got %016llx want %016llx",
            xxh64_vectors[i].len, (unsigned long long)xxh64_vectors[i].seed,
            (unsigned long long)got, (unsigned long long)xxh64_vectors[i].want);
    }
    printf("xxh64: %zu published vectors ok\n", i);
}

int main(void)
{
    test_xxh64();
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
