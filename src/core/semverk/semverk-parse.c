#include "semverk.h"
#include "../dyn-ctype.h"

typedef struct {
    int xM, xm, xp;
    uint64_t M, m, p;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
} XRange;

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

size_t sv_ws_lead(const char* s, size_t n)
{
    size_t k, lead = 0;
    while ((k = sv_ws_char(s + lead, n - lead)) != 0)
        lead += k;
    return lead;
}

size_t sv_ws_trail(const char* s, size_t n)
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

int semver_parse(const char* s, size_t n, SemVer* out)
{
    size_t i = 0, start;
    out->major = out->minor = out->patch = 0;
    out->n_pre = 0;
    out->n_build = 0;

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
    return dyn_in_set64((unsigned char)c, DYN_WS_C);
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

void sv_range_release(Range* r)
{
    int i;
    for (i = 0; i < r->n; i++) {
        free(r->s[i].c);
        r->s[i].c = NULL;
        r->s[i].n = r->s[i].cap = 0;
    }
    r->n = 0;
}

void sv_range_free(Range* r)
{
    if (!r)
        return;
    sv_range_release(r);
    free(r);
}

int semverk_parse_range(const char* s, size_t n, Range* out)
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
        if (out->n >= SV_MAX_SETS) {
            sv_range_release(out);
            return -1;
        }
        set = &out->s[out->n];
        set->n = 0;
        set->cap = 0;
        set->c = NULL;
        if (parse_set(s + i, setlen, set)
            || (set->n == 0 && sv_add_cmp(set, OP_GTE, 0, 0, 0, NULL, 0, 0))) {
            free(set->c);
            set->c = NULL;
            sv_range_release(out);
            return -1;
        }
        out->n++;
        if (next > n)
            break;
        i = next;
    }
    return 0;
}

