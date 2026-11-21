#include "time-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

typedef struct {
    JSContext* ctx;
    JSValue arr;
    int64_t limit;
    int64_t n;
    int failed;
} DynRRuleCollect;

typedef struct {
    JSContext* ctx;
    JSValue arr;
    int64_t start, end;
    int inc;
    int64_t n;
    int failed;
    int done;
} DynRRuleWindow;

typedef struct {
    int64_t from;
    int64_t res;
    int found;
} DynRRuleNext;

typedef struct {
    int64_t from;
    int64_t res;
    int have;
    int stop;
    int done;
} DynRRulePrev;

static const char* const dyn_rrule_freq_names[7] = {
    "YEARLY", "MONTHLY", "WEEKLY", "DAILY", "HOURLY", "MINUTELY", "SECONDLY"
};

static const char dyn_rrule_wd_names[7][2] = {
    { 'M', 'O' }, { 'T', 'U' }, { 'W', 'E' }, { 'T', 'H' },
    { 'F', 'R' }, { 'S', 'A' }, { 'S', 'U' }
};

static int dyn_rrule_date_epoch(JSContext* ctx, JSValueConst v, int64_t* out)
{
    JSValue fn = JS_GetPropertyStr(ctx, v, "getTime");
    JSValue ms;
    double d;
    if (JS_IsException(fn))
        return -1;
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        JS_ThrowTypeError(ctx, "dyna:time: not a Date");
        return -1;
    }
    ms = JS_Call(ctx, fn, v, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(ms))
        return -1;
    if (JS_ToFloat64(ctx, &d, ms)) {
        JS_FreeValue(ctx, ms);
        return -1;
    }
    JS_FreeValue(ctx, ms);
    if (!(d >= -8640000000000000.0 && d <= 8640000000000000.0)) {
        JS_ThrowTypeError(ctx, "dyna:time: invalid Date value");
        return -1;
    }
    *out = dyn_time_floor_div((int64_t)d, 1000);
    return 0;
}

static int dyn_rrule_to_epoch(JSContext* ctx, JSValueConst v, int64_t* out,
    const char* what)
{
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= -(double)DYN_MAX_SAFE_INT && d <= (double)DYN_MAX_SAFE_INT) || d != (double)(int64_t)d) {
            JS_ThrowRangeError(ctx,
                "dyna:time: %s must be whole unix seconds", what);
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    }
    if (JS_IsBigInt(ctx, v))
        return JS_ToInt64Ext(ctx, out, v);
    if (JS_IsObject(v)) {
        JSValue fn = JS_GetPropertyStr(ctx, v, "getTime");
        int is_date;
        if (JS_IsException(fn))
            return -1;
        is_date = JS_IsFunction(ctx, fn);
        JS_FreeValue(ctx, fn);
        if (is_date)
            return dyn_rrule_date_epoch(ctx, v, out);
    }
    if (JS_IsString(v)) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        int rc;
        if (!s)
            return -1;
        rc = dyn_rrule_parse_datetime(s, len, out);
        JS_FreeCString(ctx, s);
        if (rc) {
            JS_ThrowSyntaxError(ctx, "dyna:time: invalid date for %s", what);
            return -1;
        }
        return 0;
    }
    JS_ThrowTypeError(ctx,
        "dyna:time: %s must be a Date, an ISO date string, or unix seconds",
        what);
    return -1;
}

static JSClassID dyn_rrule_class_id;

static void dyn_rrule_dispose(void* native)
{
    dyn_nat_free(native);
}

static void dyn_rrule_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_rrule_dispose(JS_GetOpaque(val, dyn_rrule_class_id));
}

static const JSClassDef dyn_rrule_class = {
    "RRule",
    .finalizer = dyn_rrule_finalizer,
};

static int dyn_rrule_name_eq(const char* s, size_t len, const char* name)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (!name[i])
            return 0;
        if (dyn_rrule_upper(s[i]) != name[i])
            return 0;
    }
    return name[i] == 0;
}

static int dyn_rrule_freq_by_name(const char* s, size_t len)
{
    int f;
    for (f = 0; f < 7; f++)
        if (dyn_rrule_name_eq(s, len, dyn_rrule_freq_names[f]))
            return f;
    return -1;
}

static int dyn_rrule_wd_by_name(const char* s, size_t len)
{
    int w;
    if (len != 2)
        return -1;
    for (w = 0; w < 7; w++)
        if (dyn_rrule_upper(s[0]) == dyn_rrule_wd_names[w][0] && dyn_rrule_upper(s[1]) == dyn_rrule_wd_names[w][1])
            return w;
    return -1;
}

static int dyn_rrule_parse_wd_str(const char* p, size_t len, int* wd, int* n)
{
    size_t i = 0;
    int sign = 1, has_num = 0, num = 0;
    *n = 0;
    if (i < len && (p[i] == '+' || p[i] == '-')) {
        sign = (p[i] == '-') ? -1 : 1;
        i++;
    }
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (num > 53)
            return -1;
        num = num * 10 + (p[i] - '0');
        i++;
        has_num = 1;
    }
    if (i + 2 > len)
        return -1;
    *wd = dyn_rrule_wd_by_name(p + i, 2);
    if (*wd < 0)
        return -1;
    i += 2;
    if (i == len) {
        if (has_num && num == 0)
            return -1;
        *n = has_num ? num * sign : 0;
        return 0;
    }
    if (has_num)
        return -1;
    if (i >= len || p[i] != '(')
        return -1;
    i++;
    sign = 1;
    if (i < len && (p[i] == '+' || p[i] == '-')) {
        sign = (p[i] == '-') ? -1 : 1;
        i++;
    }
    if (i >= len || p[i] < '0' || p[i] > '9')
        return -1;
    num = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (num > 53)
            return -1;
        num = num * 10 + (p[i] - '0');
        i++;
    }
    if (num == 0 || i >= len || p[i] != ')')
        return -1;
    i++;
    if (i != len)
        return -1;
    *n = num * sign;
    return 0;
}

static int dyn_rrule_spec_add_weekday(JSContext* ctx, DynRRuleSpec* s,
    int wd, int n)
{
    int k;
    if (n == 0)
        return dyn_rrule_set_add(s->byweekday, &s->n_byweekday,
            DYN_RRULE_WD_CAP, wd);
    if (n < -53 || n > 53) {
        JS_ThrowRangeError(ctx, "RRule: BYDAY ordinal out of range");
        return -1;
    }
    if (s->freq > DYN_RRULE_MONTHLY)
        return dyn_rrule_set_add(s->byweekday, &s->n_byweekday,
            DYN_RRULE_WD_CAP, wd);
    for (k = 0; k < s->n_bynwd; k++)
        if (s->bynwd[k * 2] == wd && s->bynwd[k * 2 + 1] == n)
            return 0;
    if (s->n_bynwd >= DYN_RRULE_BYNWD_CAP) {
        JS_ThrowRangeError(ctx, "RRule: too many BYDAY ordinals");
        return -1;
    }
    s->bynwd[s->n_bynwd * 2] = (int16_t)wd;
    s->bynwd[s->n_bynwd * 2 + 1] = (int16_t)n;
    s->n_bynwd++;
    return 0;
}

static int dyn_rrule_spec_apply_list(JSContext* ctx, DynRRuleSpec* s, int which,
    const int64_t* vals, int n,
    const char* name)
{
    int i;
    for (i = 0; i < n; i++) {
        int64_t v = vals[i];
        int rc = 0;
        switch (which) {
        case DYN_RR_LIST_BYMONTH:
            if (v < 1 || v > 12)
                goto bad;
            rc = dyn_rrule_set_add(s->bymonth, &s->n_bymonth,
                DYN_RRULE_MONTH_CAP, (int)v);
            break;
        case DYN_RR_LIST_BYMONTHDAY:
            if (v == 0 || v < -31 || v > 31)
                goto bad;
            if (v > 0)
                rc = dyn_rrule_set_add(s->bymonthday, &s->n_bymonthday,
                    DYN_RRULE_MDAY_CAP, (int)v);
            else
                rc = dyn_rrule_set_add(s->bynmonthday, &s->n_bynmonthday,
                    DYN_RRULE_MDAY_CAP, (int)v);
            break;
        case DYN_RR_LIST_BYYEARDAY:
            if (v == 0 || v < -366 || v > 366)
                goto bad;
            rc = dyn_rrule_set_add(s->byyearday, &s->n_byyearday,
                DYN_RRULE_YDAY_CAP, (int)v);
            break;
        case DYN_RR_LIST_BYWEEKNO:
            if (v == 0 || v < -53 || v > 53)
                goto bad;
            rc = dyn_rrule_set_add(s->byweekno, &s->n_byweekno,
                DYN_RRULE_WKNO_CAP, (int)v);
            break;
        default:
            if (v == 0 || v < -366 || v > 366)
                goto bad;
            if (s->n_bysetpos >= DYN_RRULE_SETPOS_CAP)
                goto full;
            s->bysetpos[s->n_bysetpos++] = (int16_t)v;
            continue;
        }
        if (rc)
            goto full;
    }
    return 0;
bad:
    JS_ThrowRangeError(ctx, "RRule: invalid %s value", name);
    return -1;
full:
    JS_ThrowRangeError(ctx, "RRule: too many %s values", name);
    return -1;
}

static int dyn_rrule_read_int_list(JSContext* ctx, JSValueConst v,
    int64_t* vals, int cap)
{
    int n = 0;
    if (JS_IsArray(ctx, v)) {
        JSValue lv;
        uint32_t len, i;
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv))
            return -1;
        if (JS_ToUint32(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        if (len > (uint32_t)cap)
            return JS_ThrowRangeError(ctx, "RRule: list too long"), -1;
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            int64_t x;
            if (JS_IsException(e))
                return -1;
            if (JS_ToInt64Ext(ctx, &x, e)) {
                JS_FreeValue(ctx, e);
                return -1;
            }
            JS_FreeValue(ctx, e);
            vals[n++] = x;
        }
        return n;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_ToInt64Ext(ctx, &vals[0], v))
            return -1;
        return 1;
    }
    return 0;
}

static int dyn_rrule_weekday_value(JSContext* ctx, JSValueConst v,
    DynRRuleSpec* s)
{
    if (JS_IsNumber(v)) {
        double d;
        int64_t x;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 0) || d > 6.0 || d != (double)(int64_t)d) {
            JS_ThrowRangeError(ctx,
                "RRule: byweekday number must be an integer 0..6 (0=MO)");
            return -1;
        }
        x = (int64_t)d;
        return dyn_rrule_spec_add_weekday(ctx, s, (int)x, 0);
    }
    if (JS_IsString(v)) {
        size_t len;
        const char* str;
        int wd, n;
        str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        if (dyn_rrule_parse_wd_str(str, len, &wd, &n) || dyn_rrule_spec_add_weekday(ctx, s, wd, n)) {
            JS_ThrowRangeError(ctx, "RRule: invalid byweekday item \"%.*s\"",
                (int)len, str);
            JS_FreeCString(ctx, str);
            return -1;
        }
        JS_FreeCString(ctx, str);
        return 0;
    }
    JS_ThrowTypeError(ctx, "RRule: byweekday items must be strings or numbers");
    return -1;
}

static int dyn_rrule_read_weekdays(JSContext* ctx, JSValueConst v,
    DynRRuleSpec* s)
{
    if (JS_IsArray(ctx, v)) {
        JSValue lv;
        uint32_t len, i;
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv))
            return -1;
        if (JS_ToUint32(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        if (len > (uint32_t)(DYN_RRULE_WD_CAP + DYN_RRULE_BYNWD_CAP))
            return JS_ThrowRangeError(ctx, "RRule: byweekday list too long"),
                   -1;
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            int rc;
            if (JS_IsException(e))
                return -1;
            rc = dyn_rrule_weekday_value(ctx, e, s);
            JS_FreeValue(ctx, e);
            if (rc)
                return -1;
        }
        return 0;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        return dyn_rrule_weekday_value(ctx, v, s);
    return 0;
}

static JSValue dyn_rrule_build(JSContext* ctx, JSValueConst new_target,
    const DynRRuleSpec* s)
{
    DynRRule* rr;
    int64_t bad_year = 0;
    int rc;

    if (!s->has_freq) {
        JS_ThrowTypeError(ctx, "RRule: freq is required "
                               "(YEARLY|MONTHLY|WEEKLY|DAILY|HOURLY|"
                               "MINUTELY|SECONDLY)");
        return JS_EXCEPTION;
    }
    rr = (DynRRule*)dyn_nat_malloc(sizeof(*rr));
    if (!rr)
        return JS_ThrowOutOfMemory(ctx);
    rc = dyn_rrule_compile(rr, s, (int64_t)time(NULL), &bad_year);
    if (rc) {
        dyn_nat_free(rr);
        return JS_ThrowRangeError(ctx, rc == DYN_RRULE_E_DTSTART_YEAR
                ? "RRule: dtstart year %lld is outside 0001..9999"
                : "RRule: until year %lld is outside 0001..9999",
            (long long)bad_year);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_rrule_class_id, rr, dyn_rrule_dispose);
}

static int dyn_rrule_collect_cb(int64_t sec, void* udp)
{
    DynRRuleCollect* c = (DynRRuleCollect*)udp;
    JSValue d;
    if (c->n >= (int64_t)DYN_RRULE_MAX_RESULTS) {
        JS_ThrowRangeError(c->ctx,
            "RRule.all(): more than %u occurrences; use between() or a limit",
            (unsigned)DYN_RRULE_MAX_RESULTS);
        c->failed = 1;
        return 1;
    }
    if (c->limit >= 0 && c->n >= c->limit)
        return 1;
    d = JS_NewDate(c->ctx, (double)sec * 1000.0);
    if (JS_IsException(d)) {
        c->failed = 1;
        return 1;
    }
    if (JS_DefinePropertyValueUint32(c->ctx, c->arr, (uint32_t)c->n, d, JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    c->n++;
    return 0;
}

static JSValue dyn_rrule_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRule* rr;
    DynRRuleCollect c;
    DynRRuleGen g;
    int64_t limit = -1;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, argv[0]))
            return JS_EXCEPTION;
        if (!(dv >= 0) || dv > (double)DYN_MAX_SAFE_INT || dv != (double)(int64_t)dv)
            return JS_ThrowRangeError(ctx,
                "RRule.all(limit) requires a non-negative integer");
        limit = (int64_t)dv;
    }
    rr = (DynRRule*)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (limit < 0 && !rr->has_count && !rr->has_until)
        return JS_ThrowRangeError(ctx,
            "RRule.all(): this rule has neither COUNT nor UNTIL, so it is "
            "infinite; pass an explicit limit to all()");
    if (limit == 0)
        return JS_NewArray(ctx);

    c.ctx = ctx;
    c.arr = JS_NewArray(ctx);
    if (JS_IsException(c.arr))
        return JS_EXCEPTION;
    c.limit = limit;
    c.n = 0;
    c.failed = 0;
    dyn_rrule_gen_init(&g, rr, rr->dtstart);
    dyn_rrule_gen_loop(&g, dyn_rrule_collect_cb, &c);
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    if (!c.failed && g.exhausted)
        return JS_FreeValue(ctx, c.arr),
               JS_ThrowRangeError(ctx,
                   "RRule.all(): recurrence budget exhausted (%s); "
                   "collected %lld of %lld results -- narrow the rule or raise "
                   "the limit",
                   dyn_rrule_limit_desc(&g), (long long)c.n,
                   (long long)(limit < 0 ? c.n : limit));
    return c.arr;
}

static int dyn_rrule_window_cb(int64_t sec, void* udp)
{
    DynRRuleWindow* w = (DynRRuleWindow*)udp;
    JSValue d;
    if (w->inc) {
        if (sec > w->end) {
            w->done = 1;
            return 1;
        }
        if (sec < w->start)
            return 0;
    } else {
        if (sec >= w->end) {
            w->done = 1;
            return 1;
        }
        if (sec <= w->start)
            return 0;
    }
    if (w->n >= (int64_t)DYN_RRULE_MAX_RESULTS) {
        JS_ThrowRangeError(w->ctx,
            "RRule.between(): more than %u results in the window",
            (unsigned)DYN_RRULE_MAX_RESULTS);
        w->failed = 1;
        return 1;
    }
    d = JS_NewDate(w->ctx, (double)sec * 1000.0);
    if (JS_IsException(d)) {
        w->failed = 1;
        return 1;
    }
    if (JS_DefinePropertyValueUint32(w->ctx, w->arr, (uint32_t)w->n, d, JS_PROP_C_W_E) < 0) {
        w->failed = 1;
        return 1;
    }
    w->n++;
    return 0;
}

static JSValue dyn_rrule_between(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRule* rr;
    DynRRuleWindow w;
    DynRRuleGen g;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "RRule.between(start, end[, inc]) requires two dates");
    w.ctx = ctx;
    w.n = 0;
    w.failed = 0;
    w.done = 0;
    w.inc = 0;
    if (dyn_rrule_to_epoch(ctx, argv[0], &w.start, "between start") || dyn_rrule_to_epoch(ctx, argv[1], &w.end, "between end"))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        int b = JS_ToBool(ctx, argv[2]);
        if (b < 0)
            return JS_EXCEPTION;
        w.inc = b;
    }
    rr = (DynRRule*)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;

    w.arr = JS_NewArray(ctx);
    if (JS_IsException(w.arr))
        return JS_EXCEPTION;
    dyn_rrule_gen_seek(&g, rr, w.start);
    if (dyn_time_floor_div(w.end, DYN_SECS_PER_DAY) < g.stop_day)
        g.stop_day = dyn_time_floor_div(w.end, DYN_SECS_PER_DAY);
    if (!rr->has_count && !rr->n_bysetpos) {
        g.bounded = 1;
        g.lo_day = dyn_time_floor_div(w.start, DYN_SECS_PER_DAY);
        g.hi_day = dyn_time_floor_div(w.end, DYN_SECS_PER_DAY);
    }
    dyn_rrule_gen_loop(&g, dyn_rrule_window_cb, &w);
    if (w.failed) {
        JS_FreeValue(ctx, w.arr);
        return JS_EXCEPTION;
    }
    if (g.exhausted && !w.done) {
        JS_FreeValue(ctx, w.arr);
        return JS_ThrowRangeError(ctx,
            "RRule.between(): recurrence budget exhausted (%s); "
            "the window is incomplete -- narrow it or raise the limit",
            dyn_rrule_limit_desc(&g));
    }
    return w.arr;
}

static int dyn_rrule_next_cb(int64_t sec, void* udp)
{
    DynRRuleNext* nx = (DynRRuleNext*)udp;
    if (sec > nx->from) {
        nx->res = sec;
        nx->found = 1;
        return 1;
    }
    return 0;
}

static JSValue dyn_rrule_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRule* rr;
    DynRRuleNext nx;
    DynRRuleGen g;

    nx.from = -1;
    nx.found = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && dyn_rrule_to_epoch(ctx, argv[0], &nx.from, "next fromDate"))
        return JS_EXCEPTION;
    rr = (DynRRule*)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        nx.from = rr->dtstart;

    dyn_rrule_gen_seek(&g, rr, nx.from);
    dyn_rrule_gen_loop(&g, dyn_rrule_next_cb, &nx);
    if (nx.found)
        return JS_NewDate(ctx, (double)nx.res * 1000.0);
    if (g.exhausted)
        return JS_ThrowRangeError(ctx,
            "RRule.next(): recurrence budget exhausted (%s); the "
            "search is incomplete -- narrow the query or raise the limit",
            dyn_rrule_limit_desc(&g));
    return JS_NULL;
}

static int dyn_rrule_prev_cb(int64_t sec, void* udp)
{
    DynRRulePrev* pv = (DynRRulePrev*)udp;
    if (pv->stop) {
        if (sec >= pv->from) {
            pv->done = 1;
            return 1;
        }
    }
    pv->res = sec;
    pv->have = 1;
    return 0;
}

static JSValue dyn_rrule_prev(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRule* rr;
    DynRRulePrev pv;
    DynRRuleGen g;

    pv.have = 0;
    pv.res = 0;
    pv.stop = 0;
    pv.done = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (dyn_rrule_to_epoch(ctx, argv[0], &pv.from, "prev fromDate"))
            return JS_EXCEPTION;
        pv.stop = 1;
    }
    rr = (DynRRule*)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (!pv.stop) {
        if (!rr->has_count && !rr->has_until)
            return JS_NULL;
        pv.from = 0;
    }
    dyn_rrule_gen_init(&g, rr, rr->dtstart);
    if (pv.stop && dyn_time_floor_div(pv.from, DYN_SECS_PER_DAY) < g.stop_day)
        g.stop_day = dyn_time_floor_div(pv.from, DYN_SECS_PER_DAY);
    dyn_rrule_gen_loop(&g, dyn_rrule_prev_cb, &pv);
    if (g.exhausted && !pv.done)
        return JS_ThrowRangeError(ctx,
            "RRule.prev(): recurrence budget exhausted (%s); the "
            "search is incomplete -- narrow the query or raise the limit",
            dyn_rrule_limit_desc(&g));
    if (pv.have)
        return JS_NewDate(ctx, (double)pv.res * 1000.0);
    return JS_NULL;
}

static void dyn_rrule_fmt_until(int64_t sec, char* out)
{
    int64_t days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    int64_t tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    int64_t y;
    int mo, d;
    int pos = 0;
    dyn_civil_from_days(days, &y, &mo, &d);
    pos += dyn_time_utoa_pad((uint64_t)y, 4, out + pos);
    pos += dyn_time_utoa_pad((uint64_t)mo, 2, out + pos);
    pos += dyn_time_utoa_pad((uint64_t)d, 2, out + pos);
    out[pos++] = 'T';
    pos += dyn_time_utoa_pad((uint64_t)(tod / 3600), 2, out + pos);
    pos += dyn_time_utoa_pad((uint64_t)((tod / 60) % 60), 2, out + pos);
    pos += dyn_time_utoa_pad((uint64_t)(tod % 60), 2, out + pos);
    out[pos++] = 'Z';
    out[pos] = 0;
}

static int dyn_rrule_put_join(DynTimeBuf* out, const int16_t* a, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int64_t v = a[i];
        char tmp[16];
        int pos = 0;
        if (i > 0 && dyn_time_buf_put(out, ",", 1))
            return -1;
        if (v < 0) {
            if (dyn_time_buf_put(out, "-", 1))
                return -1;
            v = -v;
        }
        pos = dyn_time_utoa((uint64_t)v, tmp);
        if (dyn_time_buf_put(out, tmp, (size_t)pos))
            return -1;
    }
    return 0;
}

static JSValue dyn_rrule_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRule* rr;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    (void)argc;
    (void)argv;

    rr = (DynRRule*)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (dyn_time_buf_init(ctx, &out, 64))
        return JS_EXCEPTION;

#define PUT(s)                                      \
    do {                                            \
        if (dyn_time_buf_put(&out, (s), strlen(s))) \
            goto done;                              \
    } while (0)
#define PUTN(s, n)                            \
    do {                                      \
        if (dyn_time_buf_put(&out, (s), (n))) \
            goto done;                        \
    } while (0)
    {
        char tmp[32];
        int pos;
        PUT("RRULE:FREQ=");
        PUTN(dyn_rrule_freq_names[rr->freq],
            strlen(dyn_rrule_freq_names[rr->freq]));
        if (rr->interval != 1) {
            PUT(";INTERVAL=");
            pos = dyn_time_utoa(rr->interval, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)pos))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_WKST && rr->wkst != 0) {
            PUT(";WKST=");
            PUTN(dyn_rrule_wd_names[rr->wkst], 2);
        }
        if (rr->has_count) {
            PUT(";COUNT=");
            pos = dyn_time_utoa((uint64_t)rr->count, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)pos))
                goto done;
        }
        if (rr->has_until) {
            char ubuf[32];
            dyn_rrule_fmt_until(rr->until, ubuf);
            PUT(";UNTIL=");
            if (dyn_time_buf_put(&out, ubuf, strlen(ubuf)))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYSETPOS) {
            PUT(";BYSETPOS=");
            if (dyn_rrule_put_join(&out, rr->bysetpos, rr->n_bysetpos))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYMONTH) {
            PUT(";BYMONTH=");
            if (dyn_rrule_put_join(&out, rr->bymonth, rr->n_bymonth))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYMONTHDAY) {
            int npos = rr->n_bymonthday, nneg = rr->n_bynmonthday;
            PUT(";BYMONTHDAY=");
            if (npos > 0 && dyn_rrule_put_join(&out, rr->bymonthday, npos))
                goto done;
            if (npos > 0 && nneg > 0 && dyn_time_buf_put(&out, ",", 1))
                goto done;
            if (nneg > 0 && dyn_rrule_put_join(&out, rr->bynmonthday, nneg))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYYEARDAY) {
            PUT(";BYYEARDAY=");
            if (dyn_rrule_put_join(&out, rr->byyearday, rr->n_byyearday))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYWEEKNO) {
            PUT(";BYWEEKNO=");
            if (dyn_rrule_put_join(&out, rr->byweekno, rr->n_byweekno))
                goto done;
        }
        if (rr->orig & DYN_RR_ORIG_BYWEEKDAY) {
            int i;
            PUT(";BYDAY=");
            for (i = 0; i < rr->n_byweekday; i++) {
                if (i > 0 && dyn_time_buf_put(&out, ",", 1))
                    goto done;
                if (dyn_time_buf_put(&out, dyn_rrule_wd_names[rr->byweekday[i]],
                        2))
                    goto done;
            }
            for (i = 0; i < rr->n_bynwd; i++) {
                int wd = rr->bynwd[i * 2], n = rr->bynwd[i * 2 + 1];
                if ((rr->n_byweekday || i > 0) && dyn_time_buf_put(&out, ",", 1))
                    goto done;
                if (n < 0) {
                    if (dyn_time_buf_put(&out, "-", 1))
                        goto done;
                    pos = dyn_time_utoa((uint64_t)(-n), tmp);
                } else {
                    if (dyn_time_buf_put(&out, "+", 1))
                        goto done;
                    pos = dyn_time_utoa((uint64_t)n, tmp);
                }
                if (dyn_time_buf_put(&out, tmp, (size_t)pos))
                    goto done;
                if (dyn_time_buf_put(&out, dyn_rrule_wd_names[wd], 2))
                    goto done;
            }
        }
    }
    result = JS_NewStringLen(ctx, (const char*)out.data, out.len);
done:
#undef PUT
#undef PUTN
    dyn_time_buf_free(&out);
    return result;
}

static int dyn_rrule_parse_int(const char* p, size_t len, int64_t* out)
{
    size_t i = 0;
    int neg = 0;
    int64_t v = 0;
    if (len == 0)
        return -1;
    if (p[0] == '-' || p[0] == '+') {
        neg = (p[0] == '-');
        i = 1;
        if (i == len)
            return -1;
    }
    for (; i < len; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        if (v > (INT64_MAX - 9) / 10)
            return -1;
        v = v * 10 + (p[i] - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

static int dyn_rrule_spec_parse_list(JSContext* ctx, DynRRuleSpec* s, int which,
    const char* p, size_t len)
{
    int64_t vals[DYN_RRULE_SETPOS_CAP];
    int n = 0;
    size_t i = 0;
    const char* name = which == DYN_RR_LIST_BYMONTH ? "BYMONTH" : which == DYN_RR_LIST_BYMONTHDAY ? "BYMONTHDAY"
        : which == DYN_RR_LIST_BYYEARDAY                                                          ? "BYYEARDAY"
        : which == DYN_RR_LIST_BYWEEKNO                                                           ? "BYWEEKNO"
                                                                                                  : "BYSETPOS";
    while (i < len) {
        size_t start = i;
        int64_t v;
        while (i < len && p[i] != ',')
            i++;
        if (i == start || dyn_rrule_parse_int(p + start, i - start, &v))
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid %s value", name),
                   -1;
        if (n >= DYN_RRULE_SETPOS_CAP)
            return JS_ThrowRangeError(ctx,
                       "dyna:time: %s list too long", name),
                   -1;
        vals[n++] = v;
        if (i < len)
            i++;
    }
    if (n == 0)
        return JS_ThrowSyntaxError(ctx,
                   "dyna:time: empty %s value", name),
               -1;
    if (dyn_rrule_spec_apply_list(ctx, s, which, vals, n, name))
        return -1;
    switch (which) {
    case DYN_RR_LIST_BYMONTH:
        s->orig |= DYN_RR_ORIG_BYMONTH;
        break;
    case DYN_RR_LIST_BYMONTHDAY:
        s->orig |= DYN_RR_ORIG_BYMONTHDAY;
        break;
    case DYN_RR_LIST_BYYEARDAY:
        s->orig |= DYN_RR_ORIG_BYYEARDAY;
        break;
    case DYN_RR_LIST_BYWEEKNO:
        s->orig |= DYN_RR_ORIG_BYWEEKNO;
        break;
    default:
        s->orig |= DYN_RR_ORIG_BYSETPOS;
        break;
    }
    return 0;
}

static int dyn_rrule_spec_parse_byday(JSContext* ctx, DynRRuleSpec* s,
    const char* p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int wd, n;
        while (i < len && p[i] != ',')
            i++;
        if (i == start)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: empty BYDAY value"),
                   -1;
        if (dyn_rrule_parse_wd_str(p + start, i - start, &wd, &n) || dyn_rrule_spec_add_weekday(ctx, s, wd, n))
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid BYDAY value"),
                   -1;
        if (i < len)
            i++;
    }
    s->orig |= DYN_RR_ORIG_BYWEEKDAY;
    return 0;
}

static int dyn_rrule_spec_parse_part(JSContext* ctx, DynRRuleSpec* s,
    const char* p, size_t len)
{
    size_t eq = 0, vlen;
    const char* val;
    while (eq < len && p[eq] != '=')
        eq++;
    if (eq == len)
        return JS_ThrowSyntaxError(ctx,
                   "dyna:time: RRULE part missing '='"),
               -1;
    val = p + eq + 1;
    vlen = len - eq - 1;

    if (dyn_rrule_name_eq(p, eq, "FREQ")) {
        int f = dyn_rrule_freq_by_name(val, vlen);
        if (f < 0)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: unknown FREQ"),
                   -1;
        s->freq = f;
        s->has_freq = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "INTERVAL")) {
        int64_t v;
        if (dyn_rrule_parse_int(val, vlen, &v) || v < 1 || v > 1000000)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid INTERVAL"),
                   -1;
        s->interval = (int)v;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "COUNT")) {
        int64_t v;
        if (dyn_rrule_parse_int(val, vlen, &v) || v < 1)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid COUNT"),
                   -1;
        if (s->has_until)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: COUNT and UNTIL cannot both be present"),
                   -1;
        s->count = v;
        s->has_count = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "UNTIL")) {
        int64_t v;
        if (dyn_rrule_parse_datetime(val, vlen, &v))
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid UNTIL"),
                   -1;
        if (s->has_count)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: COUNT and UNTIL cannot both be present"),
                   -1;
        s->until = v;
        s->has_until = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "WKST")) {
        int w = dyn_rrule_wd_by_name(val, vlen);
        if (w < 0)
            return JS_ThrowSyntaxError(ctx,
                       "dyna:time: invalid WKST"),
                   -1;
        s->wkst = w;
        s->orig |= DYN_RR_ORIG_WKST;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "BYDAY"))
        return dyn_rrule_spec_parse_byday(ctx, s, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYMONTH"))
        return dyn_rrule_spec_parse_list(ctx, s, DYN_RR_LIST_BYMONTH, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYMONTHDAY"))
        return dyn_rrule_spec_parse_list(ctx, s, DYN_RR_LIST_BYMONTHDAY, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYYEARDAY"))
        return dyn_rrule_spec_parse_list(ctx, s, DYN_RR_LIST_BYYEARDAY, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYWEEKNO"))
        return dyn_rrule_spec_parse_list(ctx, s, DYN_RR_LIST_BYWEEKNO, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYSETPOS"))
        return dyn_rrule_spec_parse_list(ctx, s, DYN_RR_LIST_BYSETPOS, val, vlen);
    if (dyn_rrule_name_eq(p, eq, "BYHOUR") || dyn_rrule_name_eq(p, eq, "BYMINUTE") || dyn_rrule_name_eq(p, eq, "BYSECOND") || dyn_rrule_name_eq(p, eq, "BYEASTER"))
        return JS_ThrowSyntaxError(ctx,
                   "dyna:time: unsupported RRULE parameter (%.*s)",
                   (int)eq, p),
               -1;
    return JS_ThrowSyntaxError(ctx, "dyna:time: unknown RRULE parameter (%.*s)",
               (int)eq, p),
           -1;
}

static int dyn_rrule_spec_parse_rrule(JSContext* ctx, DynRRuleSpec* s,
    const char* p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != ';')
            i++;
        if (i > start) {
            if (dyn_rrule_spec_parse_part(ctx, s, p + start, i - start))
                return -1;
        }
        if (i < len)
            i++;
    }
    return 0;
}

static JSValue dyn_rrule_from_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynRRuleSpec spec;
    const char* s;
    size_t len, i = 0;
    int have_rrule = 0;
    (void)this_val;

    memset(&spec, 0, sizeof(spec));
    spec.freq = -1;
    spec.interval = 1;
    spec.wkst = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "RRule.fromString(str[, options]) requires a string");
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const rr_keys[] = { "dtstart" };
        if (dyn_opts_strict(ctx, argv[1], rr_keys, 1)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "dtstart");
        if (JS_IsException(dv)) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(dv)) {
            if (dyn_rrule_to_epoch(ctx, dv, &spec.dtstart, "dtstart")) {
                JS_FreeValue(ctx, dv);
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
            spec.has_dtstart = 1;
        }
        JS_FreeValue(ctx, dv);
    }

    while (i < len) {
        size_t start, colon, slen;
        const char* name;
        size_t nlen;
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
            i++;
        if (i >= len)
            break;
        start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            i++;
        slen = i - start;
        colon = 0;
        while (colon < slen && s[start + colon] != ':')
            colon++;
        if (colon < slen) {
            name = s + start;
            nlen = colon;
        } else {
            name = s + start;
            nlen = 0;
        }
        if (nlen == 0 || dyn_rrule_name_eq(name, nlen, "RRULE")) {
            if (colon < slen) {
                if (dyn_rrule_spec_parse_rrule(ctx, &spec, s + start + colon + 1,
                        slen - colon - 1)) {
                    JS_FreeCString(ctx, s);
                    return JS_EXCEPTION;
                }
            } else {
                if (dyn_rrule_spec_parse_rrule(ctx, &spec, s + start, slen)) {
                    JS_FreeCString(ctx, s);
                    return JS_EXCEPTION;
                }
            }
            have_rrule = 1;
        } else if (dyn_rrule_name_eq(name, nlen, "DTSTART")) {
            int64_t v;
            if (colon >= slen || dyn_rrule_parse_datetime(s + start + colon + 1, slen - colon - 1, &v)) {
                JS_FreeCString(ctx, s);
                return JS_ThrowSyntaxError(ctx,
                           "dyna:time: invalid DTSTART in RRULE string"),
                       JS_EXCEPTION;
            }
            spec.dtstart = v;
            spec.has_dtstart = 1;
        } else {
            JS_ThrowSyntaxError(ctx, "dyna:time: unsupported property (%.*s)", (int)nlen, name);
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
    }
    JS_FreeCString(ctx, s);

    if (!have_rrule) {
        JS_ThrowSyntaxError(ctx, "dyna:time: no RRULE in string");
        return JS_EXCEPTION;
    }
    if (!spec.has_freq) {
        JS_ThrowSyntaxError(ctx, "dyna:time: RRULE missing FREQ");
        return JS_EXCEPTION;
    }
    return dyn_rrule_build(ctx, JS_UNDEFINED, &spec);
}

static int dyn_rrule_spec_from_options(JSContext* ctx, JSValueConst obj,
    DynRRuleSpec* s)
{
    JSValue v;
    int64_t vals[DYN_RRULE_SETPOS_CAP];
    int n;

    v = JS_GetPropertyStr(ctx, obj, "freq");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        size_t len;
        const char* str;
        int f;
        str = JS_ToCStringLen(ctx, &len, v);
        if (!str) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        f = dyn_rrule_freq_by_name(str, len);
        JS_FreeCString(ctx, str);
        if (f < 0) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "RRule: unknown freq");
            return -1;
        }
        s->freq = f;
        s->has_freq = 1;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "interval");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (!(dv >= 1) || dv > 1000000.0 || dv != (double)(int32_t)dv) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx,
                "RRule: interval must be an integer in [1, 1000000]");
            return -1;
        }
        s->interval = (int32_t)dv;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "count");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (!(dv >= 1) || dv > (double)DYN_MAX_SAFE_INT || dv != (double)(int64_t)dv) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "RRule: count must be a positive integer");
            return -1;
        }
        s->count = (int64_t)dv;
        s->has_count = 1;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "until");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (dyn_rrule_to_epoch(ctx, v, &s->until, "until")) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        s->has_until = 1;
    }
    JS_FreeValue(ctx, v);
    if (s->has_count && s->has_until) {
        JS_ThrowTypeError(ctx,
            "dyna:time: COUNT and UNTIL cannot both be present");
        return -1;
    }

    v = JS_GetPropertyStr(ctx, obj, "dtstart");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (dyn_rrule_to_epoch(ctx, v, &s->dtstart, "dtstart")) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        s->has_dtstart = 1;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "wkst");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int w = -1;
        if (JS_IsNumber(v)) {
            int32_t wd;
            if (JS_ToInt32(ctx, &wd, v) == 0 && wd >= 0 && wd <= 6)
                w = wd;
        } else {
            size_t len;
            const char* str = JS_ToCStringLen(ctx, &len, v);
            if (!str) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            w = dyn_rrule_wd_by_name(str, len);
            JS_FreeCString(ctx, str);
        }
        if (w < 0) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "RRule: wkst must be 0..6 or MO..SU");
            return -1;
        }
        s->wkst = w;
        s->orig |= DYN_RR_ORIG_WKST;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "bymonth");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        n = dyn_rrule_read_int_list(ctx, v, vals, DYN_RRULE_SETPOS_CAP);
        if (n < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0) {
            if (dyn_rrule_spec_apply_list(ctx, s, DYN_RR_LIST_BYMONTH, vals, n,
                    "bymonth")) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            s->orig |= DYN_RR_ORIG_BYMONTH;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "bymonthday");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        n = dyn_rrule_read_int_list(ctx, v, vals, DYN_RRULE_SETPOS_CAP);
        if (n < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0) {
            if (dyn_rrule_spec_apply_list(ctx, s, DYN_RR_LIST_BYMONTHDAY, vals,
                    n, "bymonthday")) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            s->orig |= DYN_RR_ORIG_BYMONTHDAY;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "byyearday");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        n = dyn_rrule_read_int_list(ctx, v, vals, DYN_RRULE_SETPOS_CAP);
        if (n < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0) {
            if (dyn_rrule_spec_apply_list(ctx, s, DYN_RR_LIST_BYYEARDAY, vals,
                    n, "byyearday")) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            s->orig |= DYN_RR_ORIG_BYYEARDAY;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "byweekno");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        n = dyn_rrule_read_int_list(ctx, v, vals, DYN_RRULE_SETPOS_CAP);
        if (n < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0) {
            if (dyn_rrule_spec_apply_list(ctx, s, DYN_RR_LIST_BYWEEKNO, vals,
                    n, "byweekno")) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            s->orig |= DYN_RR_ORIG_BYWEEKNO;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "bysetpos");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        n = dyn_rrule_read_int_list(ctx, v, vals, DYN_RRULE_SETPOS_CAP);
        if (n < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0) {
            if (dyn_rrule_spec_apply_list(ctx, s, DYN_RR_LIST_BYSETPOS, vals,
                    n, "bysetpos")) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            s->orig |= DYN_RR_ORIG_BYSETPOS;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, obj, "byweekday");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (dyn_rrule_read_weekdays(ctx, v, s)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (s->n_byweekday > 0 || s->n_bynwd > 0)
            s->orig |= DYN_RR_ORIG_BYWEEKDAY;
    }
    JS_FreeValue(ctx, v);

    {
        static const char* const refused[] = { "byhour", "byminute",
            "bysecond", "byeaster" };
        int ri;
        for (ri = 0; ri < 4; ri++) {
            v = JS_GetPropertyStr(ctx, obj, refused[ri]);
            if (JS_IsException(v))
                return -1;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                JS_ThrowSyntaxError(ctx,
                    "dyna:time: unsupported RRULE option (%s)", refused[ri]);
                return -1;
            }
            JS_FreeValue(ctx, v);
        }
    }

    return 0;
}

static JSValue dyn_rrule_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    DynRRuleSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.freq = -1;
    spec.interval = 1;
    spec.wkst = 0;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "RRule({ freq, ... }) requires an options object");
    if (dyn_rrule_spec_from_options(ctx, argv[0], &spec))
        return JS_EXCEPTION;
    return dyn_rrule_build(ctx, new_target, &spec);
}

static const JSCFunctionListEntry dyn_rrule_proto[] = {
    JS_CFUNC_DEF("between", 2, dyn_rrule_between),
    JS_CFUNC_DEF("all", 0, dyn_rrule_all),
    JS_CFUNC_DEF("next", 0, dyn_rrule_next),
    JS_CFUNC_DEF("prev", 0, dyn_rrule_prev),
    JS_CFUNC_DEF("toString", 0, dyn_rrule_to_string),
};

int dyn_rrule_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_rrule_class_id, &dyn_rrule_class,
            dyn_rrule_proto, countof(dyn_rrule_proto),
            dyn_rrule_ctor, "RRule")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_rrule_class_id), ctor;
        if (JS_IsException(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsException(ctor))
            return -1;
        if (JS_DefinePropertyValueStr(ctx, ctor, "fromString",
                JS_NewCFunction(ctx, dyn_rrule_from_string, "fromString",
                    1),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}

#endif
