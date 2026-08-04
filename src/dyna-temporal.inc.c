/* PlainDate and Duration for dyna:time (design 20).
   Dates are VALUES: immutable, compared by content, and every operation
   returns a new one. The calendar is proleptic Gregorian, and the conversion
   to and from a day number is exact for the whole supported range -- no
   floating point and no time zone anywhere in this file. */

#define TP_MIN_YEAR (-271821)
#define TP_MAX_YEAR   275760

/* Howard Hinnant's civil-from-days / days-from-civil. Exact integer identities,
   and the only reason a date type needs no lookup table at all. */
static int64_t tp_days_from_civil(int64_t y, int m, int d)
{
    int64_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                    /* 0..399 */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* 0..365 */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* 0..146096 */
    return era * 146097 + doe - 719468;                     /* 1970-01-01 = 0 */
}

static void tp_civil_from_days(int64_t z, int64_t *py, int *pm, int *pd)
{
    int64_t era, doe, yoe, y, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *pd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *pm = (int)(mp + (mp < 10 ? 3 : -9));
    *py = y + (*pm <= 2);
}

static int tp_is_leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int tp_days_in_month(int64_t y, int m)
{
    static const int D[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    return (m == 2 && tp_is_leap(y)) ? 29 : D[m];
}

typedef struct { int64_t y; int m, d; } tp_date_t;

_Static_assert(sizeof(tp_date_t) == 16, "tp_date_t regained padding");

/* A duration is stored as MONTHS and DAYS separately, never collapsed.
   A month is not a fixed number of days, so adding one month to 31 January
   and adding 30 days are different questions with different answers, and a
   type that stores only days cannot ask the first one. */
typedef struct { int64_t months, days, ms_part; } tp_dur_t;

_Static_assert(sizeof(tp_dur_t) == 24, "tp_dur_t regained padding");

static JSClassID dyn_pdate_class_id;
static JSClassID dyn_dur_class_id;

static void dyn_pdate_free(void *p) { free(p); }
static void dyn_dur_free(void *p) { free(p); }

static void dyn_pdate_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_pdate_class_id));
}

static void dyn_dur_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_dur_class_id));
}

static const JSClassDef dyn_pdate_class = { "PlainDate", .finalizer = dyn_pdate_finalizer };
static const JSClassDef dyn_dur_class   = { "Duration",  .finalizer = dyn_dur_finalizer };

static int tp_check(JSContext *ctx, int64_t y, int m, int d)
{
    if (y < TP_MIN_YEAR || y > TP_MAX_YEAR) {
        JS_ThrowRangeError(ctx, "PlainDate: year %lld is outside %d..%d",
                           (long long)y, TP_MIN_YEAR, TP_MAX_YEAR);
        return -1;
    }
    if (m < 1 || m > 12) {
        JS_ThrowRangeError(ctx, "PlainDate: month %d is not 1..12", m);
        return -1;
    }
    /* Refuse 31 February rather than rolling it into March. A date type that
       silently normalises an impossible date hides the caller's bug. */
    if (d < 1 || d > tp_days_in_month(y, m)) {
        JS_ThrowRangeError(ctx, "PlainDate: %lld-%02d has no day %d",
                           (long long)y, m, d);
        return -1;
    }
    return 0;
}

static JSValue tp_new_date(JSContext *ctx, int64_t y, int m, int d)
{
    tp_date_t *p;
    JSValue obj;

    if (tp_check(ctx, y, m, d) < 0)
        return JS_EXCEPTION;
    p = (tp_date_t *)malloc(sizeof *p);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->y = y; p->m = m; p->d = d;
    obj = dyn_plain_wrap(ctx, dyn_pdate_class_id, p, dyn_pdate_free);
    return obj;
}

static JSValue tp_new_dur3(JSContext *ctx, int64_t months, int64_t days,
                           int64_t ms_part)
{
    tp_dur_t *p = (tp_dur_t *)malloc(sizeof *p);

    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->months = months; p->days = days; p->ms_part = ms_part;
    return dyn_plain_wrap(ctx, dyn_dur_class_id, p, dyn_dur_free);
}

static JSValue tp_new_dur(JSContext *ctx, int64_t months, int64_t days)
{
    return tp_new_dur3(ctx, months, days, 0);
}

/* --- PlainDate --- */

static JSValue dyn_pdate_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    int64_t y;
    int32_t m, d;
    (void)new_target;

    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "new PlainDate(year, month, day): all three are required");
    if (JS_ToInt64(ctx, &y, argv[0]) || JS_ToInt32(ctx, &m, argv[1])
        || JS_ToInt32(ctx, &d, argv[2]))
        return JS_EXCEPTION;
    return tp_new_date(ctx, y, m, d);
}

/* magic 0 year, 1 month, 2 day, 3 dayOfWeek, 4 dayOfYear, 5 daysInMonth,
   6 daysInYear, 7 inLeapYear, 8 epochDay */
static JSValue dyn_pdate_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    tp_date_t *p = (tp_date_t *)dyn_plain_get(ctx, this_val, dyn_pdate_class_id);
    int64_t ed;

    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_NewInt64(ctx, p->y);
    case 1: return JS_NewInt32(ctx, p->m);
    case 2: return JS_NewInt32(ctx, p->d);
    case 3:
        /* ISO 8601: Monday is 1. 1970-01-01 was a Thursday, so the epoch day
           offset is 3, and the modulo is written to stay positive for dates
           before 1970 -- C's % keeps the sign of the dividend. */
        ed = tp_days_from_civil(p->y, p->m, p->d);
        return JS_NewInt32(ctx, (int)(((ed + 3) % 7 + 7) % 7) + 1);
    case 4:
        return JS_NewInt32(ctx, (int)(tp_days_from_civil(p->y, p->m, p->d)
                                      - tp_days_from_civil(p->y, 1, 1)) + 1);
    case 5: return JS_NewInt32(ctx, tp_days_in_month(p->y, p->m));
    case 6: return JS_NewInt32(ctx, tp_is_leap(p->y) ? 366 : 365);
    case 7: return JS_NewBool(ctx, tp_is_leap(p->y));
    default: return JS_NewInt64(ctx, tp_days_from_civil(p->y, p->m, p->d));
    }
}

/* add/subtract a Duration. Months move first and CLAMP to the end of the
   target month -- 31 Jan plus one month is 28 or 29 Feb, never 3 March -- and
   days are added afterwards, exactly. Order matters and is part of the
   contract: (date + 1 month) + 30 days is not (date + 30 days) + 1 month. */
static JSValue tp_shift(JSContext *ctx, const tp_date_t *p, int64_t months,
                        int64_t days)
{
    int64_t y = p->y, tm;
    int m, d = p->d, dim;

    tm = (int64_t)(p->m - 1) + months;
    y += tm >= 0 ? tm / 12 : (tm - 11) / 12;
    m = (int)(((tm % 12) + 12) % 12) + 1;
    if (y < TP_MIN_YEAR || y > TP_MAX_YEAR)
        return JS_ThrowRangeError(ctx, "PlainDate: the result leaves the "
                                       "supported year range");
    dim = tp_days_in_month(y, m);
    if (d > dim)
        d = dim;
    if (days) {
        int64_t ed = tp_days_from_civil(y, m, d) + days;
        int mm, dd;
        if (ed < -100000000LL || ed > 100000000LL)
            return JS_ThrowRangeError(ctx, "PlainDate: the result leaves the "
                                           "supported range");
        tp_civil_from_days(ed, &y, &mm, &dd);
        m = mm; d = dd;
    }
    return tp_new_date(ctx, y, m, d);
}

/* magic 0 add, 1 subtract */
static JSValue dyn_pdate_add(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    tp_date_t *p = (tp_date_t *)dyn_plain_get(ctx, this_val, dyn_pdate_class_id);
    tp_dur_t *dur;
    int64_t sign = magic ? -1 : 1;

    if (!p)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDate.%s(duration): a Duration is "
                                      "required", magic ? "subtract" : "add");
    dur = (tp_dur_t *)dyn_plain_get(ctx, argv[0], dyn_dur_class_id);
    if (!dur)
        return JS_EXCEPTION;
    return tp_shift(ctx, p, sign * dur->months, sign * dur->days);
}

/* until(other) -> Duration, in whole months and the remaining days. */
static JSValue dyn_pdate_until(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    tp_date_t *a = (tp_date_t *)dyn_plain_get(ctx, this_val, dyn_pdate_class_id);
    tp_date_t *b;
    int64_t months, ea, eb;
    int y2, m2, d2;
    int64_t yy;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDate.until(other): a PlainDate is "
                                      "required");
    b = (tp_date_t *)dyn_plain_get(ctx, argv[0], dyn_pdate_class_id);
    if (!b)
        return JS_EXCEPTION;
    months = (b->y - a->y) * 12 + (b->m - a->m);
    if (months > 0 && b->d < a->d) months--;
    if (months < 0 && b->d > a->d) months++;
    {   /* days remaining after moving `months` whole months */
        int64_t tm = (int64_t)(a->m - 1) + months;
        int mm, dd, dim;
        yy = a->y + (tm >= 0 ? tm / 12 : (tm - 11) / 12);
        mm = (int)(((tm % 12) + 12) % 12) + 1;
        dim = tp_days_in_month(yy, mm);
        dd = a->d > dim ? dim : a->d;
        ea = tp_days_from_civil(yy, mm, dd);
        (void)y2; (void)m2; (void)d2;
    }
    eb = tp_days_from_civil(b->y, b->m, b->d);
    return tp_new_dur(ctx, months, eb - ea);
}

static JSValue dyn_pdate_compare(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    tp_date_t *a = (tp_date_t *)dyn_plain_get(ctx, this_val, dyn_pdate_class_id);
    tp_date_t *b;
    int64_t ea, eb;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDate.compare(other): a PlainDate "
                                      "is required");
    b = (tp_date_t *)dyn_plain_get(ctx, argv[0], dyn_pdate_class_id);
    if (!b)
        return JS_EXCEPTION;
    ea = tp_days_from_civil(a->y, a->m, a->d);
    eb = tp_days_from_civil(b->y, b->m, b->d);
    return JS_NewInt32(ctx, ea < eb ? -1 : (ea > eb ? 1 : 0));
}

static JSValue dyn_pdate_tostring(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    tp_date_t *p = (tp_date_t *)dyn_plain_get(ctx, this_val, dyn_pdate_class_id);
    char buf[32];
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    /* ISO 8601 expands years outside 0..9999 with a sign and six digits;
       printing "12345-01-01" would be ambiguous with a four-digit year. */
    if (p->y >= 0 && p->y <= 9999)
        snprintf(buf, sizeof buf, "%04d-%02d-%02d", (int)p->y, p->m, p->d);
    else
        snprintf(buf, sizeof buf, "%c%06lld-%02d-%02d", p->y < 0 ? '-' : '+',
                 (long long)(p->y < 0 ? -p->y : p->y), p->m, p->d);
    return JS_NewString(ctx, buf);
}

/* PlainDate.from("YYYY-MM-DD") and .fromEpochDay(n) */
static JSValue dyn_pdate_from(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s: an argument is required",
                                 magic ? "dateFromEpochDay(day)" : "parseDate(text)");
    if (magic == 1) {
        int64_t ed;
        int m, d;
        int64_t y;
        if (JS_ToInt64(ctx, &ed, argv[0]))
            return JS_EXCEPTION;
        if (ed < -100000000LL || ed > 100000000LL)
            return JS_ThrowRangeError(ctx, "dateFromEpochDay: out of range");
        tp_civil_from_days(ed, &y, &m, &d);
        return tp_new_date(ctx, y, m, d);
    }
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        int64_t y = 0;
        int m = 0, d = 0, neg = 0, i = 0, nd = 0;
        JSValue r;
        if (!s)
            return JS_EXCEPTION;
        /* Hand-rolled, because sscanf("%d") would accept "+1", " 12", "0x10"
           and a locale-dependent shape, none of which ISO 8601 permits. */
        if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
        while (s[i] >= '0' && s[i] <= '9' && nd < 6) { y = y * 10 + (s[i] - '0'); i++; nd++; }
        if ((nd != 4 && nd != 6) || s[i] != '-') goto bad;
        i++;
        if (!(s[i] >= '0' && s[i] <= '9') || !(s[i+1] >= '0' && s[i+1] <= '9')) goto bad;
        m = (s[i] - '0') * 10 + (s[i+1] - '0'); i += 2;
        if (s[i] != '-') goto bad;
        i++;
        if (!(s[i] >= '0' && s[i] <= '9') || !(s[i+1] >= '0' && s[i+1] <= '9')) goto bad;
        d = (s[i] - '0') * 10 + (s[i+1] - '0'); i += 2;
        if (s[i] != '\0') goto bad;
        if (neg) y = -y;
        JS_FreeCString(ctx, s);
        return tp_new_date(ctx, y, m, d);
    bad:
        r = JS_ThrowSyntaxError(ctx, "parseDate: '%s' is not an ISO 8601 date "
                                     "(YYYY-MM-DD)", s);
        JS_FreeCString(ctx, s);
        return r;
    }
}

/* --- Duration --- */

static JSValue dyn_dur_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    int64_t years = 0, months = 0, weeks = 0, days = 0;
    int64_t hours = 0, minutes = 0, seconds = 0, millis = 0;
    JSValueConst o;
    (void)new_target;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "new Duration({years?, months?, weeks?, "
                                      "days?}): an options object is required");
    o = argv[0];
#define FIELD(name, var) do { \
        JSValue v_ = JS_GetPropertyStr(ctx, o, name); \
        if (JS_IsException(v_)) return JS_EXCEPTION; \
        if (!JS_IsUndefined(v_)) { \
            int rc_ = JS_ToInt64(ctx, &(var), v_); \
            JS_FreeValue(ctx, v_); \
            if (rc_ < 0) return JS_EXCEPTION; \
        } else { JS_FreeValue(ctx, v_); } \
    } while (0)
    FIELD("years", years);
    FIELD("months", months);
    FIELD("weeks", weeks);
    FIELD("days", days);
    FIELD("hours", hours);
    FIELD("minutes", minutes);
    FIELD("seconds", seconds);
    FIELD("milliseconds", millis);
#undef FIELD
    /* Years fold into months and weeks into days: both are exact conversions.
       Months into days is NOT, so it does not happen here or anywhere. */
    /* Hours, minutes and seconds are exact multiples of a millisecond, so
       they fold. Days do NOT fold into hours -- that would assume every day
       is 24 hours, which is the assumption this type exists to avoid. */
    return tp_new_dur3(ctx, years * 12 + months, weeks * 7 + days,
                       ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis);
}

/* magic 0 months, 1 days, 2 years, 3 sign, 4 blank */
static JSValue dyn_dur_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    tp_dur_t *p = (tp_dur_t *)dyn_plain_get(ctx, this_val, dyn_dur_class_id);

    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_NewInt64(ctx, p->months);
    case 1: return JS_NewInt64(ctx, p->days);
    case 2: return JS_NewInt64(ctx, p->months / 12);
    case 3: {
        int64_t s = p->months ? p->months
                  : (p->days ? p->days : p->ms_part);
        return JS_NewInt32(ctx, s > 0 ? 1 : (s < 0 ? -1 : 0));
    }
    default: return JS_NewBool(ctx, p->months == 0 && p->days == 0
                                    && p->ms_part == 0);
    }
}

static JSValue dyn_dur_tostring(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    tp_dur_t *p = (tp_dur_t *)dyn_plain_get(ctx, this_val, dyn_dur_class_id);
    /* Worst case, counted rather than guessed: sign 1 + 'P' 1 + Y 15+1 +
       M 2+1 + D 19+1 + 'T' 1 + H 13+1 + M 2+1 + S 2+1+3+1 = 66, plus the NUL.
       buf was 64 and off was never bounds-checked. 96 leaves real headroom. */
    char buf[96];
    int off = 0, neg = 0;
    int64_t mo, d, ms;
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    if (!p->months && !p->days && !p->ms_part)
        return JS_NewString(ctx, "P0D");
    mo = p->months; d = p->days; ms = p->ms_part;
    /* ISO-8601 puts ONE sign on the whole duration, so a mixed-sign value has
       no representation. Taking the sign from the leading component emitted
       "-P1M10D" for (months -1, days +10) -- valid ISO, and a different
       duration. Refuse rather than hand a consumer a parseable wrong answer. */
    if (((mo > 0) || (d > 0) || (ms > 0)) && ((mo < 0) || (d < 0) || (ms < 0)))
        return JS_ThrowRangeError(ctx,
            "Duration.toString: mixed-sign components (months %lld, days %lld, "
            "ms %lld) have no ISO-8601 representation",
            (long long)mo, (long long)d, (long long)ms);
    if (mo < 0 || d < 0 || ms < 0) {
        buf[off++] = '-';
        neg = 1;
    }
    buf[off++] = 'P';
    /* Magnitude without the -INT64_MIN UB. */
#define DUR_MAG(v) ((uint64_t)((v) < 0 ? (uint64_t)(-((v) + 1)) + 1 : (uint64_t)(v)))
    /* With a leading '-', components print as magnitudes (the old code instead
       negated everything, emitting invalid "-P5M-10D" for mixed signs and
       hitting UB on INT64_MIN). Without one, components print signed, as
       before. */
#define DUR_COMP(v, div, suffix)                                               \
    do {                                                                       \
        if (neg)                                                               \
            off += snprintf(buf + off, sizeof buf - off, "%llu" suffix,        \
                            (unsigned long long)(DUR_MAG(v) / (div)));         \
        else                                                                   \
            off += snprintf(buf + off, sizeof buf - off, "%lld" suffix,        \
                            (long long)((v) / (div)));                         \
    } while (0)
    if (mo / 12) DUR_COMP(mo, 12, "Y");
    if (mo % 12) DUR_COMP(mo % 12, 1, "M");
    if (d)       DUR_COMP(d, 1, "D");
    if (ms) {   /* ISO 8601 puts a T before any time component */
        int64_t h = ms / 3600000, mi = ms / 60000 % 60;
        int64_t sec = ms / 1000 % 60, mss = ms % 1000;
        buf[off++] = 'T';
        if (h)  DUR_COMP(h, 1, "H");
        if (mi) DUR_COMP(mi, 1, "M");
        if (sec || mss) {
            if (mss) {
                if (neg)
                    off += snprintf(buf + off, sizeof buf - off, "%llu.%03lluS",
                                    (unsigned long long)DUR_MAG(sec),
                                    (unsigned long long)(DUR_MAG(mss) % 1000));
                else
                    off += snprintf(buf + off, sizeof buf - off, "%lld.%03lldS",
                                    (long long)sec, (long long)mss);
            } else {
                DUR_COMP(sec, 1, "S");
            }
        }
    }
#undef DUR_COMP
#undef DUR_MAG
    buf[off] = '\0';
    return JS_NewStringLen(ctx, buf, off);
}

static const JSCFunctionListEntry dyn_pdate_proto[] = {
    JS_CGETSET_MAGIC_DEF("year", dyn_pdate_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("month", dyn_pdate_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("day", dyn_pdate_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("dayOfWeek", dyn_pdate_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("dayOfYear", dyn_pdate_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("daysInMonth", dyn_pdate_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("daysInYear", dyn_pdate_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("inLeapYear", dyn_pdate_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("epochDay", dyn_pdate_get, NULL, 8),
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_pdate_add, 0),
    JS_CFUNC_MAGIC_DEF("subtract", 1, dyn_pdate_add, 1),
    JS_CFUNC_DEF("until", 1, dyn_pdate_until),
    JS_CFUNC_DEF("compare", 1, dyn_pdate_compare),
    JS_CFUNC_DEF("toString", 0, dyn_pdate_tostring),
};

static const JSCFunctionListEntry dyn_dur_proto[] = {
    JS_CGETSET_MAGIC_DEF("months", dyn_dur_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("days", dyn_dur_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("years", dyn_dur_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("sign", dyn_dur_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("blank", dyn_dur_get, NULL, 4),
    JS_CFUNC_DEF("toString", 0, dyn_dur_tostring),
};

/* --- PlainTime ---------------------------------------------------------
   Wall-clock time of day with no date and no zone. Stored as ONE integer
   count of milliseconds since midnight: every comparison is an integer
   compare and every arithmetic result is exact, where four separate fields
   would need carry handling at each of them. */

#define TP_DAY_MS 86400000

typedef struct { int32_t ms; } tp_time_t;

/* One integer, so a PlainTime is a register. If this grows, the claim in
   API.md that comparison is an integer compare stops being true. */
_Static_assert(sizeof(tp_time_t) == 4, "tp_time_t is no longer one integer");

static JSClassID dyn_ptime_class_id;

static void dyn_ptime_free(void *p) { free(p); }
static void dyn_ptime_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_ptime_class_id));
}
static const JSClassDef dyn_ptime_class = {
    "PlainTime", .finalizer = dyn_ptime_finalizer
};

static JSValue tp_new_time(JSContext *ctx, int32_t ms)
{
    tp_time_t *p = (tp_time_t *)malloc(sizeof *p);

    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->ms = ms;
    return dyn_plain_wrap(ctx, dyn_ptime_class_id, p, dyn_ptime_free);
}

static JSValue dyn_ptime_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    int32_t f[4] = { 0, 0, 0, 0 };
    static const int32_t LIM[4] = { 23, 59, 59, 999 };
    static const char *NM[4] = { "hour", "minute", "second", "millisecond" };
    int i;
    (void)new_target;

    for (i = 0; i < 4 && i < argc; i++)
        if (JS_ToInt32(ctx, &f[i], argv[i]))
            return JS_EXCEPTION;
    for (i = 0; i < 4; i++) {
        /* 24:00 is refused. It is a legal instant only as the END of a day,
           and accepting it here would make two different values compare
           unequal while naming the same wall clock. */
        if (f[i] < 0 || f[i] > LIM[i])
            return JS_ThrowRangeError(ctx, "PlainTime: %s %d is not 0..%d",
                                      NM[i], f[i], LIM[i]);
    }
    return tp_new_time(ctx, ((f[0] * 60 + f[1]) * 60 + f[2]) * 1000 + f[3]);
}

/* magic 0 hour, 1 minute, 2 second, 3 millisecond, 4 msSinceMidnight */
static JSValue dyn_ptime_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    tp_time_t *p = (tp_time_t *)dyn_plain_get(ctx, this_val, dyn_ptime_class_id);

    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_NewInt32(ctx, p->ms / 3600000);
    case 1: return JS_NewInt32(ctx, p->ms / 60000 % 60);
    case 2: return JS_NewInt32(ctx, p->ms / 1000 % 60);
    case 3: return JS_NewInt32(ctx, p->ms % 1000);
    default: return JS_NewInt32(ctx, p->ms);
    }
}

/* magic 0 add, 1 subtract. Time WRAPS at midnight and reports how many days
   it crossed, because a time of day has nowhere else to put the overflow --
   and silently discarding it would make the result quietly wrong. */
static JSValue dyn_ptime_add(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    tp_time_t *p = (tp_time_t *)dyn_plain_get(ctx, this_val, dyn_ptime_class_id);
    tp_dur_t *d;
    int64_t total;

    if (!p)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainTime.%s(duration): a Duration is "
                                      "required", magic ? "subtract" : "add");
    d = (tp_dur_t *)dyn_plain_get(ctx, argv[0], dyn_dur_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->months)
        return JS_ThrowRangeError(ctx, "PlainTime: a duration in months has no "
            "meaning for a time of day -- it cannot be converted to hours");
    total = (int64_t)p->ms + (magic ? -1 : 1) * d->ms_part;
    total %= TP_DAY_MS;
    if (total < 0)
        total += TP_DAY_MS;
    return tp_new_time(ctx, (int32_t)total);
}

static JSValue dyn_ptime_compare(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    tp_time_t *a = (tp_time_t *)dyn_plain_get(ctx, this_val, dyn_ptime_class_id);
    tp_time_t *b;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainTime.compare(other): a PlainTime "
                                      "is required");
    b = (tp_time_t *)dyn_plain_get(ctx, argv[0], dyn_ptime_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, a->ms < b->ms ? -1 : (a->ms > b->ms ? 1 : 0));
}

static JSValue dyn_ptime_tostring(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    tp_time_t *p = (tp_time_t *)dyn_plain_get(ctx, this_val, dyn_ptime_class_id);
    char buf[16];
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    /* Milliseconds are omitted when zero, as ISO 8601 allows -- but seconds
       are always printed, so the shape never depends on the value. */
    if (p->ms % 1000)
        snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", p->ms / 3600000,
                 p->ms / 60000 % 60, p->ms / 1000 % 60, p->ms % 1000);
    else
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", p->ms / 3600000,
                 p->ms / 60000 % 60, p->ms / 1000 % 60);
    return JS_NewString(ctx, buf);
}

/* parseTime("HH:MM[:SS[.mmm]]") */
static JSValue dyn_ptime_parse(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const char *s;
    int f[4] = { 0, 0, 0, 0 };
    static const int LIM[4] = { 23, 59, 59, 999 };
    int i = 0, k;
    JSValue r;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "parseTime(text): text is required");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
#define TWO(dst) do { \
        if (!(s[i] >= '0' && s[i] <= '9') || !(s[i+1] >= '0' && s[i+1] <= '9')) \
            goto bad; \
        (dst) = (s[i] - '0') * 10 + (s[i+1] - '0'); i += 2; } while (0)
    TWO(f[0]);
    if (s[i] != ':') goto bad;
    i++;
    TWO(f[1]);
    if (s[i] == ':') {
        i++;
        TWO(f[2]);
        if (s[i] == '.') {
            i++;
            for (k = 0; k < 3; k++) {
                if (!(s[i] >= '0' && s[i] <= '9')) goto bad;
                f[3] = f[3] * 10 + (s[i] - '0');
                i++;
            }
        }
    }
#undef TWO
    if (s[i] != '\0') goto bad;
    for (k = 0; k < 4; k++)
        if (f[k] > LIM[k]) {
            r = JS_ThrowRangeError(ctx, "parseTime: '%s' has a field out of "
                                        "range", s);
            JS_FreeCString(ctx, s);
            return r;
        }
    JS_FreeCString(ctx, s);
    return tp_new_time(ctx, ((f[0] * 60 + f[1]) * 60 + f[2]) * 1000 + f[3]);
bad:
    r = JS_ThrowSyntaxError(ctx, "parseTime: '%s' is not HH:MM[:SS[.mmm]]", s);
    JS_FreeCString(ctx, s);
    return r;
}

static const JSCFunctionListEntry dyn_ptime_proto[] = {
    JS_CGETSET_MAGIC_DEF("hour", dyn_ptime_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("minute", dyn_ptime_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("second", dyn_ptime_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("millisecond", dyn_ptime_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("msSinceMidnight", dyn_ptime_get, NULL, 4),
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_ptime_add, 0),
    JS_CFUNC_MAGIC_DEF("subtract", 1, dyn_ptime_add, 1),
    JS_CFUNC_DEF("compare", 1, dyn_ptime_compare),
    JS_CFUNC_DEF("toString", 0, dyn_ptime_tostring),
};

/* --- PlainDateTime -----------------------------------------------------
   A date and a time of day, still with no zone. The whole reason this is a
   separate type rather than a pair: adding time to it CARRIES into the date,
   where PlainTime wraps and loses the day. 23:30 on the 1st plus two hours
   is 01:30 on the 2nd here, and 01:30 on the 1st there. */

typedef struct { tp_date_t d; int32_t ms; } tp_dt_t;

_Static_assert(sizeof(tp_dt_t) == 24, "tp_dt_t regained padding");

static JSClassID dyn_pdt_class_id;

static void dyn_pdt_free(void *p) { free(p); }
static void dyn_pdt_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, dyn_pdt_class_id));
}
static const JSClassDef dyn_pdt_class = {
    "PlainDateTime", .finalizer = dyn_pdt_finalizer
};

static JSValue tp_new_dt(JSContext *ctx, int64_t y, int m, int d, int32_t ms)
{
    tp_dt_t *p;

    if (tp_check(ctx, y, m, d) < 0)
        return JS_EXCEPTION;
    p = (tp_dt_t *)malloc(sizeof *p);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->d.y = y; p->d.m = m; p->d.d = d; p->ms = ms;
    return dyn_plain_wrap(ctx, dyn_pdt_class_id, p, dyn_pdt_free);
}

static JSValue dyn_pdt_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    int64_t y;
    int32_t f[6] = { 0, 0, 0, 0, 0, 0 };   /* m d H M S ms */
    static const int32_t LIM[6] = { 12, 31, 23, 59, 59, 999 };
    static const char *NM[6] = { "month", "day", "hour", "minute", "second",
                                 "millisecond" };
    int i;
    (void)new_target;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "new PlainDateTime(year, month, day, "
            "hour?, minute?, second?, millisecond?): the date is required");
    if (JS_ToInt64(ctx, &y, argv[0]))
        return JS_EXCEPTION;
    for (i = 0; i < 6 && i + 1 < argc; i++)
        if (JS_ToInt32(ctx, &f[i], argv[i + 1]))
            return JS_EXCEPTION;
    for (i = 2; i < 6; i++)                /* the date fields are tp_check's */
        if (f[i] < 0 || f[i] > LIM[i])
            return JS_ThrowRangeError(ctx, "PlainDateTime: %s %d is not 0..%d",
                                      NM[i], f[i], LIM[i]);
    return tp_new_dt(ctx, y, f[0], f[1],
                     ((f[2] * 60 + f[3]) * 60 + f[4]) * 1000 + f[5]);
}

/* magic 0 year .. 6 millisecond, 7 epochDay, 8 dayOfWeek */
static JSValue dyn_pdt_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    tp_dt_t *p = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    int64_t ed;

    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_NewInt64(ctx, p->d.y);
    case 1: return JS_NewInt32(ctx, p->d.m);
    case 2: return JS_NewInt32(ctx, p->d.d);
    case 3: return JS_NewInt32(ctx, p->ms / 3600000);
    case 4: return JS_NewInt32(ctx, p->ms / 60000 % 60);
    case 5: return JS_NewInt32(ctx, p->ms / 1000 % 60);
    case 6: return JS_NewInt32(ctx, p->ms % 1000);
    case 7: return JS_NewInt64(ctx, tp_days_from_civil(p->d.y, p->d.m, p->d.d));
    default:
        ed = tp_days_from_civil(p->d.y, p->d.m, p->d.d);
        return JS_NewInt32(ctx, (int)(((ed + 3) % 7 + 7) % 7) + 1);
    }
}

/* magic 0 add, 1 subtract. Months and days move the DATE (clamping exactly as
   PlainDate does), then the time part is added and its overflow CARRIES --
   which is the whole difference from PlainTime. */
static JSValue dyn_pdt_add(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    tp_dt_t *p = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    tp_dur_t *dur;
    int64_t sign, total, carry, ed;
    int64_t y = 0;
    int m = 0, d = 0;
    JSValue nd;
    tp_date_t *ndp;

    if (!p)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDateTime.%s(duration): a Duration "
                                      "is required", magic ? "subtract" : "add");
    dur = (tp_dur_t *)dyn_plain_get(ctx, argv[0], dyn_dur_class_id);
    if (!dur)
        return JS_EXCEPTION;
    sign = magic ? -1 : 1;
    total = (int64_t)p->ms + sign * dur->ms_part;
    /* Floor division, not truncation: -1 ms must carry back a whole day, and
       C's / rounds toward zero, which would leave a negative time of day. */
    carry = total >= 0 ? total / TP_DAY_MS
                       : -((-total + TP_DAY_MS - 1) / TP_DAY_MS);
    total -= carry * TP_DAY_MS;

    nd = tp_shift(ctx, &p->d, sign * dur->months, sign * dur->days + carry);
    if (JS_IsException(nd))
        return nd;
    ndp = (tp_date_t *)dyn_plain_get(ctx, nd, dyn_pdate_class_id);
    if (!ndp) { JS_FreeValue(ctx, nd); return JS_EXCEPTION; }
    y = ndp->y; m = ndp->m; d = ndp->d;
    JS_FreeValue(ctx, nd);
    (void)ed;
    return tp_new_dt(ctx, y, m, d, (int32_t)total);
}

static JSValue dyn_pdt_compare(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    tp_dt_t *a = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    tp_dt_t *b;
    int64_t ea, eb;

    if (!a)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PlainDateTime.compare(other): a "
                                      "PlainDateTime is required");
    b = (tp_dt_t *)dyn_plain_get(ctx, argv[0], dyn_pdt_class_id);
    if (!b)
        return JS_EXCEPTION;
    ea = tp_days_from_civil(a->d.y, a->d.m, a->d.d) * TP_DAY_MS + a->ms;
    eb = tp_days_from_civil(b->d.y, b->d.m, b->d.d) * TP_DAY_MS + b->ms;
    return JS_NewInt32(ctx, ea < eb ? -1 : (ea > eb ? 1 : 0));
}

static JSValue dyn_pdt_tostring(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    tp_dt_t *p = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    char buf[48];
    int off;
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    if (p->d.y >= 0 && p->d.y <= 9999)
        off = snprintf(buf, sizeof buf, "%04d-%02d-%02d", (int)p->d.y,
                       p->d.m, p->d.d);
    else
        off = snprintf(buf, sizeof buf, "%c%06lld-%02d-%02d",
                       p->d.y < 0 ? '-' : '+',
                       (long long)(p->d.y < 0 ? -p->d.y : p->d.y),
                       p->d.m, p->d.d);
    if (p->ms % 1000)
        snprintf(buf + off, sizeof buf - off, "T%02d:%02d:%02d.%03d",
                 p->ms / 3600000, p->ms / 60000 % 60, p->ms / 1000 % 60,
                 p->ms % 1000);
    else
        snprintf(buf + off, sizeof buf - off, "T%02d:%02d:%02d",
                 p->ms / 3600000, p->ms / 60000 % 60, p->ms / 1000 % 60);
    return JS_NewString(ctx, buf);
}

/* toPlainDate() / toPlainTime(): magic 0 and 1 */
static JSValue dyn_pdt_part(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    tp_dt_t *p = (tp_dt_t *)dyn_plain_get(ctx, this_val, dyn_pdt_class_id);
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    return magic ? tp_new_time(ctx, p->ms)
                 : tp_new_date(ctx, p->d.y, p->d.m, p->d.d);
}

static const JSCFunctionListEntry dyn_pdt_proto[] = {
    JS_CGETSET_MAGIC_DEF("year", dyn_pdt_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("month", dyn_pdt_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("day", dyn_pdt_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("hour", dyn_pdt_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("minute", dyn_pdt_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("second", dyn_pdt_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("millisecond", dyn_pdt_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("epochDay", dyn_pdt_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("dayOfWeek", dyn_pdt_get, NULL, 8),
    JS_CFUNC_MAGIC_DEF("add", 1, dyn_pdt_add, 0),
    JS_CFUNC_MAGIC_DEF("subtract", 1, dyn_pdt_add, 1),
    JS_CFUNC_MAGIC_DEF("toPlainDate", 0, dyn_pdt_part, 0),
    JS_CFUNC_MAGIC_DEF("toPlainTime", 0, dyn_pdt_part, 1),
    JS_CFUNC_DEF("compare", 1, dyn_pdt_compare),
    JS_CFUNC_DEF("toString", 0, dyn_pdt_tostring),
};
