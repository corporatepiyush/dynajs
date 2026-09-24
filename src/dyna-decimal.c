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

/* a - b, the signed form (OP_SUB negates then adds; the ln reduction needs
   the direct form). */
static int dec_sub(dec_t *r, const dec_t *a, const dec_t *b)
{
    dec_t nb;
    int rc;

    dec_init(&nb);
    if (dec_copy(&nb, b) < 0) { dec_free(&nb); return -1; }
    if (nb.nd)
        nb.sign = (uint8_t)!nb.sign;
    rc = dec_add(r, a, &nb);
    dec_free(&nb);
    return rc;
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
    /* Enforce the divide cell cap BEFORE any allocation: the schoolbook core is
       O(nn * bn) with up to nine subtractions per output digit, so a hostile
       big/big pair (e.g. two ~50k-digit operands) would run ~10^10 byte-ops
       uninterruptibly on the JS thread. The cap is checked as a 64-bit product
       so it is never exceeded by a wrapped multiply. */
    if ((uint64_t)nn * (uint64_t)b->nd > DEC_MAX_DIV_CELLS)
        return DEC_E_TOOSLOW;
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
    /* The exponent is computed in INT64: a->exp - b->exp spans ±4e9 (both are
       parse-capped at ±2e9), which silently WRAPPED in int32 and returned a
       wrong answer -- 1e2000000000 / 1e-2000000000 came out 1e-294967333
       instead of an error. Same ±2e9 house bound, same -1 error, as dec_mul
       (the C-16 fix this one completes). */
    {
        int64_t e = (int64_t)a->exp - (int64_t)b->exp - (int64_t)shift;
        if (e > 2000000000LL || e < -2000000000LL)
            goto done;                    /* rc is still -1: the caller throws */
        r->exp = (int32_t)e;
    }
    r->sign = (uint8_t)(a->sign ^ b->sign);
    dec_trim(r);
    if (dec_round_sig(r, prec, mode, rn != 0) < 0)
        goto done;
    rc = 0;
done:
    free(num); free(q); free(rem);
    return rc;
}

/* Exact TRUNCATED integer division of a by b: the quotient is toward zero
   and the remainder takes the dividend's sign, so a == q*b + r holds on the
   nose -- the pair `%` and divmod both promise. NULL qout means remainder
   only. The error codes are dec_mod's (-2 = /0, DEC_E_TOOSLOW, -1). */
static int dec_int_divmod(dec_t *qout, dec_t *r, const dec_t *a, const dec_t *b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    uint32_t an, bn, qn, rn;
    uint8_t *ad = NULL, *bd = NULL, *q = NULL, *rem = NULL;
    int rc = -1;

    if (b->nd == 0)
        return -2;
    if (a->nd == 0) {
        if (qout) { qout->nd = 0; qout->exp = 0; qout->sign = 0; }
        r->nd = 0; r->exp = 0; r->sign = 0;
        return 0;
    }
    /* Both operands scaled to integers at the same position lo: a = A*10^lo
       and b = B*10^lo, so a/b == A/B exactly and the remainder is
       (A mod B)*10^lo. */
    an = (uint32_t)(a->exp + (int32_t)a->nd - lo);
    bn = (uint32_t)(b->exp + (int32_t)b->nd - lo);
    if (an > DEC_MAX_DIGITS || bn > DEC_MAX_DIGITS)
        return -1;
    /* Same cell cap as dec_div: the schoolbook core is O(an*bn), so a hostile
       big/big pair would otherwise run uninterruptibly here too. */
    if ((uint64_t)an * (uint64_t)bn > DEC_MAX_DIV_CELLS)
        return DEC_E_TOOSLOW;
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
    if (qout) {
        if (dec_grow(qout, qn ? qn : 1) < 0) { rc = -1; goto done; }
        memcpy(qout->d, q, qn);
        qout->nd = qn;
        qout->exp = 0;                  /* A/B is a pure integer */
        qout->sign = (uint8_t)((a->sign ^ b->sign) & (qn ? 1u : 0u));
        dec_trim(qout);
    }
    rc = 0;
done:
    free(ad); free(bd); free(q); free(rem);
    return rc;
}

/* The remainder of TRUNCATED division, which is what `%` means everywhere. */
static int dec_mod(dec_t *r, const dec_t *a, const dec_t *b)
{
    return dec_int_divmod(NULL, r, a, b);
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

static void dyn_dec_dispose(void *p)
{
    dec_free(p);
    free(p);
}

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

/* straggler: the Decimal options bag is {precision, rounding} --
   BOTH keys are accepted on every op that takes a bag, including the ops
   that ignore one of them (add/sub/mul are exact: a `precision` there is
   the documented accepted-and-ignored form; sqrt/exp/ln/log10 ignore
   `rounding`). Unknown KEYS are the misspelled intent and refuse, naming
   the key and the valid set -- never silently ignored. A BARE rounding
   string ("down") is not a bag and skips this check. */
static int dyn_dec_opts_strict(JSContext *ctx, JSValueConst o)
{
    if (!JS_IsObject(o))
        return 0;
    {
        JSPropertyEnum *props = NULL;
        uint32_t nprops = 0, i;
        int bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) { bad = 1; break; }
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

/* THE one rounding-mode lookup. Accepts either spelling the module allows:
 * {rounding: "halfEven"} in an options object, or the BARE string
 * round(v, dp, "down"). Both spellings used to be looked up by two separate
 * loops that disagreed on the accept-set (the bare string was refused here
 * but accepted by dyn_dec_round's inline copy, with a different error
 * message); one helper now serves both, so both accept exactly RND_NAMES and
 * both reject with the same listing. A non-object, non-string (or a missing
 * `rounding`) leaves the halfEven default. */
static int dyn_rnd_mode(JSContext *ctx, JSValueConst o, int *mode)
{
    JSValue v;
    const char *s;
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
        if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return 0; }
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

static int dyn_prec(JSContext *ctx, JSValueConst o, uint32_t *prec)
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
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return 0; }
    if (JS_ToInt64(ctx, &p, v) < 0) { JS_FreeValue(ctx, v); return -1; }
    {   /* an INTEGER, like Money's amount option: 34.9 silently truncating
           to 34 looks like a working option and quietly changes the answer */
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0) { JS_FreeValue(ctx, v); return -1; }
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

static dec_t *dyn_dec_alloc(JSContext *ctx)
{
    dec_t *x = (dec_t *)malloc(sizeof *x);
    if (!x) { JS_ThrowOutOfMemory(ctx); return NULL; }
    dec_init(x);
    return x;
}

static JSValue dyn_dec_wrap(JSContext *ctx, JSValueConst new_target,
                            dec_t *x)
{
    return dyn_plain_wrap(ctx, new_target, dyn_dec_class_id, x, dyn_dec_dispose);
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
    if (rc == DEC_E_TOOSLOW) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
            "exact operation (multiply is limited to %u digit-pairs, divide to "
            "%u)", (unsigned)DEC_MAX_MUL_CELLS, (unsigned)DEC_MAX_DIV_CELLS);
    }
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
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
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
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
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
    double dpd;

    if (!a)
        return JS_EXCEPTION;
    /* dp is an INTEGER in -1000..1000: a fractional dp (1.9 -> 1) silently
       rounding at a different place than the caller wrote is the same
       coercion trap pow(n) had; 1.999.round(1.9) must not look like it
       worked. JS_ToInt64 alone maps NaN to 0. */
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &dpd, argv[0]) < 0)
            return JS_EXCEPTION;
        if (dpd != trunc(dpd))
            return JS_ThrowRangeError(ctx, "Decimal: %s places must be an "
                                      "integer", magic ? "toFixed" : "round");
        if (JS_ToInt64(ctx, &dp, argv[0]) < 0)
            return JS_EXCEPTION;
    }
    if (dp < -1000 || dp > 1000)
        return JS_ThrowRangeError(ctx, "Decimal: %s places must be -1000 to 1000",
                                  magic ? "toFixed" : "round");
    /* the ONE mode lookup: an options object or a bare string, same
       accept-set, same error (the inline RND_NAMES loop that used to handle
       the bare string here disagreed with dyn_rnd_mode on both) */
    if (dyn_rnd_mode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &mode) < 0)
        return JS_EXCEPTION;
    if (magic == 1 && (argc <= 1 || (!JS_IsString(argv[1]) && !JS_IsObject(argv[1]))))
        mode = RND_HALF_UP;             /* toFixed's documented default is
                                           halfUp, the financial convention --
                                           round() keeps the context default
                                           halfEven. The shared lookup only
                                           defaults when no mode was given, so
                                           an explicit {} keeps halfEven. */
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
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
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
    double ed;

    if (!a)
        return JS_EXCEPTION;
    /* The exponent is an INTEGER in -10000..10000. JS_ToInt64 alone would
       silently truncate 2.9 to 2 and map NaN to 0 (2 ** NaN == 1!), the same
       wrong-answer-by-coercion trap dyn_prec rejects for `precision` and
       money_amount rejects for minorUnits. */
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
    if (rc == DEC_E_TOOSLOW) {
        /* the WORK budget, not the digit bound: pow reaches the multiply/divide
           cell caps (squaring a wide base, or 1/acc for a negative exponent)
           long before any single result exceeds DEC_MAX_DIGITS */
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
    if (!r) { dec_free(&acc); return JS_EXCEPTION; }
    if (dec_copy(r, &acc) < 0) {
        dec_free(&acc); dec_free(r); free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&acc);
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
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

/* ------------------------------------------------------- sqrt, exp, ln */

/* Error codes beyond the arithmetic ones (-1 generic, -2 /0, -3 too slow). */
#define DEC_E_INVALID  (-4)             /* sqrt(neg), ln(nonpositive) */
#define DEC_E_OVERFLOW (-5)             /* exp past the context's Emax */
#define DEC_E_BUDGET   (-6)             /* the op refuses this precision */

/* The ln/exp/log10 working budget. These ops are series of schoolbook
   multiplies (w^2 cells each, ~w terms at working precision w), so the cost
   is ~w^3 cells: capped at 2^28, the same order as ONE hostile mul/div at
   the house caps. precision is capped near 350 by this, not by an algorithm. */
#define DEC_SERIES_CELLS 268435456.0    /* 2^28 */

/* t /= 2, exactly: *5 and one place down. */
static int dec_halve(dec_t *t)
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

/* The adjusted exponent: value = d.ddd * 10^adj with d in 1..9. */
static int32_t dec_adjusted(const dec_t *x)
{
    return x->nd ? x->exp + (int32_t)x->nd - 1 : 0;
}

/* Integer square root of the most-significant-first digit string msd[0..nd):
   root = floor(sqrt(N)) little-endian (exactly ceil(nd/2) digits), and the
   final remainder N - root^2 little-endian. Long division's mirror: two
   digits come down per step, and the trial digit d is the largest with
   (20*root + d)*d <= rem -- at most nine subtractions, so it stays in the
   "simple enough to be obviously right" school of the division core. */
static int dec_isqrt(const uint8_t *msd, uint32_t nd,
                     uint8_t *root, uint32_t *rln,
                     uint8_t *rem, uint32_t *rmn)
{
    uint32_t S = (nd + 1) / 2, rl = 0, rn = 0, i;
    uint8_t *b20;

    *rln = 0;
    *rmn = 0;
    /* 20*root: root never exceeds S+1 digits, so b20 never exceeds S+3. */
    b20 = (uint8_t *)calloc(S + 4, 1);
    if (!b20)
        return -1;
    for (i = 0; i < S; i++) {
        int hd, ld, d, carry;
        uint32_t k, bl;
        if ((nd & 1) && i == 0) {
            hd = 0;                     /* the pad digit above an odd length */
            ld = msd[0];                /* msd[0] is the MOST significant */
        } else if (nd & 1) {
            hd = msd[2 * i - 1];        /* the stream reads two places down */
            ld = msd[2 * i];
        } else {
            hd = msd[2 * i];
            ld = msd[2 * i + 1];
        }
        /* rem = rem*100 + the pair */
        if (rn) memmove(rem + 2, rem, rn);
        rem[0] = (uint8_t)ld;
        rem[1] = (uint8_t)hd;
        rn += 2;
        while (rn && rem[rn - 1] == 0) rn--;
        /* b20 = 20*root: double, then one place up */
        carry = 0;
        for (k = 0; k < rl; k++) {
            int s2 = root[k] * 2 + carry;
            b20[k] = (uint8_t)(s2 % 10);
            carry = s2 / 10;
        }
        bl = rl;
        if (carry) b20[bl++] = (uint8_t)carry;
        if (bl) { memmove(b20 + 1, b20, bl); bl++; }
        b20[0] = 0;
        while (bl && b20[bl - 1] == 0) bl--;
        for (d = 9; d >= 0; d--) {
            /* prod = b20*d + d*d, one digit-wise pass */
            uint8_t *prod = (uint8_t *)calloc(bl + 2, 1);
            uint32_t pl;
            if (!prod) { free(b20); return -1; }
            carry = d * d;
            for (k = 0; k < bl; k++) {
                int s2 = b20[k] * d + carry;
                prod[k] = (uint8_t)(s2 % 10);
                carry = s2 / 10;
            }
            pl = bl;
            while (carry) { prod[pl++] = (uint8_t)(carry % 10); carry /= 10; }
            while (pl && prod[pl - 1] == 0) pl--;
            if (dec_ge(rem, rn, prod, pl)) {
                rn = dec_sub_into(rem, rn, prod, pl);
                free(prod);
                break;
            }
            free(prod);
        }
        /* root = root*10 + d (the found d; d == 0 always "fits") */
        memmove(root + 1, root, rl);
        root[0] = (uint8_t)(d < 0 ? 0 : d);
        rl++;
    }
    free(b20);
    /* root has EXACTLY S digits: padded N lies in [10^(2S-2), 10^(2S)) */
    *rln = rl;
    *rmn = rn;
    return 0;
}

/* Does x sit exactly on a prec-digit half-ulp tie as far as its carried
   digits show (guard digit 5, every digit below zero)? Such a result may be
   a hair above the tie in truth -- only deeper computation can decide, so
   the series ops retry at a higher working precision when this fires. */
static int dec_on_tie(const dec_t *x, uint32_t prec)
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

/* The square root, correctly rounded to `prec` digits in HALF_EVEN (the
   rounding python's decimal also always uses for sqrt -- its context mode is
   ignored, and so is ours: see dyn_dec_math). The identity that makes exact
   rounding cheap: with R = floor(sqrt(M)) and q = M - R*R, the true tail
   below R is q/(sqrt(M)+R), which is < 1/2 when q <= R, > 1/2 when q > R,
   and exactly 0 when q == 0 -- an irrational sqrt can never land ON the
   half, so one guard digit plus `q != 0` decides every mode, with no
   double-rounding hole. */
static int dec_sqrt(dec_t *r, const dec_t *a, uint32_t prec)
{
    uint32_t D, S, s = 0, rl, rn, ntot, k;
    int32_t e = a->exp;
    int t = (int)(e & 1);               /* an odd exponent folds in: m*10^(2k+1)
                                           = (m*10) * 10^(2k) */
    uint8_t *md, *root, *rem;
    int rc = -1;

    if (a->nd == 0) {                   /* sqrt(0) = 0 */
        r->nd = 0; r->exp = 0; r->sign = 0;
        return 0;
    }
    if (a->sign)
        return DEC_E_INVALID;
    ntot = a->nd + (uint32_t)t;
    /* Scale M = |a| * 10^(t + 2s) so floor(sqrt(M)) carries prec+1 digits
       (one to round on). Digits are never LOST for huge inputs: there the
       root simply has more than prec+1 digits and rounds down directly. */
    if (ntot < 2u * prec + 2u)
        s = (2u * prec + 2u - ntot + 1) / 2;
    D = ntot + 2u * s;
    S = (D + 1) / 2;
    if (D > DEC_MAX_DIGITS)
        return -1;
    md = (uint8_t *)calloc(D + 2, 1);
    root = (uint8_t *)calloc(S + 2, 1);
    rem = (uint8_t *)calloc(D + 4, 1);
    if (!md || !root || !rem)
        goto done;
    for (k = 0; k < a->nd; k++)         /* most-significant first */
        md[k] = a->d[a->nd - 1 - k];
    if (dec_isqrt(md, D, root, &rl, rem, &rn) < 0)
        goto done;
    if (dec_grow(r, rl ? rl : 1) < 0)
        goto done;
    memcpy(r->d, root, rl);
    r->nd = rl;
    r->exp = (e - t) / 2 - (int32_t)s;  /* sqrt(|a|) = R * 10^(e'/2 - s) + tail */
    r->sign = 0;
    dec_trim(r);
    if (r->nd > prec + 1) {
        /* The remainder digit sits BELOW the guard place (nd >= prec+2), so
           it can safely carry the sticky bit. */
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
    free(md); free(root); free(rem);
    return rc;
}

/* 2*atanh(u) at `w` significant digits, |u| <= 1/3: the workhorse series
   behind ln (ln(y) = 2*atanh((y-1)/(y+1))). All terms positive, so there is
   no cancellation; terms shrink by |u|^2 >= 9x per step. */
static int dec_atanh2(dec_t *r, const dec_t *u, uint32_t w)
{
    dec_t uu, term, sum, t, acc;
    uint32_t k;
    int rc = -1;

    dec_init(&uu); dec_init(&term); dec_init(&sum); dec_init(&t);
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
        /* a zero term (u == 0, i.e. ln(1)) can never shrink: stop now */
        if (dec_is_zero(&term))
            break;
        /* stop once the remaining tail is under the w+3 noise floor */
        if (dec_adjusted(&term) + (int32_t)w + 3 < dec_adjusted(&sum))
            break;
        {
            char buf[16];
            dec_t kd;
            snprintf(buf, sizeof buf, "%u", 2 * k + 1);
            dec_init(&kd);
            if (dec_parse(&kd, buf, strlen(buf)) < 0) { dec_free(&kd); goto done; }
            if (dec_div(&t, &term, &kd, w, RND_HALF_EVEN) < 0) {
                dec_free(&kd);
                goto done;
            }
            dec_free(&kd);
        }
        /* dec_add REALLOCS r when the sum outgrows it, so r must alias
           NEITHER operand -- accumulate through acc, never in place */
        if (dec_add(&acc, &sum, &t) < 0 || dec_copy(&sum, &acc) < 0
            || dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
            goto done;
    }
    if (dec_add(&acc, &sum, &sum) < 0 || dec_copy(r, &acc) < 0)
        goto done;                      /* the 2 of 2*atanh */
    rc = 0;
done:
    dec_free(&uu); dec_free(&term); dec_free(&sum); dec_free(&t);
    dec_free(&acc);
    return rc;
}

/* An int64 as an exact dec_t, for the small integer operands the math
   kernels need: 2, the series divisors, and the exponent E (which is often
   negative -- ln(1e-100) reduces with E = -100). */
static int dec_small(dec_t *x, int64_t v)
{
    char buf[24];
    int n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

    dec_init(x);
    if (u == 0) return 0;
    while (u > 0 && n < 24) { buf[n++] = (char)(u % 10u); u /= 10u; }
    if (u != 0 || dec_grow(x, (uint32_t)n) < 0)
        return -1;
    memcpy(x->d, buf, (size_t)n);       /* numeric digits, little-endian */
    x->nd = (uint32_t)n;
    x->exp = 0;
    x->sign = (uint8_t)neg;
    return 0;
}

/* ln(2) at w digits: 2*atanh(1/3). */
static int dec_const_ln2(dec_t *r, uint32_t w)
{
    dec_t u, one, three;
    int rc;

    dec_init(&u); dec_init(&one); dec_init(&three);
    if (dec_grow(&one, 1) < 0 || dec_grow(&three, 1) < 0)
        return -1;
    one.d[0] = 1; one.nd = 1;
    three.d[0] = 3; three.nd = 1;
    rc = dec_div(&u, &one, &three, w, RND_HALF_EVEN);
    if (rc == 0)
        rc = dec_atanh2(r, &u, w);
    dec_free(&u); dec_free(&one); dec_free(&three);
    return rc;
}

/* ln(10) at w digits: ln(10) = ln(8) + ln(1.25) = 3*ln2 + 2*atanh(1/9).
   The 1/9 argument is what keeps this cheap: its series gains ~1.9 digits
   per term (atanh(2/3) for ln5 would gain only 0.35). */
static int dec_const_ln10(dec_t *r, uint32_t w)
{
    dec_t u, l2, l9;
    int rc = -1;

    dec_init(&u); dec_init(&l2); dec_init(&l9);
    if (dec_const_ln2(&l2, w) < 0)
        goto done;
    {
        dec_t one, nine;
        dec_init(&one); dec_init(&nine);
        if (dec_small(&one, 1) < 0 || dec_small(&nine, 9) < 0) {
            dec_free(&one); dec_free(&nine);
            goto done;
        }
        if (dec_div(&u, &one, &nine, w + 2, RND_HALF_EVEN) < 0) {
            dec_free(&one); dec_free(&nine);
            goto done;
        }
        dec_free(&one); dec_free(&nine);
    }
    if (dec_atanh2(&l9, &u, w + 2) < 0)
        goto done;
    /* r = 3*ln2 + 2*atanh(1/9); atanh2 returns the DOUBLED series. Each add
       runs through u (local scratch): dec_add reallocs r, so r never
       aliases one of its own operands. */
    if (dec_add(&u, &l9, &l2) < 0
        || dec_add(&l9, &u, &l2) < 0
        || dec_add(&u, &l9, &l2) < 0)
        goto done;
    if (dec_copy(r, &u) < 0 || dec_round_sig(r, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&u); dec_free(&l2); dec_free(&l9);
    return rc;
}

/* ln(x) for x > 0, correctly rounded to `prec` digits (HALF_EVEN, matching
   python's decimal, which ignores its context mode here exactly as we do).
   Reduction: x = t * 10^E with t in [1,2), ln(x) = ln(t) + k*ln2 + E*ln10.
   The working precision w = prec + ~24 carries the integer part of E*ln10
   (10 digits at the +-2e9 exponent bound) plus the guard. */
static int dec_ln_at(dec_t *r, const dec_t *a, uint32_t prec, uint32_t w,
                     int *tie)
{
    dec_t t, num, den, u, acc, c, small, sum;
    int32_t E;
    uint32_t k = 0;
    int rc = -1, direct = 0;

    dec_init(&t); dec_init(&num); dec_init(&den); dec_init(&u);
    dec_init(&acc); dec_init(&c); dec_init(&small); dec_init(&sum);
    if (dec_copy(&t, a) < 0)
        goto done;
    /* x in [0.5, 2): use u = (x-1)/(x+1) on the RAW value -- no E*ln10 or
       k*ln2 term, so ln of a near-1 value cannot cancel (ln(0.999...9) with
       a hundred 9s would otherwise eat a hundred guard digits in the
       E = -1 constant subtraction). |u| <= 1/3, same convergence. The raw
       digits also stay UNROUNDED here: ln(1 - 1e-70) needs the digit at
       1e-70, which a w-digit pre-rounding would silently delete. */
    {
        dec_t lo, hi;
        dec_init(&lo); dec_init(&hi);
        if (dec_parse(&lo, "0.5", 3) < 0 || dec_small(&hi, 2) < 0) {
            dec_free(&lo); dec_free(&hi);
            goto done;
        }
        if (dec_cmp(&t, &lo) >= 0 && dec_cmp(&t, &hi) < 0)
            direct = 1;
        dec_free(&lo); dec_free(&hi);
    }
    if (!direct && dec_round_sig(&t, w, RND_HALF_EVEN, 0) < 0)
        goto done;
    if (direct) {
        E = 0;
    } else {
        E = dec_adjusted(&t);
        t.exp -= E;                     /* t in [1, 10), same digits */
        {
            dec_t two;
            if (dec_small(&two, 2) < 0) { dec_free(&two); goto done; }
            while (dec_cmp(&t, &two) >= 0) {   /* t in [1, 2) after at most 3 */
                dec_free(&two);
                if (dec_halve(&t) < 0)
                    goto done;
                k++;
                if (dec_small(&two, 2) < 0) { dec_free(&two); goto done; }
            }
            dec_free(&two);
        }
    }
    /* u = (t-1)/(t+1), |u| <= 1/3 */
    if (dec_grow(&num, w + 2) < 0 || dec_grow(&den, w + 2) < 0)
        goto done;
    {
        dec_t one;
        if (dec_small(&one, 1) < 0) { dec_free(&one); goto done; }
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
    /* small is ONLY ever a dec_small target (dec_init inside resets d to the
       inline buffer WITHOUT freeing: give it a heap buffer via anything else
       and the next dec_small leaks it -- the exact leak the ln(25)-shaped
       ASan probe caught). Sums accumulate through `sum` instead. */
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
        /* E * ln10: E is an exact integer within +-2e9 */
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
    dec_free(&t); dec_free(&num); dec_free(&den); dec_free(&u);
    dec_free(&acc); dec_free(&c); dec_free(&small); dec_free(&sum);
    return rc;
}

static int dec_ln(dec_t *r, const dec_t *a, uint32_t prec)
{
    uint32_t w = prec + 24;
    int tie = 0, attempt, rc;

    if (a->nd == 0)
        return DEC_E_INVALID;           /* ln(0): no -Infinity in this module */
    if (a->sign)
        return DEC_E_INVALID;
    /* the series work is ~3.7*w^3 cells: ln(t) ~1.05, ln2 ~1.05, ln10 ~1.57
       (all three are atanh series of w-digit multiplies) */
    if ((double)4.2 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    for (attempt = 0; attempt < 3; attempt++) {
        rc = dec_ln_at(r, a, prec, w, &tie);
        if (rc != 0 || !tie || attempt == 2)
            break;
        /* a retry over budget leaves the tie-rounded answer in place */
        if ((double)4.2 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
    return rc;
}

/* log10(x) = ln(x) / ln(10), both sides at the same working precision, so
   exact powers of ten come out exact (c/c cancels). */
static int dec_log10(dec_t *r, const dec_t *a, uint32_t prec)
{
    dec_t L, c, acc;
    uint32_t w = prec + 24;
    int tie, attempt, rc = -1;

    if (a->nd == 0 || a->sign)
        return DEC_E_INVALID;
    /* ln's ~3.7*w^3 plus one final w-digit divide */
    if ((double)4.6 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    dec_init(&L); dec_init(&c); dec_init(&acc);
    for (attempt = 0; attempt < 3; attempt++) {
        tie = 0;
        if (dec_ln_at(&L, a, w, w, &tie) < 0)
            goto done;
        if (dec_const_ln10(&c, w) < 0)
            goto done;
        if (dec_div(&acc, &L, &c, prec, RND_HALF_EVEN) < 0)
            goto done;
        /* a half-ulp tie in the quotient is decided deeper: retry with a
         * wider ln so the quotient's guard digits are real */
        if (!dec_on_tie(&acc, prec) || attempt == 2) {
            if (dec_copy(r, &acc) < 0)
                goto done;
            rc = 0;
            goto done;
        }
        /* a retry over budget leaves the tie-rounded answer in place */
        if ((double)4.6 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
done:
    dec_free(&L); dec_free(&c); dec_free(&acc);
    return rc;
}

/* e^x, correctly rounded to `prec` digits (HALF_EVEN; python's decimal again
   ignores its context mode here, and so do we). Reduction by repeated
   halving: with t = x/2^n <= 1/8, e^t converges in ~1.1*w series terms and
   the n squarings amplify the working error by at most 2^n, so the guard
   w - prec covers the adjusted exponent of x. */
static int dec_exp(dec_t *r, const dec_t *a, uint32_t prec)
{
    dec_t t, sum, term, kk, tmp;
    uint32_t w, n = 0, k;
    int neg, rc = -1, attempt;
    double est, amp;                    /* log10|e^x| and ~log10|x| */

    if (a->nd == 0) {                   /* e^0 = 1 */
        if (dec_grow(r, 1) < 0)
            return -1;
        r->d[0] = 1; r->nd = 1; r->exp = 0; r->sign = 0;
        return 0;
    }
    /* Two different magnitudes: log10|x| sets the squaring-chain guard, and
       x*log10(e) = log10|e^x| sets the overflow/underflow bounds.
       FORMING x*log10(e) as d*pow(10,adj) is a NaN trap: past adj 308 the
       pow is inf and any zero digit contributes 0*inf = NaN, whose
       comparisons are all false -- BOTH gates open and the op sinks into
       the unbounded halving/squaring path (a 16.7 s exp). The estimate is
       therefore SPLIT on adj: beyond adj 7 the gates decide from the log
       alone (|x| >= 1e7 makes est >= 4.3e6, far past either bound); below
       adj 7 every pow term is <= 1e6 and the 17-digit xd is finite, so the
       Emax hair zone (x ~ 2302585.09) still lands inside the double. */
    neg = a->sign;                      /* BEFORE any gate: the fast path
                                           below reads it, and an uninitialized
                                           read is UB the optimizer may mangle */
    {
        double lead = (double)a->d[a->nd - 1];
        double adj = (double)(a->exp + (int32_t)a->nd - 1);
        if (a->nd > 1)
            lead += (double)a->d[a->nd - 2] / 10.0;
        amp = adj + log10(lead);        /* ~log10|x|: finite for any adj */
        if (adj >= 7.0) {
            if (!neg)
                return DEC_E_OVERFLOW;
            r->nd = 0; r->exp = 0; r->sign = 0;
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
    /* The context magnitude limits, mirroring python decimal's default
       context: the ideal result past Emax = 999999 overflows -- exactly
       e^x >= 10^1000000, i.e. x >= 10^6 * ln10 -- and a result the context
       cannot represent at all (adjusted exponent below
       Etiny = -(999999 + prec - 1) minus the round-to-zero half step)
       underflows to 0 (this module has one zero; python prints a subnormal
       zero). Between those bounds even a 1e-999998-scale result is computed
       in full; results whose adjusted exponent lands in the subnormal band
       [Etiny, -999999] are QUANTIZED to the Etiny exponent below, matching
       python's subnormal rounding. The overflow gate is DECIDED EXACTLY in
       the hair zone (x within ~1e-6 of the threshold, beyond what a double
       resolves) by comparing against 10^6*ln10. */
    if (!neg && est > 999998.0) {
        dec_t ln10c, lim;
        dec_init(&ln10c); dec_init(&lim);
        /* the threshold is irrational, so no finite x ever EQUALS it; the
         * constant is carried at prec+40 digits so an input must carry more
         * than prec+40 significant digits to slip past the approximation */
        if (dec_const_ln10(&ln10c, prec + 40) < 0) {
            dec_free(&ln10c); dec_free(&lim);
            return -1;
        }
        if (dec_small(&lim, 1000000) < 0) {
            dec_free(&ln10c); dec_free(&lim);
            return -1;
        }
        if (dec_mul_into(&lim, &ln10c) < 0) {
            dec_free(&ln10c); dec_free(&lim);
            return -1;
        }
        dec_free(&ln10c);
        {                               /* overflow iff x > 10^6 * ln10 */
            int c2 = dec_cmp(a, &lim);
            dec_free(&lim);
            if (c2 > 0)
                return DEC_E_OVERFLOW;
        }
    }
    /* python's exact zero boundary: value < 0.5*10^Etiny, i.e. one half
       step BELOW Etiny -- the gate sits at Etiny - 0.301 (log10 2), and the
       [0.5, 1)*10^Etiny remainder still computes and quantizes to
       1*10^Etiny like python's smallest subnormal. */
    if (neg && est > 999999.0 + (double)prec - 1.0 + 0.30103) {
        r->nd = 0; r->exp = 0; r->sign = 0;
        return 0;
    }
    /* the squaring chain amplifies by 2^n, n = log2(|x|/0.125) halvings: for
       |x| <= 0.125 there are none, so the guard must not grow for SMALL
       magnitudes (it never needs |log10|x|| -- that direction made the
       budget refuse every |x| < 1e-115 at high precision) */
    w = prec + 12u + (uint32_t)(amp > 0.0 ? amp : 0.0) + 2u;
    if (w > 5100u)
        w = 5100u;
    if ((double)2.6 * (double)w * (double)w * (double)w > DEC_SERIES_CELLS)
        return DEC_E_BUDGET;
    dec_init(&t); dec_init(&sum); dec_init(&term); dec_init(&kk); dec_init(&tmp);
    for (attempt = 0; attempt < 3; attempt++) {
        dec_free(&t); dec_free(&sum); dec_free(&term); dec_free(&tmp);
        dec_init(&t); dec_init(&sum); dec_init(&term); dec_init(&tmp);
        n = 0;
        if (dec_copy(&t, a) < 0)
            goto done;
        t.sign = 0;                     /* magnitude */
        if (dec_round_sig(&t, w, RND_HALF_EVEN, 0) < 0)
            goto done;
        {
            dec_t one8th;
            if (dec_parse(&one8th, "0.125", 5) < 0) { dec_free(&one8th); goto done; }
            while (dec_cmp(&t, &one8th) > 0) {
                dec_free(&one8th);
                if (dec_halve(&t) < 0) { dec_free(&one8th); goto done; }
                n++;
                if (dec_parse(&one8th, "0.125", 5) < 0) { dec_free(&one8th); goto done; }
            }
            dec_free(&one8th);
        }
        /* series: e^(+-t) = sum (+=t)^k / k! */
        if (dec_grow(&sum, 1) < 0 || dec_grow(&term, 1) < 0)
            goto done;
        sum.d[0] = 1; sum.nd = 1;
        term.d[0] = 1; term.nd = 1;
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
            /* alternating tails are bounded by the first dropped term */
            if (dec_adjusted(&term) + (int32_t)w + 3 < dec_adjusted(&sum))
                break;
            /* accumulate through tmp: dec_add(r, a, b) with r == a and
               a->exp above lo clobbers unread digits (see dec_atanh2) */
            if (dec_add(&tmp, &sum, &term) < 0 || dec_copy(&sum, &tmp) < 0
                || dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
                goto done;
        }
        while (n--) {                   /* e^t -> e^x by n squarings */
            if (dec_mul_into(&sum, &sum) < 0)
                goto done;
            if (dec_round_sig(&sum, w, RND_HALF_EVEN, 0) < 0)
                goto done;
        }
        /* a half-ulp tie whose truth lives in the dropped tail is decided by
           recomputing deeper; two refinements, then accept. A retry that
           would trip the budget leaves the tie-rounding in place -- the
           honest fallback is a possibly-1-ulp answer, not a refusal. */
        if (!dec_on_tie(&sum, prec) || attempt == 2)
            break;
        if ((double)2.6 * (double)(w + 50) * (double)(w + 50) * (double)(w + 50)
            > DEC_SERIES_CELLS)
            break;
        w += 50;
    }
    if (dec_round_sig(&sum, prec, RND_HALF_EVEN, 0) < 0)
        goto done;
    /* subnormal band: a result whose adjusted exponent fell below Emin
       (-999999) is quantized to the Etiny exponent, python's subnormal
       rounding -- the digits shrink to what fits above Etiny instead of
       staying a full-precision value the context could not hold */
    if (sum.nd && sum.exp + (int32_t)sum.nd - 1 < -999999) {
        if (dec_round_at(&sum, -999999 - (int32_t)prec + 1,
                         RND_HALF_EVEN, 0) < 0)
            goto done;
    }
    if (dec_copy(r, &sum) < 0)
        goto done;
    rc = 0;
done:
    dec_free(&t); dec_free(&sum); dec_free(&term); dec_free(&kk); dec_free(&tmp);
    return rc;
}


/* magic 0 = sqrt, 1 = exp, 2 = ln, 3 = log10 */
static JSValue dyn_dec_math(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), *r;
    uint32_t prec;
    int mode, rc;
    static const char *const NAMES[] = { "sqrt", "exp", "ln", "log10" };

    if (!a)
        return JS_EXCEPTION;
    /* {precision, rounding}: rounding is ACCEPTED and IGNORED -- these are the
       first nonlinear ops, and like python's decimal they are always
       correctly rounded half-even (IEEE's statistically-neutral default);
       div keeps its caller's mode, these do not have one. */
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
    case 0:  rc = dec_sqrt(r, a, prec); break;
    case 1:  rc = dec_exp(r, a, prec); break;
    case 2:  rc = dec_ln(r, a, prec); break;
    default: rc = dec_log10(r, a, prec); break;
    }
    if (rc == DEC_E_INVALID) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx,
            magic == 0
                ? "Decimal.sqrt: the square root of a negative number is not "
                  "a decimal"
                : "Decimal.%s: the logarithm of a negative number is not a "
                  "decimal",
            NAMES[magic]);
    }
    if (rc == DEC_E_OVERFLOW) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.exp: e^x overflows the context (the adjusted exponent "
            "passes 999999, the same Emax python decimal's default context "
            "sets); exp of a very negative x is 0, not an error");
    }
    if (rc == DEC_E_BUDGET) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx,
            "Decimal.%s: exceeds the operation budget at this precision (the "
            "series core is schoolbook; sqrt has no such cap)",
            NAMES[magic]);
    }
    if (rc < 0) {
        dec_free(r); free(r);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
                                  DEC_MAX_DIGITS);
    }
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

/* magic 0 = floor, 1 = ceil, 2 = trunc. EXACT: the integer toward
   -Infinity/+Infinity/zero, with no context and no rounding option -- these
   are definitions, not approximations. A zero result is 0, never -0 (the
   module's convention, like neg). */
static JSValue dyn_dec_integral(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t, *r;
    static const int MODES[] = { RND_FLOOR, RND_CEIL, RND_DOWN };

    (void)argc; (void)argv;
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
    if (!r) { dec_free(&t); return JS_EXCEPTION; }
    if (dec_copy(r, &t) < 0) {
        dec_free(&t); dec_free(r); free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec_free(&t);
    return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
}

/* divmod(x) -> [quotient, remainder], the EXACT truncated-division pair:
   q is toward zero, r takes the dividend's sign, and a == q*b + r on the
   nose (mod by zero throws). opts are accepted and ignored: both halves are
   exact, like add/sub/mul/mod. */
static JSValue dyn_dec_divmod(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), b, *q, *rr;
    JSValue arr, qv, rv;
    int rc;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Decimal arithmetic needs an operand");
    /*divmod ignores the bag (both halves are exact), but a bag with
       an unknown KEY is still the misspelled intent -- checked, not read. */
    if (dyn_dec_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (dyn_dec_from(ctx, argv[0], &b) < 0)
        return JS_EXCEPTION;
    q = dyn_dec_alloc(ctx);
    rr = dyn_dec_alloc(ctx);
    if (!q || !rr) { dec_free(&b); dec_free(q); free(q); dec_free(rr); free(rr); return JS_EXCEPTION; }
    rc = dec_int_divmod(q, rr, a, &b);
    dec_free(&b);
    if (rc == -2) {
        dec_free(q); free(q); dec_free(rr); free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: division by zero");
    }
    if (rc == DEC_E_TOOSLOW) {
        dec_free(q); free(q); dec_free(rr); free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: operands too large for one "
            "exact operation (divide is limited to %u cells)",
            (unsigned)DEC_MAX_DIV_CELLS);
    }
    if (rc < 0) {
        dec_free(q); free(q); dec_free(rr); free(rr);
        return JS_ThrowRangeError(ctx, "Decimal: result exceeds %u digits",
                                  DEC_MAX_DIGITS);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dec_free(q); free(q); dec_free(rr); free(rr);
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

/* Truncates toward zero. A value with a fractional part, or a magnitude at
   or past 2^63, throws RangeError: BigInt itself is arbitrary-precision, but
   the engine's C bridge hands one over as an int64, so that is the honest
   documented bound (9223372036854775807; -9223372036854775808 fits). */
static JSValue dyn_dec_to_bigint(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dec_t *a = dyn_dec_this(ctx, this_val), t;
    uint64_t v = 0;
    int i;

    (void)argc; (void)argv;
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
        /* digits() alone misses 1e100, whose DIGIT count is 1 -- the positive
           exponent widens the integer just the same */
        dec_free(&t);
        return JS_ThrowRangeError(ctx,
            "Decimal.toBigInt: the magnitude exceeds the int64 range this "
            "bridge returns (|x| must stay below 2^63)");
    }
    for (i = (int)t.nd - 1; i >= 0; i--)
        v = v * 10 + (uint64_t)t.d[i];
    for (i = 0; i < (int)t.exp; i++)     /* 25e5 -> 2500000: at most 19 digits,
                                            so the widening cannot wrap the u64 */
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
    /* 0 - v is the well-defined wrap for v == 2^63 (INT64_MIN's pattern) */
    return JS_NewBigInt64(ctx, a->sign ? (int64_t)(0u - v) : (int64_t)v);
}

/* The constants: Decimal.ZERO / ONE / TWO / TEN / NEG_ONE. */
static JSValue dyn_dec_const(JSContext *ctx, const char *text)
{
    dec_t *x = dyn_dec_alloc(ctx);

    if (!x)
        return JS_EXCEPTION;
    if (dec_parse(x, text, strlen(text)) < 0) {
        dec_free(x); free(x);
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

static void dyn_money_dispose(void *p)
{
    free(p);
}

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

static JSValue dyn_money_new(JSContext *ctx, JSValueConst new_target,
                             int64_t amount, const char *code, uint8_t minor)
{
    money_t *m = (money_t *)malloc(sizeof *m);

    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->amount = amount;
    m->minor = minor;
    memcpy(m->code, code, 3);
    m->code[3] = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_money_class_id, m, dyn_money_dispose);
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

/* Defined below with the factories; the constructor shares it. */
static int money_minor_opt(JSContext *ctx, JSValueConst opts,
                           const char *what, uint8_t base, uint8_t *minor);

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

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "new Money(minorUnits, currency): a currency code is required");
    if (money_amount(ctx, argv[0], &amount) < 0 || money_code(ctx, argv[1], up) < 0)
        return JS_EXCEPTION;
    /*{minorDigits} is the whole bag -- strict now too, so a typo'd key
       stops silently leaving the amount scaled by the default. */
    if (money_minor_opt(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "new Money",
                        money_minor(up), &minor))
        return JS_EXCEPTION;
    return dyn_money_new(ctx, new_target, amount, up, minor);
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
    if ((*a)->minor != (*b)->minor) {
        /* Same code, different SCALE: the amount is a count of MINOR units,
           so the same integer means different money at 2 vs 6 digits. Refuse;
           do NOT renormalize -- silently rescaling a ledger amount is the bug
           this type exists to prevent. */
        JS_ThrowTypeError(ctx, "Money: cannot combine %s amounts with %u and %u "
                          "minor digits; the scales must match",
                          (*a)->code, (unsigned)(*a)->minor,
                          (unsigned)(*b)->minor);
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
    return dyn_money_new(ctx, JS_UNDEFINED, r, a->code, a->minor);
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
    /* The bound is computed in 128 bits: |INT64_MIN| has no positive int64_t
       representation, so INT64_MIN / 1 == INT64_MIN never tripped the old
       check and mul(-1) wrapped INT64_MIN back to itself (-- a silent
       wrong amount on the exact value the type is supposed to guard). */
    if (k != 0 && (( __int128)a->amount * ( __int128)k > (__int128)INT64_MAX
                   || ( __int128)a->amount * ( __int128)k < (__int128)INT64_MIN))
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    return dyn_money_new(ctx, JS_UNDEFINED, a->amount * k, a->code, a->minor);
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
    /* Accumulate the weights in a 128-bit sum so a hostile set of weights (e.g.
       several 2^62 entries) cannot silently wrap the int64 total to 0 or to a
       negative value. If it would exceed the int64 range, fail cleanly rather
       than later dividing by a wrapped (possibly zero) total — the old code hit
       a SIGFPE or produced shares whose sum is no longer the original. */
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
            /* (int64_t)d is UB for d >= 2^63: range-check BEFORE the cast,
               with the established overflow error. */
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
    if (a->amount == INT64_MIN) {
        /* -a->amount below is UB for INT64_MIN: the split would be
           9223372036854775808 shares of -1, which no int64 amount can hold
           either. Refuse with the standard overflow error. */
        free(w);
        return JS_ThrowRangeError(ctx, "Money: the amount overflows an int64");
    }
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
        JSValue m = dyn_money_new(ctx, JS_UNDEFINED, neg ? -w[i] : w[i], a->code, a->minor);
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
        return dyn_dec_wrap(ctx, JS_UNDEFINED, r);
    }
    return JS_NewString(ctx, buf);
}

/* ---------------------------------------------------------------- ---
 * Money.fromString / Money.fromDecimal / Money.fromMinor: the three input
 * forms callers otherwise hand-roll minorUnits for. ALL of them are EXACT --
 * the stored value never touches a double, so a ledger amount past 2^53
 * minor units survives -- and all refuse a value that is not a whole count
 * of the currency's minor unit rather than silently rounding a cent away.
 * (The number-typed amount() accessor is the one place precision ends at
 * 2^53: read an exact value with toString()/toDecimal().)
 *
 *   fromString(str, currency[, opts])   human decimal text, ',' grouping
 *   fromDecimal(d, currency[, opts])    an exact Decimal
 *   fromMinor(n, currency[, opts])      a raw minor count: number | bigint |
 *                                       amount string (the spellings that
 *                                       can carry an int64 without loss)
 *
 * All three share ONE scale rule: trailing zeros are TEXT, not value --
 * "1.500" is exactly 150 minor units on USD and is admitted, while a
 * nonzero digit past the currency's scale ("1.005") is a finer value and is
 * refused. The three also share one strict bag, {minorDigits}, which
 * is also the constructor's bag -- now strict there too, so the rule is
 * learnable across the class: a typo'd key used to be silently dropped and
 * the amount was silently scaled. */

/*the Money options bag is exactly {minorDigits}; an unknown key is the
 * misspelled intent and refuses, naming the key and the valid set. */
static int money_opts_strict(JSContext *ctx, JSValueConst o)
{
    if (!JS_IsObject(o))
        return 0;
    {
        JSPropertyEnum *props = NULL;
        uint32_t nprops = 0, i;
        int bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) { bad = 1; break; }
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

/* base (from the currency table) plus the bag's {minorDigits} override. */
static int money_minor_opt(JSContext *ctx, JSValueConst opts,
                           const char *what, uint8_t base, uint8_t *minor)
{
    JSValue v;
    int64_t md;
    double mdd;

    *minor = base;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;   /* null/undefined are not bags: the no-options reading */
    if (!JS_IsObject(opts)) {
        /* A bag is an object or absent: a string/number in this position is
           a caller bug, not a silently defaulted bag. */
        JS_ThrowTypeError(ctx, "%s: options must be an object", what);
        return -1;
    }
    if (money_opts_strict(ctx, opts))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "minorDigits");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        /* minorDigits is an INTEGER like every other count in this module:
           2.5 silently truncating to 2 quietly changes what one printed
           "0.01" means -- the same coercion trap money_amount rejects. */
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

/* [-]?( D{1,3} (,D{3})* | D+ ) (.D+)? -> the exact int64 minor-unit count.
 * EXACT: the integer and fraction are accumulated separately (the fraction
 * into a bounded < 10^7 counter) and combined once in 128 bits, so
 * "9,223,372,036,854,775,807.00" fits while "9,223,372,036,854,775,808.00"
 * overflows cleanly -- and nothing ever wraps.
 *
 * Grouping is structural, not stripped: after the first group every ',' must
 * introduce exactly 3 digits ("1,234.56" and "1234.56" are one value; "12,34"
 * and "1,23.45" are typos, and a typo that is silently accepted becomes a
 * silently wrong amount).
 *
 * The scale rule is the VALUE rule (the one fromDecimal follows): trailing
 * zeros past the currency's minor digits are exact TEXT, not finer value --
 * "1.500" on USD is 150 -- while a NONZERO digit past the scale is a
 * fraction finer than the minor unit and is refused. Rounding a fraction of
 * a cent away is exactly the bug this type exists to prevent. */
static int money_parse_decimal_str(JSContext *ctx, const char *s, size_t n,
                                   const char *what, uint8_t minor,
                                   int64_t *out)
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
            nfrac = -1;         /* marks "point seen, no fraction digit yet" */
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
                /* Past the currency's scale only ZEROS are exact text
                   ("1.500" on USD is 150); a nonzero digit there is a value
                   finer than the minor unit -- refuse, never round. */
                JS_ThrowRangeError(ctx,
                    "%s: the amount has value past the %u minor digits -- "
                    "a fractional minor unit is not money", what,
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
                               "fraction digits", what, s);
        return -1;
    }
    if (!seen_digit) {
        JS_ThrowTypeError(ctx, "%s: the amount string has no digits", what);
        return -1;
    }
    if (comma) {
        /* re-walk the integer part: first group 1..3 digits, the rest == 3 */
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
    /* value = acc * 10^minor + frac scaled to the minor scale (frac carries
       at most `minor` digits -- zero text past the scale was already
       admitted as exact), one 128-bit combine */
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

/* The shared (currency, opts) half of the three factories. */
static int money_factory_args(JSContext *ctx, JSValueConst csv,
                              JSValueConst optsv, const char *what,
                              char up[4], uint8_t *minor)
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

/* Money.fromString(str, currency[, opts]) */
static JSValue dyn_money_from_string(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    char up[4];
    uint8_t minor;
    const char *s;
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

/* Money.fromDecimal(d, currency[, opts]) -- exact: d * 10^minorDigits must
 * be an integer (a Decimal with more scale than the currency is refused, not
 * rounded). */
static JSValue dyn_money_from_decimal(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    char up[4];
    uint8_t minor;
    dec_t *x;
    __int128 acc = 0;
    int32_t p, top;
    int64_t amount;
    (void)this_val;

    if (argc < 2 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    x = (dec_t *)JS_GetOpaque(argv[0], dyn_dec_class_id);
    if (!x)
        return JS_ThrowTypeError(ctx,
            "Money.fromDecimal(d, currency[, opts]): d must be a Decimal");
    if (money_factory_args(ctx, argv[1], argc > 2 ? argv[2] : JS_UNDEFINED,
                           "Money.fromDecimal", up, &minor))
        return JS_EXCEPTION;
    if (dec_is_zero(x)) {
        amount = 0;
    } else {
        top = x->exp + (int32_t)x->nd - 1;   /* highest nonzero position */
        /* positions BELOW -minor carry value the currency cannot represent */
        for (p = x->exp; p < -(int32_t)minor && p <= top; p++)
            if (dec_at(x, p) != 0)
                return JS_ThrowRangeError(ctx,
                    "Money.fromDecimal: the Decimal has fraction digits past "
                    "the %u minor digits -- a fractional minor unit is not "
                    "money", (unsigned)minor);
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

/* Money.fromMinor(n, currency[, opts]) -- the raw minor-unit count as
 * number (integer), bigint, or an amount string. The bigint and string
 * spellings are the exact ones: a ledger amount past 2^53 has no faithful
 * Number form, and the string never rounds at all. The string takes the
 * SAME amount grammar as fromString (optional ',' grouping, optional '.'
 * fraction) with the value required to be whole minor units: "1,234" and
 * "5.0" are 1234 and 5, while "5.5" is refused. */
static JSValue dyn_money_from_minor(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
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
        const char *s;
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
        /* JS_ToBigInt64 silently reduces mod 2^64: range-check the decimal
         * form first (the same trap the varint readers refuse). */
        const char *s;
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
            const char *lim = neg ? "9223372036854775808" : "9223372036854775807";
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

/* ------------------------------------------------------------ registration */

static int dyn_decimal_init_module(JSContext *ctx, JSModuleDef *m)
{
    static const struct { const char *name, *text; } DEC_CONSTS[] = {
        { "ZERO", "0" }, { "ONE", "1" }, { "TWO", "2" },
        { "TEN", "10" }, { "NEG_ONE", "-1" },
    };
    unsigned k;

    if (dyn_register_plain_class(ctx, m, &dyn_dec_class_id, &dyn_dec_class,
                                 dyn_dec_proto, countof(dyn_dec_proto),
                                 dyn_dec_ctor, "Decimal") < 0)
        return -1;
    {   /* the constants hang off the CONSTRUCTOR: same recovery the shared
           registrar itself uses -- proto, then its "constructor" */
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
            if (JS_IsException(v) ||
                JS_DefinePropertyValueStr(ctx, ctor, DEC_CONSTS[k].name, v,
                                          JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_plain_class(ctx, m, &dyn_money_class_id,
                                 &dyn_money_class, dyn_money_proto,
                                 countof(dyn_money_proto), dyn_money_ctor,
                                 "Money") < 0)
        return -1;
    {   /*the input factories hang off the CONSTRUCTOR as statics,
           same recovery the Decimal constants above use. */
        JSValue proto = JS_GetClassProto(ctx, dyn_money_class_id);
        JSValue ctor;
        static const struct { const char *name; JSCFunction *fn; int arity; }
        MONEY_STATICS[] = {
            { "fromString", dyn_money_from_string, 2 },
            { "fromDecimal", dyn_money_from_decimal, 2 },
            { "fromMinor", dyn_money_from_minor, 2 },
        };
        unsigned k;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor))
            return -1;
        for (k = 0; k < countof(MONEY_STATICS); k++) {
            JSValue fn = JS_NewCFunction(ctx, MONEY_STATICS[k].fn,
                                         MONEY_STATICS[k].name,
                                         MONEY_STATICS[k].arity);
            if (JS_IsException(fn) ||
                JS_DefinePropertyValueStr(ctx, ctor, MONEY_STATICS[k].name, fn,
                                          JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
        }
        JS_FreeValue(ctx, ctor);
    }
    return 0;
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
