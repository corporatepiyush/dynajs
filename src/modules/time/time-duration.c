#include "time-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#define DYN_TIME_DUR_BUF 64

JSValue dyn_time_duration_string(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_time_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t ms;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64Ext(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    return tp_new_dur3(ctx, JS_UNDEFINED, 0, 0, ms);
}

JSValue dyn_time_duration_secs(JSContext* ctx, JSValueConst this_val,
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

#endif
