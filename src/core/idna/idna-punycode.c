#include "idna.h"

enum { PC_BASE = 36,
    PC_TMIN = 1,
    PC_TMAX = 26,
    PC_SKEW = 38,
    PC_DAMP = 700,
    PC_INITIAL_N = 128,
    PC_INITIAL_BIAS = 72,
    PC_DELIM = 0x2D };

static int pc_adapt(int delta, int numpoints, int firsttime)
{
    int k = 0;
    delta = firsttime ? delta / PC_DAMP : delta / 2;
    delta += delta / numpoints;
    while (delta > ((PC_BASE - PC_TMIN) * PC_TMAX) / 2) {
        delta /= PC_BASE - PC_TMIN;
        k += PC_BASE;
    }
    return k + (PC_BASE - PC_TMIN + 1) * delta / (delta + PC_SKEW);
}

static int pc_digit(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= '0' && c <= '9')
        return c - '0' + 26;
    return -1;
}

int pc_encode(const uint32_t* cps, int n, dyn_cp_t* out)
{
    int i, h, b, bias, nout;
    int64_t delta;
    uint32_t nval, m;
    for (i = 0; i < n; i++) {
        if (cps[i] > 0x10FFFF || (cps[i] >= 0xD800 && cps[i] <= 0xDFFF))
            return -1;
    }
    h = b = 0;
    for (i = 0; i < n; i++)
        if (cps[i] < 0x80) {
            if (dyn_cp_put(out, cps[i]))
                return -1;
            b++;
        }
    h = b;
    if (b > 0)
        if (dyn_cp_put(out, PC_DELIM))
            return -1;
    nval = PC_INITIAL_N;
    delta = 0;
    bias = PC_INITIAL_BIAS;
    nout = 0;
    while (h < n) {
        m = 0x110000;
        for (i = 0; i < n; i++)
            if (cps[i] >= nval && cps[i] < m)
                m = cps[i];
        delta += (int64_t)(m - nval) * (h + 1);
        if (delta > 0x7fffffff)
            return -1;
        nval = m;
        for (i = 0; i < n; i++) {
            if (cps[i] < nval)
                delta++;
            if (cps[i] == nval) {
                int64_t q = delta;
                int k;
                for (k = PC_BASE;; k += PC_BASE) {
                    int t = (k <= bias) ? PC_TMIN
                                        : (k >= bias + PC_TMAX ? PC_TMAX : k - bias);
                    int d;
                    if (q < t)
                        break;
                    d = t + (q - t) % (PC_BASE - t);
                    if (dyn_cp_put(out, d < 26 ? d + 'a' : d - 26 + '0'))
                        return -1;
                    q = (q - t) / (PC_BASE - t);
                }
                if (dyn_cp_put(out, q < 26 ? q + 'a' : q - 26 + '0'))
                    return -1;
                bias = pc_adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        nval++;
        nout++;
    }
    (void)nout;
    return 0;
}

int pc_decode(const char* s, size_t n, dyn_cp_t* out)
{
    size_t ip = 0, i;
    uint32_t nval = PC_INITIAL_N;
    int bias = PC_INITIAL_BIAS;
    long long ival = 0;
    ip = 0;
    for (i = 0; i < n; i++)
        if (s[i] == PC_DELIM)
            ip = i + 1;
    if (ip > 0) {
        size_t j;
        for (j = 0; j + 1 < ip; j++) {
            if ((unsigned char)s[j] >= 0x80)
                return -1;
            if (dyn_cp_put(out, (unsigned char)s[j]))
                return -1;
        }
    }
    while (ip < n) {
        long long oldi = ival;
        long long w = 1;
        int k;
        for (k = PC_BASE;; k += PC_BASE) {
            int d, t;
            if (ip >= n)
                return -1;
            d = pc_digit((unsigned char)s[ip++]);
            if (d < 0)
                return -1;
            if (d > (0x7FFFFFFF - ival) / w)
                return -1;
            ival += d * w;
            t = (k <= bias) ? PC_TMIN : (k >= bias + PC_TMAX ? PC_TMAX : k - bias);
            if (d < t)
                break;
            if (w > (0x7FFFFFFF - (PC_BASE - t)) / (PC_BASE - t))
                return -1;
            w *= PC_BASE - t;
        }
        {
            size_t outlen = out->n + 1;
            bias = pc_adapt((int)(ival - oldi), (int)outlen, oldi == 0);
            nval += (uint32_t)(ival / (long long)outlen);
            ival %= (long long)outlen;
        }
        if (nval > 0x10FFFF || (nval >= 0xD800 && nval <= 0xDFFF))
            return -1;
        {
            uint32_t* arr = (uint32_t*)realloc(out->p, (out->n + 1) * sizeof(uint32_t));
            if (!arr)
                return -1;
            out->p = arr;
            for (i = out->n; i > (size_t)ival; i--)
                out->p[i] = out->p[i - 1];
            out->p[ival] = nval;
            out->n++;
            out->cap = out->n;
        }
        ival++;
    }
    return 0;
}
