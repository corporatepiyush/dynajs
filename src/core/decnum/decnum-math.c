#include "decnum.h"

int dec_pow_uint(dec_t* acc, dec_t* base, uint64_t e)
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

static int dec_isqrt(const uint8_t* msd, uint32_t nd,
    uint8_t* root, uint32_t* rln,
    uint8_t* rem, uint32_t* rmn)
{
    uint32_t S = (nd + 1) / 2, rl = 0, rn = 0, i;
    uint8_t *b20, *prod;

    *rln = 0;
    *rmn = 0;
    b20 = (uint8_t*)dyn_dec_calloc(2u * S + 12u, 1);
    if (!b20)
        return -1;
    prod = b20 + S + 4;
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
        {
            for (d = 9; d >= 0; d--) {
                uint32_t pl;
                memset(prod, 0, bl + 2);
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
                    break;
                }
            }
        }
        memmove(root + 1, root, rl);
        root[0] = (uint8_t)(d < 0 ? 0 : d);
        rl++;
    }
    dyn_dec_free(b20);
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

int dec_sqrt(dec_t* r, const dec_t* a, uint32_t prec)
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
    md = (uint8_t*)dyn_dec_calloc(D + 2, 1);
    root = (uint8_t*)dyn_dec_calloc(S + 2, 1);
    rem = (uint8_t*)dyn_dec_calloc(D + 4, 1);
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
    dyn_dec_free(md);
    dyn_dec_free(root);
    dyn_dec_free(rem);
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

int dec_ln(dec_t* r, const dec_t* a, uint32_t prec)
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

int dec_log10(dec_t* r, const dec_t* a, uint32_t prec)
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

int dec_exp(dec_t* r, const dec_t* a, uint32_t prec)
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
