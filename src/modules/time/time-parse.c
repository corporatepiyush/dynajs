#include "time-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#define DYN_U64_2_POW_63 (((uint64_t)1) << 63)

struct DynTimeUnit {
    const char* name;
    int len;
    int64_t ns;
};

static JSValue dyn_time_ns_to_jsvalue(JSContext* ctx, int64_t ns)
{
    if (ns >= -DYN_MAX_SAFE_INT && ns <= DYN_MAX_SAFE_INT)
        return JS_NewInt64(ctx, ns);
    return JS_NewBigInt64(ctx, ns);
}

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

JSValue dyn_time_parse_duration(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return dyn_time_ns_to_jsvalue(ctx, ns);
}

JSValue dyn_time_parse_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_time_scale_ns(ns, 1000000));
}

JSValue dyn_time_parse_duration_secs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_time_scale_ns(ns, 1000000000));
}

static int dyn_time_scan_year(const char* s, size_t len, size_t* ppos,
    int64_t* out);

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

JSValue dyn_time_parse_rfc3339(JSContext* ctx, JSValueConst this_val,
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

#endif
