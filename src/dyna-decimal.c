/* dyna:decimal -- exact decimal arithmetic, and an integral money type.
   The default context is IEEE 754-2008 decimal128 (34 significant digits,
   half-even), which is a standard rather than a house rule and is what Python's
   decimal, Java's BigDecimal and SQL NUMERIC also speak. Full API: see the module header. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DEC_INLINE     40               /* digits held with no allocation */
#define DEC_MAX_DIGITS 100000u          /* a bound, not a house style */
/* LATENCY caps. DEC_MAX_DIGITS alone permits 2.5e9 multiply cells and 1.0e10
   divide cells from a parsed string, and a divide cell is ~11 byte-operations.
   Both figures assume 1 byte-op/ns; re-derive if the kernels are ever timed. */
#define DEC_MAX_MUL_CELLS (1u << 26)    /* nd(a)*nd(b), ~67 ms worst case */
#define DEC_MAX_DIV_CELLS (1u << 23)    /* an*bn,       ~92 ms worst case */
#define DEC_E_TOOSLOW     (-3)          /* distinct from -1 (wide) and -2 (/0) */
/* Rendered characters. Separate from DEC_MAX_DIGITS because the notation is
   positional: the exponent, not the significand, sets the length. */
#define DEC_MAX_TEXT   1000000
#define DEC_DEF_PREC   34               /* decimal128 */

enum { RND_UP, RND_DOWN, RND_CEIL, RND_FLOOR,
       RND_HALF_UP, RND_HALF_DOWN, RND_HALF_EVEN, RND_HALF_ODD };

/* value = (-1)^sign * (sum d[i] * 10^i) * 10^exp, d little-endian, no leading
   zero digit. A digit array rather than 10^9 limbs: parse and format dominate
   real decimal workloads and both are then exact and trivial. */
typedef struct {
    int32_t  exp;
    uint32_t nd, cap;
    uint8_t  sign;
    uint8_t *d;
    uint8_t  inl[DEC_INLINE];
} dec_t;

static void dec_init(dec_t *x)
{
    x->exp = 0; x->nd = 0; x->sign = 0;
    x->d = x->inl; x->cap = DEC_INLINE;
}

static void dec_free(dec_t *x)
{
    if (x->d != x->inl)
        free(x->d);
    x->d = x->inl;
    x->cap = DEC_INLINE;
    x->nd = 0;
}

static int dec_grow(dec_t *x, uint32_t need)
{
    uint8_t *np;
    if (need <= x->cap)
        return 0;
    if (need > DEC_MAX_DIGITS)
        return -1;
    {   /* geometric, so parsing a long literal is not one malloc per digit */
        uint32_t want = x->cap + x->cap / 2 + 16;
        if (want > need) need = want;
        if (need > DEC_MAX_DIGITS) need = DEC_MAX_DIGITS;
    }
    np = (uint8_t *)malloc(need);
    if (!np)
        return -1;
    memcpy(np, x->d, x->nd);
    if (x->d != x->inl)
        free(x->d);
    x->d = np;
    x->cap = need;
    return 0;
}

/* Canonical: no leading zero digit, and zero is nd == 0. */
static void dec_trim(dec_t *x)
{
    while (x->nd && x->d[x->nd - 1] == 0)
        x->nd--;
    if (x->nd == 0) {
        x->exp = 0;
        x->sign = 0;
        return;
    }
    {   /* trailing zeros move into the exponent: 1.500 and 1.5 are one value */
        uint32_t k = 0;
        while (k < x->nd && x->d[k] == 0) k++;
        if (k) {
            memmove(x->d, x->d + k, x->nd - k);
            x->nd -= k;
            x->exp += (int32_t)k;
        }
    }
}

static int dec_is_zero(const dec_t *x) { return x->nd == 0; }

/* The digit at decimal position p (the 10^p place). */
static int dec_at(const dec_t *x, int32_t p)
{
    if (p < x->exp || p >= x->exp + (int32_t)x->nd)
        return 0;
    return x->d[p - x->exp];
}

static int dec_cmp_abs(const dec_t *a, const dec_t *b)
{
    int32_t ha, hb, lo, p;

    if (a->nd == 0) return b->nd == 0 ? 0 : -1;
    if (b->nd == 0) return 1;
    ha = a->exp + (int32_t)a->nd;
    hb = b->exp + (int32_t)b->nd;
    if (ha != hb) return ha < hb ? -1 : 1;
    lo = a->exp < b->exp ? a->exp : b->exp;
    for (p = ha - 1; p >= lo; p--) {
        int da = dec_at(a, p), db = dec_at(b, p);
        if (da != db) return da < db ? -1 : 1;
    }
    return 0;
}

static int dec_cmp(const dec_t *a, const dec_t *b)
{
    int c;
    if (a->nd == 0 && b->nd == 0) return 0;
    if (a->sign != b->sign) return a->sign ? -1 : 1;
    c = dec_cmp_abs(a, b);
    return a->sign ? -c : c;
}

static int dec_copy(dec_t *r, const dec_t *a)
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

/* |a| + |b| into r, which may alias neither. */
static int dec_add_abs(dec_t *r, const dec_t *a, const dec_t *b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    uint32_t n = (uint32_t)(hi - lo) + 1, i;
    int carry = 0;

    if (a->nd == 0) return dec_copy(r, b);
    if (b->nd == 0) return dec_copy(r, a);
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

/* |a| - |b| into r, with |a| >= |b|. */
static int dec_sub_abs(dec_t *r, const dec_t *a, const dec_t *b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    uint32_t n = (uint32_t)(hi - lo), i;
    int borrow = 0;

    if (b->nd == 0) return dec_copy(r, a);
    if (dec_grow(r, n ? n : 1) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        int p = (int)(lo + (int32_t)i);
        int s = dec_at(a, p) - dec_at(b, p) - borrow;
        if (s < 0) { s += 10; borrow = 1; } else { borrow = 0; }
        r->d[i] = (uint8_t)s;
    }
    r->nd = n;
    r->exp = lo;
    dec_trim(r);
    return 0;
}

static int dec_add(dec_t *r, const dec_t *a, const dec_t *b)
{
    if (a->sign == b->sign) {
        if (dec_add_abs(r, a, b) < 0) return -1;
        r->sign = r->nd ? a->sign : 0;
        return 0;
    }
    {
        int c = dec_cmp_abs(a, b);
        if (c == 0) { r->nd = 0; r->exp = 0; r->sign = 0; return 0; }
        if (c > 0) {
            if (dec_sub_abs(r, a, b) < 0) return -1;
            r->sign = r->nd ? a->sign : 0;
        } else {
            if (dec_sub_abs(r, b, a) < 0) return -1;
            r->sign = r->nd ? b->sign : 0;
        }
        return 0;
    }
}

static int dec_mul(dec_t *r, const dec_t *a, const dec_t *b)
{
    uint32_t n, i, j;
    uint32_t *wide;

    if (a->nd == 0 || b->nd == 0) {
        r->nd = 0; r->exp = 0; r->sign = 0;
        return 0;
    }
    n = a->nd + b->nd;
    if ((uint64_t)a->nd * (uint64_t)b->nd > DEC_MAX_MUL_CELLS)
        return DEC_E_TOOSLOW;
    if (n > DEC_MAX_DIGITS)
        return -1;
    /* DEFERRED CARRY. Normalising inside the inner loop costs an integer
       divide and a modulo per digit PAIR -- 576 of each for 24x24 digits, on
       the dependency chain. A u32 column accumulator holds up to 81*n before
       it can overflow (n is bounded by DEC_MAX_DIGITS), so the whole product
       is multiply-accumulate with no division at all, normalised once. */
    wide = (uint32_t *)calloc(n, sizeof *wide);
    if (!wide)
        return -1;
    for (i = 0; i < a->nd; i++) {
        uint32_t av = a->d[i];
        if (!av)
            continue;
        for (j = 0; j < b->nd; j++)
            wide[i + j] += av * (uint32_t)b->d[j];
    }
    if (dec_grow(r, n) < 0) { free(wide); return -1; }
    {
        uint32_t carry = 0;
        for (i = 0; i < n; i++) {
            uint32_t v = wide[i] + carry;
            r->d[i] = (uint8_t)(v % 10);
            carry = v / 10;
        }
        /* n = a->nd + b->nd is always one place wider than the product, so the
           final carry has somewhere to go and cannot be dropped. */
    }
    free(wide);
    r->nd = n;
    r->exp = a->exp + b->exp;
    r->sign = (uint8_t)(a->sign ^ b->sign);
    dec_trim(r);
    return 0;
}

/* ------------------------------------------------------------- rounding */

/* Round away every digit below decimal position `target`. `sticky` says a
   nonzero remainder was already discarded (division). ONE decision function
   over (guard, sticky, sign, parity), so no two modes can drift apart. */
/* Should the surviving digits be incremented? ONE function over (guard,
   sticky, sign, parity), so no two modes can drift apart. */
static int dec_round_up(int mode, int guard, int rest, int sign, int last)
{
    switch (mode) {
    case RND_UP:        return guard || rest;
    case RND_DOWN:      return 0;
    case RND_CEIL:      return (guard || rest) && !sign;
    case RND_FLOOR:     return (guard || rest) && sign;
    case RND_HALF_UP:   return guard >= 5;
    case RND_HALF_DOWN: return guard > 5 || (guard == 5 && rest);
    case RND_HALF_ODD:
    case RND_HALF_EVEN:
        if (guard != 5)
            return guard > 5;
        if (rest)
            return 1;
        return mode == RND_HALF_ODD ? (last % 2 == 0) : (last % 2 == 1);
    default:            return 0;
    }
}

/* Round away every digit below decimal position `target`. `sticky` says a
   nonzero remainder was already discarded (division). */
static int dec_round_at(dec_t *x, int32_t target, int mode, int sticky)
{
    int guard = 0, rest = sticky;
    int32_t p;

    if (x->nd == 0 && !sticky)
        return 0;
    if (target > x->exp && x->nd) {
        guard = dec_at(x, target - 1);
        for (p = x->exp; p < target - 1; p++)
            if (dec_at(x, p)) { rest = 1; break; }
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

static int dec_round_sig(dec_t *x, uint32_t prec, int mode, int sticky)
{
    if (x->nd <= prec && !sticky)
        return 0;
    if (x->nd == 0)
        return 0;
    return dec_round_at(x, x->exp + (int32_t)x->nd - (int32_t)prec, mode, sticky);
}

/* --------------------------------------------------------------- division */

/* Exact integer quotient and remainder of two digit arrays, schoolbook: one
   output digit per step, found by at most nine subtractions. Simple enough to
   be obviously right, which is what an arithmetic core needs first. */
/* Is rem (length rn) at least b (length bn)? */
static int dec_ge(const uint8_t *rem, uint32_t rn, const uint8_t *b, uint32_t bn)
{
    uint32_t k;
    if (rn != bn)
        return rn > bn;
    for (k = bn; k-- > 0;)
        if (rem[k] != b[k])
            return rem[k] > b[k];
    return 1;
}

/* rem -= b, returning the new length. */
static uint32_t dec_sub_into(uint8_t *rem, uint32_t rn, const uint8_t *b,
                             uint32_t bn)
{
    uint32_t k;
    int borrow = 0;
    for (k = 0; k < rn; k++) {
        int s = rem[k] - (k < bn ? b[k] : 0) - borrow;
        if (s < 0) { s += 10; borrow = 1; } else { borrow = 0; }
        rem[k] = (uint8_t)s;
    }
    while (rn && rem[rn - 1] == 0) rn--;
    return rn;
}

/* Exact integer quotient and remainder of two digit arrays, schoolbook: one
   output digit per step, found by at most nine subtractions. Simple enough to
   be obviously right, which is what an arithmetic core needs first. */
static int dec_divmod_int(uint8_t *q, uint32_t *qn, uint8_t *rem, uint32_t *rn,
                          const uint8_t *a, uint32_t an,
                          const uint8_t *b, uint32_t bn)
{
    int32_t i;
    uint32_t r = 0;

    *qn = an;
    memset(q, 0, an);
    for (i = (int32_t)an - 1; i >= 0; i--) {
        int d = 0;
        if (r) memmove(rem + 1, rem, r);
        rem[0] = a[i];
        r++;
        while (r && rem[r - 1] == 0) r--;
        while (dec_ge(rem, r, b, bn)) {
            r = dec_sub_into(rem, r, b, bn);
            if (++d > 9)
                return -1;              /* cannot happen; a wrong answer would */
        }
        q[i] = (uint8_t)d;
    }
    while (*qn && q[*qn - 1] == 0) (*qn)--;
    *rn = r;
    return 0;
}

/* a / b to `prec` significant digits. */
static int dec_div(dec_t *r, const dec_t *a, const dec_t *b, uint32_t prec, int mode)
{
    uint8_t *num = NULL, *q = NULL, *rem = NULL;
    uint32_t nn, qn = 0, rn = 0, extra;
    int32_t shift;
    int rc = -1;

    if (b->nd == 0)
        return -2;                      /* division by zero: the caller throws */
    if (a->nd == 0) {
        r->nd = 0; r->exp = 0; r->sign = 0;
        return 0;
    }
    /* Scale the numerator so the integer quotient carries prec+2 digits: one
       to round on and one so `rest` is not lost. */
    extra = prec + 2 + b->nd;
    if (extra > DEC_MAX_DIGITS || a->nd > DEC_MAX_DIGITS - extra)
        return -1;
    nn = a->nd + extra;
    num = (uint8_t *)calloc(nn, 1);
    q = (uint8_t *)calloc(nn, 1);
    rem = (uint8_t *)calloc(nn + 1, 1);
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
    r->exp = a->exp - b->exp - shift;
    r->sign = (uint8_t)(a->sign ^ b->sign);
    dec_trim(r);
    if (dec_round_sig(r, prec, mode, rn != 0) < 0)
        goto done;
    rc = 0;
done:
    free(num); free(q); free(rem);
    return rc;
}

/* The remainder of TRUNCATED division, which is what `%` means everywhere. */
static int dec_mod(dec_t *r, const dec_t *a, const dec_t *b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    uint32_t an, bn, qn, rn;
    uint8_t *ad = NULL, *bd = NULL, *q = NULL, *rem = NULL;
    int rc = -1;

    if (b->nd == 0)
        return -2;
    if (a->nd == 0) { r->nd = 0; r->exp = 0; r->sign = 0; return 0; }
    an = (uint32_t)(a->exp + (int32_t)a->nd - lo);
    bn = (uint32_t)(b->exp + (int32_t)b->nd - lo);
    if (an > DEC_MAX_DIGITS || bn > DEC_MAX_DIGITS)
        return -1;
    ad = (uint8_t *)calloc(an, 1);
    bd = (uint8_t *)calloc(bn, 1);
    q = (uint8_t *)calloc(an, 1);
    rem = (uint8_t *)calloc(an + 1, 1);
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
    r->sign = a->sign;                  /* the remainder takes the dividend's */
    dec_trim(r);
    rc = 0;
done:
    free(ad); free(bd); free(q); free(rem);
    return rc;
}

/* ------------------------------------------------------------ text in/out */

/* The [eE[+-]digits] tail, or -1. */
static int dec_parse_exp(const char *s, size_t n, size_t *pi, int32_t *exp)
{
    size_t i = *pi + 1;
    int esign = 0, edig = 0;
    int64_t ev = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) { esign = s[i] == '-'; i++; }
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

/* [+-]digits[.digits][eE[+-]digits], and nothing else. Hand-rolled because
   strtod reads LC_NUMERIC for the radix and accepts hex, whitespace and
   partial input -- none of which a value grammar may. */
static int dec_parse(dec_t *x, const char *s, size_t n)
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
            x->nd = nd;                 /* dec_grow copies nd bytes: keeping it
                                           current is what makes the copy whole */
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
        return -1;                      /* trailing text is not a number */
    x->nd = nd;
    x->exp = exp - (dot < 0 ? 0 : (int32_t)(nd - (uint32_t)dot));
    {   /* the parse read most-significant first; the store is little-endian */
        uint32_t a = 0, b = nd;
        while (a + 1 < b) {
            uint8_t t = x->d[a]; x->d[a] = x->d[b - 1]; x->d[b - 1] = t;
            a++; b--;
        }
    }
    dec_trim(x);
    return 0;
}

typedef struct { uint8_t *p; size_t n, cap; int oom, too_long; } db_t;

static void db_init(db_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = b->too_long = 0; }
static void db_free(db_t *b) { free(b->p); b->p = NULL; }

static void db_write(db_t *b, const void *p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        uint8_t *np;
        while (nc < b->n + n) nc += nc / 2 + 8;
        np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void db_put(db_t *b, uint8_t c) { db_write(b, &c, 1); }

/* Plain notation always: a money amount printed in exponent form is a bug
   report waiting to happen, and these values are bounded by DEC_MAX_DIGITS. */
/* Plain notation always: a money amount printed in exponent form is a bug
   report waiting to happen, and these values are bounded by DEC_MAX_DIGITS. */
static void dec_write(db_t *b, const dec_t *x)
{
    int32_t hi, lo, p;

    if (x->nd == 0) { db_put(b, '0'); return; }
    if (x->sign) db_put(b, '-');
    hi = x->exp + (int32_t)x->nd - 1;
    lo = x->exp;
    if (hi < 0) hi = 0;                 /* always one integer digit */
    if (lo > 0) lo = 0;                 /* an integer prints all its places */
    /* This notation is always positional, so the exponent -- not the digit
       count -- decides the length: "1e999999999" is one digit and a billion
       characters. Refuse rather than allocate it. */
    if ((int64_t)hi - lo >= DEC_MAX_TEXT) { b->too_long = 1; return; }
    for (p = hi; p >= lo; p--) {
        if (p == -1) db_put(b, '.');
        db_put(b, (uint8_t)('0' + dec_at(x, p)));
    }
}

/* Name the limit rather than reporting an allocation failure: the caller's
   input was legal to construct and only its rendering is absurd. */
static JSValue dyn_dec_too_long(JSContext *ctx)
{
    return JS_ThrowRangeError(ctx,
        "decimal: positional text would exceed %d characters; the exponent is "
        "too large to render", DEC_MAX_TEXT);
}

/* Exactly `dp` places after the point, zero-padded. */
static void dec_write_fixed(db_t *b, const dec_t *x, int32_t dp, int neg)
{
    int32_t p, hi;

    if (neg) db_put(b, '-');
    hi = x->nd ? x->exp + (int32_t)x->nd - 1 : 0;
    if (hi < 0) hi = 0;
    if ((int64_t)hi + (dp > 0 ? dp : 0) >= DEC_MAX_TEXT) { b->too_long = 1; return; }
    for (p = hi; p >= 0; p--)
        db_put(b, (uint8_t)('0' + dec_at(x, p)));
    if (dp > 0) {
        db_put(b, '.');
        for (p = -1; p >= -dp; p--)
            db_put(b, (uint8_t)('0' + dec_at(x, p)));
    }
}

/* ------------------------------------------------------------- the class */

static JSClassID dyn_dec_class_id;

static void dyn_dec_finalizer(JSRuntime *rt, JSValue val)
{
    dec_t *x = (dec_t *)JS_GetOpaque(val, dyn_dec_class_id);
    (void)rt;
    if (x) { dec_free(x); free(x); }
}

static const JSClassDef dyn_dec_class = {
    "Decimal", .finalizer = dyn_dec_finalizer,
};

static const char *const RND_NAMES[] = {
    "up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven", "halfOdd"
};

static int dyn_rnd_mode(JSContext *ctx, JSValueConst o, int *mode)
{
    JSValue v;
    const char *s;
    size_t k;

    *mode = RND_HALF_EVEN;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "rounding");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return 0; }
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

static int dyn_prec(JSContext *ctx, JSValueConst o, uint32_t *prec)
{
    JSValue v;
    int64_t p = DEC_DEF_PREC;

    *prec = DEC_DEF_PREC;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "precision");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return 0; }
    if (JS_ToInt64(ctx, &p, v) < 0) { JS_FreeValue(ctx, v); return -1; }
    JS_FreeValue(ctx, v);
    if (p < 1 || p > 5000) {
        JS_ThrowRangeError(ctx, "precision must be 1 to 5000 significant digits");
        return -1;
    }
    *prec = (uint32_t)p;
    return 0;
}

static dec_t *dyn_dec_alloc(JSContext *ctx)
{
    dec_t *x = (dec_t *)malloc(sizeof *x);
    if (!x) { JS_ThrowOutOfMemory(ctx); return NULL; }
    dec_init(x);
    return x;
}

static JSValue dyn_dec_wrap(JSContext *ctx, dec_t *x)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)dyn_dec_class_id);
    if (JS_IsException(obj)) {
        dec_free(x);
        free(x);
        return obj;
    }
    JS_SetOpaque(obj, x);
    return obj;
}

/* A number arrives through its own shortest round-trip text, so `new
   Decimal(0.1)` is the double 0.1 exactly as JS prints it -- not the binary
   value's full 55-digit expansion, and not a silent reinterpretation. */
static int dyn_dec_from(JSContext *ctx, JSValueConst v, dec_t *out)
{
    const char *s;
    size_t n;
    JSValue sv;
    int rc;

    if (JS_IsObject(v)) {
        dec_t *o = (dec_t *)JS_GetOpaque(v, dyn_dec_class_id);
        if (o) {
            dec_init(out);
            return dec_copy(out, o) < 0 ? -1 : 0;
        }
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (d != d || d == INFINITY || d == -INFINITY) {
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

static JSValue dyn_dec_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    dec_t *x;

    (void)new_target;
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
    return dyn_dec_wrap(ctx, x);
}

static dec_t *dyn_dec_this(JSContext *ctx, JSValueConst t)
{
    dec_t *x = (dec_t *)JS_GetOpaque2(ctx, t, dyn_dec_class_id);
    return x;
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD };

static JSValue dyn_dec_arith(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
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
    if (!r) { dec_free(&b); return JS_EXCEPTION; }
    switch (magic) {
    case OP_ADD: rc = dec_add(r, a, &b); break;
    case OP_SUB: {
        dec_t nb = b;
        nb.sign = (uint8_t)(b.nd ? !b.sign : 0);
        rc = dec_add(r, a, &nb);
        break;
    }
    case OP_MUL: rc = dec_mul(r, a, &b); break;
    case OP_DIV: rc = dec_div(r, a, &b, prec, mode); break;
    default:     rc = dec_mod(r, a, &b); break;
    }
    dec_free(&b);
    if (rc == DEC_E_TOOSLOW)
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
            "exact operation (multiply is limited to %u digit-pairs)",
            (unsigned)DEC_MAX_MUL_CELLS);
    if (rc == -2) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc < 0) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
                                  DEC_MAX_DIGITS);
    }
    /* Addition and multiplication are EXACT here: rounding a sum that fits is
       how a ledger loses a cent nobody can find. */
    return dyn_dec_wrap(ctx, r);
}

static JSValue dyn_dec_unary(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;

    (void)argc; (void)argv;
    if (!a)
        return JS_EXCEPTION;
    r = dyn_dec_alloc(ctx);
    if (!r)
        return JS_EXCEPTION;
    if (dec_copy(r, a) < 0) {
        dec_free(r); free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == 0)
        r->sign = 0;                    /* abs */
    else if (r->nd)
        r->sign = (uint8_t)!r->sign;    /* neg; -0 is 0 */
    return dyn_dec_wrap(ctx, r);
}

static JSValue dyn_dec_cmp(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
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

/* magic 0 = round(dp), 1 = toFixed(dp) */
static JSValue dyn_dec_round(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    int64_t dp = 0;
    int mode;

    if (!a)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt64(ctx, &dp, argv[0]) < 0)
        return JS_EXCEPTION;
    if (dp < -1000 || dp > 1000)
        return JS_ThrowRangeError(ctx, "Decimal: %s places must be -1000 to 1000",
                                  magic ? "toFixed" : "round");
    if (dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *s = JS_ToCString(ctx, argv[1]);
        size_t k;
        int found = 0;
        if (!s)
            return JS_EXCEPTION;
        for (k = 0; k < countof(RND_NAMES); k++)
            if (strcmp(s, RND_NAMES[k]) == 0) { mode = (int)k; found = 1; break; }
        JS_FreeCString(ctx, s);
        if (!found)
            return JS_ThrowRangeError(ctx, "Decimal: unknown rounding mode");
    }
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
        dec_t *r = dyn_dec_alloc(ctx);
        if (!r) { dec_free(&t); return JS_EXCEPTION; }
        if (dec_copy(r, &t) < 0) {
            dec_free(&t); dec_free(r); free(r);
            return JS_ThrowOutOfMemory(ctx);
        }
        dec_free(&t);
        return dyn_dec_wrap(ctx, r);
    }
    {
        db_t b;
        JSValue out;
        db_init(&b);
        /* the ORIGINAL sign: the rounded magnitude may be zero, and the value
           was still negative -- every standard implementation prints -0.00 */
        dec_write_fixed(&b, &t, (int32_t)dp, a->sign && (t.nd || a->nd));
        dec_free(&t);
        if (b.too_long) { db_free(&b); return dyn_dec_too_long(ctx); }
        if (b.oom) { db_free(&b); return JS_ThrowOutOfMemory(ctx); }
        out = JS_NewStringLen(ctx, (const char *)b.p, b.n);
        db_free(&b);
        return out;
    }
}

/* acc = acc * m, in place. */
static int dec_mul_into(dec_t *acc, const dec_t *m)
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

static int dec_pow_uint(dec_t *acc, dec_t *base, uint64_t e)
{
    int rc = 0;

    if (dec_grow(acc, 1) < 0)
        return -1;
    acc->d[0] = 1; acc->nd = 1; acc->exp = 0; acc->sign = 0;
    while (e > 0 && rc == 0) {
        if (e & 1)
            rc = dec_mul_into(acc, base);
        e >>= 1;
        if (e && rc == 0)
            rc = dec_mul_into(base, base);
    }
    return rc;
}

static JSValue dyn_dec_pow(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), acc, base, *r;
    int64_t e = 0;
    uint32_t prec;
    int mode, rc, neg;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &e, argv[0]) < 0)
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
            one.d[0] = 1; one.nd = 1;
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
    if (rc < 0) {
        dec_free(&acc);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
                                  DEC_MAX_DIGITS);
    }
    r = dyn_dec_alloc(ctx);
    if (!r) { dec_free(&acc); return JS_EXCEPTION; }
    if (dec_copy(r, &acc) < 0) {
        dec_free(&acc); dec_free(r); free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&acc);
    return dyn_dec_wrap(ctx, r);
}

/* magic 0 = toString, 1 = toNumber, 2 = isZero, 3 = sign, 4 = digits */
static JSValue dyn_dec_query(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val);
    db_t b;
    JSValue out;

    (void)argc; (void)argv;
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
    if (b.too_long) { db_free(&b); return dyn_dec_too_long(ctx); }
    if (b.oom) { db_free(&b); return JS_ThrowOutOfMemory(ctx); }
    if (magic == 1) {
        /* The engine's own ToNumber over the exact text: one correctly-rounded
           conversion, and the only place a Decimal may become approximate. */
        JSValue sv = JS_NewStringLen(ctx, (const char *)b.p, b.n);
        double d;
        db_free(&b);
        if (JS_IsException(sv))
            return sv;
        if (JS_ToFloat64(ctx, &d, sv) < 0) { JS_FreeValue(ctx, sv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, sv);
        return JS_NewFloat64(ctx, d);
    }
    out = JS_NewStringLen(ctx, (const char *)b.p, b.n);
    db_free(&b);
    return out;
}

static const JSCFunctionListEntry dyn_dec_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_dec_arith, OP_ADD),
    JS_CFUNC_MAGIC_DEF("sub", 1, dyn_dec_arith, OP_SUB),
    JS_CFUNC_MAGIC_DEF("mul", 1, dyn_dec_arith, OP_MUL),
    JS_CFUNC_MAGIC_DEF("div", 1, dyn_dec_arith, OP_DIV),
    JS_CFUNC_MAGIC_DEF("mod", 1, dyn_dec_arith, OP_MOD),
    JS_CFUNC_DEF("pow", 1, dyn_dec_pow),
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

/* ------------------------------------------------------------------ Money */

/* Minor units in an int64 and a currency tag: money is NOT decimal arithmetic,
   it is integer arithmetic with a unit, and most "money bugs" come from using
   a float-shaped type for it. */
typedef struct {
    int64_t amount;
    char    code[4];
    uint8_t minor;                      /* fraction digits for the currency */
} money_t;

static JSClassID dyn_money_class_id;

static void dyn_money_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_money_class_id));
}

static const JSClassDef dyn_money_class = {
    "Money", .finalizer = dyn_money_finalizer,
};

/* Currencies whose minor unit is not 2. The rest default to 2, which is right
   far more often than it is wrong, and a caller can say so explicitly. */
static const struct { const char *code; uint8_t minor; } MONEY_MINOR[] = {
    {"JPY",0},{"KRW",0},{"VND",0},{"CLP",0},{"ISK",0},{"PYG",0},{"RWF",0},
    {"UGX",0},{"VUV",0},{"XAF",0},{"XOF",0},{"XPF",0},{"DJF",0},{"GNF",0},
    {"KMF",0},{"MGA",0},{"BIF",0},
    {"BHD",3},{"IQD",3},{"JOD",3},{"KWD",3},{"LYD",3},{"OMR",3},{"TND",3},
};

static uint8_t money_minor(const char *code)
{
    size_t k;
    for (k = 0; k < countof(MONEY_MINOR); k++)
        if (strcmp(MONEY_MINOR[k].code, code) == 0)
            return MONEY_MINOR[k].minor;
    return 2;
}

static money_t *dyn_money_this(JSContext *ctx, JSValueConst t)
{
    return (money_t *)JS_GetOpaque2(ctx, t, dyn_money_class_id);
}

static JSValue dyn_money_new(JSContext *ctx, int64_t amount, const char *code,
                             uint8_t minor)
{
    money_t *m = (money_t *)malloc(sizeof *m);
    JSValue obj;

    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->amount = amount;
    m->minor = minor;
    memcpy(m->code, code, 3);
    m->code[3] = 0;
    obj = JS_NewObjectClass(ctx, (int)dyn_money_class_id);
    if (JS_IsException(obj)) {
        free(m);
        return obj;
    }
    JS_SetOpaque(obj, m);
    return obj;
}

/* A currency is three letters, upper-cased. Anything else is a typo that would
   otherwise become a silently distinct currency. */
static int money_code(JSContext *ctx, JSValueConst v, char out[4])
{
    const char *code = JS_ToCString(ctx, v);
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
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
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

/* The amount is an INTEGER count of the smallest unit: 1999 is $19.99. */
static int money_amount(JSContext *ctx, JSValueConst v, int64_t *out)
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

static JSValue dyn_money_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    int64_t amount;
    char up[4];
    uint8_t minor;

    (void)new_target;
    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "new Money(minorUnits, currency): a currency code is required");
    if (money_amount(ctx, argv[0], &amount) < 0 || money_code(ctx, argv[1], up) < 0)
        return JS_EXCEPTION;
    minor = money_minor(up);
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "minorDigits");
        int64_t md;
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &md, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (md < 0 || md > 6) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "new Money: minorDigits is 0 to 6");
            }
            minor = (uint8_t)md;
        }
        JS_FreeValue(ctx, v);
    }
    return dyn_money_new(ctx, amount, up, minor);
}

static int money_pair(JSContext *ctx, JSValueConst a_val, JSValueConst b_val,
                      money_t **a, money_t **b)
{
    *a = dyn_money_this(ctx, a_val);
    if (!*a)
        return -1;
    *b = JS_IsObject(b_val) ? (money_t *)JS_GetOpaque(b_val, dyn_money_class_id)
                            : NULL;
    if (!*b) {
        JS_ThrowTypeError(ctx, "Money: the operand must be a Money");
        return -1;
    }
    if (strcmp((*a)->code, (*b)->code) != 0) {
        /* Adding USD to EUR is not arithmetic, it is a missing exchange rate. */
        JS_ThrowTypeError(ctx, "Money: cannot combine %s and %s",
                          (*a)->code, (*b)->code);
        return -1;
    }
    return 0;
}

/* magic 0 = add, 1 = sub, 2 = cmp, 3 = equals */
static JSValue dyn_money_op(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    money_t *a, *b;
    int64_t r;

    /* JS_EXCEPTION means "an exception is PENDING" -- returning it without
       throwing propagates the uninitialized sentinel, which surfaces in the
       CALLER as "<some local> is not initialized". `money.add()` reported a
       ReferenceError naming an unrelated variable in the calling frame. */
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Money: this operation needs one argument");
    if (money_pair(ctx, this_val, argv[0], &a, &b) < 0)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewInt32(ctx, a->amount < b->amount ? -1 : a->amount > b->amount);
    if (magic == 3)
        return JS_NewBool(ctx, a->amount == b->amount);
    r = magic == 0 ? a->amount + b->amount : a->amount - b->amount;
    if ((magic == 0 && ((b->amount > 0 && r < a->amount)
                        || (b->amount < 0 && r > a->amount)))
        || (magic == 1 && ((b->amount < 0 && r < a->amount)
                           || (b->amount > 0 && r > a->amount))))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, r, a->code, a->minor);
}

static JSValue dyn_money_mul(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    money_t *a = dyn_money_this(ctx, this_val);
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
    if (k != 0 && (a->amount > INT64_MAX / (k < 0 ? -k : k)
                   || a->amount < INT64_MIN / (k < 0 ? -k : k)))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, a->amount * k, a->code, a->minor);
}

/* THE OPERATION THAT JUSTIFIES THE TYPE: split an amount into shares whose sum
   is exactly the original. The remainder goes one minor unit at a time to the
   earliest shares, so nothing is created and nothing is lost. */
/* The share weights: non-negative integers, and at least one nonzero. */
static int64_t *money_weights(JSContext *ctx, JSValueConst arr, int64_t n,
                              int64_t *total)
{
    int64_t *w = (int64_t *)calloc((size_t)n, sizeof *w), i;

    *total = 0;
    if (!w) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        double d;
        if (JS_IsException(e) || JS_ToFloat64(ctx, &d, e) < 0) {
            JS_FreeValue(ctx, e);
            free(w);
            return NULL;
        }
        JS_FreeValue(ctx, e);
        if (!(d >= 0) || d != (double)(int64_t)d) {
            free(w);
            JS_ThrowRangeError(ctx,
                "Money.allocate: every share is a non-negative integer weight");
            return NULL;
        }
        w[i] = (int64_t)d;
        *total += w[i];
    }
    if (*total == 0) {
        free(w);
        JS_ThrowRangeError(ctx, "Money.allocate: the weights sum to zero");
        return NULL;
    }
    return w;
}

/* THE OPERATION THAT JUSTIFIES THE TYPE: split an amount into shares whose sum
   is exactly the original. The remainder goes one minor unit at a time to the
   earliest shares, so nothing is created and nothing is lost. */
static JSValue dyn_money_allocate(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    money_t *a = dyn_money_this(ctx, this_val);
    JSValue lv, out;
    int64_t n = 0, total = 0, given = 0, i, abs_amount, rem;
    int64_t *w;
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
    neg = a->amount < 0;
    abs_amount = neg ? -a->amount : a->amount;
    for (i = 0; i < n; i++) {
        /* floor(amount * w / total), in two parts so the product of the amount
           and a weight cannot overflow on the way. */
        int64_t part = abs_amount % total, share;
        if (w[i] && (part > INT64_MAX / w[i]
                     || abs_amount / total > INT64_MAX / w[i])) {
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
    if (JS_IsException(out)) { free(w); return out; }
    for (i = 0; i < n; i++) {
        JSValue m = dyn_money_new(ctx, neg ? -w[i] : w[i], a->code, a->minor);
        if (JS_IsException(m)
            || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)i, m,
                                            JS_PROP_C_W_E) < 0) {
            free(w);
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    free(w);
    return out;
}

/* magic 0 = toString, 1 = amount, 2 = currency, 3 = format, 4 = toDecimal */
static JSValue dyn_money_query(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    money_t *a = dyn_money_this(ctx, this_val);
    char buf[64];
    int64_t whole, frac, div = 1;
    int k;

    (void)argc; (void)argv;
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
    if (frac < 0) frac = -frac;
    if (a->minor == 0)
        snprintf(buf, sizeof buf, "%lld", (long long)whole);
    else
        snprintf(buf, sizeof buf, "%s%lld.%0*lld",
                 (a->amount < 0 && whole == 0) ? "-" : "", (long long)whole,
                 (int)a->minor, (long long)frac);
    if (magic == 3) {
        static const struct { const char *code, *sym; } SYM[] = {
            {"USD","$"},{"EUR","\xE2\x82\xAC"},{"GBP","\xC2\xA3"},
            {"JPY","\xC2\xA5"},{"CNY","\xC2\xA5"},{"INR","\xE2\x82\xB9"},
            {"KRW","\xE2\x82\xA9"},{"CAD","CA$"},{"AUD","A$"},
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
        dec_t *r = dyn_dec_alloc(ctx);
        if (!r)
            return JS_EXCEPTION;
        if (dec_parse(r, buf, strlen(buf)) < 0) {
            dec_free(r); free(r);
            return JS_ThrowInternalError(ctx, "Money.toDecimal failed");
        }
        return dyn_dec_wrap(ctx, r);
    }
    return JS_NewString(ctx, buf);
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

/* ------------------------------------------------------------ registration */

static int dyn_decimal_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_dec_class_id, &dyn_dec_class,
                                 dyn_dec_proto, countof(dyn_dec_proto),
                                 dyn_dec_ctor, "Decimal") < 0)
        return -1;
    return dyn_register_plain_class(ctx, m, &dyn_money_class_id,
                                    &dyn_money_class, dyn_money_proto,
                                    countof(dyn_money_proto), dyn_money_ctor,
                                    "Money");
}

int js_nat_init_decimal(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:decimal", dyn_decimal_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Decimal");
    return JS_AddModuleExport(ctx, m, "Money");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_DECIMAL */
