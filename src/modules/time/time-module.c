#include "time-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

static void dyn_time_norm_month(int64_t* y, int64_t* mo)
{
    int64_t m0 = *mo - 1;
    int64_t yshift = dyn_time_floor_div(m0, 12);
    *y += yshift;
    *mo = dyn_time_floor_mod(m0, 12) + 1;
}

static const char dyn_time_d2[201] = "00010203040506070809101112131415161718192021222324"
                                     "25262728293031323334353637383940414243444546474849"
                                     "50515253545556575859606162636465666768697071727374"
                                     "75767778798081828384858687888990919293949596979899";

int dyn_time_utoa_pad(uint64_t v, int width, char* out)
{
    char tmp[24];
    char* e = tmp + sizeof tmp;
    char* p = e;
    int n;
    while (v >= 100) {
        unsigned r = (unsigned)(v % 100);
        v /= 100;
        p -= 2;
        memcpy(p, dyn_time_d2 + 2 * r, 2);
    }
    if (v >= 10) {
        p -= 2;
        memcpy(p, dyn_time_d2 + 2 * v, 2);
    } else {
        *--p = (char)('0' + (int)v);
    }
    n = (int)(e - p);
    if (n < width) {
        memset(out, '0', (size_t)(width - n));
        memcpy(out + (width - n), p, (size_t)n);
        return width;
    }
    memcpy(out, p, (size_t)n);
    return n;
}

int dyn_time_utoa(uint64_t v, char* out)
{
    return dyn_time_utoa_pad(v, 0, out);
}

int dyn_time_buf_reserve(DynTimeBuf* b, size_t extra)
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

const char* const dyn_time_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

const char* const dyn_time_weekday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

int dyn_time_read_int(JSContext* ctx, JSValueConst v, const char* name,
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

int dyn_time_read_offset(JSContext* ctx, JSValueConst opts,
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

JSClassID dyn_time_format_class_id;

static const JSClassDef dyn_time_format_class = {
    "Format",
    .finalizer = dyn_time_format_finalizer,
};

static const JSCFunctionListEntry dyn_time_format_proto[] = {
    JS_CFUNC_DEF("format", 1, dyn_time_format_format),
    JS_CFUNC_DEF("parse", 1, dyn_time_format_parse),
    JS_CGETSET_DEF("layout", dyn_time_format_layout, NULL),
};

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

JSClassID dyn_dp_class_id;

static const JSClassDef dyn_dp_class = {
    "DateParser",
    .finalizer = dyn_dp_finalizer,
};

static const JSCFunctionListEntry dyn_dp_proto[] = {
    JS_CFUNC_DEF("parse", 1, dyn_dp_parse_method),
    JS_CGETSET_DEF("locale", dyn_dp_locale_get, NULL),
    JS_CGETSET_DEF("dayFirst", dyn_dp_dayfirst_get, NULL),
};

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
