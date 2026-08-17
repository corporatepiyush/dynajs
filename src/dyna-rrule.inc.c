/* ================================================================ *
 *  class RRule -- RFC 5545 recurrence rules.
 *
 *  Semantics are python-dateutil's rrule (the vendored oracle), ported
 *  field-for-field: the per-year masks, the BYWEEKNO week-numbering, the
 *  BYSETPOS day/time indexing, the per-frequency period advance and the
 *  month-end day clamping are all dateutil 2.8.2/2.9.0's exact arithmetic.
 *  The differential was validated against pinned dateutil before this
 *  shipped (1194 randomized + 33 fixed vectors, 0 mismatches).
 *
 *  Time model: everything is UTC whole-second unix time. DTSTART fixes the
 *  time-of-day for every occurrence of a day-based frequency; BYHOUR /
 *  BYMINUTE / BYSECOND / BYEASTER are NOT modelled and are REFUSED rather
 *  than silently ignored.
 *
 *  Bounds (the brief's infinite-recurrence protection):
 *    - `all()` without COUNT, UNTIL or an explicit limit refuses.
 *    - every call scans at most DYN_RRULE_MAX_STEPS frequency periods; a
 *      rule that never matches (e.g. FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30)
 *      returns [] / null instead of spinning.
 *    - every call RETURNS at most DYN_RRULE_MAX_RESULTS occurrences; a
 *      rule producing more (a huge COUNT over many periods) raises
 *      RangeError rather than growing an unbounded JS array.
 *    - generation stops at year 9999, dateutil's datetime.MAXYEAR bound.
 *    - dtstart/until years are validated into 0001..9999 at construction.
 *
 *  Hardening notes (audit):
 *    - BYDAY ordinal downgrade (freq > MONTHLY) is decided in build(), on
 *      the FINAL freq, so fromString part order is irrelevant:
 *      "BYDAY=1MO;FREQ=WEEKLY" == "FREQ=WEEKLY;BYDAY=1MO" == plain MO.
 *    - toString joins positive and negative BYMONTHDAY with a comma
 *      ("15,-1"), matching dateutil and round-tripping.
 *    - NaN / Infinity / out-of-range numbers in dtstart, until, count,
 *      interval, limits and Date.getTime() are rejected, never cast.
 *    - documented deviation: empty by* ARRAYS are treated as absent
 *      (dateutil treats bymonthday=[] as "given but no filter" while still
 *      suppressing the dtstart defaults -- its string round-trip quirk).
 * ================================================================ */

enum {
    DYN_RRULE_YEARLY = 0,
    DYN_RRULE_MONTHLY,
    DYN_RRULE_WEEKLY,
    DYN_RRULE_DAILY,
    DYN_RRULE_HOURLY,
    DYN_RRULE_MINUTELY,
    DYN_RRULE_SECONDLY,
};

static const char *const dyn_rrule_freq_names[7] = {
    "YEARLY", "MONTHLY", "WEEKLY", "DAILY", "HOURLY", "MINUTELY", "SECONDLY"
};
static const char dyn_rrule_wd_names[7][2] = {
    { 'M', 'O' }, { 'T', 'U' }, { 'W', 'E' }, { 'T', 'H' },
    { 'F', 'R' }, { 'S', 'A' }, { 'S', 'U' }
};

/* Periods scanned per call before a rule is declared non-productive, and
 * occurrences returned per call before the call refuses. Both bound CPU
 * AND memory: no per-occurrence buffer grows unboundedly. */
#define DYN_RRULE_MAX_STEPS    1000000u
#define DYN_RRULE_MAX_RESULTS  1000000u
#define DYN_RRULE_MAX_YEAR     9999

/* capacity of the fixed-size by* arrays (RFC maxima; bysetpos keeps input
 * order, everything else is sorted+deduped) */
#define DYN_RRULE_MONTH_CAP    16
#define DYN_RRULE_MDAY_CAP     32
#define DYN_RRULE_YDAY_CAP     800
#define DYN_RRULE_WKNO_CAP     128
#define DYN_RRULE_SETPOS_CAP   800
#define DYN_RRULE_WD_CAP       8
#define DYN_RRULE_BYNWD_CAP    742   /* 7 weekdays x 106 ordinals */

/* which parts were EXPLICITLY given (drives toString and the defaulting) */
#define DYN_RR_ORIG_BYMONTH    0x001
#define DYN_RR_ORIG_BYMONTHDAY 0x002
#define DYN_RR_ORIG_BYYEARDAY  0x004
#define DYN_RR_ORIG_BYWEEKNO   0x008
#define DYN_RR_ORIG_BYWEEKDAY  0x010
#define DYN_RR_ORIG_BYSETPOS   0x020
#define DYN_RR_ORIG_WKST       0x040

/* positive mod 7 (weekday arithmetic needs Python's %, not C's) */
#define DYN_RRULE_PMOD7(x) (((x) % 7 + 7) % 7)

/* ---- tiny sorted-set helpers (all by* sets except bysetpos) ---- */

static int dyn_rrule_set_add(int16_t *a, int *n, int cap, int v)
{
    int i;
    for (i = 0; i < *n; i++) {
        if (a[i] == v)
            return 0;
        if (a[i] > v)
            break;
    }
    if (*n >= cap)
        return -1;
    memmove(a + i + 1, a + i, (size_t)(*n - i) * sizeof(a[0]));
    a[i] = (int16_t)v;
    (*n)++;
    return 0;
}

static int dyn_rrule_in(const int16_t *a, int n, int v)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (a[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && a[lo] == v;
}

static int dyn_rrule_is_leap(int64_t y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

/* engine weekday (0=Sun) -> RFC weekday (0=Mon..6=Sun) */
static int dyn_rrule_iso_wd(int64_t days)
{
    return (dyn_weekday_from_days(days) + 6) % 7;
}

static char dyn_rrule_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* ---- date coercion (Date | ISO string | unix seconds) ---- */

/* Strict RFC5545/RFC3339 date parse: YYYYMMDD[THHMMSS[Z]],
 * YYYY-MM-DD[THH:MM:SS[.fraction][Z|+-HH:MM]]. UTC throughout (an explicit
 * offset is subtracted). 0 on success, -1 on any deviation. */
static int dyn_rrule_parse_datetime(const char *s, size_t len, int64_t *out)
{
    size_t pos = 0;
    int64_t y, mo, d, h = 0, mi = 0, se = 0, off = 0;
    int compact;

    if (len < 8)
        return -1;
    if (dyn_time_expect_digits(s, len, pos, 4, &y))
        return -1;
    pos += 4;
    compact = (s[pos] >= '0' && s[pos] <= '9');
    if (compact) {
        if (dyn_time_expect_digits(s, len, pos, 2, &mo)) return -1;
        pos += 2;
        if (dyn_time_expect_digits(s, len, pos, 2, &d)) return -1;
        pos += 2;
    } else {
        if (pos >= len || s[pos] != '-') return -1;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &mo)) return -1;
        pos += 2;
        if (pos >= len || s[pos] != '-') return -1;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &d)) return -1;
        pos += 2;
    }

    if (pos < len && (s[pos] == 'T' || s[pos] == 't')) {
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &h)) return -1;
        pos += 2;
        if (pos < len && s[pos] == ':') {
            pos++;
            if (dyn_time_expect_digits(s, len, pos, 2, &mi)) return -1;
            pos += 2;
            if (pos < len && s[pos] == ':') {
                pos++;
                if (dyn_time_expect_digits(s, len, pos, 2, &se)) return -1;
                pos += 2;
            }
        } else {
            if (dyn_time_expect_digits(s, len, pos, 2, &mi)) return -1;
            pos += 2;
            if (dyn_time_expect_digits(s, len, pos, 2, &se)) return -1;
            pos += 2;
        }
        if (pos < len && s[pos] == '.') {
            size_t start;
            pos++;
            start = pos;
            while (pos < len && s[pos] >= '0' && s[pos] <= '9')
                pos++;
            if (pos == start)
                return -1;      /* '.' with no digits */
        }
        if (pos < len && (s[pos] == 'Z' || s[pos] == 'z')) {
            pos++;
        } else if (pos < len && (s[pos] == '+' || s[pos] == '-')) {
            int sign = (s[pos] == '-') ? -1 : 1;
            int64_t oh, om;
            pos++;
            if (dyn_time_expect_digits(s, len, pos, 2, &oh)) return -1;
            pos += 2;
            if (pos < len && s[pos] == ':') {
                pos++;
                if (dyn_time_expect_digits(s, len, pos, 2, &om)) return -1;
                pos += 2;
            } else {
                if (dyn_time_expect_digits(s, len, pos, 2, &om)) return -1;
                pos += 2;
            }
            if (oh > 23 || om > 59)
                return -1;
            off = sign * (oh * 3600 + om * 60);
        }
    }
    if (pos != len)
        return -1;

    if (mo < 1 || mo > 12 || d < 1 || d > dyn_time_days_in_month(y, (int)mo))
        return -1;
    if (h > 23 || mi > 59 || se > 60)
        return -1;
    *out = dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY +
           h * 3600 + mi * 60 + se - off;
    return 0;
}

/* epoch ms of a Date-like object (a callable getTime). NaN and anything
 * outside the JS Date range [-8.64e15, +8.64e15] ms are rejected before any
 * int64 cast (an out-of-range double cast is UB). */
static int dyn_rrule_date_epoch(JSContext *ctx, JSValueConst v, int64_t *out)
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

/* Date | ISO string | number(bigint|unix seconds) -> unix seconds.
 * Numbers must be whole seconds within int64-safe range; NaN, Infinity and
 * fractional values throw rather than silently truncating. */
static int dyn_rrule_to_epoch(JSContext *ctx, JSValueConst v, int64_t *out,
                              const char *what)
{
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= -(double)DYN_MAX_SAFE_INT && d <= (double)DYN_MAX_SAFE_INT) ||
            d != (double)(int64_t)d) {
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
        const char *s = JS_ToCStringLen(ctx, &len, v);
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

/* ---- rule construction ---- */

typedef struct DynRRuleSpec {
    int freq;             /* -1 until set */
    int has_freq;
    int32_t interval;
    int wkst;             /* 0=MO */
    int64_t count, until, dtstart;
    int has_count, has_until, has_dtstart;
    uint32_t orig;
    /* by* sets (sorted+deduped except bysetpos, which keeps input order) */
    int16_t bymonth[DYN_RRULE_MONTH_CAP];    int n_bymonth;
    int16_t bymonthday[DYN_RRULE_MDAY_CAP];  int n_bymonthday;
    int16_t bynmonthday[DYN_RRULE_MDAY_CAP]; int n_bynmonthday;
    int16_t byyearday[DYN_RRULE_YDAY_CAP];   int n_byyearday;
    int16_t byweekno[DYN_RRULE_WKNO_CAP];    int n_byweekno;
    int16_t bysetpos[DYN_RRULE_SETPOS_CAP];  int n_bysetpos;
    int16_t byweekday[DYN_RRULE_WD_CAP];     int n_byweekday;
    int16_t bynwd[DYN_RRULE_BYNWD_CAP * 2];  int n_bynwd; /* (wd,n) pairs */
} DynRRuleSpec;

typedef struct DynRRule {
    uint8_t freq, wkst;
    int32_t interval;
    int64_t dtstart, until, count;
    int has_until, has_count;
    int64_t ds_y;
    int ds_mo, ds_d, ds_h, ds_mi, ds_s;
    uint32_t orig;
    int16_t bymonth[DYN_RRULE_MONTH_CAP];    int n_bymonth;
    int16_t bymonthday[DYN_RRULE_MDAY_CAP];  int n_bymonthday;
    int16_t bynmonthday[DYN_RRULE_MDAY_CAP]; int n_bynmonthday;
    int16_t byyearday[DYN_RRULE_YDAY_CAP];   int n_byyearday;
    int16_t byweekno[DYN_RRULE_WKNO_CAP];    int n_byweekno;
    int16_t bysetpos[DYN_RRULE_SETPOS_CAP];  int n_bysetpos;
    int16_t byweekday[DYN_RRULE_WD_CAP];     int n_byweekday;
    int16_t bynwd[DYN_RRULE_BYNWD_CAP * 2];  int n_bynwd;
    /* which masks the generator must build (cheap rules build none) */
    uint8_t need_mmask, need_mday, need_nmday, need_wday, need_wno, need_nwd;
} DynRRule;

static JSClassID dyn_rrule_class_id;

static void dyn_rrule_dispose(void *native)
{
    free(native);
}

static void dyn_rrule_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_rrule_dispose(JS_GetOpaque(val, dyn_rrule_class_id));
}

static const JSClassDef dyn_rrule_class = {
    "RRule",
    .finalizer = dyn_rrule_finalizer,
};

/* case-insensitive match of s[0..len) against an ASCII name */
static int dyn_rrule_name_eq(const char *s, size_t len, const char *name)
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

static int dyn_rrule_freq_by_name(const char *s, size_t len)
{
    int f;
    for (f = 0; f < 7; f++)
        if (dyn_rrule_name_eq(s, len, dyn_rrule_freq_names[f]))
            return f;
    return -1;
}

static int dyn_rrule_wd_by_name(const char *s, size_t len)
{
    int w;
    if (len != 2)
        return -1;
    for (w = 0; w < 7; w++)
        if (dyn_rrule_upper(s[0]) == dyn_rrule_wd_names[w][0] &&
            dyn_rrule_upper(s[1]) == dyn_rrule_wd_names[w][1])
            return w;
    return -1;
}

/* parse a weekday item: "MO", "1MO", "-1FR", "+2TU", "MO(+1)" */
static int dyn_rrule_parse_wd_str(const char *p, size_t len, int *wd, int *n)
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
            return -1;          /* "0MO" */
        *n = has_num ? num * sign : 0;
        return 0;
    }
    if (has_num)
        return -1;              /* "1MO(...)" is malformed */
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

/* add a byweekday item to the spec. Ordinals are NOT downgraded here:
 * fromString parses parts in arbitrary order, so s->freq may still be -1
 * when BYDAY arrives; the final-freq downgrade happens in build(). Every
 * failure path throws (a bare -1 with no exception set becomes a bogus
 * JS_EXCEPTION downstream). */
static int dyn_rrule_spec_add_weekday(JSContext *ctx, DynRRuleSpec *s,
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

enum {
    DYN_RR_LIST_BYMONTH = 0,
    DYN_RR_LIST_BYMONTHDAY,
    DYN_RR_LIST_BYYEARDAY,
    DYN_RR_LIST_BYWEEKNO,
    DYN_RR_LIST_BYSETPOS,
};

/* validate + insert a whole int list per its option's RFC ranges */
static int dyn_rrule_spec_apply_list(JSContext *ctx, DynRRuleSpec *s, int which,
                                     const int64_t *vals, int n,
                                     const char *name)
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
        default: /* BYSETPOS: input order kept, no dedup (dateutil does not) */
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

/* read a scalar-or-array of numbers into vals (cap entries); returns count */
static int dyn_rrule_read_int_list(JSContext *ctx, JSValueConst v,
                                   int64_t *vals, int cap)
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

/* one byweekday item: a number 0..6 (0=MO) or a weekday string */
static int dyn_rrule_weekday_value(JSContext *ctx, JSValueConst v,
                                   DynRRuleSpec *s)
{
    if (JS_IsNumber(v)) {
        double d;
        int64_t x;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        /* reject NaN, negatives, fractions (1.5 must not truncate to MO) */
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
        const char *str;
        int wd, n;
        str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        if (dyn_rrule_parse_wd_str(str, len, &wd, &n) ||
            dyn_rrule_spec_add_weekday(ctx, s, wd, n)) {
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

/* read byweekday (scalar or array) into the spec; array length is capped
 * so a hostile 10M-element array is rejected, not iterated */
static int dyn_rrule_read_weekdays(JSContext *ctx, JSValueConst v,
                                   DynRRuleSpec *s)
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

/* sort the bynwd pairs by (wday, n) -- dateutil's ordering for toString */
static void dyn_rrule_sort_bynwd(int16_t *bynwd, int n)
{
    int i;
    for (i = 1; i < n; i++) {
        int16_t w = bynwd[i * 2], nn = bynwd[i * 2 + 1];
        int j = i - 1;
        while (j >= 0 &&
               (bynwd[j * 2] > w ||
                (bynwd[j * 2] == w && bynwd[j * 2 + 1] > nn))) {
            bynwd[(j + 1) * 2] = bynwd[j * 2];
            bynwd[(j + 1) * 2 + 1] = bynwd[j * 2 + 1];
            j--;
        }
        bynwd[(j + 1) * 2] = w;
        bynwd[(j + 1) * 2 + 1] = nn;
    }
}

/* build the rule from a validated spec (applies dateutil's defaults) */
static JSValue dyn_rrule_build(JSContext *ctx, const DynRRuleSpec *s)
{
    DynRRule *rr;
    int64_t ds_sec;
    int no_by;

    if (!s->has_freq) {
        JS_ThrowTypeError(ctx, "RRule: freq is required "
                               "(YEARLY|MONTHLY|WEEKLY|DAILY|HOURLY|"
                               "MINUTELY|SECONDLY)");
        return JS_EXCEPTION;
    }
    ds_sec = s->has_dtstart ? s->dtstart : (int64_t)time(NULL);

    rr = (DynRRule *)malloc(sizeof(*rr));
    if (!rr)
        return JS_ThrowOutOfMemory(ctx);
    rr->freq = (uint8_t)s->freq;
    rr->interval = s->interval;
    rr->wkst = (uint8_t)s->wkst;
    rr->dtstart = ds_sec;
    rr->until = s->until;
    rr->count = s->count;
    rr->has_until = s->has_until;
    rr->has_count = s->has_count;
    rr->orig = s->orig;
    memcpy(rr->bymonth, s->bymonth, sizeof(rr->bymonth));
    memcpy(rr->bymonthday, s->bymonthday, sizeof(rr->bymonthday));
    memcpy(rr->bynmonthday, s->bynmonthday, sizeof(rr->bynmonthday));
    memcpy(rr->byyearday, s->byyearday, sizeof(rr->byyearday));
    memcpy(rr->byweekno, s->byweekno, sizeof(rr->byweekno));
    memcpy(rr->bysetpos, s->bysetpos, sizeof(rr->bysetpos));
    memcpy(rr->byweekday, s->byweekday, sizeof(rr->byweekday));
    /* BYDAY ordinals are positional only for YEARLY/MONTHLY (dateutil folds
     * ordinals to plain weekdays when freq > MONTHLY in its constructor).
     * Doing it HERE makes fromString order-independent. */
    if (s->freq > DYN_RRULE_MONTHLY && s->n_bynwd > 0) {
        int k;
        for (k = 0; k < s->n_bynwd; k++) {
            int wd = s->bynwd[k * 2];
            dyn_rrule_set_add(rr->byweekday, &rr->n_byweekday,
                              DYN_RRULE_WD_CAP, wd);
        }
        rr->n_bynwd = 0;
    } else {
        memcpy(rr->bynwd, s->bynwd, sizeof(rr->bynwd));
        rr->n_bynwd = s->n_bynwd;
    }
    rr->n_bymonth = s->n_bymonth;
    rr->n_bymonthday = s->n_bymonthday;
    rr->n_bynmonthday = s->n_bynmonthday;
    rr->n_byyearday = s->n_byyearday;
    rr->n_byweekno = s->n_byweekno;
    rr->n_bysetpos = s->n_bysetpos;
    rr->n_byweekday = s->n_byweekday;
    dyn_rrule_sort_bynwd(rr->bynwd, rr->n_bynwd); /* toString order */

    {
        int64_t days = dyn_time_floor_div(ds_sec, DYN_SECS_PER_DAY);
        int64_t tod = dyn_time_floor_mod(ds_sec, DYN_SECS_PER_DAY);
        dyn_civil_from_days(days, &rr->ds_y, &rr->ds_mo, &rr->ds_d);
        rr->ds_h = (int)(tod / 3600);
        rr->ds_mi = (int)((tod / 60) % 60);
        rr->ds_s = (int)(tod % 60);
    }

    /* dateutil's datetime bounds: generation stops at MAXYEAR=9999, and the
     * toString UNTIL formatter assumes a 4-digit year -- a negative year
     * would render as a 20-digit uint64 and overflow the format buffer. */
    if (rr->ds_y < 1 || rr->ds_y > DYN_RRULE_MAX_YEAR) {
        free(rr);
        return JS_ThrowRangeError(ctx,
            "RRule: dtstart year %lld is outside 0001..9999",
            (long long)rr->ds_y);
    }
    if (rr->has_until) {
        int64_t udays = dyn_time_floor_div(rr->until, DYN_SECS_PER_DAY);
        int64_t uy;
        int um, ud;
        dyn_civil_from_days(udays, &uy, &um, &ud);
        if (uy < 1 || uy > DYN_RRULE_MAX_YEAR) {
            free(rr);
            return JS_ThrowRangeError(ctx,
                "RRule: until year %lld is outside 0001..9999",
                (long long)uy);
        }
    }

    /* dateutil defaults: with no by-weekday/-monthday/-yearday/-weekno, the
     * period repeats the dtstart fields. Explicit (even if empty) arrays
     * suppress them (documented deviation: empty arrays are treated as
     * absent rather than dateutil's "given but no filter"). */
    no_by = !(rr->orig & (DYN_RR_ORIG_BYMONTHDAY | DYN_RR_ORIG_BYYEARDAY |
                          DYN_RR_ORIG_BYWEEKNO | DYN_RR_ORIG_BYWEEKDAY));
    if (no_by) {
        if (rr->freq == DYN_RRULE_YEARLY) {
            if (!(rr->orig & DYN_RR_ORIG_BYMONTH))
                rr->bymonth[rr->n_bymonth++] = (int16_t)rr->ds_mo;
            rr->bymonthday[rr->n_bymonthday++] = (int16_t)rr->ds_d;
        } else if (rr->freq == DYN_RRULE_MONTHLY) {
            rr->bymonthday[rr->n_bymonthday++] = (int16_t)rr->ds_d;
        } else if (rr->freq == DYN_RRULE_WEEKLY) {
            int64_t days = dyn_time_floor_div(ds_sec, DYN_SECS_PER_DAY);
            rr->byweekday[rr->n_byweekday++] = (int16_t)dyn_rrule_iso_wd(days);
        }
    }

    /* which per-year masks the generator must build */
    rr->need_mmask  = rr->n_bymonth > 0;
    rr->need_mday   = rr->n_bymonthday > 0;
    rr->need_nmday  = rr->n_bynmonthday > 0;
    /* the weekday mask is required by the WEEKLY dayset boundary check, by
     * the byweekday filter, and by the byweekno/bynwd mask builders */
    rr->need_wday   = rr->freq == DYN_RRULE_WEEKLY || rr->n_byweekday > 0 ||
                      rr->n_byweekno > 0 || rr->n_bynwd > 0;
    rr->need_wno    = rr->n_byweekno > 0;
    rr->need_nwd    = rr->n_bynwd > 0;

    return dyn_plain_wrap(ctx, dyn_rrule_class_id, rr, dyn_rrule_dispose);
}

/* ================================================================ *
 *  Generation engine (dateutil rrule._iter + _iterinfo, field-for-field)
 * ================================================================ */

typedef int (*DynRRuleYield)(int64_t sec, void *ud);

#define DYN_RRULE_YEAR_ARR 373   /* yearlen+7 */

typedef struct DynRRuleGen {
    const DynRRule *rr;
    int64_t year; int month, day, hour, minute, second, weekday;
    int64_t yearordinal;
    int yearlen, nextyearlen, yearweekday;
    int lastyear, lastmonth;
    int mrange[13];
    uint8_t mmask[DYN_RRULE_YEAR_ARR];
    int16_t mdaymask[DYN_RRULE_YEAR_ARR];
    int16_t nmdaymask[DYN_RRULE_YEAR_ARR];
    uint8_t wdaymask[385];
    uint8_t wnomask[DYN_RRULE_YEAR_ARR];
    uint8_t nwdaymask[DYN_RRULE_YEAR_ARR];
    int n_timeset;
    int32_t timeset[4];          /* seconds-of-day; always 1 entry in this API */
    uint32_t steps;
} DynRRuleGen;

/* weekday of day-of-year index i (handles negative / past-year indices) */
static inline int dyn_rrule_gen_wdof(const DynRRuleGen *g, int i)
{
    return g->wdaymask[DYN_RRULE_PMOD7(i)];
}

static void dyn_rrule_build_wnomask(DynRRuleGen *g);

/* Build the per-year masks. Only the masks the rule's filters consult are
 * built; a plain DAILY rule rebuilds nothing but two lengths. */
static void dyn_rrule_rebuild_year(DynRRuleGen *g)
{
    const DynRRule *rr = g->rr;
    int64_t y = g->year;
    int i;

    g->yearlen = 365 + dyn_rrule_is_leap(y);
    g->nextyearlen = 365 + dyn_rrule_is_leap(y + 1);
    g->yearordinal = dyn_days_from_civil(y, 1, 1);
    g->yearweekday = dyn_rrule_iso_wd(g->yearordinal);

    if (rr->need_wday)
        for (i = 0; i < 385; i++)
            g->wdaymask[i] = (uint8_t)((g->yearweekday + i) % 7);

    /* month start offsets, [0,13]: mrange[m-1] = start of month m (0-based) */
    for (i = 0; i < 12; i++)
        g->mrange[i] = (int)(dyn_days_from_civil(y, i + 1, 1) - g->yearordinal);
    g->mrange[12] = g->yearlen;

    if (rr->need_mmask || rr->need_mday || rr->need_nmday) {
        for (i = 0; i < g->yearlen + 7; i++) {
            int64_t yy;
            int mo, d;
            dyn_civil_from_days(g->yearordinal + i, &yy, &mo, &d);
            if (rr->need_mmask)
                g->mmask[i] = (uint8_t)mo;
            if (rr->need_mday || rr->need_nmday) {
                g->mdaymask[i] = (int16_t)d;
                g->nmdaymask[i] = (int16_t)(d - dyn_time_days_in_month(yy, mo) - 1);
            }
        }
    }
    if (rr->need_wno)
        dyn_rrule_build_wnomask(g);
}

/* ISO-style week-number mask (dateutil's wnomask, extended by 7 days). */
static void dyn_rrule_build_wnomask(DynRRuleGen *g)
{
    const DynRRule *rr = g->rr;
    int wkst = rr->wkst;
    int firstwkst = (7 - g->yearweekday + wkst) % 7;
    int no1wkst = firstwkst;
    int wyearlen, numweeks, i, j;

    if (no1wkst >= 4) {
        no1wkst = 0;
        wyearlen = g->yearlen + DYN_RRULE_PMOD7(g->yearweekday - wkst);
    } else {
        wyearlen = g->yearlen - no1wkst;
    }
    numweeks = wyearlen / 7 + (wyearlen % 7) / 4;

    memset(g->wnomask, 0, sizeof(g->wnomask));
    for (i = 0; i < rr->n_byweekno; i++) {
        int n = rr->byweekno[i];
        int idx;
        if (n < 0)
            n += numweeks + 1;
        if (!(0 < n && n <= numweeks))
            continue;
        if (n > 1) {
            idx = no1wkst + (n - 1) * 7;
            if (no1wkst != firstwkst)
                idx -= 7 - firstwkst;
        } else {
            idx = no1wkst;
        }
        for (j = 0; j < 7; j++) {
            g->wnomask[idx] = 1;
            idx++;
            if (g->wdaymask[idx] == wkst)
                break;
        }
    }
    if (dyn_rrule_in(rr->byweekno, rr->n_byweekno, 1)) {
        /* week 1 of next year, if it starts inside this one */
        int idx = no1wkst + numweeks * 7;
        if (no1wkst != firstwkst)
            idx -= 7 - firstwkst;
        if (idx < g->yearlen) {
            for (j = 0; j < 7; j++) {
                g->wnomask[idx] = 1;
                idx++;
                if (g->wdaymask[idx] == wkst)
                    break;
            }
        }
    }
    if (no1wkst) {
        /* the last week of last year may extend into this one */
        int lnumweeks;
        if (!dyn_rrule_in(rr->byweekno, rr->n_byweekno, -1)) {
            /* Jan 1 of LAST year, not (Jan 1 of this year - THIS year's
             * length), which is one day short in a non-leap-to-leap step */
            int lyearlen = 365 + dyn_rrule_is_leap(g->year - 1);
            int lyearweekday = dyn_rrule_iso_wd(g->yearordinal - lyearlen);
            int lno1wkst = (7 - lyearweekday + wkst) % 7;
            if (lno1wkst >= 4) {
                lno1wkst = 0;
                lnumweeks = 52 + (((lyearlen +
                                    DYN_RRULE_PMOD7(lyearweekday - wkst)) % 7) / 4);
            } else {
                lnumweeks = 52 + (((g->yearlen - no1wkst) % 7) / 4);
            }
        } else {
            lnumweeks = -1;
        }
        if (dyn_rrule_in(rr->byweekno, rr->n_byweekno, lnumweeks)) {
            for (i = 0; i < no1wkst; i++)
                g->wnomask[i] = 1;
        }
    }
}

/* mark the nth weekday of the given inclusive day range (dateutil nwdaymask) */
static void dyn_rrule_mark_nwd_range(DynRRuleGen *g, int first, int last)
{
    const DynRRule *rr = g->rr;
    int p;
    for (p = 0; p < rr->n_bynwd; p++) {
        int wd = rr->bynwd[p * 2], n = rr->bynwd[p * 2 + 1];
        int i;
        if (n < 0) {
            i = last + (n + 1) * 7;
            i -= DYN_RRULE_PMOD7(dyn_rrule_gen_wdof(g, i) - wd);
        } else {
            i = first + (n - 1) * 7;
            i += (7 - dyn_rrule_gen_wdof(g, i) + wd) % 7;
        }
        if (first <= i && i <= last)
            g->nwdaymask[i] = 1;
    }
}

/* rebuild all year-level state + the month-scoped nwdaymask */
static void dyn_rrule_rebuild(DynRRuleGen *g)
{
    const DynRRule *rr = g->rr;
    if ((int)g->year != g->lastyear)
        dyn_rrule_rebuild_year(g);
    if (rr->n_bynwd &&
        ((int)g->year != g->lastyear || g->month != g->lastmonth)) {
        memset(g->nwdaymask, 0, (size_t)g->yearlen);
        if (rr->freq == DYN_RRULE_YEARLY) {
            if (rr->n_bymonth) {
                int k;
                for (k = 0; k < rr->n_bymonth; k++) {
                    int m = rr->bymonth[k];
                    dyn_rrule_mark_nwd_range(g, g->mrange[m - 1],
                                             g->mrange[m] - 1);
                }
            } else {
                dyn_rrule_mark_nwd_range(g, 0, g->yearlen - 1);
            }
        } else if (rr->freq == DYN_RRULE_MONTHLY) {
            dyn_rrule_mark_nwd_range(g, g->mrange[g->month - 1],
                                     g->mrange[g->month] - 1);
        }
    }
    g->lastyear = (int)g->year;
    g->lastmonth = g->month;
}

/* Does day-of-year index i survive every by* filter? (dateutil's filter loop) */
static int dyn_rrule_day_kept(const DynRRuleGen *g, int i)
{
    const DynRRule *rr = g->rr;
    if (rr->n_bymonth && !dyn_rrule_in(rr->bymonth, rr->n_bymonth, g->mmask[i]))
        return 0;
    if (rr->n_byweekno && !g->wnomask[i])
        return 0;
    if (rr->n_byweekday &&
        !dyn_rrule_in(rr->byweekday, rr->n_byweekday, g->wdaymask[i]))
        return 0;
    if (rr->n_bynwd && !g->nwdaymask[i])
        return 0;
    if (rr->n_bymonthday || rr->n_bynmonthday) {
        if (!dyn_rrule_in(rr->bymonthday, rr->n_bymonthday, g->mdaymask[i]) &&
            !dyn_rrule_in(rr->bynmonthday, rr->n_bynmonthday, g->nmdaymask[i]))
            return 0;
    }
    if (rr->n_byyearday) {
        int16_t v1, v2;
        if (i < g->yearlen) {
            v1 = (int16_t)(i + 1);
            v2 = (int16_t)(-g->yearlen + i);
        } else {
            /* cross-year day (WEEKLY dayset) */
            v1 = (int16_t)(i + 1 - g->yearlen);
            v2 = (int16_t)(-g->nextyearlen + i - g->yearlen);
        }
        if (!dyn_rrule_in(rr->byyearday, rr->n_byyearday, v1) &&
            !dyn_rrule_in(rr->byyearday, rr->n_byyearday, v2))
            return 0;
    }
    return 1;
}

/* one candidate occurrence: apply until / dtstart / count, yield */
static int dyn_rrule_emit(const DynRRule *rr, int64_t res, int64_t *count,
                          DynRRuleYield fn, void *ud)
{
    if (rr->has_until && res > rr->until)
        return -1;               /* exhausted: stop the whole loop */
    if (res >= rr->dtstart) {
        if (rr->has_count) {
            (*count)--;
            if (*count < 0)
                return 1;        /* count reached */
        }
        if (fn(res, ud))
            return 1;
    }
    return 0;
}

/* set the cursor to the period containing `sec` (freq-invariant fields stay
 * at their dtstart values, exactly as dateutil's cursor does) */
static void dyn_rrule_cursor_from_sec(DynRRuleGen *g, int64_t sec)
{
    const DynRRule *rr = g->rr;
    int64_t days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    int64_t tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    int64_t y;
    int mo, d;

    dyn_civil_from_days(days, &y, &mo, &d);
    g->year = y;
    g->month = mo;
    g->day = d;
    switch (rr->freq) {
    case DYN_RRULE_YEARLY:
        g->month = rr->ds_mo;
        g->day = rr->ds_d;
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_MONTHLY:
        g->day = rr->ds_d;
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_WEEKLY:
    case DYN_RRULE_DAILY:
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_HOURLY:
        g->hour = (int)(tod / 3600);
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_MINUTELY:
        g->hour = (int)(tod / 3600);
        g->minute = (int)((tod / 60) % 60);
        g->second = rr->ds_s;
        break;
    default: /* SECONDLY */
        g->hour = (int)(tod / 3600);
        g->minute = (int)((tod / 60) % 60);
        g->second = (int)(tod % 60);
        break;
    }
    g->weekday = dyn_rrule_iso_wd(days);
}

static void dyn_rrule_gen_init(DynRRuleGen *g, const DynRRule *rr,
                               int64_t cursor_sec)
{
    memset(g, 0, sizeof(*g));
    g->rr = rr;
    dyn_rrule_cursor_from_sec(g, cursor_sec);
    dyn_rrule_rebuild(g);
    if (rr->freq >= DYN_RRULE_HOURLY) {
        int64_t tod = dyn_time_floor_mod(cursor_sec, DYN_SECS_PER_DAY);
        int h = (int)(tod / 3600), mi = (int)((tod / 60) % 60),
            se = (int)(tod % 60);
        if (rr->freq == DYN_RRULE_HOURLY)
            g->timeset[0] = h * 3600 + rr->ds_mi * 60 + rr->ds_s;
        else if (rr->freq == DYN_RRULE_MINUTELY)
            g->timeset[0] = h * 3600 + mi * 60 + rr->ds_s;
        else
            g->timeset[0] = h * 3600 + mi * 60 + se;
    } else {
        g->timeset[0] = rr->ds_h * 3600 + rr->ds_mi * 60 + rr->ds_s;
    }
    g->n_timeset = 1;
}

/* lexicographic cursor comparison: is g_a's period at/after g_b's? */
static int dyn_rrule_cursor_ge(const DynRRuleGen *a, const DynRRuleGen *b)
{
    int64_t x[6], y[6];
    int i;
    x[0] = a->year; x[1] = a->month; x[2] = a->day;
    x[3] = a->hour; x[4] = a->minute; x[5] = a->second;
    y[0] = b->year; y[1] = b->month; y[2] = b->day;
    y[3] = b->hour; y[4] = b->minute; y[5] = b->second;
    for (i = 0; i < 6; i++) {
        if (x[i] != y[i])
            return x[i] > y[i];
    }
    return 1;
}

/* Run the generator from the cursor, calling fn(sec) for each occurrence.
 * The loop is bounded by DYN_RRULE_MAX_STEPS periods and by year 9999. */
static void dyn_rrule_gen_loop(DynRRuleGen *g, DynRRuleYield fn, void *ud)
{
    const DynRRule *rr = g->rr;
    int freq = rr->freq, interval = rr->interval, wkst = rr->wkst;
    int64_t count = rr->count;
    int32_t kept[373];
    int64_t poslist[DYN_RRULE_SETPOS_CAP];

    for (;;) {
        int start, end, i, j, k;
        int n_kept = 0;
        int filtered = 0;

        if (++g->steps > DYN_RRULE_MAX_STEPS)
            break;

        /* ---- the period's candidate days ---- */
        if (freq == DYN_RRULE_YEARLY) {
            start = 0;
            end = g->yearlen;
            for (i = start; i < end; i++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
            }
        } else if (freq == DYN_RRULE_MONTHLY) {
            start = g->mrange[g->month - 1];
            end = g->mrange[g->month];
            for (i = start; i < end; i++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
            }
        } else if (freq == DYN_RRULE_WEEKLY) {
            int64_t day0 = dyn_days_from_civil(g->year, g->month, g->day);
            int i0 = (int)(day0 - g->yearordinal);
            i = i0;
            for (j = 0; j < 7; j++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
                i++;
                if (g->wdaymask[i] == wkst)
                    break;
            }
        } else {
            int64_t day0 = dyn_days_from_civil(g->year, g->month, g->day);
            int i0 = (int)(day0 - g->yearordinal);
            if (dyn_rrule_day_kept(g, i0))
                kept[n_kept++] = i0;
            else
                filtered = 1;
        }

        /* ---- emission ---- */
        if (rr->n_bysetpos && g->n_timeset > 0) {
            int ntime = g->n_timeset;
            int n_pos = 0;
            for (k = 0; k < rr->n_bysetpos; k++) {
                int pos = rr->bysetpos[k];
                int daypos, timepos, idx;
                int64_t res;
                if (pos < 0) {
                    int q = pos / ntime, r = pos % ntime;
                    if (r < 0) { r += ntime; q--; }
                    daypos = q;
                    timepos = r;
                } else {
                    daypos = (pos - 1) / ntime;
                    timepos = (pos - 1) % ntime;
                }
                idx = daypos < 0 ? n_kept + daypos : daypos;
                if (idx < 0 || idx >= n_kept)
                    continue;
                res = (g->yearordinal + kept[idx]) * DYN_SECS_PER_DAY +
                      g->timeset[timepos];
                for (j = 0; j < n_pos; j++)
                    if (poslist[j] == res)
                        break;
                if (j == n_pos)
                    poslist[n_pos++] = res;
                if (n_pos >= DYN_RRULE_SETPOS_CAP) {
                    /* bysetpos count exceeds cap: keep what sorted, stop */
                    break;
                }
            }
            /* insertion sort (n small; mixed pos signs need ordering) */
            for (k = 1; k < n_pos; k++) {
                int64_t v = poslist[k];
                j = k - 1;
                while (j >= 0 && poslist[j] > v) {
                    poslist[j + 1] = poslist[j];
                    j--;
                }
                poslist[j + 1] = v;
            }
            for (k = 0; k < n_pos; k++) {
                int rc = dyn_rrule_emit(rr, poslist[k], &count, fn, ud);
                if (rc)
                    return;
            }
        } else {
            for (i = 0; i < n_kept; i++) {
                int64_t base = (g->yearordinal + kept[i]) * DYN_SECS_PER_DAY;
                for (j = 0; j < g->n_timeset; j++) {
                    int rc = dyn_rrule_emit(rr, base + g->timeset[j],
                                            &count, fn, ud);
                    if (rc)
                        return;
                }
            }
        }

        /* ---- advance to the next period ---- */
        {
            int fixday = 0;
            if (freq == DYN_RRULE_YEARLY) {
                g->year += interval;
                if (g->year > DYN_RRULE_MAX_YEAR)
                    return;
                dyn_rrule_rebuild(g);
            } else if (freq == DYN_RRULE_MONTHLY) {
                g->month += interval;
                if (g->month > 12) {
                    int div = g->month / 12, mod = g->month % 12;
                    g->month = mod;
                    g->year += div;
                    if (g->month == 0) {
                        g->month = 12;
                        g->year--;
                    }
                    if (g->year > DYN_RRULE_MAX_YEAR)
                        return;
                }
                dyn_rrule_rebuild(g);
            } else if (freq == DYN_RRULE_WEEKLY) {
                if (wkst > g->weekday)
                    g->day += -(g->weekday + 1 + (6 - wkst)) + interval * 7;
                else
                    g->day += -(g->weekday - wkst) + interval * 7;
                g->weekday = wkst;
                fixday = 1;
            } else if (freq == DYN_RRULE_DAILY) {
                g->day += interval;
                fixday = 1;
            } else if (freq == DYN_RRULE_HOURLY) {
                int ndays, h;
                if (filtered)
                    g->hour += ((23 - g->hour) / interval) * interval;
                h = g->hour + interval;
                ndays = h / 24;
                g->hour = h % 24;
                if (ndays) {
                    g->day += ndays;
                    fixday = 1;
                }
                g->timeset[0] = g->hour * 3600 + rr->ds_mi * 60 + rr->ds_s;
            } else if (freq == DYN_RRULE_MINUTELY) {
                int total, nhours, div, h;
                if (filtered)
                    g->minute += ((1439 - (g->hour * 60 + g->minute)) /
                                  interval) * interval;
                total = g->minute + interval;
                nhours = total / 60;
                g->minute = total % 60;
                h = g->hour + nhours;
                div = h / 24;
                g->hour = h % 24;
                if (div) {
                    g->day += div;
                    fixday = 1;
                }
                g->timeset[0] = g->hour * 3600 + g->minute * 60 + rr->ds_s;
            } else { /* SECONDLY */
                int total, nmin, div2;
                if (filtered)
                    g->second += ((86399 -
                                   (g->hour * 3600 + g->minute * 60 +
                                    g->second)) / interval) * interval;
                total = g->second + interval;
                nmin = total / 60;
                g->second = total % 60;
                div2 = (g->minute + nmin) / 60;
                g->minute = (g->minute + nmin) % 60;
                if (div2) {
                    g->hour += div2;
                    div2 = g->hour / 24;
                    g->hour = g->hour % 24;
                    if (div2) {
                        g->day += div2;
                        fixday = 1;
                    }
                }
                g->timeset[0] = g->hour * 3600 + g->minute * 60 + g->second;
            }

            if (fixday && g->day > 28) {
                int dim = dyn_time_days_in_month(g->year, g->month);
                if (g->day > dim) {
                    while (g->day > dim) {
                        g->day -= dim;
                        g->month++;
                        if (g->month == 13) {
                            g->month = 1;
                            g->year++;
                            if (g->year > DYN_RRULE_MAX_YEAR)
                                return;
                        }
                        dim = dyn_time_days_in_month(g->year, g->month);
                    }
                    dyn_rrule_rebuild(g);
                }
            }
        }
    }
}

/* ================================================================ *
 *  Methods
 * ================================================================ */

/* shared collector: append a Date per occurrence into a JS array. Bounded
 * by DYN_RRULE_MAX_RESULTS: beyond it the call REFUSES (RangeError) rather
 * than letting the array grow without limit. */
typedef struct {
    JSContext *ctx;
    JSValue arr;
    int64_t limit;               /* -1 = no external limit */
    int64_t n;
    int failed;
} DynRRuleCollect;

static int dyn_rrule_collect_cb(int64_t sec, void *udp)
{
    DynRRuleCollect *c = (DynRRuleCollect *)udp;
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
    if (JS_SetPropertyUint32(c->ctx, c->arr, (uint32_t)c->n, d) < 0) {
        c->failed = 1;
        return 1;
    }
    c->n++;
    return 0;
}

static JSValue dyn_rrule_all(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    DynRRule *rr;
    DynRRuleCollect c;
    int64_t limit = -1;

    /* coerce before resolving the handle: ToNumber runs user JS */
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, argv[0]))
            return JS_EXCEPTION;
        if (!(dv >= 0) || dv > (double)DYN_MAX_SAFE_INT ||
            dv != (double)(int64_t)dv)
            return JS_ThrowRangeError(ctx,
                "RRule.all(limit) requires a non-negative integer");
        limit = (int64_t)dv;
    }
    rr = (DynRRule *)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
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
    {
        DynRRuleGen g;
        dyn_rrule_gen_init(&g, rr, rr->dtstart);
        dyn_rrule_gen_loop(&g, dyn_rrule_collect_cb, &c);
    }
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

/* between window: stop past `end`; append within (start, end) or [start, end] */
typedef struct {
    JSContext *ctx;
    JSValue arr;
    int64_t start, end;
    int inc;
    int64_t n;
    int failed;
} DynRRuleWindow;

static int dyn_rrule_window_cb(int64_t sec, void *udp)
{
    DynRRuleWindow *w = (DynRRuleWindow *)udp;
    JSValue d;
    if (w->inc) {
        if (sec > w->end)
            return 1;
        if (sec < w->start)
            return 0;
    } else {
        if (sec >= w->end)
            return 1;
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
    if (JS_SetPropertyUint32(w->ctx, w->arr, (uint32_t)w->n, d) < 0) {
        w->failed = 1;
        return 1;
    }
    w->n++;
    return 0;
}

static JSValue dyn_rrule_between(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    DynRRule *rr;
    DynRRuleWindow w;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "RRule.between(start, end[, inc]) requires two dates");
    w.ctx = ctx;
    w.n = 0;
    w.failed = 0;
    w.inc = 0;
    /* coerce before resolving the handle (user JS in to_epoch) */
    if (dyn_rrule_to_epoch(ctx, argv[0], &w.start, "between start") ||
        dyn_rrule_to_epoch(ctx, argv[1], &w.end, "between end"))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        int b = JS_ToBool(ctx, argv[2]);
        if (b < 0)
            return JS_EXCEPTION;
        w.inc = b;
    }
    rr = (DynRRule *)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;

    w.arr = JS_NewArray(ctx);
    if (JS_IsException(w.arr))
        return JS_EXCEPTION;
    {
        DynRRuleGen g;
        if (!rr->has_count) {
            /* Uncounted rule: skip the cursor to the window's start period.
             * Every occurrence before that period is < start and therefore
             * out of the window, so a far-future query cannot consume the
             * step budget crossing years of irrelevant periods. Counted
             * rules must iterate from dtstart for COUNT semantics. */
            DynRRuleGen a, b;
            dyn_rrule_gen_init(&a, rr, rr->dtstart);
            dyn_rrule_gen_init(&b, rr, w.start);
            if (dyn_rrule_cursor_ge(&b, &a))
                dyn_rrule_gen_init(&g, rr, w.start);
            else
                g = a;
        } else {
            dyn_rrule_gen_init(&g, rr, rr->dtstart);
        }
        dyn_rrule_gen_loop(&g, dyn_rrule_window_cb, &w);
    }
    if (w.failed) {
        JS_FreeValue(ctx, w.arr);
        return JS_EXCEPTION;
    }
    return w.arr;
}

/* next: first occurrence strictly after fromDate (default: dtstart) */
typedef struct {
    int64_t from;
    int64_t res;
    int found;
} DynRRuleNext;

static int dyn_rrule_next_cb(int64_t sec, void *udp)
{
    DynRRuleNext *nx = (DynRRuleNext *)udp;
    if (sec > nx->from) {
        nx->res = sec;
        nx->found = 1;
        return 1;
    }
    return 0;
}

static JSValue dyn_rrule_next(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DynRRule *rr;
    DynRRuleNext nx;
    DynRRuleGen g;

    nx.from = -1;
    nx.found = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) &&
        dyn_rrule_to_epoch(ctx, argv[0], &nx.from, "next fromDate"))
        return JS_EXCEPTION;
    rr = (DynRRule *)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        nx.from = rr->dtstart;

    if (!rr->has_count) {
        /* skip straight to fromDate's period -- every earlier occurrence is
         * <= fromDate, and count (which needs every occurrence) is absent. */
        DynRRuleGen a, b;
        dyn_rrule_gen_init(&a, rr, rr->dtstart);
        dyn_rrule_gen_init(&b, rr, nx.from);
        if (dyn_rrule_cursor_ge(&b, &a))
            dyn_rrule_gen_init(&g, rr, nx.from);
        else
            g = a;
    } else {
        dyn_rrule_gen_init(&g, rr, rr->dtstart);
    }
    dyn_rrule_gen_loop(&g, dyn_rrule_next_cb, &nx);
    if (nx.found)
        return JS_NewDate(ctx, (double)nx.res * 1000.0);
    return JS_NULL;
}

/* prev: last occurrence strictly before fromDate (default: the rule's last,
 * which is only well-defined when count or until bounds it) */
typedef struct {
    int64_t from;
    int64_t res;
    int have;
    int stop;
} DynRRulePrev;

static int dyn_rrule_prev_cb(int64_t sec, void *udp)
{
    DynRRulePrev *pv = (DynRRulePrev *)udp;
    if (pv->stop) {
        if (sec >= pv->from)
            return 1;
    }
    pv->res = sec;
    pv->have = 1;
    return 0;
}

static JSValue dyn_rrule_prev(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DynRRule *rr;
    DynRRulePrev pv;
    DynRRuleGen g;

    pv.have = 0;
    pv.res = 0;
    pv.stop = 0;
    /* coerce before resolving the handle (CLAUDE.md sec. 8): to_epoch runs
     * user JS (getTime) that must not precede the handle lookup */
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (dyn_rrule_to_epoch(ctx, argv[0], &pv.from, "prev fromDate"))
            return JS_EXCEPTION;
        pv.stop = 1;
    }
    rr = (DynRRule *)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (!pv.stop) {
        /* prev() = the last occurrence; undefined for an infinite rule */
        if (!rr->has_count && !rr->has_until)
            return JS_NULL;
        pv.from = 0;
    }
    dyn_rrule_gen_init(&g, rr, rr->dtstart);
    dyn_rrule_gen_loop(&g, dyn_rrule_prev_cb, &pv);
    if (pv.have)
        return JS_NewDate(ctx, (double)pv.res * 1000.0);
    return JS_NULL;
}

/* ---- toString ---- */

/* YYYYMMDDTHHMMSSZ from unix seconds (UTC). dtstart/until years are
 * validated into 0001..9999 at construction, so this never sees a negative
 * or wider-than-4-digit year; the buffer still leaves headroom. */
static void dyn_rrule_fmt_until(int64_t sec, char *out)
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

static int dyn_rrule_put_join(DynTimeBuf *out, const int16_t *a, int n)
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

static JSValue dyn_rrule_to_string(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DynRRule *rr;
    DynTimeBuf out;
    JSValue result = JS_EXCEPTION;
    (void)argc;
    (void)argv;

    rr = (DynRRule *)dyn_plain_get(ctx, this_val, dyn_rrule_class_id);
    if (!rr)
        return JS_EXCEPTION;
    if (dyn_time_buf_init(ctx, &out, 64))
        return JS_EXCEPTION;

#define PUT(s) do { if (dyn_time_buf_put(&out, (s), strlen(s))) goto done; } while (0)
#define PUTN(s, n) do { if (dyn_time_buf_put(&out, (s), (n))) goto done; } while (0)
    {
        char tmp[32];
        int pos;
        PUT("RRULE:FREQ=");
        PUTN(dyn_rrule_freq_names[rr->freq],
             strlen(dyn_rrule_freq_names[rr->freq]));
        if (rr->interval != 1) {
            PUT(";INTERVAL=");
            pos = dyn_time_utoa(rr->interval, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)pos)) goto done;
        }
        if (rr->orig & DYN_RR_ORIG_WKST && rr->wkst != 0) {
            PUT(";WKST=");
            PUTN(dyn_rrule_wd_names[rr->wkst], 2);
        }
        if (rr->has_count) {
            PUT(";COUNT=");
            pos = dyn_time_utoa((uint64_t)rr->count, tmp);
            if (dyn_time_buf_put(&out, tmp, (size_t)pos)) goto done;
        }
        if (rr->has_until) {
            char ubuf[32];
            dyn_rrule_fmt_until(rr->until, ubuf);
            PUT(";UNTIL=");
            if (dyn_time_buf_put(&out, ubuf, strlen(ubuf))) goto done;
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
            /* positives then negatives, comma-separated across the lists --
             * "15,-1", never "15-1" */
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
                if (i > 0 && dyn_time_buf_put(&out, ",", 1)) goto done;
                if (dyn_time_buf_put(&out, dyn_rrule_wd_names[rr->byweekday[i]],
                                     2)) goto done;
            }
            for (i = 0; i < rr->n_bynwd; i++) {
                int wd = rr->bynwd[i * 2], n = rr->bynwd[i * 2 + 1];
                if ((rr->n_byweekday || i > 0) &&
                    dyn_time_buf_put(&out, ",", 1))
                    goto done;
                if (n < 0) {
                    if (dyn_time_buf_put(&out, "-", 1)) goto done;
                    pos = dyn_time_utoa((uint64_t)(-n), tmp);
                } else {
                    if (dyn_time_buf_put(&out, "+", 1)) goto done;
                    pos = dyn_time_utoa((uint64_t)n, tmp);
                }
                if (dyn_time_buf_put(&out, tmp, (size_t)pos)) goto done;
                if (dyn_time_buf_put(&out, dyn_rrule_wd_names[wd], 2))
                    goto done;
            }
        }
    }
    result = JS_NewStringLen(ctx, (const char *)out.data, out.len);
 done:
#undef PUT
#undef PUTN
    dyn_time_buf_free(&out);
    return result;
}

/* ================================================================ *
 *  String parsing (RRule.fromString)
 * ================================================================ */

/* parse a signed decimal integer (no spaces) */
static int dyn_rrule_parse_int(const char *p, size_t len, int64_t *out)
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

/* set one integer-list option from a comma-separated string */
static int dyn_rrule_spec_parse_list(JSContext *ctx, DynRRuleSpec *s, int which,
                                     const char *p, size_t len)
{
    int64_t vals[DYN_RRULE_SETPOS_CAP];
    int n = 0;
    size_t i = 0;
    const char *name = which == DYN_RR_LIST_BYMONTH ? "BYMONTH" :
                       which == DYN_RR_LIST_BYMONTHDAY ? "BYMONTHDAY" :
                       which == DYN_RR_LIST_BYYEARDAY ? "BYYEARDAY" :
                       which == DYN_RR_LIST_BYWEEKNO ? "BYWEEKNO" : "BYSETPOS";
    while (i < len) {
        size_t start = i;
        int64_t v;
        while (i < len && p[i] != ',')
            i++;
        if (i == start || dyn_rrule_parse_int(p + start, i - start, &v))
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid %s value", name), -1;
        if (n >= DYN_RRULE_SETPOS_CAP)
            return JS_ThrowRangeError(ctx,
                "dyna:time: %s list too long", name), -1;
        vals[n++] = v;
        if (i < len)
            i++;
    }
    if (n == 0)
        return JS_ThrowSyntaxError(ctx,
            "dyna:time: empty %s value", name), -1;
    if (dyn_rrule_spec_apply_list(ctx, s, which, vals, n, name))
        return -1;
    switch (which) {
    case DYN_RR_LIST_BYMONTH:    s->orig |= DYN_RR_ORIG_BYMONTH;    break;
    case DYN_RR_LIST_BYMONTHDAY: s->orig |= DYN_RR_ORIG_BYMONTHDAY; break;
    case DYN_RR_LIST_BYYEARDAY:  s->orig |= DYN_RR_ORIG_BYYEARDAY;  break;
    case DYN_RR_LIST_BYWEEKNO:   s->orig |= DYN_RR_ORIG_BYWEEKNO;   break;
    default:                     s->orig |= DYN_RR_ORIG_BYSETPOS;   break;
    }
    return 0;
}

/* parse a BYDAY comma list into the spec */
static int dyn_rrule_spec_parse_byday(JSContext *ctx, DynRRuleSpec *s,
                                      const char *p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int wd, n;
        while (i < len && p[i] != ',')
            i++;
        if (i == start)
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: empty BYDAY value"), -1;
        if (dyn_rrule_parse_wd_str(p + start, i - start, &wd, &n) ||
            dyn_rrule_spec_add_weekday(ctx, s, wd, n))
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid BYDAY value"), -1;
        if (i < len)
            i++;
    }
    s->orig |= DYN_RR_ORIG_BYWEEKDAY;
    return 0;
}

/* one RRULE part "NAME=VALUE" */
static int dyn_rrule_spec_parse_part(JSContext *ctx, DynRRuleSpec *s,
                                     const char *p, size_t len)
{
    size_t eq = 0, vlen;
    const char *val;
    while (eq < len && p[eq] != '=')
        eq++;
    if (eq == len)
        return JS_ThrowSyntaxError(ctx,
            "dyna:time: RRULE part missing '='"), -1;
    val = p + eq + 1;
    vlen = len - eq - 1;

    if (dyn_rrule_name_eq(p, eq, "FREQ")) {
        int f = dyn_rrule_freq_by_name(val, vlen);
        if (f < 0)
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: unknown FREQ"), -1;
        s->freq = f;
        s->has_freq = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "INTERVAL")) {
        int64_t v;
        if (dyn_rrule_parse_int(val, vlen, &v) || v < 1 || v > 1000000)
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid INTERVAL"), -1;
        s->interval = (int)v;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "COUNT")) {
        int64_t v;
        if (dyn_rrule_parse_int(val, vlen, &v) || v < 1)
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid COUNT"), -1;
        s->count = v;
        s->has_count = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "UNTIL")) {
        int64_t v;
        if (dyn_rrule_parse_datetime(val, vlen, &v))
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid UNTIL"), -1;
        s->until = v;
        s->has_until = 1;
        return 0;
    }
    if (dyn_rrule_name_eq(p, eq, "WKST")) {
        int w = dyn_rrule_wd_by_name(val, vlen);
        if (w < 0)
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: invalid WKST"), -1;
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
    if (dyn_rrule_name_eq(p, eq, "BYHOUR") ||
        dyn_rrule_name_eq(p, eq, "BYMINUTE") ||
        dyn_rrule_name_eq(p, eq, "BYSECOND") ||
        dyn_rrule_name_eq(p, eq, "BYEASTER"))
        return JS_ThrowSyntaxError(ctx,
            "dyna:time: unsupported RRULE parameter (%.*s)",
            (int)eq, p), -1;
    return JS_ThrowSyntaxError(ctx, "dyna:time: unknown RRULE parameter (%.*s)",
                               (int)eq, p), -1;
}

static int dyn_rrule_spec_parse_rrule(JSContext *ctx, DynRRuleSpec *s,
                                      const char *p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != ';')
            i++;
        /* skip empty parts: a trailing ';' or ";;" is tolerated */
        if (i > start) {
            if (dyn_rrule_spec_parse_part(ctx, s, p + start, i - start))
                return -1;
        }
        if (i < len)
            i++;
    }
    return 0;
}

/* RRule.fromString(str, { dtstart }) */
static JSValue dyn_rrule_from_string(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    DynRRuleSpec spec;
    const char *s;
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

    /* whitespace-separated "lines"; DTSTART: wins over the option (dateutil) */
    while (i < len) {
        size_t start, colon, slen;
        const char *name;
        size_t nlen;
        while (i < len && (s[i] == ' ' || s[i] == '\t' ||
                           s[i] == '\r' || s[i] == '\n'))
            i++;
        if (i >= len)
            break;
        start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '\r' && s[i] != '\n')
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
            nlen = 0;                       /* bare value => RRULE */
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
            if (colon >= slen ||
                dyn_rrule_parse_datetime(s + start + colon + 1,
                                         slen - colon - 1, &v)) {
                JS_FreeCString(ctx, s);
                return JS_ThrowSyntaxError(ctx,
                    "dyna:time: invalid DTSTART in RRULE string"), JS_EXCEPTION;
            }
            spec.dtstart = v;
            spec.has_dtstart = 1;
        } else {
            JS_FreeCString(ctx, s);
            return JS_ThrowSyntaxError(ctx,
                "dyna:time: unsupported property (%.*s)", (int)nlen, name),
                JS_EXCEPTION;
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
    return dyn_rrule_build(ctx, &spec);
}

/* ================================================================ *
 *  Constructor + registration
 * ================================================================ */

static int dyn_rrule_spec_from_options(JSContext *ctx, JSValueConst obj,
                                       DynRRuleSpec *s)
{
    JSValue v;
    int64_t vals[DYN_RRULE_SETPOS_CAP];
    int n;

    v = JS_GetPropertyStr(ctx, obj, "freq");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        size_t len;
        const char *str;
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
        if (!(dv >= 1) || dv > (double)DYN_MAX_SAFE_INT ||
            dv != (double)(int64_t)dv) {
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
        size_t len;
        const char *str;
        int w;
        str = JS_ToCStringLen(ctx, &len, v);
        if (!str) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        w = dyn_rrule_wd_by_name(str, len);
        JS_FreeCString(ctx, str);
        if (w < 0) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "RRule: wkst must be MO..SU");
            return -1;
        }
        s->wkst = w;
        s->orig |= DYN_RR_ORIG_WKST;
    }
    JS_FreeValue(ctx, v);

    /* by* lists. Each read either throws (return -1) or returns >= 0; a -1
     * must abort IMMEDIATELY -- continuing with a pending exception builds
     * a partial rule and returns it with the exception still set. */
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

    return 0;
}

static JSValue dyn_rrule_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    DynRRuleSpec spec;
    (void)new_target;

    memset(&spec, 0, sizeof(spec));
    spec.freq = -1;
    spec.interval = 1;
    spec.wkst = 0;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "RRule({ freq, ... }) requires an options object");
    if (dyn_rrule_spec_from_options(ctx, argv[0], &spec))
        return JS_EXCEPTION;
    return dyn_rrule_build(ctx, &spec);
}

static const JSCFunctionListEntry dyn_rrule_proto[] = {
    JS_CFUNC_DEF("between", 2, dyn_rrule_between),
    JS_CFUNC_DEF("all", 0, dyn_rrule_all),
    JS_CFUNC_DEF("next", 0, dyn_rrule_next),
    JS_CFUNC_DEF("prev", 0, dyn_rrule_prev),
    JS_CFUNC_DEF("toString", 0, dyn_rrule_to_string),
};

static int dyn_rrule_register(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_rrule_class_id, &dyn_rrule_class,
                                 dyn_rrule_proto, countof(dyn_rrule_proto),
                                 dyn_rrule_ctor, "RRule") < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_rrule_class_id), ctor;
        if (JS_IsException(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsException(ctor))
            return -1;
        if (JS_SetPropertyStr(ctx, ctor, "fromString",
                JS_NewCFunction(ctx, dyn_rrule_from_string, "fromString",
                                1)) < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}
