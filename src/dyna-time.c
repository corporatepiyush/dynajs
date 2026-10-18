#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_time_add_ck(JSContext* ctx, int64_t a, int64_t b,
    int64_t* out, const char* what)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        JS_ThrowRangeError(ctx,
            "dyna:time: %s overflows the supported range", what);
        return -1;
    }
    *out = a + b;
    return 0;
}

#define DYN_NS_PER_US 1000LL
#define DYN_NS_PER_MS 1000000LL
#define DYN_NS_PER_SEC 1000000000LL
#define DYN_NS_PER_MIN 60000000000LL
#define DYN_NS_PER_HOUR 3600000000000LL
#define DYN_SECS_PER_DAY 86400LL
#define DYN_MAX_SAFE_INT 9007199254740991LL
#define DYN_U64_2_POW_63 (((uint64_t)1) << 63)

#define DYN_TIME_MIN_YEAR (-271821)
#define DYN_TIME_MAX_YEAR 275760

static int64_t dyn_time_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t dyn_time_floor_mod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

static int64_t dyn_days_from_civil(int64_t y, int m, int64_t d)
{
    int64_t era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void dyn_civil_from_days(int64_t z, int64_t* y, int* m, int* d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y += (*m <= 2);
}

static int dyn_weekday_from_days(int64_t z)
{
    return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

static int dyn_time_days_in_month(int64_t y, int m)
{
    int64_t y2 = y;
    int m2 = m + 1;
    if (m2 > 12) {
        m2 = 1;
        y2++;
    }
    return (int)(dyn_days_from_civil(y2, m2, 1) - dyn_days_from_civil(y, m, 1));
}

static void dyn_time_norm_month(int64_t* y, int64_t* mo)
{
    int64_t m0 = *mo - 1;
    int64_t yshift = dyn_time_floor_div(m0, 12);
    *y += yshift;
    *mo = dyn_time_floor_mod(m0, 12) + 1;
}

static time_t dyn_time_to_time_t(int64_t sec)
{
    if (sizeof(time_t) == 4) {
        if (sec < INT32_MIN)
            sec = INT32_MIN;
        else if (sec > INT32_MAX)
            sec = INT32_MAX;
    }
    return (time_t)sec;
}

static int dyn_time_utoa(uint64_t v, char* out)
{
    char tmp[20];
    int n = 0, i;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

static int dyn_time_utoa_pad(uint64_t v, int width, char* out)
{
    char tmp[20];
    int n = dyn_time_utoa(v, tmp);
    int i;
    if (n >= width) {
        memcpy(out, tmp, (size_t)n);
        return n;
    }
    for (i = 0; i < width - n; i++)
        out[i] = '0';
    memcpy(out + (width - n), tmp, (size_t)n);
    return width;
}

static int dyn_time_fmt_frac(uint64_t frac, int width, char* out)
{
    char digits[16];
    int n = dyn_time_utoa_pad(frac, width, digits);
    while (n > 0 && digits[n - 1] == '0')
        n--;
    if (n == 0)
        return 0;
    out[0] = '.';
    memcpy(out + 1, digits, (size_t)n);
    return n + 1;
}

typedef struct DynTimeBuf {
    JSContext* ctx;
    uint8_t* data;
    size_t len, cap;
} DynTimeBuf;

static int dyn_time_buf_init(JSContext* ctx, DynTimeBuf* b, size_t hint)
{
    b->ctx = ctx;
    b->len = 0;
    b->cap = hint > 0 ? hint : 16;
    b->data = js_malloc(ctx, b->cap);
    if (!b->data) {
        b->cap = 0;
        return -1;
    }
    return 0;
}

static int dyn_time_buf_reserve(DynTimeBuf* b, size_t extra)
{
    size_t need = b->len + extra;
    size_t ncap;
    uint8_t* nd;
    if (need <= b->cap)
        return 0;
    ncap = b->cap * 2;
    if (ncap < need)
        ncap = need;
    nd = js_realloc(b->ctx, b->data, ncap);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static int dyn_time_buf_put(DynTimeBuf* b, const void* src, size_t n)
{
    if (dyn_time_buf_reserve(b, n))
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void dyn_time_buf_free(DynTimeBuf* b)
{
    js_free(b->ctx, b->data);
    b->data = NULL;
}

static JSValue dyn_time_ns_to_jsvalue(JSContext* ctx, int64_t ns)
{
    if (ns >= -DYN_MAX_SAFE_INT && ns <= DYN_MAX_SAFE_INT)
        return JS_NewInt64(ctx, ns);
    return JS_NewBigInt64(ctx, ns);
}

#define DYN_TIME_DUR_BUF 64

static JSValue dyn_time_duration_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;
    uint64_t u;
    int neg;
    char buf[DYN_TIME_DUR_BUF];
    int pos = 0;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &ns, argv[0]))
        return JS_EXCEPTION;

    neg = ns < 0;
    u = (uint64_t)ns;
    if (neg)
        u = -u;

    if (u == 0)
        return JS_NewStringLen(ctx, "0s", 2);

    if (neg)
        buf[pos++] = '-';

    if (u < (uint64_t)DYN_NS_PER_SEC) {
        uint64_t ip, divisor;
        int width, ulen;
        const char* unit;

        if (u < (uint64_t)DYN_NS_PER_US) {
            ip = u;
            divisor = 1;
            width = 0;
            unit = "ns";
            ulen = 2;
        } else if (u < (uint64_t)DYN_NS_PER_MS) {
            divisor = (uint64_t)DYN_NS_PER_US;
            ip = u / divisor;
            width = 3;
            unit = "\xc2\xb5s";
            ulen = 3;
        } else {
            divisor = (uint64_t)DYN_NS_PER_MS;
            ip = u / divisor;
            width = 6;
            unit = "ms";
            ulen = 2;
        }
        pos += dyn_time_utoa(ip, buf + pos);
        if (width > 0)
            pos += dyn_time_fmt_frac(u % divisor, width, buf + pos);
        memcpy(buf + pos, unit, (size_t)ulen);
        pos += ulen;
    } else {
        uint64_t total_sec = u / (uint64_t)DYN_NS_PER_SEC;
        uint64_t frac_ns = u % (uint64_t)DYN_NS_PER_SEC;
        uint64_t secs = total_sec % 60ULL;
        uint64_t total_min = total_sec / 60ULL;

        if (total_min > 0) {
            uint64_t mins = total_min % 60ULL;
            uint64_t total_hr = total_min / 60ULL;
            if (total_hr > 0) {
                pos += dyn_time_utoa(total_hr, buf + pos);
                buf[pos++] = 'h';
            }
            pos += dyn_time_utoa(mins, buf + pos);
            buf[pos++] = 'm';
        }
        pos += dyn_time_utoa(secs, buf + pos);
        pos += dyn_time_fmt_frac(frac_ns, 9, buf + pos);
        buf[pos++] = 's';
    }
    return JS_NewStringLen(ctx, buf, pos);
}

struct DynTimeUnit {
    const char* name;
    int len;
    int64_t ns;
};

static const struct DynTimeUnit dyn_time_units[] = {
    { "ns", 2, 1LL },
    { "us", 2, DYN_NS_PER_US },
    { "\xc2\xb5s", 3, DYN_NS_PER_US },
    { "\xce\xbcs", 3, DYN_NS_PER_US },
    { "ms", 2, DYN_NS_PER_MS },
    { "s", 1, DYN_NS_PER_SEC },
    { "m", 1, DYN_NS_PER_MIN },
    { "h", 1, DYN_NS_PER_HOUR },
};

static int dyn_time_lookup_unit(const char* s, size_t ulen, int64_t* out_ns)
{
    size_t i;
    for (i = 0; i < countof(dyn_time_units); i++) {
        if ((size_t)dyn_time_units[i].len == ulen && memcmp(dyn_time_units[i].name, s, ulen) == 0) {
            *out_ns = dyn_time_units[i].ns;
            return 0;
        }
    }
    return -1;
}

static void dyn_time_leading_fraction(const char* s, size_t len, size_t* pi,
    uint64_t* pf, double* pscale)
{
    size_t i = *pi;
    uint64_t x = 0;
    double scale = 1.0;
    int overflow = 0;

    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        int digit = s[i] - '0';
        if (!overflow) {
            if (x > (DYN_U64_2_POW_63 - 1) / 10) {
                overflow = 1;
            } else {
                uint64_t y = x * 10 + (uint64_t)digit;
                if (y > DYN_U64_2_POW_63)
                    overflow = 1;
                else {
                    x = y;
                    scale *= 10.0;
                }
            }
        }
    }
    *pi = i;
    *pf = x;
    *pscale = scale;
}

static int dyn_time_parse_duration_ns(JSContext* ctx, JSValueConst argv0,
    int64_t* out)
{
    const char* orig;
    size_t len, i = 0;
    int neg = 0;
    uint64_t d = 0;
    int rc = -1;

    orig = JS_ToCStringLen(ctx, &len, argv0);
    if (!orig)
        return -1;

    if (len > 0 && (orig[0] == '-' || orig[0] == '+')) {
        neg = (orig[0] == '-');
        i = 1;
    }

    if (len - i == 1 && orig[i] == '0') {
        *out = 0;
        rc = 0;
        goto done;
    }
    if (i >= len)
        goto bad;

    while (i < len) {
        uint64_t v = 0, f = 0;
        double scale = 1.0;
        int have_int, have_frac = 0;
        size_t start, unit_start;
        int64_t unit_ns;

        if (!(orig[i] == '.' || (orig[i] >= '0' && orig[i] <= '9')))
            goto bad;

        start = i;
        for (; i < len && orig[i] >= '0' && orig[i] <= '9'; i++) {
            int digit = orig[i] - '0';
            if (v > DYN_U64_2_POW_63 / 10)
                goto bad;
            v = v * 10 + (uint64_t)digit;
            if (v > DYN_U64_2_POW_63)
                goto bad;
        }
        have_int = (i != start);

        if (i < len && orig[i] == '.') {
            size_t before;
            i++;
            before = i;
            dyn_time_leading_fraction(orig, len, &i, &f, &scale);
            have_frac = (i != before);
        }
        if (!have_int && !have_frac)
            goto bad;

        unit_start = i;
        while (i < len && orig[i] != '.' && !(orig[i] >= '0' && orig[i] <= '9'))
            i++;
        if (i == unit_start)
            goto bad;
        if (dyn_time_lookup_unit(orig + unit_start, i - unit_start, &unit_ns))
            goto bad;

        if (v > DYN_U64_2_POW_63 / (uint64_t)unit_ns)
            goto bad;
        v *= (uint64_t)unit_ns;

        if (f > 0) {
            double add = (double)f * ((double)unit_ns / scale);
            v += (uint64_t)add;
            if (v > DYN_U64_2_POW_63)
                goto bad;
        }

        d += v;
        if (d > DYN_U64_2_POW_63)
            goto bad;
    }

    if (neg) {
        *out = (d == DYN_U64_2_POW_63) ? INT64_MIN : -(int64_t)d;
    } else {
        if (d > (uint64_t)INT64_MAX)
            goto bad;
        *out = (int64_t)d;
    }
    rc = 0;
    goto done;

bad:
    JS_ThrowSyntaxError(ctx, "dyna:time: invalid duration");

done:
    JS_FreeCString(ctx, orig);
    return rc;
}

static int dyn_time_u64_bits(uint64_t v)
{
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

#if defined(__SIZEOF_INT128__)
static double dyn_time_scale_ns(int64_t ns, uint64_t div)
{
    int neg = ns < 0;
    uint64_t n = neg ? (uint64_t)(-(ns + 1)) + 1 : (uint64_t)ns;
    unsigned __int128 num;
    uint64_t q, r, t, half, frac, lhs, rhs;
    int k, sh;

    if (!n)
        return 0.0;
    k = 55 - (dyn_time_u64_bits(n) - dyn_time_u64_bits(div));
    num = ((unsigned __int128)n) << k;
    q = (uint64_t)(num / div);
    r = (uint64_t)(num % div);
    sh = dyn_time_u64_bits(q) - 53;
    t = q >> sh;
    half = (uint64_t)1 << (sh - 1);
    frac = q & ((half << 1) - 1);
    lhs = frac * div + r;
    rhs = half * div;
    if (lhs > rhs || (lhs == rhs && (t & 1)))
        t++;
    if (t == ((uint64_t)1 << 53)) {
        t >>= 1;
        sh++;
    }
    {
        double out = ldexp((double)t, sh - k);
        return neg ? -out : out;
    }
}
#else
static double dyn_time_scale_ns(int64_t ns, uint64_t div)
{
    return (double)ns / (double)div;
}
#endif

static JSValue dyn_time_parse_duration(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return dyn_time_ns_to_jsvalue(ctx, ns);
}

static JSValue dyn_time_parse_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_time_scale_ns(ns, 1000000));
}

static JSValue dyn_time_parse_duration_secs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_time_scale_ns(ns, 1000000000));
}

static JSValue dyn_time_now(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    JSValue obj;

    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "sec",
            JS_NewInt64(ctx, (int64_t)ts.tv_sec),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "nsec",
               JS_NewInt32(ctx, (int32_t)ts.tv_nsec),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue dyn_time_now_unix_nano(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC + (int64_t)ts.tv_nsec);
}

static JSValue dyn_time_now_millis(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewInt64(ctx, (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000);
}

static JSValue dyn_time_now_sec(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewInt64(ctx, (int64_t)ts.tv_sec);
}

static JSValue dyn_time_monotonic_nano(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC + (int64_t)ts.tv_nsec);
}

static const char* const dyn_time_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char* const dyn_time_weekday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

#define DYN_TIME_RFC_BUF 96

static int dyn_time_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys);

static const char* const dyn_rfc3339_keys[] = { "nsec", "offsetMinutes" };

static int dyn_time_read_int(JSContext* ctx, JSValueConst v, const char* name,
    int64_t* out)
{
    double d;
    int64_t m;

    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:time: %s must be a number", name);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    if (d != d || d - d != 0.0) {
        JS_ThrowRangeError(ctx, "dyna:time: %s must be an integer", name);
        return -1;
    }
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        JS_ThrowRangeError(ctx, "dyna:time: %s is out of range", name);
        return -1;
    }
    m = (int64_t)d;
    if ((double)m != d) {
        JS_ThrowRangeError(ctx, "dyna:time: %s must be an integer", name);
        return -1;
    }
    *out = m;
    return 0;
}

static int dyn_time_read_nsec(JSContext* ctx, JSValueConst v, int64_t* pns)
{
    return dyn_time_read_int(ctx, v, "nsec", pns);
}

static int dyn_time_read_offset(JSContext* ctx, JSValueConst opts,
    int64_t* poff)
{
    JSValue v;
    int64_t m;

    *poff = 0;
    v = JS_GetPropertyStr(ctx, opts, "offsetMinutes");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (dyn_time_read_int(ctx, v, "offsetMinutes", &m)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (m < -1439 || m > 1439) {
        JS_ThrowRangeError(ctx, "dyna:time: offsetMinutes must be in "
                                "[-1439, 1439] (RFC 3339 offsets span at most "
                                "+/-23:59)");
        return -1;
    }
    *poff = m;
    return 0;
}

static JSValue dyn_time_format_rfc3339(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t sec, nsec = 0, y, days, tod, off = 0;
    int mo, d, h, mi, s, utc = 1;
    int64_t local;
    char buf[DYN_TIME_RFC_BUF];
    int pos = 0;

    (void)this_val;
    if (dyn_time_read_int(ctx, argv[0], "sec", &sec))
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        int64_t off_min;
        JSValue nv;
        if (argc > 2)
            return JS_ThrowTypeError(ctx, "formatRFC3339(sec, opts): the bag "
                                          "form takes exactly 2 arguments (the (nsec, utc) tail is the "
                                          "legacy form)");
        if (dyn_time_opts_strict(ctx, argv[1], dyn_rfc3339_keys, 2))
            return JS_EXCEPTION;
        if (dyn_time_read_offset(ctx, argv[1], &off_min))
            return JS_EXCEPTION;
        off = off_min * 60;
        nv = JS_GetPropertyStr(ctx, argv[1], "nsec");
        if (JS_IsException(nv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(nv)) {
            int bad = dyn_time_read_nsec(ctx, nv, &nsec);
            JS_FreeValue(ctx, nv);
            if (bad)
                return JS_EXCEPTION;
        } else {
            JS_FreeValue(ctx, nv);
        }
        utc = 0;
    } else {
        if (argc > 1 && !JS_IsUndefined(argv[1]) && dyn_time_read_nsec(ctx, argv[1], &nsec))
            return JS_EXCEPTION;
        if (argc > 2 && !JS_IsUndefined(argv[2])) {
            int b;
            if (!JS_IsBool(argv[2]))
                return JS_ThrowTypeError(ctx, "dyna:time: utc must be a boolean");
            b = JS_ToBool(ctx, argv[2]);
            if (b < 0)
                return JS_EXCEPTION;
            utc = b;
        }
        if (!utc) {
            time_t ti = dyn_time_to_time_t(sec);
            struct tm tmv;
            off = localtime_r(&ti, &tmv) ? tmv.tm_gmtoff : 0;
        }
    }
    if (nsec < 0 || nsec > 999999999)
        return JS_ThrowRangeError(ctx, "dyna:time: nsec must be in [0, 999999999]");

    if (dyn_time_add_ck(ctx, sec, off, &local, "the timestamp"))
        return JS_EXCEPTION;
    days = dyn_time_floor_div(local, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(local, DYN_SECS_PER_DAY);
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);
    dyn_civil_from_days(days, &y, &mo, &d);

    if (y < 0) {
        buf[pos++] = '-';
        y = -y;
    }
    pos += dyn_time_utoa_pad((uint64_t)y, 4, buf + pos);
    buf[pos++] = '-';
    pos += dyn_time_utoa_pad((uint64_t)mo, 2, buf + pos);
    buf[pos++] = '-';
    pos += dyn_time_utoa_pad((uint64_t)d, 2, buf + pos);
    buf[pos++] = 'T';
    pos += dyn_time_utoa_pad((uint64_t)h, 2, buf + pos);
    buf[pos++] = ':';
    pos += dyn_time_utoa_pad((uint64_t)mi, 2, buf + pos);
    buf[pos++] = ':';
    pos += dyn_time_utoa_pad((uint64_t)s, 2, buf + pos);
    if (nsec > 0)
        pos += dyn_time_fmt_frac((uint64_t)nsec, 9, buf + pos);

    if (off == 0) {
        buf[pos++] = 'Z';
    } else {
        int64_t o = off;
        int oneg = o < 0;
        if (oneg)
            o = -o;
        buf[pos++] = oneg ? '-' : '+';
        pos += dyn_time_utoa_pad((uint64_t)(o / 3600), 2, buf + pos);
        buf[pos++] = ':';
        pos += dyn_time_utoa_pad((uint64_t)((o / 60) % 60), 2, buf + pos);
    }
    return JS_NewStringLen(ctx, buf, pos);
}

enum {
    DYN_TOK_LIT = 0,
    DYN_TOK_YEAR,
    DYN_TOK_MON_ABBR,
    DYN_TOK_WD_ABBR,
    DYN_TOK_MONTH,
    DYN_TOK_DAY,
    DYN_TOK_HOUR,
    DYN_TOK_MIN,
    DYN_TOK_SEC,
    DYN_TOK_OFFSET
};

typedef struct {
    uint8_t kind;
    uint32_t off, len;
} DynTimeTok;

static inline uint8_t dyn_time_token_at(const char* p, size_t rem, size_t* adv)
{
    if (rem >= 4 && memcmp(p, "2006", 4) == 0) {
        *adv = 4;
        return DYN_TOK_YEAR;
    }
    if (rem >= 3 && memcmp(p, "Jan", 3) == 0) {
        *adv = 3;
        return DYN_TOK_MON_ABBR;
    }
    if (rem >= 3 && memcmp(p, "Mon", 3) == 0) {
        *adv = 3;
        return DYN_TOK_WD_ABBR;
    }
    if (rem >= 2) {
        if (p[0] == '%' && p[1] == 'z') {
            *adv = 2;
            return DYN_TOK_OFFSET;
        }
        if (memcmp(p, "01", 2) == 0) {
            *adv = 2;
            return DYN_TOK_MONTH;
        }
        if (memcmp(p, "02", 2) == 0) {
            *adv = 2;
            return DYN_TOK_DAY;
        }
        if (memcmp(p, "15", 2) == 0) {
            *adv = 2;
            return DYN_TOK_HOUR;
        }
        if (memcmp(p, "04", 2) == 0) {
            *adv = 2;
            return DYN_TOK_MIN;
        }
        if (memcmp(p, "05", 2) == 0) {
            *adv = 2;
            return DYN_TOK_SEC;
        }
    }
    *adv = 1;
    return DYN_TOK_LIT;
}

static size_t dyn_time_tokenize(const char* layout, size_t llen,
    DynTimeTok* tok, size_t cap)
{
    size_t i = 0, n = 0;

    while (i < llen) {
        size_t adv;
        uint8_t k = dyn_time_token_at(layout + i, llen - i, &adv);
        if (k == DYN_TOK_LIT) {
            size_t start = i;
            while (i < llen) {
                size_t a2;
                if (dyn_time_token_at(layout + i, llen - i, &a2) != DYN_TOK_LIT)
                    break;
                i += a2;
            }
            if (tok) {
                if (n >= cap)
                    return SIZE_MAX;
                tok[n].kind = DYN_TOK_LIT;
                tok[n].off = (uint32_t)start;
                tok[n].len = (uint32_t)(i - start);
            }
            n++;
            continue;
        }
        if (tok) {
            if (n >= cap)
                return SIZE_MAX;
            tok[n].kind = k;
            tok[n].off = (uint32_t)i;
            tok[n].len = (uint32_t)adv;
        }
        n++;
        i += adv;
    }
    return n;
}

typedef struct {
    int64_t y;
    int mo, d, h, mi, s, wd;
    int off_min;
} DynTimeParts;

static void dyn_time_explode(int64_t sec, int off_min, DynTimeParts* t)
{
    int64_t local;
    if (((int64_t)off_min * 60 > 0 && sec > INT64_MAX - (int64_t)off_min * 60)
        || ((int64_t)off_min * 60 < 0 && sec < INT64_MIN - (int64_t)off_min * 60))
        local = sec > 0 ? INT64_MAX : INT64_MIN;
    else
        local = sec + (int64_t)off_min * 60;
    int64_t days = dyn_time_floor_div(local, DYN_SECS_PER_DAY);
    int64_t tod = dyn_time_floor_mod(local, DYN_SECS_PER_DAY);
    t->off_min = off_min;
    t->h = (int)(tod / 3600);
    t->mi = (int)((tod / 60) % 60);
    t->s = (int)(tod % 60);
    dyn_civil_from_days(days, &t->y, &t->mo, &t->d);
    t->wd = dyn_weekday_from_days(days);
}

static inline int dyn_time_emit_one(DynTimeBuf* out, uint8_t kind,
    const char* lit, size_t litlen,
    const DynTimeParts* t)
{
    char tmp[24];
    int n;

    switch (kind) {
    case DYN_TOK_LIT:
        return dyn_time_buf_put(out, lit, litlen);
    case DYN_TOK_YEAR: {
        int yneg = t->y < 0;
        uint64_t yy = (uint64_t)(yneg ? -t->y : t->y);
        if (yneg && dyn_time_buf_put(out, "-", 1))
            return -1;
        n = dyn_time_utoa_pad(yy, 4, tmp);
        return dyn_time_buf_put(out, tmp, (size_t)n);
    }
    case DYN_TOK_MON_ABBR:
        return dyn_time_buf_put(out, dyn_time_month_abbr[t->mo - 1], 3);
    case DYN_TOK_WD_ABBR:
        return dyn_time_buf_put(out, dyn_time_weekday_abbr[t->wd], 3);
    case DYN_TOK_OFFSET: {
        int o = t->off_min;
        if (o == 0)
            return dyn_time_buf_put(out, "Z", 1);
        tmp[0] = o < 0 ? '-' : '+';
        if (o < 0)
            o = -o;
        n = dyn_time_utoa_pad((uint64_t)(o / 60), 2, tmp + 1);
        tmp[1 + n] = ':';
        n += 1 + dyn_time_utoa_pad((uint64_t)(o % 60), 2, tmp + 2 + n);
        return dyn_time_buf_put(out, tmp, (size_t)n + 1);
    }
    default: {
        int v = kind == DYN_TOK_MONTH ? t->mo : kind == DYN_TOK_DAY ? t->d
            : kind == DYN_TOK_HOUR                                  ? t->h
            : kind == DYN_TOK_MIN                                   ? t->mi
                                                                    : t->s;
        n = dyn_time_utoa_pad((uint64_t)v, 2, tmp);
        return dyn_time_buf_put(out, tmp, (size_t)n);
    }
    }
}

static JSValue dyn_time_format_unix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t sec;
    int64_t off_min = 0;
    int64_t y, days, tod;
    int mo, d, h, mi, s, wd;
    const char* layout;
    size_t llen, i;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    int have_buf = 0;

    (void)this_val;
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;
    layout = JS_ToCStringLen(ctx, &llen, argv[1]);
    if (!layout)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        static const char* const fu_keys[] = { "offsetMinutes" };
        if (!JS_IsObject(argv[2])) {
            JS_FreeCString(ctx, layout);
            return JS_ThrowTypeError(ctx,
                "formatUnix: options must be an object");
        }
        if (dyn_time_opts_strict(ctx, argv[2], fu_keys, 1)) {
            JS_FreeCString(ctx, layout);
            return JS_EXCEPTION;
        }
        if (dyn_time_read_offset(ctx, argv[2], &off_min)) {
            JS_FreeCString(ctx, layout);
            return JS_EXCEPTION;
        }
    }

    {
        int64_t local;
        if (dyn_time_add_ck(ctx, sec, off_min * 60, &local, "the timestamp"))
            return JS_EXCEPTION;
        days = dyn_time_floor_div(local, DYN_SECS_PER_DAY);
        tod = dyn_time_floor_mod(local, DYN_SECS_PER_DAY);
    }
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);
    dyn_civil_from_days(days, &y, &mo, &d);
    wd = dyn_weekday_from_days(days);

    if (dyn_time_buf_init(ctx, &out, llen + 16))
        goto done;
    have_buf = 1;

    i = 0;
    while (i < llen) {
        char tmp[24];
        size_t rem = llen - i;
        const char* p = layout + i;
        int n;

        if (rem >= 4 && memcmp(p, "2006", 4) == 0) {
            int yneg = y < 0;
            uint64_t yy = (uint64_t)(yneg ? -y : y);
            if (yneg && dyn_time_buf_put(&out, "-", 1))
                goto done;
            n = dyn_time_utoa_pad(yy, 4, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 4;
        } else if (rem >= 3 && memcmp(p, "Jan", 3) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_month_abbr[mo - 1], 3))
                goto done;
            i += 3;
        } else if (rem >= 3 && memcmp(p, "Mon", 3) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_weekday_abbr[wd], 3))
                goto done;
            i += 3;
        } else if (rem >= 2 && memcmp(p, "01", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)mo, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "02", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)d, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "15", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)h, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "04", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)mi, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && memcmp(p, "05", 2) == 0) {
            n = dyn_time_utoa_pad((uint64_t)s, 2, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)n))
                goto done;
            i += 2;
        } else if (rem >= 2 && p[0] == '%' && p[1] == 'z') {
            int o = (int)off_min;
            if (o == 0) {
                if (dyn_time_buf_put(&out, "Z", 1))
                    goto done;
            } else {
                tmp[0] = o < 0 ? '-' : '+';
                if (o < 0)
                    o = -o;
                n = dyn_time_utoa_pad((uint64_t)(o / 60), 2, tmp + 1);
                tmp[1 + n] = ':';
                n += 1 + dyn_time_utoa_pad((uint64_t)(o % 60), 2, tmp + 2 + n);
                if (dyn_time_buf_put(&out, tmp, (size_t)n + 1))
                    goto done;
            }
            i += 2;
        } else {
            if (dyn_time_buf_put(&out, p, 1))
                goto done;
            i += 1;
        }
    }
    result = JS_NewStringLen(ctx, (const char*)out.data, out.len);

done:
    if (have_buf)
        dyn_time_buf_free(&out);
    JS_FreeCString(ctx, layout);
    return result;
}

static int dyn_time_expect_digits(const char* s, size_t len, size_t pos,
    int n, int64_t* out);
static int dyn_time_scan_year(const char* s, size_t len, size_t* ppos,
    int64_t* out);

typedef struct {
    char* layout;
    size_t llen;
    DynTimeTok* tok;
    size_t ntok;
    int off_min;
} DynTimeFormat;

static JSClassID dyn_time_format_class_id;

static void dyn_time_format_dispose(void* native)
{
    DynTimeFormat* f = (DynTimeFormat*)native;
    if (!f)
        return;
    free(f->layout);
    free(f->tok);
    free(f);
}

static void dyn_time_format_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_time_format_dispose(JS_GetOpaque(val, dyn_time_format_class_id));
}

static const JSClassDef dyn_time_format_class = {
    "Format",
    .finalizer = dyn_time_format_finalizer,
};

static JSValue dyn_time_format_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    DynTimeFormat* f;
    const char* layout;
    size_t llen, ntok;
    int64_t off_min = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Format(layout) requires a layout");
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Format(layout): the layout must be a string");
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const fmt_keys[] = { "offsetMinutes" };
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "Format(layout, opts): options must "
                                          "be an object");
        if (dyn_time_opts_strict(ctx, argv[1], fmt_keys, 1))
            return JS_EXCEPTION;
        if (dyn_time_read_offset(ctx, argv[1], &off_min))
            return JS_EXCEPTION;
    }
    layout = JS_ToCStringLen(ctx, &llen, argv[0]);
    if (!layout)
        return JS_EXCEPTION;
    ntok = dyn_time_tokenize(layout, llen, NULL, 0);
    f = (DynTimeFormat*)malloc(sizeof(*f));
    if (!f) {
        JS_FreeCString(ctx, layout);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->llen = llen;
    f->ntok = ntok;
    f->off_min = (int)off_min;
    f->layout = (char*)malloc(llen ? llen : 1);
    f->tok = (DynTimeTok*)malloc((ntok ? ntok : 1) * sizeof(DynTimeTok));
    if (!f->layout || !f->tok) {
        dyn_time_format_dispose(f);
        JS_FreeCString(ctx, layout);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(f->layout, layout, llen);
    JS_FreeCString(ctx, layout);
    dyn_time_tokenize(f->layout, llen, f->tok, ntok);
    return dyn_plain_wrap(ctx, new_target, dyn_time_format_class_id, f,
        dyn_time_format_dispose);
}

static JSValue dyn_time_format_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynTimeFormat* f;
    int64_t sec;
    DynTimeParts t;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    (void)argc;

    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;
    f = (DynTimeFormat*)dyn_plain_get(ctx, this_val,
        dyn_time_format_class_id);
    if (!f)
        return JS_EXCEPTION;
    dyn_time_explode(sec, f->off_min, &t);
    if (dyn_time_buf_init(ctx, &out, f->llen + 16))
        return JS_EXCEPTION;
    {
        size_t k;
        int bad = 0;
        for (k = 0; k < f->ntok && !bad; k++)
            bad = dyn_time_emit_one(&out, f->tok[k].kind,
                f->layout + f->tok[k].off, f->tok[k].len,
                &t);
        if (!bad)
            result = JS_NewStringLen(ctx, (const char*)out.data, out.len);
    }
    dyn_time_buf_free(&out);
    return result;
}

static JSValue dyn_time_format_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynTimeFormat* f;
    const char* str;
    size_t slen, pos = 0, k;
    int64_t y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0, v;
    int64_t off_sec = 0;
    int have_off = 0;
    JSValue result;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    f = (DynTimeFormat*)dyn_plain_get(ctx, this_val,
        dyn_time_format_class_id);
    if (!f) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    off_sec = (int64_t)f->off_min * 60;
    for (k = 0; k < f->ntok; k++) {
        const DynTimeTok* tk = &f->tok[k];
        int i;
        switch (tk->kind) {
        case DYN_TOK_LIT:
            if (pos + tk->len > slen || memcmp(str + pos, f->layout + tk->off, tk->len) != 0)
                goto fail;
            pos += tk->len;
            break;
        case DYN_TOK_OFFSET: {
            int64_t oh, om, so;
            int neg = 0;
            if (pos < slen && (str[pos] == 'Z' || str[pos] == 'z')) {
                pos++;
                so = 0;
            } else {
                if (pos >= slen || (str[pos] != '+' && str[pos] != '-'))
                    goto fail;
                neg = str[pos] == '-';
                pos++;
                if (dyn_time_expect_digits(str, slen, pos, 2, &oh))
                    goto fail;
                pos += 2;
                if (pos >= slen || str[pos] != ':')
                    goto fail;
                pos++;
                if (dyn_time_expect_digits(str, slen, pos, 2, &om))
                    goto fail;
                pos += 2;
                if (oh > 23 || om > 59)
                    goto fail;
                so = neg ? -(oh * 3600 + om * 60) : (oh * 3600 + om * 60);
            }
            if (have_off && so != off_sec)
                goto fail;
            off_sec = so;
            have_off = 1;
            break;
        }
        case DYN_TOK_YEAR: {
            int neg = 0;
            if (pos < slen && str[pos] == '-') {
                neg = 1;
                pos++;
            }
            if (dyn_time_expect_digits(str, slen, pos, 4, &y))
                goto fail;
            pos += 4;
            if (neg)
                y = -y;
        } break;
        case DYN_TOK_MON_ABBR:
            for (i = 0; i < 12; i++)
                if (pos + 3 <= slen && memcmp(str + pos, dyn_time_month_abbr[i], 3) == 0)
                    break;
            if (i == 12)
                goto fail;
            mo = i + 1;
            pos += 3;
            break;
        case DYN_TOK_WD_ABBR:
            for (i = 0; i < 7; i++)
                if (pos + 3 <= slen && memcmp(str + pos, dyn_time_weekday_abbr[i], 3) == 0)
                    break;
            if (i == 7)
                goto fail;
            pos += 3;
            break;
        default:
            if (dyn_time_expect_digits(str, slen, pos, 2, &v))
                goto fail;
            pos += 2;
            if (tk->kind == DYN_TOK_MONTH)
                mo = v;
            else if (tk->kind == DYN_TOK_DAY)
                d = v;
            else if (tk->kind == DYN_TOK_HOUR)
                h = v;
            else if (tk->kind == DYN_TOK_MIN)
                mi = v;
            else
                se = v;
            break;
        }
    }
    if (pos != slen)
        goto fail;
    if (mo < 1 || mo > 12 || d < 1 || d > dyn_time_days_in_month(y, (int)mo) || h > 23 || mi > 59 || se > 60)
        goto fail;
    if (y < DYN_TIME_MIN_YEAR || y > DYN_TIME_MAX_YEAR)
        goto fail;
    if (off_sec < -172800 || off_sec > 172800)
        goto fail;
    JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx,
        dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se - off_sec);
fail:
    result = JS_ThrowSyntaxError(ctx, "input does not match the layout");
    JS_FreeCString(ctx, str);
    return result;
}

static JSValue dyn_time_format_layout(JSContext* ctx, JSValueConst this_val)
{
    DynTimeFormat* f = (DynTimeFormat*)dyn_plain_get(ctx, this_val,
        dyn_time_format_class_id);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, f->layout, f->llen);
}

static const JSCFunctionListEntry dyn_time_format_proto[] = {
    JS_CFUNC_DEF("format", 1, dyn_time_format_format),
    JS_CFUNC_DEF("parse", 1, dyn_time_format_parse),
    JS_CGETSET_DEF("layout", dyn_time_format_layout, NULL),
};

static int dyn_time_expect_digits(const char* s, size_t len, size_t pos,
    int n, int64_t* out)
{
    int64_t v = 0;
    int i;
    if (pos + (size_t)n > len)
        return -1;
    for (i = 0; i < n; i++) {
        char c = s[pos + (size_t)i];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return 0;
}

static int dyn_time_scan_year(const char* s, size_t len, size_t* ppos,
    int64_t* out)
{
    size_t pos = *ppos, start;
    int neg = 0;
    int64_t v = 0;

    if (pos < len && s[pos] == '-') {
        neg = 1;
        pos++;
    }
    start = pos;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
        if (v > (INT64_MAX - 9) / 10)
            return -1;
        v = v * 10 + (s[pos] - '0');
        if (v > DYN_TIME_MAX_YEAR)
            return -1;
        pos++;
    }
    if (pos - start < 4)
        return -1;
    *out = neg ? -v : v;
    *ppos = pos;
    return 0;
}

static JSValue dyn_time_parse_rfc3339(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t len, pos = 0;
    int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, nsec = 0, off_sec = 0;
    JSValue result;

    (void)this_val;
    (void)argc;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    if (dyn_time_scan_year(s, len, &pos, &y))
        goto fail;
    if (pos >= len || s[pos] != '-')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &mo))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != '-')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &d))
        goto fail;
    pos += 2;
    if (pos >= len || (s[pos] != 'T' && s[pos] != 't'))
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &h))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != ':')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &mi))
        goto fail;
    pos += 2;
    if (pos >= len || s[pos] != ':')
        goto fail;
    pos++;
    if (dyn_time_expect_digits(s, len, pos, 2, &se))
        goto fail;
    pos += 2;

    if (pos < len && s[pos] == '.') {
        size_t start;
        int ndig, i, use;
        int64_t frac = 0;
        pos++;
        start = pos;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9')
            pos++;
        ndig = (int)(pos - start);
        if (ndig == 0)
            goto fail;
        use = ndig < 9 ? ndig : 9;
        for (i = 0; i < use; i++)
            frac = frac * 10 + (s[start + (size_t)i] - '0');
        for (; i < 9; i++)
            frac *= 10;
        nsec = frac;
    }

    if (pos >= len)
        goto fail;
    if (s[pos] == 'Z' || s[pos] == 'z') {
        pos++;
        off_sec = 0;
    } else if (s[pos] == '+' || s[pos] == '-') {
        int sign = (s[pos] == '-') ? -1 : 1;
        int64_t oh, om;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &oh))
            goto fail;
        pos += 2;
        if (pos >= len || s[pos] != ':')
            goto fail;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &om))
            goto fail;
        pos += 2;
        if (oh > 23 || om > 59)
            goto fail;
        off_sec = sign * (oh * 3600 + om * 60);
    } else {
        goto fail;
    }
    if (pos != len)
        goto fail;

    if (mo < 1 || mo > 12)
        goto fail;
    if (d < 1 || d > dyn_time_days_in_month(y, (int)mo))
        goto fail;
    if (h > 23 || mi > 59 || se > 60)
        goto fail;

    {
        int64_t days = dyn_days_from_civil(y, (int)mo, d);
        int64_t total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se - off_sec;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            result = JS_EXCEPTION;
            goto out;
        }
        if (JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt64(ctx, total),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueStr(ctx, obj, "nsec",
                   JS_NewInt32(ctx, (int32_t)nsec),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, obj);
            result = JS_EXCEPTION;
            goto out;
        }
        result = obj;
        goto out;
    }

fail:
    JS_ThrowSyntaxError(ctx, "dyna:time: invalid RFC3339 timestamp");
    result = JS_EXCEPTION;

out:
    JS_FreeCString(ctx, s);
    return result;
}

static JSValue dyn_time_date(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t y, mo, d, h = 0, mi = 0, s = 0, days, total;

    (void)this_val;
    if (JS_ToInt64Ext(ctx, &y, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt64Ext(ctx, &mo, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt64Ext(ctx, &d, argv[2]))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToInt64Ext(ctx, &h, argv[3]))
        return JS_EXCEPTION;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && JS_ToInt64Ext(ctx, &mi, argv[4]))
        return JS_EXCEPTION;
    if (argc > 5 && !JS_IsUndefined(argv[5]) && JS_ToInt64Ext(ctx, &s, argv[5]))
        return JS_EXCEPTION;

    dyn_time_norm_month(&y, &mo);
    if (y < DYN_TIME_MIN_YEAR || y > DYN_TIME_MAX_YEAR)
        return JS_ThrowRangeError(ctx, "date: year %lld is outside %d..%d",
            (long long)y, DYN_TIME_MIN_YEAR,
            DYN_TIME_MAX_YEAR);
    if (d < -1000000000 || d > 1000000000 || h < -1000000 || h > 1000000
        || mi < -1000000 || mi > 1000000 || s < -1000000 || s > 1000000)
        return JS_ThrowRangeError(ctx, "date: a field is out of range");
    days = dyn_days_from_civil(y, (int)mo, d);
    total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + s;
    return JS_NewInt64(ctx, total);
}

static JSValue dyn_time_from_unix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t sec, days, tod, y, yday_start;
    int mo, d, h, mi, s, wd, yday;
    JSValue obj;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;

    days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    h = (int)(tod / 3600);
    mi = (int)((tod / 60) % 60);
    s = (int)(tod % 60);

    dyn_civil_from_days(days, &y, &mo, &d);
    wd = dyn_weekday_from_days(days);
    yday_start = dyn_days_from_civil(y, 1, 1);
    yday = (int)(days - yday_start) + 1;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "year", JS_NewInt64(ctx, y),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "month", JS_NewInt32(ctx, mo),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "day", JS_NewInt32(ctx, d),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "hour", JS_NewInt32(ctx, h),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "min", JS_NewInt32(ctx, mi),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt32(ctx, s),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "weekday", JS_NewInt32(ctx, wd),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, obj, "yday", JS_NewInt32(ctx, yday),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

#include "dyna-temporal.inc.c"

static JSValue dyn_time_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ms;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, ms);
}

static JSValue dyn_time_duration_secs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t s;

    (void)this_val;
    (void)argc;
    if (JS_IsBigInt(ctx, argv[0])) {
        if (JS_ToBigInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (isnan(d) || isinf(d))
            return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, 0);
        d = trunc(d);
        if (d < -9.0e18 || d > 9.0e18)
            return JS_ThrowRangeError(ctx, "durationSecs: %.17g seconds overflows "
                                           "the Duration millisecond field",
                d);
        s = (int64_t)d;
    }
    if (s > INT64_MAX / 1000 || s < INT64_MIN / 1000)
        return JS_ThrowRangeError(ctx, "durationSecs: %lld seconds overflows "
                                       "the Duration millisecond field",
            (long long)s);
    return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, s * 1000);
}

static JSValue dyn_time_to_plain_datetime(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    tp_date_t* d;
    tp_time_t* t;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "toPlainDateTime(date, time): a PlainDate "
                                      "and a PlainTime are required");
    d = (tp_date_t*)dyn_plain_get(ctx, argv[0], dyn_pdate_class_id);
    if (!d)
        return JS_EXCEPTION;
    t = (tp_time_t*)dyn_plain_get(ctx, argv[1], dyn_ptime_class_id);
    if (!t)
        return JS_EXCEPTION;
    return tp_new_dt(ctx, JS_UNDEFINED, d->y, d->m, d->d, t->ms);
}

static JSValue dyn_pdt_until(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    tp_dt_t* a = (tp_dt_t*)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    tp_dt_t* b;
    int64_t months, ea, eb, rem, days, ms;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDateTime.until(other): a "
                                      "PlainDateTime is required");
    b = (tp_dt_t*)dyn_plain_get(ctx, argv[0], dyn_pdt_class_id);
    if (!b)
        return JS_EXCEPTION;

    months = (b->d.y - a->d.y) * 12 + (b->d.m - a->d.m);
    if (months > 0 && b->d.d < a->d.d)
        months--;
    if (months < 0 && b->d.d > a->d.d)
        months++;
    {
        int64_t tm = (int64_t)(a->d.m - 1) + months;
        int64_t yy = a->d.y + (tm >= 0 ? tm / 12 : (tm - 11) / 12);
        int mm = (int)(((tm % 12) + 12) % 12) + 1;
        int dim = tp_days_in_month(yy, mm);
        int dd = a->d.d > dim ? dim : a->d.d;
        ea = tp_days_from_civil(yy, mm, dd) * TP_DAY_MS + a->ms;
    }
    eb = tp_days_from_civil(b->d.y, b->d.m, b->d.d) * TP_DAY_MS + b->ms;
    rem = eb - ea;
    days = rem / TP_DAY_MS;
    ms = rem % TP_DAY_MS;
    return tp_new_dur3(ctx, JS_UNDEFINED, months, days, ms);
}

static const JSCFunctionListEntry dyn_time_funcs[] = {
    JS_CFUNC_MAGIC_DEF("parseDate", 1, dyn_pdate_from, 0),
    JS_CFUNC_MAGIC_DEF("dateFromEpochDay", 1, dyn_pdate_from, 1),
    JS_CFUNC_DEF("parseTime", 1, dyn_ptime_parse),
    JS_PROP_INT64_DEF("Nanosecond", 1LL, 0),
    JS_PROP_INT64_DEF("Microsecond", DYN_NS_PER_US, 0),
    JS_PROP_INT64_DEF("Millisecond", DYN_NS_PER_MS, 0),
    JS_PROP_INT64_DEF("Second", DYN_NS_PER_SEC, 0),
    JS_PROP_INT64_DEF("Minute", DYN_NS_PER_MIN, 0),
    JS_PROP_INT64_DEF("Hour", DYN_NS_PER_HOUR, 0),
    JS_CFUNC_DEF("durationString", 1, dyn_time_duration_string),
    JS_CFUNC_DEF("parseDuration", 1, dyn_time_parse_duration),
    JS_CFUNC_DEF("parseDurationMs", 1, dyn_time_parse_duration_ms),
    JS_CFUNC_DEF("parseDurationSecs", 1, dyn_time_parse_duration_secs),
    JS_CFUNC_DEF("durationMs", 1, dyn_time_duration_ms),
    JS_CFUNC_DEF("durationSecs", 1, dyn_time_duration_secs),
    JS_CFUNC_DEF("now", 0, dyn_time_now),
    JS_CFUNC_DEF("nowSec", 0, dyn_time_now_sec),
    JS_CFUNC_DEF("nowUnixNano", 0, dyn_time_now_unix_nano),
    JS_CFUNC_DEF("nowNanos", 0, dyn_time_now_unix_nano),
    JS_CFUNC_DEF("nowMillis", 0, dyn_time_now_millis),
    JS_CFUNC_DEF("monotonicNano", 0, dyn_time_monotonic_nano),
    JS_CFUNC_DEF("formatRFC3339", 1, dyn_time_format_rfc3339),
    JS_CFUNC_DEF("formatUnix", 2, dyn_time_format_unix),
    JS_CFUNC_DEF("parseRFC3339", 1, dyn_time_parse_rfc3339),
    JS_CFUNC_DEF("date", 3, dyn_time_date),
    JS_CFUNC_DEF("fromUnix", 1, dyn_time_from_unix),
    JS_CFUNC_DEF("toPlainDateTime", 2, dyn_time_to_plain_datetime),
};

#define DYN_DP_MAX_NAMES 12

typedef struct {
    const char* name;
    const char* const* months;
    const char* const* months_abbr;
    const char* const* days;
    int day_first;
} DynDateLocale;

static const char* const dyn_dp_en_months[12] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"
};
static const char* const dyn_dp_en_abbr[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};
static const char* const dyn_dp_en_days[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"
};

static const char* const dyn_dp_fr_months[12] = {
    "janvier", "fevrier", "mars", "avril", "mai", "juin",
    "juillet", "aout", "septembre", "octobre", "novembre", "decembre"
};
static const char* const dyn_dp_fr_abbr[12] = {
    "jan", "fev", "mar", "avr", "mai", "jui",
    "jul", "aou", "sep", "oct", "nov", "dec"
};
static const char* const dyn_dp_fr_days[7] = {
    "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"
};

static const char* const dyn_dp_de_months[12] = {
    "januar", "februar", "marz", "april", "mai", "juni",
    "juli", "august", "september", "oktober", "november", "dezember"
};
static const char* const dyn_dp_de_abbr[12] = {
    "jan", "feb", "mar", "apr", "mai", "jun",
    "jul", "aug", "sep", "okt", "nov", "dez"
};
static const char* const dyn_dp_de_days[7] = {
    "sonntag", "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag"
};

static const char* const dyn_dp_es_months[12] = {
    "enero", "febrero", "marzo", "abril", "mayo", "junio",
    "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"
};
static const char* const dyn_dp_es_abbr[12] = {
    "ene", "feb", "mar", "abr", "may", "jun",
    "jul", "ago", "sep", "oct", "nov", "dic"
};
static const char* const dyn_dp_es_days[7] = {
    "domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"
};

static const DynDateLocale dyn_dp_locales[] = {
    { "en-US", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "en-GB", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 1 },
    { "en", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "fr", dyn_dp_fr_months, dyn_dp_fr_abbr, dyn_dp_fr_days, 1 },
    { "de", dyn_dp_de_months, dyn_dp_de_abbr, dyn_dp_de_days, 1 },
    { "es", dyn_dp_es_months, dyn_dp_es_abbr, dyn_dp_es_days, 1 },
};

typedef struct {
    const DynDateLocale* loc;
    int64_t base;
    int has_base;
} DynDateParser;

static JSClassID dyn_dp_class_id;

static void dyn_dp_dispose(void* native)
{
    free(native);
}

static void dyn_dp_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_dp_dispose(JS_GetOpaque(val, dyn_dp_class_id));
}

static const JSClassDef dyn_dp_class = {
    "DateParser",
    .finalizer = dyn_dp_finalizer,
};

static int dyn_dp_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static void dyn_dp_skip_space(const char* s, size_t len, size_t* pos)
{
    while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == ',' || s[*pos] == '.'))
        (*pos)++;
}

static int dyn_dp_digits(const char* s, size_t len, size_t* pos, int max,
    int64_t* out)
{
    int n = 0;
    int64_t v = 0;
    while (*pos < len && n < max && s[*pos] >= '0' && s[*pos] <= '9') {
        v = v * 10 + (s[*pos] - '0');
        (*pos)++;
        n++;
    }
    *out = v;
    return n;
}

static size_t dyn_dp_word(const char* s, size_t len, size_t pos,
    const char* word)
{
    size_t k = 0;
    while (word[k]) {
        if (pos + k >= len || dyn_dp_lower((unsigned char)s[pos + k]) != word[k])
            return 0;
        k++;
    }
    if (pos + k < len) {
        int c = dyn_dp_lower((unsigned char)s[pos + k]);
        if (c >= 'a' && c <= 'z')
            return 0;
    }
    return k;
}

static int dyn_dp_month(const DynDateParser* p, const char* s, size_t len,
    size_t* pos)
{
    int i;
    for (i = 0; i < 12; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->months[i]);
        if (k) {
            *pos += k;
            return i + 1;
        }
    }
    for (i = 0; i < 12; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->months_abbr[i]);
        if (k) {
            *pos += k;
            return i + 1;
        }
    }
    return 0;
}

static int dyn_dp_weekday(const DynDateParser* p, const char* s, size_t len,
    size_t* pos)
{
    int i;
    for (i = 0; i < 7; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->days[i]);
        if (k) {
            *pos += k;
            return i;
        }
        {
            const char* d = p->loc->days[i];
            if (*pos + 3 <= len && dyn_dp_lower((unsigned char)s[*pos]) == d[0] && dyn_dp_lower((unsigned char)s[*pos + 1]) == d[1] && dyn_dp_lower((unsigned char)s[*pos + 2]) == d[2]) {
                int nx = (*pos + 3 < len)
                    ? dyn_dp_lower((unsigned char)s[*pos + 3])
                    : 0;
                if (!(nx >= 'a' && nx <= 'z')) {
                    *pos += 3;
                    return i;
                }
            }
        }
    }
    return -1;
}

static int64_t dyn_dp_expand_year(int64_t y, int digits)
{
    if (digits > 2)
        return y;
    return (y <= 69) ? 2000 + y : 1900 + y;
}

static int dyn_dp_time(const char* s, size_t len, size_t* pos,
    int64_t* h, int64_t* mi, int64_t* se)
{
    size_t p = *pos;
    int64_t v;
    int n;

    n = dyn_dp_digits(s, len, &p, 2, &v);
    if (n == 0 || p >= len || s[p] != ':')
        return 0;
    *h = v;
    p++;
    if (dyn_dp_digits(s, len, &p, 2, &v) == 0)
        return 0;
    *mi = v;
    *se = 0;
    if (p < len && s[p] == ':') {
        size_t q = p + 1;
        if (dyn_dp_digits(s, len, &q, 2, &v)) {
            *se = v;
            p = q;
        }
    }
    while (p < len && s[p] == ' ')
        p++;
    if (dyn_dp_word(s, len, p, "pm")) {
        if (*h < 12)
            *h += 12;
        p += 2;
    } else if (dyn_dp_word(s, len, p, "am")) {
        if (*h == 12)
            *h = 0;
        p += 2;
    }
    if (*h > 23 || *mi > 59 || *se > 60)
        return 0;
    *pos = p;
    return 1;
}

static int dyn_dp_valid_ymd(int64_t y, int64_t mo, int64_t d)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int max;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    max = mdays[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        max = 29;
    return d <= max;
}

static int dyn_dp_unit(const char* s, size_t len, size_t* pos, int64_t* secs,
    int* months)
{
    static const struct {
        const char* w;
        int64_t s;
        int mo;
    } units[] = {
        { "seconds", 1, 0 },
        { "second", 1, 0 },
        { "secs", 1, 0 },
        { "sec", 1, 0 },
        { "minutes", 60, 0 },
        { "minute", 60, 0 },
        { "mins", 60, 0 },
        { "min", 60, 0 },
        { "hours", 3600, 0 },
        { "hour", 3600, 0 },
        { "hrs", 3600, 0 },
        { "hr", 3600, 0 },
        { "days", 86400, 0 },
        { "day", 86400, 0 },
        { "weeks", 604800, 0 },
        { "week", 604800, 0 },
        { "months", 0, 1 },
        { "month", 0, 1 },
        { "years", 0, 12 },
        { "year", 0, 12 },
    };
    size_t i;
    for (i = 0; i < countof(units); i++) {
        size_t k = dyn_dp_word(s, len, *pos, units[i].w);
        if (k) {
            *pos += k;
            *secs = units[i].s;
            *months = units[i].mo;
            return 1;
        }
    }
    return 0;
}

static int64_t dyn_dp_add_months(int64_t t, int64_t n)
{
    int64_t days = t / DYN_SECS_PER_DAY, tod = t % DYN_SECS_PER_DAY, y;
    int mo, d;
    if (tod < 0) {
        tod += DYN_SECS_PER_DAY;
        days--;
    }
    dyn_civil_from_days(days, &y, &mo, &d);
    {
        int64_t total = (int64_t)mo - 1 + n;
        int64_t ny = y + (total >= 0 ? total / 12 : -((-total + 11) / 12));
        int nm = (int)(total - (ny - y) * 12) + 1;
        while (!dyn_dp_valid_ymd(ny, nm, d))
            d--;
        return dyn_days_from_civil(ny, nm, d) * DYN_SECS_PER_DAY + tod;
    }
}

static int64_t dyn_dp_midnight(int64_t t)
{
    int64_t r = t % DYN_SECS_PER_DAY;
    if (r < 0)
        r += DYN_SECS_PER_DAY;
    return t - r;
}

static int dyn_dp_parse(const DynDateParser* p, const char* s, size_t len,
    int64_t now, int64_t* out)
{
    size_t pos = 0;
    int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, v;
    int n, have_date = 0, have_time = 0, wd;

    dyn_dp_skip_space(s, len, &pos);
    if (pos >= len)
        return -1;

    if (dyn_dp_word(s, len, pos, "now")) {
        *out = now;
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "today")) {
        *out = dyn_dp_midnight(now);
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "tomorrow")) {
        *out = dyn_dp_midnight(now) + DYN_SECS_PER_DAY;
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "yesterday")) {
        *out = dyn_dp_midnight(now) - DYN_SECS_PER_DAY;
        return 0;
    }
    {
        size_t q = pos;
        int forward = 1, months = 0;
        int64_t unit = 0;
        size_t k = dyn_dp_word(s, len, q, "in");
        if (k) {
            q += k;
            dyn_dp_skip_space(s, len, &q);
        }
        n = dyn_dp_digits(s, len, &q, 9, &v);
        if (n > 0) {
            size_t after = q;
            dyn_dp_skip_space(s, len, &after);
            if (dyn_dp_unit(s, len, &after, &unit, &months)) {
                size_t tail = after;
                dyn_dp_skip_space(s, len, &tail);
                if (dyn_dp_word(s, len, tail, "ago")) {
                    forward = 0;
                    tail += 3;
                } else if (!k) {
                    goto not_relative;
                }
                dyn_dp_skip_space(s, len, &tail);
                if (tail != len)
                    goto not_relative;
                *out = months
                    ? dyn_dp_add_months(now, forward ? v * months : -(v * months))
                    : now + (forward ? v * unit : -(v * unit));
                return 0;
            }
        }
    }
not_relative:
    {
        size_t q = pos;
        int dir = 0;
        size_t k = dyn_dp_word(s, len, q, "next");
        if (k) {
            dir = 1;
            q += k;
        } else if ((k = dyn_dp_word(s, len, q, "last")) != 0) {
            dir = -1;
            q += k;
        }
        if (dir) {
            dyn_dp_skip_space(s, len, &q);
            wd = dyn_dp_weekday(p, s, len, &q);
            dyn_dp_skip_space(s, len, &q);
            if (wd >= 0 && q == len) {
                int64_t day0 = dyn_dp_midnight(now);
                int cur = (int)(((day0 / DYN_SECS_PER_DAY) % 7 + 7 + 4) % 7);
                int delta = dir > 0 ? (wd - cur + 7) % 7 : -(((cur - wd) + 7) % 7);
                if (delta == 0)
                    delta = dir > 0 ? 7 : -7;
                *out = day0 + (int64_t)delta * DYN_SECS_PER_DAY;
                return 0;
            }
        }
    }

    {
        size_t q = pos;
        if (dyn_dp_weekday(p, s, len, &q) >= 0) {
            dyn_dp_skip_space(s, len, &q);
            if (q < len)
                pos = q;
        }
    }
    dyn_dp_skip_space(s, len, &pos);

    if ((mo = dyn_dp_month(p, s, len, &pos)) > 0) {
        dyn_dp_skip_space(s, len, &pos);
        if (dyn_dp_digits(s, len, &pos, 2, &d) == 0)
            return -1;
        dyn_dp_skip_space(s, len, &pos);
        n = dyn_dp_digits(s, len, &pos, 4, &y);
        if (n == 0)
            return -1;
        y = dyn_dp_expand_year(y, n);
        have_date = 1;
    } else {
        n = dyn_dp_digits(s, len, &pos, 4, &v);
        if (n == 0)
            return -1;
        if (n == 4 && pos < len && (s[pos] == '-' || s[pos] == '/')) {
            char sep = s[pos];
            y = v;
            pos++;
            if (dyn_dp_digits(s, len, &pos, 2, &mo) == 0)
                return -1;
            if (pos >= len || s[pos] != sep)
                return -1;
            pos++;
            if (dyn_dp_digits(s, len, &pos, 2, &d) == 0)
                return -1;
            have_date = 1;
        } else {
            size_t q = pos;
            dyn_dp_skip_space(s, len, &q);
            {
                size_t mq = q;
                int named = dyn_dp_month(p, s, len, &mq);
                if (named > 0) {
                    d = v;
                    mo = named;
                    pos = mq;
                    dyn_dp_skip_space(s, len, &pos);
                    n = dyn_dp_digits(s, len, &pos, 4, &y);
                    if (n == 0)
                        return -1;
                    y = dyn_dp_expand_year(y, n);
                    have_date = 1;
                }
            }
            if (!have_date) {
                int64_t b;
                if (pos >= len || (s[pos] != '/' && s[pos] != '-' && s[pos] != '.'))
                    return -1;
                pos++;
                if (dyn_dp_digits(s, len, &pos, 2, &b) == 0)
                    return -1;
                if (pos >= len || (s[pos] != '/' && s[pos] != '-' && s[pos] != '.'))
                    return -1;
                pos++;
                n = dyn_dp_digits(s, len, &pos, 4, &y);
                if (n == 0)
                    return -1;
                y = dyn_dp_expand_year(y, n);
                if (p->loc->day_first) {
                    d = v;
                    mo = b;
                } else {
                    mo = v;
                    d = b;
                }
                have_date = 1;
            }
        }
    }
    if (!have_date)
        return -1;
    if (!dyn_dp_valid_ymd(y, mo, d))
        return -1;

    dyn_dp_skip_space(s, len, &pos);
    if (pos < len && (s[pos] == 'T' || s[pos] == 't'))
        pos++;
    if (pos < len)
        have_time = dyn_dp_time(s, len, &pos, &h, &mi, &se);
    if (pos < len && !have_time)
        return -1;
    dyn_dp_skip_space(s, len, &pos);
    if (pos != len)
        return -1;

    *out = dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se;
    return 0;
}

static int dyn_time_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static JSValue dyn_dp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    DynDateParser* p;
    const char* name = NULL;
    const DynDateLocale* loc = &dyn_dp_locales[0];
    JSValue obj, proto;
    size_t i;
    int64_t base = 0;
    int has_base = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        size_t nlen;
        name = JS_ToCStringLen(ctx, &nlen, argv[0]);
        if (!name)
            return JS_EXCEPTION;
        for (i = 0; i < countof(dyn_dp_locales); i++) {
            if (strcmp(name, dyn_dp_locales[i].name) == 0) {
                loc = &dyn_dp_locales[i];
                break;
            }
        }
        if (i == countof(dyn_dp_locales)) {
            JSValue e = JS_ThrowRangeError(ctx,
                "dyna:time: unknown locale \"%s\"; known: "
                "en-US en-GB en fr de es",
                name);
            JS_FreeCString(ctx, name);
            return e;
        }
        JS_FreeCString(ctx, name);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const dp_keys[] = { "now" };
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "DateParser: options must be an object");
        if (dyn_time_opts_strict(ctx, argv[1], dp_keys, 1))
            return JS_EXCEPTION;
        JSValue nv = JS_GetPropertyStr(ctx, argv[1], "now");
        if (JS_IsException(nv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(nv)) {
            if (dyn_time_read_int(ctx, nv, "now", &base)) {
                JS_FreeValue(ctx, nv);
                return JS_EXCEPTION;
            }
            has_base = 1;
        }
        JS_FreeValue(ctx, nv);
    }

    p = (DynDateParser*)malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->loc = loc;
    p->base = base;
    p->has_base = has_base;

    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        free(p);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_dp_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        free(p);
        return obj;
    }
    JS_SetOpaque(obj, p);
    return obj;
}

static JSValue dyn_dp_parse_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynDateParser* p;
    const char* s;
    size_t len;
    int64_t out = 0, now;
    int rc;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "parse(text) requires an argument");
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    p = (DynDateParser*)JS_GetOpaque2(ctx, this_val, dyn_dp_class_id);
    if (!p) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    now = p->has_base ? p->base : (int64_t)time(NULL);
    rc = dyn_dp_parse(p, s, len, now, &out);
    JS_FreeCString(ctx, s);
    if (rc)
        return JS_NULL;
    return JS_NewInt64(ctx, out);
}

static JSValue dyn_dp_locale_get(JSContext* ctx, JSValueConst this_val)
{
    DynDateParser* p = (DynDateParser*)JS_GetOpaque2(ctx, this_val,
        dyn_dp_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewString(ctx, p->loc->name);
}

static JSValue dyn_dp_dayfirst_get(JSContext* ctx, JSValueConst this_val)
{
    DynDateParser* p = (DynDateParser*)JS_GetOpaque2(ctx, this_val,
        dyn_dp_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->loc->day_first);
}

static const JSCFunctionListEntry dyn_dp_proto[] = {
    JS_CFUNC_DEF("parse", 1, dyn_dp_parse_method),
    JS_CGETSET_DEF("locale", dyn_dp_locale_get, NULL),
    JS_CGETSET_DEF("dayFirst", dyn_dp_dayfirst_get, NULL),
};

#include "dyna-rrule.inc.c"

static const JSCFunctionListEntry dyn_pdt_until_proto[] = {
    JS_CFUNC_DEF("until", 1, dyn_pdt_until),
};

static int dyn_time_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_time_format_class_id,
            &dyn_time_format_class, dyn_time_format_proto,
            countof(dyn_time_format_proto),
            dyn_time_format_ctor, "Format")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_dp_class_id, &dyn_dp_class,
            dyn_dp_proto, countof(dyn_dp_proto),
            dyn_dp_ctor, "DateParser")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pdate_class_id, &dyn_pdate_class,
            dyn_pdate_proto, countof(dyn_pdate_proto),
            dyn_pdate_ctor, "PlainDate")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_dur_class_id, &dyn_dur_class,
            dyn_dur_proto, countof(dyn_dur_proto),
            dyn_dur_ctor, "Duration")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ptime_class_id, &dyn_ptime_class,
            dyn_ptime_proto, countof(dyn_ptime_proto),
            dyn_ptime_ctor, "PlainTime")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pdt_class_id, &dyn_pdt_class,
            dyn_pdt_proto, countof(dyn_pdt_proto),
            dyn_pdt_ctor, "PlainDateTime")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_pdt_class_id);
        if (!JS_IsObject(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, dyn_pdt_until_proto,
            countof(dyn_pdt_until_proto));
        JS_FreeValue(ctx, proto);
    }
    if (dyn_rrule_register(ctx, m) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_time_funcs,
        countof(dyn_time_funcs));
}

int js_nat_init_time(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:time", dyn_time_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Format") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "DateParser") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "PlainDate") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Duration") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "PlainTime") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "PlainDateTime") < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "RRule") < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_time_funcs,
        countof(dyn_time_funcs));
}

#endif
