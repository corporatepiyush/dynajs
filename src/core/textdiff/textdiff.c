#include "textdiff.h"

#include "../dyn-hash.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DYN_DIFF_MAX_DIAGONAL_STEPS (1u << 28)
#define DYN_DIFF_MAX_DIAGONAL_STEPS_CEIL (1u << 31)
#define DYN_DIFF_TOO_COSTLY (-2)

typedef struct {
    uint32_t* slot;
    uint32_t *ent_off, *ent_len;
    const uint8_t** ent_src;
    uint32_t n_ent, mask;
} dyn_intern_t;

typedef struct {
    int32_t *fwd, *rev;
    int32_t off;
    uint64_t budget;
    int32_t overflow;
} dyn_snake_ws_t;

enum { DYN_W_DIFF,
    DYN_W_EQ,
    DYN_W_DEL,
    DYN_W_INS };

typedef struct {
    int32_t a0, a1, b0, b1;
    int8_t kind;
} dyn_witem_t;

typedef struct {
    dyn_witem_t* v;
    uint32_t n, cap;
} dyn_wstack_t;

static int td_put(dyn_td_buf_t* b, const uint8_t* p, size_t n)
{
    if (b->error)
        return -1;
    if (n > b->cap - b->size) {
        size_t nc = b->cap ? b->cap : 256;
        uint8_t* nb;
        while (nc - b->size < n) {
            if (nc > SIZE_MAX / 2) {
                b->error = 1;
                return -1;
            }
            nc *= 2;
        }
        nb = (uint8_t*)realloc(b->buf, nc);
        if (!nb) {
            b->error = 1;
            return -1;
        }
        b->buf = nb;
        b->cap = nc;
    }
    if (n && p)
        memcpy(b->buf + b->size, p, n);
    b->size += n;
    return 0;
}

static int td_putc(dyn_td_buf_t* b, uint8_t c)
{
    return td_put(b, &c, 1);
}

static int td_putstr(dyn_td_buf_t* b, const char* s)
{
    return td_put(b, (const uint8_t*)s, strlen(s));
}

static int td_printf(dyn_td_buf_t* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int td_printf(dyn_td_buf_t* b, const char* fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || td_put(b, NULL, (size_t)n + 1) < 0) {
        b->error = 1;
        return -1;
    }
    b->size -= (size_t)n + 1;
    va_start(ap, fmt);
    vsnprintf((char*)b->buf + b->size, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->size += (size_t)n;
    return 0;
}

void dyn_td_buf_free(dyn_td_buf_t* b)
{
    free(b->buf);
    b->buf = NULL;
    b->size = b->cap = 0;
    b->error = 0;
}

static void dyn_td_toks_free(dyn_td_toks_t* t)
{
    free(t->off);
    free(t->len);
    free(t->id);
    t->off = t->len = t->id = NULL;
    t->n = 0;
}

static size_t dyn_grow(size_t cur, size_t need)
{
    size_t nc = cur ? cur : 64;
    while (nc < need) {
        if (nc < (1u << 16))
            nc *= 2;
        else if (nc < (1u << 20))
            nc += nc / 2;
        else
            nc += nc / 4;
    }
    return nc;
}

static int dyn_toks_push(dyn_td_toks_t* t, uint32_t* cap, uint32_t off, uint32_t len)
{
    if (t->n == *cap) {
        uint32_t nc = (uint32_t)dyn_grow(*cap, (size_t)*cap + 1);
        uint32_t *o, *l, *i;
        if (nc > DYN_TD_MAX_TOKENS)
            nc = DYN_TD_MAX_TOKENS;
        if (nc == *cap)
            return -1;
        o = (uint32_t*)realloc(t->off, nc * sizeof(uint32_t));
        if (o)
            t->off = o;
        l = (uint32_t*)realloc(t->len, nc * sizeof(uint32_t));
        if (l)
            t->len = l;
        i = (uint32_t*)realloc(t->id, nc * sizeof(uint32_t));
        if (i)
            t->id = i;
        if (!o || !l || !i)
            return -1;
        *cap = nc;
    }
    t->off[t->n] = off;
    t->len[t->n] = len;
    t->n++;
    return 0;
}

static int dyn_is_ws_byte(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v';
}

static int dyn_tokenize(const uint8_t* s, uint32_t n, int mode, dyn_td_toks_t* t)
{
    uint32_t cap = 0, i = 0;
    t->off = t->len = t->id = NULL;
    t->n = 0;
    while (i < n) {
        uint32_t start = i;
        if (mode == DYN_TD_LINES) {
            while (i < n && s[i] != '\n')
                i++;
            if (i < n)
                i++;
        } else if (mode == DYN_TD_WORDS) {
            int ws = dyn_is_ws_byte(s[i]);
            while (i < n && dyn_is_ws_byte(s[i]) == ws)
                i++;
        } else {
            uint8_t c = s[i++];
            if (c >= 0xF0)
                i += 3;
            else if (c >= 0xE0)
                i += 2;
            else if (c >= 0xC0)
                i += 1;
            if (i > n)
                i = n;
        }
        if (dyn_toks_push(t, &cap, start, i - start) < 0)
            return -1;
    }
    return 0;
}

static void dyn_intern_free(dyn_intern_t* t)
{
    free(t->slot);
    free(t->ent_off);
    free(t->ent_len);
    free(t->ent_src);
    t->slot = NULL;
    t->ent_off = NULL;
    t->ent_len = NULL;
    t->ent_src = NULL;
}

static uint64_t dyn_tok_hash(const uint8_t* p, uint32_t n)
{
    return dyn_xxh64(p, n, 0);
}

static int dyn_intern_init(dyn_intern_t* t, uint32_t total)
{
    uint32_t cap = 16;
    while (cap < total * 2 && cap < (1u << 30))
        cap <<= 1;
    t->mask = cap - 1;
    t->n_ent = 0;
    t->slot = (uint32_t*)calloc(cap, sizeof(uint32_t));
    t->ent_off = (uint32_t*)malloc((total + 1) * sizeof(uint32_t));
    t->ent_len = (uint32_t*)malloc((total + 1) * sizeof(uint32_t));
    t->ent_src = (const uint8_t**)malloc((total + 1) * sizeof(const uint8_t*));
    return (t->slot && t->ent_off && t->ent_len && t->ent_src) ? 0 : -1;
}

static uint32_t dyn_intern(dyn_intern_t* t, const uint8_t* src, uint32_t off,
    uint32_t len)
{
    uint64_t h = dyn_tok_hash(src + off, len);
    uint32_t i = (uint32_t)(h & t->mask);
    while (t->slot[i]) {
        uint32_t e = t->slot[i] - 1;
        if (t->ent_len[e] == len
            && memcmp(t->ent_src[e] + t->ent_off[e], src + off, len) == 0)
            return e;
        i = (i + 1) & t->mask;
    }
    t->ent_off[t->n_ent] = off;
    t->ent_len[t->n_ent] = len;
    t->ent_src[t->n_ent] = src;
    t->slot[i] = t->n_ent + 1;
    return t->n_ent++;
}

static int32_t dyn_middle_snake(const uint32_t* A, int32_t a0, int32_t a1,
    const uint32_t* B, int32_t b0, int32_t b1,
    dyn_snake_ws_t* w,
    int32_t* px, int32_t* py,
    int32_t* pu, int32_t* pv)
{
    int32_t N = a1 - a0, M = b1 - b0;
    int32_t delta = N - M, odd = delta & 1;
    int32_t dmax = (N + M + 1) / 2, d, k;
    int32_t *fwd = w->fwd + w->off, *rev = w->rev + w->off;

    fwd[1] = 0;
    rev[1] = 0;
    for (d = 0; d <= dmax; d++) {
        for (k = -d; k <= d; k += 2) {
            int32_t x, y, x0, y0;
            if (k == -d || (k != d && fwd[k - 1] < fwd[k + 1]))
                x = fwd[k + 1];
            else
                x = fwd[k - 1] + 1;
            y = x - k;
            x0 = x;
            y0 = y;
            while (x < N && y < M && A[a0 + x] == B[b0 + y]) {
                x++;
                y++;
            }
            fwd[k] = x;
            if (odd && (k - delta) >= -(d - 1) && (k - delta) <= (d - 1)
                && x + rev[delta - k] >= N) {
                *px = a0 + x0;
                *py = b0 + y0;
                *pu = a0 + x;
                *pv = b0 + y;
                return 2 * d - 1;
            }
        }
        if (w->budget < (uint64_t)(2 * d + 1)) {
            w->budget = 0;
            w->overflow = 1;
            return DYN_DIFF_TOO_COSTLY;
        }
        w->budget -= (uint64_t)(2 * d + 1);
        for (k = -d; k <= d; k += 2) {
            int32_t x, y, x0, y0;
            if (k == -d || (k != d && rev[k - 1] < rev[k + 1]))
                x = rev[k + 1];
            else
                x = rev[k - 1] + 1;
            y = x - k;
            x0 = x;
            y0 = y;
            while (x < N && y < M && A[a1 - x - 1] == B[b1 - y - 1]) {
                x++;
                y++;
            }
            rev[k] = x;
            if (!odd && (k - delta) >= -d && (k - delta) <= d
                && x + fwd[delta - k] >= N) {
                *px = a1 - x;
                *py = b1 - y;
                *pu = a1 - x0;
                *pv = b1 - y0;
                return 2 * d;
            }
        }
        if (w->budget < (uint64_t)(2 * d + 1)) {
            w->budget = 0;
            w->overflow = 1;
            return DYN_DIFF_TOO_COSTLY;
        }
        w->budget -= (uint64_t)(2 * d + 1);
    }
    *px = *pu = a0;
    *py = *pv = b0;
    return -1;
}

static int dyn_script_add(dyn_td_script_t* s, int8_t op, uint32_t a0, uint32_t a1,
    uint32_t b0, uint32_t b1)
{
    if ((op == DYN_TD_DEL && a0 == a1) || (op == DYN_TD_INS && b0 == b1)
        || (op == DYN_TD_EQ && a0 == a1))
        return 0;
    if (s->n && s->v[s->n - 1].op == op) {
        s->v[s->n - 1].a1 = a1;
        s->v[s->n - 1].b1 = b1;
        return 0;
    }
    if (s->n == s->cap) {
        uint32_t nc = (uint32_t)dyn_grow(s->cap, (size_t)s->cap + 1);
        dyn_td_hunk_t* nv = (dyn_td_hunk_t*)realloc(s->v, nc * sizeof(dyn_td_hunk_t));
        if (!nv)
            return -1;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n].op = op;
    s->v[s->n].a0 = a0;
    s->v[s->n].a1 = a1;
    s->v[s->n].b0 = b0;
    s->v[s->n].b1 = b1;
    s->n++;
    return 0;
}

static int dyn_wpush(dyn_wstack_t* s, int8_t kind, int32_t a0, int32_t a1,
    int32_t b0, int32_t b1)
{
    if (s->n == s->cap) {
        uint32_t nc = (uint32_t)dyn_grow(s->cap, (size_t)s->cap + 1);
        dyn_witem_t* nv = (dyn_witem_t*)realloc(s->v, nc * sizeof(dyn_witem_t));
        if (!nv)
            return -1;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n].kind = kind;
    s->v[s->n].a0 = a0;
    s->v[s->n].a1 = a1;
    s->v[s->n].b0 = b0;
    s->v[s->n].b1 = b1;
    s->n++;
    return 0;
}

static int dyn_diff_run(const uint32_t* A, uint32_t na, const uint32_t* B,
    uint32_t nb, dyn_snake_ws_t* w, dyn_td_script_t* out)
{
    dyn_wstack_t st;
    int rc = 0;
    st.v = NULL;
    st.n = 0;
    st.cap = 0;
    if (dyn_wpush(&st, DYN_W_DIFF, 0, (int32_t)na, 0, (int32_t)nb) < 0)
        return -1;
    while (st.n) {
        dyn_witem_t it = st.v[--st.n];
        int32_t N, M, x, y, u, v;
        switch (it.kind) {
        case DYN_W_EQ:
            rc = dyn_script_add(out, DYN_TD_EQ, (uint32_t)it.a0, (uint32_t)it.a1,
                (uint32_t)it.b0, (uint32_t)it.b1);
            break;
        case DYN_W_DEL:
            rc = dyn_script_add(out, DYN_TD_DEL, (uint32_t)it.a0, (uint32_t)it.a1,
                (uint32_t)it.b0, (uint32_t)it.b0);
            break;
        case DYN_W_INS:
            rc = dyn_script_add(out, DYN_TD_INS, (uint32_t)it.a0, (uint32_t)it.a0,
                (uint32_t)it.b0, (uint32_t)it.b1);
            break;
        default:
            N = it.a1 - it.a0;
            M = it.b1 - it.b0;
            if (N <= 0 && M <= 0)
                break;
            {
                int32_t p = 0, sfx = 0;
                while (p < N && p < M && A[it.a0 + p] == B[it.b0 + p])
                    p++;
                while (sfx < N - p && sfx < M - p
                    && A[it.a1 - 1 - sfx] == B[it.b1 - 1 - sfx])
                    sfx++;
                if (p || sfx) {
                    if (dyn_wpush(&st, DYN_W_EQ, it.a1 - sfx, it.a1,
                            it.b1 - sfx, it.b1)
                            < 0
                        || dyn_wpush(&st, DYN_W_DIFF, it.a0 + p, it.a1 - sfx,
                               it.b0 + p, it.b1 - sfx)
                            < 0
                        || dyn_wpush(&st, DYN_W_EQ, it.a0, it.a0 + p,
                               it.b0, it.b0 + p)
                            < 0)
                        rc = -1;
                    break;
                }
            }
            if (N <= 0) {
                rc = dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1);
                break;
            }
            if (M <= 0) {
                rc = dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0);
                break;
            }
            if (N == 1 || M == 1) {
                int32_t i, hit = -1;
                if (M == 1) {
                    for (i = it.a0; i < it.a1; i++)
                        if (A[i] == B[it.b0]) {
                            hit = i;
                            break;
                        }
                } else {
                    for (i = it.b0; i < it.b1; i++)
                        if (B[i] == A[it.a0]) {
                            hit = i;
                            break;
                        }
                }
                if (hit < 0) {
                    if (dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0) < 0)
                        rc = -1;
                    break;
                }
                if (M == 1) {
                    if (dyn_wpush(&st, DYN_W_DEL, hit + 1, it.a1, it.b1, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_EQ, hit, hit + 1, it.b0, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_DEL, it.a0, hit, it.b0, it.b0) < 0)
                        rc = -1;
                } else {
                    if (dyn_wpush(&st, DYN_W_INS, it.a1, it.a1, hit + 1, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_EQ, it.a0, it.a1, hit, hit + 1) < 0
                        || dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, hit) < 0)
                        rc = -1;
                }
                break;
            }
            if (dyn_middle_snake(A, it.a0, it.a1, B, it.b0, it.b1, w,
                    &x, &y, &u, &v)
                < 0) {
                rc = w->overflow ? DYN_DIFF_TOO_COSTLY : -1;
                break;
            }
            if ((x == it.a0 && y == it.b0 && u == it.a0 && v == it.b0)
                || (x >= it.a1 && y >= it.b1) || (u <= it.a0 && v <= it.b0)) {
                if (dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1) < 0
                    || dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0) < 0)
                    rc = -1;
                break;
            }
            if (dyn_wpush(&st, DYN_W_DIFF, u, it.a1, v, it.b1) < 0
                || dyn_wpush(&st, DYN_W_EQ, x, u, y, v) < 0
                || dyn_wpush(&st, DYN_W_DIFF, it.a0, x, it.b0, y) < 0)
                rc = -1;
            break;
        }
        if (rc < 0)
            break;
    }
    free(st.v);
    return rc;
}

void dyn_td_res_free(dyn_td_res_t* r)
{
    free(r->sc.v);
    dyn_td_toks_free(&r->ta);
    dyn_td_toks_free(&r->tb);
    memset(r, 0, sizeof *r);
}

#define DYN_TD_DP_CELLS (1u << 24)

#define DYN_TD_TRACE_BYTES (1u << 20)

static int dyn_td_lcs_script(const uint32_t* A, const uint32_t* B, uint32_t p, uint32_t n, uint32_t m,
    const uint32_t* ck, uint32_t K, int tr, dyn_td_script_t* out)
{
    uint8_t *ops = (uint8_t*)malloc((size_t)n + m + 1), *dir = (uint8_t*)malloc((size_t)K * m);
    uint32_t* row = (uint32_t*)malloc(((size_t)m + 1) * sizeof *row);
    uint32_t i = n, j = m, k = 0, c, ia = p, jb = p;
    int rc = 0;
    if (!ops || !dir || !row) {
        free(ops);
        free(dir);
        free(row);
        return -1;
    }
    for (c = (n - 1) / K + 1; c-- > 0;) {
        uint32_t r0 = c * K, w = j, ii, jj;
        memcpy(row, ck + (size_t)c * (m + 1), ((size_t)j + 1) * sizeof *row);
        for (ii = r0 + 1; ii <= i; ii++) {
            uint32_t diag = row[0], ai = A[p + ii - 1];
            uint8_t* dr = dir + (size_t)(ii - r0 - 1) * w;
            row[0] = 0;
            for (jj = 1; jj <= w; jj++) {
                uint32_t up = row[jj], left = row[jj - 1];
                if (ai == B[p + jj - 1]) {
                    row[jj] = diag + 1;
                    dr[jj - 1] = 0;
                } else if (tr ? up < left : left >= up) {
                    row[jj] = left;
                    dr[jj - 1] = 2;
                } else {
                    row[jj] = up;
                    dr[jj - 1] = 1;
                }
                diag = up;
            }
        }
        while (i > r0) {
            uint8_t d = j ? dir[(size_t)(i - r0 - 1) * w + (j - 1)] : 1;
            ops[k++] = d;
            if (d == 0) {
                i--;
                j--;
            } else if (d == 1)
                i--;
            else
                j--;
        }
    }
    free(dir);
    free(row);
    while (j) {
        ops[k++] = 2;
        j--;
    }
    while (k && rc == 0) {
        uint8_t d = ops[--k];
        if (d == 0) {
            rc = dyn_script_add(out, DYN_TD_EQ, ia, ia + 1, jb, jb + 1);
            ia++;
            jb++;
        } else if (d == (tr ? 2 : 1)) {
            rc = dyn_script_add(out, DYN_TD_DEL, ia, ia + 1, jb, jb);
            ia++;
        } else {
            rc = dyn_script_add(out, DYN_TD_INS, ia, ia, jb, jb + 1);
            jb++;
        }
    }
    free(ops);
    return rc;
}

static int dyn_td_bound(const uint32_t* A, uint32_t na, const uint32_t* B, uint32_t nb,
    uint32_t n_ent, uint64_t bud, dyn_td_script_t* out)
{
    uint32_t p = 0, s = 0, n, m, i, j, K = 1, *ck = NULL;
    const uint32_t *X = A, *Y = B;
    int tr = 0;
    uint64_t lcs = 0, d, h;
    int rc = 0;
    while (p < na && p < nb && A[p] == B[p])
        p++;
    while (s < na - p && s < nb - p && A[na - 1 - s] == B[nb - 1 - s])
        s++;
    n = na - p - s;
    m = nb - p - s;
    if (n < 2 || m < 2)
        return 0;
    if ((uint64_t)n * m <= DYN_TD_DP_CELLS) {
        uint32_t* row;
        if (m > n) {
            uint32_t t = n;
            n = m;
            m = t;
            X = B;
            Y = A;
            tr = 1;
        }
        K = m >= DYN_TD_TRACE_BYTES ? 1 : DYN_TD_TRACE_BYTES / m;
        ck = (uint32_t*)calloc(((size_t)(n - 1) / K + 1) * ((size_t)m + 1), sizeof *ck);
        if (!ck)
            return -1;
        row = (uint32_t*)calloc((size_t)m + 1, sizeof *row);
        if (!row) {
            free(ck);
            return -1;
        }
        for (i = 1; i <= n; i++) {
            uint32_t diag = row[0], ai = X[p + i - 1];
            if ((i - 1) % K == 0)
                memcpy(ck + (size_t)((i - 1) / K) * (m + 1), row, ((size_t)m + 1) * sizeof *row);
            for (j = 1; j <= m; j++) {
                uint32_t up = row[j];
                if (ai == Y[p + j - 1])
                    row[j] = diag + 1;
                else if (row[j - 1] < up)
                    row[j] = up;
                else
                    row[j] = row[j - 1];
                diag = up;
            }
        }
        lcs = row[m];
        free(row);
    } else {
        uint32_t* cnt = (uint32_t*)calloc((size_t)n_ent + 1, sizeof *cnt);
        if (!cnt)
            return -1;
        for (i = 0; i < n; i++)
            cnt[A[p + i]]++;
        for (j = 0; j < m; j++)
            if (cnt[B[p + j]]) {
                cnt[B[p + j]]--;
                lcs++;
            }
        free(cnt);
    }
    d = (uint64_t)n + m - 2 * lcs;
    h = (d + 1) / 2;
    if (2 * h * h >= bud) {
        if (!ck)
            return DYN_TD_ECOSTLY;
        rc = dyn_script_add(out, DYN_TD_EQ, 0, p, 0, p) < 0
                || dyn_td_lcs_script(X, Y, p, n, m, ck, K, tr, out) < 0
                || dyn_script_add(out, DYN_TD_EQ, na - s, na, nb - s, nb) < 0
            ? -1
            : 1;
    }
    free(ck);
    return rc;
}

int dyn_td_diff(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb,
    int mode, dyn_td_res_t* r)
{
    return dyn_td_diff_ex(a, na, b, nb, mode, 0, r);
}

int dyn_td_diff_ex(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb,
    int mode, uint64_t ceil, dyn_td_res_t* r)
{
    dyn_intern_t in;
    dyn_snake_ws_t ws;
    uint32_t i;
    int32_t span;
    int rc = DYN_TD_ENOMEM;

    memset(r, 0, sizeof *r);
    memset(&in, 0, sizeof in);
    ws.fwd = NULL;
    ws.rev = NULL;
    ws.off = 0;
    ws.overflow = 0;
    if (dyn_tokenize(a, na, mode, &r->ta) < 0
        || dyn_tokenize(b, nb, mode, &r->tb) < 0) {
        rc = DYN_TD_ETOKENS;
        goto done;
    }
    if (dyn_intern_init(&in, r->ta.n + r->tb.n) < 0) {
        rc = DYN_TD_ENOMEM;
        goto done;
    }
    for (i = 0; i < r->ta.n; i++)
        r->ta.id[i] = dyn_intern(&in, a, r->ta.off[i], r->ta.len[i]);
    for (i = 0; i < r->tb.n; i++)
        r->tb.id[i] = dyn_intern(&in, b, r->tb.off[i], r->tb.len[i]);

    span = (int32_t)(r->ta.n + r->tb.n) + 3;
    ws.off = span;
    {
        uint64_t bud = (uint64_t)(r->ta.n + r->tb.n);
        bud *= bud;
        bud *= 2;
        if (bud < DYN_DIFF_MAX_DIAGONAL_STEPS)
            bud = DYN_DIFF_MAX_DIAGONAL_STEPS;
        if (!ceil)
            ceil = DYN_DIFF_MAX_DIAGONAL_STEPS_CEIL;
        if (bud > ceil)
            bud = ceil;
        ws.budget = ceil == UINT64_MAX ? UINT64_MAX : bud;
        if (ceil != UINT64_MAX) {
            int brc = dyn_td_bound(r->ta.id, r->ta.n, r->tb.id, r->tb.n, in.n_ent, bud, &r->sc);
            if (brc == 1) {
                rc = DYN_TD_OK;
                goto done;
            }
            if (brc < 0) {
                rc = brc == DYN_TD_ECOSTLY ? DYN_TD_ECOSTLY : DYN_TD_ENOMEM;
                goto done;
            }
        }
    }
    ws.fwd = (int32_t*)calloc((size_t)span * 2 + 1, sizeof(int32_t));
    ws.rev = (int32_t*)calloc((size_t)span * 2 + 1, sizeof(int32_t));
    if (!ws.fwd || !ws.rev) {
        rc = DYN_TD_ENOMEM;
        goto done;
    }
    {
        int32_t drc = dyn_diff_run(r->ta.id, r->ta.n, r->tb.id, r->tb.n, &ws, &r->sc);
        if (drc == DYN_DIFF_TOO_COSTLY) {
            rc = DYN_TD_ECOSTLY;
            goto done;
        }
        if (drc < 0) {
            rc = DYN_TD_ENOMEM;
            goto done;
        }
    }
    rc = DYN_TD_OK;
done:
    free(ws.fwd);
    free(ws.rev);
    dyn_intern_free(&in);
    if (rc < 0)
        dyn_td_res_free(r);
    return rc;
}

typedef struct {
    int8_t op;
    uint32_t a, b;
} dyn_ud_ent_t;

static int dyn_ud_line(dyn_td_buf_t* o, char tag, const uint8_t* p, uint32_t n)
{
    td_putc(o, (uint8_t)tag);
    td_put(o, p, n);
    if (!n || p[n - 1] != '\n')
        td_putstr(o, "\n\\ No newline at end of file\n");
    return o->error ? -1 : 0;
}

static void dyn_ud_range(dyn_td_buf_t* o, char tag, uint32_t start, uint32_t len)
{
    if (len == 1)
        td_printf(o, "%c%u", tag, start + 1);
    else
        td_printf(o, "%c%u,%u", tag, len ? start + 1 : start, len);
}

int dyn_td_unified(const uint8_t* a, const uint8_t* b, const dyn_td_res_t* res,
    uint32_t ctxl, const char* an, const char* bn, dyn_td_buf_t* o)
{
    dyn_ud_ent_t* ent;
    uint32_t n = 0, i;

    ent = (dyn_ud_ent_t*)malloc(((size_t)res->ta.n + res->tb.n + 1) * sizeof *ent);
    if (!ent)
        return DYN_TD_ENOMEM;
    for (i = 0; i < res->sc.n;) {
        const dyn_td_hunk_t* h = &res->sc.v[i];
        uint32_t j, t;
        if (h->op == DYN_TD_EQ) {
            for (t = 0; t < h->a1 - h->a0; t++) {
                ent[n].op = DYN_TD_EQ;
                ent[n].a = h->a0 + t;
                ent[n].b = h->b0 + t;
                n++;
            }
            i++;
            continue;
        }
        for (j = i; j < res->sc.n && res->sc.v[j].op != DYN_TD_EQ; j++)
            if (res->sc.v[j].op == DYN_TD_DEL)
                for (t = res->sc.v[j].a0; t < res->sc.v[j].a1; t++) {
                    ent[n].op = DYN_TD_DEL;
                    ent[n].a = t;
                    ent[n].b = h->b0;
                    n++;
                }
        for (j = i; j < res->sc.n && res->sc.v[j].op != DYN_TD_EQ; j++)
            if (res->sc.v[j].op == DYN_TD_INS)
                for (t = res->sc.v[j].b0; t < res->sc.v[j].b1; t++) {
                    ent[n].op = DYN_TD_INS;
                    ent[n].a = res->sc.v[j].a0;
                    ent[n].b = t;
                    n++;
                }
        i = j;
    }
    td_printf(o, "--- %s\n+++ %s\n", an, bn);
    {
        uint32_t pos = 0, prev_end = 0;
        while (pos < n) {
            uint32_t hs, last, k, he, al = 0, bl = 0;
            if (ent[pos].op == DYN_TD_EQ) {
                pos++;
                continue;
            }
            hs = pos - prev_end > ctxl ? pos - ctxl : prev_end;
            last = pos;
            k = pos + 1;
            while (k < n) {
                uint32_t r = 0;
                if (ent[k].op != DYN_TD_EQ) {
                    last = k++;
                    continue;
                }
                while (k + r < n && ent[k + r].op == DYN_TD_EQ)
                    r++;
                if (k + r == n || r > 2 * (uint64_t)ctxl)
                    break;
                k += r;
            }
            he = n - (last + 1) > ctxl ? last + 1 + ctxl : n;
            for (k = hs; k < he; k++) {
                al += ent[k].op != DYN_TD_INS;
                bl += ent[k].op != DYN_TD_DEL;
            }
            td_putstr(o, "@@ ");
            dyn_ud_range(o, '-', ent[hs].a, al);
            td_putc(o, ' ');
            dyn_ud_range(o, '+', ent[hs].b, bl);
            td_putstr(o, " @@\n");
            for (k = hs; k < he; k++) {
                const dyn_ud_ent_t* e = &ent[k];
                int rc;
                if (e->op == DYN_TD_INS)
                    rc = dyn_ud_line(o, '+', b + res->tb.off[e->b], res->tb.len[e->b]);
                else
                    rc = dyn_ud_line(o, e->op == DYN_TD_EQ ? ' ' : '-', a + res->ta.off[e->a], res->ta.len[e->a]);
                if (rc < 0)
                    break;
            }
            prev_end = he;
            pos = he;
        }
    }
    free(ent);
    return o->error ? DYN_TD_ENOMEM : DYN_TD_OK;
}

typedef struct {
    const uint8_t* p;
    size_t n, at, lineno;
} dyn_pl_t;

static int dyn_pl_next(dyn_pl_t* r, const uint8_t** s, size_t* len)
{
    const uint8_t* nl;
    if (r->at >= r->n)
        return 0;
    *s = r->p + r->at;
    nl = (const uint8_t*)memchr(*s, '\n', r->n - r->at);
    *len = nl ? (size_t)(nl - *s) : r->n - r->at;
    r->at += *len + (nl ? 1 : 0);
    r->lineno++;
    return 1;
}

static int dyn_pl_peek_marker(dyn_pl_t* r)
{
    if (r->at < r->n && r->p[r->at] == '\\') {
        const uint8_t* s;
        size_t l;
        dyn_pl_next(r, &s, &l);
        return 1;
    }
    return 0;
}

static int dyn_pl_num(const uint8_t** s, const uint8_t* e, uint32_t* v)
{
    uint64_t x = 0;
    const uint8_t* p = *s;
    if (p >= e || *p < '0' || *p > '9')
        return -1;
    while (p < e && *p >= '0' && *p <= '9') {
        x = x * 10 + (uint64_t)(*p++ - '0');
        if (x > 0x7fffffffu)
            return -1;
    }
    *v = (uint32_t)x;
    *s = p;
    return 0;
}

static int dyn_pl_hunk_head(const uint8_t* s, size_t len, uint32_t* as, uint32_t* al,
    uint32_t* bs, uint32_t* bl)
{
    const uint8_t *p = s + 3, *e = s + len;
    *al = 1;
    *bl = 1;
    if (p >= e || *p++ != '-' || dyn_pl_num(&p, e, as) < 0)
        return -1;
    if (p < e && *p == ',') {
        p++;
        if (dyn_pl_num(&p, e, al) < 0)
            return -1;
    }
    if (e - p < 2 || p[0] != ' ' || p[1] != '+')
        return -1;
    p += 2;
    if (dyn_pl_num(&p, e, bs) < 0)
        return -1;
    if (p < e && *p == ',') {
        p++;
        if (dyn_pl_num(&p, e, bl) < 0)
            return -1;
    }
    if (e - p < 3 || memcmp(p, " @@", 3) != 0)
        return -1;
    return 0;
}

int dyn_td_apply(const uint8_t* ts, uint32_t tn, const uint8_t* p, size_t pn,
    dyn_td_buf_t* o, char* msg)
{
    dyn_td_toks_t tt;
    dyn_pl_t r;
    uint32_t pos = 0, hunks = 0;
    int rc = DYN_TD_ENOMEM;

    memset(&tt, 0, sizeof tt);
    if (dyn_tokenize(ts, tn, DYN_TD_LINES, &tt) < 0)
        return DYN_TD_ETOKENS;
    r.p = p;
    r.n = pn;
    r.at = 0;
    r.lineno = 0;
    for (;;) {
        const uint8_t* s;
        size_t len;
        uint32_t as, al, bs, bl, st, ra, rb, tl;
        size_t head_line;
        if (!dyn_pl_next(&r, &s, &len))
            break;
        if (len < 3 || memcmp(s, "@@ ", 3) != 0) {
            if (hunks && len >= 4 && memcmp(s, "--- ", 4) == 0 && r.at + 4 <= r.n && memcmp(r.p + r.at, "+++ ", 4) == 0) {
                snprintf(msg, DYN_TD_MSG_CAP, "ApplyPatch: patch line %zu starts a second file; apply one file's patch at a time", r.lineno);
                rc = DYN_TD_ESYNTAX;
                goto done;
            }
            continue;
        }
        head_line = r.lineno;
        if (dyn_pl_hunk_head(s, len, &as, &al, &bs, &bl) < 0) {
            snprintf(msg, DYN_TD_MSG_CAP, "ApplyPatch: patch line %zu: malformed hunk header (expected \"@@ -l[,n] +l[,n] @@\")", r.lineno);
            rc = DYN_TD_ESYNTAX;
            goto done;
        }
        if (al && !as) {
            snprintf(msg, DYN_TD_MSG_CAP, "ApplyPatch: patch line %zu: hunk removes lines starting at line 0", r.lineno);
            rc = DYN_TD_ESYNTAX;
            goto done;
        }
        hunks++;
        st = al ? as - 1 : as;
        if (st < pos || (uint64_t)st + al > tt.n) {
            snprintf(msg, 200, "ApplyPatch: hunk %u (patch line %zu) does not apply: old lines %u..%u are %s (text has %u lines)",
                hunks, head_line, st + 1, st + al, st < pos ? "before the previous hunk's end" : "past the end of the text", tt.n);
            rc = DYN_TD_ECONFLICT;
            goto done;
        }
        for (; pos < st; pos++)
            td_put(o, ts + tt.off[pos], tt.len[pos]);
        tl = st;
        ra = al;
        rb = bl;
        while (ra || rb) {
            const uint8_t* c;
            size_t cl;
            int tag, nl;
            if (!dyn_pl_next(&r, &s, &len)) {
                snprintf(msg, DYN_TD_MSG_CAP, "ApplyPatch: hunk %u (patch line %zu) is truncated: %u old and %u new lines missing", hunks, head_line, ra, rb);
                rc = DYN_TD_ESYNTAX;
                goto done;
            }
            tag = len ? s[0] : ' ';
            c = len ? s + 1 : s;
            cl = len ? len - 1 : 0;
            if ((tag == ' ' && !(ra && rb)) || (tag == '-' && !ra) || (tag == '+' && !rb)
                || (tag != ' ' && tag != '-' && tag != '+')) {
                snprintf(msg, DYN_TD_MSG_CAP, "ApplyPatch: patch line %zu: unexpected line in hunk %u (%u old and %u new lines still due)", r.lineno, hunks, ra, rb);
                rc = DYN_TD_ESYNTAX;
                goto done;
            }
            nl = !dyn_pl_peek_marker(&r);
            if (tag != '+') {
                if (tt.len[tl] != cl + (size_t)nl || memcmp(ts + tt.off[tl], c, cl) != 0
                    || (nl && ts[tt.off[tl] + cl] != '\n')) {
                    snprintf(msg, 200, "ApplyPatch: hunk %u (patch line %zu) does not apply: text line %u differs from patch line %zu",
                        hunks, head_line, tl + 1, r.lineno - (nl ? 0 : 1));
                    rc = DYN_TD_ECONFLICT;
                    goto done;
                }
                tl++;
                ra--;
            }
            if (tag != '-') {
                td_put(o, c, cl);
                if (nl)
                    td_putc(o, '\n');
                rb--;
            }
        }
        pos = tl;
    }
    for (; pos < tt.n; pos++)
        td_put(o, ts + tt.off[pos], tt.len[pos]);
    rc = o->error ? DYN_TD_ENOMEM : DYN_TD_OK;
done:
    dyn_td_toks_free(&tt);
    return rc;
}
