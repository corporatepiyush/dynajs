/*
 * dyna:semver -- semver 2.0.0 parsing, comparison, and range matching.
 *
 * `Range` is a compiled capability: parse once, match many. Measured crossover
 * is N=10 uses of one instance versus N free calls (5.9x at N=1000).
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc/free: the Range capability owns its compiled state */
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* JS Number.MAX_SAFE_INTEGER: the largest exact integer, and npm's cap on any
 * numeric field/identifier. A value above this makes parse fail. */
#define SV_MAX_SAFE 9007199254740991ULL

/* Capacity limits. A version/identifier list or range exceeding these is
 * rejected as invalid -- the bounds are far beyond any real-world input. */
#define SEMVER_MAX_IDS   24   /* prerelease or build identifiers per version */
#define SV_MAX_SETS      16   /* ||-separated comparator sets in a range */
#define SV_MAX_CMPS      24   /* comparators per set after expansion */

/* Synthetic "-0" prerelease used for exclusive upper bounds ("<2.0.0-0"). It is
 * a static const so it outlives every comparator that points at it. */
static const char SV_ZERO[] = "0";

/* One dot-separated identifier, as a borrowed span into a caller-owned string.
 * `numeric` is 1 iff the span is all-digits (a numeric identifier). */
typedef struct {
    const char *s;
    size_t len;
    int numeric;
} SemVerId;

/* A fully parsed version. Prerelease/build spans borrow the source string. */
typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
    int n_build;
    SemVerId build[SEMVER_MAX_IDS];
} SemVer;

/* ==================================================================== *
 *  scalar helpers                                                       *
 * ==================================================================== */

static int sv_is_digit(char c) { return c >= '0' && c <= '9'; }

static int sv_u64toa(uint64_t v, char *buf)   /* returns byte length, no NUL */
{
    char tmp[24];
    int k = 0, j;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < k; j++) buf[j] = tmp[k - 1 - j];
    return k;
}

/* Parse [s,s+n) as a strict numeric field (major/minor/patch or an x-range
 * number): no leading zero, <= 16 digits, value <= MAX_SAFE. 0 / -1. */
static int sv_parse_num(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (n == 0 || n > 16)
        return -1;
    if (n > 1 && s[0] == '0')
        return -1;
    for (i = 0; i < n; i++) {
        if (!sv_is_digit(s[i]))
            return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > SV_MAX_SAFE)
        return -1;
    *out = v;
    return 0;
}

/* Parse the dot-separated identifier list in [s,s+n) into ids[]. Every
 * identifier must be non-empty and drawn from [0-9A-Za-z-]. For a prerelease
 * (is_build==0) an all-digit identifier must have no leading zero. 0 / -1. */
static int sv_parse_ids(const char *s, size_t n, SemVerId *ids, int *pcount,
                        int is_build)
{
    size_t start = 0, i;
    int count = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            size_t len = i - start;
            const char *id = s + start;
            int alldig = 1;
            size_t k;
            if (len == 0)
                return -1;                       /* empty identifier */
            if (count >= SEMVER_MAX_IDS)
                return -1;
            for (k = 0; k < len; k++) {
                char c = id[k];
                if (sv_is_digit(c))
                    continue;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')
                    alldig = 0;
                else
                    return -1;                   /* invalid character */
            }
            if (!is_build && alldig && len > 1 && id[0] == '0')
                return -1;                       /* numeric id leading zero */
            ids[count].s = id;
            ids[count].len = len;
            ids[count].numeric = alldig;
            count++;
            start = i + 1;
        }
    }
    *pcount = count;
    return 0;
}

/* Strict SemVer 2.0.0 parse of the whole [s,s+n). 0 / -1. */
static int semver_parse(const char *s, size_t n, SemVer *out)
{
    size_t i = 0, start;
    memset(out, 0, sizeof(*out));

    start = i;
    while (i < n && sv_is_digit(s[i])) i++;
    if (sv_parse_num(s + start, i - start, &out->major)) return -1;
    if (i >= n || s[i] != '.') return -1;
    i++;

    start = i;
    while (i < n && sv_is_digit(s[i])) i++;
    if (sv_parse_num(s + start, i - start, &out->minor)) return -1;
    if (i >= n || s[i] != '.') return -1;
    i++;

    start = i;
    while (i < n && sv_is_digit(s[i])) i++;
    if (sv_parse_num(s + start, i - start, &out->patch)) return -1;

    if (i < n && s[i] == '-') {
        i++;
        start = i;
        while (i < n && s[i] != '+') i++;
        if (sv_parse_ids(s + start, i - start, out->pre, &out->n_pre, 0))
            return -1;
    }
    if (i < n && s[i] == '+') {
        i++;
        start = i;
        while (i < n) i++;
        if (sv_parse_ids(s + start, i - start, out->build, &out->n_build, 1))
            return -1;
    }
    return (i == n) ? 0 : -1;                     /* reject trailing garbage */
}

/* ==================================================================== *
 *  precedence comparison (SemVer 2.0.0 sec.11)                          *
 * ==================================================================== */

/* Compare two all-digit spans by numeric value, tolerant of leading zeros. */
static int sv_cmp_num(const char *a, size_t al, const char *b, size_t bl)
{
    int c;
    while (al > 1 && *a == '0') { a++; al--; }
    while (bl > 1 && *b == '0') { b++; bl--; }
    if (al != bl)
        return al < bl ? -1 : 1;
    c = memcmp(a, b, al);
    return (c > 0) - (c < 0);
}

static int sv_cmp_id(const SemVerId *a, const SemVerId *b)
{
    size_t m;
    int c;
    if (a->numeric && b->numeric)
        return sv_cmp_num(a->s, a->len, b->s, b->len);
    if (a->numeric) return -1;                    /* numeric < alphanumeric */
    if (b->numeric) return 1;
    m = a->len < b->len ? a->len : b->len;
    c = memcmp(a->s, b->s, m);
    if (c) return c < 0 ? -1 : 1;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    return 0;
}

static int sv_cmp_pre(const SemVerId *a, int an, const SemVerId *b, int bn)
{
    int i, m = an < bn ? an : bn;
    for (i = 0; i < m; i++) {
        int c = sv_cmp_id(&a[i], &b[i]);
        if (c) return c;
    }
    if (an != bn) return an < bn ? -1 : 1;        /* larger set is higher */
    return 0;
}

static int semver_compare(const SemVer *a, const SemVer *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    if (a->n_pre == 0 && b->n_pre == 0) return 0;
    if (a->n_pre == 0) return 1;                  /* release > prerelease */
    if (b->n_pre == 0) return -1;
    return sv_cmp_pre(a->pre, a->n_pre, b->pre, b->n_pre);
}

/* ==================================================================== *
 *  range grammar                                                        *
 * ==================================================================== */

enum { OP_LT, OP_LTE, OP_GT, OP_GTE, OP_EQ, OP_TILDE, OP_CARET };

/* A "partial" version from a comparator token: any of major/minor/patch may be
 * a wildcard (x, X, star, or missing). A higher wildcard forces lower ones. */
typedef struct {
    int xM, xm, xp;
    uint64_t M, m, p;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
} XRange;

/* A primitive comparator: an operator plus a concrete version (wildcards and
 * caret/tilde/hyphen/x-range shorthand have all been resolved away). */
typedef struct {
    int op;                                       /* OP_LT/LTE/GT/GTE/EQ */
    SemVer v;
} Comparator;

typedef struct {
    Comparator c[SV_MAX_CMPS];
    int n;
} CompSet;

typedef struct {
    CompSet s[SV_MAX_SETS];
    int n;
} Range;

/* Append a resolved comparator. When add_zero, the version gets a synthetic
 * "-0" prerelease (an exclusive upper bound that also excludes prereleases at
 * that tuple); such a bound carries no other prerelease. 0 / -1 (set full). */
static int sv_add_cmp(CompSet *set, int op, uint64_t M, uint64_t m, uint64_t p,
                      const SemVerId *pre, int npre, int add_zero)
{
    Comparator *c;
    if (set->n >= SV_MAX_CMPS)
        return -1;
    c = &set->c[set->n++];
    memset(&c->v, 0, sizeof(c->v));
    c->op = op;
    c->v.major = M;
    c->v.minor = m;
    c->v.patch = p;
    if (add_zero) {
        c->v.pre[0].s = SV_ZERO;
        c->v.pre[0].len = 1;
        c->v.pre[0].numeric = 1;
        c->v.n_pre = 1;
    } else if (npre > 0) {
        int i;
        if (npre > SEMVER_MAX_IDS)
            return -1;
        for (i = 0; i < npre; i++)
            c->v.pre[i] = pre[i];
        c->v.n_pre = npre;
    }
    return 0;
}

/* Parse one comparator-token version into an XRange (a leading 'v' is allowed).
 * Handles wildcards, partial versions, and an optional prerelease/build (a
 * prerelease/build tail is only legal when a patch token is present). 0 / -1. */
static int parse_partial(const char *s, size_t n, XRange *out)
{
    size_t i = 0;
    int present[3] = {0, 0, 0}, wild[3] = {0, 0, 0};
    uint64_t num[3] = {0, 0, 0};
    int t;

    memset(out, 0, sizeof(*out));
    if (n > 0 && s[0] == 'v') { s++; n--; }
    if (n == 0) { out->xM = out->xm = out->xp = 1; return 0; }

    for (t = 0; t < 3; t++) {
        size_t start, tl;
        const char *tk;
        if (t > 0) {
            if (i < n && s[i] == '.') i++;
            else break;
        }
        start = i;
        while (i < n && s[i] != '.' && s[i] != '-' && s[i] != '+') i++;
        tl = i - start;
        tk = s + start;
        present[t] = 1;
        if (tl == 0)
            return -1;                            /* empty token ("1." etc) */
        if (tl == 1 && (tk[0] == 'x' || tk[0] == 'X' || tk[0] == '*'))
            wild[t] = 1;
        else if (sv_parse_num(tk, tl, &num[t]))
            return -1;
    }

    out->xM = !present[0] || wild[0];
    out->xm = out->xM || !present[1] || wild[1];
    out->xp = out->xm || !present[2] || wild[2];
    out->M = out->xM ? 0 : num[0];
    out->m = (present[1] && !wild[1]) ? num[1] : 0;
    out->p = (present[2] && !wild[2]) ? num[2] : 0;

    if (i < n && (s[i] == '-' || s[i] == '+')) {
        int concrete = !(out->xM || out->xm || out->xp);
        if (!present[2])
            return -1;                            /* pre/build needs a patch */
        if (s[i] == '-') {
            SemVerId tmp[SEMVER_MAX_IDS];
            int cnt;
            size_t start;
            i++;
            start = i;
            while (i < n && s[i] != '+') i++;
            if (sv_parse_ids(s + start, i - start, tmp, &cnt, 0))
                return -1;
            if (concrete) {
                int k;
                for (k = 0; k < cnt; k++) out->pre[k] = tmp[k];
                out->n_pre = cnt;
            }
        }
        if (i < n && s[i] == '+') {
            SemVerId tmp[SEMVER_MAX_IDS];
            int cnt;
            size_t start;
            i++;
            start = i;
            while (i < n) i++;
            if (sv_parse_ids(s + start, i - start, tmp, &cnt, 1))
                return -1;
        }
    }
    return (i == n) ? 0 : -1;
}

/* ^x.y.z (compatible with, leftmost nonzero fixed). See README expansions. */
static int expand_caret(CompSet *set, const XRange *x)
{
    if (x->xM)
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);   /* any */
    if (x->xm) {
        if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0)) return -1;
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    }
    if (x->xp) {
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, 0, NULL, 0, 0)) return -1;
        if (x->M == 0)
            return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    }
    if (sv_add_cmp(set, OP_GTE, x->M, x->m, x->p, x->pre, x->n_pre, 0))
        return -1;
    if (x->M != 0)
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    if (x->m != 0)
        return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
    return sv_add_cmp(set, OP_LT, x->M, x->m, x->p + 1, NULL, 0, 1);
}

/* ~x.y.z (patch-level, or minor-level when no minor is given). */
static int expand_tilde(CompSet *set, const XRange *x)
{
    if (x->xM)
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);   /* any */
    if (x->xm) {
        if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0)) return -1;
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    }
    {
        uint64_t p = x->xp ? 0 : x->p;
        const SemVerId *pre = x->xp ? NULL : x->pre;
        int npre = x->xp ? 0 : x->n_pre;
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, p, pre, npre, 0)) return -1;
        return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
    }
}

/* A plain/operator comparator, including x-range wildcard expansion. */
static int expand_op(CompSet *set, int op, const XRange *x)
{
    int anyX = x->xM || x->xm || x->xp;
    uint64_t M, m;

    if (!anyX)
        return sv_add_cmp(set, op, x->M, x->m, x->p, x->pre, x->n_pre, 0);

    if (x->xM) {
        if (op == OP_GT || op == OP_LT)
            return sv_add_cmp(set, OP_LT, 0, 0, 0, NULL, 0, 1);   /* nothing */
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);      /* any */
    }

    if (op == OP_EQ) {
        if (x->xm) {
            if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0)) return -1;
            return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
        }
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, 0, NULL, 0, 0)) return -1;
        return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
    }

    M = x->M;
    m = x->xm ? 0 : x->m;
    switch (op) {
    case OP_GT:
        if (x->xm) return sv_add_cmp(set, OP_GTE, M + 1, 0, 0, NULL, 0, 0);
        return sv_add_cmp(set, OP_GTE, M, m + 1, 0, NULL, 0, 0);
    case OP_LTE:
        if (x->xm) return sv_add_cmp(set, OP_LT, M + 1, 0, 0, NULL, 0, 1);
        return sv_add_cmp(set, OP_LT, M, m + 1, 0, NULL, 0, 1);
    case OP_GTE:
        return sv_add_cmp(set, OP_GTE, M, m, 0, NULL, 0, 0);
    case OP_LT:
        return sv_add_cmp(set, OP_LT, M, m, 0, NULL, 0, 1);
    }
    return -1;
}

/* "from - to" hyphen range. */
static int expand_hyphen(CompSet *set, const XRange *from, const XRange *to)
{
    if (from->xM) {
        /* no lower bound */
    } else if (from->xm) {
        if (sv_add_cmp(set, OP_GTE, from->M, 0, 0, NULL, 0, 0)) return -1;
    } else if (from->xp) {
        if (sv_add_cmp(set, OP_GTE, from->M, from->m, 0, NULL, 0, 0)) return -1;
    } else {
        if (sv_add_cmp(set, OP_GTE, from->M, from->m, from->p,
                       from->pre, from->n_pre, 0)) return -1;
    }

    if (to->xM) {
        /* no upper bound */
    } else if (to->xm) {
        if (sv_add_cmp(set, OP_LT, to->M + 1, 0, 0, NULL, 0, 1)) return -1;
    } else if (to->xp) {
        if (sv_add_cmp(set, OP_LT, to->M, to->m + 1, 0, NULL, 0, 1)) return -1;
    } else if (to->n_pre > 0) {
        if (sv_add_cmp(set, OP_LTE, to->M, to->m, to->p,
                       to->pre, to->n_pre, 0)) return -1;
    } else {
        if (sv_add_cmp(set, OP_LTE, to->M, to->m, to->p, NULL, 0, 0)) return -1;
    }
    return 0;
}

static int sv_is_space(char c) { return c == ' ' || c == '\t'; }

/* Parse one ||-separated comparator set. 0 / -1. */
static int parse_set(const char *s, size_t n, CompSet *set)
{
    size_t i, k;

    while (n > 0 && sv_is_space(s[0])) { s++; n--; }
    while (n > 0 && sv_is_space(s[n - 1])) n--;
    if (n == 0)
        return 0;                                 /* empty -> caller adds any */

    /* hyphen range: the first '-' flanked by whitespace on both sides */
    for (k = 1; k + 1 < n; k++) {
        if (s[k] == '-' && sv_is_space(s[k - 1]) && sv_is_space(s[k + 1])) {
            const char *fs = s;
            size_t fl = k, ts = k + 1, tl;
            XRange xf, xt;
            while (fl > 0 && sv_is_space(fs[fl - 1])) fl--;
            while (ts < n && sv_is_space(s[ts])) ts++;
            tl = n - ts;
            if (parse_partial(fs, fl, &xf)) return -1;
            if (parse_partial(s + ts, tl, &xt)) return -1;
            return expand_hyphen(set, &xf, &xt);
        }
    }

    i = 0;
    while (i < n) {
        int op;
        size_t opstart, ol, vstart, vl;
        const char *o;
        XRange x;
        int r;

        while (i < n && sv_is_space(s[i])) i++;
        if (i >= n) break;

        opstart = i;
        while (i < n && (s[i] == '<' || s[i] == '>' || s[i] == '=' ||
                         s[i] == '~' || s[i] == '^')) i++;
        ol = i - opstart;
        o = s + opstart;
        if (ol == 0) op = OP_EQ;
        else if (ol == 1 && o[0] == '=') op = OP_EQ;
        else if (ol == 1 && o[0] == '>') op = OP_GT;
        else if (ol == 1 && o[0] == '<') op = OP_LT;
        else if (ol == 1 && o[0] == '~') op = OP_TILDE;
        else if (ol == 1 && o[0] == '^') op = OP_CARET;
        else if (ol == 2 && o[0] == '>' && o[1] == '=') op = OP_GTE;
        else if (ol == 2 && o[0] == '<' && o[1] == '=') op = OP_LTE;
        else if (ol == 2 && o[0] == '~' && o[1] == '>') op = OP_TILDE;
        else return -1;                           /* unknown operator */

        while (i < n && sv_is_space(s[i])) i++;
        vstart = i;
        while (i < n && !sv_is_space(s[i])) i++;
        vl = i - vstart;
        if (vl == 0)
            return -1;                            /* operator without version */
        if (parse_partial(s + vstart, vl, &x))
            return -1;

        if (op == OP_CARET) r = expand_caret(set, &x);
        else if (op == OP_TILDE) r = expand_tilde(set, &x);
        else r = expand_op(set, op, &x);
        if (r) return -1;
    }
    return 0;
}

/* Parse the whole range string (||-split) into a Range. 0 / -1. */
static int parse_range(const char *s, size_t n, Range *out)
{
    size_t i = 0;
    out->n = 0;
    for (;;) {
        size_t j = i, setlen, next;
        CompSet *set;
        while (j + 1 < n && !(s[j] == '|' && s[j + 1] == '|')) j++;
        if (j + 1 < n && s[j] == '|' && s[j + 1] == '|') {
            setlen = j - i;
            next = j + 2;
        } else {
            setlen = n - i;
            next = n + 1;                          /* stop sentinel */
        }
        if (out->n >= SV_MAX_SETS)
            return -1;
        set = &out->s[out->n];
        set->n = 0;
        if (parse_set(s + i, setlen, set))
            return -1;
        if (set->n == 0 &&
            sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0))
            return -1;                            /* empty set matches all */
        out->n++;
        if (next > n) break;
        i = next;
    }
    return 0;
}

static int cmp_test(const SemVer *v, const Comparator *c)
{
    int r = semver_compare(v, &c->v);
    switch (c->op) {
    case OP_LT:  return r < 0;
    case OP_LTE: return r <= 0;
    case OP_GT:  return r > 0;
    case OP_GTE: return r >= 0;
    case OP_EQ:  return r == 0;
    }
    return 0;
}

static int set_test(const SemVer *v, const CompSet *set)
{
    int i;
    for (i = 0; i < set->n; i++)
        if (!cmp_test(v, &set->c[i]))
            return 0;
    if (v->n_pre > 0) {
        /* a prerelease version needs a same-tuple prerelease comparator */
        for (i = 0; i < set->n; i++) {
            const SemVer *cv = &set->c[i].v;
            if (cv->n_pre > 0 && cv->major == v->major &&
                cv->minor == v->minor && cv->patch == v->patch)
                return 1;
        }
        return 0;
    }
    return 1;
}

static int range_test(const SemVer *v, const Range *rg)
{
    int i;
    for (i = 0; i < rg->n; i++)
        if (set_test(v, &rg->s[i]))
            return 1;
    return 0;
}

/* ==================================================================== *
 *  version-string builders                                              *
 * ==================================================================== */

/* major.minor.patch(-prerelease), no build metadata. Fresh JS string or OOM. */
static JSValue sv_build_version(JSContext *ctx, const SemVer *v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char *buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++) cap += v->pre[i].len + 1;
    buf = js_malloc(ctx, cap);
    if (!buf) return JS_EXCEPTION;
    o += sv_u64toa(v->major, buf + o); buf[o++] = '.';
    o += sv_u64toa(v->minor, buf + o); buf[o++] = '.';
    o += sv_u64toa(v->patch, buf + o);
    if (v->n_pre > 0) {
        buf[o++] = '-';
        for (i = 0; i < v->n_pre; i++) {
            if (i) buf[o++] = '.';
            memcpy(buf + o, v->pre[i].s, v->pre[i].len);
            o += v->pre[i].len;
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return r;
}

/* A numeric prerelease identifier as a JS Number (exact when it fits). */
static JSValue sv_id_number(JSContext *ctx, const SemVerId *id)
{
    size_t k;
    if (id->len <= 18) {
        uint64_t x = 0;
        for (k = 0; k < id->len; k++) x = x * 10 + (uint64_t)(id->s[k] - '0');
        return JS_NewInt64(ctx, (int64_t)x);
    }
    {
        double d = 0;
        for (k = 0; k < id->len; k++) d = d * 10 + (double)(id->s[k] - '0');
        return JS_NewFloat64(ctx, d);
    }
}

/* Fresh array of prerelease identifiers (numbers for numeric, else strings). */
static JSValue sv_pre_array(JSContext *ctx, const SemVer *v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr)) return arr;
    for (i = 0; i < v->n_pre; i++) {
        JSValue item = v->pre[i].numeric
            ? sv_id_number(ctx, &v->pre[i])
            : JS_NewStringLen(ctx, v->pre[i].s, v->pre[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* Fresh array of build identifiers (always strings). */
static JSValue sv_build_array(JSContext *ctx, const SemVer *v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr)) return arr;
    for (i = 0; i < v->n_build; i++) {
        JSValue item = JS_NewStringLen(ctx, v->build[i].s, v->build[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* ==================================================================== *
 *  JS boundary: single-version helpers                                  *
 * ==================================================================== */

static const char *sv_arg_str(JSContext *ctx, int argc, JSValueConst *argv,
                              size_t *plen)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:semver: argument must be a string");
        return NULL;
    }
    return JS_ToCStringLen(ctx, plen, argv[0]);
}

/* parse(v) -> object | throw */
static JSValue dyn_semver_parse(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    SemVer v;
    JSValue obj, ver, pre, build;
    int err;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s) return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JSValue e = JS_ThrowTypeError(ctx, "dyna:semver: invalid version \"%.*s\"",
                                      (int)(n > 80 ? 80 : n), s);
        JS_FreeCString(ctx, s);   /* `s` is IN the error text: free after it */
        return e;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) { JS_FreeCString(ctx, s); return obj; }
    ver = sv_build_version(ctx, &v);
    pre = JS_IsException(ver) ? JS_EXCEPTION : sv_pre_array(ctx, &v);
    build = JS_IsException(pre) ? JS_EXCEPTION : sv_build_array(ctx, &v);
    JS_FreeCString(ctx, s);
    if (JS_IsException(ver) || JS_IsException(pre) || JS_IsException(build)) {
        JS_FreeValue(ctx, ver);
        JS_FreeValue(ctx, pre);
        JS_FreeValue(ctx, build);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    err  = JS_DefinePropertyValueStr(ctx, obj, "major",
                                     JS_NewInt64(ctx, (int64_t)v.major),
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "minor",
                                     JS_NewInt64(ctx, (int64_t)v.minor),
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "patch",
                                     JS_NewInt64(ctx, (int64_t)v.patch),
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "prerelease", pre,
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "build", build,
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "version", ver,
                                     JS_PROP_C_W_E) < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* isValid(v) -> bool (never throws) */
static JSValue dyn_semver_is_valid(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    SemVer v;
    int ok;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewBool(ctx, 0);
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NewBool(ctx, 0);
    }
    ok = semver_parse(s, n, &v) == 0;
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* clean(v) -> normalized string | null (invalid) ; throws on non-string */
static JSValue dyn_semver_clean(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *s, *p;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s) return JS_EXCEPTION;
    p = s;
    while (n > 0 && (p[0] == ' ' || p[0] == '\t' || p[0] == '\n' ||
                     p[0] == '\r')) { p++; n--; }
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' ||
                     p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    while (n > 0 && (p[0] == '=' || p[0] == 'v')) { p++; n--; }
    r = (semver_parse(p, n, &v) == 0) ? sv_build_version(ctx, &v) : JS_NULL;
    JS_FreeCString(ctx, s);
    return r;
}

enum { FLD_MAJOR, FLD_MINOR, FLD_PATCH, FLD_PRERELEASE };

/* major/minor/patch(v) -> number ; prerelease(v) -> array. magic selects. */
static JSValue dyn_semver_field(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    const char *s;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s) return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
    }
    switch (magic) {
    case FLD_MAJOR: r = JS_NewInt64(ctx, (int64_t)v.major); break;
    case FLD_MINOR: r = JS_NewInt64(ctx, (int64_t)v.minor); break;
    case FLD_PATCH: r = JS_NewInt64(ctx, (int64_t)v.patch); break;
    default:        r = sv_pre_array(ctx, &v); break;
    }
    JS_FreeCString(ctx, s);
    return r;
}

/* ==================================================================== *
 *  JS boundary: comparison                                              *
 * ==================================================================== */

/* Coerce+parse two version args; write the precedence result. 0 / -1 (throws). */
static int sv_two(JSContext *ctx, int argc, JSValueConst *argv, int *cmp)
{
    const char *as, *bs;
    size_t an, bn;
    SemVer a, b;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "dyna:semver: expected two version strings");
        return -1;
    }
    as = JS_ToCStringLen(ctx, &an, argv[0]);
    if (!as) return -1;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) { JS_FreeCString(ctx, as); return -1; }
    if (semver_parse(as, an, &a) || semver_parse(bs, bn, &b)) {
        JS_FreeCString(ctx, as);
        JS_FreeCString(ctx, bs);
        JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
        return -1;
    }
    *cmp = semver_compare(&a, &b);
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return 0;
}

/* compare(a,b) -> -1|0|1 */
static JSValue dyn_semver_compare(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int c;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c)) return JS_EXCEPTION;
    return JS_NewInt32(ctx, c);
}

enum { CMP_GT, CMP_GTE, CMP_LT, CMP_LTE, CMP_EQ, CMP_NEQ };

/* gt/gte/lt/lte/eq/neq(a,b) -> bool */
static JSValue dyn_semver_cmpbool(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    int c, r;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c)) return JS_EXCEPTION;
    switch (magic) {
    case CMP_GT:  r = c > 0;  break;
    case CMP_GTE: r = c >= 0; break;
    case CMP_LT:  r = c < 0;  break;
    case CMP_LTE: r = c <= 0; break;
    case CMP_EQ:  r = c == 0; break;
    default:      r = c != 0; break;
    }
    return JS_NewBool(ctx, r);
}

/* ==================================================================== *
 *  JS boundary: sort / maxSatisfying / minSatisfying                    *
 * ==================================================================== */

/* Read a JS Array of version strings into parallel owned arrays: `elems` (the
 * original values, for output), `cstrs` (owned C strings the SemVers borrow),
 * and `svs` (parsed). On any error frees everything filled so far and throws.
 * On success the caller owns all three and must free via sv_free_versions. */
static int sv_read_versions(JSContext *ctx, JSValueConst arr, JSValue **pelems,
                            const char ***pcstrs, SemVer **psvs, uint32_t *pcount)
{
    JSValue lval, *elems = NULL;
    const char **cstrs = NULL;
    SemVer *svs = NULL;
    uint32_t len, i, filled = 0;
    int isarr = JS_IsArray(ctx, arr);

    if (isarr < 0) return -1;
    if (!isarr) {
        JS_ThrowTypeError(ctx, "dyna:semver: expected an Array of versions");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lval)) return -1;
    if (JS_ToUint32(ctx, &len, lval)) { JS_FreeValue(ctx, lval); return -1; }
    JS_FreeValue(ctx, lval);

    if (len > 0 && (size_t)len > ((size_t)-1) / sizeof(SemVer)) {
        JS_ThrowRangeError(ctx, "dyna:semver: array too large");
        return -1;
    }
    if (len > 0) {
        elems = js_malloc(ctx, (size_t)len * sizeof(*elems));
        cstrs = js_malloc(ctx, (size_t)len * sizeof(*cstrs));
        svs = js_malloc(ctx, (size_t)len * sizeof(*svs));
        if (!elems || !cstrs || !svs) {
            js_free(ctx, elems); js_free(ctx, cstrs); js_free(ctx, svs);
            return -1;
        }
    }

    for (i = 0; i < len; i++) {
        size_t slen;
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e)) goto fail;
        elems[i] = e;
        cstrs[i] = NULL;
        filled = i + 1;
        if (!JS_IsString(e)) {
            JS_ThrowTypeError(ctx,
                "dyna:semver: every array element must be a string");
            goto fail;
        }
        cstrs[i] = JS_ToCStringLen(ctx, &slen, e);
        if (!cstrs[i]) goto fail;
        if (semver_parse(cstrs[i], slen, &svs[i])) {
            JS_ThrowTypeError(ctx, "dyna:semver: invalid version in array");
            goto fail;
        }
    }
    *pelems = elems; *pcstrs = cstrs; *psvs = svs; *pcount = len;
    return 0;

 fail:
    for (i = 0; i < filled; i++) {
        if (cstrs && cstrs[i]) JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems); js_free(ctx, cstrs); js_free(ctx, svs);
    return -1;
}

static void sv_free_versions(JSContext *ctx, JSValue *elems,
                             const char **cstrs, SemVer *svs, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (cstrs && cstrs[i]) JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems);
    js_free(ctx, cstrs);
    js_free(ctx, svs);
}

/* Stable bottom-up merge sort of index array by ascending precedence. */
static void sv_sort_indices(const SemVer *svs, uint32_t *idx, uint32_t *tmp,
                            uint32_t n)
{
    uint32_t width;
    for (width = 1; width < n; width *= 2) {
        uint32_t i;
        for (i = 0; i < n; i += 2 * width) {
            uint32_t lo = i;
            uint32_t mid = (i + width < n) ? i + width : n;
            uint32_t hi = (i + 2 * width < n) ? i + 2 * width : n;
            uint32_t l = lo, r = mid, k = lo;
            while (l < mid && r < hi) {
                if (semver_compare(&svs[idx[l]], &svs[idx[r]]) <= 0)
                    tmp[k++] = idx[l++];
                else
                    tmp[k++] = idx[r++];
            }
            while (l < mid) tmp[k++] = idx[l++];
            while (r < hi)  tmp[k++] = idx[r++];
        }
        memcpy(idx, tmp, (size_t)n * sizeof(uint32_t));
    }
}

/* sort(arr) -> new array sorted by ascending precedence */
static JSValue dyn_semver_sort(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue *elems, out;
    const char **cstrs;
    SemVer *svs;
    uint32_t count, *idx, *tmp, i;

    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: sort expects an Array of versions");
    }
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count))
        return JS_EXCEPTION;

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    if (count == 0) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return out;
    }
    idx = js_malloc(ctx, (size_t)count * sizeof(*idx));
    tmp = js_malloc(ctx, (size_t)count * sizeof(*tmp));
    if (!idx || !tmp) {
        js_free(ctx, idx); js_free(ctx, tmp);
        JS_FreeValue(ctx, out);
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) idx[i] = i;
    sv_sort_indices(svs, idx, tmp, count);

    for (i = 0; i < count; i++) {
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                                         JS_DupValue(ctx, elems[idx[i]]),
                                         JS_PROP_C_W_E) < 0) {
            js_free(ctx, idx); js_free(ctx, tmp);
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
    }
    js_free(ctx, idx);
    js_free(ctx, tmp);
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

/* maxSatisfying / minSatisfying(versions, range) -> string | null */
static JSValue dyn_semver_bestsat(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int want_max)
{
    JSValue *elems, out;
    const char **cstrs;
    SemVer *svs;
    const char *rs;
    size_t rn;
    Range *rg;
    uint32_t count, i;
    long best = -1;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: expected (Array, rangeString)");
    }
    /* coerce ALL args to C locals before any parsing */
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs) return JS_EXCEPTION;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count)) {
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    rg = js_malloc(ctx, sizeof(*rg));
    if (!rg) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    if (parse_range(rs, rn, rg)) {
        js_free(ctx, rg);
        sv_free_versions(ctx, elems, cstrs, svs, count);
        JS_FreeCString(ctx, rs);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid range");
    }

    for (i = 0; i < count; i++) {
        if (!range_test(&svs[i], rg))
            continue;
        if (best < 0) { best = (long)i; continue; }
        {
            int c = semver_compare(&svs[i], &svs[best]);
            if (want_max ? (c > 0) : (c < 0))
                best = (long)i;
        }
    }
    out = (best < 0) ? JS_NULL : JS_DupValue(ctx, elems[best]);

    js_free(ctx, rg);
    sv_free_versions(ctx, elems, cstrs, svs, count);
    JS_FreeCString(ctx, rs);
    return out;
}

/* ==================================================================== *
 *  JS boundary: satisfies                                               *
 * ==================================================================== */

static JSValue dyn_semver_satisfies(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *vs, *rs;
    size_t vn, rn;
    SemVer v;
    Range *rg;
    int res;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: satisfies(version, range) expects two strings");
    }
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs) return JS_EXCEPTION;
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs) { JS_FreeCString(ctx, vs); return JS_EXCEPTION; }

    if (semver_parse(vs, vn, &v)) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
    }
    rg = js_malloc(ctx, sizeof(*rg));
    if (!rg) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    if (parse_range(rs, rn, rg)) {
        js_free(ctx, rg);
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid range");
    }
    res = range_test(&v, rg);
    js_free(ctx, rg);
    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rs);
    return JS_NewBool(ctx, res);
}

/* ==================================================================== *
 *  JS boundary: inc                                                     *
 * ==================================================================== */

/* A mutable increment identifier: either a numeric value or a borrowed span. */
typedef struct {
    int is_num;
    uint64_t num;
    const char *str;
    size_t slen;
} IncId;

typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    IncId pre[SEMVER_MAX_IDS + 2];
} IncVer;

enum { REL_MAJOR, REL_MINOR, REL_PATCH,
       REL_PREMAJOR, REL_PREMINOR, REL_PREPATCH, REL_PRERELEASE };

/* compareIdentifiers(pre0, id): numeric ids sort below alphanumeric ones. */
static int inc_cmp_id0(const IncId *a, const char *id, size_t idn)
{
    int id_num = idn > 0;
    size_t k, m;
    int c;
    for (k = 0; k < idn; k++)
        if (!sv_is_digit(id[k])) { id_num = 0; break; }
    if (a->is_num && id_num) {
        char tmp[24];
        int tl = sv_u64toa(a->num, tmp);
        return sv_cmp_num(tmp, (size_t)tl, id, idn);
    }
    if (a->is_num) return -1;
    if (id_num) return 1;
    m = a->slen < idn ? a->slen : idn;
    c = memcmp(a->str, id, m);
    if (c) return c < 0 ? -1 : 1;
    if (a->slen != idn) return a->slen < idn ? -1 : 1;
    return 0;
}

/* Bump the prerelease (npm SemVer.inc 'pre' step, identifierBase '0'). */
static void inc_pre_step(IncVer *v, const char *id, size_t idn)
{
    if (v->n_pre == 0) {
        v->pre[0].is_num = 1;
        v->pre[0].num = 0;
        v->n_pre = 1;
    } else {
        int i = v->n_pre, found = 0;
        while (--i >= 0) {
            if (v->pre[i].is_num) { v->pre[i].num++; found = 1; break; }
        }
        if (!found && v->n_pre < (int)countof(v->pre)) {
            v->pre[v->n_pre].is_num = 1;
            v->pre[v->n_pre].num = 0;
            v->n_pre++;
        }
    }
    if (id) {
        int replace = 1;
        if (inc_cmp_id0(&v->pre[0], id, idn) == 0 &&
            v->n_pre >= 2 && v->pre[1].is_num)
            replace = 0;
        if (replace) {
            v->pre[0].is_num = 0;
            v->pre[0].str = id;
            v->pre[0].slen = idn;
            v->pre[1].is_num = 1;
            v->pre[1].num = 0;
            v->n_pre = 2;
        }
    }
}

static void inc_apply(IncVer *v, int rel, const char *id, size_t idn)
{
    switch (rel) {
    case REL_MAJOR:
        if (v->minor != 0 || v->patch != 0 || v->n_pre == 0) v->major++;
        v->minor = 0; v->patch = 0; v->n_pre = 0;
        break;
    case REL_MINOR:
        if (v->patch != 0 || v->n_pre == 0) v->minor++;
        v->patch = 0; v->n_pre = 0;
        break;
    case REL_PATCH:
        if (v->n_pre == 0) v->patch++;
        v->n_pre = 0;
        break;
    case REL_PREMAJOR:
        v->n_pre = 0; v->patch = 0; v->minor = 0; v->major++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREMINOR:
        v->n_pre = 0; v->patch = 0; v->minor++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREPATCH:
        v->n_pre = 0; v->patch++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PRERELEASE:
        if (v->n_pre == 0) v->patch++;
        inc_pre_step(v, id, idn);
        break;
    }
}

/* Format an IncVer as major.minor.patch(-prerelease). Fresh JS string or OOM. */
static JSValue inc_format(JSContext *ctx, const IncVer *v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char *buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++)
        cap += (v->pre[i].is_num ? 20 : v->pre[i].slen) + 1;
    buf = js_malloc(ctx, cap);
    if (!buf) return JS_EXCEPTION;
    o += sv_u64toa(v->major, buf + o); buf[o++] = '.';
    o += sv_u64toa(v->minor, buf + o); buf[o++] = '.';
    o += sv_u64toa(v->patch, buf + o);
    if (v->n_pre > 0) {
        buf[o++] = '-';
        for (i = 0; i < v->n_pre; i++) {
            if (i) buf[o++] = '.';
            if (v->pre[i].is_num) {
                o += sv_u64toa(v->pre[i].num, buf + o);
            } else {
                memcpy(buf + o, v->pre[i].str, v->pre[i].slen);
                o += v->pre[i].slen;
            }
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return r;
}

static int inc_release_id(const char *s, size_t n)
{
    static const struct { const char *name; int id; } m[] = {
        {"major", REL_MAJOR}, {"minor", REL_MINOR}, {"patch", REL_PATCH},
        {"premajor", REL_PREMAJOR}, {"preminor", REL_PREMINOR},
        {"prepatch", REL_PREPATCH}, {"prerelease", REL_PRERELEASE},
    };
    size_t i;
    for (i = 0; i < countof(m); i++)
        if (strlen(m[i].name) == n && memcmp(m[i].name, s, n) == 0)
            return m[i].id;
    return -1;
}

/* inc(v, release[, identifier]) -> new version string | throw */
static JSValue dyn_semver_inc(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *vs, *rel_s, *id = NULL;
    size_t vn, rel_n, idn = 0;
    SemVer sv;
    IncVer iv;
    int rel, i;
    JSValue r;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: inc(version, release[, identifier]) expects strings");
    }
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsString(argv[2])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: inc identifier must be a string");
    }
    /* coerce all JS args to C locals first */
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs) return JS_EXCEPTION;
    rel_s = JS_ToCStringLen(ctx, &rel_n, argv[1]);
    if (!rel_s) { JS_FreeCString(ctx, vs); return JS_EXCEPTION; }
    if (argc >= 3 && JS_IsString(argv[2])) {
        id = JS_ToCStringLen(ctx, &idn, argv[2]);
        if (!id) { JS_FreeCString(ctx, vs); JS_FreeCString(ctx, rel_s);
                   return JS_EXCEPTION; }
    }

    rel = inc_release_id(rel_s, rel_n);
    if (rel < 0 || semver_parse(vs, vn, &sv)) {
        JSValue e = JS_ThrowTypeError(ctx, rel < 0
            ? "dyna:semver: invalid release type"
            : "dyna:semver: invalid version");
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id) JS_FreeCString(ctx, id);
        return e;
    }

    memset(&iv, 0, sizeof(iv));
    iv.major = sv.major; iv.minor = sv.minor; iv.patch = sv.patch;
    iv.n_pre = sv.n_pre;
    for (i = 0; i < sv.n_pre; i++) {
        if (sv.pre[i].numeric) {
            uint64_t x = 0;
            size_t k;
            for (k = 0; k < sv.pre[i].len && k < 18; k++)
                x = x * 10 + (uint64_t)(sv.pre[i].s[k] - '0');
            iv.pre[i].is_num = 1;
            iv.pre[i].num = x;
        } else {
            iv.pre[i].is_num = 0;
            iv.pre[i].str = sv.pre[i].s;
            iv.pre[i].slen = sv.pre[i].len;
        }
    }
    inc_apply(&iv, rel, id, idn);
    r = inc_format(ctx, &iv);

    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rel_s);
    if (id) JS_FreeCString(ctx, id);
    return r;
}

/* ==================================================================== *
 *  JS boundary: coerce                                                  *
 * ==================================================================== */

static JSValue dyn_semver_coerce(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *s;
    size_t n, i;
    JSValue result = JS_NULL;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s) return JS_EXCEPTION;

    for (i = 0; i < n; i++) {
        size_t j, k;
        int hm = 0, hp = 0;
        size_t ms = 0, ml = 0, ps = 0, pl = 0;
        char buf[64];
        size_t o = 0;
        SemVer sv;

        if (!sv_is_digit(s[i]) || (i > 0 && sv_is_digit(s[i - 1])))
            continue;                             /* not a boundary digit */
        j = i;
        while (j < n && sv_is_digit(s[j])) j++;
        if (j - i > 16) { i = j - 1; continue; }  /* over-long: skip run */

        k = j;
        if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
            size_t a = k + 1, b = a;
            while (b < n && sv_is_digit(s[b])) b++;
            if (b - a <= 16) {
                hm = 1; ms = a; ml = b - a; k = b;
                if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
                    size_t c = k + 1, d = c;
                    while (d < n && sv_is_digit(s[d])) d++;
                    if (d - c <= 16) { hp = 1; ps = c; pl = d - c; }
                }
            }
        }

        memcpy(buf + o, s + i, j - i); o += j - i; buf[o++] = '.';
        if (hm) { memcpy(buf + o, s + ms, ml); o += ml; } else buf[o++] = '0';
        buf[o++] = '.';
        if (hp) { memcpy(buf + o, s + ps, pl); o += pl; } else buf[o++] = '0';

        if (semver_parse(buf, o, &sv) == 0)
            result = JS_NewStringLen(ctx, buf, o);
        break;                                    /* commit to first candidate */
    }
    JS_FreeCString(ctx, s);
    return result;
}

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

/* ==================================================================== *
 *  class Range -- a COMPILED range: parse once, test many versions      *
 * ==================================================================== *
 *
 * The free satisfies()/maxSatisfying()/minSatisfying() js_malloc a Range,
 * parse_range() it and free it again on EVERY call. A dependency solver asks
 * "does this version satisfy this range?" in an inner loop over thousands of
 * versions, so that parse is pure repeated work.
 *
 * new Range(">=1.2.3 <2") parses once; .test(v) is then a comparison against
 * the compiled comparator sets. Same relationship as new RegExp to a literal
 * match. A plain GC object: the compiled state is a fixed-size POD struct plus
 * the source string, so there is nothing scarce to close.
 *
 * The compiled state is READ-ONLY, so a Range is freely reusable and needs no
 * busy flag -- unlike Hasher, no method mutates it.
 */

typedef struct {
    Range rg;
    char *source;   /* the range as given, for .source */
} dyn_range_t;

static JSClassID dyn_range_class_id;

static void dyn_range_dispose(void *native)
{
    dyn_range_t *r = (dyn_range_t *)native;
    if (!r)
        return;
    free(r->source);
    free(r);
}

static void dyn_range_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_range_dispose(JS_GetOpaque(val, dyn_range_class_id));
}

static const JSClassDef dyn_range_class = {
    "Range",
    .finalizer = dyn_range_finalizer,
};

static JSValue dyn_range_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    dyn_range_t *r;

    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Range(rangeString) requires a range");
    /* coerce first: nothing is allocated yet, so a throwing coercion strands
     * nothing. */
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    r = (dyn_range_t *)malloc(sizeof(*r));
    if (!r) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (parse_range(s, n, &r->rg)) {
        free(r);
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid range");
    }
    r->source = (char *)malloc(n + 1);
    if (!r->source) {
        free(r);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(r->source, s, n);
    r->source[n] = '\0';
    JS_FreeCString(ctx, s);

    return dyn_plain_wrap(ctx, dyn_range_class_id, r, dyn_range_dispose);
}

/* .test(version) -> boolean. An unparseable version is `false`, not a throw:
 * "does this satisfy" has a defensible answer for junk, and a solver filtering
 * a list should not have to pre-validate every entry. */
static JSValue dyn_range_test(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *vs;
    size_t vn;
    SemVer v;
    dyn_range_t *r;
    int ok;

    (void)argc;
    /* coerce the argument to C locals BEFORE resolving the native handle */
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    r = (dyn_range_t *)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }
    ok = semver_parse(vs, vn, &v) == 0 && range_test(&v, &r->rg);
    JS_FreeCString(ctx, vs);
    return JS_NewBool(ctx, ok);
}

/* magic: 1 = maxSatisfying, 0 = minSatisfying. Returns the ORIGINAL array
 * element (not a re-serialised version), matching the free functions. */
static JSValue dyn_range_bestsat(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int want_max)
{
    JSValue *elems = NULL, out;
    const char **cstrs = NULL;
    SemVer *svs = NULL;
    dyn_range_t *r;
    uint32_t count, i;
    long best = -1;

    (void)argc;
    /* the array read runs user JS (length/index getters), so it happens before
     * the handle is resolved */
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count))
        return JS_EXCEPTION;
    r = (dyn_range_t *)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) {
        if (!range_test(&svs[i], &r->rg))
            continue;
        if (best < 0) { best = (long)i; continue; }
        {
            int c = semver_compare(&svs[i], &svs[best]);
            if (want_max ? (c > 0) : (c < 0))
                best = (long)i;
        }
    }
    out = (best < 0) ? JS_NULL : JS_DupValue(ctx, elems[best]);
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

/* .filter(versions) -> every satisfying element, input order preserved. */
static JSValue dyn_range_filter(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue *elems = NULL, out;
    const char **cstrs = NULL;
    SemVer *svs = NULL;
    dyn_range_t *r;
    uint32_t count, i, o = 0;

    (void)argc;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count))
        return JS_EXCEPTION;
    r = (dyn_range_t *)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) {
        if (!range_test(&svs[i], &r->rg))
            continue;
        if (JS_DefinePropertyValueUint32(ctx, out, o++,
                                         JS_DupValue(ctx, elems[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
    }
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

static JSValue dyn_range_get_source(JSContext *ctx, JSValueConst this_val)
{
    dyn_range_t *r = (dyn_range_t *)dyn_plain_get(ctx, this_val,
                                                  dyn_range_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewString(ctx, r->source);
}

/* Number of ||-separated comparator sets the range compiled to. Exposed
 * because it is the one observable measure of compilation having happened. */
static JSValue dyn_range_get_set_count(JSContext *ctx, JSValueConst this_val)
{
    dyn_range_t *r = (dyn_range_t *)dyn_plain_get(ctx, this_val,
                                                  dyn_range_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, r->rg.n);
}

static const JSCFunctionListEntry dyn_range_proto[] = {
    JS_CFUNC_DEF("test", 1, dyn_range_test),
    JS_CFUNC_MAGIC_DEF("maxSatisfying", 1, dyn_range_bestsat, 1),
    JS_CFUNC_MAGIC_DEF("minSatisfying", 1, dyn_range_bestsat, 0),
    JS_CFUNC_DEF("filter", 1, dyn_range_filter),
    JS_CGETSET_DEF("source", dyn_range_get_source, NULL),
    JS_CGETSET_DEF("setCount", dyn_range_get_set_count, NULL),
};

static const JSCFunctionListEntry dyn_semver_funcs[] = {
    JS_CFUNC_DEF("parse", 1, dyn_semver_parse),
    JS_CFUNC_DEF("isValid", 1, dyn_semver_is_valid),
    JS_CFUNC_DEF("clean", 1, dyn_semver_clean),
    JS_CFUNC_DEF("compare", 2, dyn_semver_compare),
    JS_CFUNC_MAGIC_DEF("gt", 2, dyn_semver_cmpbool, CMP_GT),
    JS_CFUNC_MAGIC_DEF("gte", 2, dyn_semver_cmpbool, CMP_GTE),
    JS_CFUNC_MAGIC_DEF("lt", 2, dyn_semver_cmpbool, CMP_LT),
    JS_CFUNC_MAGIC_DEF("lte", 2, dyn_semver_cmpbool, CMP_LTE),
    JS_CFUNC_MAGIC_DEF("eq", 2, dyn_semver_cmpbool, CMP_EQ),
    JS_CFUNC_MAGIC_DEF("neq", 2, dyn_semver_cmpbool, CMP_NEQ),
    JS_CFUNC_DEF("sort", 1, dyn_semver_sort),
    JS_CFUNC_MAGIC_DEF("major", 1, dyn_semver_field, FLD_MAJOR),
    JS_CFUNC_MAGIC_DEF("minor", 1, dyn_semver_field, FLD_MINOR),
    JS_CFUNC_MAGIC_DEF("patch", 1, dyn_semver_field, FLD_PATCH),
    JS_CFUNC_MAGIC_DEF("prerelease", 1, dyn_semver_field, FLD_PRERELEASE),
    JS_CFUNC_DEF("inc", 2, dyn_semver_inc),
    JS_CFUNC_DEF("satisfies", 2, dyn_semver_satisfies),
    JS_CFUNC_MAGIC_DEF("maxSatisfying", 2, dyn_semver_bestsat, 1),
    JS_CFUNC_MAGIC_DEF("minSatisfying", 2, dyn_semver_bestsat, 0),
    JS_CFUNC_DEF("coerce", 1, dyn_semver_coerce),
};

static int dyn_semver_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_range_class_id, &dyn_range_class,
                                 dyn_range_proto, countof(dyn_range_proto),
                                 dyn_range_ctor, "Range") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_semver_funcs,
                                  countof(dyn_semver_funcs));
}

int js_nat_init_semver(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:semver", dyn_semver_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Range");
    return JS_AddModuleExportList(ctx, m, dyn_semver_funcs,
                                  countof(dyn_semver_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SEMVER */
