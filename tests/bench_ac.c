/*
 * bench_ac.c -- audit E10-11 bench for the Aho-Corasick automaton (dyn-ac).
 *
 * WHY A C HARNESS AND NOT A SHELL SCRIPT. Three things the audit needs are
 * only reachable from C: (1) nanosecond clocks around insert/build/scan
 * separately; (2) the automaton's OWN memory accounting -- n_states x per-state
 * cost -- rather than a process-wide RSS smear; (3) the differential, which
 * links the current dyn_ac_* and a candidate implementation side by side in
 * one binary and compares their hit streams over the same corpus
 * (bench/http_parse_bench.c is the repo precedent for standalone C benches).
 *
 *   clang -O2 -std=gnu17 -Isrc/core -o .obj/bench_ac \
 *       tests/bench_ac.c src/core/dyn-ac.c && .obj/bench_ac
 *
 * Modes:
 *   .obj/bench_ac              baseline table (build + memory + scan rows)
 *   .obj/bench_ac --shape      automaton shape stats that discriminate the
 *                              redesign candidates (real-edge density,
 *                              childless states, distinct post-build rows)
 *   .obj/bench_ac --cap        build to the DYN_AC_MAX_STATES boundary via
 *                              the test_ac_caps counting construction
 *   .obj/bench_ac --diff       old vs candidate hit-stream differential over a
 *                              systematic corpus (needs the candidate linked:
 *                              -DHAVE_AC_CAND, tests/ac_cand.c)
 *
 * Determinism: every pattern set and text comes from a splitmix64 stream with
 * a fixed seed, so old-vs-new rows are the same bytes. Output is one
 * machine-readable line per measurement ("#AC ..."), diffable across runs.
 *
 * Scan cost convention: MB/s is 10^6 bytes/s; ns/byte is printed alongside.
 * The emit callback only bumps a volatile counter -- nothing else is in the
 * timed region, and the same emit shape runs on both implementations, so the
 * emit cost is a controlled constant across the comparison.
 */
#include "dyn-ac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

#ifdef HAVE_AC_CAND
#include "ac_cand.h"            /* candidate: dyn_da_* over dyn_ac_cand_t */
#endif

/* ---- deterministic randomness ------------------------------------------- */
static uint64_t sm_state = 0x243F6A8885A308D3ull;
static uint64_t sm64(void)
{
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t sm_below(uint32_t n) { return (uint32_t)(sm64() % n); }

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static long rss_kib(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (long)ru.ru_maxrss;  /* same units both calls, so deltas compare */
}

/* ---- pattern sets --------------------------------------------------------- */
/* One generator per shape the two consumers actually see:
 *   S10     10 short English words, heavy prefix sharing (the pinned shape)
 *   M1000   1000 medium pronounceable words (dict codec territory)
 *   X10000  10000 mixed URL/UTF-8-ish patterns (matcher territory)
 * And the adversarial shapes (pass-4 rows, cheap to include always):
 *   A2      200 patterns over the 2-symbol alphabet {a,b}
 *   P1      36 single-character patterns (max output-link pressure)
 *   L64     64 patterns sharing a 60-byte prefix (max fail-chain depth)
 */
#define NPAT_MAX 10000
typedef struct {
    uint8_t *buf;               /* all pattern bytes, back to back */
    size_t *off;                /* NPAT_MAX+1 offsets into buf */
    size_t *len;
    int n;
} patset_t;

static void patset_free(patset_t *ps)
{
    free(ps->buf); free(ps->off); free(ps->len);
    memset(ps, 0, sizeof *ps);
}

static void patset_push(patset_t *ps, const uint8_t *p, size_t l)
{
    if (ps->n >= NPAT_MAX) {
        fprintf(stderr, "patset_push: set overflow\n");
        exit(1);
    }
    memcpy(ps->buf + ps->off[ps->n], p, l);
    ps->len[ps->n] = l;
    ps->n++;
    ps->off[ps->n] = ps->off[ps->n - 1] + l;
}

static const char *S10_WORDS[] = {
    "he", "she", "his", "hers", "her",
    "herself", "here", "there", "three", "hand"
};

static void gen_s10(patset_t *ps)
{
    for (int i = 0; i < 10; i++)
        patset_push(ps, (const uint8_t *)S10_WORDS[i], strlen(S10_WORDS[i]));
}

/* pronounceable: consonant-vowel alternation, seeded */
static void gen_word(uint8_t *out, size_t l)
{
    static const char *C = "bcdfghjklmnpqrstvwxyz";
    static const char *V = "aeiou";
    for (size_t i = 0; i < l; i++)
        out[i] = (uint8_t)((i & 1) ? V[sm_below(5)] : C[sm_below(21)]);
}

static void gen_m1000(patset_t *ps)
{
    uint8_t w[32];
    for (int i = 0; i < 1000; i++) {
        size_t l = 6 + (size_t)(i % 3) * 3;         /* 6, 9, 12 */
        gen_word(w, l);
        patset_push(ps, w, l);
    }
}

static void gen_x10000(patset_t *ps)
{
    static const char *SHORT_A = "abcdefghijklmnopqrstuvwxyz0123456789";
    static const char *URL_A = "abcdefghijklmnopqrstuvwxyz0123456789-_/.";
    static const char *Q_A = "abcdefghijklmnopqrstuvwxyz0123456789./:%?=&";
    uint8_t w[32];
    for (int i = 0; i < 10000; i++) {
        switch (i & 3) {
        case 0: {                                   /* short 2-4 */
            size_t l = 2 + sm_below(3);
            for (size_t k = 0; k < l; k++) w[k] = (uint8_t)SHORT_A[sm_below(36)];
            patset_push(ps, w, l);
            break;
        }
        case 1: {                                   /* URL-ish 6-12 */
            size_t l = 6 + sm_below(7);
            for (size_t k = 0; k < l; k++) w[k] = (uint8_t)URL_A[sm_below(39)];
            patset_push(ps, w, l);
            break;
        }
        case 2: {                                   /* query-ish 14-24 */
            size_t l = 14 + sm_below(11);
            for (size_t k = 0; k < l; k++) w[k] = (uint8_t)Q_A[sm_below(42)];
            patset_push(ps, w, l);
            break;
        }
        default: {                                  /* UTF-8-ish 4-10 */
            size_t l = 4 + sm_below(7), k = 0;
            while (k < l) {
                if (sm_below(3) == 0 && k + 2 <= l) {
                    w[k++] = 0xC3;                  /* U+00E9-ish */
                    w[k++] = (uint8_t)(0xA8 + sm_below(8));
                } else {
                    w[k++] = (uint8_t)URL_A[sm_below(39)];
                }
            }
            patset_push(ps, w, l);
            break;
        }
        }
        if (i % 100 == 99 && i < NPAT_MAX - 2) {    /* prefix cluster: one more
                                                     * pattern that extends the
                                                     * previous one by 3 bytes
                                                     * (bounded: the cluster
                                                     * consumes an extra i) */
            size_t b = ps->off[ps->n - 1], l = ps->len[ps->n - 1];
            uint8_t ext[32];
            size_t take = l < 24 ? l : 24;
            memcpy(ext, ps->buf + b, take);
            for (int k = 0; k < 3; k++)
                ext[take + (size_t)k] = (uint8_t)URL_A[sm_below(39)];
            patset_push(ps, ext, take + 3);
            i++;
        }
    }
}

static void gen_a2(patset_t *ps)
{
    uint8_t w[32];
    for (int i = 0; i < 200; i++) {
        size_t l = 8 + sm_below(17);                /* 8-24 over {a,b} */
        for (size_t k = 0; k < l; k++) w[k] = (uint8_t)('a' + sm_below(2));
        patset_push(ps, w, l);
    }
}

static void gen_p1(patset_t *ps)
{
    for (int i = 0; i < 36; i++) {
        uint8_t c = (uint8_t)(i < 26 ? 'a' + i : '0' + (i - 26));
        patset_push(ps, &c, 1);
    }
}

static void gen_l64(patset_t *ps)
{
    uint8_t w[68];
    memset(w, 'K', 60);                             /* shared 60-byte prefix */
    for (int i = 0; i < 64; i++) {
        w[60] = (uint8_t)('a' + i % 26);
        w[61] = (uint8_t)('a' + (i / 26) % 26);
        w[62] = (uint8_t)('0' + i % 10);
        w[63] = (uint8_t)('0' + (i / 10) % 10);
        patset_push(ps, w, 64);
    }
}

typedef void (*patgen_t)(patset_t *);

/* ---- text classes --------------------------------------------------------- */
#define TEXT_N ((size_t)8 << 20)                    /* 8 MiB per class */

/* byte usage map of the set: which bytes ANY pattern contains */
static void used_bytes(const patset_t *ps, uint8_t *map /* 256 */)
{
    memset(map, 0, 256);
    for (int i = 0; i < ps->n; i++) {
        const uint8_t *p = ps->buf + ps->off[i];
        for (size_t k = 0; k < ps->len[i]; k++) map[p[k]] = 1;
    }
}

/* a small alphabet disjoint from every pattern byte (for no-match texts and
 * near-miss terminators); returns count, 0 if the set swallows everything */
static int disjoint_alphabet(const patset_t *ps, uint8_t *out, int want)
{
    uint8_t map[256];
    int n = 0, c;
    used_bytes(ps, map);
    for (c = 0x20; c < 0x100 && n < want; c++)
        if (!map[c]) out[n++] = (uint8_t)c;
    for (c = 0x00; c < 0x20 && n < want; c++)
        if (!map[c]) out[n++] = (uint8_t)c;
    return n;
}

/* (a) many matches: random patterns back to back with separators */
static void text_many(const patset_t *ps, uint8_t *t, size_t n,
                      const uint8_t *sep, int nsep)
{
    size_t i = 0;
    while (i < n) {
        int pi = sm_below((uint32_t)ps->n);
        size_t l = ps->len[pi];
        if (i + l + 1 > n) break;
        memcpy(t + i, ps->buf + ps->off[pi], l);
        i += l;
        t[i++] = sep[sm_below((uint32_t)nsep)];
    }
    while (i < n) t[i++] = sep[0];
}

/* (b) no matches: random bytes over the disjoint alphabet */
static void text_none(const patset_t *ps, uint8_t *t, size_t n)
{
    uint8_t sep[16];
    int ns = disjoint_alphabet(ps, sep, 16);
    if (ns == 0) {                                  /* set swallows the whole
                                                     * byte space: fall back to
                                                     * high-bit bytes */
        for (size_t i = 0; i < n; i++) t[i] = (uint8_t)(0x80 + sm_below(0x80));
        return;
    }
    for (size_t i = 0; i < n; i++) t[i] = sep[sm_below((uint32_t)ns)];
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* (c) adversarial near-miss: prefixes of the LONGEST patterns, each cut one
 * byte short and terminated by a byte that cannot extend the walk -- the
 * automaton dives to max depth then fails, per segment. */
static void text_near(const patset_t *ps, uint8_t *t, size_t n)
{
    uint8_t sep[16];
    int ns = disjoint_alphabet(ps, sep, 16);
    static int idx[NPAT_MAX];
    int nl = 0, want;
    size_t thresh, *lens;
    size_t i = 0;

    if (ns == 0)
        for (int k = 0; k < 16; k++) sep[k] = (uint8_t)(0x80 + k), ns = 16;

    want = ps->n / 10 > 8 ? ps->n / 10 : 8;
    lens = malloc((size_t)ps->n * sizeof *lens);
    memcpy(lens, ps->len, (size_t)ps->n * sizeof *lens);
    qsort(lens, (size_t)ps->n, sizeof *lens, cmp_size);
    thresh = lens[want - 1];                        /* top-decile length */
    free(lens);
    for (int k = 0; k < ps->n; k++)
        if (ps->len[k] >= thresh && ps->len[k] >= 2) idx[nl++] = k;
    if (nl == 0)
        for (int k = 0; k < ps->n; k++)
            if (ps->len[k] >= 2) idx[nl++] = k;
    if (nl == 0) {                                  /* every pattern is 1 byte:
                                                     * nothing can be cut short,
                                                     * so near degrades to none */
        text_none(ps, t, n);
        return;
    }

    while (i < n) {
        int pi = idx[sm_below((uint32_t)nl)];
        size_t l = ps->len[pi] - 1;                 /* all but the last byte */
        if (i + l + 1 > n) break;
        memcpy(t + i, ps->buf + ps->off[pi], l);
        t[i + l] = sep[sm_below((uint32_t)ns)];
        i += l + 1;
    }
    while (i < n) t[i++] = sep[0];
}

/* ---- emitters -------------------------------------------------------------- */
typedef struct { volatile long hits; } sink_t;

static int count_emit(void *ud, int pat, size_t end)
{
    sink_t *s = (sink_t *)ud;
    (void)pat; (void)end;
    s->hits++;
    return 0;
}

/* diff-mode recorder: FNV over the (pat,end) stream */
static int rec_emit(void *ud, int pat, size_t end)
{
    uint64_t *h = (uint64_t *)ud;
    *h ^= (uint64_t)(uint32_t)pat * 0x9E3779B97F4A7C15ull;
    *h = (*h << 27) | (*h >> 37);
    *h ^= (uint64_t)end;
    *h *= 0xBF58476D1CE4E5B9ull;
    return 0;
}

/* ---- build + report -------------------------------------------------------- */
/* the automaton's own accounting, per materialization (the struct is part
 * of the public header, so the bench can see what the engine holds) */
static size_t steady_bytes_states(const dyn_ac_t *a)
{
    if (a->compact)
        return a->n_states * (sizeof(uint32_t) + sizeof(int32_t)
                              + sizeof(dyn_ac_oo))
             + a->tab_cap * sizeof(dyn_ac_slot)
             + a->n_pat * sizeof(size_t)
             + sizeof(*a);
    return a->n_states * (DYN_AC_ALPHABET * sizeof(int32_t)
                          + 3 * sizeof(int32_t))
         + a->n_pat * sizeof(size_t)
         + sizeof(*a);
}

static dyn_ac_t *build_old(const patset_t *ps, double *build_ms)
{
    double t0 = now_ms();
    dyn_ac_t *a = dyn_ac_new((size_t)ps->n);
    if (!a) { printf("#AC ERROR alloc\n"); exit(1); }
    for (int i = 0; i < ps->n; i++)
        if (dyn_ac_insert(a, ps->buf + ps->off[i], ps->len[i], i) < 0) {
            printf("#AC ERROR insert refused\n");
            exit(1);
        }
    if (dyn_ac_build(a) < 0) { printf("#AC ERROR build\n"); exit(1); }
    *build_ms = now_ms() - t0;
    return a;
}

/* scan one text, calibrated so the timed region clears the clock; three
 * passes, best kept (re-run rule: no delta believed off a single pass) */
static void scan_row(const char *impl, const void *a, const char *set,
                     const char *cls, const uint8_t *t, size_t n,
                     int is_cand)
{
    sink_t s = { 0 };
    double best = 1e18;
    long hits = 0;
    int pass;
    for (pass = 0; pass < 3; pass++) {
        int reps = 1;
        for (;;) {
            double ta = now_ms();
            int k;
            double dt;
            for (k = 0; k < reps; k++) {
                s.hits = 0;
                if (is_cand) {
#ifdef HAVE_AC_CAND
                    dyn_da_run((const dyn_ac_cand_t *)a, t, n, count_emit, &s);
#endif
                } else {
                    dyn_ac_run((const dyn_ac_t *)a, t, n, count_emit, &s);
                }
            }
            dt = now_ms() - ta;
            if (dt >= 200.0 || reps >= (1 << 22)) {
                if (dt / (double)reps < best) {
                    best = dt / (double)reps;
                    hits = s.hits;
                }
                break;
            }
            reps *= 2;
        }
    }
    { uint32_t th = 2166136261u; for (size_t q = 0; q < n; q++) { th ^= t[q]; th *= 16777619u; }
    printf("#AC impl=%s set=%s text=%s n=%zu scan_ms=%.3f ns_byte=%.4f "
           "MBs=%.1f hits=%ld texthash=%08x\n",
           impl, set, cls, n, best, best * 1e6 / (double)n,
           (double)n / best / 1000.0, hits, th); }
}

static void run_set(const char *impl, const patset_t *ps, const char *name,
                    int is_cand)
{
    double bms;
    long rb = rss_kib(), ra;
    static uint8_t *tmany, *tnone, *tnear;
    if (!tmany) {
        tmany = malloc(TEXT_N); tnone = malloc(TEXT_N); tnear = malloc(TEXT_N);
    }
    uint8_t sep[16];
    int ns = disjoint_alphabet(ps, sep, 16);
    if (!ns) { sep[0] = 0x80; ns = 1; }

    if (!is_cand) {
        dyn_ac_t *a = build_old(ps, &bms);
        ra = rss_kib();
        printf("#AC impl=%s set=%s build_ms=%.3f states=%zu bytes_state=%.1f "
               "steady_kib=%.1f rss_delta_kib=%ld ascii=%d\n",
               impl, name, bms, a->n_states,
               (double)steady_bytes_states(a) / (double)a->n_states,
               (double)steady_bytes_states(a) / 1024.0,
               ra - rb, a->ascii);
        sm_state = 0xABCDEFA987654321ull;   /* identical texts for both impls */
        text_many(ps, tmany, TEXT_N, sep, ns);
        text_none(ps, tnone, TEXT_N);
        text_near(ps, tnear, TEXT_N);
        scan_row(impl, a, name, "many", tmany, TEXT_N, 0);
        scan_row(impl, a, name, "none", tnone, TEXT_N, 0);
        scan_row(impl, a, name, "near", tnear, TEXT_N, 0);
        dyn_ac_free(a);
    }
#ifdef HAVE_AC_CAND
    else {
        double t0 = now_ms();
        dyn_ac_cand_t *c = dyn_da_new((size_t)ps->n);
        if (!c) { printf("#AC ERROR cand alloc\n"); exit(1); }
        for (int i = 0; i < ps->n; i++)
            if (dyn_da_insert(c, ps->buf + ps->off[i], ps->len[i], i) < 0) {
                printf("#AC ERROR cand insert refused\n");
                exit(1);
            }
        if (dyn_da_build(c) < 0) { printf("#AC ERROR cand build\n"); exit(1); }
        bms = now_ms() - t0;
        ra = rss_kib();
        size_t st = dyn_da_states(c), bytes = dyn_da_bytes(c);
        printf("#AC impl=%s set=%s build_ms=%.3f states=%zu bytes_state=%.2f "
               "steady_kib=%.1f rss_delta_kib=%ld\n",
               impl, name, bms, st, (double)bytes / (double)st,
               (double)bytes / 1024.0, ra - rb);
        sm_state = 0xABCDEFA987654321ull;   /* identical texts for both impls */
        text_many(ps, tmany, TEXT_N, sep, ns);
        text_none(ps, tnone, TEXT_N);
        text_near(ps, tnear, TEXT_N);
        scan_row(impl, c, name, "many", tmany, TEXT_N, 1);
        scan_row(impl, c, name, "none", tnone, TEXT_N, 1);
        scan_row(impl, c, name, "near", tnear, TEXT_N, 1);
        dyn_da_free(c);
    }
#endif
}

/* ---- --shape: input-shape stats that drive the design constants -----------
 *
 * Implementation-independent: the trie is rebuilt harness-side so the stats
 * describe the PATTERN SET, not the engine's current internals. fill and
 * childless_pct are what the E10-11 redesign constants were derived from
 * (dense rows are ~99.6% redundant; row-dedup of densified rows was measured
 * dead on arrival at 85-90% distinct rows on the realistic sets). */
typedef struct {
    int32_t head;               /* first child edge */
} sh_node;

typedef struct {
    sh_node *nodes;
    size_t n, cap;
    uint8_t *e_byte;
    int32_t *e_target, *e_next;
    size_t n_edges, e_cap;
} sh_trie;

static void sh_add_edge(sh_trie *t, uint8_t b, int32_t target, int32_t next)
{
    if (t->n_edges == t->e_cap) {
        t->e_cap = t->e_cap ? t->e_cap * 2 : 32;
        t->e_byte = (uint8_t *)realloc(t->e_byte, t->e_cap);
        t->e_target = (int32_t *)realloc(t->e_target, t->e_cap * sizeof(int32_t));
        t->e_next = (int32_t *)realloc(t->e_next, t->e_cap * sizeof(int32_t));
    }
    t->e_byte[t->n_edges] = b;
    t->e_target[t->n_edges] = target;
    t->e_next[t->n_edges] = next;
    t->n_edges++;
}

static int32_t sh_new_node(sh_trie *t)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->nodes = (sh_node *)realloc(t->nodes, t->cap * sizeof(sh_node));
    }
    t->nodes[t->n].head = -1;
    return (int32_t)t->n++;
}

static void shape_set(const patset_t *ps, const char *name)
{
    double bms;
    sh_trie t = { 0 };
    size_t edges, childful = 0;
    long rb = rss_kib(), ra;

    sh_new_node(&t);                            /* root */
    for (int i = 0; i < ps->n; i++) {
        int32_t s = 0;
        const uint8_t *p = ps->buf + ps->off[i];
        for (size_t k = 0; k < ps->len[i]; k++) {
            int32_t e = t.nodes[s].head;
            while (e >= 0 && t.e_byte[e] != p[k])
                e = t.e_next[e];
            if (e < 0) {
                int32_t ns = sh_new_node(&t);
                sh_add_edge(&t, p[k], ns, t.nodes[s].head);
                t.nodes[s].head = t.n_edges - 1;
                s = ns;
            } else {
                s = t.e_target[e];
            }
        }
    }
    edges = t.n_edges;
    for (size_t s = 0; s < t.n; s++)
        if (t.nodes[s].head >= 0) childful++;

    dyn_ac_t *a = build_old(ps, &bms);
    ra = rss_kib();
    size_t dense_mib = a->n_states * 1024 / (1024 * 1024);
    (void)rb;
    printf("#ACSHAPE set=%s states=%zu edges=%zu fill=%.4f childful=%zu "
           "childless_pct=%.1f bytes_state=%.1f dense_would_mib=%zu "
           "build_ms=%.3f rss_delta_kib=%ld compact=%u\n",
           name, a->n_states, edges,
           (double)edges / ((double)a->n_states * 256.0), childful,
           100.0 * (double)(a->n_states - childful) / (double)a->n_states,
           (double)steady_bytes_states(a) / (double)a->n_states,
           dense_mib, bms, ra - rb, a->compact);
    free(t.nodes); free(t.e_byte); free(t.e_target); free(t.e_next);
    dyn_ac_free(a);
}

/* ---- --cap: the counting construction at the DYN_AC_MAX_STATES boundary --- */
static void pat4(uint8_t *out, uint32_t i)
{
    out[0] = 0xA5; out[1] = (uint8_t)(i >> 16);
    out[2] = (uint8_t)(i >> 8); out[3] = (uint8_t)i;
}
static size_t states4(size_t n)
{
    return n + (n + 255) / 256 + (n + 65535) / 65536 + 2;
}

static void cap_mode(void)
{
    size_t n_at = 0, n;
    for (n = 1; states4(n) <= (size_t)DYN_AC_MAX_STATES; n++) n_at = n;
    printf("#AC cap: n=%zu states=%zu cap=%d\n", n_at, states4(n_at),
           DYN_AC_MAX_STATES);

    long rss0 = rss_kib();
    dyn_ac_t *a = dyn_ac_new(n_at);
    if (!a) { printf("#AC ERROR alloc\n"); exit(1); }
    uint8_t p[4];
    double t0 = now_ms();
    for (n = 0; n < n_at; n++) {
        pat4(p, (uint32_t)n);
        if (dyn_ac_insert(a, p, 4, (int)n) < 0) {
            printf("#AC ERROR insert %zu refused\n", n);
            exit(1);
        }
    }
    double tins = now_ms() - t0;
    t0 = now_ms();
    if (dyn_ac_build(a) < 0) { printf("#AC ERROR build\n"); exit(1); }
    double tb = now_ms() - t0;
    long rss1 = rss_kib();
    printf("#AC impl=old set=cap build_insert_ms=%.1f build_fail_ms=%.1f "
           "states=%zu steady_mib=%.1f rss0_kib=%ld rss1_kib=%ld\n",
           tins, tb, a->n_states,
           (double)steady_bytes_states(a) / (1024.0 * 1024.0),
           rss0, rss1);

    size_t tn = TEXT_N;
    uint8_t *t = malloc(tn);
    for (size_t i = 0; i < tn; i += 4) pat4(t + i, (uint32_t)((i >> 2) & 0xFFFFFFu));
    scan_row("old", a, "cap", "many", t, tn, 0);
    for (size_t i = 0; i < tn; i++) t[i] = (uint8_t)(0x20 + (i & 0x1F));
    scan_row("old", a, "cap", "none", t, tn, 0);
    dyn_ac_free(a);

#ifdef HAVE_AC_CAND
    /* the same construction against the candidate: the cap is on STATES, so
     * the boundary pattern count is the same number for both structures */
    long crss0 = rss_kib();
    dyn_ac_cand_t *c = dyn_da_new(n_at);
    if (!c) { printf("#AC ERROR cand alloc\n"); exit(1); }
    t0 = now_ms();
    for (n = 0; n < n_at; n++) {
        pat4(p, (uint32_t)n);
        if (dyn_da_insert(c, p, 4, (int)n) < 0) {
            printf("#AC ERROR cand insert %zu refused\n", n);
            exit(1);
        }
    }
    tins = now_ms() - t0;
    t0 = now_ms();
    if (dyn_da_build(c) < 0) { printf("#AC ERROR cand build\n"); exit(1); }
    tb = now_ms() - t0;
    long crss1 = rss_kib();
    printf("#AC impl=cand set=cap build_insert_ms=%.1f build_fail_ms=%.1f "
           "states=%zu steady_mib=%.1f rss0_kib=%ld rss1_kib=%ld\n",
           tins, tb, dyn_da_states(c),
           (double)dyn_da_bytes(c) / (1024.0 * 1024.0), crss0, crss1);
    for (size_t i = 0; i < tn; i += 4) pat4(t + i, (uint32_t)((i >> 2) & 0xFFFFFFu));
    scan_row("cand", c, "cap", "many", t, tn, 1);
    for (size_t i = 0; i < tn; i++) t[i] = (uint8_t)(0x20 + (i & 0x1F));
    scan_row("cand", c, "cap", "none", t, tn, 1);
    dyn_da_free(c);
#endif
    free(t);
}

/* ---- --diff: old vs candidate over a systematic corpus -------------------- */
static void diff_mode(void)
{
#ifndef HAVE_AC_CAND
    printf("#AC diff: no candidate linked "
           "(build with -DHAVE_AC_CAND tests/ac_cand.c)\n");
    (void)rec_emit;
#else
    static const struct { patgen_t gen; const char *name; } sets[] = {
        { gen_s10, "S10" }, { gen_m1000, "M1000" }, { gen_x10000, "X10000" },
        { gen_a2, "A2" }, { gen_p1, "P1" }, { gen_l64, "L64" },
    };
    int bad = 0, cases = 0;
    for (size_t si = 0; si < sizeof sets / sizeof sets[0]; si++) {
        patset_t ps = { 0 };
        ps.off = malloc((NPAT_MAX + 9) * sizeof *ps.off);
        ps.len = malloc((size_t)(NPAT_MAX + 8) * sizeof *ps.len);
        ps.buf = malloc(4 << 20);
        ps.off[0] = 0;
        sm_state = 0x243F6A8885A308D3ull;           /* same sets as baseline */
        sets[si].gen(&ps);

        dyn_ac_t *o = dyn_ac_new((size_t)ps.n);
        for (int i = 0; i < ps.n; i++)
            dyn_ac_insert(o, ps.buf + ps.off[i], ps.len[i], i);
        dyn_ac_build(o);

        dyn_ac_cand_t *c = dyn_da_new((size_t)ps.n);
        for (int i = 0; i < ps.n; i++)
            dyn_da_insert(c, ps.buf + ps.off[i], ps.len[i], i);
        dyn_da_build(c);

        /* corpus per set: the three classes at 1 MiB + edge cases */
        size_t n = 1u << 20;
        uint8_t *t1 = malloc(n), *t2 = malloc(n), *t3 = malloc(n);
        uint8_t sep[16];
        int ns = disjoint_alphabet(&ps, sep, 16);
        if (!ns) { sep[0] = 0x80; ns = 1; }
        text_many(&ps, t1, n, sep, ns);
        text_none(&ps, t2, n);
        text_near(&ps, t3, n);
        const struct { const uint8_t *t; size_t l; const char *name; } corpus[] = {
            { t1, n, "many" }, { t2, n, "none" }, { t3, n, "near" },
            { t1, 0, "empty" },                       /* tlen=0 edge */
            { ps.buf + ps.off[0], ps.len[0], "exact-pattern" },
            { t2, 1, "one-byte" },
            { ps.buf, ps.off[ps.n], "all-patterns-concat" },
        };
        for (size_t ci = 0; ci < sizeof corpus / sizeof corpus[0]; ci++) {
            uint64_t ho = 1469598103934665603ull;
            uint64_t hc = 1469598103934665603ull;
            dyn_ac_run(o, corpus[ci].t, corpus[ci].l, rec_emit, &ho);
            dyn_da_run(c, corpus[ci].t, corpus[ci].l, rec_emit, &hc);
            cases++;
            if (ho != hc) {
                printf("#ACDIFF MISMATCH set=%s case=%s old=%016llx new=%016llx\n",
                       sets[si].name, corpus[ci].name,
                       (unsigned long long)ho, (unsigned long long)hc);
                bad++;
            }
        }
        free(t1); free(t2); free(t3);
        dyn_ac_free(o);
        dyn_da_free(c);
        patset_free(&ps);
    }
    printf("#ACDIFF cases=%d mismatches=%d %s\n", cases, bad,
           bad ? "FAIL" : "OK");
#endif
}

int main(int argc, char **argv)
{
    static const struct { patgen_t gen; const char *name; } sets[] = {
        { gen_s10, "S10" }, { gen_m1000, "M1000" }, { gen_x10000, "X10000" },
        { gen_a2, "A2" }, { gen_p1, "P1" }, { gen_l64, "L64" },
    };
    int shape = argc > 1 && !strcmp(argv[1], "--shape");
    int cap = argc > 1 && !strcmp(argv[1], "--cap");
    int diff = argc > 1 && !strcmp(argv[1], "--diff");

    if (diff) { diff_mode(); return 0; }
    if (cap) { cap_mode(); return 0; }

    for (size_t si = 0; si < sizeof sets / sizeof sets[0]; si++) {
        patset_t ps = { 0 };
        /* +8 slack: X10000's trailing prefix-cluster pushes one past NPAT_MAX */
        ps.off = malloc((NPAT_MAX + 9) * sizeof *ps.off);
        ps.len = malloc((size_t)(NPAT_MAX + 8) * sizeof *ps.len);
        ps.buf = malloc(4 << 20);                   /* X10000 worst ~ 200 KiB */
        ps.off[0] = 0;
        sm_state = 0x243F6A8885A308D3ull;           /* same sets every run */
        sets[si].gen(&ps);
        if (shape) shape_set(&ps, sets[si].name);
        else {
            run_set("old", &ps, sets[si].name, 0);
            run_set("cand", &ps, sets[si].name, 1);
        }
        patset_free(&ps);
    }
    return 0;
}
