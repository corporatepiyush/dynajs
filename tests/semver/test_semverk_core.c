/*
 * test_semverk_core.c -- src/core/semverk (the dyna:semver kernels: version
 * parse and precedence, range parse and test, intersects, stable sort,
 * increment) as a standalone C library.
 *
 * Oracles come from the SemVer 2.0.0 text and the node-semver README, not
 * from the code. Versions: a BNF-shaped parser written here (plus the
 * dyna extensions: a leading 'v', surrounding ASCII/NBSP whitespace, at most
 * 16 digits and 2^53-1 per number, at most 24 identifiers per list) is run
 * against 150k mutated version strings; precedence follows section 11
 * (numeric < alphanumeric, shorter list first, build ignored). Ranges are
 * generated as structured sets of primitives (=, <, <=, >, >=, ~, ~>, ^,
 * x/X/* partials, hyphen, ||) and the oracle evaluates the primitives
 * directly as tuple intervals: an upper bound like "<2.0.0" written by a
 * caret is "major.minor.patch tuple below 2.0.0", and the prerelease rule
 * (a prerelease passes a set only if a concrete bound in it shares the
 * tuple and has a prerelease) is applied separately. Intersects is checked
 * one way: a release version satisfying both ranges forces true. Sort is
 * checked against an insertion sort, inc against node-semver's increment
 * fixtures. As in node-semver, a desugared ">=0.0.0" (from *, x, ^*, ~*,
 * an empty set or a written ">=0.0.0") is ANY: it does not exclude 0.0.0
 * prereleases, so "* <0.0.0-2" admits 0.0.0-0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-semverk.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static unsigned long long rs = 0x2545f4914f6cdd1dull;
static unsigned rnd(unsigned n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

typedef struct {
    char s[24];
    int num;
    unsigned long long v;
} OId;

typedef struct {
    unsigned long long t[3];
    int npre, nbuild;
    OId pre[32], build[32];
} OVer;

static int o_num(const char* s, size_t n, unsigned long long* out, int lead0_ok)
{
    unsigned long long v = 0;
    size_t i;
    if (n == 0)
        return -1;
    if (!lead0_ok && n > 1 && s[0] == '0')
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        if (v > 900719925474099ull)
            return -2;
        v = v * 10 + (unsigned long long)(s[i] - '0');
    }
    if (v > 9007199254740991ull)
        return -2;
    *out = v;
    return 0;
}

static int o_ids(const char* s, size_t n, OId* ids, int* cnt, int build)
{
    size_t a = 0, i;
    *cnt = 0;
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] != '.')
            continue;
        {
            size_t l = i - a, k;
            int dig = 1;
            if (l == 0 || l >= sizeof(ids[0].s) || *cnt >= 24)
                return -1;
            for (k = a; k < i; k++) {
                char c = s[k];
                if (c >= '0' && c <= '9')
                    continue;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')
                    dig = 0;
                else
                    return -1;
            }
            memcpy(ids[*cnt].s, s + a, l);
            ids[*cnt].s[l] = 0;
            ids[*cnt].num = dig;
            ids[*cnt].v = 0;
            if (dig && !build && o_num(s + a, l, &ids[*cnt].v, 0))
                return -1;
            (*cnt)++;
            a = i + 1;
        }
    }
    return 0;
}

static int o_parse(const char* s0, OVer* v)
{
    char buf[512];
    const char *s = buf, *p, *e;
    size_t n = strlen(s0);
    int k;
    memset(v, 0, sizeof(*v));
    if (n >= sizeof(buf))
        return -1;
    memcpy(buf, s0, n + 1);
    for (;;) {
        if (*s == ' ' || *s == '\t' || *s == '\n')
            s++;
        else if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0)
            s += 2;
        else
            break;
    }
    e = s + strlen(s);
    for (;;) {
        if (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'))
            e--;
        else if (e - s >= 2 && (unsigned char)e[-2] == 0xC2 && (unsigned char)e[-1] == 0xA0)
            e -= 2;
        else
            break;
    }
    if (s < e && *s == 'v')
        s++;
    for (k = 0; k < 3; k++) {
        p = s;
        while (s < e && *s >= '0' && *s <= '9')
            s++;
        if (s - p > 16 || o_num(p, (size_t)(s - p), &v->t[k], 0))
            return -1;
        if (k < 2) {
            if (s >= e || *s != '.')
                return -1;
            s++;
        }
    }
    if (s < e && *s == '-') {
        p = ++s;
        while (s < e && *s != '+')
            s++;
        if (o_ids(p, (size_t)(s - p), v->pre, &v->npre, 0))
            return -1;
    }
    if (s < e && *s == '+') {
        p = ++s;
        s = e;
        if (o_ids(p, (size_t)(s - p), v->build, &v->nbuild, 1))
            return -1;
    }
    return s == e ? 0 : -1;
}

static int o_cmp(const OVer* a, const OVer* b)
{
    int i;
    for (i = 0; i < 3; i++)
        if (a->t[i] != b->t[i])
            return a->t[i] < b->t[i] ? -1 : 1;
    if (!a->npre || !b->npre)
        return (a->npre == 0) - (b->npre == 0);
    for (i = 0; i < a->npre && i < b->npre; i++) {
        const OId *x = &a->pre[i], *y = &b->pre[i];
        if (x->num && y->num) {
            if (x->v != y->v)
                return x->v < y->v ? -1 : 1;
        } else if (x->num != y->num) {
            return x->num ? -1 : 1;
        } else {
            int c = strcmp(x->s, y->s);
            if (c)
                return c < 0 ? -1 : 1;
        }
    }
    return (a->npre > b->npre) - (a->npre < b->npre);
}

static int tcmp(const OVer* a, unsigned long long M, unsigned long long m, unsigned long long p)
{
    unsigned long long b[3] = { M, m, p };
    int i;
    for (i = 0; i < 3; i++)
        if (a->t[i] != b[i])
            return a->t[i] < b[i] ? -1 : 1;
    return 0;
}

static void o_fmt(const OVer* v, char* out)
{
    int i;
    out += sprintf(out, "%llu.%llu.%llu", v->t[0], v->t[1], v->t[2]);
    for (i = 0; i < v->npre; i++)
        out += sprintf(out, "%c%s", i ? '.' : '-', v->pre[i].s);
    for (i = 0; i < v->nbuild; i++)
        out += sprintf(out, "%c%s", i ? '.' : '+', v->build[i].s);
}

static const char* const PIDS[] = { "alpha", "beta", "0", "1", "2", "10", "rc", "a-b", "x", "X", "-", "9" };

static void rand_ver(OVer* v, int maxn)
{
    int i;
    memset(v, 0, sizeof(*v));
    for (i = 0; i < 3; i++)
        v->t[i] = rnd((unsigned)maxn + 1);
    if (rnd(3) == 0) {
        v->npre = 1 + (int)rnd(3);
        for (i = 0; i < v->npre; i++) {
            const char* s = PIDS[rnd(sizeof(PIDS) / sizeof(PIDS[0]))];
            strcpy(v->pre[i].s, s);
            v->pre[i].num = s[0] >= '0' && s[0] <= '9';
            v->pre[i].v = v->pre[i].num ? strtoull(s, NULL, 10) : 0;
        }
    }
    if (rnd(5) == 0) {
        v->nbuild = 1;
        strcpy(v->build[0].s, rnd(2) ? "001" : "sha.5");
    }
}

static int k_parse(const char* s, SemVer* v) { return semver_parse(s, strlen(s), v); }

static void t_versions(void)
{
    static const char* EDGE[] = {
        "1.2.3", "v1.2.3", " 1.2.3 ", "\t1.2.3\n", "\xC2\xA0" "1.2.3\xC2\xA0", "01.2.3", "1.02.3", "1.2.03", "1.2", "1.2.3.4",
        "1.2.3-", "1.2.3+", "1.2.3-a..b", "1.2.3-01", "1.2.3-0a", "1.2.3+01", "1.2.3-a+b+c", "1.2.3-a_b", "9007199254740991.0.0",
        "9007199254740992.0.0", "12345678901234567.0.0", "1.2.3-9007199254740992", "1.2.3-9007199254740991", "1.2.3-00000000000000000a",
        "vv1.2.3", "V1.2.3", "1.2.3-\xC3\xA9", "", "v", "-1.2.3", "1.2.3-a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x",
        "1.2.3-a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q.r.s.t.u.v.w.x.y", "0.0.0", "1.2.3+a.b-c.0001", "18446744073709551616.0.0", "1.18446744073709551617.0", "1.2.36893488147419103233",
    };
    static const char AL[] = "0123456789.-+avxZ \t";
    size_t i;
    int k;
    for (i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++) {
        SemVer a;
        OVer b;
        int r = k_parse(EDGE[i], &a), w = o_parse(EDGE[i], &b);
        CHECK((r == 0) == (w == 0), "parse '%s' got %d want %d", EDGE[i], r, w);
    }
    for (k = 0; k < 150000; k++) {
        OVer x, y, ox, oy;
        SemVer a, b;
        char s[600], t[600];
        int r, w, c;
        size_t n;
        rand_ver(&x, k & 1 ? 3 : 1000);
        o_fmt(&x, s);
        if (rnd(3) == 0) {
            n = strlen(s);
            switch (rnd(4)) {
            case 0:
                s[rnd((unsigned)n)] = AL[rnd(sizeof(AL) - 1)];
                break;
            case 1:
                memmove(s + 1, s, n + 1);
                s[0] = AL[rnd(sizeof(AL) - 1)];
                break;
            case 2:
                s[rnd((unsigned)n + 1)] = 0;
                break;
            default:
                s[n] = AL[rnd(sizeof(AL) - 1)];
                s[n + 1] = 0;
                break;
            }
        }
        r = k_parse(s, &a);
        w = o_parse(s, &ox);
        CHECK((r == 0) == (w == 0), "parse '%s' got %d want %d", s, r, w);
        if (r || w)
            continue;
        CHECK(a.major == ox.t[0] && a.minor == ox.t[1] && a.patch == ox.t[2] && a.n_pre == ox.npre && a.n_build == ox.nbuild, "fields '%s'", s);
        rand_ver(&y, 3);
        o_fmt(&y, t);
        if (k_parse(t, &b) || o_parse(t, &oy))
            continue;
        c = semver_compare(&a, &b);
        w = o_cmp(&ox, &oy);
        CHECK(c == w, "compare '%s' '%s' got %d want %d", s, t, c, w);
        CHECK(semver_compare(&b, &a) == -w && semver_compare(&a, &a) == 0, "antisym '%s' '%s'", s, t);
    }
}

typedef struct {
    int op, hy;
    int parts, wild[3], npre;
    unsigned long long n[3];
    OVer pv;
    int parts2, wild2[3], npre2;
    unsigned long long n2[3];
    OVer pv2;
} Prim;

enum { P_EQ, P_EQS, P_LT, P_LTE, P_GT, P_GTE, P_TIL, P_TILG, P_CAR, P_N };
static const char* const OPS[] = { "", "=", "<", "<=", ">", ">=", "~", "~>", "^" };

static void rand_partial(int* parts, int wild[3], unsigned long long n[3], int* npre, OVer* pv)
{
    int i;
    *parts = 1 + (int)rnd(3);
    if (rnd(8) == 0)
        *parts = 0;
    for (i = 0; i < 3; i++) {
        n[i] = rnd(4);
        wild[i] = i < *parts && rnd(6) == 0;
    }
    *npre = 0;
    memset(pv, 0, sizeof(*pv));
    if (*parts == 3 && rnd(3) == 0) {
        *npre = 1 + (int)rnd(2);
        for (i = 0; i < *npre; i++) {
            const char* s = PIDS[rnd(6)];
            strcpy(pv->pre[i].s, s);
            pv->pre[i].num = s[0] >= '0' && s[0] <= '9';
            pv->pre[i].v = pv->pre[i].num ? strtoull(s, NULL, 10) : 0;
        }
        pv->npre = *npre;
    }
}

static char* put_partial(char* o, int parts, const int wild[3], const unsigned long long n[3], const OVer* pv)
{
    static const char* W[] = { "x", "X", "*" };
    int i;
    if (parts == 0)
        return o + sprintf(o, "*");
    if (rnd(5) == 0)
        *o++ = 'v';
    for (i = 0; i < parts; i++) {
        if (i)
            *o++ = '.';
        if (wild[i])
            o += sprintf(o, "%s", W[rnd(3)]);
        else
            o += sprintf(o, "%llu", n[i]);
    }
    for (i = 0; i < pv->npre; i++)
        o += sprintf(o, "%c%s", i ? '.' : '-', pv->pre[i].s);
    return o;
}

typedef struct {
    int xM, xm, xp;
    unsigned long long M, m, p;
    const OVer* pre;
} X;

static X mkx(int parts, const int wild[3], const unsigned long long n[3], const OVer* pv)
{
    X x;
    x.xM = parts < 1 || wild[0];
    x.xm = x.xM || parts < 2 || wild[1];
    x.xp = x.xm || parts < 3 || wild[2];
    x.M = x.xM ? 0 : n[0];
    x.m = x.xm ? 0 : n[1];
    x.p = x.xp ? 0 : n[2];
    x.pre = (!x.xp && pv->npre) ? pv : NULL;
    return x;
}

static int ge_full(const OVer* v, const X* x)
{
    OVer b;
    memset(&b, 0, sizeof(b));
    b.t[0] = x->M;
    b.t[1] = x->m;
    b.t[2] = x->p;
    if (x->pre) {
        b.npre = x->pre->npre;
        memcpy(b.pre, x->pre->pre, sizeof(b.pre));
    }
    return o_cmp(v, &b);
}

static int lo(const OVer* v, const X* x)
{
    if (x->M == 0 && x->m == 0 && x->p == 0 && !x->pre)
        return 1;
    return ge_full(v, x) >= 0;
}

static int prim_sat(const OVer* v, const Prim* P)
{
    X x = mkx(P->parts, P->wild, P->n, &P->pv);
    if (P->hy) {
        X y = mkx(P->parts2, P->wild2, P->n2, &P->pv2);
        int ok = 1;
        if (!x.xM)
            ok &= lo(v, &x);
        if (y.xM) {
        } else if (y.xm)
            ok &= tcmp(v, y.M + 1, 0, 0) < 0;
        else if (y.xp)
            ok &= tcmp(v, y.M, y.m + 1, 0) < 0;
        else
            ok &= ge_full(v, &y) <= 0;
        return ok;
    }
    switch (P->op) {
    case P_CAR:
        if (x.xM)
            return 1;
        if (x.xm)
            return lo(v, &x) && tcmp(v, x.M + 1, 0, 0) < 0;
        if (x.xp)
            return lo(v, &x) && (x.M == 0 ? tcmp(v, 0, x.m + 1, 0) : tcmp(v, x.M + 1, 0, 0)) < 0;
        if (ge_full(v, &x) < 0)
            return 0;
        if (x.M)
            return tcmp(v, x.M + 1, 0, 0) < 0;
        if (x.m)
            return tcmp(v, 0, x.m + 1, 0) < 0;
        return tcmp(v, 0, 0, x.p + 1) < 0;
    case P_TIL:
    case P_TILG:
        if (x.xM)
            return 1;
        if (x.xm)
            return lo(v, &x) && tcmp(v, x.M + 1, 0, 0) < 0;
        return lo(v, &x) && tcmp(v, x.M, x.m + 1, 0) < 0;
    default:
        break;
    }
    if (!x.xp) {
        int c = ge_full(v, &x);
        switch (P->op) {
        case P_LT:
            return c < 0;
        case P_LTE:
            return c <= 0;
        case P_GT:
            return c > 0;
        case P_GTE:
            return lo(v, &x);
        default:
            return c == 0;
        }
    }
    if (x.xM)
        return !(P->op == P_LT || P->op == P_GT);
    switch (P->op) {
    case P_GT:
        return x.xm ? tcmp(v, x.M + 1, 0, 0) >= 0 && ge_full(v, &(X) { 0, 0, 0, x.M + 1, 0, 0, NULL }) >= 0
                    : ge_full(v, &(X) { 0, 0, 0, x.M, x.m + 1, 0, NULL }) >= 0;
    case P_LTE:
        return x.xm ? tcmp(v, x.M + 1, 0, 0) < 0 : tcmp(v, x.M, x.m + 1, 0) < 0;
    case P_GTE:
        return lo(v, &x);
    case P_LT:
        return tcmp(v, x.M, x.m, 0) < 0;
    default:
        return lo(v, &x) && (x.xm ? tcmp(v, x.M + 1, 0, 0) : tcmp(v, x.M, x.m + 1, 0)) < 0;
    }
}

static int pre_admit(const OVer* v, const X* x)
{
    return x->pre && tcmp(v, x->M, x->m, x->p) == 0;
}

static int set_sat(const OVer* v, const Prim* P, int np)
{
    int i, adm = v->npre == 0;
    for (i = 0; i < np; i++) {
        X x = mkx(P[i].parts, P[i].wild, P[i].n, &P[i].pv);
        if (!prim_sat(v, &P[i]))
            return 0;
        if (P[i].op == P_TIL || P[i].op == P_TILG) {
            if (!x.xp)
                adm |= pre_admit(v, &x);
        } else {
            adm |= pre_admit(v, &x);
        }
        if (P[i].hy) {
            X y = mkx(P[i].parts2, P[i].wild2, P[i].n2, &P[i].pv2);
            adm |= pre_admit(v, &y);
        }
    }
    return adm;
}

typedef struct {
    int nsets, np[3];
    Prim p[3][3];
    char src[400];
} ORange;

static void rand_range(ORange* R)
{
    char* o = R->src;
    int s, i;
    R->nsets = 1 + (int)rnd(3);
    for (s = 0; s < R->nsets; s++) {
        if (s)
            o += sprintf(o, rnd(2) ? " || " : "||");
        if (rnd(6) == 0) {
            Prim* P = &R->p[s][0];
            memset(P, 0, sizeof(*P));
            R->np[s] = 1;
            P->hy = 1;
            rand_partial(&P->parts, P->wild, P->n, &P->npre, &P->pv);
            rand_partial(&P->parts2, P->wild2, P->n2, &P->npre2, &P->pv2);
            o = put_partial(o, P->parts, P->wild, P->n, &P->pv);
            o += sprintf(o, rnd(2) ? " - " : "  -\t");
            o = put_partial(o, P->parts2, P->wild2, P->n2, &P->pv2);
            continue;
        }
        R->np[s] = (int)rnd(3) + (rnd(10) != 0);
        for (i = 0; i < R->np[s]; i++) {
            Prim* P = &R->p[s][i];
            memset(P, 0, sizeof(*P));
            P->op = (int)rnd(P_N);
            rand_partial(&P->parts, P->wild, P->n, &P->npre, &P->pv);
            if (i)
                *o++ = ' ';
            o += sprintf(o, "%s%s", OPS[P->op], rnd(6) == 0 ? " " : "");
            o = put_partial(o, P->parts, P->wild, P->n, &P->pv);
        }
    }
    *o = 0;
}

static int o_range(const OVer* v, const ORange* R)
{
    int s;
    for (s = 0; s < R->nsets; s++)
        if (set_sat(v, R->p[s], R->np[s]))
            return 1;
    return 0;
}

static void t_ranges(void)
{
    static OVer grid[4000];
    static SemVer kgrid[4000];
    static char gs[4000][64];
    int ng = 0, k, i;
    unsigned a, b, c;
    for (a = 0; a < 5; a++)
        for (b = 0; b < 5; b++)
            for (c = 0; c < 5; c++)
                for (i = 0; i < 6; i++) {
                    OVer* v = &grid[ng];
                    memset(v, 0, sizeof(*v));
                    v->t[0] = a;
                    v->t[1] = b;
                    v->t[2] = c;
                    if (i) {
                        const char* s = PIDS[i - 1];
                        v->npre = 1;
                        strcpy(v->pre[0].s, s);
                        v->pre[0].num = s[0] >= '0' && s[0] <= '9';
                        v->pre[0].v = v->pre[0].num ? strtoull(s, NULL, 10) : 0;
                    }
                    o_fmt(v, gs[ng]);
                    k_parse(gs[ng], &kgrid[ng]);
                    ng++;
                }
    for (k = 0; k < 6000; k++) {
        ORange R, R2;
        Range kr, kr2;
        int both = 0, got;
        rand_range(&R);
        got = semverk_parse_range(R.src, strlen(R.src), &kr);
        CHECK(got == 0, "range parse '%s'", R.src);
        if (got)
            continue;
        for (i = 0; i < ng; i++) {
            int w = o_range(&grid[i], &R), g = semverk_range_test(&kgrid[i], &kr);
            CHECK(g == w, "'%s' test %s got %d want %d", R.src, gs[i], g, w);
        }
        rand_range(&R2);
        if (semverk_parse_range(R2.src, strlen(R2.src), &kr2) == 0) {
            for (i = 0; i < ng && !both; i++)
                both = grid[i].npre == 0 && o_range(&grid[i], &R) && o_range(&grid[i], &R2);
            got = sv_range_intersects(&kr, &kr2);
            CHECK(!both || got, "intersects '%s' '%s'", R.src, R2.src);
            CHECK(got == sv_range_intersects(&kr2, &kr), "intersects symmetry '%s' '%s'", R.src, R2.src);
            sv_range_release(&kr2);
        }
        sv_range_release(&kr);
    }
    {
        static const char* BAD[] = { ">>1.2.3", "1.2.3 -", "~=1", "1.2.3-", "1.2.3 - ", "=<1", "^01.2", "1.2.3.4", "a.b.c", "1.2-pre", "<" };
        for (i = 0; i < (int)(sizeof(BAD) / sizeof(BAD[0])); i++) {
            Range r;
            int got = semverk_parse_range(BAD[i], strlen(BAD[i]), &r);
            CHECK(got != 0, "range reject '%s'", BAD[i]);
            if (!got)
                sv_range_release(&r);
        }
    }
}

static void t_sort(void)
{
    static SemVer sv[300];
    static OVer ov[300];
    static char s[300][64];
    uint32_t idx[300], tmp[300], oi[300];
    int k;
    for (k = 0; k < 400; k++) {
        uint32_t n = (uint32_t)rnd(300), i, j;
        for (i = 0; i < n; i++) {
            rand_ver(&ov[i], 2);
            o_fmt(&ov[i], s[i]);
            k_parse(s[i], &sv[i]);
            idx[i] = oi[i] = i;
        }
        for (i = 1; i < n; i++) {
            uint32_t x = oi[i];
            for (j = i; j > 0 && o_cmp(&ov[oi[j - 1]], &ov[x]) > 0; j--)
                oi[j] = oi[j - 1];
            oi[j] = x;
        }
        semverk_sort_indices(sv, idx, tmp, n);
        CHECK(n == 0 || memcmp(idx, oi, n * sizeof(uint32_t)) == 0, "stable sort n=%u", n);
    }
}

static void t_inc(void)
{
    static const char* const T[][4] = {
        { "1.2.3", "major", NULL, "2.0.0" }, { "1.2.3", "minor", NULL, "1.3.0" }, { "1.2.3", "patch", NULL, "1.2.4" },
        { "1.2.3-tag", "major", NULL, "2.0.0" }, { "1.2.0-0", "patch", NULL, "1.2.0" }, { "1.2.3-4", "major", NULL, "2.0.0" },
        { "1.2.3-4", "minor", NULL, "1.3.0" }, { "1.2.3-4", "patch", NULL, "1.2.3" }, { "1.2.3-alpha.0.beta", "major", NULL, "2.0.0" },
        { "1.2.3-alpha.0.beta", "minor", NULL, "1.3.0" }, { "1.2.3-alpha.0.beta", "patch", NULL, "1.2.3" },
        { "1.2.4", "prerelease", NULL, "1.2.5-0" }, { "1.2.3-0", "prerelease", NULL, "1.2.3-1" },
        { "1.2.3-alpha.0", "prerelease", NULL, "1.2.3-alpha.1" }, { "1.2.3-alpha.1", "prerelease", NULL, "1.2.3-alpha.2" },
        { "1.2.3-alpha.0.beta", "prerelease", NULL, "1.2.3-alpha.1.beta" },
        { "1.2.3-alpha.10.0.beta", "prerelease", NULL, "1.2.3-alpha.10.1.beta" },
        { "1.2.3-alpha.10.beta.0", "prerelease", NULL, "1.2.3-alpha.10.beta.1" },
        { "1.2.3-alpha.9.beta", "prerelease", NULL, "1.2.3-alpha.10.beta" }, { "1.2.0", "prepatch", NULL, "1.2.1-0" },
        { "1.2.0-1", "prepatch", NULL, "1.2.1-0" }, { "1.2.0", "preminor", NULL, "1.3.0-0" }, { "1.2.3-1", "preminor", NULL, "1.3.0-0" },
        { "1.2.0", "premajor", NULL, "2.0.0-0" }, { "1.2.3-1", "premajor", NULL, "2.0.0-0" }, { "1.2.0-1", "minor", NULL, "1.2.0" }, { "1.2.0", "minor", NULL, "1.3.0" }, { "1.0.0", "major", NULL, "2.0.0" },
        { "1.0.0-1", "major", NULL, "1.0.0" }, { "1.0.1-1", "major", NULL, "2.0.0" }, { "1.1.0-1", "major", NULL, "2.0.0" }, { "1.2.1-1", "minor", NULL, "1.3.0" }, { "1.2.3", "major", "dev", "2.0.0" }, { "1.2.3", "prerelease", "dev", "1.2.4-dev.0" },
        { "1.2.3-dev.0", "prerelease", "dev", "1.2.3-dev.1" }, { "1.2.3-0", "prerelease", "dev", "1.2.3-dev.0" },
        { "1.2.3-alpha.0", "prerelease", "dev", "1.2.3-dev.0" }, { "1.2.3-alpha.0.beta", "prerelease", "dev", "1.2.3-dev.0" },
        { "1.2.4-dev", "prerelease", "dev", "1.2.4-dev.0" }, { "1.2.0", "prepatch", "dev", "1.2.1-dev.0" },
        { "1.2.0-1", "prepatch", "dev", "1.2.1-dev.0" }, { "1.2.0", "preminor", "dev", "1.3.0-dev.0" },
        { "1.2.3-1", "preminor", "dev", "1.3.0-dev.0" }, { "1.2.0", "premajor", "dev", "2.0.0-dev.0" },
        { "1.2.3-1", "premajor", "dev", "2.0.0-dev.0" }, { "1.2.0-1", "minor", "dev", "1.2.0" }, { "1.0.0-1", "major", "dev", "1.0.0" },
        { "1.2.3-dev.bar", "prerelease", "dev", "1.2.3-dev.0" }, { "1.2.3-alpha", "release", NULL, "1.2.3" },
        { "1.2.3", "release", NULL, "1.2.3" }, { "1.2.3", "fake", NULL, NULL }, { "1.2.3", "Major", NULL, NULL },
        { "1.2.3", "majo", NULL, NULL }, { "1.2.3", "prereleases", NULL, NULL },
    };
    size_t i;
    for (i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        SemVer sv;
        IncVer iv;
        char out[128], *o = out;
        int rel = semverk_inc_release_id(T[i][1], strlen(T[i][1])), j;
        if (!T[i][3]) {
            CHECK(rel < 0, "release id '%s' rejected", T[i][1]);
            continue;
        }
        CHECK(rel >= 0 && k_parse(T[i][0], &sv) == 0, "inc setup %s %s", T[i][0], T[i][1]);
        if (rel < 0)
            continue;
        memset(&iv, 0, sizeof(iv));
        iv.major = sv.major;
        iv.minor = sv.minor;
        iv.patch = sv.patch;
        iv.n_pre = sv.n_pre;
        for (j = 0; j < sv.n_pre; j++) {
            iv.pre[j].is_num = sv.pre[j].numeric;
            iv.pre[j].num = sv.pre[j].numeric ? strtoull(sv.pre[j].s, NULL, 10) : 0;
            iv.pre[j].str = sv.pre[j].s;
            iv.pre[j].slen = sv.pre[j].len;
        }
        semverk_inc_apply(&iv, rel, T[i][2], T[i][2] ? strlen(T[i][2]) : 0);
        o += sprintf(o, "%llu.%llu.%llu", (unsigned long long)iv.major, (unsigned long long)iv.minor, (unsigned long long)iv.patch);
        for (j = 0; j < iv.n_pre; j++) {
            *o++ = j ? '.' : '-';
            if (iv.pre[j].is_num)
                o += sprintf(o, "%llu", (unsigned long long)iv.pre[j].num);
            else
                o += sprintf(o, "%.*s", (int)iv.pre[j].slen, iv.pre[j].str);
        }
        *o = 0;
        CHECK(strcmp(out, T[i][3]) == 0, "inc %s %s %s got %s want %s", T[i][0], T[i][1], T[i][2] ? T[i][2] : "-", out, T[i][3]);
    }
}

int main(void)
{
    t_versions();
    t_ranges();
    t_sort();
    t_inc();
    printf("%s test_semverk_core: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
