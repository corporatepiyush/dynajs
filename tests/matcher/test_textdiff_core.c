/*
 * test_textdiff_core.c -- src/core/textdiff (Myers diff, unified-diff writer
 * and patch applier behind dyna:matcher Diff*, UnifiedDiff, ApplyPatch) as a
 * standalone C library.
 *
 * Oracles: the hunk script must rebuild both inputs (EQ+DEL tokens give a,
 * EQ+INS give b) and keep as many tokens equal as an O(n*m) LCS table, in all
 * three token modes; dyn_td_apply(a, dyn_td_unified(a, b)) must give back b
 * for seeded random line texts, with and without trailing newlines and at
 * context 0..3. Fixed cases pin the exact unified text (two equal lines
 * between changes at context 1 join one hunk: a gap splits only above 2*context), the error codes for a
 * malformed header, a second file, a conflicting context line and a
 * truncated hunk, and the token cap. Lopsided pairs (65536 chars vs 3..5) go
 * through the LCS table path, which must give a minimal script too (the
 * search it replaces needs 2*ceil(D/2)^2 steps and could not finish).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-textdiff.h"
#include "../lib/dyn-xalloc.h"

static int fails, checks;

static void tput(dyn_td_buf_t* b, const uint8_t* p, size_t n)
{
    if (b->size + n > b->cap) {
        size_t nc = (b->size + n) * 2 + 16;
        b->buf = xrealloc(b->buf, nc);
        b->cap = nc;
    }
    memcpy(b->buf + b->size, p, n);
    b->size += n;
}

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(uint32_t k)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng % k);
}

static size_t gen(char* out, int lines, int nl_end, int mode)
{
    static const char* const W[] = { "alpha", "beta", "gamma", "delta", "x", "" };
    size_t n = 0;
    int i;
    for (i = 0; i < lines; i++) {
        const char* w = W[rnd(6)];
        if (mode == DYN_TD_CHARS) {
            out[n++] = (char)('a' + rnd(4));
            continue;
        }
        memcpy(out + n, w, strlen(w));
        n += strlen(w);
        if (mode == DYN_TD_WORDS)
            out[n++] = rnd(3) ? ' ' : '\n';
        else if (i + 1 < lines || nl_end)
            out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

static int tok_eq(const uint8_t* a, const dyn_td_toks_t* ta, uint32_t i,
    const uint8_t* b, const dyn_td_toks_t* tb, uint32_t j)
{
    return ta->len[i] == tb->len[j] && memcmp(a + ta->off[i], b + tb->off[j], ta->len[i]) == 0;
}

static uint32_t lcs(const uint8_t* a, const dyn_td_toks_t* ta, const uint8_t* b, const dyn_td_toks_t* tb)
{
    uint32_t i, j, w = tb->n + 1, r;
    uint32_t* t = xcalloc((size_t)(ta->n + 1) * w, sizeof *t);
    for (i = 1; i <= ta->n; i++)
        for (j = 1; j <= tb->n; j++)
            t[i * w + j] = tok_eq(a, ta, i - 1, b, tb, j - 1) ? t[(i - 1) * w + j - 1] + 1
                : (t[(i - 1) * w + j] > t[i * w + j - 1] ? t[(i - 1) * w + j] : t[i * w + j - 1]);
    r = t[ta->n * w + tb->n];
    free(t);
    return r;
}

static void check_script(const char* a, size_t na, const char* b, size_t nb, int mode)
{
    dyn_td_res_t r;
    dyn_td_buf_t ra, rb;
    uint32_t i, k, eq = 0;
    int rc = dyn_td_diff((const uint8_t*)a, (uint32_t)na, (const uint8_t*)b, (uint32_t)nb, mode, &r);
    CHECK(rc == DYN_TD_OK, "diff rc %d", rc);
    if (rc)
        return;
    memset(&ra, 0, sizeof ra);
    memset(&rb, 0, sizeof rb);
    for (i = 0; i < r.sc.n; i++) {
        const dyn_td_hunk_t* h = &r.sc.v[i];
        if (h->op != DYN_TD_INS)
            for (k = h->a0; k < h->a1; k++)
                tput(&ra, (const uint8_t*)a + r.ta.off[k], r.ta.len[k]);
        if (h->op != DYN_TD_DEL)
            for (k = h->b0; k < h->b1; k++)
                tput(&rb, (const uint8_t*)b + r.tb.off[k], r.tb.len[k]);
        if (h->op == DYN_TD_EQ) {
            CHECK(h->a1 - h->a0 == h->b1 - h->b0, "EQ hunk spans differ");
            for (k = 0; k < h->a1 - h->a0; k++)
                CHECK(tok_eq((const uint8_t*)a, &r.ta, h->a0 + k, (const uint8_t*)b, &r.tb, h->b0 + k), "EQ token differs");
            eq += h->a1 - h->a0;
        }
    }
    CHECK(ra.size == na && (!na || memcmp(ra.buf, a, na) == 0), "script does not rebuild a (mode %d)", mode);
    CHECK(rb.size == nb && (!nb || memcmp(rb.buf, b, nb) == 0), "script does not rebuild b (mode %d)", mode);
    CHECK(eq == lcs((const uint8_t*)a, &r.ta, (const uint8_t*)b, &r.tb), "not minimal: %u equal tokens, LCS %u (mode %d)",
        eq, lcs((const uint8_t*)a, &r.ta, (const uint8_t*)b, &r.tb), mode);
    dyn_td_buf_free(&ra);
    dyn_td_buf_free(&rb);
    dyn_td_res_free(&r);
}

static int unified(const char* a, const char* b, uint32_t ctxl, dyn_td_buf_t* o)
{
    dyn_td_res_t r;
    int rc = dyn_td_diff((const uint8_t*)a, (uint32_t)strlen(a), (const uint8_t*)b, (uint32_t)strlen(b), DYN_TD_LINES, &r);
    if (rc)
        return rc;
    rc = dyn_td_unified((const uint8_t*)a, (const uint8_t*)b, &r, ctxl, "a", "b", o);
    dyn_td_res_free(&r);
    return rc;
}

static int apply(const char* t, const char* p, dyn_td_buf_t* o, char* msg)
{
    msg[0] = 0;
    return dyn_td_apply((const uint8_t*)t, (uint32_t)strlen(t), (const uint8_t*)p, strlen(p), o, msg);
}

static void test_random(void)
{
    static char a[4096], b[4096];
    int it, mode;
    char msg[DYN_TD_MSG_CAP];
    for (mode = DYN_TD_LINES; mode <= DYN_TD_CHARS; mode++)
        for (it = 0; it < 150; it++) {
            size_t na = gen(a, (int)rnd(40), (int)rnd(2), mode), nb = gen(b, (int)rnd(40), (int)rnd(2), mode);
            check_script(a, na, b, nb, mode);
        }
    for (it = 0; it < 400; it++) {
        dyn_td_buf_t u, o;
        uint32_t ctxl = rnd(4);
        int rc;
        gen(a, (int)rnd(30), (int)rnd(2), DYN_TD_LINES);
        gen(b, (int)rnd(30), (int)rnd(2), DYN_TD_LINES);
        memset(&u, 0, sizeof u);
        memset(&o, 0, sizeof o);
        rc = unified(a, b, ctxl, &u);
        CHECK(rc == DYN_TD_OK, "unified rc %d", rc);
        tput(&u, (const uint8_t*)"", 1);
        rc = apply(a, (const char*)u.buf, &o, msg);
        CHECK(rc == DYN_TD_OK, "apply rc %d (%s) context %u", rc, msg, ctxl);
        CHECK(rc || (o.size == strlen(b) && memcmp(o.buf, b, o.size) == 0), "apply(a, unified(a, b)) != b at context %u", ctxl);
        dyn_td_buf_free(&u);
        dyn_td_buf_free(&o);
    }
}

static void test_fixed(void)
{
    dyn_td_buf_t o;
    char msg[DYN_TD_MSG_CAP];
    int rc;
    const char* want = "--- a\n+++ b\n@@ -1,5 +1,5 @@\n one\n-two\n+TWO\n three\n four\n-five\n\\ No newline at end of file\n+5\n\\ No newline at end of file\n";
    memset(&o, 0, sizeof o);
    rc = unified("one\ntwo\nthree\nfour\nfive", "one\nTWO\nthree\nfour\n5", 1, &o);
    CHECK(rc == DYN_TD_OK && o.size == strlen(want) && memcmp(o.buf, want, o.size) == 0, "unified text: %.*s", (int)o.size, (const char*)o.buf);
    dyn_td_buf_free(&o);

    memset(&o, 0, sizeof o);
    rc = apply("a\nb\n", "@@ -x @@\n", &o, msg);
    CHECK(rc == DYN_TD_ESYNTAX && strstr(msg, "malformed hunk header"), "malformed header: %d %s", rc, msg);
    dyn_td_buf_free(&o);

    memset(&o, 0, sizeof o);
    rc = apply("a\n", "--- a\n+++ b\n@@ -1 +1 @@\n-a\n+b\n--- c\n+++ d\n", &o, msg);
    CHECK(rc == DYN_TD_ESYNTAX && strstr(msg, "second file; apply one"), "second file: %d %s", rc, msg);
    dyn_td_buf_free(&o);

    memset(&o, 0, sizeof o);
    rc = apply("a\nb\n", "@@ -1,2 +1,2 @@\n a\n-c\n+d\n", &o, msg);
    CHECK(rc == DYN_TD_ECONFLICT && strstr(msg, "text line 2 differs from patch line 3"), "conflict: %d %s", rc, msg);
    dyn_td_buf_free(&o);

    memset(&o, 0, sizeof o);
    rc = apply("a\nb\n", "@@ -1,2 +1,2 @@\n a\n", &o, msg);
    CHECK(rc == DYN_TD_ESYNTAX && strstr(msg, "is truncated: 1 old and 1 new lines missing"), "truncated: %d %s", rc, msg);
    dyn_td_buf_free(&o);

    {
        size_t n = (size_t)DYN_TD_MAX_TOKENS + 1;
        char* big = xmalloc(n + 1);
        dyn_td_res_t r;
        memset(big, 'q', n);
        big[n] = 0;
        rc = dyn_td_diff((const uint8_t*)big, (uint32_t)n, (const uint8_t*)"q", 1, DYN_TD_CHARS, &r);
        CHECK(rc == DYN_TD_ETOKENS, "token cap: %d", rc);
        memset(&o, 0, sizeof o);
        memset(big, '\n', n);
        rc = apply(big, "", &o, msg);
        CHECK(rc == DYN_TD_ETOKENS, "apply token cap: %d", rc);
        dyn_td_buf_free(&o);
        free(big);
    }
}

static void test_lopsided(void)
{
    static const char* const SMALL[] = { "abc", "axc", "xxyxx", "zxz" };
    size_t n = 65536, i;
    char* big = xmalloc(n + 16);
    memset(big, 'x', n);
    big[n] = 0;
    for (i = 0; i < 4; i++) {
        check_script(big, n, SMALL[i], strlen(SMALL[i]), DYN_TD_CHARS);
        check_script(SMALL[i], strlen(SMALL[i]), big, n, DYN_TD_CHARS);
    }
    memcpy(big, "pre", 3);
    check_script(big, n, "preab", 5, DYN_TD_CHARS);
    free(big);
}

int main(void)
{
    test_lopsided();
    test_fixed();
    test_random();
    printf("test_textdiff_core: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
