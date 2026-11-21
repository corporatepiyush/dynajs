#include "decnum.h"

int dec_sub_abs(dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    int32_t ha = a->exp + (int32_t)a->nd, hb = b->exp + (int32_t)b->nd;
    int32_t hi = ha > hb ? ha : hb;
    int64_t span = (int64_t)hi - (int64_t)lo;
    uint32_t n, i;
    int borrow = 0;

    if (b->nd == 0)
        return dec_copy(r, a);
    if (span < 0 || span > (int64_t)DEC_MAX_DIGITS)
        return -1;
    n = (uint32_t)span;
    if (dec_grow(r, n ? n : 1) < 0)
        return -1;
    {
        const uint8_t *ad = a->d, *bd = b->d;
        uint32_t oa = (uint32_t)(a->exp - lo), ob = (uint32_t)(b->exp - lo), na = a->nd, nb = b->nd;
        for (i = 0; i < n; i++) {
            int s = (i - oa < na ? ad[i - oa] : 0) - (i - ob < nb ? bd[i - ob] : 0) - borrow;
            borrow = s < 0;
            r->d[i] = (uint8_t)(s + 10 * borrow);
        }
    }
    r->nd = n;
    r->exp = lo;
    dec_trim(r);
    return 0;
}

int dec_sub(dec_t* r, const dec_t* a, const dec_t* b)
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

#define DEC_MUL_STACK_LIMBS 64

static void dec_pack4(uint32_t* L, const uint8_t* d, uint32_t nd)
{
    uint32_t k, n = nd / 4;
    for (k = 0; k < n; k++)
        L[k] = d[4 * k] + 10u * d[4 * k + 1] + 100u * d[4 * k + 2] + 1000u * d[4 * k + 3];
    if (nd & 3) {
        uint32_t v = 0, m = 1;
        for (k = 4 * n; k < nd; k++, m *= 10u)
            v += d[k] * m;
        L[n] = v;
    }
}

int dec_mul(dec_t* r, const dec_t* a, const dec_t* b)
{
    uint32_t n, i, j, la, lb, lt;
    uint64_t stack_acc[DEC_MUL_STACK_LIMBS], *acc, carry = 0;
    uint32_t stack_l[DEC_MUL_STACK_LIMBS], *A, *B;
    void* heap = NULL;

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
    la = (a->nd + 3) / 4;
    lb = (b->nd + 3) / 4;
    lt = la + lb;
    if (lt <= DEC_MUL_STACK_LIMBS) {
        acc = stack_acc;
        A = stack_l;
    } else {
        heap = dyn_dec_malloc((size_t)lt * (sizeof *acc + sizeof *A));
        if (!heap)
            return -1;
        acc = (uint64_t*)heap;
        A = (uint32_t*)(acc + lt);
    }
    B = A + la;
    dec_pack4(A, a->d, a->nd);
    dec_pack4(B, b->d, b->nd);
    memset(acc, 0, (size_t)lt * sizeof *acc);
    for (i = 0; i < la; i++) {
        uint64_t av = A[i];
        if (!av)
            continue;
        for (j = 0; j < lb; j++)
            acc[i + j] += av * B[j];
    }
    if (dec_grow(r, n) < 0) {
        dyn_dec_free(heap);
        return -1;
    }
    for (i = 0; i < lt; i++) {
        uint64_t v = acc[i] + carry;
        uint32_t limb = (uint32_t)(v % 10000u), k;
        carry = v / 10000u;
        for (k = 0; k < 4 && 4 * i + k < n; k++, limb /= 10u)
            r->d[4 * i + k] = (uint8_t)(limb % 10u);
    }
    dyn_dec_free(heap);
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

int dec_round_at(dec_t* x, int32_t target, int mode, int sticky)
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

int dec_round_sig(dec_t* x, uint32_t prec, int mode, int sticky)
{
    if (x->nd <= prec && !sticky)
        return 0;
    if (x->nd == 0)
        return 0;
    if (sticky && x->nd < prec) {
        uint32_t pad = prec - x->nd;
        if (dec_grow(x, prec) < 0)
            return -1;
        memmove(x->d + pad, x->d, x->nd);
        memset(x->d, 0, pad);
        x->nd = prec;
        x->exp -= (int32_t)pad;
    }
    return dec_round_at(x, x->exp + (int32_t)x->nd - (int32_t)prec, mode, sticky);
}

uint32_t dec_sub_into(uint8_t* rem, uint32_t rn, const uint8_t* b,
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

static uint32_t dec_mulsub_into(uint8_t* rem, uint32_t rn, const uint8_t* b, uint32_t bn, uint32_t m)
{
    uint32_t k;
    int32_t borrow = 0;
    for (k = 0; k < rn; k++) {
        int32_t s = (int32_t)rem[k] - (int32_t)(k < bn ? b[k] * m : 0) - borrow;
        borrow = 0;
        if (s < 0) {
            borrow = (-s + 9) / 10;
            s += 10 * borrow;
        }
        rem[k] = (uint8_t)s;
    }
    while (rn && rem[rn - 1] == 0)
        rn--;
    return rn;
}

static void dec_divmod_u64(uint8_t* q, uint32_t* qn, uint8_t* rem, uint32_t* rn,
    const uint8_t* a, uint32_t an, uint64_t bv)
{
    static const uint64_t p10[10] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u, 1000000000u };
    uint64_t rr = 0;
    uint32_t r = 0;
    int32_t i = (int32_t)an - 1;
    if (bv < p10[9]) {
        while (i >= 0) {
            int32_t c = (i + 1) % 9, k;
            uint64_t x = 0, qq;
            if (c == 0)
                c = 9;
            for (k = 0; k < c; k++)
                x = x * 10u + a[i - k];
            x += rr * p10[c];
            qq = x / bv;
            rr = x % bv;
            for (k = c - 1; k >= 0; k--) {
                q[i - k] = (uint8_t)(qq % 10u);
                qq /= 10u;
            }
            i -= c;
        }
    } else {
        for (; i >= 0; i--) {
            rr = rr * 10u + a[i];
            q[i] = (uint8_t)(rr / bv);
            rr %= bv;
        }
    }
    *qn = an;
    while (*qn && q[*qn - 1] == 0)
        (*qn)--;
    while (rr) {
        rem[r++] = (uint8_t)(rr % 10u);
        rr /= 10u;
    }
    *rn = r;
}

int dec_divmod_int(uint8_t* q, uint32_t* qn, uint8_t* rem, uint32_t* rn,
    const uint8_t* a, uint32_t an,
    const uint8_t* b, uint32_t bn)
{
    int32_t i;
    uint32_t r = 0;
    uint64_t btop = 0;

    if (bn <= 18) {
        uint64_t bv = 0;
        uint32_t k = bn;
        while (k)
            bv = bv * 10u + b[--k];
        if (bv) {
            dec_divmod_u64(q, qn, rem, rn, a, an, bv);
            return 0;
        }
    }
    *qn = an;
    memset(q, 0, an);
    {
        uint32_t k, s = bn > 17 ? bn - 17 : 0;
        for (k = bn; k-- > s;)
            btop = btop * 10u + b[k];
    }
    for (i = (int32_t)an - 1; i >= 0; i--) {
        uint8_t* w = rem + i;
        int d = 0;
        w[0] = a[i];
        r++;
        while (r && w[r - 1] == 0)
            r--;
        if (r >= bn && bn > 17) {
            uint64_t rtop = 0;
            uint32_t k, s = bn - 17;
            for (k = r; k-- > s;)
                rtop = rtop * 10u + w[k];
            d = (int)(rtop / (btop + 1u));
            if (d > 9)
                return -1;
            if (d)
                r = dec_mulsub_into(w, r, b, bn, (uint32_t)d);
        }
        while (dec_ge(w, r, b, bn)) {
            r = dec_sub_into(w, r, b, bn);
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

int dec_int_divmod(dec_t* qout, dec_t* r, const dec_t* a, const dec_t* b)
{
    int32_t lo = a->exp < b->exp ? a->exp : b->exp;
    uint32_t an, bn, qn, rn;
    uint8_t sbuf[512], *ad = NULL, *bd = NULL, *q = NULL, *rem = NULL, *heap = NULL;
    size_t tot;
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
    {
        int64_t an64 = (int64_t)a->exp + (int64_t)a->nd - (int64_t)lo;
        int64_t bn64 = (int64_t)b->exp + (int64_t)b->nd - (int64_t)lo;
        if (an64 < 0 || bn64 < 0 || an64 > (int64_t)DEC_MAX_DIGITS
            || bn64 > (int64_t)DEC_MAX_DIGITS)
            return -1;
        an = (uint32_t)an64;
        bn = (uint32_t)bn64;
    }
    if ((uint64_t)an * (uint64_t)bn > DEC_MAX_DIV_CELLS)
        return DEC_E_TOOSLOW;
    tot = 3u * (size_t)an + bn + 1u;
    if (tot <= sizeof sbuf)
        ad = sbuf;
    else if (!(ad = heap = (uint8_t*)dyn_dec_malloc(tot)))
        return -1;
    bd = ad + an;
    q = bd + bn;
    rem = q + an;
    memset(ad, 0, (size_t)an + bn);
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
    dyn_dec_free(heap);
    return rc;
}

void dec_write_fixed(db_t* b, const dec_t* x, int32_t dp, int neg)
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

int dec_mul_into(dec_t* acc, const dec_t* m)
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
