#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

int dyn_parse_size(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 1) || d > 1099511627776.0) {
            JS_ThrowRangeError(ctx, "Logger rollover.size: out of range");
            return -1;
        }
        *out = (uint64_t)d;
        return 0;
    }
    if (JS_IsString(v)) {
        const char* src = JS_ToCString(ctx, v);
        const char* cs = src;
        uint64_t n = 0;
        int digits = 0;
        if (!cs)
            return -1;
        while (*cs >= '0' && *cs <= '9') {
            uint64_t d = (uint64_t)(*cs - '0');
            if (n > (1099511627776ull - d) / 10ull) {
                JS_FreeCString(ctx, src);
                JS_ThrowRangeError(ctx, "Logger rollover.size: out of range");
                return -1;
            }
            n = n * 10 + d;
            cs++;
            digits++;
        }
        if (digits && (*cs == 'k' || *cs == 'K' || *cs == 'm' || *cs == 'M' || *cs == 'g' || *cs == 'G')) {
            uint64_t mult = (*cs == 'k' || *cs == 'K') ? 1024ull
                : (*cs == 'm' || *cs == 'M')           ? 1024ull * 1024
                                                       : 1024ull * 1024 * 1024;
            if (n > 1099511627776ull / mult) {
                JS_FreeCString(ctx, src);
                JS_ThrowRangeError(ctx, "Logger rollover.size: out of range");
                return -1;
            }
            cs++;
            n *= mult;
        }
        if (digits && (*cs == 'b' || *cs == 'B'))
            cs++;
        if (!digits || *cs != '\0' || n < 1) {
            JS_FreeCString(ctx, src);
            JS_ThrowRangeError(ctx,
                "Logger rollover.size: use bytes or \"500k\"/\"10m\"/\"2g\"");
            return -1;
        }
        JS_FreeCString(ctx, src);
        *out = n;
        return 0;
    }
    JS_ThrowRangeError(ctx, "Logger rollover.size: use bytes or \"500k\"/\"10m\"/\"2g\"");
    return -1;
}

int dyn_parse_frequency(JSContext* ctx, JSValueConst v, int64_t* out,
    int* named_date)
{
    if (JS_IsString(v)) {
        const char* cs = JS_ToCString(ctx, v);
        if (!cs)
            return -1;
        if (strcmp(cs, "daily") == 0) {
            *out = 86400000;
            *named_date = 1;
        } else if (strcmp(cs, "hourly") == 0) {
            *out = 3600000;
            *named_date = 1;
        } else {
            JS_FreeCString(ctx, cs);
            JS_ThrowRangeError(ctx,
                "Logger rollover.frequency: use \"daily\", \"hourly\" or milliseconds");
            return -1;
        }
        JS_FreeCString(ctx, cs);
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 1) || d > 3153600000000.0) {
            JS_ThrowRangeError(ctx, "Logger rollover.frequency: out of range");
            return -1;
        }
        *out = (int64_t)d;
        *named_date = 0;
        return 0;
    }
    JS_ThrowRangeError(ctx,
        "Logger rollover.frequency: use \"daily\", \"hourly\" or milliseconds");
    return -1;
}

#endif
