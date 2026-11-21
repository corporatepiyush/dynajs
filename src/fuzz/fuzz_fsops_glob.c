#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-path.h"
#include "fsops/fsops.h"

#define FZ_PAT_MAX 64
#define FZ_STR_MAX 64
#define FZ_PATH_MAX 256

static void fz_fail(const char* what)
{
    fprintf(stderr, "oracle: %s\n", what);
    abort();
}

static signed char fz_memo[FZ_PAT_MAX + 1][FZ_STR_MAX + 1];

static int fz_class(const char* p, size_t* end, unsigned char c, int* hit)
{
    const char* q = p + 1;
    const char* start;
    int negate = 0, matched = 0;
    if (*q == '!' || *q == '^') {
        negate = 1;
        q++;
    }
    start = q;
    while (*q && !(*q == ']' && q != start)) {
        if (q[0] && q[1] == '-' && q[2] && q[2] != ']') {
            if ((unsigned char)q[0] <= c && c <= (unsigned char)q[2])
                matched = 1;
            q += 3;
        } else {
            if ((unsigned char)*q == c)
                matched = 1;
            q++;
        }
    }
    if (*q != ']')
        return 0;
    *end = (size_t)(q + 1 - p);
    *hit = matched != negate;
    return 1;
}

static int fz_ref(const char* p0, const char* s0, size_t i, size_t j)
{
    const char *p = p0 + i, *s = s0 + j;
    int r;
    if (fz_memo[i][j] >= 0)
        return fz_memo[i][j];
    if (*p == '\0')
        r = *s == '\0';
    else if (*p == '*')
        r = fz_ref(p0, s0, i + 1, j) || (*s != '\0' && fz_ref(p0, s0, i, j + 1));
    else if (*s == '\0')
        r = 0;
    else if (*p == '?')
        r = fz_ref(p0, s0, i + 1, j + 1);
    else if (*p == '[') {
        size_t end = 0;
        int hit = 0;
        if (fz_class(p, &end, (unsigned char)*s, &hit))
            r = hit && fz_ref(p0, s0, i + end, j + 1);
        else
            r = *s == '[' && fz_ref(p0, s0, i + 1, j + 1);
    } else
        r = *p == *s && fz_ref(p0, s0, i + 1, j + 1);
    fz_memo[i][j] = (signed char)r;
    return r;
}

static int fz_ref_match(const char* p, const char* s)
{
    memset(fz_memo, -1, sizeof(fz_memo));
    return fz_ref(p, s, 0, 0);
}

static void fz_witness(const char* p, char* w)
{
    size_t k = 0;
    while (*p) {
        if (*p == '*') {
            p++;
        } else if (*p == '?') {
            w[k++] = 'x';
            p++;
        } else if (*p == '[') {
            size_t end = 0;
            int hit = 0, c, got = 'x';
            if (!fz_class(p, &end, 'x', &hit)) {
                w[k++] = '[';
                p++;
                continue;
            }
            for (c = 1; c < 256; c++) {
                fz_class(p, &end, (unsigned char)c, &hit);
                if (hit) {
                    got = c;
                    break;
                }
            }
            w[k++] = (char)got;
            p += end;
        } else {
            w[k++] = *p++;
        }
    }
    w[k] = 0;
}

static int fz_balanced(const char* p)
{
    int depth = 0;
    for (; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth < 0)
            return 0;
    }
    return depth == 0;
}

static size_t fz_close(const char* p, size_t k)
{
    int depth = 0;
    for (;; k++) {
        if (p[k] == '{')
            depth++;
        else if (p[k] == '}' && --depth == 0)
            return k;
    }
}

static uint64_t fz_count(const char* p, size_t a, size_t b, int* grouped)
{
    uint64_t prod = 1;
    size_t k = a;
    while (k < b) {
        if (p[k] == '{') {
            size_t m = fz_close(p, k), q, s = k + 1;
            uint64_t g = 0;
            int depth = 0, comma = 0;
            for (q = k + 1; q < m; q++) {
                if (p[q] == '{')
                    depth++;
                else if (p[q] == '}')
                    depth--;
                else if (p[q] == ',' && depth == 0)
                    comma = 1;
            }
            if (comma) {
                *grouped = 1;
                depth = 0;
                for (q = k + 1; q <= m; q++) {
                    if (q == m || (p[q] == ',' && depth == 0)) {
                        g += fz_count(p, s, q, grouped);
                        s = q + 1;
                    } else if (p[q] == '{')
                        depth++;
                    else if (p[q] == '}')
                        depth--;
                }
            } else
                g = fz_count(p, k + 1, m, grouped);
            prod *= g;
            if (prod > 1u << 20)
                prod = 1u << 20;
            k = m + 1;
        } else
            k++;
    }
    return prod;
}

static size_t fz_piece(const char* p, size_t k)
{
    return p[k] == '{' ? fz_close(p, k) + 1 : k + 1;
}

static void fz_nth(const char* p, size_t a, size_t b, uint64_t idx, char* out, size_t* o)
{
    int g = 0;
    while (a < b) {
        size_t e = fz_piece(p, a), q, s;
        uint64_t cr = fz_count(p, e, b, &g), i1 = idx / cr, cum = 0;
        int depth = 0, comma = 0;
        idx %= cr;
        if (p[a] != '{') {
            out[(*o)++] = p[a];
            a = e;
            continue;
        }
        for (q = a + 1; q < e - 1; q++) {
            if (p[q] == '{')
                depth++;
            else if (p[q] == '}')
                depth--;
            else if (p[q] == ',' && depth == 0)
                comma = 1;
        }
        if (!comma) {
            out[(*o)++] = '{';
            fz_nth(p, a + 1, e - 1, i1, out, o);
            out[(*o)++] = '}';
        } else {
            depth = 0;
            s = a + 1;
            for (q = a + 1; q <= e - 1; q++) {
                if (q == e - 1 || (p[q] == ',' && depth == 0)) {
                    uint64_t c = fz_count(p, s, q, &g);
                    if (i1 < cum + c) {
                        fz_nth(p, s, q, i1 - cum, out, o);
                        break;
                    }
                    cum += c;
                    s = q + 1;
                } else if (p[q] == '{')
                    depth++;
                else if (p[q] == '}')
                    depth--;
            }
        }
        a = e;
    }
}

static size_t fz_take(const uint8_t** d, size_t* n, char* out, size_t cap)
{
    size_t k = 0;
    while (*n && k < cap) {
        uint8_t c = **d;
        (*d)++;
        (*n)--;
        if (c == 0)
            break;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return k;
}

static void fz_glob(const uint8_t* d, size_t n)
{
    char p[FZ_PAT_MAX + 1], s[FZ_STR_MAX + 1], w[FZ_PAT_MAX + 1];
    int rw;
    fz_take(&d, &n, p, FZ_PAT_MAX);
    fz_take(&d, &n, s, FZ_STR_MAX);
    if (dyn_glob_match(p, s) != fz_ref_match(p, s))
        fz_fail("dyn_glob_match disagrees with the memoized reference matcher");
    fz_witness(p, w);
    rw = fz_ref_match(p, w);
    if (dyn_glob_match(p, w) != rw)
        fz_fail("dyn_glob_match disagrees with the reference on the pattern's lowest witness");
    if (!strchr(p, '[') && !rw)
        fz_fail("the reference rejects the witness of a class-free pattern");
}

static void fz_expand(const uint8_t* d, size_t n)
{
    char p[FZ_PAT_MAX + 1], s[FZ_STR_MAX + 1];
    dyn_glob_alts a, b;
    size_t plen, i;
    int rc, any = 0, plain, grouped = 0;
    uint64_t want = 0;
    plen = fz_take(&d, &n, p, FZ_PAT_MAX);
    fz_take(&d, &n, s, FZ_STR_MAX);
    plain = !strchr(p, '[') && fz_balanced(p);
    if (plain)
        want = fz_count(p, 0, plen, &grouped);
    rc = dyn_glob_expand(p, &a);
    if (plain && (want > DYN_GLOB_MAX_ALTS) != (rc == -2))
        fz_fail("expansion limit disagrees with the reference alternative count");
    if (plain && rc == 0 && a.n != want)
        fz_fail("alternative count disagrees with the reference product of brace groups");
    if (rc) {
        if (a.v || a.n)
            fz_fail("a failed expansion left alternatives behind");
        return;
    }
    if (a.n < 1 || a.n > DYN_GLOB_MAX_ALTS)
        fz_fail("alternative count outside 1..DYN_GLOB_MAX_ALTS");
    if (!strchr(p, '{') && (a.n != 1 || strcmp(a.v[0], p) != 0))
        fz_fail("a pattern without braces did not expand to itself");
    for (i = 0; i < a.n; i++) {
        if (strlen(a.v[i]) > plen)
            fz_fail("an alternative is longer than its pattern");
        if (plain) {
            int g2 = 0;
            if (!fz_balanced(a.v[i]))
                fz_fail("a balanced pattern expanded to an unbalanced alternative");
            fz_count(a.v[i], 0, strlen(a.v[i]), &g2);
            if (g2)
                fz_fail("an alternative still holds a brace group with a top-level comma");
            if (want <= 64) {
                char ref[FZ_PAT_MAX + 1];
                size_t o = 0;
                fz_nth(p, 0, plen, i, ref, &o);
                ref[o] = 0;
                if (strcmp(ref, a.v[i]) != 0)
                    fz_fail("an alternative differs from the reference expansion at the same index");
            }
        }
        if (fz_ref_match(a.v[i], s))
            any = 1;
    }
    if (dyn_glob_match_alts(&a, s) != any)
        fz_fail("dyn_glob_match_alts disagrees with the reference over the alternatives");
    if (dyn_glob_expand(p, &b) != 0 || b.n != a.n)
        fz_fail("expansion is not deterministic");
    for (i = 0; i < a.n; i++)
        if (strcmp(a.v[i], b.v[i]) != 0)
            fz_fail("expansion order is not deterministic");
    dyn_glob_alts_free(&b);
    dyn_glob_alts_free(&a);
}

static void fz_clean(const char* o, size_t k)
{
    size_t i;
    if (k == 0)
        fz_fail("normalize returned an empty path");
    for (i = 0; i + 1 < k; i++)
        if (o[i] == '/' && o[i + 1] == '/')
            fz_fail("normalized path has a double slash");
    for (i = 0; i + 2 < k; i++)
        if (o[i] == '/' && o[i + 1] == '.' && o[i + 2] == '/')
            fz_fail("normalized path has a /./ segment");
}

static void fz_path(const uint8_t* d, size_t n)
{
    char a[FZ_PATH_MAX + 2], b[FZ_PATH_MAX + 2];
    char n1[FZ_PATH_MAX + 4], n2[FZ_PATH_MAX + 4];
    char rel[2 * (2 * FZ_PATH_MAX + 4) + 16];
    char scratch[8 * FZ_PATH_MAX + 64], r1[4 * FZ_PATH_MAX + 16], r2[4 * FZ_PATH_MAX + 16];
    const char* parts[2];
    size_t lens[2], la, lb, k1, k2, kr, q1, q2;
    a[0] = '/';
    b[0] = '/';
    la = 1 + fz_take(&d, &n, a + 1, FZ_PATH_MAX);
    lb = 1 + fz_take(&d, &n, b + 1, FZ_PATH_MAX);
    k1 = dyn_path_normalize(a + 1, la - 1, n1);
    if (k1 > dyn_path_normalize_cap(la - 1))
        fz_fail("normalize wrote past its cap");
    k2 = dyn_path_normalize(n1, k1, n2);
    if (k2 != k1 || memcmp(n1, n2, k1) != 0)
        fz_fail("normalize is not idempotent");
    fz_clean(n1, k1);
    kr = dyn_path_relative(a, la, b, lb, rel, scratch);
    if (kr > dyn_path_relative_cap(la, lb))
        fz_fail("relative wrote past its cap");
    parts[0] = a;
    lens[0] = la;
    parts[1] = rel;
    lens[1] = kr;
    q1 = dyn_path_resolve(parts, lens, 2, r1, scratch);
    parts[0] = b;
    lens[0] = lb;
    q2 = dyn_path_resolve(parts, lens, 1, r2, scratch);
    if (q1 != q2 || memcmp(r1, r2, q1) != 0)
        fz_fail("resolve(from, relative(from, to)) != resolve(to)");
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1 || size > 1024)
        return 0;
    switch (data[0] % 3) {
    case 0:
        fz_glob(data + 1, size - 1);
        break;
    case 1:
        fz_expand(data + 1, size - 1);
        break;
    default:
        fz_path(data + 1, size - 1);
        break;
    }
    return 0;
}
