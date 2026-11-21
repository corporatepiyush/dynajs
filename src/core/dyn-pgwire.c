#include "dyn-pgwire.h"

#include <stdio.h>
#include <string.h>

uint32_t dyn_pgw_u32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint16_t dyn_pgw_u16(const uint8_t* p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

long dyn_pgw_frame(const uint8_t* b, size_t n, size_t maxmsg, char* type,
    const uint8_t** body, size_t* blen)
{
    uint32_t mlen;
    if (n < 5)
        return 0;
    mlen = dyn_pgw_u32(b + 1);
    if (mlen < 4 || mlen > maxmsg)
        return DYN_PGW_E_LEN;
    if (n - 1 < mlen)
        return 0;
    *type = (char)b[0];
    *body = b + 5;
    *blen = (size_t)mlen - 4;
    return (long)mlen + 1;
}

int dyn_pgw_rowdesc_begin(dyn_pgw_iter_t* it, const uint8_t* b, size_t n)
{
    it->b = b;
    it->n = n;
    it->at = 2;
    it->i = 0;
    if (n < 2)
        return DYN_PGW_E_TRUNC;
    it->count = dyn_pgw_u16(b);
    return it->count;
}

int dyn_pgw_rowdesc_next(dyn_pgw_iter_t* it, dyn_pgw_field_t* f)
{
    const uint8_t* b = it->b;
    size_t s = it->at, at = it->at;
    if (it->i >= it->count)
        return 0;
    while (at < it->n && b[at] != 0)
        at++;
    if (at >= it->n || it->n - at - 1 < 18)
        return DYN_PGW_E_TRUNC;
    f->name = b + s;
    f->nlen = at - s;
    at++;
    f->table_oid = dyn_pgw_u32(b + at);
    f->column = dyn_pgw_u16(b + at + 4);
    f->type_oid = dyn_pgw_u32(b + at + 6);
    f->format = dyn_pgw_u16(b + at + 16);
    it->at = at + 18;
    it->i++;
    return 1;
}

int dyn_pgw_datarow_begin(dyn_pgw_iter_t* it, const uint8_t* b, size_t n)
{
    return dyn_pgw_rowdesc_begin(it, b, n);
}

int dyn_pgw_datarow_next(dyn_pgw_iter_t* it, const uint8_t** col, uint32_t* clen)
{
    uint32_t l;
    if (it->i >= it->count)
        return 0;
    if (it->n - it->at < 4)
        return DYN_PGW_E_TRUNC;
    l = dyn_pgw_u32(it->b + it->at);
    it->at += 4;
    if (l != DYN_PGW_NULL && l > it->n - it->at)
        return DYN_PGW_E_PAST;
    *col = it->b + it->at;
    *clen = l;
    if (l != DYN_PGW_NULL)
        it->at += l;
    it->i++;
    return 1;
}

size_t dyn_pgw_cstr(const uint8_t* b, size_t n, size_t* at, const uint8_t** s)
{
    size_t i = *at, st = i;
    while (i < n && b[i])
        i++;
    *s = b + st;
    *at = i < n ? i + 1 : i;
    return i - st;
}

int dyn_pgw_field_next(const uint8_t* b, size_t n, size_t* at, char* code,
    const uint8_t** s, size_t* slen)
{
    size_t i = *at;
    if (i >= n || b[i] == 0)
        return 0;
    *code = (char)b[i++];
    *slen = dyn_pgw_cstr(b, n, &i, s);
    *at = i;
    return 1;
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

long dyn_pgw_bytea_hex(const uint8_t* p, size_t n, uint8_t* out)
{
    size_t i;
    if (n < 2 || p[0] != '\\' || p[1] != 'x' || (n & 1))
        return -1;
    for (i = 2; i < n; i++)
        if (hexv(p[i]) < 0)
            return -1;
    if (out)
        for (i = 0; i < (n - 2) / 2; i++)
            out[i] = (uint8_t)(hexv(p[2 + i * 2]) << 4 | hexv(p[3 + i * 2]));
    return (long)((n - 2) / 2);
}

void dyn_pgw_hex(const uint8_t* p, size_t n, char* out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    out[0] = '\\';
    out[1] = 'x';
    for (i = 0; i < n; i++) {
        out[2 + i * 2] = hex[p[i] >> 4];
        out[3 + i * 2] = hex[p[i] & 15];
    }
}

int dyn_pgw_int8_text(const uint8_t* p, size_t n, int64_t* v)
{
    uint64_t lim, acc = 0;
    size_t i = 0;
    int neg;
    if (n == 0 || n >= 48)
        return 0;
    neg = p[0] == '-';
    i = (size_t)neg;
    if (i == n)
        return 0;
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        unsigned d = (unsigned)p[i] - '0';
        if (d > 9 || acc > (lim - d) / 10)
            return 0;
        acc = acc * 10 + d;
    }
    *v = neg ? (int64_t)(0u - acc) : (int64_t)acc;
    return 1;
}

void dyn_pgw_uuid(const uint8_t* p, char* out)
{
    static const char hex[] = "0123456789abcdef";
    int i, k = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[k++] = '-';
        out[k++] = hex[p[i] >> 4];
        out[k++] = hex[p[i] & 15];
    }
}

int dyn_pgw_copy_response(const uint8_t* b, size_t n, uint8_t* fmt, uint16_t* ncols)
{
    size_t i, need;
    uint16_t k;
    if (n < 3)
        return DYN_PGW_E_TRUNC;
    if (b[0] > 1)
        return DYN_PGW_E_FORMAT;
    k = dyn_pgw_u16(b + 1);
    need = 3 + 2 * (size_t)k;
    if (n < need)
        return DYN_PGW_E_TRUNC;
    if (n > need)
        return DYN_PGW_E_PAST;
    for (i = 0; i < k; i++) {
        uint16_t f = dyn_pgw_u16(b + 3 + 2 * i);
        if (f > 1 || (b[0] == 0 && f != 0))
            return DYN_PGW_E_FORMAT;
    }
    if (fmt)
        *fmt = b[0];
    if (ncols)
        *ncols = k;
    return 0;
}

int dyn_pgw_negotiate_version(const uint8_t* b, size_t n, uint32_t* minor, uint32_t* nopt)
{
    uint32_t k, i;
    size_t at = 8;
    if (!b || n < 8)
        return -1;
    k = dyn_pgw_u32(b + 4);
    for (i = 0; i < k; i++) {
        const uint8_t* z = at < n ? (const uint8_t*)memchr(b + at, 0, n - at) : NULL;
        if (!z)
            return -1;
        at = (size_t)(z - b) + 1;
    }
    if (at != n)
        return -1;
    if (minor)
        *minor = dyn_pgw_u32(b);
    if (nopt)
        *nopt = k;
    return 0;
}

int dyn_pgw_backend_key(const uint8_t* b, size_t n, int minor, uint32_t* pid)
{
    if (!b || n < 8 || n - 4 > 256 || (minor < 2 && n != 8))
        return -1;
    if (pid)
        *pid = dyn_pgw_u32(b);
    return 0;
}

int dyn_pgw_numeric_bin(const uint8_t* p, size_t n, char* out, size_t cap, size_t* need)
{
    static const char* const special[3] = { "NaN", "Infinity", "-Infinity" };
    unsigned nd, sign, dscale, d, q[4];
    int weight, g, j, started = 0;
    size_t o = 0, k, want, i;
    if (!p || n < 8)
        return -1;
    nd = (unsigned)(p[0] << 8 | p[1]);
    weight = (int16_t)(p[2] << 8 | p[3]);
    sign = (unsigned)(p[4] << 8 | p[5]);
    dscale = (unsigned)(p[6] << 8 | p[7]);
    if (n != 8 + 2 * (size_t)nd || (dscale & 0xC000))
        return -1;
    if (sign == 0xC000 || sign == 0xD000 || sign == 0xF000) {
        const char* s = special[sign == 0xC000 ? 0 : sign == 0xD000 ? 1 : 2];
        if (nd)
            return -1;
        want = strlen(s) + 1;
        if (need)
            *need = want;
        if (!out || cap < want)
            return -2;
        memcpy(out, s, want);
        return (int)want - 1;
    }
    if (sign != 0 && sign != 0x4000)
        return -1;
    for (i = 0; i < nd; i++)
        if ((unsigned)(p[8 + 2 * i] << 8 | p[9 + 2 * i]) > 9999)
            return -1;
    want = 2 + (weight >= 0 ? 4 * ((size_t)weight + 1) : 1) + (dscale ? 1 + (size_t)dscale : 0) + 1;
    if (need)
        *need = want;
    if (!out || cap < want)
        return -2;
    if (sign == 0x4000)
        out[o++] = '-';
    for (g = 0; g <= weight; g++) {
        d = (unsigned)g < nd ? (unsigned)(p[8 + 2 * g] << 8 | p[9 + 2 * g]) : 0;
        q[0] = d / 1000;
        q[1] = d / 100 % 10;
        q[2] = d / 10 % 10;
        q[3] = d % 10;
        for (j = 0; j < 4; j++) {
            if (!started && q[j] == 0)
                continue;
            started = 1;
            out[o++] = (char)('0' + q[j]);
        }
    }
    if (!started)
        out[o++] = '0';
    if (dscale) {
        out[o++] = '.';
        for (g = weight + 1, k = 0; k < dscale; g++) {
            d = g >= 0 && (unsigned)g < nd ? (unsigned)(p[8 + 2 * g] << 8 | p[9 + 2 * g]) : 0;
            q[0] = d / 1000;
            q[1] = d / 100 % 10;
            q[2] = d / 10 % 10;
            q[3] = d % 10;
            for (j = 0; j < 4 && k < dscale; j++, k++)
                out[o++] = (char)('0' + q[j]);
        }
    }
    out[o] = 0;
    return (int)o;
}

int64_t dyn_pgw_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (int64_t)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (int64_t)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void dyn_pgw_civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int pgw_digits(const char* s, size_t n, size_t* at, size_t min, size_t max, int64_t* v)
{
    size_t k = 0;
    *v = 0;
    while (*at < n && s[*at] >= '0' && s[*at] <= '9' && k < max) {
        *v = *v * 10 + (s[*at] - '0');
        (*at)++;
        k++;
    }
    return k >= min && !(*at < n && s[*at] >= '0' && s[*at] <= '9');
}

static int pgw_lit(const char* s, size_t n, size_t* at, const char* lit)
{
    size_t l = strlen(lit);
    if (n - *at < l || memcmp(s + *at, lit, l))
        return 0;
    *at += l;
    return 1;
}

int dyn_pgw_ts_text(const char* s, size_t n, int kind, int64_t* us)
{
    static const unsigned mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    size_t at = 0;
    int64_t y, mo, dd, h = 0, mi = 0, se = 0, fr = 0, tz = 0, t, days, r;
    int bc = 0;
    if (!s || !us)
        return -1;
    if (n == 8 && !memcmp(s, "infinity", 8))
        return 1;
    if (n == 9 && !memcmp(s, "-infinity", 9))
        return 2;
    if (!pgw_digits(s, n, &at, 4, 9, &y) || !pgw_lit(s, n, &at, "-") || !pgw_digits(s, n, &at, 2, 2, &mo)
        || !pgw_lit(s, n, &at, "-") || !pgw_digits(s, n, &at, 2, 2, &dd))
        return -1;
    if (kind > 0) {
        if (!pgw_lit(s, n, &at, " ") || !pgw_digits(s, n, &at, 2, 2, &h) || !pgw_lit(s, n, &at, ":")
            || !pgw_digits(s, n, &at, 2, 2, &mi) || !pgw_lit(s, n, &at, ":") || !pgw_digits(s, n, &at, 2, 2, &se))
            return -1;
        if (pgw_lit(s, n, &at, ".")) {
            size_t b = at;
            if (!pgw_digits(s, n, &at, 1, 6, &fr))
                return -1;
            for (b = at - b; b < 6; b++)
                fr *= 10;
        }
        if (h > 23 || mi > 59 || se > 59)
            return -1;
    }
    if (kind == 2) {
        int64_t th = 0, tm = 0, ts = 0;
        int neg = at < n && s[at] == '-';
        if (!(at < n && (s[at] == '+' || s[at] == '-')))
            return -1;
        at++;
        if (!pgw_digits(s, n, &at, 2, 2, &th))
            return -1;
        if (pgw_lit(s, n, &at, ":")) {
            if (!pgw_digits(s, n, &at, 2, 2, &tm))
                return -1;
            if (pgw_lit(s, n, &at, ":") && !pgw_digits(s, n, &at, 2, 2, &ts))
                return -1;
        }
        if (th > 15 || tm > 59 || ts > 59)
            return -1;
        tz = (th * 3600 + tm * 60 + ts) * (neg ? -1 : 1);
    }
    if (pgw_lit(s, n, &at, " BC"))
        bc = 1;
    if (at != n || y < 1 || y > 294277 || mo < 1 || mo > 12 || dd < 1 || dd > mdays[mo - 1])
        return -1;
    if (bc)
        y = 1 - y;
    if (mo == 2 && dd == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return -1;
    days = dyn_pgw_days_from_civil(y, (unsigned)mo, (unsigned)dd) - DYN_PGW_EPOCH_DAYS;
    t = ((h * 60 + mi) * 60 + se - tz) * 1000000 + fr;
    if (__builtin_mul_overflow(days, (int64_t)86400000000, &r) || __builtin_add_overflow(r, t, &r))
        return -1;
    *us = r;
    return 0;
}

int dyn_pgw_ts_format(int64_t us, int kind, char* out, size_t cap)
{
    int64_t days = us / 86400000000LL, rem = us % 86400000000LL, y;
    unsigned m, d;
    int k;
    if (rem < 0) {
        rem += 86400000000LL;
        days--;
    }
    dyn_pgw_civil_from_days(days + DYN_PGW_EPOCH_DAYS, &y, &m, &d);
    k = snprintf(out, cap, "%04lld-%02u-%02u", (long long)(y > 0 ? y : 1 - y), m, d);
    if (k > 0 && kind > 0) {
        int64_t s = rem / 1000000, f = rem % 1000000;
        k += snprintf(out + k, cap > (size_t)k ? cap - (size_t)k : 0, " %02lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
        if (f) {
            char fb[8];
            int fl = 6;
            snprintf(fb, sizeof fb, "%06lld", (long long)f);
            while (fl > 1 && fb[fl - 1] == '0')
                fl--;
            k += snprintf(out + k, cap > (size_t)k ? cap - (size_t)k : 0, ".%.*s", fl, fb);
        }
        if (kind == 2)
            k += snprintf(out + k, cap > (size_t)k ? cap - (size_t)k : 0, "+00");
    }
    if (k > 0 && y <= 0)
        k += snprintf(out + k, cap > (size_t)k ? cap - (size_t)k : 0, " BC");
    return k > 0 && (size_t)k < cap ? k : -1;
}

int dyn_pgw_interval_text(const char* s, size_t n, int64_t* us, int32_t* days, int32_t* months)
{
    size_t at = 0;
    int64_t mon = 0, dy = 0, u = 0, v;
    int any = 0, timed = 0;
    if (!s || !us || !days || !months)
        return -1;
    while (at < n) {
        int neg = 0;
        size_t st;
        if (any && !pgw_lit(s, n, &at, " "))
            return -1;
        if (at < n && (s[at] == '+' || s[at] == '-'))
            neg = s[at++] == '-';
        st = at;
        if (!pgw_digits(s, n, &at, 1, 18, &v))
            return -1;
        if (at < n && s[at] == ':') {
            int64_t mi, se, fr = 0, t;
            if (timed || at - st < 2)
                return -1;
            at++;
            if (!pgw_digits(s, n, &at, 2, 2, &mi) || !pgw_lit(s, n, &at, ":") || !pgw_digits(s, n, &at, 2, 2, &se) || mi > 59 || se > 59)
                return -1;
            if (pgw_lit(s, n, &at, ".")) {
                size_t b = at;
                if (!pgw_digits(s, n, &at, 1, 6, &fr))
                    return -1;
                for (b = at - b; b < 6; b++)
                    fr *= 10;
            }
            if (v > 2562047788LL)
                return -1;
            t = ((v * 60 + mi) * 60 + se) * 1000000 + fr;
            if (__builtin_add_overflow(u, neg ? -t : t, &u))
                return -1;
            timed = 1;
        } else {
            int64_t mul;
            int64_t* dst;
            if (!pgw_lit(s, n, &at, " "))
                return -1;
            if (pgw_lit(s, n, &at, "years") || pgw_lit(s, n, &at, "year")) {
                mul = 12;
                dst = &mon;
            } else if (pgw_lit(s, n, &at, "mons") || pgw_lit(s, n, &at, "mon")) {
                mul = 1;
                dst = &mon;
            } else if (pgw_lit(s, n, &at, "days") || pgw_lit(s, n, &at, "day")) {
                mul = 1;
                dst = &dy;
            } else
                return -1;
            if (__builtin_mul_overflow(v, mul, &v) || __builtin_add_overflow(*dst, neg ? -v : v, dst))
                return -1;
        }
        any = 1;
    }
    if (!any || mon < INT32_MIN || mon > INT32_MAX || dy < INT32_MIN || dy > INT32_MAX)
        return -1;
    *us = u;
    *days = (int32_t)dy;
    *months = (int32_t)mon;
    return 0;
}

static int pgw_arr_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int pgw_arr_int(const char* s, size_t n, size_t* at, int64_t* v)
{
    int neg = 0, k = 0;
    *v = 0;
    if (*at < n && (s[*at] == '-' || s[*at] == '+'))
        neg = s[(*at)++] == '-';
    while (*at < n && s[*at] >= '0' && s[*at] <= '9' && k < 10) {
        *v = *v * 10 + (s[(*at)++] - '0');
        k++;
    }
    if (!k || (*at < n && s[*at] >= '0' && s[*at] <= '9'))
        return 0;
    if (neg)
        *v = -*v;
    return 1;
}

int dyn_pgw_array_text(const char* s, size_t n, int* ndim, int32_t dims[DYN_PGW_MAXDIM],
    dyn_pgw_span_t* sp, size_t cap, char* out, size_t* nel)
{
    int64_t bdim[DYN_PGW_MAXDIM], lo, hi;
    int32_t cnt[DYN_PGW_MAXDIM + 1];
    int nb = 0, depth = 0, nd = -1, d;
    size_t at = 0, o = 0, k = 0, total;
    if (!s || !ndim || !dims || !nel)
        return -1;
    for (d = 0; d < DYN_PGW_MAXDIM; d++)
        dims[d] = -1;
    while (at < n && pgw_arr_ws(s[at]))
        at++;
    while (at < n && s[at] == '[') {
        at++;
        if (nb == DYN_PGW_MAXDIM || !pgw_arr_int(s, n, &at, &hi))
            return -1;
        lo = 1;
        if (at < n && s[at] == ':') {
            at++;
            lo = hi;
            if (!pgw_arr_int(s, n, &at, &hi))
                return -1;
        }
        if (at >= n || s[at] != ']' || hi < lo || hi - lo + 1 > INT32_MAX)
            return -1;
        at++;
        bdim[nb++] = hi - lo + 1;
    }
    if (nb) {
        if (at >= n || s[at] != '=')
            return -1;
        at++;
    }
    while (at < n && pgw_arr_ws(s[at]))
        at++;
    if (at >= n || s[at] != '{')
        return -1;
    for (;;) {
        while (at < n && pgw_arr_ws(s[at]))
            at++;
        if (at >= n)
            return -1;
        if (s[at] == '{') {
            if ((nd >= 0 && depth >= nd) || depth >= DYN_PGW_MAXDIM)
                return -1;
            at++;
            cnt[++depth] = 0;
            continue;
        }
        if (s[at] == '}') {
            if (depth == 0)
                return -1;
            if (cnt[depth] == 0) {
                if (depth != 1 || nd >= 0)
                    return -1;
                nd = 0;
            } else if (dims[depth - 1] < 0)
                dims[depth - 1] = cnt[depth];
            else if (dims[depth - 1] != cnt[depth])
                return -1;
            at++;
            if (--depth == 0)
                break;
            cnt[depth]++;
        } else {
            size_t st = o, last = o;
            int quoted = s[at] == '"', esc = 0;
            if (depth == 0 || (nd >= 0 && depth != nd) || (nd < 0 && depth < 1))
                return -1;
            nd = depth;
            if (quoted) {
                at++;
                for (;;) {
                    if (at >= n)
                        return -1;
                    if (s[at] == '"')
                        break;
                    if (s[at] == '\\' && ++at >= n)
                        return -1;
                    if (out)
                        out[o] = s[at];
                    o++;
                    at++;
                }
                at++;
                last = o;
            } else {
                while (at < n && s[at] != ',' && s[at] != '}') {
                    if (s[at] == '{' || s[at] == '"')
                        return -1;
                    if (s[at] == '\\') {
                        if (++at >= n)
                            return -1;
                        esc = 1;
                        if (out)
                            out[o] = s[at];
                        last = ++o;
                        at++;
                        continue;
                    }
                    if (out)
                        out[o] = s[at];
                    o++;
                    if (!pgw_arr_ws(s[at]))
                        last = o;
                    at++;
                }
                o = last;
                if (o == st)
                    return -1;
            }
            if (sp && k < cap) {
                sp[k].off = (uint32_t)st;
                sp[k].len = (int32_t)(o - st);
                if (!quoted && !esc && o - st == 4 && out && (out[st] | 32) == 'n' && (out[st + 1] | 32) == 'u' && (out[st + 2] | 32) == 'l' && (out[st + 3] | 32) == 'l')
                    sp[k].len = -1;
            }
            k++;
            cnt[depth]++;
        }
        while (at < n && pgw_arr_ws(s[at]))
            at++;
        if (at < n && s[at] == ',') {
            at++;
            while (at < n && pgw_arr_ws(s[at]))
                at++;
            if (at < n && s[at] == '}')
                return -1;
            continue;
        }
        if (at >= n || s[at] != '}')
            return -1;
    }
    while (at < n && pgw_arr_ws(s[at]))
        at++;
    if (at != n)
        return -1;
    if (nd < 0)
        nd = 0;
    total = nd ? 1 : 0;
    for (d = 0; d < nd; d++)
        total *= (size_t)dims[d];
    if (total != k)
        return -1;
    if (nb && nb != nd)
        return -1;
    for (d = 0; d < nb; d++)
        if (bdim[d] != dims[d])
            return -1;
    *ndim = nd;
    *nel = k;
    if (sp && k > cap)
        return -2;
    return 0;
}

int dyn_pgw_array_bin(const uint8_t* p, size_t n, dyn_pgw_arrbin_t* h)
{
    size_t at, total = 1, k;
    int d;
    if (!p || !h || n < 12)
        return -1;
    memset(h, 0, sizeof(*h));
    h->ndim = (int32_t)dyn_pgw_u32(p);
    h->hasnull = (int32_t)dyn_pgw_u32(p + 4);
    h->elem = dyn_pgw_u32(p + 8);
    if (h->ndim < 0 || h->ndim > DYN_PGW_MAXDIM || (h->hasnull != 0 && h->hasnull != 1) || n < 12 + 8 * (size_t)h->ndim)
        return -1;
    for (d = 0; d < h->ndim; d++) {
        int32_t dim = (int32_t)dyn_pgw_u32(p + 12 + 8 * d);
        h->lb[d] = (int32_t)dyn_pgw_u32(p + 16 + 8 * d);
        if (dim < 0)
            return -1;
        h->dims[d] = dim;
        if (__builtin_mul_overflow(total, (size_t)dim, &total))
            return -1;
    }
    if (h->ndim == 0)
        total = 0;
    at = 12 + 8 * (size_t)h->ndim;
    if (total > (n - at) / 4)
        return -1;
    h->data = p + at;
    h->dlen = n - at;
    for (k = 0; k < total; k++) {
        int32_t len;
        if (n - at < 4)
            return -1;
        len = (int32_t)dyn_pgw_u32(p + at);
        at += 4;
        if (len < -1 || (len >= 0 && (size_t)len > n - at))
            return -1;
        if (len > 0)
            at += (size_t)len;
    }
    if (at != n)
        return -1;
    h->nel = total;
    return 0;
}

int dyn_pgw_array_bin_next(dyn_pgw_arrbin_t* h, size_t* at, const uint8_t** e, int32_t* len)
{
    if (*at + 4 > h->dlen)
        return 0;
    *len = (int32_t)dyn_pgw_u32(h->data + *at);
    *at += 4;
    *e = h->data + *at;
    if (*len > 0)
        *at += (size_t)*len;
    return 1;
}

uint32_t dyn_pgw_array_elem(uint32_t oid)
{
    switch (oid) {
    case 1000: return 16;
    case 1001: return 17;
    case 1005: return 21;
    case 1007: return 23;
    case 1009: return 25;
    case 1014: return 1042;
    case 1015: return 1043;
    case 1016: return 20;
    case 1021: return 700;
    case 1022: return 701;
    case 1028: return 26;
    case 1182: return 1082;
    case 1115: return 1114;
    case 1185: return 1184;
    case 1187: return 1186;
    case 1231: return 1700;
    case 2951: return 2950;
    case 199: return 114;
    case 3807: return 3802;
    case 3905: return 3904;
    case 3907: return 3906;
    case 3909: return 3908;
    case 3911: return 3910;
    case 3913: return 3912;
    case 3927: return 3926;
    default: return 0;
    }
}

static int pgw_rng_bound(const char* s, size_t n, size_t* at, char* out, size_t* o, int* inf)
{
    int inq = 0;
    if (*at < n && (s[*at] == ',' || s[*at] == ')' || s[*at] == ']')) {
        *inf = 1;
        return 0;
    }
    *inf = 0;
    while (inq || !(*at < n && (s[*at] == ',' || s[*at] == ')' || s[*at] == ']'))) {
        char ch;
        if (*at >= n)
            return -1;
        ch = s[(*at)++];
        if (ch == '\\') {
            if (*at >= n)
                return -1;
            out[(*o)++] = s[(*at)++];
        } else if (ch == '"') {
            if (!inq)
                inq = 1;
            else if (*at < n && s[*at] == '"') {
                out[(*o)++] = '"';
                (*at)++;
            } else
                inq = 0;
        } else
            out[(*o)++] = ch;
    }
    return 0;
}

int dyn_pgw_range_text(const char* s, size_t n, dyn_pgw_range_t* r, char* out)
{
    size_t at = 0, o = 0, lo;
    int inf;
    if (!s || !r || !out)
        return -1;
    memset(r, 0, sizeof(*r));
    while (at < n && pgw_arr_ws(s[at]))
        at++;
    if (n - at >= 5 && (s[at] | 32) == 'e' && (s[at + 1] | 32) == 'm' && (s[at + 2] | 32) == 'p' && (s[at + 3] | 32) == 't' && (s[at + 4] | 32) == 'y') {
        at += 5;
        while (at < n && pgw_arr_ws(s[at]))
            at++;
        if (at != n)
            return -1;
        r->flags = DYN_PGW_RANGE_EMPTY;
        return 0;
    }
    if (at >= n || (s[at] != '[' && s[at] != '('))
        return -1;
    if (s[at++] == '[')
        r->flags |= DYN_PGW_RANGE_LB_INC;
    if (pgw_rng_bound(s, n, &at, out, &o, &inf) < 0)
        return -1;
    if (inf)
        r->flags = DYN_PGW_RANGE_LB_INF;
    r->lo = (const uint8_t*)out;
    r->lo_len = inf ? -1 : (int32_t)o;
    lo = o;
    if (at >= n || s[at++] != ',')
        return -1;
    if (pgw_rng_bound(s, n, &at, out, &o, &inf) < 0)
        return -1;
    r->hi = (const uint8_t*)out + lo;
    r->hi_len = inf ? -1 : (int32_t)(o - lo);
    if (at >= n || (s[at] != ']' && s[at] != ')'))
        return -1;
    if (inf)
        r->flags |= DYN_PGW_RANGE_UB_INF;
    else if (s[at] == ']')
        r->flags |= DYN_PGW_RANGE_UB_INC;
    at++;
    while (at < n && pgw_arr_ws(s[at]))
        at++;
    return at == n ? 0 : -1;
}

int dyn_pgw_range_bin(const uint8_t* p, size_t n, dyn_pgw_range_t* r)
{
    size_t at = 1;
    int f, side;
    if (!p || !r || n < 1)
        return -1;
    memset(r, 0, sizeof(*r));
    f = p[0];
    r->lo_len = r->hi_len = -1;
    if (f & 0xE0)
        return -1;
    if (f & DYN_PGW_RANGE_EMPTY) {
        r->flags = f;
        return f == DYN_PGW_RANGE_EMPTY && n == 1 ? 0 : -1;
    }
    if (((f & DYN_PGW_RANGE_LB_INF) && (f & DYN_PGW_RANGE_LB_INC)) || ((f & DYN_PGW_RANGE_UB_INF) && (f & DYN_PGW_RANGE_UB_INC)))
        return -1;
    for (side = 0; side < 2; side++) {
        int32_t len;
        if (f & (side ? DYN_PGW_RANGE_UB_INF : DYN_PGW_RANGE_LB_INF))
            continue;
        if (n - at < 4)
            return -1;
        len = (int32_t)dyn_pgw_u32(p + at);
        at += 4;
        if (len < 0 || (size_t)len > n - at)
            return -1;
        if (side) {
            r->hi = p + at;
            r->hi_len = len;
        } else {
            r->lo = p + at;
            r->lo_len = len;
        }
        at += (size_t)len;
    }
    r->flags = f;
    return at == n ? 0 : -1;
}

int dyn_pgw_multirange_text(const char* s, size_t n, size_t* at, dyn_pgw_range_t* r, char* out)
{
    size_t st;
    int inq = 0;
    if (!s || !at || !r || !out)
        return -1;
    if (*at == 0) {
        while (*at < n && pgw_arr_ws(s[*at]))
            (*at)++;
        if (*at >= n || s[*at] != '{')
            return -1;
        (*at)++;
        while (*at < n && pgw_arr_ws(s[*at]))
            (*at)++;
        if (*at < n && s[*at] == '}')
            goto close;
    } else {
        while (*at < n && pgw_arr_ws(s[*at]))
            (*at)++;
        if (*at < n && s[*at] == '}')
            goto close;
        if (*at >= n || s[*at] != ',')
            return -1;
        (*at)++;
        while (*at < n && pgw_arr_ws(s[*at]))
            (*at)++;
    }
    st = *at;
    if (n - *at >= 5 && (s[*at] | 32) == 'e' && (s[*at + 1] | 32) == 'm' && (s[*at + 2] | 32) == 'p' && (s[*at + 3] | 32) == 't' && (s[*at + 4] | 32) == 'y')
        *at += 5;
    else {
        if (*at >= n || (s[*at] != '[' && s[*at] != '('))
            return -1;
        (*at)++;
        for (;;) {
            char ch;
            if (*at >= n)
                return -1;
            ch = s[(*at)++];
            if (ch == '\\') {
                if (*at >= n)
                    return -1;
                (*at)++;
            } else if (ch == '"')
                inq = !inq;
            else if (!inq && (ch == ']' || ch == ')'))
                break;
        }
    }
    return dyn_pgw_range_text(s + st, *at - st, r, out) == 0 ? 1 : -1;
close:
    (*at)++;
    while (*at < n && pgw_arr_ws(s[*at]))
        (*at)++;
    if (*at != n)
        return -1;
    *at = n + 1;
    return 0;
}

int dyn_pgw_multirange_bin(const uint8_t* p, size_t n, size_t* at, dyn_pgw_range_t* r)
{
    uint32_t len;
    if (!p || !at || !r)
        return -1;
    if (*at == 0) {
        size_t k = 4;
        uint32_t c, i;
        if (n < 4)
            return -1;
        c = dyn_pgw_u32(p);
        if (c > (n - 4) / 5)
            return -1;
        for (i = 0; i < c; i++) {
            if (n - k < 4)
                return -1;
            len = dyn_pgw_u32(p + k);
            k += 4;
            if (len < 1 || len > n - k || dyn_pgw_range_bin(p + k, len, r) < 0)
                return -1;
            k += len;
        }
        if (k != n)
            return -1;
        *at = 4;
    }
    if (*at >= n)
        return 0;
    len = dyn_pgw_u32(p + *at);
    *at += 4;
    dyn_pgw_range_bin(p + *at, len, r);
    *at += len;
    return 1;
}

uint32_t dyn_pgw_range_sub(uint32_t oid)
{
    switch (oid) {
    case 3904: return 23;
    case 3906: return 1700;
    case 3908: return 1114;
    case 3910: return 1184;
    case 3912: return 1082;
    case 3926: return 20;
    default: return 0;
    }
}

uint32_t dyn_pgw_multirange_range(uint32_t oid)
{
    switch (oid) {
    case 4451: return 3904;
    case 4532: return 3906;
    case 4533: return 3908;
    case 4534: return 3910;
    case 4535: return 3912;
    case 4536: return 3926;
    default: return 0;
    }
}

static size_t pgw_uri_prefix(const char* s, size_t n)
{
    if (n >= 13 && !memcmp(s, "postgresql://", 13))
        return 13;
    if (n >= 11 && !memcmp(s, "postgres://", 11))
        return 11;
    return 0;
}

static int pgw_uri_stop(char c, int port)
{
    return c == '/' || c == '?' || c == ',' || (!port && c == ':');
}

static int pgw_uri_entry(const char* s, size_t end, size_t* at, size_t* h, size_t* hn, size_t* p, size_t* pn, int* has_port)
{
    size_t i = *at;
    *p = *pn = 0;
    *has_port = 0;
    if (i < end && s[i] == '[') {
        *h = ++i;
        while (i < end && s[i] != ']')
            i++;
        if (i >= end)
            return DYN_PGW_URI_BRACKET;
        if (i == *h)
            return DYN_PGW_URI_EMPTY6;
        *hn = i - *h;
        i++;
        if (i < end && !pgw_uri_stop(s[i], 0))
            return DYN_PGW_URI_AFTER6;
    } else {
        *h = i;
        while (i < end && !pgw_uri_stop(s[i], 0))
            i++;
        *hn = i - *h;
    }
    if (i < end && s[i] == ':') {
        *has_port = 1;
        *p = ++i;
        while (i < end && !pgw_uri_stop(s[i], 1))
            i++;
        *pn = i - *p;
    }
    *at = i;
    return 0;
}

int dyn_pgw_uri_split(const char* s, size_t n, dyn_pgw_uri_t* u)
{
    const char* z = (const char*)memchr(s, 0, n);
    size_t st, i, h, hn, p, pn;
    int hp, rc;
    if (z)
        n = (size_t)(z - s);
    memset(u, 0, sizeof(*u));
    st = pgw_uri_prefix(s, n);
    if (!st)
        return DYN_PGW_URI_SCHEME;
    i = st;
    while (i < n && s[i] != '@' && s[i] != '/')
        i++;
    if (i < n && s[i] == '@') {
        i = st;
        while (s[i] != ':' && s[i] != '@')
            i++;
        u->user = st;
        u->user_n = i - st;
        u->has_user = u->user_n > 0;
        if (s[i] == ':') {
            u->pass = ++i;
            while (s[i] != '@')
                i++;
            u->pass_n = i - u->pass;
            u->has_pass = u->pass_n > 0;
        }
        i++;
    } else {
        i = st;
    }
    u->net = i;
    for (;;) {
        rc = pgw_uri_entry(s, n, &i, &h, &hn, &p, &pn, &hp);
        if (rc)
            return rc;
        if (i >= n || s[i] != ',')
            break;
        i++;
    }
    u->net_end = i;
    if (i < n && s[i] == '/') {
        u->db = ++i;
        while (i < n && s[i] != '?')
            i++;
        u->db_n = i - u->db;
        u->has_db = u->db_n > 0;
    }
    if (i < n) {
        u->q = i + 1;
        u->q_n = n - u->q;
        u->has_q = 1;
    }
    return 0;
}

int dyn_pgw_uri_host(const char* s, size_t end, size_t* at, size_t* h, size_t* hn, size_t* p, size_t* pn, int* has_port)
{
    if (*at > end)
        return 0;
    if (pgw_uri_entry(s, end, at, h, hn, p, pn, has_port))
        return -1;
    if (*at < end && s[*at] == ',')
        (*at)++;
    else
        *at = end + 1;
    return 1;
}

int dyn_pgw_uri_param(const char* s, size_t end, size_t* at, size_t* k, size_t* kn, size_t* v, size_t* vn)
{
    size_t i = *at;
    int eq = 0;
    if (i >= end)
        return 0;
    *k = i;
    *kn = *v = *vn = 0;
    for (; i < end && s[i] != '&'; i++)
        if (s[i] == '=') {
            if (eq)
                return -2;
            eq = 1;
            *kn = i - *k;
            *v = i + 1;
        }
    if (!eq)
        return -1;
    *vn = i - *v;
    *at = i < end ? i + 1 : i;
    return 1;
}

static int pgw_uri_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

long dyn_pgw_uri_decode(const char* s, size_t n, char* out)
{
    size_t i = 0, o = 0;
    int hi, lo;
    while (i < n && s[i] == ' ')
        i++;
    while (i < n && s[i] != ' ') {
        if (s[i] != '%') {
            out[o++] = s[i++];
            continue;
        }
        hi = i + 1 < n ? pgw_uri_hex(s[i + 1]) : -1;
        lo = hi >= 0 && i + 2 < n ? pgw_uri_hex(s[i + 2]) : -1;
        if (hi < 0 || lo < 0)
            return DYN_PGW_DEC_HEX;
        if (!(hi | lo))
            return DYN_PGW_DEC_NUL;
        out[o++] = (char)(hi << 4 | lo);
        i += 3;
    }
    while (i < n && s[i] == ' ')
        i++;
    return i < n ? DYN_PGW_DEC_SPACE : (long)o;
}

static const char* pgw_pp_field(const char* t, const char* e, const char* tok)
{
    int bs = 0;
    if (e - t >= 2 && t[0] == '*' && t[1] == ':')
        return t + 2;
    while (t < e) {
        if (*t == '\\' && !bs) {
            t++;
            bs = 1;
        }
        if (t < e && *t == ':' && !*tok && !bs)
            return t + 1;
        bs = 0;
        if (!*tok)
            return NULL;
        if (t < e && *t == *tok) {
            t++;
            tok++;
        } else {
            return NULL;
        }
    }
    return NULL;
}

long dyn_pgw_pgpass_find(const char* f, size_t n, const char* host, const char* port, const char* db, const char* user, char* out)
{
    size_t i = 0;
    while (i < n) {
        size_t ls = i, le, o = 0;
        const char *t, *e, *z;
        while (i < n && f[i] != '\n')
            i++;
        le = i;
        if (i < n)
            i++;
        z = (const char*)memchr(f + ls, 0, le - ls);
        if (z)
            le = (size_t)(z - f);
        if (le == ls || f[ls] == '#')
            continue;
        while (le > ls && f[le - 1] == '\r')
            le--;
        if (le == ls)
            continue;
        e = f + le;
        if (!(t = pgw_pp_field(f + ls, e, host)) || !(t = pgw_pp_field(t, e, port)) || !(t = pgw_pp_field(t, e, db))
            || !(t = pgw_pp_field(t, e, user)))
            continue;
        while (t < e && *t != ':') {
            if (*t == '\\' && t + 1 < e)
                t++;
            out[o++] = *t++;
        }
        return (long)o;
    }
    return -1;
}
