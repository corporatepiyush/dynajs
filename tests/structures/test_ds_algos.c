/* Suffix array (SA-IS + Kasai LCP), PATRICIA trie, CSR graph algorithms and the R*-tree of
 * dyn-ds.c, each checked against an independent brute-force reference. The suffix array is
 * also checked against the worked example "banana" (Wikipedia, "Suffix array"); the
 * "mississippi" array and LCP were worked by hand from the definition and are cross-checked by
 * the naive suffix sort below. The library is one translation unit here with a
 * counting allocator, so every check also proves nothing leaks. The cursor insert that
 * PatriciaTrie load uses is checked against plain put over random, sorted, duplicate-heavy,
 * reversed and run-shuffled key orders. The ConsistentHash ring sort (radix on the hash from 4096
 * points, qsort below) is checked against the name-order comparator on forged runs of equal hashes. The PATRICIA node
 * arrays are also checked with keys of 0..40 bytes (across the 16-byte inline key limit, non-byte-aligned bit lengths),
 * a 150-deep chain of nested prefixes (past the 64-entry path and candidate stacks, so both fallbacks run), and an
 * allocation-failure sweep (every put either lands whole or leaves the trie unchanged, across the 1024-node chunk edge). */
#include <math.h>
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

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const uint8_t* g_text;
static size_t g_n;
static int naive_suffix_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    size_t lx = g_n - x, ly = g_n - y, l = lx < ly ? lx : ly;
    int c = memcmp(g_text + x, g_text + y, l);
    if (c)
        return c;
    return lx < ly ? -1 : lx > ly;
}

static void sa_check_text(const uint8_t* text, size_t n, const char* label)
{
    dyn_sa_t* sa = dyn_sa_build(text, n);
    uint32_t* ref = xmalloc(sizeof(uint32_t) * (n ? n : 1));
    const uint32_t *got, *lcp;
    size_t i;
    CHECK(sa != NULL, "%s: build", label);
    if (!sa) {
        free(ref);
        return;
    }
    for (i = 0; i < n; i++)
        ref[i] = (uint32_t)i;
    g_text = text;
    g_n = n;
    qsort(ref, n, sizeof(uint32_t), naive_suffix_cmp);
    got = dyn_sa_array(sa);
    CHECK(dyn_sa_len(sa) == n, "%s: length", label);
    for (i = 0; i < n; i++)
        if (got[i] != ref[i]) {
            CHECK(0, "%s: sa[%zu] = %u want %u", label, i, got[i], ref[i]);
            break;
        }
    lcp = dyn_sa_lcp(sa);
    CHECK(lcp != NULL, "%s: lcp", label);
    for (i = 1; lcp && i < n; i++) {
        size_t h = 0;
        while (ref[i] + h < n && ref[i - 1] + h < n && text[ref[i] + h] == text[ref[i - 1] + h])
            h++;
        if (lcp[i] != h) {
            CHECK(0, "%s: lcp[%zu] = %u want %zu", label, i, lcp[i], h);
            break;
        }
    }
    for (i = 0; i < 20 && n; i++) {
        size_t start = rnd() % n, m = 1 + rnd() % 6, first, want = 0, j;
        if (start + m > n)
            m = n - start;
        for (j = 0; j + m <= n; j++)
            if (!memcmp(text + j, text + start, m))
                want++;
        CHECK(dyn_sa_find(sa, text + start, m, &first) == want, "%s: find count", label);
    }
    {
        uint8_t absent[3] = { 0xFF, 0xFE, 0xFD };
        size_t want = 0, j;
        for (j = 0; j + 3 <= n; j++)
            if (!memcmp(text + j, absent, 3))
                want++;
        CHECK(dyn_sa_find(sa, absent, 3, NULL) == want, "%s: find of a rare pattern", label);
    }
    {
        size_t pos = 0, best = 0, a, b;
        size_t rep = dyn_sa_longest_repeat(sa, &pos);
        if (n <= 400) {
            for (a = 0; a < n; a++)
                for (b = a + 1; b < n; b++) {
                    size_t h = 0;
                    while (b + h < n && text[a + h] == text[b + h])
                        h++;
                    if (h > best)
                        best = h;
                }
            CHECK(rep == best, "%s: longest repeat %zu want %zu", label, rep, best);
        }
        if (rep) {
            size_t cnt = dyn_sa_find(sa, text + pos, rep, NULL);
            CHECK(cnt >= 2, "%s: the longest repeat occurs at least twice", label);
        }
    }
    dyn_sa_free(sa);
    free(ref);
}

static void test_sa(void)
{
    static const uint32_t banana[] = { 5, 3, 1, 0, 4, 2 };
    static const uint32_t mississippi[] = { 10, 7, 4, 1, 0, 9, 8, 6, 3, 5, 2 };
    dyn_sa_t* sa = dyn_sa_build((const uint8_t*)"banana", 6);
    CHECK(sa && !memcmp(dyn_sa_array(sa), banana, sizeof(banana)), "banana suffix array");
    CHECK(sa && dyn_sa_find(sa, (const uint8_t*)"ana", 3, NULL) == 2, "banana has two 'ana'");
    dyn_sa_free(sa);
    sa = dyn_sa_build((const uint8_t*)"mississippi", 11);
    CHECK(sa && !memcmp(dyn_sa_array(sa), mississippi, sizeof(mississippi)), "mississippi suffix array");
    {
        static const uint32_t lcp_ref[] = { 0, 1, 1, 4, 0, 0, 1, 0, 2, 1, 3 };
        CHECK(sa && !memcmp(dyn_sa_lcp(sa), lcp_ref, sizeof(lcp_ref)), "mississippi LCP");
    }
    dyn_sa_free(sa);
    {
        uint8_t buf[3000];
        unsigned alpha, trial;
        static const unsigned alphabets[] = { 1, 2, 4, 26, 256 };
        for (alpha = 0; alpha < sizeof(alphabets) / sizeof(alphabets[0]); alpha++)
            for (trial = 0; trial < 12; trial++) {
                size_t n = trial == 0 ? 0 : trial == 1 ? 1
                                                       : (size_t)(rnd() % 3000);
                size_t i;
                char label[64];
                for (i = 0; i < n; i++)
                    buf[i] = (uint8_t)(alphabets[alpha] == 256 ? rnd() : 'a' + rnd() % alphabets[alpha]);
                snprintf(label, sizeof(label), "alphabet %u length %zu", alphabets[alpha], n);
                sa_check_text(buf, n, label);
            }
        {
            size_t a = 1, b = 2, n = 0;
            buf[n++] = 'a';
            buf[n++] = 'b';
            while (n < 2500) {
                size_t len = a + b > 2500 - n ? 2500 - n : a;
                memmove(buf + n, buf, len);
                n += len;
                a = b;
                b = n;
            }
            sa_check_text(buf, n, "fibonacci string");
        }
        memset(buf, 0, 1000);
        sa_check_text(buf, 1000, "1000 zero bytes");
        {
            unsigned len, alpha_n, bad = 0;
            unsigned long total = 0;
            uint32_t ref[16];
            for (alpha_n = 2; alpha_n <= 3; alpha_n++)
                for (len = 1; len <= (alpha_n == 2 ? 14u : 9u); len++) {
                    unsigned long combos = 1, c;
                    unsigned d;
                    for (d = 0; d < len; d++)
                        combos *= alpha_n;
                    for (c = 0; c < combos; c++) {
                        unsigned long v = c;
                        dyn_sa_t* es;
                        for (d = 0; d < len; d++) {
                            buf[d] = (uint8_t)('a' + v % alpha_n);
                            v /= alpha_n;
                        }
                        es = dyn_sa_build(buf, len);
                        for (d = 0; d < len; d++)
                            ref[d] = d;
                        g_text = buf;
                        g_n = len;
                        qsort(ref, len, sizeof(uint32_t), naive_suffix_cmp);
                        if (!es || memcmp(dyn_sa_array(es), ref, sizeof(uint32_t) * len))
                            bad++;
                        dyn_sa_free(es);
                        total++;
                    }
                }
            CHECK(bad == 0, "exhaustive small strings: %u of %lu suffix arrays wrong", bad, total);
            printf("suffix array: %lu exhaustive strings (binary to 14, ternary to 9)\n", total);
        }
    }
    printf("suffix array ok\n");
}

typedef struct {
    uint8_t key[8];
    uint32_t bits;
    uint64_t val;
    int live;
} pt_ref;

static int pt_prefix_of(const uint8_t* p, uint32_t pbits, const uint8_t* k, uint32_t kbits)
{
    uint32_t i;
    if (pbits > kbits)
        return 0;
    for (i = 0; i < pbits; i++)
        if (((p[i >> 3] >> (7 - (i & 7))) & 1) != ((k[i >> 3] >> (7 - (i & 7))) & 1))
            return 0;
    return 1;
}

static int pt_same(const pt_ref* r, const uint8_t* k, uint32_t bits)
{
    return r->bits == bits && pt_prefix_of(r->key, bits, k, bits);
}

static int pt_count_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    (void)key;
    (void)bits;
    (void)val;
    (*(size_t*)ud)++;
    return 0;
}

typedef struct {
    uint32_t prev_bits;
    uint8_t prev[8];
    int have;
    int ordered;
} pt_order;

static int pt_bit_cmp(const uint8_t* a, uint32_t ab, const uint8_t* b, uint32_t bb)
{
    uint32_t i, l = ab < bb ? ab : bb;
    for (i = 0; i < l; i++) {
        int x = (a[i >> 3] >> (7 - (i & 7))) & 1, y = (b[i >> 3] >> (7 - (i & 7))) & 1;
        if (x != y)
            return x - y;
    }
    return ab < bb ? -1 : ab > bb;
}

static int pt_order_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    pt_order* o = (pt_order*)ud;
    (void)val;
    if (o->have && pt_bit_cmp(o->prev, o->prev_bits, key, bits) >= 0)
        o->ordered = 0;
    memset(o->prev, 0, 8);
    memcpy(o->prev, key, (bits + 7) / 8);
    o->prev_bits = bits;
    o->have = 1;
    return 0;
}

static const dyn_ds_ops U64V = { 8, NULL, NULL, NULL, NULL, NULL, NULL };

static void test_patricia(void)
{
    dyn_patricia_t* t = dyn_patricia_new(&U64V);
    pt_ref* ref = calloc(4000, sizeof(pt_ref));
    size_t nref = 0, i, j;
    long op;
    for (op = 0; op < 60000; op++) {
        uint8_t key[8];
        uint32_t bits = (uint32_t)(rnd() % 25);
        uint64_t v = rnd();
        unsigned kind = (unsigned)(rnd() % 10);
        uint64_t r = rnd();
        memcpy(key, &r, 8);
        key[0] &= 0x0F;
        if (kind < 5) {
            int found = 0;
            for (i = 0; i < nref; i++)
                if (ref[i].live && pt_same(&ref[i], key, bits)) {
                    ref[i].val = v;
                    found = 1;
                }
            if (!found && nref < 4000) {
                memset(ref[nref].key, 0, 8);
                memcpy(ref[nref].key, key, (bits + 7) / 8);
                ref[nref].bits = bits;
                ref[nref].val = v;
                ref[nref].live = 1;
                nref++;
            } else if (!found) {
                continue;
            }
            CHECK(dyn_patricia_put(t, key, bits, &v) == !found, "patricia put");
        } else if (kind < 7) {
            int found = 0;
            for (i = 0; i < nref; i++)
                if (ref[i].live && pt_same(&ref[i], key, bits)) {
                    ref[i].live = 0;
                    found = 1;
                }
            CHECK(dyn_patricia_remove(t, key, bits) == found, "patricia remove");
        } else if (kind < 9) {
            uint64_t* got = dyn_patricia_get(t, key, bits);
            pt_ref* want = NULL;
            for (i = 0; i < nref; i++)
                if (ref[i].live && pt_same(&ref[i], key, bits))
                    want = &ref[i];
            CHECK((got != NULL) == (want != NULL) && (!got || *got == want->val), "patricia get");
        } else {
            uint32_t qbits = 24, matched = 0;
            uint64_t* got = dyn_patricia_longest_prefix(t, key, qbits, &matched);
            pt_ref* want = NULL;
            for (i = 0; i < nref; i++)
                if (ref[i].live && pt_prefix_of(ref[i].key, ref[i].bits, key, qbits) && (!want || ref[i].bits > want->bits))
                    want = &ref[i];
            CHECK((got != NULL) == (want != NULL) && (!got || (*got == want->val && matched == want->bits)), "patricia longest prefix");
        }
        {
            size_t live = 0;
            for (j = 0; j < nref; j++)
                live += ref[j].live;
            CHECK(dyn_patricia_size(t) == live, "patricia size %zu want %zu", dyn_patricia_size(t), live);
        }
    }
    {
        size_t visited = 0, live = 0;
        pt_order o;
        memset(&o, 0, sizeof(o));
        o.ordered = 1;
        dyn_patricia_walk(t, (const uint8_t*)"", 0, pt_count_visit, &visited);
        for (j = 0; j < nref; j++)
            live += ref[j].live;
        CHECK(visited == live, "patricia walk visits %zu of %zu", visited, live);
        dyn_patricia_walk(t, (const uint8_t*)"", 0, pt_order_visit, &o);
        CHECK(o.ordered, "patricia walk is in bit order, prefixes first");
        {
            uint8_t pre = 0x05;
            size_t sub = 0, want = 0;
            dyn_patricia_walk(t, &pre, 4, pt_count_visit, &sub);
            for (j = 0; j < nref; j++)
                if (ref[j].live && pt_prefix_of(&pre, 4, ref[j].key, ref[j].bits))
                    want++;
            CHECK(sub == want, "patricia prefix walk %zu want %zu", sub, want);
        }
    }
    dyn_patricia_free(t);
    free(ref);
    {
        dyn_patricia_t* rt = dyn_patricia_new(&U64V);
        uint8_t net10[4] = { 10, 0, 0, 0 }, net10_1[4] = { 10, 1, 0, 0 }, any[4] = { 0 }, host[4];
        uint64_t a = 1, b = 2, d = 3;
        uint32_t m = 0;
        dyn_patricia_put(rt, net10, 8, &a);
        dyn_patricia_put(rt, net10_1, 16, &b);
        dyn_patricia_put(rt, any, 0, &d);
        memcpy(host, (uint8_t[4]) { 10, 1, 2, 3 }, 4);
        CHECK(*(uint64_t*)dyn_patricia_longest_prefix(rt, host, 32, &m) == 2 && m == 16, "10.1.2.3 routes to 10.1.0.0/16");
        memcpy(host, (uint8_t[4]) { 10, 2, 3, 4 }, 4);
        CHECK(*(uint64_t*)dyn_patricia_longest_prefix(rt, host, 32, &m) == 1 && m == 8, "10.2.3.4 routes to 10.0.0.0/8");
        memcpy(host, (uint8_t[4]) { 192, 168, 1, 1 }, 4);
        CHECK(*(uint64_t*)dyn_patricia_longest_prefix(rt, host, 32, &m) == 3 && m == 0, "192.168.1.1 takes the default route");
        dyn_patricia_remove(rt, net10_1, 16);
        memcpy(host, (uint8_t[4]) { 10, 1, 2, 3 }, 4);
        CHECK(*(uint64_t*)dyn_patricia_longest_prefix(rt, host, 32, &m) == 1 && m == 8, "after removing /16, 10.1.2.3 routes to /8");
        dyn_patricia_free(rt);
    }
    printf("patricia ok\n");
}

static void test_csr(void)
{
    unsigned trial;
    for (trial = 0; trial < 24; trial++) {
        uint32_t n = 1 + (uint32_t)(rnd() % (trial < 12 ? 60 : 1500));
        size_t m = (size_t)(rnd() % (n * 4 + 1)), i;
        int directed = (int)(trial & 1);
        dyn_csr_edge* e = malloc(sizeof(dyn_csr_edge) * (m ? m : 1));
        dyn_csr_t* g;
        int32_t *bfs = malloc(sizeof(int32_t) * n), *pred = malloc(sizeof(int32_t) * n);
        double *dist = malloc(sizeof(double) * n), *bf = malloc(sizeof(double) * n), *pr = malloc(sizeof(double) * n), *pr2 = malloc(sizeof(double) * n);
        uint32_t *comp = malloc(sizeof(uint32_t) * n), src = (uint32_t)(rnd() % n), k;
        for (i = 0; i < m; i++) {
            e[i].src = (uint32_t)(rnd() % n);
            e[i].dst = (uint32_t)(rnd() % n);
            e[i].w = (double)(1 + rnd() % 100);
        }
        g = dyn_csr_build(n, e, m, directed);
        CHECK(g && dyn_csr_nodes(g) == n && dyn_csr_edges(g) == (directed ? m : 2 * m), "csr build");
        CHECK(dyn_csr_bfs(g, src, bfs) == 0, "csr bfs");
        {
            int32_t* d = xmalloc(sizeof(int32_t) * n);
            int changed = 1;
            for (k = 0; k < n; k++)
                d[k] = -1;
            d[src] = 0;
            while (changed) {
                changed = 0;
                for (i = 0; i < m; i++) {
                    uint32_t a = e[i].src, b = e[i].dst;
                    if (d[a] >= 0 && (d[b] < 0 || d[b] > d[a] + 1)) {
                        d[b] = d[a] + 1;
                        changed = 1;
                    }
                    if (!directed && d[b] >= 0 && (d[a] < 0 || d[a] > d[b] + 1)) {
                        d[a] = d[b] + 1;
                        changed = 1;
                    }
                }
            }
            CHECK(!memcmp(d, bfs, sizeof(int32_t) * n), "csr bfs matches relaxation");
            free(d);
        }
        CHECK(dyn_csr_dijkstra(g, src, dist, pred) == 0, "csr dijkstra");
        for (k = 0; k < n; k++)
            bf[k] = INFINITY;
        bf[src] = 0;
        for (k = 0; k < n; k++) {
            int changed = 0;
            for (i = 0; i < m; i++) {
                uint32_t a = e[i].src, b = e[i].dst;
                if (bf[a] + e[i].w < bf[b]) {
                    bf[b] = bf[a] + e[i].w;
                    changed = 1;
                }
                if (!directed && bf[b] + e[i].w < bf[a]) {
                    bf[a] = bf[b] + e[i].w;
                    changed = 1;
                }
            }
            if (!changed)
                break;
        }
        CHECK(!memcmp(bf, dist, sizeof(double) * n), "csr dijkstra matches Bellman-Ford");
        for (k = 0; k < n; k++)
            if (pred[k] >= 0) {
                const double* w;
                uint32_t cnt, j, ok = 0;
                const uint32_t* nb = dyn_csr_neighbors(g, (uint32_t)pred[k], &w, &cnt);
                for (j = 0; j < cnt; j++)
                    if (nb[j] == k && dist[(uint32_t)pred[k]] + w[j] == dist[k])
                        ok = 1;
                CHECK(ok, "csr predecessor edge is tight");
            }
        {
            uint32_t nc = dyn_csr_components(g, comp), *lab = xmalloc(sizeof(uint32_t) * n), next = 0;
            for (k = 0; k < n; k++)
                lab[k] = UINT32_MAX;
            for (k = 0; k < n; k++) {
                int changed = 1;
                if (lab[k] != UINT32_MAX)
                    continue;
                lab[k] = next;
                while (changed) {
                    changed = 0;
                    for (i = 0; i < m; i++) {
                        uint32_t a = e[i].src, b = e[i].dst;
                        if (lab[a] == next && lab[b] != next) {
                            lab[b] = next;
                            changed = 1;
                        }
                        if (lab[b] == next && lab[a] != next) {
                            lab[a] = next;
                            changed = 1;
                        }
                    }
                }
                next++;
            }
            CHECK(nc == next && !memcmp(lab, comp, sizeof(uint32_t) * n), "csr weak components match a flood fill (%u vs %u)", nc, next);
            free(lab);
        }
        {
            int it = dyn_csr_pagerank(g, 0.85, 200, 1e-14, pr);
            double sum = 0, maxdiff = 0;
            uint32_t r;
            CHECK(it >= 0, "pagerank runs");
            for (k = 0; k < n; k++) {
                sum += pr[k];
                pr2[k] = 1.0 / n;
            }
            CHECK(fabs(sum - 1) < 1e-9, "pagerank sums to 1 (%.15f)", sum);
            for (r = 0; r < (uint32_t)(it + 1) && r < 200; r++) {
                double* nx = calloc(n, sizeof(double));
                double dang = 0;
                uint32_t* deg = xcalloc(n, sizeof(uint32_t));
                for (i = 0; i < m; i++) {
                    deg[e[i].src]++;
                    if (!directed)
                        deg[e[i].dst]++;
                }
                for (k = 0; k < n; k++)
                    if (!deg[k])
                        dang += pr2[k];
                for (i = 0; i < m; i++) {
                    nx[e[i].dst] += pr2[e[i].src] / deg[e[i].src];
                    if (!directed)
                        nx[e[i].src] += pr2[e[i].dst] / deg[e[i].dst];
                }
                for (k = 0; k < n; k++)
                    pr2[k] = 0.15 / n + 0.85 * (nx[k] + dang / n);
                free(nx);
                free(deg);
            }
            for (k = 0; k < n; k++)
                if (fabs(pr[k] - pr2[k]) > maxdiff)
                    maxdiff = fabs(pr[k] - pr2[k]);
            CHECK(maxdiff < 1e-12, "pagerank matches an edge-list power iteration (max diff %g)", maxdiff);
        }
        dyn_csr_free(g);
        free(e);
        free(bfs);
        free(pred);
        free(dist);
        free(bf);
        free(pr);
        free(pr2);
        free(comp);
    }
    {
        dyn_csr_edge bad = { 0, 5, 1 };
        CHECK(dyn_csr_build(3, &bad, 1, 1) == NULL, "an edge to a missing node is refused");
        bad.dst = 1;
        bad.w = -1;
        {
            dyn_csr_t* g = dyn_csr_build(3, &bad, 1, 1);
            double d[3];
            CHECK(g && dyn_csr_dijkstra(g, 0, d, NULL) == -1, "dijkstra refuses a negative weight");
            dyn_csr_free(g);
        }
    }
    printf("csr ok\n");
}

typedef struct {
    double lo[8], hi[8];
    uint64_t id;
    int live;
} rs_ref;

static int rs_overlaps(const rs_ref* r, const double* lo, const double* hi, unsigned dims)
{
    unsigned d;
    for (d = 0; d < dims; d++)
        if (r->hi[d] < lo[d] || hi[d] < r->lo[d])
            return 0;
    return 1;
}

static double rs_mindist2(const rs_ref* r, const double* p, unsigned dims)
{
    double s = 0;
    unsigned d;
    for (d = 0; d < dims; d++) {
        double c = p[d] < r->lo[d] ? r->lo[d] - p[d] : p[d] > r->hi[d] ? p[d] - r->hi[d]
                                                                       : 0;
        s += c * c;
    }
    return s;
}

static int dbl_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void test_rstar(unsigned dims)
{
    dyn_rstar_t* t = dyn_rstar_new(dims);
    size_t cap = 25000, n = 0, i;
    rs_ref* ref = xcalloc(cap, sizeof(rs_ref));
    long op;
    for (op = 0; op < 40000; op++) {
        unsigned kind = (unsigned)(rnd() % 10);
        if (kind < 6 && n < cap) {
            rs_ref* r = &ref[n];
            unsigned d;
            for (d = 0; d < dims; d++) {
                double c = (double)(rnd() % 100000) / 100, w = (double)(rnd() % 300) / 100;
                r->lo[d] = c;
                r->hi[d] = c + w;
            }
            r->id = (uint64_t)n;
            r->live = 1;
            CHECK(dyn_rstar_insert(t, r->lo, r->hi, r->id) == 0, "rstar insert");
            n++;
        } else if (kind < 8 && n) {
            rs_ref* r = &ref[rnd() % n];
            int want = r->live;
            CHECK(dyn_rstar_remove(t, r->lo, r->hi, r->id) == want, "rstar remove %llu", (unsigned long long)r->id);
            r->live = 0;
        } else {
            double lo[3], hi[3];
            unsigned d;
            size_t want = 0;
            for (d = 0; d < dims; d++) {
                lo[d] = (double)(rnd() % 100000) / 100;
                hi[d] = lo[d] + (double)(rnd() % 3000) / 100;
            }
            for (i = 0; i < n; i++)
                if (ref[i].live && rs_overlaps(&ref[i], lo, hi, dims))
                    want++;
            CHECK(dyn_rstar_search(t, lo, hi, NULL, NULL) == want, "rstar search count");
        }
        if (op % 5000 == 0)
            CHECK(dyn_rstar_check(t) == 0, "rstar invariants at op %ld (dims %u)", op, dims);
    }
    CHECK(dyn_rstar_check(t) == 0, "rstar final invariants (dims %u)", dims);
    {
        size_t live = 0;
        for (i = 0; i < n; i++)
            live += ref[i].live;
        CHECK(dyn_rstar_size(t) == live, "rstar size");
        CHECK(dyn_rstar_height(t) <= 8, "rstar height %u", dyn_rstar_height(t));
        printf("rstar dims %u: %zu boxes, height %u\n", dims, live, dyn_rstar_height(t));
    }
    {
        unsigned q;
        for (q = 0; q < 50; q++) {
            double p[3], got_d[10], *all = malloc(sizeof(double) * n);
            uint64_t ids[10];
            size_t k = 10, got, na = 0, j;
            unsigned d;
            for (d = 0; d < dims; d++)
                p[d] = (double)(rnd() % 100000) / 100;
            got = dyn_rstar_nearest(t, p, k, ids, got_d);
            for (i = 0; i < n; i++)
                if (ref[i].live)
                    all[na++] = rs_mindist2(&ref[i], p, dims);
            qsort(all, na, sizeof(double), dbl_cmp);
            CHECK(got == (na < k ? na : k), "rstar nearest count");
            for (j = 0; j < got; j++) {
                CHECK(got_d[j] == all[j], "rstar nearest distance %zu", j);
                CHECK(ref[ids[j]].live && rs_mindist2(&ref[ids[j]], p, dims) == got_d[j], "rstar nearest id matches its distance");
            }
            free(all);
        }
    }
    for (i = 0; i < n; i++)
        if (ref[i].live)
            CHECK(dyn_rstar_remove(t, ref[i].lo, ref[i].hi, ref[i].id) == 1, "rstar remove all");
    CHECK(dyn_rstar_size(t) == 0 && dyn_rstar_check(t) == 0, "rstar empty after removing all");
    {
        double lo[3] = { 1, 2, 3 }, hi[3] = { 0, 2, 3 };
        CHECK(dyn_rstar_insert(t, lo, hi, 1) == -1, "an inverted box is refused");
    }
    dyn_rstar_free(t);
    free(ref);
    CHECK(dyn_rstar_new(0) == NULL && dyn_rstar_new(9) == NULL, "rstar dimension bounds");
}

static void test_rstar_bulk(unsigned dims)
{
    static const size_t sizes[] = { 0, 1, 16, 17, 33, 100, 257, 4097, 30000 };
    size_t si;
    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        size_t n = sizes[si], i, q;
        dyn_rstar_t* t = dyn_rstar_new(dims);
        double* bx = malloc(sizeof(double) * 2 * dims * (n + 1));
        uint64_t* ids = malloc(sizeof(uint64_t) * (n + 1));
        rs_ref* ref = xcalloc(n + 1, sizeof(rs_ref));
        for (i = 0; i < n; i++) {
            unsigned d;
            for (d = 0; d < dims; d++) {
                double c = (double)(rnd() % 100000) / 100, w = (double)(rnd() % 300) / 100;
                ref[i].lo[d] = bx[i * 2 * dims + d] = c;
                ref[i].hi[d] = bx[i * 2 * dims + dims + d] = c + w;
            }
            ref[i].id = ids[i] = (uint64_t)i;
            ref[i].live = 1;
        }
        CHECK(dyn_rstar_bulk_load(t, bx, ids, n) == 0, "rstar bulk load n=%zu dims %u", n, dims);
        CHECK(dyn_rstar_size(t) == n, "rstar bulk size %zu", n);
        CHECK(dyn_rstar_check(t) == 0, "rstar bulk invariants n=%zu dims %u", n, dims);
        CHECK(dyn_rstar_height(t) <= 6, "rstar bulk height %u for n=%zu", dyn_rstar_height(t), n);
        for (q = 0; q < 40; q++) {
            double lo[8], hi[8];
            unsigned d;
            size_t want = 0;
            for (d = 0; d < dims; d++) {
                lo[d] = (double)(rnd() % 100000) / 100;
                hi[d] = lo[d] + (double)(rnd() % 3000) / 100;
            }
            for (i = 0; i < n; i++)
                if (rs_overlaps(&ref[i], lo, hi, dims))
                    want++;
            CHECK(dyn_rstar_search(t, lo, hi, NULL, NULL) == want, "rstar bulk search count n=%zu", n);
        }
        if (n) {
            double lo[8], hi[8];
            unsigned d;
            for (d = 0; d < dims; d++) {
                lo[d] = 0;
                hi[d] = 2000;
            }
            CHECK(dyn_rstar_insert(t, lo, hi, 999999) == 0 && dyn_rstar_check(t) == 0, "rstar insert after bulk load");
            CHECK(dyn_rstar_remove(t, ref[0].lo, ref[0].hi, 0) == 1 && dyn_rstar_check(t) == 0, "rstar remove after bulk load");
            CHECK(dyn_rstar_bulk_load(t, bx, ids, n) == -1, "rstar bulk load refuses a non-empty tree");
        }
        dyn_rstar_free(t);
        free(bx);
        free(ids);
        free(ref);
    }
    {
        dyn_rstar_t* t = dyn_rstar_new(2);
        double bad[4] = { 1, 0, 0, 0 };
        uint64_t id = 1;
        CHECK(dyn_rstar_bulk_load(t, bad, &id, 1) == -1 && dyn_rstar_size(t) == 0, "rstar bulk load refuses an inverted box");
        dyn_rstar_free(t);
    }
}

static void test_rstar_bulk_sweep(void)
{
    static const unsigned dimset[] = { 1, 2, 8 };
    size_t di;
    for (di = 0; di < sizeof(dimset) / sizeof(dimset[0]); di++) {
        unsigned dims = dimset[di];
        size_t n, i;
        double* bx = xmalloc(sizeof(double) * 2 * dims * 2000);
        uint64_t* ids = malloc(sizeof(uint64_t) * 2000);
        for (i = 0; i < 2000; i++) {
            unsigned d;
            for (d = 0; d < dims; d++) {
                double c = (double)(rnd() % 100000) / 100;
                bx[i * 2 * dims + d] = c;
                bx[i * 2 * dims + dims + d] = c + (double)(rnd() % 300) / 100;
            }
            ids[i] = (uint64_t)i;
        }
        for (n = 1; n <= 2000; n++) {
            dyn_rstar_t* t = dyn_rstar_new(dims);
            int ok = dyn_rstar_bulk_load(t, bx, ids, n) == 0 && dyn_rstar_size(t) == n && dyn_rstar_check(t) == 0;
            double lo[8], hi[8];
            unsigned d;
            for (d = 0; d < dims; d++) {
                lo[d] = -1;
                hi[d] = 1e9;
            }
            ok = ok && dyn_rstar_search(t, lo, hi, NULL, NULL) == n;
            dyn_rstar_free(t);
            if (!ok) {
                CHECK(0, "rstar bulk sweep n=%zu dims %u", n, dims);
                break;
            }
        }
        CHECK(n == 2001, "rstar bulk sweep dims %u covered 1..2000", dims);
        free(bx);
        free(ids);
    }
}

typedef struct {
    uint8_t key[8];
    uint32_t bits;
    uint64_t val;
} pt_rec;

typedef struct {
    pt_rec* out;
    size_t n, cap;
} pt_dump;

static int pt_dump_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    pt_dump* d = (pt_dump*)ud;
    if (d->n < d->cap) {
        memset(d->out[d->n].key, 0, 8);
        memcpy(d->out[d->n].key, key, (bits + 7) / 8);
        d->out[d->n].bits = bits;
        memcpy(&d->out[d->n].val, val, 8);
    }
    d->n++;
    return 0;
}

static int pt_rec_cmp(const void* a, const void* b)
{
    const pt_rec* x = (const pt_rec*)a;
    const pt_rec* y = (const pt_rec*)b;
    return pt_bit_cmp(x->key, x->bits, y->key, y->bits);
}

typedef struct {
    uint8_t key[48];
    uint32_t bits;
    uint64_t val;
    int live;
} ptl_ref;

static int ptl_prefix(const uint8_t* a, uint32_t abits, const uint8_t* b, uint32_t bbits)
{
    uint32_t i;
    if (abits > bbits)
        return 0;
    for (i = 0; i < abits; i++)
        if (((a[i >> 3] >> (7 - (i & 7))) & 1) != ((b[i >> 3] >> (7 - (i & 7))) & 1))
            return 0;
    return 1;
}

static int ptl_count_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    (void)key;
    (void)bits;
    (void)val;
    ++*(size_t*)ud;
    return 0;
}

static void ptl_key(uint8_t* k, uint32_t* bits)
{
    uint32_t len = (uint32_t)(rnd() % 41), i;
    memset(k, 0, 48);
    for (i = 0; i < len; i++)
        k[i] = (uint8_t)('a' + (rnd() % 100 < 85 ? 0 : 1));
    *bits = len * 8;
    if (len && rnd() % 3 == 0)
        *bits -= (uint32_t)(rnd() % 8);
}

static void test_patricia_long(void)
{
    dyn_patricia_t* t = dyn_patricia_new(&U64V);
    ptl_ref* ref = calloc(3000, sizeof(ptl_ref));
    size_t nref = 0, i;
    long op;
    for (op = 0; op < 40000; op++) {
        uint8_t key[48];
        uint32_t bits;
        uint64_t v = rnd();
        unsigned kind = (unsigned)(rnd() % 10);
        ptl_ref* want = NULL;
        ptl_key(key, &bits);
        for (i = 0; i < nref; i++)
            if (ref[i].live && ref[i].bits == bits && ptl_prefix(ref[i].key, bits, key, bits))
                want = &ref[i];
        if (kind < 5) {
            if (!want && nref == 3000)
                continue;
            CHECK(dyn_patricia_put(t, key, bits, &v) == !want, "patricia long put");
            if (!want) {
                want = &ref[nref++];
                memcpy(want->key, key, 48);
                want->bits = bits;
                want->live = 1;
            }
            want->val = v;
        } else if (kind < 7) {
            CHECK(dyn_patricia_remove(t, key, bits) == (want != NULL), "patricia long remove");
            if (want)
                want->live = 0;
        } else if (kind < 8) {
            uint64_t* got = dyn_patricia_get(t, key, bits);
            CHECK((got != NULL) == (want != NULL) && (!got || *got == want->val), "patricia long get");
        } else if (kind < 9) {
            uint32_t matched = 0;
            uint64_t* got;
            want = NULL;
            for (i = 0; i < nref; i++)
                if (ref[i].live && ptl_prefix(ref[i].key, ref[i].bits, key, bits) && (!want || ref[i].bits > want->bits))
                    want = &ref[i];
            got = dyn_patricia_longest_prefix(t, key, bits, &matched);
            CHECK((got != NULL) == (want != NULL) && (!got || (*got == want->val && matched == want->bits)), "patricia long longest prefix");
        } else {
            size_t seen = 0, model = 0;
            for (i = 0; i < nref; i++)
                model += ref[i].live && ptl_prefix(key, bits, ref[i].key, ref[i].bits);
            dyn_patricia_walk(t, key, bits, ptl_count_visit, &seen);
            CHECK(seen == model, "patricia long walk prefix %zu want %zu", seen, model);
        }
    }
    {
        size_t live = 0;
        for (i = 0; i < nref; i++)
            live += ref[i].live;
        CHECK(dyn_patricia_size(t) == live, "patricia long size");
    }
    dyn_patricia_free(t);
    free(ref);
}

static void test_patricia_chain(void)
{
    dyn_patricia_t* t = dyn_patricia_new(&U64V);
    dyn_patricia_cursor cur;
    uint8_t key[200];
    uint32_t k, matched;
    size_t seen;
    memset(key, 'a', sizeof(key));
    memset(&cur, 0, sizeof(cur));
    for (k = 1; k <= 150; k++) {
        uint64_t v = k;
        CHECK(dyn_patricia_put(t, key, k * 8, &v) == 1, "chain put %u", k);
    }
    for (k = 1; k <= 150; k += 3)
        CHECK(dyn_patricia_remove(t, key, k * 8) == 1, "chain remove %u", k);
    for (k = 1; k <= 160; k++) {
        uint32_t want = 0, j;
        uint64_t* got;
        key[k] = 'b';
        for (j = 1; j <= k && j <= 150; j++)
            if ((j - 1) % 3 != 0)
                want = j;
        got = dyn_patricia_longest_prefix(t, key, (k + 1) * 8, &matched);
        CHECK(want ? (got && *got == want && matched == want * 8) : got == NULL, "chain longest prefix %u", k);
        key[k] = 'a';
        got = dyn_patricia_get(t, key, k * 8);
        CHECK(k <= 150 && (k - 1) % 3 != 0 ? (got && *got == k) : got == NULL, "chain get %u", k);
    }
    seen = 0;
    dyn_patricia_walk(t, key, 600, ptl_count_visit, &seen);
    CHECK(seen == 51, "chain walk under 75 bytes saw %zu", seen);
    dyn_patricia_free(t);
    t = dyn_patricia_new(&U64V);
    for (k = 1; k <= 150; k++) {
        uint64_t v = k;
        CHECK(dyn_patricia_put_cursor(t, &cur, key, k * 8, &v) == 1, "chain cursor put %u", k);
    }
    for (k = 1; k <= 150; k++) {
        uint64_t* got = dyn_patricia_get(t, key, k * 8);
        CHECK(got && *got == k, "chain cursor get %u", k);
    }
    dyn_patricia_cursor_free(&cur);
    dyn_patricia_free(t);
}

static void test_patricia_oom(void)
{
    long k;
    for (k = 0; k < 400; k++) {
        dyn_patricia_t* t;
        uint8_t ok[1200];
        uint32_t i, n = 0;
        size_t live = 0;
        ds_fail_after = k;
        t = dyn_patricia_new(&U64V);
        if (!t) {
            ds_fail_after = -1;
            continue;
        }
        for (i = 0; i < 1200; i++) {
            uint8_t key[24];
            uint64_t v = i;
            uint32_t len = i % 7 == 0 ? 20 : 4;
            memset(key, 0, sizeof(key));
            memcpy(key, &i, 4);
            ok[i] = (uint8_t)(dyn_patricia_put(t, key, len * 8, &v) == 1);
        }
        ds_fail_after = -1;
        for (i = 0; i < 1200; i++) {
            uint8_t key[24];
            uint32_t len = i % 7 == 0 ? 20 : 4;
            uint64_t* got;
            memset(key, 0, sizeof(key));
            memcpy(key, &i, 4);
            got = dyn_patricia_get(t, key, len * 8);
            CHECK(ok[i] ? (got && *got == i) : got == NULL, "oom %ld key %u", k, i);
            live += ok[i];
            n += ok[i];
        }
        CHECK(dyn_patricia_size(t) == live, "oom %ld size", k);
        live = 0;
        dyn_patricia_walk(t, (const uint8_t*)"", 0, ptl_count_visit, &live);
        CHECK(live == n, "oom %ld walk", k);
        for (i = 0; i < 1200; i++) {
            uint8_t key[24];
            uint64_t v = i;
            uint32_t len = i % 7 == 0 ? 20 : 4;
            memset(key, 0, sizeof(key));
            memcpy(key, &i, 4);
            if (!ok[i])
                CHECK(dyn_patricia_put(t, key, len * 8, &v) == 1, "oom %ld retry %u", k, i);
        }
        CHECK(dyn_patricia_size(t) == 1200, "oom %ld refill", k);
        dyn_patricia_free(t);
    }
}

static void test_patricia_cursor(void)
{
    static const char* orders[] = { "random", "sorted", "sorted-dup", "reversed", "sorted-runs" };
    size_t o;
    for (o = 0; o < sizeof(orders) / sizeof(orders[0]); o++) {
        size_t n = 3000, i, mism = 0;
        pt_rec* in = calloc(n, sizeof(pt_rec));
        pt_dump d1 = { calloc(n, sizeof(pt_rec)), 0, n }, d2 = { calloc(n, sizeof(pt_rec)), 0, n };
        dyn_patricia_t* t1 = dyn_patricia_new(&U64V);
        dyn_patricia_t* t2 = dyn_patricia_new(&U64V);
        dyn_patricia_cursor cur;
        memset(&cur, 0, sizeof(cur));
        for (i = 0; i < n; i++) {
            uint64_t r = rnd();
            memcpy(in[i].key, &r, 8);
            in[i].key[0] &= (uint8_t)(o == 2 ? 0x03 : 0x0F);
            in[i].bits = (uint32_t)(rnd() % (o == 2 ? 13 : 33));
            if (in[i].bits < 64)
                memset(in[i].key + (in[i].bits + 7) / 8, 0, 8 - (in[i].bits + 7) / 8);
            if (in[i].bits & 7)
                in[i].key[in[i].bits / 8] &= (uint8_t)(0xFF << (8 - (in[i].bits & 7)));
            in[i].val = rnd();
        }
        if (o != 0)
            qsort(in, n, sizeof(pt_rec), pt_rec_cmp);
        if (o == 3)
            for (i = 0; i < n / 2; i++) {
                pt_rec tmp = in[i];
                in[i] = in[n - 1 - i];
                in[n - 1 - i] = tmp;
            }
        if (o == 4)
            for (i = 0; i + 50 <= n; i += 100)
                for (size_t j = 0; j < 25; j++) {
                    pt_rec tmp = in[i + j];
                    in[i + j] = in[i + 49 - j];
                    in[i + 49 - j] = tmp;
                }
        for (i = 0; i < n; i++) {
            int a = dyn_patricia_put(t1, in[i].key, in[i].bits, &in[i].val);
            int b = dyn_patricia_put_cursor(t2, &cur, in[i].key, in[i].bits, &in[i].val);
            if (a != b || dyn_patricia_size(t1) != dyn_patricia_size(t2))
                mism++;
        }
        CHECK(mism == 0, "patricia cursor put (%s): %zu return/size mismatches", orders[o], mism);
        dyn_patricia_walk(t1, (const uint8_t*)"", 0, pt_dump_visit, &d1);
        dyn_patricia_walk(t2, (const uint8_t*)"", 0, pt_dump_visit, &d2);
        CHECK(d1.n == d2.n && d1.n == dyn_patricia_size(t1), "patricia cursor put (%s): walk sizes %zu %zu", orders[o], d1.n, d2.n);
        CHECK(d1.n <= n && memcmp(d1.out, d2.out, d1.n * sizeof(pt_rec)) == 0, "patricia cursor put (%s): same keys, bits and values in walk order", orders[o]);
        for (i = 0; i < n; i++) {
            uint64_t* g = dyn_patricia_get(t2, in[i].key, in[i].bits);
            uint64_t* w = dyn_patricia_get(t1, in[i].key, in[i].bits);
            if (!g || !w || *g != *w)
                break;
        }
        CHECK(i == n, "patricia cursor put (%s): every key reads back as with a plain put", orders[o]);
        dyn_patricia_cursor_free(&cur);
        dyn_patricia_free(t1);
        dyn_patricia_free(t2);
        free(in);
        free(d1.out);
        free(d2.out);
    }
}

static void test_chash_sort_ties(void)
{
    static const char* names[] = { "zeta", "alpha", "mid", "alpha2", "", "b" };
    size_t sizes[] = { 64, 4095, 4096, 20000 }, s;
    dyn_chash_t c;
    dyn_chash_node nodes[6];
    uint32_t i;
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 6; i++) {
        nodes[i].name = (char*)names[i];
        nodes[i].len = (uint32_t)strlen(names[i]);
        nodes[i].weight = 1;
    }
    c.nodes = nodes;
    c.nn = 6;
    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint32_t n = (uint32_t)sizes[s], bad = 0;
        dyn_chash_pt* a = xmalloc(sizeof(dyn_chash_pt) * n);
        dyn_chash_pt* b = malloc(sizeof(dyn_chash_pt) * n);
        for (i = 0; i < n; i++) {
            uint64_t r = rnd();
            a[i].h = (i % 3 == 0) ? (r % 40) << 50 : r;
            a[i].node = (uint32_t)((i * 5 + (r >> 60)) % 6);
        }
        memcpy(b, a, sizeof(dyn_chash_pt) * n);
        dyn_chash_sort(&c, a, n);
        ds_ch_sort_ctx = &c;
        qsort(b, n, sizeof(dyn_chash_pt), dyn_chash_pt_cmp);
        ds_ch_sort_ctx = NULL;
        for (i = 0; i < n; i++)
            if (a[i].h != b[i].h || strcmp(names[a[i].node], names[b[i].node]) != 0)
                bad++;
        CHECK(bad == 0, "chash ring sort n=%u: %u points differ from the comparator order", n, bad);
        free(a);
        free(b);
    }
}

int main(void)
{
    test_chash_sort_ties();
    test_sa();
    test_patricia();
    test_patricia_cursor();
    test_patricia_long();
    test_patricia_chain();
    test_patricia_oom();
    test_csr();
    test_rstar(2);
    test_rstar(3);
    test_rstar_bulk(2);
    test_rstar_bulk(3);
    test_rstar_bulk(1);
    test_rstar_bulk(8);
    test_rstar_bulk_sweep();
    printf("rstar ok\n");
    CHECK(ds_live_allocs == 0, "every allocation was freed (%ld live)", ds_live_allocs);
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
