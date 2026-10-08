#include "dyna-nat.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define SV_MAX_SAFE 9007199254740991ULL

#define SEMVER_MAX_IDS 24
#define SV_MAX_SETS 16
#define SV_MAX_CMPS 24

static const char SV_ZERO[] = "0";

typedef struct {
    const char* s;
    size_t len;
    int numeric;
} SemVerId;

typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
    int n_build;
    SemVerId build[SEMVER_MAX_IDS];
} SemVer;

static int sv_is_digit(char c) { return c >= '0' && c <= '9'; }

static const unsigned char SV_WS1[256] = {
    [0x09] = 1,
    [0x0A] = 1,
    [0x0B] = 1,
    [0x0C] = 1,
    [0x0D] = 1,
    [0x20] = 1,
    [0xC2] = 1,
    [0xE1] = 1,
    [0xE2] = 1,
    [0xE3] = 1,
    [0xEF] = 1,
};

static size_t sv_ws_char(const char* s, size_t n)
{
    unsigned char c;
    if (n == 0)
        return 0;
    c = (unsigned char)s[0];
    if (!SV_WS1[c])
        return 0;
    if (c <= 0x20)
        return 1;
    if (c == 0xC2 && n >= 2 && (unsigned char)s[1] == 0xA0)
        return 2;
    if (c == 0xE1 && n >= 3 && (unsigned char)s[1] == 0x9A && (unsigned char)s[2] == 0x80)
        return 3;
    if (c == 0xE2 && n >= 3 && (unsigned char)s[1] == 0x80) {
        unsigned char d = (unsigned char)s[2];
        if (d >= 0x80 && d <= 0x8A)
            return 3;
        if (d == 0xA8 || d == 0xA9 || d == 0xAF)
            return 3;
    }
    if (c == 0xE2 && n >= 3 && (unsigned char)s[1] == 0x81 && (unsigned char)s[2] == 0x9F)
        return 3;
    if (c == 0xE3 && n >= 3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80)
        return 3;
    if (c == 0xEF && n >= 3 && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        return 3;
    return 0;
}

static size_t sv_ws_lead(const char* s, size_t n)
{
    size_t k, lead = 0;
    while ((k = sv_ws_char(s + lead, n - lead)) != 0)
        lead += k;
    return lead;
}

static size_t sv_ws_trail(const char* s, size_t n)
{
    size_t trail = 0;
    for (;;) {
        size_t k, clen = 0;
        if (n == 0)
            break;
        for (k = 1; k <= 3 && k <= n; k++) {
            clen = sv_ws_char(s + n - k, k);
            if (clen == k)
                break;
            clen = 0;
        }
        if (clen == 0)
            break;
        n -= clen;
        trail += clen;
    }
    return trail;
}

static int sv_u64toa(uint64_t v, char* buf)
{
    return (int)u64toa(buf, v);
}

static int sv_parse_num(const char* s, size_t n, uint64_t* out)
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

static int sv_parse_ids(const char* s, size_t n, SemVerId* ids, int* pcount,
    int is_build)
{
    size_t start = 0, i;
    int count = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            size_t len = i - start;
            const char* id = s + start;
            int alldig = 1;
            size_t k;
            if (len == 0)
                return -1;
            if (count >= SEMVER_MAX_IDS)
                return -1;
            for (k = 0; k < len; k++) {
                char c = id[k];
                if (sv_is_digit(c))
                    continue;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')
                    alldig = 0;
                else
                    return -1;
            }
            if (!is_build && alldig && len > 1 && id[0] == '0')
                return -1;
            if (!is_build && alldig && len >= 16) {
                uint64_t v = 0;
                for (k = 0; k < len; k++) {
                    uint64_t d = (uint64_t)(id[k] - '0');
                    if (v > (UINT64_MAX - d) / 10)
                        return -1;
                    v = v * 10 + d;
                    if (v > SV_MAX_SAFE)
                        return -1;
                }
            }
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

static int semver_parse(const char* s, size_t n, SemVer* out)
{
    size_t i = 0, start;
    memset(out, 0, sizeof(*out));

    {
        size_t lead = sv_ws_lead(s, n), trail;
        s += lead;
        n -= lead;
        trail = sv_ws_trail(s, n);
        n -= trail;
    }
    if (n > 0 && s[0] == 'v') {
        s++;
        n--;
    }
    start = i;
    while (i < n && sv_is_digit(s[i]))
        i++;
    if (sv_parse_num(s + start, i - start, &out->major))
        return -1;
    if (i >= n || s[i] != '.')
        return -1;
    i++;

    start = i;
    while (i < n && sv_is_digit(s[i]))
        i++;
    if (sv_parse_num(s + start, i - start, &out->minor))
        return -1;
    if (i >= n || s[i] != '.')
        return -1;
    i++;

    start = i;
    while (i < n && sv_is_digit(s[i]))
        i++;
    if (sv_parse_num(s + start, i - start, &out->patch))
        return -1;

    if (i < n && s[i] == '-') {
        i++;
        start = i;
        while (i < n && s[i] != '+')
            i++;
        if (sv_parse_ids(s + start, i - start, out->pre, &out->n_pre, 0))
            return -1;
    }
    if (i < n && s[i] == '+') {
        i++;
        start = i;
        while (i < n)
            i++;
        if (sv_parse_ids(s + start, i - start, out->build, &out->n_build, 1))
            return -1;
    }
    return (i == n) ? 0 : -1;
}

static int sv_cmp_num(const char* a, size_t al, const char* b, size_t bl)
{
    int c;
    while (al > 1 && *a == '0') {
        a++;
        al--;
    }
    while (bl > 1 && *b == '0') {
        b++;
        bl--;
    }
    if (al != bl)
        return al < bl ? -1 : 1;
    c = memcmp(a, b, al);
    return (c > 0) - (c < 0);
}

static int sv_cmp_id(const SemVerId* a, const SemVerId* b)
{
    size_t m;
    int c;
    if (a->numeric && b->numeric)
        return sv_cmp_num(a->s, a->len, b->s, b->len);
    if (a->numeric)
        return -1;
    if (b->numeric)
        return 1;
    m = a->len < b->len ? a->len : b->len;
    c = memcmp(a->s, b->s, m);
    if (c)
        return c < 0 ? -1 : 1;
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return 0;
}

static int sv_cmp_pre(const SemVerId* a, int an, const SemVerId* b, int bn)
{
    int i, m = an < bn ? an : bn;
    for (i = 0; i < m; i++) {
        int c = sv_cmp_id(&a[i], &b[i]);
        if (c)
            return c;
    }
    if (an != bn)
        return an < bn ? -1 : 1;
    return 0;
}

static int semver_compare(const SemVer* a, const SemVer* b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;
    if (a->n_pre == 0 && b->n_pre == 0)
        return 0;
    if (a->n_pre == 0)
        return 1;
    if (b->n_pre == 0)
        return -1;
    return sv_cmp_pre(a->pre, a->n_pre, b->pre, b->n_pre);
}

enum { OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,
    OP_EQ,
    OP_TILDE,
    OP_CARET };

typedef struct {
    int xM, xm, xp;
    uint64_t M, m, p;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
} XRange;

typedef struct {
    int op;
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

static int sv_add_cmp(CompSet* set, int op, uint64_t M, uint64_t m, uint64_t p,
    const SemVerId* pre, int npre, int add_zero)
{
    Comparator* c;
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

static int parse_partial(const char* s, size_t n, XRange* out)
{
    size_t i = 0;
    int present[3] = { 0, 0, 0 }, wild[3] = { 0, 0, 0 };
    uint64_t num[3] = { 0, 0, 0 };
    int t;

    memset(out, 0, sizeof(*out));
    if (n > 0 && s[0] == 'v') {
        s++;
        n--;
    }
    if (n == 0) {
        out->xM = out->xm = out->xp = 1;
        return 0;
    }

    for (t = 0; t < 3; t++) {
        size_t start, tl;
        const char* tk;
        if (t > 0) {
            if (i < n && s[i] == '.')
                i++;
            else
                break;
        }
        start = i;
        while (i < n && s[i] != '.' && s[i] != '-' && s[i] != '+')
            i++;
        tl = i - start;
        tk = s + start;
        present[t] = 1;
        if (tl == 0)
            return -1;
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
            return -1;
        if (s[i] == '-') {
            SemVerId tmp[SEMVER_MAX_IDS];
            int cnt;
            size_t start;
            i++;
            start = i;
            while (i < n && s[i] != '+')
                i++;
            if (sv_parse_ids(s + start, i - start, tmp, &cnt, 0))
                return -1;
            if (concrete) {
                int k;
                for (k = 0; k < cnt; k++)
                    out->pre[k] = tmp[k];
                out->n_pre = cnt;
            }
        }
        if (i < n && s[i] == '+') {
            SemVerId tmp[SEMVER_MAX_IDS];
            int cnt;
            size_t start;
            i++;
            start = i;
            while (i < n)
                i++;
            if (sv_parse_ids(s + start, i - start, tmp, &cnt, 1))
                return -1;
        }
    }
    return (i == n) ? 0 : -1;
}

static int expand_caret(CompSet* set, const XRange* x)
{
    if (x->xM)
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);
    if (x->xm) {
        if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0))
            return -1;
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    }
    if (x->xp) {
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, 0, NULL, 0, 0))
            return -1;
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

static int expand_tilde(CompSet* set, const XRange* x)
{
    if (x->xM)
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);
    if (x->xm) {
        if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0))
            return -1;
        return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
    }
    {
        uint64_t p = x->xp ? 0 : x->p;
        const SemVerId* pre = x->xp ? NULL : x->pre;
        int npre = x->xp ? 0 : x->n_pre;
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, p, pre, npre, 0))
            return -1;
        return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
    }
}

static int expand_op(CompSet* set, int op, const XRange* x)
{
    int anyX = x->xM || x->xm || x->xp;
    uint64_t M, m;

    if (!anyX)
        return sv_add_cmp(set, op, x->M, x->m, x->p, x->pre, x->n_pre, 0);

    if (x->xM) {
        if (op == OP_GT || op == OP_LT)
            return sv_add_cmp(set, OP_LT, 0, 0, 0, NULL, 0, 1);
        return sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0);
    }

    if (op == OP_EQ) {
        if (x->xm) {
            if (sv_add_cmp(set, OP_GTE, x->M, 0, 0, NULL, 0, 0))
                return -1;
            return sv_add_cmp(set, OP_LT, x->M + 1, 0, 0, NULL, 0, 1);
        }
        if (sv_add_cmp(set, OP_GTE, x->M, x->m, 0, NULL, 0, 0))
            return -1;
        return sv_add_cmp(set, OP_LT, x->M, x->m + 1, 0, NULL, 0, 1);
    }

    M = x->M;
    m = x->xm ? 0 : x->m;
    switch (op) {
    case OP_GT:
        if (x->xm)
            return sv_add_cmp(set, OP_GTE, M + 1, 0, 0, NULL, 0, 0);
        return sv_add_cmp(set, OP_GTE, M, m + 1, 0, NULL, 0, 0);
    case OP_LTE:
        if (x->xm)
            return sv_add_cmp(set, OP_LT, M + 1, 0, 0, NULL, 0, 1);
        return sv_add_cmp(set, OP_LT, M, m + 1, 0, NULL, 0, 1);
    case OP_GTE:
        return sv_add_cmp(set, OP_GTE, M, m, 0, NULL, 0, 0);
    case OP_LT:
        return sv_add_cmp(set, OP_LT, M, m, 0, NULL, 0, 1);
    }
    return -1;
}

static int expand_hyphen(CompSet* set, const XRange* from, const XRange* to)
{
    if (from->xM) {
    } else if (from->xm) {
        if (sv_add_cmp(set, OP_GTE, from->M, 0, 0, NULL, 0, 0))
            return -1;
    } else if (from->xp) {
        if (sv_add_cmp(set, OP_GTE, from->M, from->m, 0, NULL, 0, 0))
            return -1;
    } else {
        if (sv_add_cmp(set, OP_GTE, from->M, from->m, from->p,
                from->pre, from->n_pre, 0))
            return -1;
    }

    if (to->xM) {
    } else if (to->xm) {
        if (sv_add_cmp(set, OP_LT, to->M + 1, 0, 0, NULL, 0, 1))
            return -1;
    } else if (to->xp) {
        if (sv_add_cmp(set, OP_LT, to->M, to->m + 1, 0, NULL, 0, 1))
            return -1;
    } else if (to->n_pre > 0) {
        if (sv_add_cmp(set, OP_LTE, to->M, to->m, to->p,
                to->pre, to->n_pre, 0))
            return -1;
    } else {
        if (sv_add_cmp(set, OP_LTE, to->M, to->m, to->p, NULL, 0, 0))
            return -1;
    }
    return 0;
}

static int sv_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v'
        || c == '\f';
}

static int parse_set(const char* s, size_t n, CompSet* set)
{
    size_t i, k;

    while (n > 0 && sv_is_space(s[0])) {
        s++;
        n--;
    }
    while (n > 0 && sv_is_space(s[n - 1]))
        n--;
    if (n == 0)
        return 0;

    for (k = 1; k + 1 < n; k++) {
        if (s[k] == '-' && sv_is_space(s[k - 1]) && sv_is_space(s[k + 1])) {
            const char* fs = s;
            size_t fl = k, ts = k + 1, tl;
            XRange xf, xt;
            while (fl > 0 && sv_is_space(fs[fl - 1]))
                fl--;
            while (ts < n && sv_is_space(s[ts]))
                ts++;
            tl = n - ts;
            if (parse_partial(fs, fl, &xf))
                return -1;
            if (parse_partial(s + ts, tl, &xt))
                return -1;
            return expand_hyphen(set, &xf, &xt);
        }
    }

    i = 0;
    while (i < n) {
        int op;
        size_t opstart, ol, vstart, vl;
        const char* o;
        XRange x;
        int r;

        while (i < n && sv_is_space(s[i]))
            i++;
        if (i >= n)
            break;

        opstart = i;
        while (i < n && (s[i] == '<' || s[i] == '>' || s[i] == '=' || s[i] == '~' || s[i] == '^'))
            i++;
        ol = i - opstart;
        o = s + opstart;
        if (ol == 0)
            op = OP_EQ;
        else if (ol == 1 && o[0] == '=')
            op = OP_EQ;
        else if (ol == 1 && o[0] == '>')
            op = OP_GT;
        else if (ol == 1 && o[0] == '<')
            op = OP_LT;
        else if (ol == 1 && o[0] == '~')
            op = OP_TILDE;
        else if (ol == 1 && o[0] == '^')
            op = OP_CARET;
        else if (ol == 2 && o[0] == '>' && o[1] == '=')
            op = OP_GTE;
        else if (ol == 2 && o[0] == '<' && o[1] == '=')
            op = OP_LTE;
        else if (ol == 2 && o[0] == '~' && o[1] == '>')
            op = OP_TILDE;
        else
            return -1;

        while (i < n && sv_is_space(s[i]))
            i++;
        vstart = i;
        while (i < n && !sv_is_space(s[i]))
            i++;
        vl = i - vstart;
        if (vl == 0)
            return -1;
        if (parse_partial(s + vstart, vl, &x))
            return -1;

        if (op == OP_CARET)
            r = expand_caret(set, &x);
        else if (op == OP_TILDE)
            r = expand_tilde(set, &x);
        else
            r = expand_op(set, op, &x);
        if (r)
            return -1;
    }
    return 0;
}

static int parse_range(const char* s, size_t n, Range* out)
{
    size_t i = 0;
    out->n = 0;
    for (;;) {
        size_t j = i, setlen, next;
        CompSet* set;
        while (j + 1 < n && !(s[j] == '|' && s[j + 1] == '|'))
            j++;
        if (j + 1 < n && s[j] == '|' && s[j + 1] == '|') {
            setlen = j - i;
            next = j + 2;
        } else {
            setlen = n - i;
            next = n + 1;
        }
        if (out->n >= SV_MAX_SETS)
            return -1;
        set = &out->s[out->n];
        set->n = 0;
        if (parse_set(s + i, setlen, set))
            return -1;
        if (set->n == 0 && sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0))
            return -1;
        out->n++;
        if (next > n)
            break;
        i = next;
    }
    return 0;
}

static int cmp_test(const SemVer* v, const Comparator* c)
{
    int r = semver_compare(v, &c->v);
    switch (c->op) {
    case OP_LT:
        return r < 0;
    case OP_LTE:
        return r <= 0;
    case OP_GT:
        return r > 0;
    case OP_GTE:
        return r >= 0;
    case OP_EQ:
        return r == 0;
    }
    return 0;
}

static int set_test(const SemVer* v, const CompSet* set)
{
    int i;
    for (i = 0; i < set->n; i++)
        if (!cmp_test(v, &set->c[i]))
            return 0;
    if (v->n_pre > 0) {
        for (i = 0; i < set->n; i++) {
            const SemVer* cv = &set->c[i].v;
            if (cv->n_pre > 0 && cv->major == v->major && cv->minor == v->minor && cv->patch == v->patch)
                return 1;
        }
        return 0;
    }
    return 1;
}

static int range_test(const SemVer* v, const Range* rg)
{
    int i;
    if (v->n_pre < 0)
        return 0;
    for (i = 0; i < rg->n; i++)
        if (set_test(v, &rg->s[i]))
            return 1;
    return 0;
}

static JSValue sv_build_version(JSContext* ctx, const SemVer* v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char* buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++)
        cap += v->pre[i].len + 1;
    buf = js_malloc(ctx, cap);
    if (!buf)
        return JS_EXCEPTION;
    o += sv_u64toa(v->major, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->minor, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->patch, buf + o);
    if (v->n_pre > 0) {
        buf[o++] = '-';
        for (i = 0; i < v->n_pre; i++) {
            if (i)
                buf[o++] = '.';
            memcpy(buf + o, v->pre[i].s, v->pre[i].len);
            o += v->pre[i].len;
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return r;
}

static JSValue sv_id_number(JSContext* ctx, const SemVerId* id)
{
    size_t k;
    if (id->len <= 18) {
        uint64_t x = 0;
        for (k = 0; k < id->len; k++)
            x = x * 10 + (uint64_t)(id->s[k] - '0');
        if (x < SV_MAX_SAFE)
            return JS_NewInt64(ctx, (int64_t)x);
        return JS_NewStringLen(ctx, id->s, id->len);
    }
    {
        double d = 0;
        for (k = 0; k < id->len; k++)
            d = d * 10 + (double)(id->s[k] - '0');
        return JS_NewFloat64(ctx, d);
    }
}

static JSValue sv_pre_array(JSContext* ctx, const SemVer* v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < v->n_pre; i++) {
        JSValue item = v->pre[i].numeric
            ? sv_id_number(ctx, &v->pre[i])
            : JS_NewStringLen(ctx, v->pre[i].s, v->pre[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue sv_build_array(JSContext* ctx, const SemVer* v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < v->n_build; i++) {
        JSValue item = JS_NewStringLen(ctx, v->build[i].s, v->build[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static const char* sv_arg_str(JSContext* ctx, int argc, JSValueConst* argv,
    size_t* plen)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:semver: argument must be a string");
        return NULL;
    }
    return JS_ToCStringLen(ctx, plen, argv[0]);
}

static JSValue dyn_semver_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    SemVer v;
    JSValue obj, ver, pre, build;
    int err;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JSValue e = JS_ThrowTypeError(ctx, "dyna:semver: invalid version \"%.*s\"",
            (int)(n > 80 ? 80 : n), s);
        JS_FreeCString(ctx, s);
        return e;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, s);
        return obj;
    }
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
    err = JS_DefinePropertyValueStr(ctx, obj, "major",
              JS_NewInt64(ctx, (int64_t)v.major),
              JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "minor",
               JS_NewInt64(ctx, (int64_t)v.minor),
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "patch",
               JS_NewInt64(ctx, (int64_t)v.patch),
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "prerelease", pre,
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "build", build,
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "version", ver,
               JS_PROP_C_W_E)
        < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue dyn_semver_is_valid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
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

static JSValue dyn_semver_clean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *s, *p;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    {
        size_t lead = sv_ws_lead(s, n), trail;
        p = s + lead;
        n -= lead;
        trail = sv_ws_trail(p, n);
        n -= trail;
    }
    while (n > 0 && (p[0] == '=' || p[0] == 'v')) {
        p++;
        n--;
    }
    r = (semver_parse(p, n, &v) == 0) ? sv_build_version(ctx, &v) : JS_NULL;
    JS_FreeCString(ctx, s);
    return r;
}

enum { FLD_MAJOR,
    FLD_MINOR,
    FLD_PATCH,
    FLD_PRERELEASE };

static JSValue dyn_semver_field(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
    }
    switch (magic) {
    case FLD_MAJOR:
        r = JS_NewInt64(ctx, (int64_t)v.major);
        break;
    case FLD_MINOR:
        r = JS_NewInt64(ctx, (int64_t)v.minor);
        break;
    case FLD_PATCH:
        r = JS_NewInt64(ctx, (int64_t)v.patch);
        break;
    default:
        r = (v.n_pre > 0) ? sv_pre_array(ctx, &v) : JS_NULL;
        break;
    }
    JS_FreeCString(ctx, s);
    return r;
}

static int sv_two(JSContext* ctx, int argc, JSValueConst* argv, int* cmp)
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
    if (!as)
        return -1;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) {
        JS_FreeCString(ctx, as);
        return -1;
    }
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

static JSValue dyn_semver_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int c;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, c);
}

enum { CMP_GT,
    CMP_GTE,
    CMP_LT,
    CMP_LTE,
    CMP_EQ,
    CMP_NEQ };

static JSValue dyn_semver_cmpbool(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int c, r;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c))
        return JS_EXCEPTION;
    switch (magic) {
    case CMP_GT:
        r = c > 0;
        break;
    case CMP_GTE:
        r = c >= 0;
        break;
    case CMP_LT:
        r = c < 0;
        break;
    case CMP_LTE:
        r = c <= 0;
        break;
    case CMP_EQ:
        r = c == 0;
        break;
    default:
        r = c != 0;
        break;
    }
    return JS_NewBool(ctx, r);
}

static int sv_two_versions(JSContext* ctx, int argc, JSValueConst* argv,
    SemVer* a, SemVer* b,
    const char** pas, const char** pbs)
{
    const char *as, *bs;
    size_t an, bn;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx, "dyna:semver: expected two version strings");
        return -1;
    }
    as = JS_ToCStringLen(ctx, &an, argv[0]);
    if (!as)
        return -1;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) {
        JS_FreeCString(ctx, as);
        return -1;
    }
    if (semver_parse(as, an, a) || semver_parse(bs, bn, b)) {
        JS_FreeCString(ctx, as);
        JS_FreeCString(ctx, bs);
        JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
        return -1;
    }
    *pas = as;
    *pbs = bs;
    return 0;
}

static JSValue dyn_semver_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    SemVer a, b;
    const char *as, *bs;
    (void)this_val;
    if (sv_two_versions(ctx, argc, argv, &a, &b, &as, &bs))
        return JS_EXCEPTION;
    const char* which = NULL;
    if (a.major != b.major)
        which = "major";
    else if (a.minor != b.minor)
        which = "minor";
    else if (a.patch != b.patch)
        which = "patch";
    else if (semver_compare(&a, &b) != 0) {

        which = "prerelease";
    } else {
        int i;
        int build_eq = a.n_build == b.n_build;
        for (i = 0; build_eq && i < a.n_build; i++)
            build_eq = a.build[i].len == b.build[i].len && memcmp(a.build[i].s, b.build[i].s, a.build[i].len) == 0;
        if (!build_eq)
            which = "build";
    }
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return which ? JS_NewString(ctx, which) : JS_NULL;
}

static JSValue dyn_semver_compare_build(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    SemVer a, b;
    const char *as, *bs;
    int c;
    (void)this_val;
    if (sv_two_versions(ctx, argc, argv, &a, &b, &as, &bs))
        return JS_EXCEPTION;
    c = semver_compare(&a, &b);
    if (c == 0) {
        if (a.n_build == 0 && b.n_build == 0)
            c = 0;
        else if (a.n_build == 0)
            c = -1;
        else if (b.n_build == 0)
            c = 1;
        else
            c = sv_cmp_pre(a.build, a.n_build, b.build, b.n_build);
    }
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return JS_NewInt32(ctx, c);
}

static int sv_eq_intersects(const Comparator* eq, const Comparator* c)
{
    if (eq->v.n_pre > 0) {
        if (c->v.n_pre == 0)
            return 0;
        if (c->v.major != eq->v.major || c->v.minor != eq->v.minor || c->v.patch != eq->v.patch)
            return 0;
    }
    return cmp_test(&eq->v, c);
}

static int sv_comp_matches_nothing(const Comparator* c)
{
    if (c->op != OP_LT || c->v.major != 0 || c->v.minor != 0 || c->v.patch != 0)
        return 0;
    return c->v.n_pre == 0 || (c->v.n_pre == 1 && c->v.pre[0].numeric && c->v.pre[0].len == 1 && c->v.pre[0].s[0] == '0');
}

static int sv_comp_is_any(const Comparator* c)
{
    return c->op == OP_GTE && c->v.major == 0 && c->v.minor == 0 && c->v.patch == 0 && c->v.n_pre == 0;
}

static int sv_comp_intersects(const Comparator* a, const Comparator* b)
{
    int a_up, a_dn, b_up, b_dn, a_inc, b_inc, sc;

    if (sv_comp_is_any(a) || sv_comp_is_any(b))
        return 1;
    if (sv_comp_matches_nothing(a) || sv_comp_matches_nothing(b))
        return 0;

    if (a->op == OP_EQ && b->op != OP_EQ)
        return sv_eq_intersects(a, b);
    if (b->op == OP_EQ && a->op != OP_EQ)
        return sv_eq_intersects(b, a);

    a_up = a->op == OP_GT || a->op == OP_GTE;
    a_dn = a->op == OP_LT || a->op == OP_LTE;
    b_up = b->op == OP_GT || b->op == OP_GTE;
    b_dn = b->op == OP_LT || b->op == OP_LTE;

    if (a_up && b_up)
        return 1;
    if (a_dn && b_dn)
        return 1;

    a_inc = a->op == OP_GTE || a->op == OP_LTE || a->op == OP_EQ;
    b_inc = b->op == OP_GTE || b->op == OP_LTE || b->op == OP_EQ;
    sc = semver_compare(&a->v, &b->v);
    if (sc == 0 && a_inc && b_inc)
        return 1;
    if (a_up && b_dn && sc < 0)
        return 1;
    if (a_dn && b_up && sc > 0)
        return 1;
    return 0;
}

static int sv_sets_intersect(const CompSet* x, const CompSet* y)
{
    int i, j;
    for (i = 0; i < x->n; i++)
        for (j = i + 1; j < x->n; j++)
            if (!sv_comp_intersects(&x->c[i], &x->c[j]))
                return 0;
    for (i = 0; i < y->n; i++)
        for (j = i + 1; j < y->n; j++)
            if (!sv_comp_intersects(&y->c[i], &y->c[j]))
                return 0;
    for (i = 0; i < x->n; i++)
        for (j = 0; j < y->n; j++)
            if (!sv_comp_intersects(&x->c[i], &y->c[j]))
                return 0;
    return 1;
}

static int sv_range_intersects(const Range* ra, const Range* rb)
{
    int i, j;
    for (i = 0; i < ra->n; i++)
        for (j = 0; j < rb->n; j++)
            if (sv_sets_intersect(&ra->s[i], &rb->s[j]))
                return 1;
    return 0;
}

static int sv_range_arg(JSContext* ctx, JSValueConst v, const Range** out,
    const char** powned, int* powned_flag, Range** pheap);

static JSValue dyn_semver_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static int sv_read_versions(JSContext* ctx, JSValueConst arr, JSValue** pelems,
    const char*** pcstrs, SemVer** psvs, uint32_t* pcount,
    int allow_invalid)
{
    JSValue lval, *elems = NULL;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    uint32_t len, i, filled = 0;
    int isarr = JS_IsArray(ctx, arr);

    if (isarr < 0)
        return -1;
    if (!isarr) {
        JS_ThrowTypeError(ctx, "dyna:semver: expected an Array of versions");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lval))
        return -1;
    if (JS_ToUint32(ctx, &len, lval)) {
        JS_FreeValue(ctx, lval);
        return -1;
    }
    JS_FreeValue(ctx, lval);

#if UINTPTR_MAX == UINT32_MAX
    if (len > 0 && (size_t)len > ((size_t)-1) / sizeof(SemVer)) {
        JS_ThrowRangeError(ctx, "dyna:semver: array too large");
        return -1;
    }
#endif
    if (len > 0) {
        elems = js_malloc(ctx, (size_t)len * sizeof(*elems));
        cstrs = js_malloc(ctx, (size_t)len * sizeof(*cstrs));
        svs = js_malloc(ctx, (size_t)len * sizeof(*svs));
        if (!elems || !cstrs || !svs) {
            js_free(ctx, elems);
            js_free(ctx, cstrs);
            js_free(ctx, svs);
            return -1;
        }
    }

    for (i = 0; i < len; i++) {
        size_t slen;
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e))
            goto fail;
        elems[i] = e;
        cstrs[i] = NULL;
        filled = i + 1;
        if (!JS_IsString(e)) {
            JS_ThrowTypeError(ctx,
                "dyna:semver: every array element must be a string");
            goto fail;
        }
        cstrs[i] = JS_ToCStringLen(ctx, &slen, e);
        if (!cstrs[i])
            goto fail;
        if (semver_parse(cstrs[i], slen, &svs[i])) {
            if (!allow_invalid) {
                JS_ThrowTypeError(ctx, "dyna:semver: invalid version in array "
                                       "at index %u",
                    i);
                goto fail;
            }
            svs[i].n_pre = -1;
        }
    }
    *pelems = elems;
    *pcstrs = cstrs;
    *psvs = svs;
    *pcount = len;
    return 0;

fail:
    for (i = 0; i < filled; i++) {
        if (cstrs && cstrs[i])
            JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems);
    js_free(ctx, cstrs);
    js_free(ctx, svs);
    return -1;
}

static void sv_free_versions(JSContext* ctx, JSValue* elems,
    const char** cstrs, SemVer* svs, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (cstrs && cstrs[i])
            JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems);
    js_free(ctx, cstrs);
    js_free(ctx, svs);
}

static void sv_sort_indices(const SemVer* svs, uint32_t* idx, uint32_t* tmp,
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
            while (l < mid)
                tmp[k++] = idx[l++];
            while (r < hi)
                tmp[k++] = idx[r++];
        }
        memcpy(idx, tmp, (size_t)n * sizeof(uint32_t));
    }
}

static JSValue dyn_semver_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue *elems, out;
    const char** cstrs;
    SemVer* svs;
    uint32_t count, *idx, *tmp, i;

    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: sort expects an Array of versions");
    }
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 0))
        return JS_EXCEPTION;

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    if (count > 0) {
        idx = js_malloc(ctx, (size_t)count * sizeof(*idx));
        tmp = js_malloc(ctx, (size_t)count * sizeof(*tmp));
        if (!idx || !tmp) {
            js_free(ctx, idx);
            js_free(ctx, tmp);
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
        for (i = 0; i < count; i++)
            idx[i] = i;
        sv_sort_indices(svs, idx, tmp, count);

        for (i = 0; i < count; i++) {
            JSValue e = elems[idx[i]];
            elems[idx[i]] = JS_UNDEFINED;
            if (JS_DefinePropertyValueUint32(ctx, out, i, e,
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, e);
                js_free(ctx, idx);
                js_free(ctx, tmp);
                JS_FreeValue(ctx, out);
                sv_free_versions(ctx, elems, cstrs, svs, count);
                return JS_EXCEPTION;
            }
        }
        js_free(ctx, idx);
        js_free(ctx, tmp);
    }
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

static JSValue dyn_semver_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max)
{
    JSValue *elems, out;
    const char** cstrs;
    SemVer* svs;
    const char* rs;
    size_t rn;
    Range* rg;
    uint32_t count, i;
    long best = -1;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: expected (Array, rangeString)");
    }
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs)
        return JS_EXCEPTION;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1)) {
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
        return JS_NULL;
    }

    for (i = 0; i < count; i++) {
        if (!range_test(&svs[i], rg))
            continue;
        if (best < 0) {
            best = (long)i;
            continue;
        }
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

static JSValue dyn_semver_satisfies(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *vs, *rs;
    size_t vn, rn;
    SemVer v;
    Range* rg;
    int res;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: satisfies(version, range) expects two strings");
    }
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }

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
        return JS_NewBool(ctx, 0);
    }
    res = range_test(&v, rg);
    js_free(ctx, rg);
    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rs);
    return JS_NewBool(ctx, res);
}

typedef struct {
    int is_num;
    uint64_t num;
    const char* str;
    size_t slen;
} IncId;

typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    IncId pre[SEMVER_MAX_IDS + 2];
} IncVer;

enum { REL_MAJOR,
    REL_MINOR,
    REL_PATCH,
    REL_PREMAJOR,
    REL_PREMINOR,
    REL_PREPATCH,
    REL_PRERELEASE,
    REL_RELEASE };

static int inc_cmp_id0(const IncId* a, const char* id, size_t idn)
{
    int id_num = idn > 0;
    size_t k, m;
    int c;
    for (k = 0; k < idn; k++)
        if (!sv_is_digit(id[k])) {
            id_num = 0;
            break;
        }
    if (a->is_num && id_num) {
        char tmp[24];
        int tl = sv_u64toa(a->num, tmp);
        return sv_cmp_num(tmp, (size_t)tl, id, idn);
    }
    if (a->is_num)
        return -1;
    if (id_num)
        return 1;
    m = a->slen < idn ? a->slen : idn;
    c = memcmp(a->str, id, m);
    if (c)
        return c < 0 ? -1 : 1;
    if (a->slen != idn)
        return a->slen < idn ? -1 : 1;
    return 0;
}

static void inc_pre_step(IncVer* v, const char* id, size_t idn)
{
    if (v->n_pre == 0) {
        v->pre[0].is_num = 1;
        v->pre[0].num = 0;
        v->n_pre = 1;
    } else {
        int i = v->n_pre, found = 0;
        while (--i >= 0) {
            if (v->pre[i].is_num) {
                v->pre[i].num++;
                found = 1;
                break;
            }
        }
        if (!found && v->n_pre < (int)countof(v->pre)) {
            v->pre[v->n_pre].is_num = 1;
            v->pre[v->n_pre].num = 0;
            v->n_pre++;
        }
    }
    if (id) {
        int replace = 1;
        if (inc_cmp_id0(&v->pre[0], id, idn) == 0 && v->n_pre >= 2 && v->pre[1].is_num)
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

static void inc_apply(IncVer* v, int rel, const char* id, size_t idn)
{
    switch (rel) {
    case REL_MAJOR:
        if (v->minor != 0 || v->patch != 0 || v->n_pre == 0)
            v->major++;
        v->minor = 0;
        v->patch = 0;
        v->n_pre = 0;
        break;
    case REL_MINOR:
        if (v->patch != 0 || v->n_pre == 0)
            v->minor++;
        v->patch = 0;
        v->n_pre = 0;
        break;
    case REL_PATCH:
        if (v->n_pre == 0)
            v->patch++;
        v->n_pre = 0;
        break;
    case REL_PREMAJOR:
        v->n_pre = 0;
        v->patch = 0;
        v->minor = 0;
        v->major++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREMINOR:
        v->n_pre = 0;
        v->patch = 0;
        v->minor++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREPATCH:
        v->n_pre = 0;
        v->patch++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PRERELEASE:
        if (v->n_pre == 0)
            v->patch++;
        inc_pre_step(v, id, idn);
        break;
    case REL_RELEASE:
        v->n_pre = 0;
        break;
    }
}

static JSValue inc_format(JSContext* ctx, const IncVer* v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char* buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++)
        cap += (v->pre[i].is_num ? 20 : v->pre[i].slen) + 1;
    buf = js_malloc(ctx, cap);
    if (!buf)
        return JS_EXCEPTION;
    o += sv_u64toa(v->major, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->minor, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->patch, buf + o);
    if (v->n_pre > 0) {
        buf[o++] = '-';
        for (i = 0; i < v->n_pre; i++) {
            if (i)
                buf[o++] = '.';
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

static int inc_release_id(const char* s, size_t n)
{
    static const struct {
        const char* name;
        int id;
    } m[] = {
        { "major", REL_MAJOR },
        { "minor", REL_MINOR },
        { "patch", REL_PATCH },
        { "premajor", REL_PREMAJOR },
        { "preminor", REL_PREMINOR },
        { "prepatch", REL_PREPATCH },
        { "prerelease", REL_PRERELEASE },
        { "release", REL_RELEASE },
    };
    size_t i;
    for (i = 0; i < countof(m); i++)
        if (strlen(m[i].name) == n && memcmp(m[i].name, s, n) == 0)
            return m[i].id;
    return -1;
}

static JSValue dyn_semver_inc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *vs, *rel_s, *id = NULL;
    size_t vn, rel_n, idn = 0;
    SemVer sv;
    IncVer iv;
    int rel, i, had_pre;
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
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    rel_s = JS_ToCStringLen(ctx, &rel_n, argv[1]);
    if (!rel_s) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }
    if (argc >= 3 && JS_IsString(argv[2])) {
        id = JS_ToCStringLen(ctx, &idn, argv[2]);
        if (!id) {
            JS_FreeCString(ctx, vs);
            JS_FreeCString(ctx, rel_s);
            return JS_EXCEPTION;
        }
    }

    rel = inc_release_id(rel_s, rel_n);
    if (rel < 0 || semver_parse(vs, vn, &sv)) {
        JSValue e = JS_ThrowTypeError(ctx, rel < 0 ? "dyna:semver: invalid release type" : "dyna:semver: invalid version");
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return e;
    }

    memset(&iv, 0, sizeof(iv));
    iv.major = sv.major;
    iv.minor = sv.minor;
    iv.patch = sv.patch;
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
    had_pre = iv.n_pre > 0;
    inc_apply(&iv, rel, id, idn);
    if (rel == REL_RELEASE && !had_pre) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return JS_NULL;
    }
    if (iv.major > SV_MAX_SAFE || iv.minor > SV_MAX_SAFE || iv.patch > SV_MAX_SAFE) {
        JSValue e = JS_ThrowRangeError(ctx,
            "dyna:semver: inc result exceeds MAX_SAFE_INTEGER");
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return e;
    }
    for (i = 0; i < iv.n_pre; i++) {
        if (iv.pre[i].is_num && iv.pre[i].num > SV_MAX_SAFE) {
            JSValue e = JS_ThrowRangeError(ctx,
                "dyna:semver: inc result exceeds MAX_SAFE_INTEGER");
            JS_FreeCString(ctx, vs);
            JS_FreeCString(ctx, rel_s);
            if (id)
                JS_FreeCString(ctx, id);
            return e;
        }
    }
    r = inc_format(ctx, &iv);

    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rel_s);
    if (id)
        JS_FreeCString(ctx, id);
    return r;
}

static JSValue dyn_semver_coerce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i;
    JSValue result = JS_NULL;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;

    for (i = 0; i < n; i++) {
        size_t j, k;
        int hm = 0, hp = 0;
        size_t ms = 0, ml = 0, ps = 0, pl = 0;
        char buf[64];
        size_t o = 0;
        SemVer sv;

        if (!sv_is_digit(s[i]) || (i > 0 && sv_is_digit(s[i - 1])))
            continue;
        j = i;
        while (j < n && sv_is_digit(s[j]))
            j++;
        if (j - i > 16) {
            i = j - 1;
            continue;
        }

        k = j;
        if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
            size_t a = k + 1, b = a;
            while (b < n && sv_is_digit(s[b]))
                b++;
            if (b - a <= 16) {
                hm = 1;
                ms = a;
                ml = b - a;
                k = b;
                if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
                    size_t c = k + 1, d = c;
                    while (d < n && sv_is_digit(s[d]))
                        d++;
                    if (d - c <= 16) {
                        hp = 1;
                        ps = c;
                        pl = d - c;
                    }
                }
            }
        }

        memcpy(buf + o, s + i, j - i);
        o += j - i;
        buf[o++] = '.';
        if (hm) {
            memcpy(buf + o, s + ms, ml);
            o += ml;
        } else
            buf[o++] = '0';
        buf[o++] = '.';
        if (hp) {
            memcpy(buf + o, s + ps, pl);
            o += pl;
        } else
            buf[o++] = '0';

        if (semver_parse(buf, o, &sv) == 0)
            result = JS_NewStringLen(ctx, buf, o);
        break;
    }
    JS_FreeCString(ctx, s);
    return result;
}

typedef struct {
    Range rg;
    char* source;
} dyn_range_t;

static JSClassID dyn_range_class_id;

static void dyn_range_dispose(void* native)
{
    dyn_range_t* r = (dyn_range_t*)native;
    if (!r)
        return;
    free(r->source);
    free(r);
}

static void dyn_range_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_range_dispose(JS_GetOpaque(val, dyn_range_class_id));
}

static const JSClassDef dyn_range_class = {
    "Range",
    .finalizer = dyn_range_finalizer,
};

static JSValue dyn_range_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    dyn_range_t* r;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Range(rangeString) requires a range");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    r = (dyn_range_t*)malloc(sizeof(*r));
    if (!r) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->source = (char*)malloc(n + 1);
    if (!r->source) {
        free(r);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(r->source, s, n);
    r->source[n] = '\0';
    JS_FreeCString(ctx, s);

    if (parse_range(r->source, n, &r->rg)) {
        JSValue e = JS_ThrowTypeError(ctx, "dyna:semver: invalid range \"%.*s\"",
            (int)(n > 64 ? 64 : n), r->source);
        free(r->source);
        free(r);
        return e;
    }

    return dyn_plain_wrap(ctx, new_target, dyn_range_class_id, r, dyn_range_dispose);
}

static JSValue dyn_range_test(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* vs;
    size_t vn;
    SemVer v;
    dyn_range_t* r;
    int ok;

    (void)argc;
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }
    ok = semver_parse(vs, vn, &v) == 0 && range_test(&v, &r->rg);
    JS_FreeCString(ctx, vs);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_range_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max)
{
    JSValue *elems = NULL, out;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    dyn_range_t* r;
    uint32_t count, i;
    long best = -1;

    (void)argc;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    if (!r) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    for (i = 0; i < count; i++) {
        if (!range_test(&svs[i], &r->rg))
            continue;
        if (best < 0) {
            best = (long)i;
            continue;
        }
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

static JSValue dyn_range_filter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue *elems = NULL, out;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    dyn_range_t* r;
    uint32_t count, i, o = 0;

    (void)argc;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
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
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
    }
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

static JSValue dyn_range_get_source(JSContext* ctx, JSValueConst this_val)
{
    dyn_range_t* r = (dyn_range_t*)dyn_plain_get(ctx, this_val,
        dyn_range_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewString(ctx, r->source);
}

static int sv_range_arg(JSContext* ctx, JSValueConst v, const Range** out,
    const char** powned, int* powned_flag, Range** pheap)
{
    const char* s;
    size_t n;
    dyn_range_t* r;

    *pheap = NULL;
    if (JS_IsString(v)) {
        Range* tmp;
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        tmp = (Range*)malloc(sizeof(*tmp));
        if (!tmp) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (parse_range(s, n, tmp)) {
            JS_FreeCString(ctx, s);
            free(tmp);
            JS_ThrowTypeError(ctx, "dyna:semver: invalid range");
            return -1;
        }
        *out = tmp;
        *pheap = tmp;
        *powned = s;
        *powned_flag = 1;
        return 0;
    }
    r = (dyn_range_t*)JS_GetOpaque(v, dyn_range_class_id);
    if (r) {
        *out = &r->rg;
        *powned = NULL;
        *powned_flag = 0;
        return 0;
    }
    JS_ThrowTypeError(ctx,
        "dyna:semver: expected a range string or a Range");
    return -1;
}

static JSValue dyn_semver_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const Range *ra, *rb;
    Range *ha, *hb;
    const char *own_a, *own_b;
    int flag_a, flag_b, res;
    (void)this_val;

    if (sv_range_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            &ra, &own_a, &flag_a, &ha))
        return JS_EXCEPTION;
    if (sv_range_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &rb, &own_b, &flag_b, &hb)) {
        if (flag_a)
            JS_FreeCString(ctx, own_a);
        free(ha);
        return JS_EXCEPTION;
    }
    res = sv_range_intersects(ra, rb);
    if (flag_a)
        JS_FreeCString(ctx, own_a);
    if (flag_b)
        JS_FreeCString(ctx, own_b);
    free(ha);
    free(hb);
    return JS_NewBool(ctx, res);
}

static JSValue dyn_range_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const Range* rb;
    Range* hb;
    const char* own_b;
    int flag_b, res;
    dyn_range_t* r;

    if (sv_range_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            &rb, &own_b, &flag_b, &hb))
        return JS_EXCEPTION;
    r = (dyn_range_t*)dyn_plain_get(ctx, this_val, dyn_range_class_id);
    res = r && sv_range_intersects(&r->rg, rb);
    if (flag_b)
        JS_FreeCString(ctx, own_b);
    free(hb);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, res);
}

static JSValue dyn_range_get_set_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_range_t* r = (dyn_range_t*)dyn_plain_get(ctx, this_val,
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
    JS_CFUNC_DEF("intersects", 1, dyn_range_intersects),
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
    JS_CFUNC_DEF("diff", 2, dyn_semver_diff),
    JS_CFUNC_DEF("compareBuild", 2, dyn_semver_compare_build),
    JS_CFUNC_DEF("intersects", 2, dyn_semver_intersects),
};

static int dyn_semver_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_range_class_id, &dyn_range_class,
            dyn_range_proto, countof(dyn_range_proto),
            dyn_range_ctor, "Range")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_semver_funcs,
        countof(dyn_semver_funcs));
}

int js_nat_init_semver(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:semver", dyn_semver_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Range");
    return JS_AddModuleExportList(ctx, m, dyn_semver_funcs,
        countof(dyn_semver_funcs));
}

#endif
