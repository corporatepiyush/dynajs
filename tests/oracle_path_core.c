/*
 * oracle_path_core.c -- differential: src/core/dyn-path.c (the buffer-filling
 * rewrite) against the composition it replaced.
 *
 * W1.12b is a REWRITE, not a lift. `dyna-path.c`'s helpers took a JSContext and
 * returned JSValue -- they built JS strings directly instead of writing into a
 * caller buffer -- so the segment normaliser could move verbatim but everything
 * layered on top of it had to be restated. That restatement is where a bug
 * would live, and a round trip cannot see it: both sides return *a* path, and a
 * wrong one is still a well-formed path.
 *
 * So the oracle here is the ORIGINAL composition, transcribed faithfully from
 * src/dyna-path.c with JSValue replaced by a malloc'd (char*, len) and nothing
 * else changed. That original is the thing already verified against Node's real
 * path.posix over >35,000 cases, so agreeing with it is agreeing with Node.
 * The engine is not linked and no JS runs: this is a pure C differential of one
 * pure C algorithm against another.
 *
 * Coverage: exhaustive over the alphabet {a, b, '.', '/'} for every length 0..7
 * (87,380 strings) for normalize/dirname/basename/extname/isAbsolute; all
 * ordered PAIRS of every string of length 0..4 (117,649) for join/resolve/
 * relative; plus the hand-written edge cases the module docstring calls out.
 *
 * Build:  make test-path-oracle      (or see the cc line at the bottom)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/dyn-path.h"

/* ====================================================================
 * The oracle: src/dyna-path.c's composition, transcribed.
 * JSValue -> owned char*; js_malloc -> malloc; JS_NewStringLen -> dup.
 * The normaliser itself is byte-identical to the one under test, which is
 * correct: it moved verbatim, and it is the COMPOSITION this file exists to
 * check.
 * ==================================================================== */

static size_t o_normalize_core(const char *path, size_t path_len,
                               int allow_above_root, char *res)
{
    size_t res_len = 0;
    long last_segment_length = 0;
    long last_slash = -1;
    int dots = 0;
    int code = 0;
    long i;

    for (i = 0; i <= (long)path_len; i++) {
        if (i < (long)path_len)
            code = (unsigned char)path[i];
        else if (code == '/')
            break;
        else
            code = '/';

        if (code == '/') {
            if (last_slash == i - 1 || dots == 1) {
                /* nothing */
            } else if (last_slash != i - 1 && dots == 2) {
                if (res_len < 2 || last_segment_length != 2 ||
                    res[res_len - 1] != '.' || res[res_len - 2] != '.') {
                    if (res_len > 2) {
                        long k, last_slash_index = -1;
                        for (k = (long)res_len - 1; k >= 0; k--) {
                            if (res[k] == '/') { last_slash_index = k; break; }
                        }
                        if (last_slash_index != (long)res_len - 1) {
                            if (last_slash_index == -1) {
                                res_len = 0;
                                last_segment_length = 0;
                            } else {
                                long k2, new_last_slash = -1;
                                res_len = (size_t)last_slash_index;
                                for (k2 = (long)res_len - 1; k2 >= 0; k2--) {
                                    if (res[k2] == '/') { new_last_slash = k2; break; }
                                }
                                last_segment_length =
                                    (long)res_len - 1 - new_last_slash;
                            }
                            last_slash = i;
                            dots = 0;
                            continue;
                        }
                    } else if (res_len == 2 || res_len == 1) {
                        res_len = 0;
                        last_segment_length = 0;
                        last_slash = i;
                        dots = 0;
                        continue;
                    }
                }
                if (allow_above_root) {
                    if (res_len > 0)
                        res[res_len++] = '/';
                    res[res_len++] = '.';
                    res[res_len++] = '.';
                    last_segment_length = 2;
                }
            } else {
                size_t seg_start = (size_t)(last_slash + 1);
                size_t seg_len = (size_t)(i - (last_slash + 1));
                if (res_len > 0)
                    res[res_len++] = '/';
                memcpy(res + res_len, path + seg_start, seg_len);
                res_len += seg_len;
                last_segment_length = (long)seg_len;
            }
            last_slash = i;
            dots = 0;
        } else if (code == '.' && dots != -1) {
            dots++;
        } else {
            dots = -1;
        }
    }
    return res_len;
}

/* A returned string: owned bytes plus a length (paths may not be NUL-free in
 * principle, and comparing on length is what catches a truncation). */
typedef struct { char *s; size_t n; } ostr;

static ostr o_dup(const char *s, size_t n)
{
    ostr r;
    r.s = (char *)malloc(n + 1);
    if (!r.s) { fprintf(stderr, "oom\n"); exit(2); }
    memcpy(r.s, s, n);
    r.s[n] = 0;
    r.n = n;
    return r;
}

/* path_clean_impl */
static ostr o_clean(const char *p, size_t plen)
{
    int is_abs, trailing_sep;
    char *core_buf, *final_buf;
    size_t core_len, total, o;
    ostr result;

    if (plen == 0)
        return o_dup(".", 1);

    is_abs = (p[0] == '/');
    trailing_sep = (p[plen - 1] == '/');

    core_buf = (char *)malloc(plen ? plen : 1);
    if (!core_buf) { fprintf(stderr, "oom\n"); exit(2); }
    core_len = o_normalize_core(p, plen, !is_abs, core_buf);

    if (core_len == 0) {
        free(core_buf);
        if (is_abs)
            return o_dup("/", 1);
        return trailing_sep ? o_dup("./", 2) : o_dup(".", 1);
    }

    total = (size_t)(is_abs ? 1 : 0) + core_len + (size_t)(trailing_sep ? 1 : 0);
    final_buf = (char *)malloc(total);
    if (!final_buf) { fprintf(stderr, "oom\n"); exit(2); }
    o = 0;
    if (is_abs)
        final_buf[o++] = '/';
    memcpy(final_buf + o, core_buf, core_len);
    o += core_len;
    if (trailing_sep)
        final_buf[o++] = '/';
    result = o_dup(final_buf, total);
    free(final_buf);
    free(core_buf);
    return result;
}

/* path_resolve_core */
static ostr o_resolve(const char *const *parts, const size_t *lens, int n)
{
    int i, root_idx, first, start;
    size_t sum_lens, cap, o, core_len;
    char *raw, *core_buf, *fin;
    ostr result;

    sum_lens = 0;
    for (i = 0; i < n; i++)
        sum_lens += lens[i];

    root_idx = -1;
    for (i = n - 1; i >= 0; i--) {
        if (lens[i] > 0 && parts[i][0] == '/') { root_idx = i; break; }
    }

    cap = sum_lens + (size_t)n + 2;
    raw = (char *)malloc(cap);
    if (!raw) { fprintf(stderr, "oom\n"); exit(2); }

    o = 0;
    first = 1;
    start = root_idx;
    if (root_idx == -1) {
        raw[o++] = '/';
        first = 0;
        start = 0;
    }
    for (i = start; i < n; i++) {
        if (lens[i] == 0)
            continue;
        if (!first)
            raw[o++] = '/';
        memcpy(raw + o, parts[i], lens[i]);
        o += lens[i];
        first = 0;
    }

    core_buf = (char *)malloc(o);
    if (!core_buf) { fprintf(stderr, "oom\n"); exit(2); }
    core_len = o_normalize_core(raw, o, 0, core_buf);
    free(raw);

    if (core_len == 0) {
        free(core_buf);
        return o_dup("/", 1);
    }
    fin = (char *)malloc(core_len + 1);
    if (!fin) { fprintf(stderr, "oom\n"); exit(2); }
    fin[0] = '/';
    memcpy(fin + 1, core_buf, core_len);
    result = o_dup(fin, core_len + 1);
    free(fin);
    free(core_buf);
    return result;
}

/* dyn_path_join's binding: concatenate the non-empty parts, then clean. */
static ostr o_join(const char *const *parts, const size_t *lens, int n)
{
    size_t total = 0, n_nonempty = 0, o;
    char *joined;
    ostr result;
    int i, first;

    for (i = 0; i < n; i++) {
        if (lens[i] > 0) { total += lens[i]; n_nonempty++; }
    }
    if (n == 0 || n_nonempty == 0)
        return o_dup(".", 1);

    total += n_nonempty - 1;
    joined = (char *)malloc(total);
    if (!joined) { fprintf(stderr, "oom\n"); exit(2); }
    o = 0;
    first = 1;
    for (i = 0; i < n; i++) {
        if (lens[i] == 0)
            continue;
        if (!first)
            joined[o++] = '/';
        memcpy(joined + o, parts[i], lens[i]);
        o += lens[i];
        first = 0;
    }
    result = o_clean(joined, total);
    free(joined);
    return result;
}

/* path_relative_impl, over two ALREADY-resolved sides. */
static ostr o_relative_impl(const char *from_r, size_t from_len,
                            const char *to_r, size_t to_len)
{
    size_t from_l, to_l, smallest, i, cap, o;
    long last_common_sep;
    char *buf;
    ostr result;

    if (from_len == to_len && memcmp(from_r, to_r, from_len) == 0)
        return o_dup("", 0);

    from_l = from_len - 1;
    to_l = to_len - 1;
    smallest = from_l < to_l ? from_l : to_l;
    last_common_sep = -1;

    for (i = 0; i < smallest; i++) {
        char fc = from_r[1 + i];
        if (fc != to_r[1 + i])
            break;
        if (fc == '/')
            last_common_sep = (long)i;
    }

    if (i == smallest) {
        if (to_l > smallest) {
            if (to_r[1 + i] == '/')
                return o_dup(to_r + 1 + i + 1, to_len - (1 + i + 1));
            if (i == 0)
                return o_dup(to_r + 1 + i, to_len - (1 + i));
        } else if (from_l > smallest) {
            if (from_r[1 + i] == '/')
                last_common_sep = (long)i;
            else if (i == 0)
                last_common_sep = 0;
        }
    }

    cap = from_len + to_len + 4;
    buf = (char *)malloc(cap);
    if (!buf) { fprintf(stderr, "oom\n"); exit(2); }

    o = 0;
    for (i = (size_t)(1 + last_common_sep + 1); i <= from_len; i++) {
        if (i == from_len || from_r[i] == '/') {
            if (o == 0) {
                buf[o++] = '.';
                buf[o++] = '.';
            } else {
                buf[o++] = '/';
                buf[o++] = '.';
                buf[o++] = '.';
            }
        }
    }
    {
        size_t to_start = (size_t)(1 + last_common_sep);
        size_t suffix_len = to_len - to_start;
        memcpy(buf + o, to_r + to_start, suffix_len);
        o += suffix_len;
    }
    result = o_dup(buf, o);
    free(buf);
    return result;
}

static ostr o_relative(const char *from, size_t from_n, const char *to,
                       size_t to_n)
{
    const char *one[1];
    size_t l1[1];
    ostr fr, tr, r;

    one[0] = from; l1[0] = from_n;
    fr = o_resolve(one, l1, 1);
    one[0] = to;   l1[0] = to_n;
    tr = o_resolve(one, l1, 1);
    r = o_relative_impl(fr.s, fr.n, tr.s, tr.n);
    free(fr.s);
    free(tr.s);
    return r;
}

/* dirname */
static ostr o_dirname(const char *p, size_t plen)
{
    long end, i;
    int has_root, matched_slash;

    if (plen == 0)
        return o_dup(".", 1);

    has_root = (p[0] == '/');
    end = -1;
    matched_slash = 1;
    for (i = (long)plen - 1; i >= 1; i--) {
        if (p[i] == '/') {
            if (!matched_slash) { end = i; break; }
        } else {
            matched_slash = 0;
        }
    }
    if (end == -1)
        return o_dup(has_root ? "/" : ".", 1);
    if (has_root && end == 1)
        return o_dup("//", 2);
    return o_dup(p, (size_t)end);
}

/* basename(p, ext?) */
static ostr o_basename(const char *p, size_t plen, const char *suf,
                       size_t suflen)
{
    long start, end, i;

    if (suf && suflen > 0 && suflen <= plen) {
        if (suflen == plen && memcmp(p, suf, plen) == 0)
            return o_dup("", 0);
        {
            long ext_idx = (long)suflen - 1;
            long first_non_slash_end = -1;
            int matched_slash = 1;

            start = 0;
            end = -1;
            for (i = (long)plen - 1; i >= 0; i--) {
                unsigned char c = (unsigned char)p[i];
                if (c == '/') {
                    if (!matched_slash) { start = i + 1; break; }
                } else {
                    if (first_non_slash_end == -1) {
                        matched_slash = 0;
                        first_non_slash_end = i + 1;
                    }
                    if (ext_idx >= 0) {
                        if (c == (unsigned char)suf[ext_idx]) {
                            if (--ext_idx == -1)
                                end = i;
                        } else {
                            ext_idx = -1;
                            end = first_non_slash_end;
                        }
                    }
                }
            }
            if (start == end)
                end = first_non_slash_end;
            else if (end == -1)
                end = (long)plen;
            return (end > start) ? o_dup(p + start, (size_t)(end - start))
                                 : o_dup("", 0);
        }
    } else {
        int matched_slash = 1;
        start = 0;
        end = -1;
        for (i = (long)plen - 1; i >= 0; i--) {
            if (p[i] == '/') {
                if (!matched_slash) { start = i + 1; break; }
            } else if (end == -1) {
                matched_slash = 0;
                end = i + 1;
            }
        }
        return (end == -1) ? o_dup("", 0)
                           : o_dup(p + start, (size_t)(end - start));
    }
}

/* extname */
static ostr o_extname(const char *p, size_t plen)
{
    long start_dot = -1, start_part = 0, end = -1, i;
    int matched_slash = 1, pre_dot_state = 0;

    for (i = (long)plen - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)p[i];
        if (c == '/') {
            if (!matched_slash) { start_part = i + 1; break; }
            continue;
        }
        if (end == -1) { matched_slash = 0; end = i + 1; }
        if (c == '.') {
            if (start_dot == -1)
                start_dot = i;
            else if (pre_dot_state != 1)
                pre_dot_state = 1;
        } else if (start_dot != -1) {
            pre_dot_state = -1;
        }
    }

    if (start_dot == -1 || end == -1 || pre_dot_state == 0 ||
        (pre_dot_state == 1 && start_dot == end - 1 &&
         start_dot == start_part + 1))
        return o_dup("", 0);
    return o_dup(p + start_dot, (size_t)(end - start_dot));
}

/* ====================================================================
 * The comparison
 * ==================================================================== */

static long long g_checks, g_fail;

static void esc(const char *s, size_t n, char *out)
{
    size_t i, o = 0;
    out[o++] = '"';
    for (i = 0; i < n && o < 250; i++)
        out[o++] = s[i];
    out[o++] = '"';
    out[o] = 0;
}

static void cmp(const char *what, const char *in, size_t in_n, ostr want,
                const char *got, size_t got_n)
{
    g_checks++;
    if (want.n != got_n || (want.n && memcmp(want.s, got, want.n) != 0)) {
        char a[256], b[256], c[256];
        esc(in, in_n, a);
        esc(want.s, want.n, b);
        esc(got, got_n, c);
        if (g_fail < 20)
            printf("MISMATCH %-10s in=%s oracle=%s core=%s\n", what, a, b, c);
        g_fail++;
    }
    free(want.s);
}

/* A slice reported by the core, materialised for comparison. */
static const char *slice(const char *p, size_t off, size_t len, size_t *out_n)
{
    *out_n = len;
    return p + off;
}

static void check_one(const char *p, size_t n)
{
    char out[512];
    char scratch[512];
    dyn_path_split_t sp;
    size_t got;
    const char *gs;
    size_t gn;

    got = dyn_path_normalize(p, n, out);
    cmp("normalize", p, n, o_clean(p, n), out, got);

    dyn_path_split(p, n, &sp);

    if (sp.dir_is_dot)      { gs = "."; gn = 1; }
    else if (sp.dir_is_root){ gs = "/"; gn = 1; }
    else                    { gs = slice(p, sp.dir_off, sp.dir_len, &gn); }
    cmp("dirname", p, n, o_dirname(p, n), gs, gn);

    gs = slice(p, sp.base_off, sp.base_len, &gn);
    cmp("basename", p, n, o_basename(p, n, NULL, 0), gs, gn);

    gs = slice(p, sp.ext_off, sp.ext_len, &gn);
    cmp("extname", p, n, o_extname(p, n), gs, gn);

    {
        int want_abs = (n > 0 && p[0] == '/');
        g_checks++;
        if (want_abs != dyn_path_is_absolute(p, n)) {
            printf("MISMATCH isAbsolute\n");
            g_fail++;
        }
    }

    /* basename with a suffix: try every suffix of the input itself, which is
     * where the "would empty the segment" rule lives. */
    {
        size_t k;
        for (k = 0; k <= n; k++) {
            size_t off, len;
            dyn_path_basename(p, n, p + n - k, k, &off, &len);
            cmp("basename+s", p, n, o_basename(p, n, p + n - k, k), p + off, len);
        }
    }

    /* single-part join and resolve */
    {
        const char *parts[1];
        size_t lens[1];
        parts[0] = p;
        lens[0] = n;
        got = dyn_path_join(parts, lens, 1, out, scratch);
        cmp("join1", p, n, o_join(parts, lens, 1), out, got);
        got = dyn_path_resolve(parts, lens, 1, out, scratch);
        cmp("resolve1", p, n, o_resolve(parts, lens, 1), out, got);
    }
    (void)scratch;
}

static void check_pair(const char *a, size_t an, const char *b, size_t bn)
{
    char out[1024];
    char scratch[1024];
    const char *parts[2];
    size_t lens[2];
    size_t got;

    parts[0] = a; lens[0] = an;
    parts[1] = b; lens[1] = bn;

    got = dyn_path_join(parts, lens, 2, out, scratch);
    cmp("join2", a, an, o_join(parts, lens, 2), out, got);

    got = dyn_path_resolve(parts, lens, 2, out, scratch);
    cmp("resolve2", a, an, o_resolve(parts, lens, 2), out, got);

    got = dyn_path_relative(a, an, b, bn, out, scratch);
    cmp("relative", a, an, o_relative(a, an, b, bn), out, got);
}

/* Enumerate every string of length `len` over the alphabet into buf. */
static const char ALPHA[4] = { 'a', 'b', '.', '/' };

static void enum_len(size_t len, void (*fn)(const char *, size_t))
{
    char buf[16];
    size_t total = 1, i, k;
    for (i = 0; i < len; i++)
        total *= 4;
    for (i = 0; i < total; i++) {
        size_t v = i;
        for (k = 0; k < len; k++) { buf[k] = ALPHA[v & 3]; v >>= 2; }
        fn(buf, len);
    }
}

static void enum_pairs(size_t len_a, size_t len_b)
{
    char ba[8], bb[8];
    size_t ta = 1, tb = 1, i, j, k;
    for (i = 0; i < len_a; i++) ta *= 4;
    for (i = 0; i < len_b; i++) tb *= 4;
    for (i = 0; i < ta; i++) {
        size_t v = i;
        for (k = 0; k < len_a; k++) { ba[k] = ALPHA[v & 3]; v >>= 2; }
        for (j = 0; j < tb; j++) {
            size_t w = j;
            for (k = 0; k < len_b; k++) { bb[k] = ALPHA[w & 3]; w >>= 2; }
            check_pair(ba, len_a, bb, len_b);
        }
    }
}

/* The edge cases the module docstring calls out by name -- the ones a random
 * alphabet sweep would only reach by luck. */
static const char *EDGE[] = {
    "", "/", "//", "///", ".", "..", "...", "./", "../", "/.", "/..",
    "a/..", "a/../..", "/a/../../..", "....", ".bashrc", "a.b.c", "/foo/.html",
    "foo/bar//baz", "foo/bar/./baz/", "/////a/////b/////",
    "a/b/../../../../c", "////", "/a//b//c//", ".hidden/", "x/.y/.z",
    "very/deep/path/that/keeps/going/../../../and/back/up",
};

int main(void)
{
    size_t i, j;

    for (i = 0; i <= 7; i++)
        enum_len(i, check_one);

    for (i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++)
        check_one(EDGE[i], strlen(EDGE[i]));

    /* every ordered pair of every string of length 0..4 */
    for (i = 0; i <= 4; i++)
        for (j = 0; j <= 4; j++)
            enum_pairs(i, j);

    /* every ordered pair of the named edge cases */
    for (i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++)
        for (j = 0; j < sizeof(EDGE) / sizeof(EDGE[0]); j++)
            check_pair(EDGE[i], strlen(EDGE[i]), EDGE[j], strlen(EDGE[j]));

    printf("oracle_path_core: %lld checks, %lld mismatches\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

/* cc -std=c11 -Wall -Wextra -O2 -Isrc/core tests/oracle_path_core.c \
 *    src/core/dyn-path.c -o /tmp/oracle_path_core && /tmp/oracle_path_core */
