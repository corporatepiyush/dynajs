#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "bson.h"
#include "../dyn-cdefs.h"

typedef unsigned __int128 bson_u128;

#define BSON_D128_BIAS 6176
#define BSON_D128_EMAX 6111
#define BSON_D128_EMIN (-6176)

static bson_u128 bson_d128_max_coeff(void)
{
    bson_u128 m = 1;
    int i;
    for (i = 0; i < 34; i++)
        m *= 10;
    return m - 1;
}

static int bson_d128_unpack(const uint8_t v[16], int* sign, int* exp, bson_u128* coeff)
{
    uint64_t lo = dyn_bson_rd64(v), hi = dyn_bson_rd64(v + 8);
    *sign = (int)(hi >> 63);
    if (((hi >> 61) & 3) == 3) {
        uint32_t comb = (uint32_t)((hi >> 58) & 0x1F);
        if (comb == 0x1E)
            return 1;
        if (comb == 0x1F)
            return 2;
        *exp = (int)((hi >> 47) & 0x3FFF) - BSON_D128_BIAS;
        *coeff = 0;
        return 0;
    }
    *exp = (int)((hi >> 49) & 0x3FFF) - BSON_D128_BIAS;
    *coeff = ((bson_u128)(hi & 0x1FFFFFFFFFFFFull) << 64) | lo;
    if (*coeff > bson_d128_max_coeff())
        *coeff = 0;
    return 0;
}

int dyn_bson_dec128_to_string(const uint8_t v[16], char* out, size_t cap)
{
    int sign, exp, kind, n = 0, adj, i;
    bson_u128 c;
    char digits[40], tmp[48];
    size_t o = 0;
    kind = bson_d128_unpack(v, &sign, &exp, &c);
    if (cap < 48)
        return -1;
    if (kind == 2) {
        memcpy(out, "NaN", 4);
        return 3;
    }
    if (kind == 1) {
        if (sign) {
            memcpy(out, "-Infinity", 10);
            return 9;
        }
        memcpy(out, "Infinity", 9);
        return 8;
    }
    do {
        tmp[n++] = (char)('0' + (int)(c % 10));
        c /= 10;
    } while (c);
    for (i = 0; i < n; i++)
        digits[i] = tmp[n - 1 - i];
    if (sign)
        out[o++] = '-';
    adj = exp + n - 1;
    if (exp <= 0 && adj >= -6) {
        if (exp == 0) {
            memcpy(out + o, digits, (size_t)n);
            o += (size_t)n;
        } else if (n > -exp) {
            int ip = n + exp;
            memcpy(out + o, digits, (size_t)ip);
            o += (size_t)ip;
            out[o++] = '.';
            memcpy(out + o, digits + ip, (size_t)(-exp));
            o += (size_t)(-exp);
        } else {
            out[o++] = '0';
            out[o++] = '.';
            for (i = 0; i < -exp - n; i++)
                out[o++] = '0';
            memcpy(out + o, digits, (size_t)n);
            o += (size_t)n;
        }
    } else {
        out[o++] = digits[0];
        if (n > 1) {
            out[o++] = '.';
            memcpy(out + o, digits + 1, (size_t)(n - 1));
            o += (size_t)(n - 1);
        }
        o += (size_t)snprintf(out + o, cap - o, "E%c%d", adj < 0 ? '-' : '+', adj < 0 ? -adj : adj);
    }
    out[o] = 0;
    return (int)o;
}

static int bson_ieq(const char* s, size_t n, const char* lit)
{
    size_t i;
    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != lit[i])
            return 0;
    }
    return 1;
}

static void bson_d128_pack(int sign, int exp, bson_u128 c, uint8_t out[16])
{
    uint64_t hi = ((uint64_t)sign << 63) | ((uint64_t)(exp + BSON_D128_BIAS) << 49)
        | (uint64_t)(c >> 64);
    dyn_bson_wr64(out, (uint64_t)c);
    dyn_bson_wr64(out + 8, hi);
}

int dyn_bson_dec128_from_string(const char* s, size_t n, uint8_t out[16])
{
    size_t i = 0;
    int sign = 0, exp = 0, nd = 0, seen_digit = 0, seen_dot = 0, frac = 0;
    bson_u128 c = 0, maxc = bson_d128_max_coeff();
    if (n && (s[0] == '-' || s[0] == '+')) {
        sign = s[0] == '-';
        i = 1;
    }
    if (bson_ieq(s + i, n - i, "nan")) {
        dyn_bson_wr64(out, 0);
        dyn_bson_wr64(out + 8, 0x7C00000000000000ull);
        return 0;
    }
    if (bson_ieq(s + i, n - i, "inf") || bson_ieq(s + i, n - i, "infinity")) {
        dyn_bson_wr64(out, 0);
        dyn_bson_wr64(out + 8, (sign ? 0xF800000000000000ull : 0x7800000000000000ull));
        return 0;
    }
    for (; i < n; i++) {
        char ch = s[i];
        if (ch == '.') {
            if (seen_dot)
                return -1;
            seen_dot = 1;
            continue;
        }
        if (ch < '0' || ch > '9')
            break;
        seen_digit = 1;
        if (seen_dot)
            frac++;
        if (c == 0 && ch == '0')
            continue;
        if (nd >= 34) {
            if (ch != '0')
                return -2;
            if (!seen_dot)
                exp++;
            else
                frac--;
            continue;
        }
        c = c * 10 + (bson_u128)(ch - '0');
        nd++;
    }
    if (!seen_digit)
        return -1;
    if (i < n) {
        int esign = 1;
        long e = 0;
        if (s[i] != 'e' && s[i] != 'E')
            return -1;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            esign = s[i] == '-' ? -1 : 1;
            i++;
        }
        if (i >= n)
            return -1;
        for (; i < n; i++) {
            if (s[i] < '0' || s[i] > '9')
                return -1;
            if (e < 100000)
                e = e * 10 + (s[i] - '0');
        }
        exp += (int)(esign * e);
    }
    exp -= frac;
    while (exp > BSON_D128_EMAX && c != 0 && c * 10 <= maxc) {
        c *= 10;
        exp--;
    }
    while (exp < BSON_D128_EMIN && c % 10 == 0 && c != 0) {
        c /= 10;
        exp++;
    }
    if (c == 0) {
        if (exp > BSON_D128_EMAX)
            exp = BSON_D128_EMAX;
        if (exp < BSON_D128_EMIN)
            exp = BSON_D128_EMIN;
    }
    if (exp > BSON_D128_EMAX || exp < BSON_D128_EMIN)
        return -3;
    bson_d128_pack(sign, exp, c, out);
    return 0;
}

int dyn_bson_dec128_to_f64(const uint8_t v[16], double* out)
{
    char buf[64];
    if (dyn_bson_dec128_to_string(v, buf, sizeof buf) < 0)
        return -1;
    if (buf[0] == 'N') {
        *out = DYN_NAN;
        return 0;
    }
    if (strcmp(buf, "Infinity") == 0) {
        *out = DYN_INFINITY;
        return 0;
    }
    if (strcmp(buf, "-Infinity") == 0) {
        *out = -DYN_INFINITY;
        return 0;
    }
    *out = strtod(buf, NULL);
    return 0;
}
