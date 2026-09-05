/* bench_core.c -- one harness for every src/core TU: a timing side and a
 * differential side.
 *
 * WHY BOTH IN ONE FILE. CLAUDE.md's rule is that an optimisation is guilty
 * until a benchmark proves it innocent -- but a benchmark alone proves the
 * wrong thing, because the fastest way to compute something is to compute it
 * wrongly. So every case also folds its result into a hash. Build twice, once
 * before a change and once after, and the hash must be identical while the
 * times move. That is the shape that caught three real bugs in dyn-hash and
 * dyn-compress earlier, and the shape that showed the regexp fuzzer was not
 * reaching the code it claimed to cover.
 *
 * SIZES SPAN A FEW BYTES TO MEGABYTES on purpose. A kernel that wins at 1 MB
 * routinely loses at 16 bytes to its own setup, and reporting only the large
 * row is how a regression gets shipped.
 *
 *   ./bench_core           both sides
 *   ./bench_core --hash    differential only (fast, for A/B)
 *   ./bench_core --bench   timings only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "dyn-ds.h"
#include "dyn-codec.h"
#include "dyn-path.h"
#include "dyn-prng.h"
#include "dyn-ac.h"
#include "dyn-mathx.h"
#include "dyn-dict.h"
#include "dyn-hash.h"
#include "dyn-serial.h"
#include "dyna-simd-kernels.h"

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* FNV-1a over every observable. Order-dependent by construction. */
static uint32_t HASH = 2166136261u;
static void mix(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) { HASH ^= b[i]; HASH *= 16777619u; }
}
static void mix_u64(uint64_t v) { mix(&v, sizeof v); }

static int do_bench = 1, do_hash = 1;

/* A sink for timed results. mix() is eight rounds of hashing and belongs in the
   differential pass, never inside TIME -- putting it there made prng/next read
   10.64 ns when the generator itself is a fraction of that. Same defect class
   as formatting a key inside the timed region. */
static volatile uint64_t SINK;

/* Calibrate to ~60 ms so a 20 ns call and a 20 ms call are both reported to
 * the same relative precision; performance.now-style floors otherwise turn the
 * small rows into quantised garbage. */
#define TIME(label, size, body)                                             \
    do {                                                                    \
        if (do_bench) {                                                     \
            int reps_ = 1; double t_;                                       \
            for (;;) {                                                      \
                double t0_ = now_ms();                                      \
                for (int k_ = 0; k_ < reps_; k_++) { body; }                \
                t_ = now_ms() - t0_;                                        \
                if (t_ >= 60.0 || reps_ >= (1 << 24)) break;                \
                reps_ *= 2;                                                 \
            }                                                               \
            printf("#C %-26s %9zu %12.2f ns\n", (label), (size_t)(size),    \
                   (t_ / reps_) * 1e6);                                     \
        }                                                                   \
    } while (0)

static void bench_mathx(void);
static void bench_dict(void);
static void bench_hash(void);

static const size_t SIZES[] = { 8, 64, 1024, 65536, 1048576 };
#define NSIZES (sizeof SIZES / sizeof SIZES[0])

static uint8_t *mkbuf(size_t n, unsigned seed)
{
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    size_t i;
    for (i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 24); }
    return b;
}

/* ---- dyn-codec ---------------------------------------------------------- */
static void bench_codec(void)
{
    size_t si;
    for (si = 0; si < NSIZES; si++) {
        size_t n = SIZES[si];
        uint8_t *in = mkbuf(n, 1234u + (unsigned)n);
        char *enc = (char *)malloc(n * 3 + 16);
        uint8_t *dec = (uint8_t *)malloc(n + 16);
        size_t el;

        dyn_codec_hex_encode(in, n, enc);
        if (do_hash) { mix(enc, n * 2); mix_u64(dyn_codec_hex_decode(enc, n * 2, dec)); mix(dec, n); }
        TIME("codec/hex_encode", n, dyn_codec_hex_encode(in, n, enc));
        TIME("codec/hex_decode", n, dyn_codec_hex_decode(enc, n * 2, dec));

        el = dyn_codec_base64_encode(in, n, enc);
        if (do_hash) { mix(enc, el); mix_u64(dyn_codec_base64_decode(enc, el, dec)); mix(dec, n); }
        TIME("codec/base64_encode", n, dyn_codec_base64_encode(in, n, enc));
        TIME("codec/base64_decode", n, dyn_codec_base64_decode(enc, el, dec));

        el = dyn_codec_base32_encode(in, n, enc, DYN_BASE32_STD);
        if (do_hash) { mix(enc, el); mix_u64(dyn_codec_base32_decode(enc, el, dec, DYN_BASE32_STD)); mix(dec, n); }
        TIME("codec/base32_encode", n, dyn_codec_base32_encode(in, n, enc, DYN_BASE32_STD));
        TIME("codec/base32_decode", n, dyn_codec_base32_decode(enc, el, dec, DYN_BASE32_STD));

        free(in); free(enc); free(dec);
    }
}

/* ---- dyn-prng ----------------------------------------------------------- */
static void bench_prng(void)
{
    dyn_prng_t r;
    size_t si;
    dyn_prng_seed(&r, 0x5eed1234u);
    if (do_hash) {
        int i;
        for (i = 0; i < 4096; i++) mix_u64(dyn_prng_next(&r));
        for (i = 0; i < 512; i++) { double d = dyn_prng_next_double(&r); mix(&d, sizeof d); }
        for (i = 1; i <= 512; i++) mix_u64(dyn_prng_next_bounded(&r, (uint64_t)i * 7919u));
    }
    TIME("prng/next", 8, SINK = dyn_prng_next(&r));
    TIME("prng/next_double", 8, SINK = (uint64_t)(dyn_prng_next_double(&r) * 4294967296.0));
    TIME("prng/bounded", 8, SINK = dyn_prng_next_bounded(&r, 1000003u));
    for (si = 0; si < NSIZES; si++) {
        size_t n = SIZES[si];
        uint8_t *b = (uint8_t *)malloc(n);
        dyn_prng_t r2; dyn_prng_seed(&r2, 99u);
        dyn_prng_fill(&r2, b, n);
        if (do_hash) mix(b, n);
        TIME("prng/fill", n, dyn_prng_fill(&r, b, n));
        free(b);
    }
}

/* ---- dyn-path ----------------------------------------------------------- */
static const char *const PATHS[] = {
    "/", "a", "./a/b", "/usr/local/bin/../lib/./x.so",
    "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p.txt",
    "/very/deeply/nested/path/with/many/components/and/../../dots/./here/file.tar.gz",
    "relative/../../escape/../../deep", "//double//slashes//everywhere//x"
};
#define NPATHS (sizeof PATHS / sizeof PATHS[0])

static void bench_path(void)
{
    char out[1024];
    size_t i;
    for (i = 0; i < NPATHS; i++) {
        size_t n = strlen(PATHS[i]);
        dyn_path_split_t sp;
        size_t r = dyn_path_normalize(PATHS[i], n, out);
        if (do_hash) {
            mix(out, r); mix_u64(r);
            dyn_path_split(PATHS[i], n, &sp);
            mix(&sp, sizeof sp);
        }
    }
    {
        const char *p = PATHS[5];
        size_t n = strlen(p);
        dyn_path_split_t sp;
        TIME("path/normalize", n, dyn_path_normalize(p, n, out));
        TIME("path/split", n, dyn_path_split(p, n, &sp));
    }
}

/* ---- dyn-ds ------------------------------------------------------------- */
static void bench_ds(void)
{
    static const size_t NKEYS[] = { 8, 1024, 65536 };
    size_t ki;
    for (ki = 0; ki < sizeof NKEYS / sizeof NKEYS[0]; ki++) {
        size_t nk = NKEYS[ki], i;
        dyn_mset_t *m = dyn_mset_new();
        char key[32];
        for (i = 0; i < nk; i++) {
            int kl = snprintf(key, sizeof key, "k%zu", i);
            dyn_mset_add(m, key, (size_t)kl, (int64_t)(i % 7) + 1, NULL);
        }
        if (do_hash) {
            mix_u64(dyn_mset_total(m)); mix_u64(dyn_mset_distinct(m));
            for (i = 0; i < nk; i++) {
                int kl = snprintf(key, sizeof key, "k%zu", i);
                mix_u64(dyn_mset_count(m, key, (size_t)kl));
            }
        }
        /* Keys are built ONCE, outside the timed region. Formatting them
           inside it measures snprintf -- which costs more than the hash probe
           it surrounds -- and makes every container look the same speed. */
        {
            char *kbuf = (char *)malloc(nk * 24);
            size_t *klen = (size_t *)malloc(nk * sizeof *klen);
            for (i = 0; i < nk; i++)
                klen[i] = (size_t)snprintf(kbuf + i * 24, 24, "k%zu", i);
            TIME("ds/mset_add", nk, {
                size_t j_ = (size_t)k_ % nk;
                dyn_mset_add(m, kbuf + j_ * 24, klen[j_], 1, NULL);
            });
            TIME("ds/mset_count", nk, {
                size_t j_ = (size_t)k_ % nk;
                SINK = dyn_mset_count(m, kbuf + j_ * 24, klen[j_]);
            });
            free(kbuf); free(klen);
        }
        dyn_mset_free(m);
    }
    {
        dyn_hll_t *h = dyn_hll_new(14);
        char key[32];
        size_t i;
        for (i = 0; i < 100000; i++) {
            int kl = snprintf(key, sizeof key, "u%zu", i);
            dyn_hll_add(h, key, (size_t)kl);
        }
        if (do_hash) { double e = dyn_hll_count(h); mix(&e, sizeof e); }
        {
            char *kbuf = (char *)malloc(4096 * 24);
            size_t *klen = (size_t *)malloc(4096 * sizeof *klen);
            for (i = 0; i < 4096; i++)
                klen[i] = (size_t)snprintf(kbuf + i * 24, 24, "u%zu", i);
            TIME("ds/hll_add", 100000, {
                size_t j_ = (size_t)k_ & 4095u;
                dyn_hll_add(h, kbuf + j_ * 24, klen[j_]);
            });
            free(kbuf); free(klen);
        }
        dyn_hll_free(h);
    }
}

/* ---- dyn-ac ------------------------------------------------------------- */
static int ac_count(void *ud, int pat, size_t end) { (void)pat; (void)end; (*(long *)ud)++; return 0; }

static void bench_ac(void)
{
    static const int NPAT[] = { 1, 8, 64, 512 };
    size_t pi;
    size_t tn = 1 << 20;
    uint8_t *text = mkbuf(tn, 77u);
    for (pi = 0; pi < sizeof NPAT / sizeof NPAT[0]; pi++) {
        int np = NPAT[pi], i;
        dyn_ac_t *a = dyn_ac_new((size_t)np);
        char pat[16];
        long hits = 0;
        for (i = 0; i < np; i++) {
            int pl = snprintf(pat, sizeof pat, "%c%c%c", 'a' + (i % 26), 'A' + (i % 26), '0' + (i % 10));
            dyn_ac_insert(a, (const uint8_t *)pat, (size_t)pl, i);
        }
        dyn_ac_build(a);
        dyn_ac_run(a, text, tn, ac_count, &hits);
        if (do_hash) mix_u64((uint64_t)hits);
        TIME("ac/run", (size_t)np, { long h_ = 0; dyn_ac_run(a, text, tn, ac_count, &h_); });
        dyn_ac_free(a);
    }
    free(text);
}

/* ---- dyn-serial --------------------------------------------------------- */
static void bench_serial(void)
{
    size_t si;
    for (si = 0; si < NSIZES; si++) {
        size_t n = SIZES[si], i;
        dyn_ser_t w;
        uint8_t *raw = mkbuf(n, 5u);
        dyn_ser_init(&w);
        for (i = 0; i < n / 8; i++) { dyn_ser_u8(&w, (uint8_t)i); dyn_ser_u32(&w, (uint32_t)i); }
        dyn_ser_raw(&w, raw, n);
        if (do_hash) { size_t l = 0; uint8_t *b = dyn_ser_take(&w, &l); mix(b, l); mix_u64(l); free(b); }
        else dyn_ser_free(&w);
        TIME("serial/u32_stream", n, {
            dyn_ser_t w_; dyn_ser_init(&w_);
            for (size_t j_ = 0; j_ < n / 8; j_++) dyn_ser_u32(&w_, (uint32_t)j_);
            dyn_ser_free(&w_);
        });
        free(raw);
    }
}

static void run_all(void)
{
    bench_codec();
    bench_prng();
    bench_path();
    bench_ds();
    bench_ac();
    bench_serial();
    bench_mathx();
    bench_dict();
    bench_hash();
}

/* ---- dyn-mathx ----------------------------------------------------------
 *
 * These are iterative special functions -- continued fractions and series --
 * so the cost is a function of the ARGUMENT, not of a length. A single
 * representative x would measure one branch of the implementation and call it
 * the speed of the function: dyn_gammainc_p switches between a series and a
 * continued fraction at x < a+1, and the two differ by more than 3x. So each
 * row names its regime, and the sweep covers both sides of every switch. */
static void bench_mathx(void)
{
    static const double XS[] = { 0.5, 2.0, 8.0, 30.0, 120.0 };
    size_t i;
    volatile double sink = 0;

    for (i = 0; i < sizeof XS / sizeof XS[0]; i++)
        sink += dyn_gammainc_p(3.0, XS[i]) + dyn_besselj(0, XS[i]) +
                dyn_digamma(XS[i] + 0.25) + dyn_ellipk(1.0 / (XS[i] + 2.0));
    if (do_hash) {
        double v = sink;
        mix(&v, sizeof v);
        /* Pin the VALUES, not just that it ran: a rewrite of a series is
           exactly the change that keeps the shape and moves the answer. */
        for (i = 0; i < sizeof XS / sizeof XS[0]; i++) {
            double a[6];
            a[0] = dyn_gammainc_p(3.0, XS[i]);
            a[1] = dyn_gammainc_q(3.0, XS[i]);
            a[2] = dyn_besselj(1, XS[i]);
            a[3] = dyn_bessely(0, XS[i]);
            a[4] = dyn_digamma(XS[i] + 0.25);
            a[5] = dyn_betainc(2.0, 3.0, 1.0 / (XS[i] + 2.0));
            mix(a, sizeof a);
        }
    }
    /* series regime (x < a+1) and continued-fraction regime (x >= a+1) */
    TIME("mathx/gammainc_p_series", 1, sink += dyn_gammainc_p(10.0, 2.0));
    TIME("mathx/gammainc_p_cf",     1, sink += dyn_gammainc_p(2.0, 30.0));
    TIME("mathx/besselj_small",     1, sink += dyn_besselj(0, 1.5));
    TIME("mathx/besselj_large",     1, sink += dyn_besselj(0, 120.0));
    TIME("mathx/bessely_small",     1, sink += dyn_bessely(0, 1.5));
    TIME("mathx/digamma",           1, sink += dyn_digamma(3.25));
    TIME("mathx/ellipk",            1, sink += dyn_ellipk(0.5));
    TIME("mathx/betainc",           1, sink += dyn_betainc(2.0, 3.0, 0.4));
    TIME("mathx/gammaincinv_a_gt1", 1, sink += dyn_gammaincinv(3.0, 0.5));
    /* a <= 1 takes the other branch, which is where the lgamma hoist lands. */
    TIME("mathx/gammaincinv_a_le1", 1, sink += dyn_gammaincinv(0.5, 0.5));
}

/* ---- dyn-dict -----------------------------------------------------------
 *
 * A trained dictionary pays off exactly when the payload is SHORT enough that
 * it is mostly boilerplate -- which is why the sizes here start at 91 bytes
 * and the large row is kept even though it wins nothing (CLAUDE.md sec.12:
 * publish the losing row next to the winning one). */
static void bench_dict(void)
{
    static const char *const PH[] = {
        "\"timestamp\":", "\"level\":", "\"message\":", "\"service\":",
        "\"trace_id\":", "\"span_id\":", "\"http.status\":", "\"duration_ms\":",
    };
    static const size_t NPH = sizeof PH / sizeof PH[0];
    const uint8_t *ph[8];
    size_t lens[8], i;
    dyn_dict_t *d;
    static const char REC[] =
        "{\"timestamp\":\"2026-07-28T12:00:00Z\",\"level\":\"info\","
        "\"service\":\"api\",\"message\":\"ok\",\"trace_id\":\"abc123\"}";
    const size_t RN = sizeof REC - 1;
    uint8_t *big;
    size_t bign = 65536;

    for (i = 0; i < NPH; i++) { ph[i] = (const uint8_t *)PH[i]; lens[i] = strlen(PH[i]); }
    d = dyn_dict_new(ph, lens, NPH);
    if (!d)
        return;
    big = (uint8_t *)malloc(bign);
    if (!big) { dyn_dict_free(d); return; }
    for (i = 0; i < bign; i++) big[i] = (uint8_t)REC[i % RN];

    if (do_hash) {
        dyn_outbuf_t o = { NULL, 0, 0 }, r = { NULL, 0, 0 };
        if (dyn_dict_compress(d, (const uint8_t *)REC, RN, &o) == 0) {
            mix(o.buf, o.len); mix_u64(o.len);
            /* Round-trip, and check it against the SOURCE -- a round-trip both
               sides of one build agree on detects nothing on its own. */
            if (dyn_dict_decompress(d, o.buf, o.len, &r) == 0) {
                mix_u64(r.len);
                mix_u64(r.len == RN && memcmp(r.buf, REC, RN) == 0);
            }
        }
        free(o.buf); free(r.buf);
        mix_u64(dyn_dict_count(d)); mix_u64(dyn_dict_id(d));
    }
    {
        dyn_outbuf_t o = { NULL, 0, 0 };
        TIME("dict/compress", RN, {
            o.len = 0; dyn_dict_compress(d, (const uint8_t *)REC, RN, &o);
        });
        TIME("dict/compress", bign, {
            o.len = 0; dyn_dict_compress(d, big, bign, &o);
        });
        o.len = 0;
        if (dyn_dict_compress(d, (const uint8_t *)REC, RN, &o) == 0) {
            dyn_outbuf_t r = { NULL, 0, 0 };
            TIME("dict/decompress", RN, { r.len = 0; dyn_dict_decompress(d, o.buf, o.len, &r); });
            free(r.buf);
        }
        free(o.buf);
    }
    free(big);
    dyn_dict_free(d);
}

/* ---- dyn-hash -----------------------------------------------------------
 *
 * The short rows are not decoration. dyn-ds hashes 2-7 byte keys on every
 * lookup, where the per-call setup and finalisation -- not the per-byte mix --
 * is the whole cost: measured at 3.79 ns, which is 39% of a whole
 * ds/mset_count. A hash bench that starts at 1 KB cannot see that. */
static void bench_hash(void)
{
    static const size_t NS[] = { 4, 8, 12, 16, 32, 64, 1024, 65536 };
    uint8_t *b = (uint8_t *)malloc(65536);
    size_t i, j;
    if (!b)
        return;
    for (i = 0; i < 65536; i++) b[i] = (uint8_t)(i * 31u + 7u);

    if (do_hash)
        for (i = 0; i < sizeof NS / sizeof NS[0]; i++) {
            uint8_t dg[32];
            mix_u64(dyn_xxh64(b, NS[i], 0));
            mix_u64(dyn_xxh32(b, NS[i], 0));
            mix_u64(dyn_crc32(b, NS[i]));
            mix_u64(dyn_crc32c(b, NS[i]));
            dyn_sha256(b, NS[i], dg); mix(dg, 32);
            dyn_md5(b, NS[i], dg);    mix(dg, 16);
        }
    for (j = 0; j < sizeof NS / sizeof NS[0]; j++) {
        size_t n = NS[j];
        uint8_t dg[32];
        TIME("hash/xxh64", n, SINK = dyn_xxh64(b, n, 0));
        TIME("hash/crc32c", n, SINK = dyn_crc32c(b, n));
        TIME("hash/crc32", n, SINK = dyn_crc32(b, n));
        TIME("hash/sha256", n, dyn_sha256(b, n, dg));
    }
    free(b);
}

int main(int argc, char **argv)
{
    int i;
    /* The core TUs call simd.* directly and none of them initialises the
       dispatch table -- that is the embedder's job. Omitting this calls
       through a null pointer, which on arm64 hangs rather than faulting. */
    simd_init();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hash")) do_bench = 0;
        else if (!strcmp(argv[i], "--bench")) do_hash = 0;
    }
    /* Two SEPARATE passes, never interleaved. The timing loops mutate state --
       the multiset grows, the PRNG advances -- so a hash accumulated alongside
       them depends on whether benchmarking ran, and a differential whose value
       depends on the flags is not a differential. Measured: the same tree gave
       1792170927 with --hash and 101965593 with both. */
    if (do_hash) {
        int saved = do_bench;
        do_bench = 0;
        run_all();
        printf("#H core_differential %u\n", HASH);
        do_bench = saved;
    }
    if (do_bench) {
        int saved = do_hash;
        do_hash = 0;
        run_all();
        do_hash = saved;
    }
    return 0;
}
