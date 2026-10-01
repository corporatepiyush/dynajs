#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DEC_INLINE 40
#define DEC_MAX_DIGITS 100000u
#define DEC_MAX_MUL_CELLS (1u << 26)
#define DEC_MAX_DIV_CELLS (1u << 23)
#define DEC_E_TOOSLOW (-3)
#define DEC_MAX_TEXT 1000000
#define DEC_DEF_PREC 34

enum { RND_UP,
    RND_DOWN,
    RND_CEIL,
    RND_FLOOR,
    RND_HALF_UP,
    RND_HALF_DOWN,
    RND_HALF_EVEN,
    RND_HALF_ODD };

typedef struct {
    int32_t exp;
    uint32_t nd, cap;
    uint8_t sign;
    uint8_t* d;
    uint8_t inl[DEC_INLINE];
} dec_t;

static void dec_init(dec_t* x)
{
    x->exp = 0;
    x->nd = 0;
    x->sign = 0;
    x->d = x->inl;
    x->cap = DEC_INLINE;
}

static void dec_free(dec_t* x)
{
    if (x->d != x->inl)
        free(x->d);
    x->d = x->inl;
    x->cap = DEC_INLINE;
    x->nd = 0;
}

static int dec_grow(dec_t* x, uint32_t need)
{
    uint8_t* np;
    if (need <= x->cap)
        return 0;
    if (need > DEC_MAX_DIGITS)
        return -1;
    {
        uint32_t want = x->cap + x->cap / 2 + 16;
        if (want > need)
            need = want;
        if (need > DEC_MAX_DIGITS)
            need = DEC_MAX_DIGITS;
    }
    np = (uint8_t*)malloc(need);
    if (!np)
        return -1;
    memcpy(np, x->d, x->nd);
    if (x->d != x->inl)
        free(x->d);
    x->d = np;
    x->cap = need;
    return 0;
}

static void dec_trim(dec_t* x)
{
    while (x->nd && x->d[x->nd - 1] == 0)
        x->nd--;
    if (x->nd == 0) {
        x->exp = 0;
        x->sign = 0;
        return;
    }
    {
        uint32_t k = 0;
        while (k < x->nd && x->d[k] == 0)
            k++;
        if (k) {
            memmove(x->d, x->d + k, x->nd - k);
            x->nd -= k;
            x->exp += (int32_t)k;
        }
    }
}

static int dec_is_zero(const dec_t* x) { return x->nd == 0; }

static int dec_at(const dec_t* x, int32_t p)
{
    if (p < x->exp || p >= x->exp + (int32_t)x->nd)
        return 0;
    return x->d[p - x->exp];
}

static int dec_cmp_abs(const dec_t* a, const dec_t* b)
{
    int32_t ha, hb, lo, p;

    if (a->nd == 0)
        return b->nd == 0 ? 0 : -1;
    if (b->nd == 0)
        return 1;
    ha = a->exp + (int32_t)a->nd;
    hb = b->exp + (int32_t)b->nd;
    if (ha != hb)
        return ha < hb ? -1 : 1;
    lo = a->exp < b->exp ? a->exp : b->exp;
    for (p = ha - 1; p >= lo; p--) {
        int da = dec_at(a, p), db = dec_at(b, p);
        if (da != db)
            return da < db ? -1 : 1;
    }
    return 0;
}

static int dec_cmp(const dec_t* a, const dec_t* b)
{
    int c;
    if (a->nd == 0 && b->nd == 0)
        return 0;
    if (a->sign != b->sign)
        return a->sign ? -1 : 1;
    c = dec_cmp_abs(a, b);
    return a->sign ? -c : c;
}

static int dec_copy(dec_t* r, const dec_t* a)
{
    if (r == a)
        return 0;
    if (dec_grow(r, a->nd ? a->nd : 1) < 0)
        return -1;
    memcpy(r->d, a->d, a->nd);
    r->nd = a->nd;
    r->exp = a->exp;
    r->sign = a->sign;
    return 0;
}

static int dec_add_abs(dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    uint32_t n = (uint32_t)(hi - lo) + 1, i;
    int carry = 0;

    if (a->nd == 0)
        return dec_copy(r, b);
    if (b->nd == 0)
        return dec_copy(r, a);
    if (dec_grow(r, n) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        int p = (int)(lo + (int32_t)i);
        int s = dec_at(a, p) + dec_at(b, p) + carry;
        r->d[i] = (uint8_t)(s % 10);
        carry = s / 10;
    }
    r->nd = n;
    r->exp = lo;
    dec_trim(r);
    return 0;
}

static int dec_sub_abs(dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    uint32_t n = (uint32_t)(hi - lo), i;
    int borrow = 0;

    if (b->nd == 0)
        return dec_copy(r, a);
    if (dec_grow(r, n ? n : 1) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        int p = (int)(lo + (int32_t)i);
        int s = dec_at(a, p) - dec_at(b, p) - borrow;
        if (s < 0) {
            s += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r->d[i] = (uint8_t)s;
    }
    r->nd = n;
    r->exp = lo;
    dec_trim(r);
    return 0;
}

static int dec_add(dec_t* r, const dec_t* a, const dec_t* b)
{
    if (a->sign == b->sign) {
        if (dec_add_abs(r, a, b) < 0)
            return -1;
        r->sign = r->nd ? a->sign : 0;
        return 0;
    }
    {
        int c = dec_cmp_abs(a, b);
        if (c == 0) {
            r->nd = 0;
            r->exp = 0;
            r->sign = 0;
            return 0;
        }
        if (c > 0) {
            if (dec_sub_abs(r, a, b) < 0)
                return -1;
            r->sign = r->nd ? a->sign : 0;
        } else {
            if (dec_sub_abs(r, b, a) < 0)
                return -1;
            r->sign = r->nd ? b->sign : 0;
        }
        return 0;
    }
}

static int dec_sub(dec_t* r, const dec_t* a, const dec_t* b)
{
    dec_t nb;
    int rc;

    dec_init(&nb);
    if (dec_copy(&nb, b) < 0) {
        dec_free(&nb);
        return -1;
    }
    if (nb.nd)
        nb.sign = (uint8_t)!nb.sign;
    rc = dec_add(r, a, &nb);
    dec_free(&nb);
    return rc;
}

static int dec_mul(dec_t* r, const dec_t* a, const dec_t* b)
{
    uint32_t n, i, j;
    uint32_t* wide;

    if (a->nd == 0 || b->nd == 0) {
        r->nd = 0;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    n = a->nd + b->nd;
    if ((uint64_t)a->nd * (uint64_t)b->nd > DEC_MAX_MUL_CELLS)
        return DEC_E_TOOSLOW;
    if (n > DEC_MAX_DIGITS)
        return -1;
    wide = (uint32_t*)calloc(n, sizeof *wide);
    if (!wide)
        return -1;
    for (i = 0; i < a->nd; i++) {
        uint32_t av = a->d[i];
        if (!av)
            continue;
        for (j = 0; j < b->nd; j++)
            wide[i + j] += av * (uint32_t)b->d[j];
    }
    if (dec_grow(r, n) < 0) {
        free(wide);
        return -1;
    }
    {
        uint32_t carry = 0;
        for (i = 0; i < n; i++) {
            uint32_t v = wide[i] + carry;
            r->d[i] = (uint8_t)(v % 10);
            carry = v / 10;
        }
    }
    free(wide);
    r->nd = n;
    {
        int64_t e = (int64_t)a->exp + (int64_t)b->exp;
        if (e > 2000000000LL || e < -2000000000LL)
            return -1;
        r->exp = (int32_t)e;
    }
    r->sign = (uint8_t)(a->sign ^ b->sign);
    dec_trim(r);
    return 0;
}

static int dec_round_up(int mode, int guard, int rest, int sign, int last)
{
    switch (mode) {
    case RND_UP:
        return guard || rest;
    case RND_DOWN:
        return 0;
    case RND_CEIL:
        return (guard || rest) && !sign;
    case RND_FLOOR:
        return (guard || rest) && sign;
    case RND_HALF_UP:
        return guard >= 5;
    case RND_HALF_DOWN:
        return guard > 5 || (guard == 5 && rest);
    case RND_HALF_ODD:
    case RND_HALF_EVEN:
        if (guard != 5)
            return guard > 5;
        if (rest)
            return 1;
        return mode == RND_HALF_ODD ? (last % 2 == 0) : (last % 2 == 1);
    default:
        return 0;
    }
}

static int dec_round_at(dec_t* x, int32_t target, int mode, int sticky)
{
    int guard = 0, rest = sticky;
    int32_t p;

    if (x->nd == 0 && !sticky)
        return 0;
    if (target > x->exp && x->nd) {
        guard = dec_at(x, target - 1);
        for (p = x->exp; p < target - 1; p++)
            if (dec_at(x, p)) {
                rest = 1;
                break;
            }
        if (target >= x->exp + (int32_t)x->nd) {
            x->nd = 0;
        } else {
            uint32_t drop = (uint32_t)(target - x->exp);
            memmove(x->d, x->d + drop, x->nd - drop);
            x->nd -= drop;
        }
        x->exp = target;
    }
    if (dec_round_up(mode, guard, rest, x->sign, x->nd ? x->d[0] : 0)) {
        uint32_t k = 0;
        int carry = 1;
        if (dec_grow(x, x->nd + 1) < 0)
            return -1;
        while (carry && k < x->nd) {
            int s = x->d[k] + carry;
            x->d[k] = (uint8_t)(s % 10);
            carry = s / 10;
            k++;
        }
        if (carry)
            x->d[x->nd++] = (uint8_t)carry;
    }
    dec_trim(x);
    return 0;
}

static int dec_round_sig(dec_t* x, uint32_t prec, int mode, int sticky)
{
    if (x->nd <= prec && !sticky)
        return 0;
    if (x->nd == 0)
        return 0;
    return dec_round_at(x, x->exp + (int32_t)x->nd - (int32_t)prec, mode, sticky);
}

static int dec_ge(const uint8_t* rem, uint32_t rn, const uint8_t* b, uint32_t bn)
{
    uint32_t k;
    if (rn != bn)
        return rn > bn;
    for (k = bn; k-- > 0;)
        if (rem[k] != b[k])
            return rem[k] > b[k];
    return 1;
}

static uint32_t dec_sub_into(uint8_t* rem, uint32_t rn, const uint8_t* b,
    uint32_t bn)
{
    uint32_t k;
    int borrow = 0;
    for (k = 0; k < rn; k++) {
        int s = rem[k] - (k < bn ? b[k] : 0) - borrow;
        if (s < 0) {
            s += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        rem[k] = (uint8_t)s;
    }
    while (rn && rem[rn - 1] == 0)
        rn--;
    return rn;
}

static int dec_divmod_int(uint8_t* q, uint32_t* qn, uint8_t* rem, uint32_t* rn,
    const uint8_t* a, uint32_t an,
    const uint8_t* b, uint32_t bn)
{
    int32_t i;
    uint32_t r = 0;

    *qn = an;
    memset(q, 0, an);
    for (i = (int32_t)an - 1; i >= 0; i--) {
        int d = 0;
        if (r)
            memmove(rem + 1, rem, r);
        rem[0] = a[i];
        r++;
        while (r && rem[r - 1] == 0)
            r--;
        while (dec_ge(rem, r, b, bn)) {
            r = dec_sub_into(rem, r, b, bn);
            if (++d > 9)
                return -1;
        }
        q[i] = (uint8_t)d;
    }
    while (*qn && q[*qn - 1] == 0)
        (*qn)--;
    *rn = r;
    return 0;
}

static int dec_div(dec_t* r, const dec_t* a, const dec_t* b, uint32_t prec, int mode)
{
    uint8_t *num = NULL, *q = NULL, *rem = NULL;
    uint32_t nn, qn = 0, rn = 0, extra;
    int32_t shift;
    int rc = -1;

    if (b->nd == 0)
        return -2;
    if (a->nd == 0) {
        r->nd = 0;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    extra = prec + 2 + b->nd;
    if (extra > DEC_MAX_DIGITS || a->nd > DEC_MAX_DIGITS - extra)
        return -1;
    nn = a->nd + extra;
    if ((uint64_t)nn * (uint64_t)b->nd > DEC_MAX_DIV_CELLS)
        return DEC_E_TOOSLOW;
    num = (uint8_t*)calloc(nn, 1);
    q = (uint8_t*)calloc(nn, 1);
    rem = (uint8_t*)calloc(nn + 1, 1);
    if (!num || !q || !rem)
        goto done;
    memcpy(num + extra, a->d, a->nd);
    shift = (int32_t)extra;
    if (dec_divmod_int(q, &qn, rem, &rn, num, nn, b->d, b->nd) < 0)
        goto done;
    if (dec_grow(r, qn ? qn : 1) < 0)
        goto done;
    memcpy(r->d, q, qn);
    r->nd = qn;
    {
        int64_t e = (int64_t)a->exp - (int64_t)b->exp - (int64_t)shift;
        if (e > 2000000000LL || e < -2000000000LL)
            goto done;
        r->exp = (int32_t)e;
    }
    r->sign = (uint8_t)(a->sign ^ b->sign);
    dec_trim(r);
    if (dec_round_sig(r, prec, mode, rn != 0) < 0)
        goto done;
    rc = 0;
done:
    free(num);
    free(q);
    free(rem);
    return rc;
}

static int dec_int_divmod(dec_t* qout, dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    uint32_t an, bn, qn, rn;
    uint8_t *ad = NULL, *bd = NULL, *q = NULL, *rem = NULL;
    int rc = -1;

    if (b->nd == 0)
        return -2;
    if (a->nd == 0) {
        if (qout) {
            qout->nd = 0;
            qout->exp = 0;
            qout->sign = 0;
        }
        r->nd = 0;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    an = (uint32_t)(a->exp + (int32_t)a->nd - lo);
    bn = (uint32_t)(b->exp + (int32_t)b->nd - lo);
    if (an > DEC_MAX_DIGITS || bn > DEC_MAX_DIGITS)
        return -1;
    if ((uint64_t)an * (uint64_t)bn > DEC_MAX_DIV_CELLS)
        return DEC_E_TOOSLOW;
    ad = (uint8_t*)calloc(an, 1);
    bd = (uint8_t*)calloc(bn, 1);
    q = (uint8_t*)calloc(an, 1);
    rem = (uint8_t*)calloc(an + 1, 1);
    if (!ad || !bd || !q || !rem)
        goto done;
    memcpy(ad + (a->exp - lo), a->d, a->nd);
    memcpy(bd + (b->exp - lo), b->d, b->nd);
    if (dec_divmod_int(q, &qn, rem, &rn, ad, an, bd, bn) < 0)
        goto done;
    if (dec_grow(r, rn ? rn : 1) < 0)
        goto done;
    memcpy(r->d, rem, rn);
    r->nd = rn;
    r->exp = lo;
    r->sign = a->sign;
    dec_trim(r);
    if (qout) {
        if (dec_grow(qout, qn ? qn : 1) < 0) {
            rc = -1;
            goto done;
        }
        memcpy(qout->d, q, qn);
        qout->nd = qn;
        qout->exp = 0;
        qout->sign = (uint8_t)((a->sign ^ b->sign) & (qn ? 1u : 0u));
        dec_trim(qout);
    }
    rc = 0;
done:
    free(ad);
    free(bd);
    free(q);
    free(rem);
    return rc;
}

static int dec_mod(dec_t* r, const dec_t* a, const dec_t* b)
{
    return dec_int_divmod(NULL, r, a, b);
}

static int dec_parse_exp(const char* s, size_t n, size_t* pi, int32_t* exp)
{
    size_t i = *pi + 1;
    int esign = 0, edig = 0;
    int64_t ev = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) {
        esign = s[i] == '-';
        i++;
    }
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
        ev = ev * 10 + (s[i] - '0');
        edig = 1;
        if (ev > 2000000000LL)
            return -1;
    }
    if (!edig)
        return -1;
    *exp = (int32_t)(esign ? -ev : ev);
    *pi = i;
    return 0;
}

static int dec_parse(dec_t* x, const char* s, size_t n)
{
    size_t i = 0;
    uint32_t nd = 0;
    int32_t exp = 0;
    int seen = 0, dot = -1;

    dec_init(x);
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        x->sign = s[i] == '-';
        i++;
    }
    for (; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (nd >= DEC_MAX_DIGITS)
                return -1;
            if (dec_grow(x, nd + 1) < 0)
                return -1;
            x->d[nd++] = (uint8_t)(s[i] - '0');
            x->nd = nd;
            seen = 1;
        } else if (s[i] == '.' && dot < 0) {
            dot = (int)nd;
        } else {
            break;
        }
    }
    if (!seen)
        return -1;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        if (dec_parse_exp(s, n, &i, &exp) < 0)
            return -1;
    }
    if (i != n)
        return -1;
    x->nd = nd;
    x->exp = exp - (dot < 0 ? 0 : (int32_t)(nd - (uint32_t)dot));
    {
        uint32_t a = 0, b = nd;
        while (a + 1 < b) {
            uint8_t t = x->d[a];
            x->d[a] = x->d[b - 1];
            x->d[b - 1] = t;
            a++;
            b--;
        }
    }
    dec_trim(x);
    return 0;
}

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom, too_long;
} db_t;

static void db_init(db_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = b->too_long = 0;
}
static void db_free(db_t* b)
{
    free(b->p);
    b->p = NULL;
}

static void db_write(db_t* b, const void* p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        uint8_t* np;
        while (nc < b->n + n)
            nc += nc / 2 + 8;
        np = (uint8_t*)realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void db_put(db_t* b, uint8_t c) { db_write(b, &c, 1); }

static void dec_write(db_t* b, const dec_t* x)
{
    int32_t hi, lo, p;

    if (x->nd == 0) {
        db_put(b, '0');
        return;
    }
    if (x->sign)
        db_put(b, '-');
    hi = x->exp + (int32_t)x->nd - 1;
    lo = x->exp;
    if (hi < 0)
        hi = 0;
    if (lo > 0)
        lo = 0;
    if ((int64_t)hi - lo >= DEC_MAX_TEXT) {
        b->too_long = 1;
        return;
    }
    for (p = hi; p >= lo; p--) {
        if (p == -1)
            db_put(b, '.');
        db_put(b, (uint8_t)('0' + dec_at(x, p)));
    }
}

static JSValue dyn_dec_too_long(JSContext* ctx)
{
    return JS_ThrowRangeError(ctx,
        "decimal: positional text would exceed %d characters; the exponent is "
        "too large to render",
        DEC_MAX_TEXT);
}

static void dec_write_fixed(db_t* b, const dec_t* x, int32_t dp, int neg)
{
    int32_t p, hi;

    if (neg)
        db_put(b, '-');
    hi = x->nd ? x->exp + (int32_t)x->nd - 1 : 0;
    if (hi < 0)
        hi = 0;
    if ((int64_t)hi + (dp > 0 ? dp : 0) >= DEC_MAX_TEXT) {
        b->too_long = 1;
        return;
    }
    for (p = hi; p >= 0; p--)
        db_put(b, (uint8_t)('0' + dec_at(x, p)));
    if (dp > 0) {
        db_put(b, '.');
        for (p = -1; p >= -dp; p--)
            db_put(b, (uint8_t)('0' + dec_at(x, p)));
    }
}

static JSClassID dyn_dec_class_id;

static void dyn_dec_dispose(void* p)
{
    dec_free(p);
    free(p);
}

static void dyn_dec_finalizer(JSRuntime* rt, JSValue val)
{
    dec_t* x = (dec_t*)JS_GetOpaque(val, dyn_dec_class_id);
    (void)rt;
    if (x) {
        dec_free(x);
        free(x);
    }
}

static const JSClassDef dyn_dec_class = {
    "Decimal",
    .finalizer = dyn_dec_finalizer,
};

static const char* const RND_NAMES[] = {
    "up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven", "halfOdd"
};

static int dyn_dec_opts_strict(JSContext* ctx, JSValueConst o)
{
    if (!JS_IsObject(o))
        return 0;
    {
        JSPropertyEnum* props = NULL;
        uint32_t nprops = 0, i;
        int bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char* name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) {
                bad = 1;
                break;
            }
            if (strcmp(name, "precision") != 0 && strcmp(name, "rounding") != 0) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: precision, rounding)", name);
                bad = 1;
            }
            JS_FreeCString(ctx, name);
        }
        for (i = 0; i < nprops; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return bad ? -1 : 0;
    }
}

static int dyn_rnd_mode(JSContext* ctx, JSValueConst o, int* mode)
{
    JSValue v;
    const char* s;
    size_t k;

    *mode = RND_HALF_EVEN;
    if (JS_IsString(o)) {
        v = JS_DupValue(ctx, o);
    } else if (JS_IsObject(o)) {
        if (dyn_dec_opts_strict(ctx, o))
            return -1;
        v = JS_GetPropertyStr(ctx, o, "rounding");
        if (JS_IsException(v))
            return -1;
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return 0;
        }
    } else {
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (k = 0; k < countof(RND_NAMES); k++)
        if (strcmp(s, RND_NAMES[k]) == 0) {
            *mode = (int)k;
            JS_FreeCString(ctx, s);
            return 0;
        }
    JS_ThrowRangeError(ctx, "rounding must be one of up, down, ceil, floor, "
                            "halfUp, halfDown, halfEven, halfOdd");
    JS_FreeCString(ctx, s);
    return -1;
}

static int dyn_prec(JSContext* ctx, JSValueConst o, uint32_t* prec)
{
    JSValue v;
    int64_t p = DEC_DEF_PREC;

    *prec = DEC_DEF_PREC;
    if (!JS_IsObject(o))
        return 0;
    if (dyn_dec_opts_strict(ctx, o))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "precision");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &p, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (d != (double)p) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "precision must be an integer");
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    if (p < 1 || p > 5000) {
        JS_ThrowRangeError(ctx, "precision must be 1 to 5000 significant digits");
        return -1;
    }
    *prec = (uint32_t)p;
    return 0;
}

static dec_t* dyn_dec_alloc(JSContext* ctx)
{
    dec_t* x = (dec_t*)malloc(sizeof *x);
    if (!x) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    dec_init(x);
    return x;
}

static JSValue dyn_dec_wrap(JSContext* ctx, JSValueConst new_target,
    dec_t* x)
{
    return dyn_plain_wrap(ctx, new_target, dyn_dec_class_id, x, dyn_dec_dispose);
}

static int dyn_dec_from(JSContext* ctx, JSValueConst v, dec_t* out)
{
    const char* s;
    size_t n;
    JSValue sv;
    int rc;

    if (JS_IsObject(v)) {
        dec_t* o = (dec_t*)JS_GetOpaque(v, dyn_dec_class_id);
        if (o) {
            dec_init(out);
            return dec_copy(out, o) < 0 ? -1 : 0;
        }
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (d != d || d == DYN_INFINITY || d == -DYN_INFINITY) {
            JS_ThrowRangeError(ctx, "Decimal: %s has no decimal value",
                d != d ? "NaN" : "Infinity");
            return -1;
        }
    } else if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "Decimal: expected a string, a number or a Decimal");
        return -1;
    }
    sv = JS_ToString(ctx, v);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCStringLen(ctx, &n, sv);
    JS_FreeValue(ctx, sv);
    if (!s)
        return -1;
    rc = dec_parse(out, s, n);
    if (rc < 0)
        JS_ThrowSyntaxError(ctx, "Decimal: not a decimal number: %s", s);
    JS_FreeCString(ctx, s);
    return rc;
}

static JSValue dyn_dec_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dec_t* x;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Decimal(value): a value is required");
    x = dyn_dec_alloc(ctx);
    if (!x)
        return JS_EXCEPTION;
    if (dyn_dec_from(ctx, argv[0], x) < 0) {
        dec_free(x);
        free(x);
        return JS_EXCEPTION;
    }
    return dyn_dec_wrap(ctx, new_target, x);
}

static dec_t* dyn_dec_this(JSContext* ctx, JSValueConst t)
{
    dec_t* x = (dec_t*)JS_GetOpaque2(ctx, t, dyn_dec_class_id);
    return x;
}

enum { OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD };

static JSValue dyn_dec_arith(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b, *r;
    uint32_t prec;
    int mode, rc;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal arithmetic needs an operand");
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    if (dyn_prec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0) {
        dec_free(&b);
        return JS_EXCEPTION;
    }
    r = dyn_dec_alloc(ctx);
    if (!r) {
        dec_free(&b);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case OP_ADD:
        rc = dec_add(r, a, &b);
        break;
    case OP_SUB: {
        dec_t nb = b;
        nb.sign = (uint8_t)(b.nd ? !b.sign : 0);
        rc = dec_add(r, a, &nb);
        break;
    }
    case OP_MUL:
        rc = dec_mul(r, a, &b);
        break;
    case OP_DIV:
        rc = dec_div(r, a, &b, prec, mode);
        break;
    default:
        rc = dec_mod(r, a, &b);
        break;
    }
    dec_free(&b);
    if (rc == DEC_E_TOOSLOW) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
                                       "exact operation (multiply is limited to %u digit-pairs, divide to "
                                       "%u)",
            (unsigned)DEC_MAX_MUL_CELLS, (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc == -2) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc < 0) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_unary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    r = dyn_dec_alloc(ctx);
    if (!r)
        return JS_EXCEPTION;
    if (dec_copy(r, a) < 0) {
        dec_free(r);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 0)
        r->sign = 0;
    else if (r->nd)
        r->sign = (uint8_t)!r->sign;
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_cmp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b;
    int c;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal comparison needs an operand");
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    c = dec_cmp(a, &b);
    dec_free(&b);
    return magic ? JS_NewBool(ctx, c == 0) : JS_NewInt32(ctx, c);
}

static JSValue dyn_dec_round(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    int64_t dp = 0;
    int mode;
    double dpd;

    if (!a)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &dpd, argv[0]) < 0)
            return JS_EXCEPTION;
        if (dpd != trunc(dpd))
            return JS_ThrowRangeError(ctx, "Decimal: %s places must be an "
                                           "integer",
                magic ? "toFixed" : "round");
        if (JS_ToInt64(ctx, &dp, argv[0]) < 0)
            return JS_EXCEPTION;
    }
    if (dp < -1000 || dp > 1000)
        return JS_ThrowRangeError(ctx, "Decimal: %s places must be -1000 to 1000",
            magic ? "toFixed" : "round");
    if (dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if (magic == 1 && (argc <= 1 || (!JS_IsString(argv[1]) && !JS_IsObject(argv[1]))))
        mode = RND_HALF_UP;
    dec_init(&t);
    if (dec_copy(&t, a) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_round_at(&t, (int32_t)-dp, mode, 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 0) {
        dec_t* r = dyn_dec_alloc(ctx);
        if (!r) {
            dec_free(&t);
            return JS_EXCEPTION;
        }
        if (dec_copy(r, &t) < 0) {
            dec_free(&t);
            dec_free(r);
            free(r);
            return JS_ThrowOutOfMemory(ctx);
        }
        dec_free(&t);
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
    }
    {
        db_t b;
        JSValue out;
        db_init(&b);
        dec_write_fixed(&b, &t, (int32_t)dp, a->sign && (t.nd || a->nd));
        dec_free(&t);
        if (b.too_long) {
            db_free(&b);
            return dyn_dec_too_long(ctx);
        }
        if (b.oom) {
            db_free(&b);
            return JS_ThrowOutOfMemory(ctx);
        }
        out = JS_NewStringLen(ctx, (const char*)b.p, b.n);
        db_free(&b);
        return out;
    }
}

static int dec_mul_into(dec_t* acc, const dec_t* m)
{
    dec_t t;
    int rc;

    dec_init(&t);
    rc = dec_mul(&t, acc, m);
    if (rc == 0)
        rc = dec_copy(acc, &t);
    dec_free(&t);
    return rc;
}

static int dec_pow_uint(dec_t* acc, dec_t* base, uint64_t e)
{
    int rc = 0;

    if (dec_grow(acc, 1) < 0)
        return -1;
    acc->d[0] = 1;
    acc->nd = 1;
    acc->exp = 0;
    acc->sign = 0;
    while (e > 0 && rc == 0) {
        if (e & 1)
            rc = dec_mul_into(acc, base);
        e >>= 1;
        if (e && rc == 0)
            rc = dec_mul_into(base, base);
    }
    return rc;
}

static JSValue dyn_dec_pow(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), acc, base, *r;
    int64_t e = 0;
    uint32_t prec;
    int mode, rc, neg;
    double ed;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToFloat64(ctx, &ed, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Decimal.pow(n): n must be an integer");
    if (ed != trunc(ed))
        return JS_ThrowRangeError(ctx, "Decimal.pow(n): n must be an integer");
    if (JS_ToInt64(ctx, &e, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Decimal.pow(n): n must be an integer");
    if (e < -10000 || e > 10000)
        return JS_ThrowRangeError(ctx, "Decimal.pow(n): |n| must be at most 10000");
    if (dyn_prec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    neg = e < 0;
    dec_init(&acc);
    dec_init(&base);
    rc = dec_copy(&base, a);
    if (rc == 0)
        rc = dec_pow_uint(&acc, &base, (uint64_t)(neg ? -e : e));
    if (rc == 0 && neg) {
        dec_t one, t;
        dec_init(&one);
        dec_init(&t);
        rc = dec_grow(&one, 1);
        if (rc == 0) {
            one.d[0] = 1;
            one.nd = 1;
            rc = dec_div(&t, &one, &acc, prec, mode);
            if (rc == 0)
                rc = dec_copy(&acc, &t);
        }
        dec_free(&one);
        dec_free(&t);
    }
    dec_free(&base);
    if (rc == -2) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc == DEC_E_TOOSLOW) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: pow exceeds the operation "
                                       "budget (multiply is limited to %u digit-pairs, divide to %u)",
            (unsigned)DEC_MAX_MUL_CELLS, (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc < 0) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    r = dyn_dec_alloc(ctx);
    if (!r) {
        dec_free(&acc);
        return JS_EXCEPTION;
    }
    if (dec_copy(r, &acc) < 0) {
        dec_free(&acc);
        dec_free(r);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&acc);
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t* a = dyn_dec_this(ctx, this_val);
    db_t b;
    JSValue out;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewBool(ctx, dec_is_zero(a));
    if (magic == 3)
        return JS_NewInt32(ctx, a->nd == 0 ? 0 : (a->sign ? -1 : 1));
    if (magic == 4)
        return JS_NewUint32(ctx, a->nd);
    db_init(&b);
    dec_write(&b, a);
    if (b.too_long) {
        db_free(&b);
        return dyn_dec_too_long(ctx);
    }
    if (b.oom) {
        db_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 1) {
        JSValue sv = JS_NewStringLen(ctx, (const char*)b.p, b.n);
        double d;
        db_free(&b);
        if (JS_IsException(sv))
            return sv;
        if (JS_ToFloat64(ctx, &d, sv) < 0) {
            JS_FreeValue(ctx, sv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, sv);
        return JS_NewFloat64(ctx, d);
    }
    out = JS_NewStringLen(ctx, (const char*)b.p, b.n);
    db_free(&b);
    return out;
}

#define DEC_E_INVALID (-4)
#define DEC_E_OVERFLOW (-5)
#define DEC_E_BUDGET (-6)

#define DEC_SERIES_CELLS 268435456.0

static int dec_halve(dec_t* t)
{
    uint32_t k;
    int carry = 0;

    if (t->nd == 0)
        return 0;
    if (dec_grow(t, t->nd + 1) < 0)
        return -1;
    for (k = 0; k < t->nd; k++) {
        int s2 = t->d[k] * 5 + carry;
        t->d[k] = (uint8_t)(s2 % 10);
        carry = s2 / 10;
    }
    if (carry)
        t->d[t->nd++] = (uint8_t)carry;
    t->exp--;
    dec_trim(t);
    return 0;
}

static int32_t dec_adjusted(const dec_t* x)
{
    return x->nd ? x->exp + (int32_t)x->nd - 1 : 0;
}

static int dec_isqrt(const uint8_t* msd, uint32_t nd,
    uint8_t* root, uint32_t* rln,
    uint8_t* rem, uint32_t* rmn)
{
    uint32_t S = (nd + 1) / 2, rl = 0, rn = 0, i;
    uint8_t* b20;

    *rln = 0;
    *rmn = 0;
    b20 = (uint8_t*)calloc(S + 4, 1);
    if (!b20)
        return -1;
    for (i = 0; i < S; i++) {
        int hd, ld, d, carry;
        uint32_t k, bl;
        if ((nd & 1) && i == 0) {
            hd = 0;
            ld = msd[0];
        } else if (nd & 1) {
            hd = msd[2 * i - 1];
            ld = msd[2 * i];
        } else {
            hd = msd[2 * i];
            ld = msd[2 * i + 1];
        }
        if (rn)
            memmove(rem + 2, rem, rn);
        rem[0] = (uint8_t)ld;
        rem[1] = (uint8_t)hd;
        rn += 2;
        while (rn && rem[rn - 1] == 0)
            rn--;
        carry = 0;
        for (k = 0; k < rl; k++) {
            int s2 = root[k] * 2 + carry;
            b20[k] = (uint8_t)(s2 % 10);
            carry = s2 / 10;
        }
        bl = rl;
        if (carry)
            b20[bl++] = (uint8_t)carry;
        if (bl) {
            memmove(b20 + 1, b20, bl);
            bl++;
        }
        b20[0] = 0;
        while (bl && b20[bl - 1] == 0)
            bl--;
        for (d = 9; d >= 0; d--) {
            uint8_t* prod = (uint8_t*)calloc(bl + 2, 1);
            uint32_t pl;
            if (!prod) {
                free(b20);
                return -1;
            }
            carry = d * d;
            for (k = 0; k < bl; k++) {
                int s2 = b20[k] * d + carry;
                prod[k] = (uint8_t)(s2 % 10);
                carry = s2 / 10;
            }
            pl = bl;
            while (carry) {
                prod[pl++] = (uint8_t)(carry % 10);
                carry /= 10;
            }
            while (pl && prod[pl - 1] == 0)
                pl--;
            if (dec_ge(rem, rn, prod, pl)) {
                rn = dec_sub_into(rem, rn, prod, pl);
                free(prod);
                break;
            }
            free(prod);
        }
        memmove(root + 1, root, rl);
        root[0] = (uint8_t)(d < 0 ? 0 : d);
        rl++;
    }
    free(b20);
    *rln = rl;
    *rmn = rn;
    return 0;
}

static int dec_on_tie(const dec_t* x, uint32_t prec)
{
    int32_t cut, p;

    if ((int32_t)x->nd < (int32_t)prec + 1)
        return 0;
    cut = x->exp + (int32_t)x->nd - (int32_t)prec;
    if (dec_at(x, cut - 1) != 5)
        return 0;
    for (p = x->exp; p < cut - 1; p++)
        if (dec_at(x, p))
            return 0;
    return 1;
}

static int dec_sqrt(dec_t* r, const dec_t* a, uint32_t prec)
{
    uint32_t D, S, s = 0, rl, rn, ntot, k;
    int32_t e = a->exp;
    int t = (int)(e & 1);
    uint8_t *md, *root, *rem;
    int rc = -1;

    if (a->nd == 0) {
        r->nd = 0;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    if (a->sign)
        return DEC_E_INVALID;
    ntot = a->nd + (uint32_t)t;
    if (ntot < 2u * prec + 2u)
        s = (2u * prec + 2u - ntot + 1) / 2;
    D = ntot + 2u * s;
    S = (D + 1) / 2;
    if (D > DEC_MAX_DIGITS)
        return -1;
    md = (uint8_t*)calloc(D + 2, 1);
    root = (uint8_t*)calloc(S + 2, 1);
    rem = (uint8_t*)calloc(D + 4, 1);
    if (!md || !root || !rem)
        goto done;
    for (k = 0; k < a->nd; k++)
        md[k] = a->d[a->nd - 1 - k];
    if (dec_isqrt(md, D, root, &rl, rem, &rn) < 0)
        goto done;
    if (dec_grow(r, rl ? rl : 1) < 0)
        goto done;
    memcpy(r->d, root, rl);
    r->nd = rl;
    r->exp = (e - t) / 2 - (int32_t)s;
    r->sign = 0;
    dec_trim(r);
    if (r->nd > prec + 1) {
        if (rn)
            r->d[0] = (uint8_t)(r->d[0] | 1);
        if (dec_round_sig(r, prec, RND_HALF_EVEN, 0) < 0)
            goto done;
    } else {
        if (dec_round_sig(r, prec, RND_HALF_EVEN, rn != 0) < 0)
            goto done;
    }
    rc = 0;
done:
    free(md);
    free(root);
    free(rem);
    return rc;
}

static int dec_atanh2(dec_t* r, const dec_t* u, uint32_t w)
{
    dec_t uu, term, sum, t, acc;
    uint32_t k;
    int rc = -1;

    dec_init(&uu);
    dec_init(&term);
    dec_init(&sum);
    dec_init(&t);
    dec_init(&acc);
    if (dec_mul(&uu, u, u) < 0 || dec_round_sig(&uu, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (dec_copy(&term, u) < 0 || dec_copy(&sum, u) < 0)
        goto done;
    for (k = 1;; k++) {
        if (dec_mul_into(&term, &uu) < 0)
            goto done;
        if (dec_round_sig(&term, w, RND_HALF_EVEN, 0) < 0)
            goto done;
        if (dec_is_zero(&term))
            break;
        if (dec_adjusted(&term) + (int32_t)w + 3 < dec_adjusted(&sum))
            break;
        {
            char buf[16];
            dec_t kd;
            snprintf(buf, sizeof buf, "%u", 2 * k + 1);
            dec_init(&kd);
            if (dec_parse(&kd, buf, strlen(buf)) < 0) {
                dec_free(&kd);
                goto done;
            }
            if (dec_div(&t, &term, &kd, w, RND_HALF_EVEN) < 0) {
                dec_free(&kd);
                goto done;
            }
            dec_free(&kd);
        }
        if (dec_add(&acc, &sum, &t) < 0 || dec_copy(&sum, &acc) < 0
            || dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
            goto done;
    }
    if (dec_add(&acc, &sum, &sum) < 0 || dec_copy(r, &acc) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&uu);
    dec_free(&term);
    dec_free(&sum);
    dec_free(&t);
    dec_free(&acc);
    return rc;
}

static int dec_small(dec_t* x, int64_t v)
{
    char buf[24];
    int n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

    dec_init(x);
    if (u == 0)
        return 0;
    while (u > 0 && n < 24) {
        buf[n++] = (char)(u % 10u);
        u /= 10u;
    }
    if (u != 0 || dec_grow(x, (uint32_t)n) < 0)
        return -1;
    memcpy(x->d, buf, (size_t)n);
    x->nd = (uint32_t)n;
    x->exp = 0;
    x->sign = (uint8_t)neg;
    return 0;
}

static int dec_const_ln2(dec_t* r, uint32_t w)
{
    dec_t u, one, three;
    int rc;

    dec_init(&u);
    dec_init(&one);
    dec_init(&three);
    if (dec_grow(&one, 1) < 0 || dec_grow(&three, 1) < 0)
        return -1;
    one.d[0] = 1;
    one.nd = 1;
    three.d[0] = 3;
    three.nd = 1;
    rc = dec_div(&u, &one, &three, w, RND_HALF_EVEN);
    if (rc == 0)
        rc = dec_atanh2(r, &u, w);
    dec_free(&u);
    dec_free(&one);
    dec_free(&three);
    return rc;
}

static int dec_const_ln10(dec_t* r, uint32_t w)
{
    dec_t u, l2, l9;
    int rc = -1;

    dec_init(&u);
    dec_init(&l2);
    dec_init(&l9);
    if (dec_const_ln2(&l2, w) < 0)
        goto done;
    {
        dec_t one, nine;
        dec_init(&one);
        dec_init(&nine);
        if (dec_small(&one, 1) < 0 || dec_small(&nine, 9) < 0) {
            dec_free(&one);
            dec_free(&nine);
            goto done;
        }
        if (dec_div(&u, &one, &nine, w + 2, RND_HALF_EVEN) < 0) {
            dec_free(&one);
            dec_free(&nine);
            goto done;
        }
        dec_free(&one);
        dec_free(&nine);
    }
    if (dec_atanh2(&l9, &u, w + 2) < 0)
        goto done;
    if (dec_add(&u, &l9, &l2) < 0
        || dec_add(&l9, &u, &l2) < 0
        || dec_add(&u, &l9, &l2) < 0)
        goto done;
    if (dec_copy(r, &u) < 0 || dec_round_sig(r, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&u);
    dec_free(&l2);
    dec_free(&l9);
    return rc;
}

static int dec_ln_at(dec_t* r, const dec_t* a, uint32_t prec, uint32_t w,
    int* tie)
{
    dec_t t, num, den, u, acc, c, small, sum;
    int32_t E;
    uint32_t k = 0;
    int rc = -1, direct = 0;

    dec_init(&t);
    dec_init(&num);
    dec_init(&den);
    dec_init(&u);
    dec_init(&acc);
    dec_init(&c);
    dec_init(&small);
    dec_init(&sum);
    if (dec_copy(&t, a) < 0)
        goto done;
    {
        dec_t lo, hi;
        dec_init(&lo);
        dec_init(&hi);
        if (dec_parse(&lo, "0.5", 3) < 0 || dec_small(&hi, 2) < 0) {
            dec_free(&lo);
            dec_free(&hi);
            goto done;
        }
        if (dec_cmp(&t, &lo) >= 0 && dec_cmp(&t, &hi) < 0)
            direct = 1;
        dec_free(&lo);
        dec_free(&hi);
    }
    if (!direct && dec_round_sig(&t, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (direct) {
        E = 0;
    } else {
        E = dec_adjusted(&t);
        t.exp -= E;
        {
            dec_t two;
            if (dec_small(&two, 2) < 0) {
                dec_free(&two);
                goto done;
            }
            while (dec_cmp(&t, &two) >= 0) {
                dec_free(&two);
                if (dec_halve(&t) < 0)
                    goto done;
                k++;
                if (dec_small(&two, 2) < 0) {
                    dec_free(&two);
                    goto done;
                }
            }
            dec_free(&two);
        }
    }
    if (dec_grow(&num, w + 2) < 0 || dec_grow(&den, w + 2) < 0)
        goto done;
    {
        dec_t one;
        if (dec_small(&one, 1) < 0) {
            dec_free(&one);
            goto done;
        }
        if (dec_sub(&num, &t, &one) < 0 || dec_add(&den, &t, &one) < 0) {
            dec_free(&one);
            goto done;
        }
        dec_free(&one);
    }
    if (dec_div(&u, &num, &den, w + 2, RND_HALF_EVEN) < 0)
        goto done;
    if (dec_atanh2(&acc, &u, w + 2) < 0 || dec_round_sig(&acc, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (k) {
        if (dec_const_ln2(&c, w) < 0)
            goto done;
        if (dec_small(&small, (int64_t)k) < 0)
            goto done;
        if (dec_mul_into(&c, &small) < 0)
            goto done;
        if (dec_add(&sum, &acc, &c) < 0 || dec_copy(&acc, &sum) < 0)
            goto done;
    }
    if (E != 0) {
        if (dec_const_ln10(&c, w) < 0)
            goto done;
        if (dec_small(&small, (int64_t)E) < 0)
            goto done;
        if (dec_mul_into(&c, &small) < 0)
            goto done;
        if (dec_round_sig(&c, w, RND_HALF_EVEN, 0) < 0)
            goto done;
        if (dec_add(&sum, &acc, &c) < 0 || dec_copy(&acc, &sum) < 0)
            goto done;
    }
    if (tie)
        *tie = dec_on_tie(&acc, prec);
    if (dec_round_sig(&acc, prec, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (dec_copy(r, &acc) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&t);
    dec_free(&num);
    dec_free(&den);
    dec_free(&u);
    dec_free(&acc);
    dec_free(&c);
    dec_free(&small);
    dec_free(&sum);
    return rc;
}

static int dec_ln(dec_t* r, const dec_t* a, uint32_t prec)
{
    uint32_t w = prec + 24;
    int tie = 0, attempt, rc;

    if (a->nd == 0)
        return DEC_E_INVALID;
    if (a->sign)
        return DEC_E_INVALID;
    if ((double)4.2 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    for (attempt = 0; attempt < 3; attempt++) {
        rc = dec_ln_at(r, a, prec, w, &tie);
        if (rc != 0 || !tie || attempt == 2)
            break;
        if ((double)4.2 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
    return rc;
}

static int dec_log10(dec_t* r, const dec_t* a, uint32_t prec)
{
    dec_t L, c, acc;
    uint32_t w = prec + 24;
    int tie, attempt, rc = -1;

    if (a->nd == 0 || a->sign)
        return DEC_E_INVALID;
    if ((double)4.6 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    dec_init(&L);
    dec_init(&c);
    dec_init(&acc);
    for (attempt = 0; attempt < 3; attempt++) {
        tie = 0;
        if (dec_ln_at(&L, a, w, w, &tie) < 0)
            goto done;
        if (dec_const_ln10(&c, w) < 0)
            goto done;
        if (dec_div(&acc, &L, &c, prec, RND_HALF_EVEN) < 0)
            goto done;
        if (!dec_on_tie(&acc, prec) || attempt == 2) {
            if (dec_copy(r, &acc) < 0)
                goto done;
            rc = 0;
            goto done;
        }
        if ((double)4.6 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
done:
    dec_free(&L);
    dec_free(&c);
    dec_free(&acc);
    return rc;
}

static int dec_exp(dec_t* r, const dec_t* a, uint32_t prec)
{
    dec_t t, sum, term, kk, tmp;
    uint32_t w, n = 0, k;
    int neg, rc = -1, attempt;
    double est, amp;

    if (a->nd == 0) {
        if (dec_grow(r, 1) < 0)
            return -1;
        r->d[0] = 1;
        r->nd = 1;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    neg = a->sign;
    {
        double lead = (double)a->d[a->nd - 1];
        double adj = (double)(a->exp + (int32_t)a->nd - 1);
        if (a->nd > 1)
            lead += (double)a->d[a->nd - 2] / 10.0;
        amp = adj + log10(lead);
        if (adj >= 7.0) {
            if (!neg)
                return DEC_E_OVERFLOW;
            r->nd = 0;
            r->exp = 0;
            r->sign = 0;
            return 0;
        }
        {
            double xd = 0.0;
            int i, top = a->nd > 17 ? 17 : (int)a->nd;
            for (i = 0; i < top; i++)
                xd += (double)a->d[a->nd - 1 - i]
                    * pow(10.0, (double)(adj - (double)i));
            est = xd * 0.4342944819032518;
        }
    }
    if (!neg && est > 999998.0) {
        dec_t ln10c, lim;
        dec_init(&ln10c);
        dec_init(&lim);
        if (dec_const_ln10(&ln10c, prec + 40) < 0) {
            dec_free(&ln10c);
            dec_free(&lim);
            return -1;
        }
        if (dec_small(&lim, 1000000) < 0) {
            dec_free(&ln10c);
            dec_free(&lim);
            return -1;
        }
        if (dec_mul_into(&lim, &ln10c) < 0) {
            dec_free(&ln10c);
            dec_free(&lim);
            return -1;
        }
        dec_free(&ln10c);
        {
            int c2 = dec_cmp(a, &lim);
            dec_free(&lim);
            if (c2 > 0)
                return DEC_E_OVERFLOW;
        }
    }
    if (neg && est > 999999.0 + (double)prec - 1.0 + 0.30103) {
        r->nd = 0;
        r->exp = 0;
        r->sign = 0;
        return 0;
    }
    w = prec + 12u + (uint32_t)(amp > 0.0 ? amp : 0.0) + 2u;
    if (w > 5100u)
        w = 5100u;
    if ((double)2.6 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    dec_init(&t);
    dec_init(&sum);
    dec_init(&term);
    dec_init(&kk);
    dec_init(&tmp);
    for (attempt = 0; attempt < 3; attempt++) {
        dec_free(&t);
        dec_free(&sum);
        dec_free(&term);
        dec_free(&tmp);
        dec_init(&t);
        dec_init(&sum);
        dec_init(&term);
        dec_init(&tmp);
        n = 0;
        if (dec_copy(&t, a) < 0)
            goto done;
        t.sign = 0;
        if (dec_round_sig(&t, w, RND_HALF_EVEN, 0) < 0)
            goto done;
        {
            dec_t one8th;
            if (dec_parse(&one8th, "0.125", 5) < 0) {
                dec_free(&one8th);
                goto done;
            }
            while (dec_cmp(&t, &one8th) > 0) {
                dec_free(&one8th);
                if (dec_halve(&t) < 0) {
                    dec_free(&one8th);
                    goto done;
                }
                n++;
                if (dec_parse(&one8th, "0.125", 5) < 0) {
                    dec_free(&one8th);
                    goto done;
                }
            }
            dec_free(&one8th);
        }
        if (dec_grow(&sum, 1) < 0 || dec_grow(&term, 1) < 0)
            goto done;
        sum.d[0] = 1;
        sum.nd = 1;
        term.d[0] = 1;
        term.nd = 1;
        for (k = 1;; k++) {
            if (dec_mul_into(&term, &t) < 0)
                goto done;
            if (dec_round_sig(&term, w, RND_HALF_EVEN, 0) < 0)
                goto done;
            if (neg)
                term.sign = (uint8_t)!term.sign;
            if (dec_small(&kk, (int64_t)k) < 0)
                goto done;
            if (dec_div(&tmp, &term, &kk, w, RND_HALF_EVEN) < 0)
                goto done;
            dec_free(&term);
            if (dec_copy(&term, &tmp) < 0)
                goto done;
            if (dec_is_zero(&term))
                break;
            if (dec_adjusted(&term) + (int32_t)w + 3 < dec_adjusted(&sum))
                break;
            if (dec_add(&tmp, &sum, &term) < 0 || dec_copy(&sum, &tmp) < 0
                || dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
                goto done;
        }
        while (n--) {
            if (dec_mul_into(&sum, &sum) < 0)
                goto done;
            if (dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
                goto done;
        }
        if (!dec_on_tie(&sum, prec) || attempt == 2)
            break;
        if ((double)2.6 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
    if (dec_round_sig(&sum, prec, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (sum.nd && sum.exp + (int32_t)sum.nd - 1 < -999999) {
        if (dec_round_at(&sum, -999999 - (int32_t)prec + 1,
                RND_HALF_EVEN, 0)
            < 0)
            goto done;
    }
    if (dec_copy(r, &sum) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&t);
    dec_free(&sum);
    dec_free(&term);
    dec_free(&kk);
    dec_free(&tmp);
    return rc;
}

static JSValue dyn_dec_math(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;
    uint32_t prec;
    int mode, rc;
    static const char* const NAMES[] = { "sqrt", "exp", "ln", "log10" };

    if (!a)
        return JS_EXCEPTION;
    if (dyn_prec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &prec) < 0
        || dyn_rnd_mode(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if ((magic == 2 || magic == 3) && dec_is_zero(a))
        return JS_ThrowRangeError(ctx,
            "Decimal.%s: the logarithm of zero is -Infinity, and this module "
            "has no Infinity -- it would not round-trip",
            NAMES[magic]);
    r = dyn_dec_alloc(ctx);
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        rc = dec_sqrt(r, a, prec);
        break;
    case 1:
        rc = dec_exp(r, a, prec);
        break;
    case 2:
        rc = dec_ln(r, a, prec);
        break;
    default:
        rc = dec_log10(r, a, prec);
        break;
    }
    if (rc == DEC_E_INVALID) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx,
            magic == 0
                ? "Decimal.sqrt: the square root of a negative number is not "
                  "a decimal"
                : "Decimal.%s: the logarithm of a negative number is not a "
                  "decimal",
            NAMES[magic]);
    }
    if (rc == DEC_E_OVERFLOW) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.exp: e^x overflows the context (the adjusted exponent "
            "passes 999999, the same Emax python decimal's default context "
            "sets); exp of a very negative x is 0, not an error");
    }
    if (rc == DEC_E_BUDGET) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.%s: exceeds the operation budget at this precision (the "
            "series core is schoolbook; sqrt has no such cap)",
            NAMES[magic]);
    }
    if (rc < 0) {
        dec_free(r);
        free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_integral(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t, *r;
    static const int MODES[] = { RND_FLOOR, RND_CEIL, RND_DOWN };

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    dec_init(&t);
    if (dec_copy(&t, a) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_round_at(&t, 0, MODES[magic], 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = dyn_dec_alloc(ctx);
    if (!r) {
        dec_free(&t);
        return JS_EXCEPTION;
    }
    if (dec_copy(r, &t) < 0) {
        dec_free(&t);
        dec_free(r);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&t);
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

static JSValue dyn_dec_divmod(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b, *q, *rr;
    JSValue arr, qv, rv;
    int rc;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal arithmetic needs an operand");
    if (dyn_dec_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    q = dyn_dec_alloc(ctx);
    rr = dyn_dec_alloc(ctx);
    if (!q || !rr) {
        dec_free(&b);
        dec_free(q);
        free(q);
        dec_free(rr);
        free(rr);
        return JS_EXCEPTION;
    }
    rc = dec_int_divmod(q, rr, a, &b);
    dec_free(&b);
    if (rc == -2) {
        dec_free(q);
        free(q);
        dec_free(rr);
        free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc == DEC_E_TOOSLOW) {
        dec_free(q);
        free(q);
        dec_free(rr);
        free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
                                       "exact operation (divide is limited to %u cells)",
            (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc < 0) {
        dec_free(q);
        free(q);
        dec_free(rr);
        free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
            DEC_MAX_DIGITS);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dec_free(q);
        free(q);
        dec_free(rr);
        free(rr);
        return arr;
    }
    qv = dyn_dec_wrap(ctx, JS_UNDEFINED, q);
    rv = dyn_dec_wrap(ctx, JS_UNDEFINED, rr);
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, qv, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueUint32(ctx, arr, 1, rv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_dec_to_bigint(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    uint64_t v = 0;
    int i;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    dec_init(&t);
    if (dec_copy(&t, a) < 0 || dec_round_at(&t, 0, RND_DOWN, 0) < 0) {
        dec_free(&t);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dec_cmp(a, &t) != 0) {
        dec_free(&t);
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the value has a fractional part");
    }
    if (t.nd > 19 || (t.nd && t.exp > 0 && (uint32_t)t.nd + (uint32_t)t.exp > 19u)) {
        dec_free(&t);
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the magnitude exceeds the int64 range this "
            "bridge returns (|x| must stay below 2^63)");
    }
    for (i = (int)t.nd - 1; i >= 0; i--)
        v = v * 10 + (uint64_t)t.d[i];
    for (i = 0; i < (int)t.exp; i++)
        v *= 10;
    dec_free(&t);
    if (!a->sign && v > (uint64_t)INT64_MAX) {
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the magnitude exceeds the int64 range this "
            "bridge returns (|x| must stay below 2^63)");
    }
    if (a->sign && v > (uint64_t)INT64_MAX + 1u) {
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the magnitude exceeds the int64 range this "
            "bridge returns (|x| must stay below 2^63)");
    }
    return JS_NewBigInt64(ctx, a->sign ? (int64_t)(0u - v) : (int64_t)v);
}

static JSValue dyn_dec_const(JSContext* ctx, const char* text)
{
    dec_t* x = dyn_dec_alloc(ctx);

    if (!x)
        return JS_EXCEPTION;
    if (dec_parse(x, text, strlen(text)) < 0) {
        dec_free(x);
        free(x);
        return JS_ThrowInternalError(ctx, "decimal: bad constant %s", text);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, x);
}

static const JSCFunctionListEntry dyn_dec_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_dec_arith, OP_ADD),
    JS_CFUNC_MAGIC_DEF("sub", 1, dyn_dec_arith, OP_SUB),
    JS_CFUNC_MAGIC_DEF("mul", 1, dyn_dec_arith, OP_MUL),
    JS_CFUNC_MAGIC_DEF("div", 1, dyn_dec_arith, OP_DIV),
    JS_CFUNC_MAGIC_DEF("mod", 1, dyn_dec_arith, OP_MOD),
    JS_CFUNC_DEF("divmod", 1, dyn_dec_divmod),
    JS_CFUNC_DEF("pow", 1, dyn_dec_pow),
    JS_CFUNC_MAGIC_DEF("sqrt", 0, dyn_dec_math, 0),
    JS_CFUNC_MAGIC_DEF("exp", 0, dyn_dec_math, 1),
    JS_CFUNC_MAGIC_DEF("ln", 0, dyn_dec_math, 2),
    JS_CFUNC_MAGIC_DEF("log10", 0, dyn_dec_math, 3),
    JS_CFUNC_MAGIC_DEF("floor", 0, dyn_dec_integral, 0),
    JS_CFUNC_MAGIC_DEF("ceil", 0, dyn_dec_integral, 1),
    JS_CFUNC_MAGIC_DEF("trunc", 0, dyn_dec_integral, 2),
    JS_CFUNC_DEF("toBigInt", 0, dyn_dec_to_bigint),
    JS_CFUNC_MAGIC_DEF("abs", 0, dyn_dec_unary, 0),
    JS_CFUNC_MAGIC_DEF("neg", 0, dyn_dec_unary, 1),
    JS_CFUNC_MAGIC_DEF("cmp", 1, dyn_dec_cmp, 0),
    JS_CFUNC_MAGIC_DEF("equals", 1, dyn_dec_cmp, 1),
    JS_CFUNC_MAGIC_DEF("round", 0, dyn_dec_round, 0),
    JS_CFUNC_MAGIC_DEF("toFixed", 0, dyn_dec_round, 1),
    JS_CFUNC_MAGIC_DEF("toString", 0, dyn_dec_query, 0),
    JS_CFUNC_MAGIC_DEF("toJSON", 0, dyn_dec_query, 0),
    JS_CFUNC_MAGIC_DEF("toNumber", 0, dyn_dec_query, 1),
    JS_CFUNC_MAGIC_DEF("isZero", 0, dyn_dec_query, 2),
    JS_CFUNC_MAGIC_DEF("sign", 0, dyn_dec_query, 3),
    JS_CFUNC_MAGIC_DEF("digits", 0, dyn_dec_query, 4),
};

typedef struct {
    int64_t amount;
    char code[4];
    uint8_t minor;
} money_t;

static JSClassID dyn_money_class_id;

static void dyn_money_dispose(void* p)
{
    free(p);
}

static void dyn_money_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_money_class_id));
}

static const JSClassDef dyn_money_class = {
    "Money",
    .finalizer = dyn_money_finalizer,
};

static const struct {
    const char* code;
    uint8_t minor;
} MONEY_MINOR[] = {
    { "JPY", 0 },
    { "KRW", 0 },
    { "VND", 0 },
    { "CLP", 0 },
    { "ISK", 0 },
    { "PYG", 0 },
    { "RWF", 0 },
    { "UGX", 0 },
    { "VUV", 0 },
    { "XAF", 0 },
    { "XOF", 0 },
    { "XPF", 0 },
    { "DJF", 0 },
    { "GNF", 0 },
    { "KMF", 0 },
    { "MGA", 0 },
    { "BIF", 0 },
    { "BHD", 3 },
    { "IQD", 3 },
    { "JOD", 3 },
    { "KWD", 3 },
    { "LYD", 3 },
    { "OMR", 3 },
    { "TND", 3 },
};

static uint8_t money_minor(const char* code)
{
    size_t k;
    for (k = 0; k < countof(MONEY_MINOR); k++)
        if (strcmp(MONEY_MINOR[k].code, code) == 0)
            return MONEY_MINOR[k].minor;
    return 2;
}

static money_t* dyn_money_this(JSContext* ctx, JSValueConst t)
{
    return (money_t*)JS_GetOpaque2(ctx, t, dyn_money_class_id);
}

static JSValue dyn_money_new(JSContext* ctx, JSValueConst new_target,
    int64_t amount, const char* code, uint8_t minor)
{
    money_t* m = (money_t*)malloc(sizeof *m);

    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->amount = amount;
    m->minor = minor;
    memcpy(m->code, code, 3);
    m->code[3] = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_money_class_id, m, dyn_money_dispose);
}

static int money_code(JSContext* ctx, JSValueConst v, char out[4])
{
    const char* code = JS_ToCString(ctx, v);
    int k;

    if (!code)
        return -1;
    if (strlen(code) != 3) {
        JS_FreeCString(ctx, code);
        JS_ThrowRangeError(ctx, "Money: a currency is a 3-letter code");
        return -1;
    }
    for (k = 0; k < 3; k++) {
        char c = code[k];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') {
            JS_FreeCString(ctx, code);
            JS_ThrowRangeError(ctx, "Money: a currency is 3 letters");
            return -1;
        }
        out[k] = c;
    }
    out[3] = 0;
    JS_FreeCString(ctx, code);
    return 0;
}

static int money_minor_opt(JSContext* ctx, JSValueConst opts,
    const char* what, uint8_t base, uint8_t* minor);

static int money_amount(JSContext* ctx, JSValueConst v, int64_t* out)
{
    double d;

    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "Money: minorUnits is an integer count of the "
                               "smallest unit");
        return -1;
    }
    if (JS_ToInt64(ctx, out, v) < 0 || JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    if (d != (double)*out) {
        JS_ThrowRangeError(ctx, "Money: minorUnits must be an integer -- 1999 "
                                "is $19.99, and a fractional cent is not money");
        return -1;
    }
    return 0;
}

static JSValue dyn_money_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    int64_t amount;
    char up[4];
    uint8_t minor;

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "new Money(minorUnits, currency): a currency code is required");
    if (money_amount(ctx, argv[0], &amount) < 0 || money_code(ctx, argv[1], up) < 0)
        return JS_EXCEPTION;
    if (money_minor_opt(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "new Money",
            money_minor(up), &minor))
        return JS_EXCEPTION;
    return dyn_money_new(ctx, new_target, amount, up, minor);
}

static int money_pair(JSContext* ctx, JSValueConst a_val, JSValueConst b_val,
    money_t** a, money_t** b)
{
    *a = dyn_money_this(ctx, a_val);
    if (!*a)
        return -1;
    *b = JS_IsObject(b_val) ? (money_t*)JS_GetOpaque(b_val, dyn_money_class_id)
                            : NULL;
    if (!*b) {
        JS_ThrowTypeError(ctx, "Money: the operand must be a Money");
        return -1;
    }
    if (strcmp((*a)->code, (*b)->code) != 0) {
        JS_ThrowTypeError(ctx, "Money: cannot combine %s and %s",
            (*a)->code, (*b)->code);
        return -1;
    }
    if ((*a)->minor != (*b)->minor) {
        JS_ThrowTypeError(ctx, "Money: cannot combine %s amounts with %u and %u "
                               "minor digits; the scales must match",
            (*a)->code, (unsigned)(*a)->minor,
            (unsigned)(*b)->minor);
        return -1;
    }
    return 0;
}

static JSValue dyn_money_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    money_t *a, *b;
    int64_t r;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Money: this operation needs one argument");
    if (money_pair(ctx, this_val, argv[0], &a, &b) < 0)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewInt32(ctx, a->amount < b->amount ? -1 : a->amount > b->amount);
    if (magic == 3)
        return JS_NewBool(ctx, a->amount == b->amount);
    r = magic == 0 ? a->amount + b->amount : a->amount - b->amount;
    if ((magic == 0 && ((b->amount > 0 && r < a->amount) || (b->amount < 0 && r > a->amount)))
        || (magic == 1 && ((b->amount < 0 && r < a->amount) || (b->amount > 0 && r > a->amount))))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, JS_UNDEFINED, r, a->code, a->minor);
}

static JSValue dyn_money_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    money_t* a = dyn_money_this(ctx, this_val);
    int64_t k;
    double d;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &k, argv[0]) < 0)
        return JS_ThrowTypeError(ctx, "Money.mul(n): n must be an integer");
    if (JS_ToFloat64(ctx, &d, argv[0]) < 0)
        return JS_EXCEPTION;
    if (d != (double)k)
        return JS_ThrowRangeError(ctx, "Money.mul(n): n must be an integer -- "
                                       "use allocate() to split an amount");
    if (k != 0 && ((__int128)a->amount * (__int128)k > (__int128)INT64_MAX || (__int128)a->amount * (__int128)k < (__int128)INT64_MIN))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, JS_UNDEFINED, a->amount * k, a->code, a->minor);
}

static int64_t* money_weights(JSContext* ctx, JSValueConst arr, int64_t n,
    int64_t* total)
{
    int64_t *w = (int64_t*)calloc((size_t)n, sizeof *w), i;

    *total = 0;
    if (!w) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    {
        __int128 sum = 0;
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            double d;
            if (JS_IsException(e) || JS_ToFloat64(ctx, &d, e) < 0) {
                JS_FreeValue(ctx, e);
                free(w);
                return NULL;
            }
            JS_FreeValue(ctx, e);
            if (d >= 9223372036854775808.0) {
                free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: the weights overflow an int64");
                return NULL;
            }
            if (!(d >= 0) || d != (double)(int64_t)d) {
                free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: every share is a non-negative integer weight");
                return NULL;
            }
            w[i] = (int64_t)d;
            sum += (__int128)w[i];
            if (sum > (__int128)INT64_MAX) {
                free(w);
                JS_ThrowRangeError(ctx,
                    "Money.allocate: the weights overflow an int64");
                return NULL;
            }
        }
        *total = (int64_t)sum;
    }
    if (*total == 0) {
        free(w);
        JS_ThrowRangeError(ctx, "Money.allocate: the weights sum to zero");
        return NULL;
    }
    return w;
}

static JSValue dyn_money_allocate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    money_t* a = dyn_money_this(ctx, this_val);
    JSValue lv, out;
    int64_t n = 0, total = 0, given = 0, i, abs_amount, rem;
    int64_t* w;
    int neg;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "Money.allocate(shares): shares is an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 1 || n > 100000)
        return JS_ThrowRangeError(ctx, "Money.allocate: 1 to 100000 shares");
    w = money_weights(ctx, argv[0], n, &total);
    if (!w)
        return JS_EXCEPTION;
    if (a->amount == INT64_MIN) {
        free(w);
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    }
    neg = a->amount < 0;
    abs_amount = neg ? -a->amount : a->amount;
    for (i = 0; i < n; i++) {
        int64_t part = abs_amount % total, share;
        if (w[i] && (part > INT64_MAX / w[i] || abs_amount / total > INT64_MAX / w[i])) {
            free(w);
            return JS_ThrowRangeError(ctx,
                "Money.allocate: the weights overflow an int64");
        }
        share = abs_amount / total * w[i] + part * w[i] / total;
        given += share;
        w[i] = share;
    }
    for (i = 0, rem = abs_amount - given; i < n && rem > 0; i++, rem--)
        w[i]++;
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        free(w);
        return out;
    }
    for (i = 0; i < n; i++) {
        JSValue m = dyn_money_new(ctx, JS_UNDEFINED, neg ? -w[i] : w[i], a->code, a->minor);
        if (JS_IsException(m)
            || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)i, m,
                   JS_PROP_C_W_E)
                < 0) {
            free(w);
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    free(w);
    return out;
}

static JSValue dyn_money_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    money_t* a = dyn_money_this(ctx, this_val);
    char buf[64];
    int64_t whole, frac, div = 1;
    int k;

    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 1)
        return JS_NewInt64(ctx, a->amount);
    if (magic == 2)
        return JS_NewString(ctx, a->code);
    for (k = 0; k < a->minor; k++)
        div *= 10;
    whole = a->amount / div;
    frac = a->amount % div;
    if (frac < 0)
        frac = -frac;
    if (a->minor == 0)
        snprintf(buf, sizeof buf, "%lld", (long long)whole);
    else
        snprintf(buf, sizeof buf, "%s%lld.%0*lld",
            (a->amount < 0 && whole == 0) ? "-" : "", (long long)whole,
            (int)a->minor, (long long)frac);
    if (magic == 3) {
        static const struct {
            const char *code, *sym;
        } SYM[] = {
            { "USD", "$" },
            { "EUR", "\xE2\x82\xAC" },
            { "GBP", "\xC2\xA3" },
            { "JPY", "\xC2\xA5" },
            { "CNY", "\xC2\xA5" },
            { "INR", "\xE2\x82\xB9" },
            { "KRW", "\xE2\x82\xA9" },
            { "CAD", "CA$" },
            { "AUD", "A$" },
        };
        char out[96];
        size_t si;
        for (si = 0; si < countof(SYM); si++)
            if (strcmp(SYM[si].code, a->code) == 0) {
                snprintf(out, sizeof out, "%s%s%s", a->amount < 0 ? "-" : "",
                    SYM[si].sym, buf[0] == '-' ? buf + 1 : buf);
                return JS_NewString(ctx, out);
            }
        snprintf(out, sizeof out, "%s %s", buf, a->code);
        return JS_NewString(ctx, out);
    }
    if (magic == 4) {
        dec_t* r = dyn_dec_alloc(ctx);
        if (!r)
            return JS_EXCEPTION;
        if (dec_parse(r, buf, strlen(buf)) < 0) {
            dec_free(r);
            free(r);
            return JS_ThrowInternalError(ctx, "Money.toDecimal failed");
        }
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
    }
    return JS_NewString(ctx, buf);
}

static int money_opts_strict(JSContext* ctx, JSValueConst o)
{
    if (!JS_IsObject(o))
        return 0;
    {
        JSPropertyEnum* props = NULL;
        uint32_t nprops = 0, i;
        int bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char* name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) {
                bad = 1;
                break;
            }
            if (strcmp(name, "minorDigits") != 0) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: minorDigits)", name);
                bad = 1;
            }
            JS_FreeCString(ctx, name);
        }
        for (i = 0; i < nprops; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return bad ? -1 : 0;
    }
}

static int money_minor_opt(JSContext* ctx, JSValueConst opts,
    const char* what, uint8_t base, uint8_t* minor)
{
    JSValue v;
    int64_t md;
    double mdd;

    *minor = base;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", what);
        return -1;
    }
    if (money_opts_strict(ctx, opts))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "minorDigits");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_ToFloat64(ctx, &mdd, v) < 0
            || JS_ToInt64(ctx, &md, v) < 0 || mdd != (double)md) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "%s: minorDigits must be an integer", what);
            return -1;
        }
        if (md < 0 || md > 6) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "%s: minorDigits is 0 to 6", what);
            return -1;
        }
        *minor = (uint8_t)md;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int money_parse_decimal_str(JSContext* ctx, const char* s, size_t n,
    const char* what, uint8_t minor,
    int64_t* out)
{
    size_t i = 0;
    int neg = 0, nfrac = 0, comma = 0, seen_dot = 0, seen_digit = 0;
    __int128 acc = 0, frac = 0, val;

    if (n == 0) {
        JS_ThrowTypeError(ctx, "%s: the amount string is empty", what);
        return -1;
    }
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    for (; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (seen_dot || nfrac == -1) {
                JS_ThrowTypeError(ctx,
                    "%s: more than one decimal point in \"%s\"", what, s);
                return -1;
            }
            if (!seen_digit) {
                JS_ThrowTypeError(ctx,
                    "%s: \"%s\" needs a digit before the decimal point", what, s);
                return -1;
            }
            seen_dot = 1;
            nfrac = -1;
            continue;
        }
        if (c == ',') {
            if (seen_dot) {
                JS_ThrowTypeError(ctx,
                    "%s: a group separator after the decimal point in \"%s\"",
                    what, s);
                return -1;
            }
            comma = 1;
            continue;
        }
        if (c < '0' || c > '9') {
            JS_ThrowTypeError(ctx,
                "%s: \"%s\" is not a decimal amount", what, s);
            return -1;
        }
        if (seen_dot) {
            if (nfrac < 0)
                nfrac = 0;
            nfrac++;
            if (nfrac <= (int)minor) {
                frac = frac * 10 + (c - '0');
            } else if (c != '0') {
                JS_ThrowRangeError(ctx,
                    "%s: the amount has value past the %u minor digits -- "
                    "a fractional minor unit is not money",
                    what,
                    (unsigned)minor);
                return -1;
            }
        } else {
            seen_digit = 1;
            acc = acc * 10 + (c - '0');
            if (acc > (__int128)INT64_MAX + 1) {
                JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
                return -1;
            }
        }
    }
    if (nfrac == -1) {
        JS_ThrowTypeError(ctx, "%s: \"%s\" has a decimal point with no "
                               "fraction digits",
            what, s);
        return -1;
    }
    if (!seen_digit) {
        JS_ThrowTypeError(ctx, "%s: the amount string has no digits", what);
        return -1;
    }
    if (comma) {
        size_t g = 0, glen = 0, k;
        for (k = (s[0] == '-' ? 1 : 0); k < n && s[k] != '.'; k++) {
            if (s[k] == ',') {
                if (glen == 0 || glen > 3 || (g && glen != 3)) {
                    JS_ThrowTypeError(ctx,
                        "%s: \"%s\" has malformed ',' grouping", what, s);
                    return -1;
                }
                g++;
                glen = 0;
            } else {
                glen++;
            }
        }
        if (glen == 0 || glen > 3 || (g && glen != 3)) {
            JS_ThrowTypeError(ctx,
                "%s: \"%s\" has malformed ',' grouping", what, s);
            return -1;
        }
    }
    {
        int k;
        for (k = 0; k < (int)minor; k++)
            acc *= 10;
        for (k = nfrac; k < (int)minor; k++)
            frac *= 10;
    }
    val = acc + frac;
    if (neg) {
        if (val > (__int128)INT64_MAX + 1) {
            JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            return -1;
        }
        *out = (int64_t)-val;
    } else {
        if (val > (__int128)INT64_MAX) {
            JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            return -1;
        }
        *out = (int64_t)val;
    }
    return 0;
}

static int money_factory_args(JSContext* ctx, JSValueConst csv,
    JSValueConst optsv, const char* what,
    char up[4], uint8_t* minor)
{
    if (!JS_IsString(csv)) {
        JS_ThrowTypeError(ctx,
            "%s: a 3-letter currency code is required", what);
        return -1;
    }
    if (money_code(ctx, csv, up) < 0)
        return -1;
    return money_minor_opt(ctx, optsv, what, money_minor(up), minor);
}

static JSValue dyn_money_from_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    const char* s;
    size_t n;
    int64_t amount;
    (void)this_val;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Money.fromString(str, currency[, opts])");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromString", up, &minor))
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (money_parse_decimal_str(ctx, s, n, "Money.fromString", minor, &amount)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, s);
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

static JSValue dyn_money_from_decimal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    dec_t* x;
    __int128 acc = 0;
    int32_t p, top;
    int64_t amount;
    (void)this_val;

    if (argc < 2 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    x = (dec_t*)JS_GetOpaque(argv[0], dyn_dec_class_id);
    if (!x)
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromDecimal", up, &minor))
        return JS_EXCEPTION;
    if (dec_is_zero(x)) {
        amount = 0;
    } else {
        top = x->exp + (int32_t)x->nd - 1;
        for (p = x->exp; p < -(int32_t)minor && p <= top; p++)
            if (dec_at(x, p) != 0)
                return JS_ThrowRangeError(ctx,
                    "Money.fromDecimal: the Decimal has fraction digits past "
                    "the %u minor digits -- a fractional minor unit is not "
                    "money",
                    (unsigned)minor);
        if (top >= 19)
            return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        for (p = top; p >= -(int32_t)minor; p--) {
            acc = acc * 10 + dec_at(x, p);
            if (acc > (__int128)INT64_MAX + 1)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        }
        if (x->sign) {
            if (acc > (__int128)INT64_MAX + 1)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            amount = (int64_t)-acc;
        } else {
            if (acc > (__int128)INT64_MAX)
                return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
            amount = (int64_t)acc;
        }
    }
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

static JSValue dyn_money_from_minor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char up[4];
    uint8_t minor;
    int64_t amount;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Money.fromMinor(n, currency[, opts])");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
            "Money.fromMinor", up, &minor))
        return JS_EXCEPTION;
    if (JS_IsString(argv[0])) {
        const char* s;
        size_t n;
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (money_parse_decimal_str(ctx, s, n, "Money.fromMinor", 0, &amount)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, s);
    } else if (JS_IsBigInt(ctx, argv[0])) {
        const char* s;
        size_t n, start = 0;
        int neg = 0, bad = 0;
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (n > 0 && s[0] == '-') {
            neg = 1;
            start = 1;
        }
        while (start < n && s[start] == '0')
            start++;
        if (n - start > 19)
            bad = 1;
        else if (n - start == 19) {
            const char* lim = neg ? "9223372036854775808" : "9223372036854775807";
            bad = memcmp(s + start, lim, 19) > 0;
        }
        JS_FreeCString(ctx, s);
        if (bad)
            return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
        if (JS_ToBigInt64(ctx, &amount, argv[0]))
            return JS_EXCEPTION;
    } else if (JS_IsNumber(argv[0])) {
        if (money_amount(ctx, argv[0], &amount))
            return JS_EXCEPTION;
    } else {
        return JS_ThrowTypeError(ctx,
            "Money.fromMinor(n): n must be a number, bigint or digit string");
    }
    return dyn_money_new(ctx, JS_UNDEFINED, amount, up, minor);
}

static const JSCFunctionListEntry dyn_money_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_money_op, 0),
    JS_CFUNC_MAGIC_DEF("sub", 1, dyn_money_op, 1),
    JS_CFUNC_MAGIC_DEF("cmp", 1, dyn_money_op, 2),
    JS_CFUNC_MAGIC_DEF("equals", 1, dyn_money_op, 3),
    JS_CFUNC_DEF("mul", 1, dyn_money_mul),
    JS_CFUNC_DEF("allocate", 1, dyn_money_allocate),
    JS_CFUNC_MAGIC_DEF("toString", 0, dyn_money_query, 0),
    JS_CFUNC_MAGIC_DEF("toJSON", 0, dyn_money_query, 0),
    JS_CFUNC_MAGIC_DEF("amount", 0, dyn_money_query, 1),
    JS_CFUNC_MAGIC_DEF("currency", 0, dyn_money_query, 2),
    JS_CFUNC_MAGIC_DEF("format", 0, dyn_money_query, 3),
    JS_CFUNC_MAGIC_DEF("toDecimal", 0, dyn_money_query, 4),
};

static int dyn_decimal_init_module(JSContext* ctx, JSModuleDef* m)
{
    static const struct {
        const char *name, *text;
    } DEC_CONSTS[] = {
        { "ZERO", "0" },
        { "ONE", "1" },
        { "TWO", "2" },
        { "TEN", "10" },
        { "NEG_ONE", "-1" },
    };
    unsigned k;

    if (dyn_register_plain_class(ctx, m, &dyn_dec_class_id, &dyn_dec_class,
            dyn_dec_proto, countof(dyn_dec_proto),
            dyn_dec_ctor, "Decimal")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_dec_class_id);
        JSValue ctor;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor))
            return -1;
        for (k = 0; k < countof(DEC_CONSTS); k++) {
            JSValue v = dyn_dec_const(ctx, DEC_CONSTS[k].text);
            if (JS_IsException(v) || JS_DefinePropertyValueStr(ctx, ctor, DEC_CONSTS[k].name, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_plain_class(ctx, m, &dyn_money_class_id,
            &dyn_money_class, dyn_money_proto,
            countof(dyn_money_proto), dyn_money_ctor,
            "Money")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_money_class_id);
        JSValue ctor;
        static const struct {
            const char* name;
            JSCFunction* fn;
            int arity;
        } MONEY_STATICS[] = {
            { "fromString", dyn_money_from_string, 2 },
            { "fromDecimal", dyn_money_from_decimal, 2 },
            { "fromMinor", dyn_money_from_minor, 2 },
        };
        unsigned mk;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor))
            return -1;
        for (mk = 0; mk < countof(MONEY_STATICS); mk++) {
            JSValue fn = JS_NewCFunction(ctx, MONEY_STATICS[mk].fn,
                MONEY_STATICS[mk].name,
                MONEY_STATICS[mk].arity);
            if (JS_IsException(fn) || JS_DefinePropertyValueStr(ctx, ctor, MONEY_STATICS[mk].name, fn, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}

int js_nat_init_decimal(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:decimal", dyn_decimal_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Decimal");
    return JS_AddModuleExport(ctx, m, "Money");
}

#endif
