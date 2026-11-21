#include "decnum.h"

int dec_grow(dec_t* x, uint32_t need)
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
    np = (uint8_t*)dyn_dec_malloc(need);
    if (!np)
        return -1;
    memcpy(np, x->d, x->nd);
    if (x->d != x->inl)
        dyn_dec_free(x->d);
    x->d = np;
    x->cap = need;
    return 0;
}

void dec_trim(dec_t* x)
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

static int dec_add_abs(dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    int64_t span = (int64_t)hi - (int64_t)lo;
    uint32_t n, i;
    int carry = 0;

    if (a->nd == 0)
        return dec_copy(r, b);
    if (b->nd == 0)
        return dec_copy(r, a);
    if (span < 0 || span > (int64_t)DEC_MAX_DIGITS)
        return -1;
    n = (uint32_t)span + 1;
    if (dec_grow(r, n) < 0)
        return -1;
    {
        const uint8_t *ad = a->d, *bd = b->d;
        uint32_t oa = (uint32_t)(a->exp - lo), ob = (uint32_t)(b->exp - lo), na = a->nd, nb = b->nd;
        for (i = 0; i < n; i++) {
            int s = (i - oa < na ? ad[i - oa] : 0) + (i - ob < nb ? bd[i - ob] : 0) + carry;
            carry = s >= 10;
            r->d[i] = (uint8_t)(s - 10 * carry);
        }
    }
    r->nd = n;
    r->exp = lo;
    dec_trim(r);
    return 0;
}

int dec_add(dec_t* r, const dec_t* a, const dec_t* b)
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

#define DEC_DIV_STACK 512

int dec_div(dec_t* r, const dec_t* a, const dec_t* b, uint32_t prec, int mode)
{
    uint8_t sbuf[DEC_DIV_STACK], *num = NULL, *q = NULL, *rem = NULL, *heap = NULL;
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
    if (3u * nn + 1u <= DEC_DIV_STACK)
        num = sbuf;
    else if (!(num = heap = (uint8_t*)dyn_dec_malloc(3u * (size_t)nn + 1u)))
        return -1;
    q = num + nn;
    rem = q + nn;
    memset(num, 0, extra);
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
    dyn_dec_free(heap);
    return rc;
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

int dec_parse(dec_t* x, const char* s, size_t n)
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

int dec_atanh2(dec_t* r, const dec_t* u, uint32_t w)
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
            dec_t kd;
            if (dec_small(&kd, (int64_t)2 * k + 1) < 0) {
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

int dec_small(dec_t* x, int64_t v)
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

static int dec_const_ln2_calc(dec_t* r, uint32_t w)
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

static int dec_const_ln10_calc(dec_t* r, uint32_t w)
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

#define DEC_CONST_CACHE_DIGITS 256

typedef struct {
    uint32_t w, nd;
    int32_t exp;
    uint8_t sign, ok;
    uint8_t d[DEC_CONST_CACHE_DIGITS];
} dec_const_slot_t;

static _Thread_local dec_const_slot_t dec_const_cache[2];

static int dec_const_cached(dec_t* r, uint32_t w, int which, int (*calc)(dec_t*, uint32_t))
{
    dec_const_slot_t* s = &dec_const_cache[which];
    int rc;
    if (s->ok && s->w == w) {
        if (dec_grow(r, s->nd ? s->nd : 1) < 0)
            return -1;
        memcpy(r->d, s->d, s->nd);
        r->nd = s->nd;
        r->exp = s->exp;
        r->sign = s->sign;
        return 0;
    }
    rc = calc(r, w);
    if (rc == 0 && r->nd <= DEC_CONST_CACHE_DIGITS) {
        memcpy(s->d, r->d, r->nd);
        s->nd = r->nd;
        s->exp = r->exp;
        s->sign = r->sign;
        s->w = w;
        s->ok = 1;
    }
    return rc;
}

int dec_const_ln2(dec_t* r, uint32_t w)
{
    return dec_const_cached(r, w, 0, dec_const_ln2_calc);
}

int dec_const_ln10(dec_t* r, uint32_t w)
{
    return dec_const_cached(r, w, 1, dec_const_ln10_calc);
}
