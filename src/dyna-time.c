/*
 * dyna:time -- durations, RFC3339, monotonic clock, civil calendar math.
 *
 * Duration is an integer nanosecond count.
 *   - Inputs accept Number or BigInt; `monotonicNano() - t0` exceeds 2^53 at
 *     ~104 days, so BigInt is the normal case, wrapping mod 2^64.
 *   - parseDuration returns a Number when the magnitude is a safe integer and
 *     a BigInt otherwise, so sub-day values still compare with ===.
 *   - The unit-named twins (parseDurationMs/parseDurationSecs, durationMs/
 *     durationSecs) speak the unit in their name; see their comments.
 *   - durationString emits the largest unit first and trims trailing zeros
 *     from every fraction; tests/test_time.js pins the exact bytes.
 *   - PlainDate/PlainTime/PlainDateTime/Duration (dyna-temporal.inc.c) are
 *     zone-less value types; toPlainDateTime joins the two splitters and
 *     PlainDateTime.until folds months first, exactly as PlainDate.until.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_NS_PER_US     1000LL
#define DYN_NS_PER_MS     1000000LL
#define DYN_NS_PER_SEC    1000000000LL
#define DYN_NS_PER_MIN    60000000000LL
#define DYN_NS_PER_HOUR   3600000000000LL
#define DYN_SECS_PER_DAY  86400LL
#define DYN_MAX_SAFE_INT  9007199254740991LL   /* 2^53 - 1 */
#define DYN_U64_2_POW_63  (((uint64_t)1) << 63)

/* The year range PlainDate enforces (TP_MIN_YEAR/TP_MAX_YEAR in
 * dyna-temporal.inc.c, included at the bottom of this file -- those macros
 * are not visible up here). date() and parseRFC3339 cap years at the same
 * range, which also keeps days * DYN_SECS_PER_DAY inside int64. */
#define DYN_TIME_MIN_YEAR (-271821)
#define DYN_TIME_MAX_YEAR   275760

/* ================================================================ *
 *  Civil calendar: Howard Hinnant's days_from_civil / civil_from_days
 *  (http://howardhinnant.github.io/date_algorithms.html, public domain).
 *  The ONLY calendar-math primitive this module uses -- see header comment.
 * ================================================================ */

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

/* Days since the 1970-01-01 epoch (negative for earlier dates). `m` must be
 * in [1, 12]; `d` may be ANY integer (including <1 or >last-day-of-month) --
 * the linear day-of-year formula carries such overflow into the next/prior
 * month/year for free, which is exactly what date()'s field normalization
 * relies on. */
static int64_t dyn_days_from_civil(int64_t y, int m, int64_t d)
{
    int64_t era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                       /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Inverse of dyn_days_from_civil: decompose a day count into y/m/d (m in
 * [1,12], d in [1,31]). Exact for any int64_t z. */
static void dyn_civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                    /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* [0, 365] */
    mp = (5 * doy + 2) / 153;                                   /* [0, 11] */
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                   /* [1, 31] */
    *m = (int)(mp + (mp < 10 ? 3 : -9));                        /* [1, 12] */
    *y += (*m <= 2);
}

/* [0, 6] -> Sun..Sat, i.e. Sunday is 0. */
static int dyn_weekday_from_days(int64_t z)
{
    return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

/* Number of days in civil month (y, m) (m in [1, 12]), leap years included;
 * derived from the same primitive so it can never disagree with it. */
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

/* Carry an out-of-[1,12] month into *y (floor-style: month 0 => December of
 * the prior year, month 13 => January of the next). */
static void dyn_time_norm_month(int64_t *y, int64_t *mo)
{
    int64_t m0 = *mo - 1;
    int64_t yshift = dyn_time_floor_div(m0, 12);
    *y += yshift;
    *mo = dyn_time_floor_mod(m0, 12) + 1;
}

/* int64_t -> time_t, clamped on the (only theoretical, on this repo's 64-bit
 * targets) 32-bit time_t platform -- mirrors the same defensive clamp this
 * engine's own getTimezoneOffset() applies (date_timezone.inc.c). */
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

/* ================================================================ *
 *  Small bounded numeric-to-decimal helpers (never a stack overflow: every
 *  caller's buffer is sized against a proven max digit count, see header).
 * ================================================================ */

/* Write v's decimal digits (no sign, no padding) into out; returns the
 * digit count (1-20). */
static int dyn_time_utoa(uint64_t v, char *out)
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

/* Zero-padded to AT LEAST `width` digits (wider if v needs more, never
 * truncated -- the rule layout tokens and years are emitted with). */
static int dyn_time_utoa_pad(uint64_t v, int width, char *out)
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

/* Format `frac` (an integer in [0, 10^width)) as ".ddd" with trailing zeros
 * stripped; writes nothing and returns 0 if frac == 0 once padded/trimmed
 * (i.e. the fraction is exactly zero). width <= 9 at every call site. */
static int dyn_time_fmt_frac(uint64_t frac, int width, char *out)
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

/* ================================================================ *
 *  Growable output buffer for formatUnix (the only function whose output
 *  length is proportional to a caller-controlled, unbounded input string).
 * ================================================================ */

typedef struct DynTimeBuf {
    JSContext *ctx;
    uint8_t *data;
    size_t len, cap;
} DynTimeBuf;

static int dyn_time_buf_init(JSContext *ctx, DynTimeBuf *b, size_t hint)
{
    b->ctx = ctx;
    b->len = 0;
    b->cap = hint > 0 ? hint : 16;
    b->data = js_malloc(ctx, b->cap);
    if (!b->data) {
        b->cap = 0;
        return -1; /* js_malloc already threw */
    }
    return 0;
}

static int dyn_time_buf_reserve(DynTimeBuf *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t ncap;
    uint8_t *nd;
    if (need <= b->cap)
        return 0;
    ncap = b->cap * 2;
    if (ncap < need)
        ncap = need;
    nd = js_realloc(b->ctx, b->data, ncap);
    if (!nd)
        return -1; /* js_realloc already threw */
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static int dyn_time_buf_put(DynTimeBuf *b, const void *src, size_t n)
{
    if (dyn_time_buf_reserve(b, n))
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void dyn_time_buf_free(DynTimeBuf *b)
{
    js_free(b->ctx, b->data);
    b->data = NULL;
}

/* ================================================================ *
 *  Duration: constants are plain module properties (see registration at the
 *  bottom); durationString / parseDuration below.
 * ================================================================ */

/* Number when safe (so plain integer literals compare with ===), BigInt
 * otherwise (so precision is never silently lost) -- see header comment. */
static JSValue dyn_time_ns_to_jsvalue(JSContext *ctx, int64_t ns)
{
    if (ns >= -DYN_MAX_SAFE_INT && ns <= DYN_MAX_SAFE_INT)
        return JS_NewInt64(ctx, ns);
    return JS_NewBigInt64(ctx, ns);
}

#define DYN_TIME_DUR_BUF 64

/* durationString(ns) -> string. Zero is "0s"; a negative duration carries a
 * leading '-'. Below 1s one unit is chosen by magnitude: ns (no fraction), then
 * µs (3 fractional digits), then ms (6). At or above 1s the form is [Xh][Ym]Zs,
 * h and m present only when the duration reaches them, seconds carrying 9
 * fractional digits. Every fraction is trimmed of trailing zeros and omitted
 * entirely when it becomes empty. tests/test_time.js pins the exact bytes. */
static JSValue dyn_time_duration_string(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
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
        u = -u; /* well-defined unsigned negation; exact even for ns==INT64_MIN */

    if (u == 0)
        return JS_NewStringLen(ctx, "0s", 2);

    if (neg)
        buf[pos++] = '-';

    if (u < (uint64_t)DYN_NS_PER_SEC) {
        uint64_t ip, divisor;
        int width, ulen;
        const char *unit;

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
            unit = "\xc2\xb5s"; /* U+00B5 MICRO SIGN + 's' */
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
    const char *name;
    int len;
    int64_t ns;
};

/* The accepted unit set, including both spellings of the micro sign
 * (U+00B5 and the Greek-letter lookalike
 * U+03BC). The unit "span" scanned by the caller is matched by its EXACT
 * byte length first, so "m" vs "ms" is never ambiguous (see caller). */
static const struct DynTimeUnit dyn_time_units[] = {
    { "ns", 2, 1LL },
    { "us", 2, DYN_NS_PER_US },
    { "\xc2\xb5s", 3, DYN_NS_PER_US },   /* µs  U+00B5 */
    { "\xce\xbcs", 3, DYN_NS_PER_US },   /* μs  U+03BC */
    { "ms", 2, DYN_NS_PER_MS },
    { "s",  1, DYN_NS_PER_SEC },
    { "m",  1, DYN_NS_PER_MIN },
    { "h",  1, DYN_NS_PER_HOUR },
};

static int dyn_time_lookup_unit(const char *s, size_t ulen, int64_t *out_ns)
{
    size_t i;
    for (i = 0; i < countof(dyn_time_units); i++) {
        if ((size_t)dyn_time_units[i].len == ulen &&
            memcmp(dyn_time_units[i].name, s, ulen) == 0) {
            *out_ns = dyn_time_units[i].ns;
            return 0;
        }
    }
    return -1;
}

/* Consume a run of ASCII digits as a fraction: accumulates into *pf (and
 * grows *pscale = 10^ndigits-kept) only while doing so cannot overflow --
 * once it would, further digits are still consumed (so the scan position
 * stays correct) but no longer counted -- float64 has nowhere near enough
 * precision for a 19-digit fraction anyway. Always "succeeds"; *pi is
 * advanced past every digit consumed. */
static void dyn_time_leading_fraction(const char *s, size_t len, size_t *pi,
                                      uint64_t *pf, double *pscale)
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

/* The parser core behind parseDuration and its unit-named twins: parses
 * argv[0] into WHOLE NANOSECONDS. Returns 0 with *out set, or -1 with the
 * SyntaxError already thrown -- one grammar, one error, so every entry point
 * is byte-for-byte the same failure as parseDuration. */
static int dyn_time_parse_duration_ns(JSContext *ctx, JSValueConst argv0,
                                      int64_t *out)
{
    const char *orig;
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

    /* The one special case: a bare "0" needs no unit. */
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
            goto bad; /* missing unit */
        if (dyn_time_lookup_unit(orig + unit_start, i - unit_start, &unit_ns))
            goto bad; /* unknown unit */

        if (v > DYN_U64_2_POW_63 / (uint64_t)unit_ns)
            goto bad; /* overflow */
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

/* parseDuration(str) -> number|bigint: nanoseconds, a Number below 2^53 and
 * a BigInt from there up. Accepts a possibly-signed sequence of
 * (number)(unit) pairs, e.g. "300ms", "-1.5h", "2h45m"; units
 * ns/us/µs/μs/ms/s/m/h. Throws SyntaxError on malformed input (unlike
 * date()'s deliberately permissive/normalizing sibling). */
static JSValue dyn_time_parse_duration(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return dyn_time_ns_to_jsvalue(ctx, ns);
}

/* parseDurationMs(str) / parseDurationSecs(str) -> number: the same parse,
 * delivered in the unit the name says out loud. The ns count itself is exact
 * (int64 end to end); the conversion is one IEEE division, so the result is
 * the correctly-rounded double of ns/1e6 (ns/1e9) -- exact whenever that
 * quotient is a safe integer, which holds far past the point where
 * parseDuration itself had to switch to BigInt ("200000h" is 7.2e11 ms,
 * exact). Beyond 2^53 in the RESULT unit the usual double rounding applies;
 * parseDuration stays the exact path. Same grammar, same SyntaxError. */
static JSValue dyn_time_parse_duration_ms(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, (double)ns / 1e6);
}

static JSValue dyn_time_parse_duration_secs(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    int64_t ns;

    (void)this_val;
    (void)argc;
    if (dyn_time_parse_duration_ns(ctx, argv[0], &ns))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, (double)ns / 1e9);
}

/* ================================================================ *
 *  Clock: CLOCK_REALTIME (wall) / CLOCK_MONOTONIC. Never blocks/sleeps.
 * ================================================================ */

static JSValue dyn_time_now(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
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
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "nsec",
                                  JS_NewInt32(ctx, (int32_t)ts.tv_nsec),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue dyn_time_now_unix_nano(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC +
                                (int64_t)ts.tv_nsec);
}

static JSValue dyn_time_now_millis(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewInt64(ctx, (int64_t)ts.tv_sec * 1000 +
                            (int64_t)ts.tv_nsec / 1000000);
}

/* nowSec(): whole seconds since the epoch -- the `sec` half of now() as one
 * number, for callers whose resolution is a second and who would otherwise
 * write now().sec. */
static JSValue dyn_time_now_sec(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewInt64(ctx, (int64_t)ts.tv_sec);
}

static JSValue dyn_time_monotonic_nano(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:time: clock_gettime failed");
    return JS_NewBigInt64(ctx, (int64_t)ts.tv_sec * DYN_NS_PER_SEC +
                                (int64_t)ts.tv_nsec);
}

/* ================================================================ *
 *  Formatting
 * ================================================================ */

static const char *const dyn_time_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const dyn_time_weekday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

#define DYN_TIME_RFC_BUF 96

/* Defined with the DateParser below (strict bags); declared here because
 * formatRFC3339/formatUnix/Format are the first callers. */
static int dyn_time_opts_strict(JSContext *ctx, JSValueConst opts,
                                const char *const *keys, int nkeys);

/*the valid keys of formatRFC3339's options bag. */
static const char *const dyn_rfc3339_keys[] = { "nsec", "offsetMinutes" };

/* Read one INTEGER-valued number with NUMBER typing -- the one policy every
 * typed slot of this surface follows (nsec in both of its positions, sec,
 * offsetMinutes):
 *   - a non-number (a string, boolean, null, object, symbol, BigInt) throws
 *     TypeError "<name> must be a number": the declared type of these slots is
 *     `number`, and coercing "abc" to 0, `true` to 1 ns or an object's valueOf
 *     to whatever it returns is how a caller's typo becomes a silent value;
 *   - NaN/Infinity and a fraction throw RangeError "<name> must be an
 *     integer": the slot has no room for the dropped part, so truncating 2.5
 *     to 2 is data loss, not a conversion;
 *   - a whole number that does not FIT in an int64 (1e300) throws RangeError
 *     "<name> is out of range" -- it is a range failure, and reporting it as
 *     "must be an integer" describes the wrong thing;
 *   - a whole int64 value is range-checked by the CALLER, which owns the
 *     domain bound ("nsec must be in [0, 999999999]", "offsetMinutes must be
 *     in [-1439, 1439]").
 * 0 with the value set, or -1 with the TypeError/RangeError pending. */
static int dyn_time_read_int(JSContext *ctx, JSValueConst v, const char *name,
                             int64_t *out)
{
    double d;
    int64_t m;

    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:time: %s must be a number", name);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    /* `d != d` is NaN, `d - d != 0` is +/-Infinity: neither has an integer it
       could stand for, and both must be reported as the non-integer they are
       (the range test below is false for NaN, which would otherwise call it a
       range failure). */
    if (d != d || d - d != 0.0) {
        JS_ThrowRangeError(ctx, "dyna:time: %s must be an integer", name);
        return -1;
    }
    /* [-2^63, 2^63): the boundary is exact in double, and the next double
       below 2^63 is still an int64. A whole number outside this span is a
       RANGE failure, not a fractional one. */
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        JS_ThrowRangeError(ctx, "dyna:time: %s is out of range", name);
        return -1;
    }
    m = (int64_t)d;          /* defined: d is finite and inside int64 */
    if ((double)m != d) {
        JS_ThrowRangeError(ctx, "dyna:time: %s must be an integer", name);
        return -1;
    }
    *out = m;
    return 0;
}

/* Read one nsec value (see dyn_time_read_int for the typing policy). */
static int dyn_time_read_nsec(JSContext *ctx, JSValueConst v, int64_t *pns)
{
    return dyn_time_read_int(ctx, v, "nsec", pns);
}

/*read and validate one offsetMinutes value (RFC 3339 section 5.6
 * bounds the numeric zone offset to +/-23:59, i.e. +/-1439 minutes; the
 * parse side accepts exactly that range and the format side refuses to
 * emit anything its own parser would reject). An absent property is the
 * zero offset; a PRESENT one is typed and integer-valued like every other
 * typed slot here (dyn_time_read_int). 0 with the value set, or -1
 * with the exception pending. */
static int dyn_time_read_offset(JSContext *ctx, JSValueConst opts,
                                int64_t *poff)
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

/* formatRFC3339(unixSec, nsec=0, utc=true) -> string (the original form), or
 * formatRFC3339(unixSec, { nsec, offsetMinutes }) -> string (render at
 * a FIXED UTC offset instead of at UTC/local). The bag form is EXACTLY two
 * arguments -- a third positional is refused, not ignored. The bag is strict:
 * unknown keys throw a TypeError naming the key and the valid set.
 *
 * EVERY typed slot here is typed as declared, with no coercion (see
 * dyn_time_read_int): `sec` and `nsec` are whole numbers, `nsec` within
 * [0, 999999999], `offsetMinutes` a whole number of minutes within
 * [-1439, 1439], and `utc` a boolean. A non-number throws TypeError, a
 * fraction or NaN throws RangeError, a whole number that does not fit an
 * int64 throws RangeError "out of range".
 *
 * The second argument is the ONE overloaded slot: an OBJECT there is the
 * options bag (that is how the bag form is spelled), so `formatRFC3339(sec,
 * {})` is the empty bag and `formatRFC3339(sec, { nsec: 1 })` sets the nsec
 * -- an object is never read as a legacy nsec. Every non-object is the legacy
 * nsec, which refuses exactly what the bag's nsec refuses.
 *
 * The offset form appends "Z" for a zero offset and "+HH:MM"/"-HH:MM"
 * otherwise -- exactly the shapes parseRFC3339 accepts, so the pair
 * round-trips. */
static JSValue dyn_time_format_rfc3339(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    int64_t sec, nsec = 0, y, days, tod, off = 0;
    int mo, d, h, mi, s, utc = 1;
    char buf[DYN_TIME_RFC_BUF];
    int pos = 0;

    (void)this_val;
    if (dyn_time_read_int(ctx, argv[0], "sec", &sec))
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        /* the bag form: exactly (sec, opts) -- a third positional
           argument is refused, not silently ignored (the legacy (nsec, utc)
           tail belongs to the non-bag overload only) */
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
        utc = 0;            /* off is already the whole story */
    } else {
        if (argc > 1 && !JS_IsUndefined(argv[1]) &&
            dyn_time_read_nsec(ctx, argv[1], &nsec))
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
            /* Ask the OS for the local offset ONLY -- see header comment. */
            time_t ti = dyn_time_to_time_t(sec);
            struct tm tmv;
            localtime_r(&ti, &tmv);
            off = tmv.tm_gmtoff;
        }
    }
    if (nsec < 0 || nsec > 999999999)
        return JS_ThrowRangeError(ctx, "dyna:time: nsec must be in [0, 999999999]");

    days = dyn_time_floor_div(sec + off, DYN_SECS_PER_DAY);
    tod = dyn_time_floor_mod(sec + off, DYN_SECS_PER_DAY);
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

/* ================================================================ *
 *  Layout tokens
 *
 *  A layout is scanned ONCE into tokens, and the same emitter drives both
 *  formatUnix (which tokenises per call) and class Format (which does not).
 *  One emitter means the two cannot drift; the differential in
 *  tests/test_time_format.js is over the token boundary, not over two copies
 *  of the same switch.
 *
 *  Measured before this existed (tests/bench_time_layout.js): the scan was
 *  57.4 ns of a 123.2 ns formatUnix call with a 20-character layout -- 46.6%
 *  -- and grew at 4.0 ns per layout character, because every position probed
 *  eight alternatives with memcmp. That is what Format hoists out.
 *
 *  Literal runs are COALESCED, so "2006-01-02" is three field tokens and two
 *  one-byte literals rather than ten separate byte appends. formatUnix gets
 *  that improvement too.
 * ================================================================ */

enum {
    DYN_TOK_LIT = 0,   /* raw bytes from the layout */
    DYN_TOK_YEAR,      /* 2006 */
    DYN_TOK_MON_ABBR,  /* Jan  */
    DYN_TOK_WD_ABBR,   /* Mon  */
    DYN_TOK_MONTH,     /* 01   */
    DYN_TOK_DAY,       /* 02   */
    DYN_TOK_HOUR,      /* 15   */
    DYN_TOK_MIN,       /* 04   */
    DYN_TOK_SEC,       /* 05   */
    DYN_TOK_OFFSET     /* %z ("Z" at zero, else "+HH:MM"/"-HH:MM") */
};

typedef struct {
    uint8_t kind;
    uint32_t off, len;   /* into the layout; only meaningful for DYN_TOK_LIT */
} DynTimeTok;

/* The token at `p`, or DYN_TOK_LIT. *adv receives how many bytes it spans. */
static inline uint8_t dyn_time_token_at(const char *p, size_t rem, size_t *adv)
{
    if (rem >= 4 && memcmp(p, "2006", 4) == 0) { *adv = 4; return DYN_TOK_YEAR; }
    if (rem >= 3 && memcmp(p, "Jan", 3) == 0)  { *adv = 3; return DYN_TOK_MON_ABBR; }
    if (rem >= 3 && memcmp(p, "Mon", 3) == 0)  { *adv = 3; return DYN_TOK_WD_ABBR; }
    if (rem >= 2) {
        if (p[0] == '%' && p[1] == 'z') { *adv = 2; return DYN_TOK_OFFSET; }
        if (memcmp(p, "01", 2) == 0) { *adv = 2; return DYN_TOK_MONTH; }
        if (memcmp(p, "02", 2) == 0) { *adv = 2; return DYN_TOK_DAY; }
        if (memcmp(p, "15", 2) == 0) { *adv = 2; return DYN_TOK_HOUR; }
        if (memcmp(p, "04", 2) == 0) { *adv = 2; return DYN_TOK_MIN; }
        if (memcmp(p, "05", 2) == 0) { *adv = 2; return DYN_TOK_SEC; }
    }
    *adv = 1;
    return DYN_TOK_LIT;
}

/* Scan `layout` into `tok` (capacity `cap`), for class Format. Passing cap 0
 * counts without writing, so the constructor sizes exactly once. Only the
 * capability tokenises; formatUnix emits during its own scan. */
static size_t dyn_time_tokenize(const char *layout, size_t llen,
                                DynTimeTok *tok, size_t cap)
{
    size_t i = 0, n = 0;

    while (i < llen) {
        size_t adv;
        uint8_t k = dyn_time_token_at(layout + i, llen - i, &adv);
        if (k == DYN_TOK_LIT) {
            /* Coalesce the whole literal run in one token. */
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

/* A broken-down time at one fixed UTC offset (off_min minutes; 0 = UTC), so
 * the emitter takes one argument rather than eight.: the fields are the
 * wall-clock fields AT that offset, and DYN_TOK_OFFSET emits the offset
 * itself. */
typedef struct {
    int64_t y;
    int mo, d, h, mi, s, wd;
    int off_min;
} DynTimeParts;

static void dyn_time_explode(int64_t sec, int off_min, DynTimeParts *t)
{
    int64_t local = sec + (int64_t)off_min * 60;
    int64_t days = dyn_time_floor_div(local, DYN_SECS_PER_DAY);
    int64_t tod = dyn_time_floor_mod(local, DYN_SECS_PER_DAY);
    t->off_min = off_min;
    t->h = (int)(tod / 3600);
    t->mi = (int)((tod / 60) % 60);
    t->s = (int)(tod % 60);
    dyn_civil_from_days(days, &t->y, &t->mo, &t->d);
    t->wd = dyn_weekday_from_days(days);
}

/* THE emitter, one token at a time. Both callers go through it, so there is one
 * switch and it cannot drift -- and neither caller has to materialise a token
 * array it does not want.
 *
 * That last point was measured, not assumed. The first version of this had
 * formatUnix tokenise into an array and then walk it, which cost 123 -> 189 ns
 * on a 20-character layout and 876 -> 2395 ns on a 209-character one (the long
 * layout overflowed the inline array, so it tokenised three times and
 * malloc'd). Emitting during the scan is one pass for the free function and one
 * pass for the capability, with no array in between.
 *
 * 0 on success, -1 on allocation failure. */
static inline int dyn_time_emit_one(DynTimeBuf *out, uint8_t kind,
                             const char *lit, size_t litlen,
                             const DynTimeParts *t)
{
    char tmp[8];
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
        /*the RFC 3339 numeric offset of the parts -- "Z" at zero and
         * "+HH:MM"/"-HH:MM" otherwise, the exact shapes parseRFC3339 and
         * Format.parse accept back. */
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
        int v = kind == DYN_TOK_MONTH ? t->mo :
                kind == DYN_TOK_DAY   ? t->d  :
                kind == DYN_TOK_HOUR  ? t->h  :
                kind == DYN_TOK_MIN   ? t->mi : t->s;
        n = dyn_time_utoa_pad((uint64_t)v, 2, tmp);
        return dyn_time_buf_put(out, tmp, (size_t)n);
    }
    }
}

/* formatUnix keeps its own fused scan-and-emit loop -- it is NOT routed through
 * the token emitter class Format uses, and that is a measured decision.
 *
 * Sharing one emitter is the obvious way to keep the two from drifting, and it
 * was tried twice:
 *
 *   tokenise into an array, then walk it   123 -> 189 ns (20-char layout)
 *   emit during the scan via a helper      123 -> 164 ns
 *   `static inline` on both helpers        no change (163.5 either way)
 *
 * against a 123.2 ns baseline. A 33% regression on the free function to avoid
 * duplicating one switch is the wrong trade, so the switch is duplicated and
 * the two are held together by a DIFFERENTIAL instead: 51,663 (layout, time)
 * pairs in tests/test_time_format.js assert that Format.format and formatUnix
 * produce byte-identical output, over token lookalikes, literal runs, layouts
 * that overflow the inline arrays, and negative years. A test that compares
 * two implementations is a stronger guarantee than one implementation that is
 * slower. 's %z token and the {offsetMinutes} bag ride the same
 * differential (tests/test_time_offset.js).
 */
static JSValue dyn_time_format_unix(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int64_t sec;
    int64_t off_min = 0;
    int64_t y, days, tod;
    int mo, d, h, mi, s, wd;
    const char *layout;
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
    /*the third argument is the strict bag {offsetMinutes}; without it
     * the layout renders at UTC exactly as before. A bag is an object or
     * absent -- a string/number here is a caller bug, not a default. */
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        static const char *const fu_keys[] = { "offsetMinutes" };
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
        int64_t local = sec + off_min * 60;
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
        char tmp[8];
        size_t rem = llen - i;
        const char *p = layout + i;
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
            /* 's offset token, emitted exactly as dyn_time_emit_one's
             * DYN_TOK_OFFSET case does ("Z" at zero, else "+HH:MM"). */
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
    result = JS_NewStringLen(ctx, (const char *)out.data, out.len);

 done:
    if (have_buf)
        dyn_time_buf_free(&out);
    JS_FreeCString(ctx, layout);
    return result;
}

/* Defined below with the rest of the parsers; declared here because
 * Format.parse is the first caller. */
static int dyn_time_expect_digits(const char *s, size_t len, size_t pos,
                                  int n, int64_t *out);
static int dyn_time_scan_year(const char *s, size_t len, size_t *ppos,
                              int64_t *out);

/* ================================================================ *
 *  class Format -- a compiled layout
 *
 *  A compiled capability: the constructor takes CONFIGURATION (the layout) and
 *  one instance formats unbounded times. It exists because the measurement said
 *  so, not on principle -- tests/bench_time_layout.js put the layout scan at
 *  46.6% of a 20-character formatUnix call, growing at 4.0 ns per character.
 *
 *  Read-only compiled state, so it is freely reusable and needs no busy flag.
 * ================================================================ */

typedef struct {
    char *layout;
    size_t llen;
    DynTimeTok *tok;
    size_t ntok;
    int off_min;                /*the fixed UTC offset this instance
                                   renders its fields and %z at */
} DynTimeFormat;

static JSClassID dyn_time_format_class_id;

static void dyn_time_format_dispose(void *native)
{
    DynTimeFormat *f = (DynTimeFormat *)native;
    if (!f)
        return;
    free(f->layout);
    free(f->tok);
    free(f);
}

static void dyn_time_format_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_time_format_dispose(JS_GetOpaque(val, dyn_time_format_class_id));
}

static const JSClassDef dyn_time_format_class = {
    "Format",
    .finalizer = dyn_time_format_finalizer,
};

static JSValue dyn_time_format_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    DynTimeFormat *f;
    const char *layout;
    size_t llen, ntok;
    int64_t off_min = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Format(layout) requires a layout");
    /* The API contract is "requires a string": a non-string layout used to be
     * silently coerced by JS_ToCStringLen (numbers became digit literals,
     * null became the empty layout). Refuse it up front instead. */
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Format(layout): the layout must be a string");
    /*the second argument is the strict bag {offsetMinutes}; it is the
     * instance's fixed UTC offset -- what its fields and %z render at. A bag
     * is an object or absent -- a string/number here is a caller bug, not a
     * default. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char *const fmt_keys[] = { "offsetMinutes" };
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
    f = (DynTimeFormat *)malloc(sizeof(*f));
    if (!f) {
        JS_FreeCString(ctx, layout);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->llen = llen;
    f->ntok = ntok;
    f->off_min = (int)off_min;
    f->layout = (char *)malloc(llen ? llen : 1);
    f->tok = (DynTimeTok *)malloc((ntok ? ntok : 1) * sizeof(DynTimeTok));
    if (!f->layout || !f->tok) {
        dyn_time_format_dispose(f);
        JS_FreeCString(ctx, layout);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(f->layout, layout, llen);
    JS_FreeCString(ctx, layout);
    /* The whole point of the class: this runs once, not once per format(). */
    dyn_time_tokenize(f->layout, llen, f->tok, ntok);
    return dyn_plain_wrap(ctx, new_target, dyn_time_format_class_id, f,
                          dyn_time_format_dispose);
}

static JSValue dyn_time_format_format(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    DynTimeFormat *f;
    int64_t sec;
    DynTimeParts t;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    (void)argc;

    /* Coerce before resolving the handle (CLAUDE.md section 8): ToNumber can
     * run a valueOf that drops the last reference to `this`. */
    if (JS_ToInt64Ext(ctx, &sec, argv[0]))
        return JS_EXCEPTION;
    f = (DynTimeFormat *)dyn_plain_get(ctx, this_val,
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
            result = JS_NewStringLen(ctx, (const char *)out.data, out.len);
    }
    dyn_time_buf_free(&out);
    return result;
}

/* parse(str) -> unix seconds, driven by the SAME token list. The module could
 * not parse an arbitrary layout before this: parseRFC3339 handles exactly one
 * shape. Strict -- every literal must match byte for byte and every field must
 * be exactly as wide as it is formatted -- so format() and parse() round-trip.
 * A field the layout does not mention takes its value from the reference date
 * 1970-01-01T00:00:00Z. */
static JSValue dyn_time_format_parse(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    DynTimeFormat *f;
    const char *str;
    size_t slen, pos = 0, k;
    int64_t y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0, v;
    int64_t off_sec = 0;        /*the offset the fields are read at */
    int have_off = 0;
    JSValue result;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    f = (DynTimeFormat *)dyn_plain_get(ctx, this_val,
                                       dyn_time_format_class_id);
    if (!f) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    /* Without a %z in the string the fields are read AT the instance's
     * configured offset (format() rendered them there, so format/parse
     * round-trips); a %z that parses supplies its own and must be the only
     * one -- two disagreeing offsets in one string cannot both be true. */
    off_sec = (int64_t)f->off_min * 60;
    for (k = 0; k < f->ntok; k++) {
        const DynTimeTok *tk = &f->tok[k];
        int i;
        switch (tk->kind) {
        case DYN_TOK_LIT:
            if (pos + tk->len > slen ||
                memcmp(str + pos, f->layout + tk->off, tk->len) != 0)
                goto fail;
            pos += tk->len;
            break;
        case DYN_TOK_OFFSET: {
            /* The exact shapes DYN_TOK_OFFSET emits: "Z" at zero (a lowercase
             * 'z' is accepted, as parseRFC3339 accepts it), else
             * "+HH:MM"/"-HH:MM" with RFC 3339 bounds (<= +/-23:59). */
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
                goto fail;      /* two %z tokens that disagree */
            off_sec = so;
            have_off = 1;
            break;
        }
        case DYN_TOK_YEAR:
            /* EXACTLY four digits, plus an optional sign -- not the greedy
             * scan formatRFC3339's parser uses. A greedy year eats the month
             * and day of a delimiter-free layout: "2006010215:04:05" formats
             * 1970 as "1970010100:00:00", and a greedy scan reads the year as
             * 19700101 and then fails on the colon. The consequence, and it is
             * documented: a year outside -9999..9999 formats fine and does not
             * parse back. */
            {
                int neg = 0;
                if (pos < slen && str[pos] == '-') { neg = 1; pos++; }
                if (dyn_time_expect_digits(str, slen, pos, 4, &y))
                    goto fail;
                pos += 4;
                if (neg)
                    y = -y;
            }
            break;
        case DYN_TOK_MON_ABBR:
            for (i = 0; i < 12; i++)
                if (pos + 3 <= slen &&
                    memcmp(str + pos, dyn_time_month_abbr[i], 3) == 0)
                    break;
            if (i == 12)
                goto fail;
            mo = i + 1;
            pos += 3;
            break;
        case DYN_TOK_WD_ABBR:
            /* Consumed and checked for shape, but it carries no information a
             * date does not already determine, so it is not used. */
            for (i = 0; i < 7; i++)
                if (pos + 3 <= slen &&
                    memcmp(str + pos, dyn_time_weekday_abbr[i], 3) == 0)
                    break;
            if (i == 7)
                goto fail;
            pos += 3;
            break;
        default:
            if (dyn_time_expect_digits(str, slen, pos, 2, &v))
                goto fail;
            pos += 2;
            if (tk->kind == DYN_TOK_MONTH)     mo = v;
            else if (tk->kind == DYN_TOK_DAY)  d = v;
            else if (tk->kind == DYN_TOK_HOUR) h = v;
            else if (tk->kind == DYN_TOK_MIN)  mi = v;
            else                               se = v;
            break;
        }
    }
    if (pos != slen)                  /* trailing input is not a match */
        goto fail;
    /* Strict means STRICT: "2021-02-30" is refused, not normalized into
     * March -- parseRFC3339 refuses it, and a parse that silently moves an
     * impossible date makes format(parse(format(t))) lie. The days-in-month
     * check subsumes the old d > 31 bound. */
    if (mo < 1 || mo > 12 ||
        d < 1 || d > dyn_time_days_in_month(y, (int)mo) ||
        h > 23 || mi > 59 || se > 60)
        goto fail;
    JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx,
        dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY +
        h * 3600 + mi * 60 + se - off_sec);
fail:
    result = JS_ThrowSyntaxError(ctx, "input does not match the layout");
    JS_FreeCString(ctx, str);
    return result;
}

static JSValue dyn_time_format_layout(JSContext *ctx, JSValueConst this_val)
{
    DynTimeFormat *f =
        (DynTimeFormat *)dyn_plain_get(ctx, this_val,
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

/* ================================================================ *
 *  Parsing
 * ================================================================ */

/* Read exactly n ASCII digits at s[pos..pos+n) into *out; 0 on success. */
static int dyn_time_expect_digits(const char *s, size_t len, size_t pos,
                                  int n, int64_t *out)
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

/* Scan an optional '-' sign then a run of ASCII digits (>= 4 of them) as a
 * year, advancing *ppos past it; 0 on success. Unlike the month/day/time
 * fields (always exactly 2 digits), the year is variable-width and
 * possibly negative, because formatRFC3339/formatUnix's "2006" token emits
 * at least 4 digits, wider if the year needs it,
 * signed if negative -- so the parser must accept the same shape to
 * round-trip the formatter's own output. Overflow-safe: an absurdly long
 * digit run is rejected rather than silently wrapping. Years outside the
 * PlainDate range are rejected right here, so a 12-digit year cannot pass
 * the scan and overflow the caller's days * DYN_SECS_PER_DAY later. */
static int dyn_time_scan_year(const char *s, size_t len, size_t *ppos,
                              int64_t *out)
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
            return -1; /* overflow */
        v = v * 10 + (s[pos] - '0');
        if (v > DYN_TIME_MAX_YEAR)
            return -1; /* magnitude, not accumulation: the widest year we
                          ever emit is 6 digits (+275760) */
        pos++;
    }
    if (pos - start < 4)
        return -1; /* every year we ever emit has at least 4 digits */
    *out = neg ? -v : v;
    *ppos = pos;
    return 0;
}

/* parseRFC3339(str) -> {sec, nsec}. Strict: throws SyntaxError on anything
 * that doesn't fit "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)" with
 * valid calendar fields. */
static JSValue dyn_time_parse_rfc3339(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s;
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
        goto fail; /* trailing garbage */

    if (mo < 1 || mo > 12)
        goto fail;
    if (d < 1 || d > dyn_time_days_in_month(y, (int)mo))
        goto fail;
    if (h > 23 || mi > 59 || se > 60)
        goto fail; /* se==60 tolerated as a leap-second literal */

    {
        int64_t days = dyn_days_from_civil(y, (int)mo, d);
        int64_t total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se - off_sec;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            result = JS_EXCEPTION;
            goto out;
        }
        if (JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt64(ctx, total),
                                      JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, obj, "nsec",
                                      JS_NewInt32(ctx, (int32_t)nsec),
                                      JS_PROP_C_W_E) < 0) {
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

/* ================================================================ *
 *  Civil date helpers
 * ================================================================ */

/* date(y, mo, d, h=0, mi=0, s=0) -> unixSec (UTC), carrying an
 * out-of-[1,12] month into the year (see dyn_time_norm_month). */
static JSValue dyn_time_date(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
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
    /* Cap the year BEFORE days * DYN_SECS_PER_DAY: a year of 3e11 gives
     * ~1e14 days and the product overflows int64 (UB / silent wrap). The
     * same bounds PlainDate refuses, so both date surfaces agree. */
    if (y < DYN_TIME_MIN_YEAR || y > DYN_TIME_MAX_YEAR)
        return JS_ThrowRangeError(ctx, "date: year %lld is outside %d..%d",
                                  (long long)y, DYN_TIME_MIN_YEAR,
                                  DYN_TIME_MAX_YEAR);
    days = dyn_days_from_civil(y, (int)mo, d);
    total = days * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + s;
    return JS_NewInt64(ctx, total);
}

/* fromUnix(sec) -> {year, month, day, hour, min, sec, weekday, yday}. */
static JSValue dyn_time_from_unix(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
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
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "month", JS_NewInt32(ctx, mo),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "day", JS_NewInt32(ctx, d),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "hour", JS_NewInt32(ctx, h),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "min", JS_NewInt32(ctx, mi),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "sec", JS_NewInt32(ctx, s),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "weekday", JS_NewInt32(ctx, wd),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, obj, "yday", JS_NewInt32(ctx, yday),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* ---------- module registration ---------- */

/* PlainDate and Duration: dates as values (design 20). */
#include "dyna-temporal.inc.c"

/* ================================================================ *
 *  Duration builders + the PlainDateTime joiner/until.
 *  Below the include on purpose: they build Durations and PlainDateTimes,
 *  so they need tp_new_dur3 / tp_new_dt and the class ids that file defines.
 * ================================================================ */

/* durationMs(ms) -> Duration, exactly `new Duration({ milliseconds: ms })`:
 * the field semantics are the constructor's (integral value, JS_ToInt64
 * coercion -- NaN/Infinity land on 0, non-integers truncate toward zero),
 * negative allowed. A builder for the caller who has a scalar in hand and
 * does not want to spell the options object. */
static JSValue dyn_time_duration_ms(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    int64_t ms;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, ms);
}

/* durationSecs(s) -> Duration, exactly `new Duration({ seconds: s })` -- the
 * seconds fold into the millisecond field the same way the constructor folds
 * them, so fractional input truncates to whole seconds first (1.9 is 1s, NOT
 * 1.9s; pass durationMs for sub-second precision). The fold is CHECKED here
 * (the constructor's own fold is plain int64 math): s * 1000 leaving int64
 * is signed-overflow UB, so it is refused with a RangeError instead. */
static JSValue dyn_time_duration_secs(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    int64_t s;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &s, argv[0]))
        return JS_EXCEPTION;
    if (s > INT64_MAX / 1000 || s < INT64_MIN / 1000)
        return JS_ThrowRangeError(ctx, "durationSecs: %lld seconds overflows "
                                       "the Duration millisecond field",
                                  (long long)s);
    return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, s * 1000);
}

/* toPlainDateTime(date, time): the joiner for the two splitters. A pure
 * field copy -- the date and the ms-since-midnight land in a PlainDateTime
 * unchanged -- so toPlainDateTime(dt.toPlainDate(), dt.toPlainTime()) is dt
 * for every dt, and the round-trip identity is testable as an invariant
 * (tests/test_time_twins.js runs it over 1000 random date-times). */
static JSValue dyn_time_to_plain_datetime(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    tp_date_t *d;
    tp_time_t *t;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "toPlainDateTime(date, time): a PlainDate "
                                      "and a PlainTime are required");
    d = (tp_date_t *)dyn_plain_get(ctx, argv[0], dyn_pdate_class_id);
    if (!d)
        return JS_EXCEPTION;
    t = (tp_time_t *)dyn_plain_get(ctx, argv[1], dyn_ptime_class_id);
    if (!t)
        return JS_EXCEPTION;
    return tp_new_dt(ctx, JS_UNDEFINED, d->y, d->m, d->d, t->ms);
}

/* PlainDateTime.until(other) -> Duration. The PlainDate.until fold on the
 * date parts -- whole months first, the day-of-month clamp applied (the
 * fold compares b's day-of-month against a's UNCLAMPED day: 31 Jan ->
 * 28 Feb is P28D -- 0 months + 28 days -- while 31 Jan -> 3 Mar is P1M3D),
 * then the remaining
 * days -- with the time-of-day remainder folded into the Duration's
 * millisecond component.
 *
 * The day count after the month shift counts whole 24-hour days FROM THE
 * SHIFTED, DAY-CLAMPED POINT (exactly the point add() resumes from), not
 * calendar dates: b's earlier time of day costs a day. That is what makes
 * a.add(a.until(b)) === b hold exactly, which tests/test_time_twins.js
 * asserts over 1000 random pairs. The consequence, and it is documented: a
 * result may be MIXED-SIGN (months > 0 with a negative ms remainder, when
 * b falls on the very day the month shift lands on but earlier in it) --
 * correct under add()/subtract(), but toString() refuses it, as it refuses
 * every mixed-sign Duration (ISO 8601 has no representation). Exact
 * component negation between a.until(b) and b.until(a) is guaranteed only
 * while both stay inside one calendar month; across a month boundary the
 * two folds clamp different days (PlainDate.until has the same property). */
static JSValue dyn_pdt_until(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    tp_dt_t *a = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    tp_dt_t *b;
    int64_t months, ea, eb, rem, days, ms;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDateTime.until(other): a "
                                      "PlainDateTime is required");
    b = (tp_dt_t *)dyn_plain_get(ctx, argv[0], dyn_pdt_class_id);
    if (!b)
        return JS_EXCEPTION;

    /* months-first, the very arithmetic dyn_pdate_until runs on the dates */
    months = (b->d.y - a->d.y) * 12 + (b->d.m - a->d.m);
    if (months > 0 && b->d.d < a->d.d)
        months--;
    if (months < 0 && b->d.d > a->d.d)
        months++;
    {   /* epoch-ms of `a` shifted by `months` months -- day clamped to the
         * target month by the same formula tp_shift applies -- at a's time
         * of day. Bounded by the year range (|days| < 1.1e8), so the day *
         * TP_DAY_MS product cannot leave int64. */
        int64_t tm = (int64_t)(a->d.m - 1) + months;
        int64_t yy = a->d.y + (tm >= 0 ? tm / 12 : (tm - 11) / 12);
        int mm = (int)(((tm % 12) + 12) % 12) + 1;
        int dim = tp_days_in_month(yy, mm);
        int dd = a->d.d > dim ? dim : a->d.d;
        ea = tp_days_from_civil(yy, mm, dd) * TP_DAY_MS + a->ms;
    }
    eb = tp_days_from_civil(b->d.y, b->d.m, b->d.d) * TP_DAY_MS + b->ms;
    /* The remainder split into whole days + sub-day ms (C truncation: the ms
     * part carries the sign of the whole). */
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
    /* nowNanos: the same clock as nowUnixNano under the name the duration
     * side of the module spells it with. One C function, two exports. */
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


/* ================================================================
 *  class DateParser -- natural-language dates, per locale
 *
 *  STDLIB_OOP_PLAN section 13: `Date.create(string)` plus per-locale format
 *  masks, restated as a compiled capability. The CONFIGURATION is the locale;
 *  one instance parses unbounded strings.
 *
 *  It ships under clause (2) of CLAUDE.md section 14 -- it EXPRESSES something
 *  no free function here could -- rather than on a crossover. There was no
 *  natural-language date parser to be faster than. Its measured cost is
 *  published in the docs as construction versus per-parse, not as a ratio
 *  against a baseline that does not exist.
 *
 *  WHAT THE LOCALE DECIDES, and it is the whole reason a locale exists here:
 *  "03/04/2026" is 4 March in en-GB, fr, de and es, and 3 April in en-US. A
 *  parser that guessed would be wrong for one of them silently, on a value
 *  that looks perfectly well-formed. So the ORDER is configuration, and the
 *  month and weekday names are the tables that go with it.
 *
 *  The tables are built in the CONSTRUCTOR, never on first parse: a lazy build
 *  is a hidden static write and a TSan finding waiting to happen
 *  (STDLIB_OOP_PLAN section 11).
 *
 *  Read-only after construction, so no busy flag: parse() coerces its argument
 *  before touching any state, and nothing it does runs user JS.
 * ================================================================ */

#define DYN_DP_MAX_NAMES 12

typedef struct {
    const char *name;
    const char *const *months;      /* 12 full names, lower case */
    const char *const *months_abbr; /* 12 abbreviations, lower case */
    const char *const *days;        /* 7 full names from Sunday, lower case */
    int day_first;                  /* 1 = DD/MM/YYYY, 0 = MM/DD/YYYY */
} DynDateLocale;

static const char *const dyn_dp_en_months[12] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december" };
static const char *const dyn_dp_en_abbr[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec" };
static const char *const dyn_dp_en_days[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };

static const char *const dyn_dp_fr_months[12] = {
    "janvier", "fevrier", "mars", "avril", "mai", "juin",
    "juillet", "aout", "septembre", "octobre", "novembre", "decembre" };
static const char *const dyn_dp_fr_abbr[12] = {
    "jan", "fev", "mar", "avr", "mai", "jui",
    "jul", "aou", "sep", "oct", "nov", "dec" };
static const char *const dyn_dp_fr_days[7] = {
    "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" };

static const char *const dyn_dp_de_months[12] = {
    "januar", "februar", "marz", "april", "mai", "juni",
    "juli", "august", "september", "oktober", "november", "dezember" };
static const char *const dyn_dp_de_abbr[12] = {
    "jan", "feb", "mar", "apr", "mai", "jun",
    "jul", "aug", "sep", "okt", "nov", "dez" };
static const char *const dyn_dp_de_days[7] = {
    "sonntag", "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag" };

static const char *const dyn_dp_es_months[12] = {
    "enero", "febrero", "marzo", "abril", "mayo", "junio",
    "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
static const char *const dyn_dp_es_abbr[12] = {
    "ene", "feb", "mar", "abr", "may", "jun",
    "jul", "ago", "sep", "oct", "nov", "dic" };
static const char *const dyn_dp_es_days[7] = {
    "domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado" };

/* en-US is the ONLY month-first locale here, which is the point of the flag. */
static const DynDateLocale dyn_dp_locales[] = {
    { "en-US", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "en-GB", dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 1 },
    { "en",    dyn_dp_en_months, dyn_dp_en_abbr, dyn_dp_en_days, 0 },
    { "fr",    dyn_dp_fr_months, dyn_dp_fr_abbr, dyn_dp_fr_days, 1 },
    { "de",    dyn_dp_de_months, dyn_dp_de_abbr, dyn_dp_de_days, 1 },
    { "es",    dyn_dp_es_months, dyn_dp_es_abbr, dyn_dp_es_days, 1 },
};

typedef struct {
    const DynDateLocale *loc;
    int64_t base;            /* the "now" relative words resolve against */
    int has_base;            /* 0 = read the clock at each parse */
} DynDateParser;

static JSClassID dyn_dp_class_id;

static void dyn_dp_dispose(void *native)
{
    free(native);
}

static void dyn_dp_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_dp_dispose(JS_GetOpaque(val, dyn_dp_class_id));
}

static const JSClassDef dyn_dp_class = {
    "DateParser",
    .finalizer = dyn_dp_finalizer,
};

/* ---- scanning primitives. The input is folded to lower case as it is read,
 *      so the tables hold one spelling and "JULY", "July" and "july" are the
 *      same three comparisons. ---- */

static int dyn_dp_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static void dyn_dp_skip_space(const char *s, size_t len, size_t *pos)
{
    while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == ',' ||
                          s[*pos] == '.'))
        (*pos)++;
}

/* Read up to `max` digits. Returns the count read, 0 when there are none. */
static int dyn_dp_digits(const char *s, size_t len, size_t *pos, int max,
                         int64_t *out)
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

/* Match `word` (lower case, NUL-terminated) at s+pos, case-insensitively.
 * Returns its length on a match and 0 otherwise. A match must end at a
 * non-letter, so "mar" cannot swallow the start of "march". */
static size_t dyn_dp_word(const char *s, size_t len, size_t pos,
                          const char *word)
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

/* Month index (1..12) at s+pos, or 0. Full names are tried before
 * abbreviations so "march" does not stop at "mar". */
static int dyn_dp_month(const DynDateParser *p, const char *s, size_t len,
                        size_t *pos)
{
    int i;
    for (i = 0; i < 12; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->months[i]);
        if (k) { *pos += k; return i + 1; }
    }
    for (i = 0; i < 12; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->months_abbr[i]);
        if (k) { *pos += k; return i + 1; }
    }
    return 0;
}

/* Weekday index (0=Sunday..6) at s+pos, or -1. */
static int dyn_dp_weekday(const DynDateParser *p, const char *s, size_t len,
                          size_t *pos)
{
    int i;
    for (i = 0; i < 7; i++) {
        size_t k = dyn_dp_word(s, len, *pos, p->loc->days[i]);
        if (k) { *pos += k; return i; }
        /* The first three letters, which is how every one of these locales
         * abbreviates a weekday. Compared in place: the earlier version called
         * strlen and built a NUL-terminated copy on the stack for each of the
         * seven names, on every parse that reached this loop. */
        {
            const char *d = p->loc->days[i];
            if (*pos + 3 <= len &&
                dyn_dp_lower((unsigned char)s[*pos]) == d[0] &&
                dyn_dp_lower((unsigned char)s[*pos + 1]) == d[1] &&
                dyn_dp_lower((unsigned char)s[*pos + 2]) == d[2]) {
                int nx = (*pos + 3 < len)
                    ? dyn_dp_lower((unsigned char)s[*pos + 3]) : 0;
                if (!(nx >= 'a' && nx <= 'z')) { *pos += 3; return i; }
            }
        }
    }
    return -1;
}

/* Two-digit years: 69 and below are 2000s, 70 and above are 1900s -- the POSIX
 * strptime window, chosen because it is the one every other tool uses. */
static int64_t dyn_dp_expand_year(int64_t y, int digits)
{
    if (digits > 2)
        return y;
    return (y <= 69) ? 2000 + y : 1900 + y;
}

/* hh:mm[:ss] [am|pm] at s+pos. Returns 1 on a match. */
static int dyn_dp_time(const char *s, size_t len, size_t *pos,
                       int64_t *h, int64_t *mi, int64_t *se)
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
        if (*h < 12) *h += 12;
        p += 2;
    } else if (dyn_dp_word(s, len, p, "am")) {
        if (*h == 12) *h = 0;
        p += 2;
    }
    if (*h > 23 || *mi > 59 || *se > 60)
        return 0;
    *pos = p;
    return 1;
}

static int dyn_dp_valid_ymd(int64_t y, int64_t mo, int64_t d)
{
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    max = mdays[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        max = 29;
    return d <= max;
}

/* The unit table for "in 3 days" / "2 weeks ago", in seconds. Months and years
 * are NOT here: they are not a fixed number of seconds, so they are applied to
 * the calendar instead. */
static int dyn_dp_unit(const char *s, size_t len, size_t *pos, int64_t *secs,
                       int *months)
{
    static const struct { const char *w; int64_t s; int mo; } units[] = {
        { "seconds", 1, 0 }, { "second", 1, 0 }, { "secs", 1, 0 }, { "sec", 1, 0 },
        { "minutes", 60, 0 }, { "minute", 60, 0 }, { "mins", 60, 0 }, { "min", 60, 0 },
        { "hours", 3600, 0 }, { "hour", 3600, 0 }, { "hrs", 3600, 0 }, { "hr", 3600, 0 },
        { "days", 86400, 0 }, { "day", 86400, 0 },
        { "weeks", 604800, 0 }, { "week", 604800, 0 },
        { "months", 0, 1 }, { "month", 0, 1 },
        { "years", 0, 12 }, { "year", 0, 12 },
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

/* Add `n` calendar months to a unix second, clamping the day of month -- so
 * 31 January plus one month is 28 February and not 3 March. */
static int64_t dyn_dp_add_months(int64_t t, int64_t n)
{
    int64_t days = t / DYN_SECS_PER_DAY, tod = t % DYN_SECS_PER_DAY, y;
    int mo, d;
    if (tod < 0) { tod += DYN_SECS_PER_DAY; days--; }
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

/* Midnight of the day containing `t`. */
static int64_t dyn_dp_midnight(int64_t t)
{
    int64_t r = t % DYN_SECS_PER_DAY;
    if (r < 0)
        r += DYN_SECS_PER_DAY;
    return t - r;
}

/* The parse itself. Returns 0 and sets *out, or -1 when nothing matches.
 * `now` is the instant relative words resolve against. */
static int dyn_dp_parse(const DynDateParser *p, const char *s, size_t len,
                        int64_t now, int64_t *out)
{
    size_t pos = 0;
    int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, v;
    int n, have_date = 0, have_time = 0, wd;

    dyn_dp_skip_space(s, len, &pos);
    if (pos >= len)
        return -1;

    /* ---- relative words, which are a whole parse on their own ---- */
    if (dyn_dp_word(s, len, pos, "now")) { *out = now; return 0; }
    if (dyn_dp_word(s, len, pos, "today")) { *out = dyn_dp_midnight(now); return 0; }
    if (dyn_dp_word(s, len, pos, "tomorrow")) {
        *out = dyn_dp_midnight(now) + DYN_SECS_PER_DAY;
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "yesterday")) {
        *out = dyn_dp_midnight(now) - DYN_SECS_PER_DAY;
        return 0;
    }
    {   /* "in 3 days", "3 days ago", "in 2 months" */
        size_t q = pos;
        int forward = 1, months = 0;
        int64_t unit = 0;
        size_t k = dyn_dp_word(s, len, q, "in");
        if (k) { q += k; dyn_dp_skip_space(s, len, &q); }
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
                    /* Neither "in" nor "ago": "3 days" alone is not a date. */
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
    /* "next monday", "last friday" -- the nearest such weekday strictly after
     * or before today, which is what the words mean in every language here. */
    {
        size_t q = pos;
        int dir = 0;
        size_t k = dyn_dp_word(s, len, q, "next");
        if (k) { dir = 1; q += k; }
        else if ((k = dyn_dp_word(s, len, q, "last")) != 0) { dir = -1; q += k; }
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

    /* ---- absolute forms ---- */
    /* A leading weekday name is decoration: "Monday, 3 March 2026". */
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
        /* "July 28 2026" */
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
            /* ISO: 2026-07-28 */
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
                    /* "28 July 2026" */
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
                /* numeric, and the ORDER is the locale's */
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
                if (p->loc->day_first) { d = v; mo = b; }
                else { mo = v; d = b; }
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

    *out = dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY +
           h * 3600 + mi * 60 + se;
    return 0;
}

/*the strict option-bag check for dyna:time's bags (same shape as
 * the pilot in dyna-file.c). Rejects keys outside `keys` with a TypeError
 * naming the key and the valid set; an unknown key that used to be
 * silently ignored is exactly the bug this contract kills. */
static int dyn_time_opts_strict(JSContext *ctx, JSValueConst opts,
                                const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { bad = 1; break; }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) { JS_FreeCString(ctx, name); bad = 1; break; }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
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

static JSValue dyn_dp_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    DynDateParser *p;
    const char *name = NULL;
    const DynDateLocale *loc = &dyn_dp_locales[0];
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
                "en-US en-GB en fr de es", name);
            JS_FreeCString(ctx, name);
            return e;
        }
        JS_FreeCString(ctx, name);
    }
    /* An explicit `now` makes every relative word deterministic, which is what
     * lets a test assert "tomorrow" without racing the clock.
     * {now} is the whole bag; a typo'd key used to be silently
     * dropped and the parser raced the real clock. A bag is an object or
     * absent -- a string/number here is a caller bug, not a default. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char *const dp_keys[] = { "now" };
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "DateParser: options must be an object");
        if (dyn_time_opts_strict(ctx, argv[1], dp_keys, 1))
            return JS_EXCEPTION;
        JSValue nv = JS_GetPropertyStr(ctx, argv[1], "now");
        if (JS_IsException(nv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(nv)) {
            double dv;
            if (JS_ToFloat64(ctx, &dv, nv)) {
                JS_FreeValue(ctx, nv);
                return JS_EXCEPTION;
            }
            base = (int64_t)dv;
            has_base = 1;
        }
        JS_FreeValue(ctx, nv);
    }

    p = (DynDateParser *)malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->loc = loc;
    p->base = base;
    p->has_base = has_base;

    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) { free(p); return JS_EXCEPTION; }
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_dp_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { free(p); return obj; }
    JS_SetOpaque(obj, p);
    return obj;
}

/* parse(text) -> unix seconds, or null when nothing matches.
 *
 * null rather than a throw, because this parses text a HUMAN typed: "not a
 * date I recognise" is an ordinary outcome of that, and a caller who wants an
 * error can write one at the site where the failure means something. */
static JSValue dyn_dp_parse_method(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DynDateParser *p;
    const char *s;
    size_t len;
    int64_t out = 0, now;
    int rc;

    /* Coerce first: ToString runs user JS, and this object is a plain GC class
     * with nothing to free, but the ORDER is the rule either way. */
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "parse(text) requires an argument");
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    p = (DynDateParser *)JS_GetOpaque2(ctx, this_val, dyn_dp_class_id);
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

static JSValue dyn_dp_locale_get(JSContext *ctx, JSValueConst this_val)
{
    DynDateParser *p = (DynDateParser *)JS_GetOpaque2(ctx, this_val,
                                                      dyn_dp_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewString(ctx, p->loc->name);
}

static JSValue dyn_dp_dayfirst_get(JSContext *ctx, JSValueConst this_val)
{
    DynDateParser *p = (DynDateParser *)JS_GetOpaque2(ctx, this_val,
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

static int dyn_time_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_time_format_class_id,
                                 &dyn_time_format_class, dyn_time_format_proto,
                                 countof(dyn_time_format_proto),
                                 dyn_time_format_ctor, "Format") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_dp_class_id, &dyn_dp_class,
                                 dyn_dp_proto, countof(dyn_dp_proto),
                                 dyn_dp_ctor, "DateParser") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pdate_class_id, &dyn_pdate_class,
                                 dyn_pdate_proto, countof(dyn_pdate_proto),
                                 dyn_pdate_ctor, "PlainDate") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_dur_class_id, &dyn_dur_class,
                                 dyn_dur_proto, countof(dyn_dur_proto),
                                 dyn_dur_ctor, "Duration") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ptime_class_id, &dyn_ptime_class,
                                 dyn_ptime_proto, countof(dyn_ptime_proto),
                                 dyn_ptime_ctor, "PlainTime") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pdt_class_id, &dyn_pdt_class,
                                 dyn_pdt_proto, countof(dyn_pdt_proto),
                                 dyn_pdt_ctor, "PlainDateTime") < 0)
        return -1;
    {   /* PlainDateTime.until lives in dyna-time.c (this file) rather than
         * dyna-temporal.inc.c: it folds months exactly the way
         * dyn_pdate_until does, but its day/ms remainder arithmetic is
         * epoch-millisecond math on tp_dt_t, and keeping it beside that
         * struct's sibling surfaces (toPlainDateTime below) keeps the whole
         * joiner/differ pair in one place. dyn_register_plain_class has
         * no post-registration method list, so the method is attached through
         * the live prototype -- the same shape dyna:ml uses for CSR.fromDense
         * (id is module-static, so the registration above must have set it). */
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

int js_nat_init_time(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:time", dyn_time_init_module);
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

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TIME */
