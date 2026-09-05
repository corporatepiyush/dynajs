/* Date */

static int64_t math_mod(int64_t a, int64_t b) {
    /* return positive modulo */
    int64_t m = a % b;
    return m + (m < 0) * b;
}

static int64_t floor_div(int64_t a, int64_t b) {
    /* integer division rounding toward -Infinity */
    int64_t m = a % b;
    return (a - (m + (m < 0) * b)) / b;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);

static __exception int JS_ThisTimeValue(JSContext *ctx, double *valp, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE && JS_IsNumber(p->u.object_data))
            return JS_ToFloat64(ctx, valp, p->u.object_data);
    }
    JS_ThrowTypeError(ctx, "not a Date object");
    return -1;
}

static JSValue JS_SetThisTimeValue(JSContext *ctx, JSValueConst this_val, double v)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE) {
            JS_FreeValue(ctx, p->u.object_data);
            p->u.object_data = JS_NewFloat64(ctx, v);
            return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a Date object");
}

static int64_t days_from_year(int64_t y) {
    return 365 * (y - 1970) + floor_div(y - 1969, 4) -
        floor_div(y - 1901, 100) + floor_div(y - 1601, 400);
}

static int64_t days_in_year(int64_t y) {
    return 365 + !(y % 4) - !(y % 100) + !(y % 400);
}

/* return the year, update days */
static int64_t year_from_days(int64_t *days) {
    int64_t y, d1, nd, d = *days;
    y = floor_div(d * 10000, 3652425) + 1970;
    /* the initial approximation is very good, so only a few
       iterations are necessary */
    for(;;) {
        d1 = d - days_from_year(y);
        if (d1 < 0) {
            y--;
            d1 += days_in_year(y);
        } else {
            nd = days_in_year(y);
            if (d1 < nd)
                break;
            d1 -= nd;
            y++;
        }
    }
    *days = d1;
    return y;
}

static int const month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static char const month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static char const day_names[] = "SunMonTueWedThuFriSat";

/* Fixed-width decimal emitters for get_date_string(). snprintf costs hundreds
   of ns per call and every date string needs 3-5 of them; these are a few
   stores each. Callers guarantee the value ranges and the buffer headroom. */
static char const date_digit_pairs[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536"
    "37383940414243444546474849505152535455565758596061626364656667686970717273"
    "7475767778798081828384858687888990919293949596979899";

/* 0 <= v <= 99 */
static inline int date_put_u2(char *p, int v)
{
    p[0] = date_digit_pairs[v * 2];
    p[1] = date_digit_pairs[v * 2 + 1];
    return 2;
}

/* 0 <= v <= 999 */
static inline int date_put_u3(char *p, int v)
{
    p[0] = '0' + v / 100;
    date_put_u2(p + 1, v % 100);
    return 3;
}

/* 0 <= v <= 9999 */
static inline int date_put_u4(char *p, int v)
{
    date_put_u2(p, v / 100);
    date_put_u2(p + 2, v % 100);
    return 4;
}

/* equivalent to snprintf(p, avail, "%0*d", 4 + (y < 0), y) */
static int date_put_year(char *p, size_t avail, int y)
{
    if (likely((unsigned)y <= 9999))
        return date_put_u4(p, y);
    return snprintf(p, avail, "%0*d", 4 + (y < 0), y);
}

static __exception int get_date_fields(JSContext *ctx, JSValueConst obj,
                                       double fields[minimum_length(9)], int is_local, int force)
{
    double dval;
    int64_t d, days, wd, y, i, md, h, m, s, ms, tz = 0;

    if (JS_ThisTimeValue(ctx, &dval, obj))
        return -1;

    if (isnan(dval)) {
        if (!force)
            return FALSE; /* NaN */
        d = 0;        /* initialize all fields to 0 */
    } else {
        d = dval;     /* assuming -8.64e15 <= dval <= -8.64e15 */
        if (is_local) {
            tz = -getTimezoneOffset(d);
            d += tz * 60000;
        }
    }

    /* result is >= 0, we can use % */
    h = math_mod(d, 86400000);
    days = (d - h) / 86400000;
    ms = h % 1000;
    h = (h - ms) / 1000;
    s = h % 60;
    h = (h - s) / 60;
    m = h % 60;
    h = (h - m) / 60;
    wd = math_mod(days + 4, 7); /* week day */
    y = year_from_days(&days);

    for(i = 0; i < 11; i++) {
        md = month_days[i];
        if (i == 1)
            md += days_in_year(y) - 365;
        if (days < md)
            break;
        days -= md;
    }
    fields[0] = y;
    fields[1] = i;
    fields[2] = days + 1;
    fields[3] = h;
    fields[4] = m;
    fields[5] = s;
    fields[6] = ms;
    fields[7] = wd;
    fields[8] = tz;
    return TRUE;
}

static double time_clip(double t) {
    if (t >= -8.64e15 && t <= 8.64e15)
        return trunc(t) + 0.0;  /* convert -0 to +0 */
    else
        return NAN;
}

/* The spec mandates the use of 'double' and it specifies the order
   of the operations */
static double set_date_fields(double fields[minimum_length(7)], int is_local) {
    double y, m, dt, ym, mn, day, h, s, milli, time, tv;
    int yi, mi, i;
    int64_t days;
    volatile double temp;  /* enforce evaluation order */

    /* emulate 21.4.1.15 MakeDay ( year, month, date ) */
    y = fields[0];
    m = fields[1];
    dt = fields[2];
    ym = y + floor(m / 12);
    mn = fmod(m, 12);
    if (mn < 0)
        mn += 12;
    if (!(ym >= -271821 && ym <= 275760))   /* NaN fails both bounds */
        return NAN;

    yi = ym;
    mi = mn;
    days = days_from_year(yi);
    for(i = 0; i < mi; i++) {
        days += month_days[i];
        if (i == 1)
            days += days_in_year(yi) - 365;
    }
    day = days + dt - 1;

    /* emulate 21.4.1.14 MakeTime ( hour, min, sec, ms ) */
    h = fields[3];
    m = fields[4];
    s = fields[5];
    milli = fields[6];
    /* Use a volatile intermediary variable to ensure order of evaluation
     * as specified in ECMA. This fixes a test262 error on
     * test262/test/built-ins/Date/UTC/fp-evaluation-order.js.
     * Without the volatile qualifier, the compile can generate code
     * that performs the computation in a different order or with instructions
     * that produce a different result such as FMA (float multiply and add).
     */
    time = h * 3600000;
    time += (temp = m * 60000);
    time += (temp = s * 1000);
    time += milli;

    /* emulate 21.4.1.16 MakeDate ( day, time ) */
    tv = (temp = day * 86400000) + time;   /* prevent generation of FMA */
    if (!isfinite(tv))
        return NAN;

    /* adjust for local time and clip */
    if (is_local) {
        int64_t ti = tv < INT64_MIN ? INT64_MIN : tv >= 0x1p63 ? INT64_MAX : (int64_t)tv;
        tv += getTimezoneOffset(ti) * 60000;
    }
    return time_clip(tv);
}

static double set_date_fields_checked(double fields[minimum_length(7)], int is_local)
{
    int i;
    double a;
    for(i = 0; i < 7; i++) {
        a = fields[i];
        if (!isfinite(a))
            return NAN;
        fields[i] = trunc(a);
        if (i == 0 && fields[0] >= 0 && fields[0] < 100)
            fields[0] += 1900;
    }
    return set_date_fields(fields, is_local);
}

static JSValue get_date_field(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    // get_date_field(obj, n, is_local)
    double fields[9];
    int res, n, is_local;

    is_local = magic & 0x0F;
    n = (magic >> 4) & 0x0F;
    res = get_date_fields(ctx, this_val, fields, is_local, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res)
        return JS_NAN;

    if (magic & 0x100) {    // getYear
        fields[0] -= 1900;
    }
    return JS_NewFloat64(ctx, fields[n]);
}

static JSValue set_date_field(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    // _field(obj, first_field, end_field, args, is_local)
    double fields[9];
    int res, first_field, end_field, is_local, i, n, res1;
    double d, a;

    d = NAN;
    first_field = (magic >> 8) & 0x0F;
    end_field = (magic >> 4) & 0x0F;
    is_local = magic & 0x0F;

    res = get_date_fields(ctx, this_val, fields, is_local, first_field == 0);
    if (res < 0)
        return JS_EXCEPTION;
    res1 = res;
    
    // Argument coercion is observable and must be done unconditionally.
    n = min_int(argc, end_field - first_field);
    for(i = 0; i < n; i++) {
        if (JS_ToFloat64(ctx, &a, argv[i]))
            return JS_EXCEPTION;
        if (!isfinite(a))
            res = FALSE;
        fields[first_field + i] = trunc(a);
    }

    if (!res1)
        return JS_NAN; /* thisTimeValue is NaN */

    if (res && argc > 0)
        d = set_date_fields(fields, is_local);

    return JS_SetThisTimeValue(ctx, this_val, d);
}

/* fmt:
   0: toUTCString: "Tue, 02 Jan 2018 23:04:46 GMT"
   1: toString: "Wed Jan 03 2018 00:05:22 GMT+0100 (CET)"
   2: toISOString: "2018-01-02T23:02:56.927Z"
   3: toLocaleString: "1/2/2018, 11:40:40 PM"
   part: 1=date, 2=time 3=all
   XXX: should use a variant of strftime().
 */
static JSValue get_date_string(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    // _string(obj, fmt, part)
    char buf[64];
    double fields[9];
    int res, fmt, part, pos;
    int y, mon, d, h, m, s, ms, wd, tz;

    fmt = (magic >> 4) & 0x0F;
    part = magic & 0x0F;

    res = get_date_fields(ctx, this_val, fields, fmt & 1, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res) {
        if (fmt == 2)
            return JS_ThrowRangeError(ctx, "Date value is NaN");
        else
            return js_new_string8(ctx, "Invalid Date");
    }

    y = fields[0];
    mon = fields[1];
    d = fields[2];
    h = fields[3];
    m = fields[4];
    s = fields[5];
    ms = fields[6];
    wd = fields[7];
    tz = fields[8];

    pos = 0;

    if (part & 1) { /* date part */
        switch(fmt) {
        case 0:
            memcpy(buf + pos, day_names + wd * 3, 3); pos += 3;
            buf[pos++] = ',';
            buf[pos++] = ' ';
            pos += date_put_u2(buf + pos, d);
            buf[pos++] = ' ';
            memcpy(buf + pos, month_names + mon * 3, 3); pos += 3;
            buf[pos++] = ' ';
            pos += date_put_year(buf + pos, sizeof(buf) - pos, y);
            buf[pos++] = ' ';
            break;
        case 1:
            memcpy(buf + pos, day_names + wd * 3, 3); pos += 3;
            buf[pos++] = ' ';
            memcpy(buf + pos, month_names + mon * 3, 3); pos += 3;
            buf[pos++] = ' ';
            pos += date_put_u2(buf + pos, d);
            buf[pos++] = ' ';
            pos += date_put_year(buf + pos, sizeof(buf) - pos, y);
            if (part == 3) {
                buf[pos++] = ' ';
            }
            break;
        case 2:
            if (likely((unsigned)y <= 9999)) {
                pos += date_put_u4(buf + pos, y);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%+07d", y);
            }
            buf[pos++] = '-';
            pos += date_put_u2(buf + pos, mon + 1);
            buf[pos++] = '-';
            pos += date_put_u2(buf + pos, d);
            buf[pos++] = 'T';
            break;
        case 3:
            pos += date_put_u2(buf + pos, mon + 1);
            buf[pos++] = '/';
            pos += date_put_u2(buf + pos, d);
            buf[pos++] = '/';
            pos += date_put_year(buf + pos, sizeof(buf) - pos, y);
            if (part == 3) {
                buf[pos++] = ',';
                buf[pos++] = ' ';
            }
            break;
        }
    }
    if (part & 2) { /* time part */
        switch(fmt) {
        case 0:
        case 1:
            pos += date_put_u2(buf + pos, h);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, m);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, s);
            memcpy(buf + pos, " GMT", 4); pos += 4;
            if (fmt == 1) {
                if (tz < 0) {
                    buf[pos++] = '-';
                    tz = -tz;
                } else {
                    buf[pos++] = '+';
                }
                /* tz is >= 0, can use % */
                pos += date_put_u2(buf + pos, tz / 60);
                pos += date_put_u2(buf + pos, tz % 60);
                /* XXX: tack the time zone code? */
            }
            break;
        case 2:
            pos += date_put_u2(buf + pos, h);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, m);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, s);
            buf[pos++] = '.';
            pos += date_put_u3(buf + pos, ms);
            buf[pos++] = 'Z';
            break;
        case 3:
            pos += date_put_u2(buf + pos, (h + 11) % 12 + 1);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, m);
            buf[pos++] = ':';
            pos += date_put_u2(buf + pos, s);
            buf[pos++] = ' ';
            buf[pos++] = (h < 12) ? 'A' : 'P';
            buf[pos++] = 'M';
            break;
        }
    }
    return JS_NewStringLen(ctx, buf, pos);
}

/* OS dependent: return the UTC time in ms since 1970. */
static int64_t date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

static JSValue js_date_constructor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    // Date(y, mon, d, h, m, s, ms)
    JSValue rv;
    int i, n;
    double val;

    if (JS_IsUndefined(new_target)) {
        /* invoked as function */
        argc = 0;
    }
    n = argc;
    if (n == 0) {
        val = date_now();
    } else if (n == 1) {
        JSValue v, dv;
        if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
            JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
            if (p->class_id == JS_CLASS_DATE && JS_IsNumber(p->u.object_data)) {
                if (JS_ToFloat64(ctx, &val, p->u.object_data))
                    return JS_EXCEPTION;
                val = time_clip(val);
                goto has_val;
            }
        }
        v = JS_ToPrimitive(ctx, argv[0], HINT_NONE);
        if (JS_IsString(v)) {
            dv = js_Date_parse(ctx, JS_UNDEFINED, 1, (JSValueConst *)&v);
            JS_FreeValue(ctx, v);
            if (JS_IsException(dv))
                return JS_EXCEPTION;
            if (JS_ToFloat64Free(ctx, &val, dv))
                return JS_EXCEPTION;
        } else {
            if (JS_ToFloat64Free(ctx, &val, v))
                return JS_EXCEPTION;
        }
        val = time_clip(val);
    } else {
        double fields[] = { 0, 0, 1, 0, 0, 0, 0 };
        if (n > 7)
            n = 7;
        for(i = 0; i < n; i++) {
            if (JS_ToFloat64(ctx, &fields[i], argv[i]))
                return JS_EXCEPTION;
        }
        val = set_date_fields_checked(fields, 1);
    }
has_val:
#if 0
    JSValueConst args[3];
    args[0] = new_target;
    args[1] = ctx->class_proto[JS_CLASS_DATE];
    args[2] = JS_NewFloat64(ctx, val);
    rv = js___date_create(ctx, JS_UNDEFINED, 3, args);
#else
    rv = js_create_from_ctor(ctx, new_target, JS_CLASS_DATE);
    if (!JS_IsException(rv))
        JS_SetObjectData(ctx, rv, JS_NewFloat64(ctx, val));
#endif
    if (!JS_IsException(rv) && JS_IsUndefined(new_target)) {
        /* invoked as a function, return (new Date()).toString(); */
        JSValue s;
        s = get_date_string(ctx, rv, 0, NULL, 0x13);
        JS_FreeValue(ctx, rv);
        rv = s;
    }
    return rv;
}

static JSValue js_Date_UTC(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    // UTC(y, mon, d, h, m, s, ms)
    double fields[] = { 0, 0, 1, 0, 0, 0, 0 };
    int i, n;

    n = argc;
    if (n == 0)
        return JS_NAN;
    if (n > 7)
        n = 7;
    for(i = 0; i < n; i++) {
        if (JS_ToFloat64(ctx, &fields[i], argv[i]))
            return JS_EXCEPTION;
    }
    return JS_NewFloat64(ctx, set_date_fields_checked(fields, 0));
}

/* Date string parsing */

static BOOL string_skip_char(const uint8_t *sp, int *pp, int c) {
    if (sp[*pp] == c) {
        *pp += 1;
        return TRUE;
    } else {
        return FALSE;
    }
}

/* skip spaces, update offset, return next char */
static int string_skip_spaces(const uint8_t *sp, int *pp) {
    int c;
    while ((c = sp[*pp]) == ' ')
        *pp += 1;
    return c;
}

/* skip dashes dots and commas */
static int string_skip_separators(const uint8_t *sp, int *pp) {
    int c;
    while ((c = sp[*pp]) == '-' || c == '/' || c == '.' || c == ',')
        *pp += 1;
    return c;
}

/* skip a word, stop on spaces, digits and separators, update offset */
static int string_skip_until(const uint8_t *sp, int *pp, const char *stoplist) {
    int c;
    while (!strchr(stoplist, c = sp[*pp]))
        *pp += 1;
    return c;
}

/* parse a numeric field (max_digits = 0 -> no maximum) */
static BOOL string_get_digits(const uint8_t *sp, int *pp, int *pval,
                              int min_digits, int max_digits)
{
    int v = 0;
    int c, p = *pp, p_start;

    p_start = p;
    while ((c = sp[p]) >= '0' && c <= '9') {
        /* arbitrary limit to 9 digits */
        if (v >= 100000000)
            return FALSE;
        v = v * 10 + c - '0';
        p++;
        if (p - p_start == max_digits)
            break;
    }
    if (p - p_start < min_digits)
        return FALSE;
    *pval = v;
    *pp = p;
    return TRUE;
}

static BOOL string_get_milliseconds(const uint8_t *sp, int *pp, int *pval) {
    /* parse optional fractional part as milliseconds and truncate. */
    /* spec does not indicate which rounding should be used */
    int mul = 100, ms = 0, c, p_start, p = *pp;

    c = sp[p];
    if (c == '.' || c == ',') {
        p++;
        p_start = p;
        while ((c = sp[p]) >= '0' && c <= '9') {
            ms += (c - '0') * mul;
            mul /= 10;
            p++;
            if (p - p_start == 9)
                break;
        }
        if (p > p_start) {
            /* only consume the separator if digits are present */
            *pval = ms;
            *pp = p;
        }
    }
    return TRUE;
}

static uint8_t upper_ascii(uint8_t c) {
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static BOOL string_get_tzoffset(const uint8_t *sp, int *pp, int *tzp, BOOL strict) {
    int tz = 0, sgn, hh, mm, p = *pp;

    sgn = sp[p++];
    if (sgn == '+' || sgn == '-') {
        int n = p;
        if (!string_get_digits(sp, &p, &hh, 1, 0))
            return FALSE;
        n = p - n;
        if (strict && n != 2 && n != 4)
            return FALSE;
        while (n > 4) {
            n -= 2;
            hh /= 100;
        }
        if (n > 2) {
            mm = hh % 100;
            hh = hh / 100;
        } else {
            mm = 0;
            if (string_skip_char(sp, &p, ':')) {
                /* optional separator */
                if (!string_get_digits(sp, &p, &mm, 2, 2))
                    return FALSE;
            } else {
                if (strict)
                    return FALSE; /* [+-]HH is not accepted in strict mode */
            }
        }
        if (hh > 23 || mm > 59)
            return FALSE;
        tz = hh * 60 + mm;
        if (sgn != '+')
            tz = -tz;
    } else
    if (sgn != 'Z') {
        return FALSE;
    }
    *pp = p;
    *tzp = tz;
    return TRUE;
}

static BOOL string_match(const uint8_t *sp, int *pp, const char *s) {
    int p = *pp;
    while (*s != '\0') {
        if (upper_ascii(sp[p]) != upper_ascii(*s++))
            return FALSE;
        p++;
    }
    *pp = p;
    return TRUE;
}

static int find_abbrev(const uint8_t *sp, int p, const char *list, int count) {
    int n, i;

    for (n = 0; n < count; n++) {
        for (i = 0;; i++) {
            if (upper_ascii(sp[p + i]) != upper_ascii(list[n * 3 + i]))
                break;
            if (i == 2)
                return n;
        }
    }
    return -1;
}

static BOOL string_get_month(const uint8_t *sp, int *pp, int *pval) {
    int n;

    n = find_abbrev(sp, *pp, month_names, 12);
    if (n < 0)
        return FALSE;

    *pval = n + 1;
    *pp += 3;
    return TRUE;
}

/* parse toISOString format */
static BOOL js_date_parse_isostring(const uint8_t *sp, int fields[9], BOOL *is_local) {
    int sgn, i, p = 0;

    /* initialize fields to the beginning of the Epoch */
    for (i = 0; i < 9; i++) {
        fields[i] = (i == 2);
    }
    *is_local = FALSE;

    /* year is either yyyy digits or [+-]yyyyyy */
    sgn = sp[p];
    if (sgn == '-' || sgn == '+') {
        p++;
        if (!string_get_digits(sp, &p, &fields[0], 6, 6))
            return FALSE;
        if (sgn == '-') {
            if (fields[0] == 0)
                return FALSE; // reject -000000
            fields[0] = -fields[0];
        }
    } else {
        if (!string_get_digits(sp, &p, &fields[0], 4, 4))
            return FALSE;
    }
    if (string_skip_char(sp, &p, '-')) {
        if (!string_get_digits(sp, &p, &fields[1], 2, 2))  /* month */
            return FALSE;
        if (fields[1] < 1)
            return FALSE;
        fields[1] -= 1;
        if (string_skip_char(sp, &p, '-')) {
            if (!string_get_digits(sp, &p, &fields[2], 2, 2))  /* day */
                return FALSE;
            if (fields[2] < 1)
                return FALSE;
        }
    }
    if (string_skip_char(sp, &p, 'T')) {
        *is_local = TRUE;
        if (!string_get_digits(sp, &p, &fields[3], 2, 2)  /* hour */
        ||  !string_skip_char(sp, &p, ':')
        ||  !string_get_digits(sp, &p, &fields[4], 2, 2)) {  /* minute */
            fields[3] = 100;  // reject unconditionally
            return TRUE;
        }
        if (string_skip_char(sp, &p, ':')) {
            if (!string_get_digits(sp, &p, &fields[5], 2, 2))  /* second */
                return FALSE;
            string_get_milliseconds(sp, &p, &fields[6]);
        }
    }
    /* parse the time zone offset if present: [+-]HH:mm or [+-]HHmm */
    if (sp[p]) {
        *is_local = FALSE;
        if (!string_get_tzoffset(sp, &p, &fields[8], TRUE))
            return FALSE;
    }
    /* error if extraneous characters */
    return sp[p] == '\0';
}

static struct {
    char name[6];
    int16_t offset;
} const js_tzabbr[] = {
    { "GMT",   0 },         // Greenwich Mean Time
    { "UTC",   0 },         // Coordinated Universal Time
    { "UT",    0 },         // Universal Time
    { "Z",     0 },         // Zulu Time
    { "EDT",  -4 * 60 },    // Eastern Daylight Time
    { "EST",  -5 * 60 },    // Eastern Standard Time
    { "CDT",  -5 * 60 },    // Central Daylight Time
    { "CST",  -6 * 60 },    // Central Standard Time
    { "MDT",  -6 * 60 },    // Mountain Daylight Time
    { "MST",  -7 * 60 },    // Mountain Standard Time
    { "PDT",  -7 * 60 },    // Pacific Daylight Time
    { "PST",  -8 * 60 },    // Pacific Standard Time
    { "WET",  +0 * 60 },    // Western European Time
    { "WEST", +1 * 60 },    // Western European Summer Time
    { "CET",  +1 * 60 },    // Central European Time
    { "CEST", +2 * 60 },    // Central European Summer Time
    { "EET",  +2 * 60 },    // Eastern European Time
    { "EEST", +3 * 60 },    // Eastern European Summer Time
};

static BOOL string_get_tzabbr(const uint8_t *sp, int *pp, int *offset) {
    for (size_t i = 0; i < countof(js_tzabbr); i++) {
        if (string_match(sp, pp, js_tzabbr[i].name)) {
            *offset = js_tzabbr[i].offset;
            return TRUE;
        }
    }
    return FALSE;
}

/* parse toString, toUTCString and other formats */
static BOOL js_date_parse_otherstring(const uint8_t *sp,
                                      int fields[minimum_length(9)],
                                      BOOL *is_local) {
    int c, i, val, p = 0, p_start;
    int num[3];
    BOOL has_year = FALSE;
    BOOL has_mon = FALSE;
    BOOL has_time = FALSE;
    int num_index = 0;

    /* initialize fields to the beginning of 2001-01-01 */
    fields[0] = 2001;
    fields[1] = 1;
    fields[2] = 1;
    for (i = 3; i < 9; i++) {
        fields[i] = 0;
    }
    *is_local = TRUE;

    while (string_skip_spaces(sp, &p)) {
        p_start = p;
        if ((c = sp[p]) == '+' || c == '-') {
            if (has_time && string_get_tzoffset(sp, &p, &fields[8], FALSE)) {
                *is_local = FALSE;
            } else {
                p++;
                if (string_get_digits(sp, &p, &val, 1, 0)) {
                    if (c == '-') {
                        if (val == 0)
                            return FALSE;
                        val = -val;
                    }
                    fields[0] = val;
                    has_year = TRUE;
                }
            }
        } else
        if (string_get_digits(sp, &p, &val, 1, 0)) {
            if (string_skip_char(sp, &p, ':')) {
                /* time part */
                fields[3] = val;
                if (!string_get_digits(sp, &p, &fields[4], 1, 2))
                    return FALSE;
                if (string_skip_char(sp, &p, ':')) {
                    if (!string_get_digits(sp, &p, &fields[5], 1, 2))
                        return FALSE;
                    string_get_milliseconds(sp, &p, &fields[6]);
                }
                has_time = TRUE;
                if ((sp[p] == '+' || sp[p] == '-') &&
                    string_get_tzoffset(sp, &p, &fields[8], FALSE)) {
                    *is_local = FALSE;
                }
            } else {
                if (p - p_start > 2 && !has_year) {
                    fields[0] = val;
                    has_year = TRUE;
                } else
                if ((val < 1 || val > 31) && !has_year) {
                    fields[0] = val + (val < 100) * 1900 + (val < 50) * 100;
                    has_year = TRUE;
                } else {
                    if (num_index == 3)
                        return FALSE;
                    num[num_index++] = val;
                }
            }
        } else
        if (string_get_month(sp, &p, &fields[1])) {
            has_mon = TRUE;
            string_skip_until(sp, &p, "0123456789 -/(");
        } else
        if (has_time && string_match(sp, &p, "PM")) {
            if (fields[3] < 12)
                fields[3] += 12;
            continue;
        } else
        if (has_time && string_match(sp, &p, "AM")) {
            if (fields[3] == 12)
                fields[3] -= 12;
            continue;
        } else
        if (string_get_tzabbr(sp, &p, &fields[8])) {
            *is_local = FALSE;
            continue;
        } else
        if (c == '(') {  /* skip parenthesized phrase */
            int level = 0;
            while ((c = sp[p]) != '\0') {
                p++;
                level += (c == '(');
                level -= (c == ')');
                if (!level)
                    break;
            }
            if (level > 0)
                return FALSE;
        } else
        if (c == ')') {
            return FALSE;
        } else {
            if (has_year + has_mon + has_time + num_index)
                return FALSE;
            /* skip a word */
            string_skip_until(sp, &p, " -/(");
        }
        string_skip_separators(sp, &p);
    }
    if (num_index + has_year + has_mon > 3)
        return FALSE;

    switch (num_index) {
    case 0:
        if (!has_year)
            return FALSE;
        break;
    case 1:
        if (has_mon)
            fields[2] = num[0];
        else
            fields[1] = num[0];
        break;
    case 2:
        if (has_year) {
            fields[1] = num[0];
            fields[2] = num[1];
        } else
        if (has_mon) {
            fields[0] = num[1] + (num[1] < 100) * 1900 + (num[1] < 50) * 100;
            fields[2] = num[0];
        } else {
            fields[1] = num[0];
            fields[2] = num[1];
        }
        break;
    case 3:
        fields[0] = num[2] + (num[2] < 100) * 1900 + (num[2] < 50) * 100;
        fields[1] = num[0];
        fields[2] = num[1];
        break;
    default:
        return FALSE;
    }
    if (fields[1] < 1 || fields[2] < 1)
        return FALSE;
    fields[1] -= 1;
    return TRUE;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue s, rv;
    int fields[9];
    double fields1[9];
    double d;
    int i, c;
    JSString *sp;
    uint8_t buf[128];
    BOOL is_local;

    rv = JS_NAN;

    s = JS_ToString(ctx, argv[0]);
    if (JS_IsException(s))
        return JS_EXCEPTION;

    sp = JS_VALUE_GET_STRING(s);
    /* Convert the string as a byte array. Leading whitespace is skipped HERE,
       not left for the parsers: the buffer is fixed, and silently truncating a
       long input made Date.parse return a WRONG timestamp rather than NaN --
       120 spaces before a valid date parsed the remains as a different day.
       An input that still does not fit is refused, because a prefix of a date
       is a date. */
    {
        uint32_t start = 0;
        while (start < sp->len) {
            c = string_get(sp, start);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
                c != '\f' && c != '\v' && c != 0xA0 && c != 0xFEFF)
                break;
            start++;
        }
        if (sp->len - start > (uint32_t)countof(buf) - 1) {
            JS_FreeValue(ctx, s);
            return JS_NAN;
        }
        for (i = 0; start + (uint32_t)i < sp->len; i++) {
            c = string_get(sp, start + i);
            if (c > 255)
                c = (c == 0x2212) ? '-' : 'x';
            buf[i] = c;
        }
    }
    buf[i] = '\0';
    if (js_date_parse_isostring(buf, fields, &is_local)
    ||  js_date_parse_otherstring(buf, fields, &is_local)) {
        static int const field_max[6] = { 0, 11, 31, 24, 59, 59 };
        BOOL valid = TRUE;
        /* check field maximum values */
        for (i = 1; i < 6; i++) {
            if (fields[i] > field_max[i])
                valid = FALSE;
        }
        /* special case 24:00:00.000 */
        if (fields[3] == 24 && (fields[4] | fields[5] | fields[6]))
            valid = FALSE;
        if (valid) {
            for(i = 0; i < 7; i++)
                fields1[i] = fields[i];
            d = set_date_fields(fields1, is_local) - fields[8] * 60000;
            rv = JS_NewFloat64(ctx, d);
        }
    }
    JS_FreeValue(ctx, s);
    return rv;
}

static JSValue js_Date_now(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    // now()
    return JS_NewInt64(ctx, date_now());
}

static JSValue js_date_Symbol_toPrimitive(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    // Symbol_toPrimitive(hint)
    JSValueConst obj = this_val;
    JSAtom hint = JS_ATOM_NULL;
    int hint_num;

    if (!JS_IsObject(obj))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    if (JS_IsString(argv[0])) {
        hint = JS_ValueToAtom(ctx, argv[0]);
        if (hint == JS_ATOM_NULL)
            return JS_EXCEPTION;
        JS_FreeAtom(ctx, hint);
    }
    switch (hint) {
    case JS_ATOM_number:
    case JS_ATOM_integer:
        hint_num = HINT_NUMBER;
        break;
    case JS_ATOM_string:
    case JS_ATOM_default:
        hint_num = HINT_STRING;
        break;
    default:
        return JS_ThrowTypeError(ctx, "invalid hint");
    }
    return JS_ToPrimitive(ctx, obj, hint_num | HINT_FORCE_ORDINARY);
}

static JSValue js_date_getTimezoneOffset(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    // getTimezoneOffset()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    if (isnan(v))
        return JS_NAN;
    else
        /* assuming -8.64e15 <= v <= -8.64e15 */
        return JS_NewInt64(ctx, getTimezoneOffset((int64_t)trunc(v)));
}

static JSValue js_date_getTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // getTime()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, v);
}

static JSValue js_date_setTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // setTime(v)
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val) || JS_ToFloat64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_SetThisTimeValue(ctx, this_val, time_clip(v));
}

static JSValue js_date_setYear(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // setYear(y)
    double y;
    JSValueConst args[1];

    if (JS_ThisTimeValue(ctx, &y, this_val) || JS_ToFloat64(ctx, &y, argv[0]))
        return JS_EXCEPTION;
    y = +y;
    if (isfinite(y)) {
        y = trunc(y);
        if (y >= 0 && y < 100)
            y += 1900;
    }
    args[0] = JS_NewFloat64(ctx, y);
    return set_date_field(ctx, this_val, 1, args, 0x011);
}

static JSValue js_date_toJSON(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    // toJSON(key)
    JSValue obj, tv, method, rv;
    double d;

    rv = JS_EXCEPTION;
    tv = JS_UNDEFINED;

    obj = JS_ToObject(ctx, this_val);
    tv = JS_ToPrimitive(ctx, obj, HINT_NUMBER);
    if (JS_IsException(tv))
        goto exception;
    if (JS_IsNumber(tv)) {
        if (JS_ToFloat64(ctx, &d, tv) < 0)
            goto exception;
        if (!isfinite(d)) {
            rv = JS_NULL;
            goto done;
        }
    }
    method = JS_GetProperty(ctx, obj, JS_ATOM_toISOString);
    if (JS_IsException(method))
        goto exception;
    if (!JS_IsFunction(ctx, method)) {
        JS_ThrowTypeError(ctx, "object needs toISOString method");
        JS_FreeValue(ctx, method);
        goto exception;
    }
    rv = JS_CallFree(ctx, method, obj, 0, NULL);
exception:
done:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, tv);
    return rv;
}

static const JSCFunctionListEntry js_date_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_Date_now ),
    JS_CFUNC_DEF("parse", 1, js_Date_parse ),
    JS_CFUNC_DEF("UTC", 7, js_Date_UTC ),
};

/* ============================================================================
 * non-ES Date.prototype extensions. English/ISO, local-time, IMMUTABLE
 * (producers return a NEW Date, never mutating the receiver -- a documented
 * divergence). No untrusted-string parsing here (Date.create's NLP parser +
 * per-locale formatting are deferred); every arg is coerced to a C local first
 * (reentrancy-safe, no native handle held); no DoS loops.
 * ========================================================================== */

static const char * const js_date_month_full[12] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December" };
static const char * const js_date_month_abbr[12] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static const char * const js_date_day_full[7] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char * const js_date_day_abbr[7] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

/* fields (local): [y, mon0-11, day1-31, h, m, s, ms, wd0-6, tz]. Returns
 * 0 ok, 1 NaN-date, -1 exception. */
static int date_this_fields(JSContext *ctx, JSValueConst this_val, double f[9])
{
    int r = get_date_fields(ctx, this_val, f, 1, 0);
    if (r < 0) return -1;
    return r ? 0 : 1;
}

static int date_now_fields(JSContext *ctx, double f[9])
{
    JSValue nd = JS_NewDate(ctx, (double)date_now());
    int r;
    if (JS_IsException(nd)) return -1;
    r = get_date_fields(ctx, nd, f, 1, 0);
    JS_FreeValue(ctx, nd);
    return r > 0 ? 0 : -1;
}

static int date_day_of_year(int y, int mon, int day)   /* 1-based */
{
    int i, n = day;
    for (i = 0; i < mon; i++) {
        n += month_days[i];
        if (i == 1) n += (int)(days_in_year(y) - 365);
    }
    return n;
}

/* days since the epoch for a local Y/M/D (for same-day comparisons). */
static int64_t date_day_number(double y, double mon, double day)
{
    return days_from_year((int64_t)y) + date_day_of_year((int)y, (int)mon, (int)day) - 1;
}

/* ISO-8601 week number (1..53). */
static int date_iso_week(int y, int mon, int day, int wd /*0=Sun*/)
{
    int iso_wd = wd == 0 ? 7 : wd;                 /* 1=Mon..7=Sun */
    int doy = date_day_of_year(y, mon, day);
    int week = (doy - iso_wd + 10) / 7;
    if (week < 1)                                  /* last week of previous year */
        return date_iso_week(y - 1, 11, 31, (int)math_mod(wd - 1, 7));
    if (week > 52) {
        int last_doy = (int)days_in_year(y);
        int wd_dec31 = (int)math_mod(wd + (last_doy - doy), 7);
        int iso_dec31 = wd_dec31 == 0 ? 7 : wd_dec31;
        if (iso_dec31 < 4) return 1;               /* belongs to next year's week 1 */
    }
    return week;
}

enum {   /* js_date_ext_pred; DOW_BASE+wd for day, MON_BASE+idx for month */
    DP_VALID, DP_TODAY, DP_YESTERDAY, DP_TOMORROW, DP_FUTURE, DP_PAST,
    DP_WEEKDAY, DP_WEEKEND, DP_LEAP,
    DP_DOW_BASE = 100, DP_MON_BASE = 200,
};

static JSValue js_date_ext_pred(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    double f[9], now[9], ms, nowms;
    int st;
    BOOL r = FALSE;
    (void)argc; (void)argv;
    st = date_this_fields(ctx, this_val, f);
    if (st < 0) return JS_EXCEPTION;
    if (magic == DP_VALID) return JS_NewBool(ctx, st == 0);
    if (st == 1) return JS_FALSE;                  /* invalid date: all others false */
    if (magic >= DP_MON_BASE) return JS_NewBool(ctx, (int)f[1] == magic - DP_MON_BASE);
    if (magic >= DP_DOW_BASE) return JS_NewBool(ctx, (int)f[7] == magic - DP_DOW_BASE);
    switch (magic) {
    case DP_WEEKDAY: r = (f[7] != 0 && f[7] != 6); break;
    case DP_WEEKEND: r = (f[7] == 0 || f[7] == 6); break;
    case DP_LEAP:    r = days_in_year((int64_t)f[0]) == 366; break;
    case DP_FUTURE:
    case DP_PAST:
        if (JS_ThisTimeValue(ctx, &ms, this_val)) return JS_EXCEPTION;
        nowms = (double)date_now();
        r = (magic == DP_FUTURE) ? (ms > nowms) : (ms < nowms);
        break;
    case DP_TODAY:
    case DP_YESTERDAY:
    case DP_TOMORROW:
        if (date_now_fields(ctx, now) < 0) return JS_EXCEPTION;
        {
            int64_t a = (int64_t)date_day_number(f[0], f[1], f[2]);
            int64_t b = (int64_t)date_day_number(now[0], now[1], now[2]);
            int64_t diff = a - b;
            r = (magic == DP_TODAY) ? (diff == 0)
              : (magic == DP_YESTERDAY) ? (diff == -1) : (diff == 1);
        }
        break;
    }
    return JS_NewBool(ctx, r);
}

enum { DQ_WEEKDAY, DQ_ISOWEEK, DQ_DAYSINMONTH };

static JSValue js_date_ext_query(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    double f[9];
    int st, mdays;
    (void)argc; (void)argv;
    st = date_this_fields(ctx, this_val, f);
    if (st < 0) return JS_EXCEPTION;
    if (st == 1) return JS_NewFloat64(ctx, NAN);
    switch (magic) {
    case DQ_WEEKDAY:  return JS_NewInt32(ctx, (int)f[7]);
    case DQ_ISOWEEK:  return JS_NewInt32(ctx, date_iso_week((int)f[0], (int)f[1], (int)f[2], (int)f[7]));
    default:
        mdays = month_days[(int)f[1]];
        if ((int)f[1] == 1) mdays += (int)(days_in_year((int64_t)f[0]) - 365);
        return JS_NewInt32(ctx, mdays);
    }
}

enum { DC_BEFORE, DC_AFTER, DC_BETWEEN };

static JSValue js_date_ext_cmp(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    double ms, a, b;
    if (JS_ThisTimeValue(ctx, &ms, this_val)) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &a, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    if (magic == DC_BETWEEN) {
        if (JS_ToFloat64(ctx, &b, argc > 1 ? argv[1] : JS_UNDEFINED)) return JS_EXCEPTION;
        if (a > b) { double t = a; a = b; b = t; }
        return JS_NewBool(ctx, ms >= a && ms <= b);
    }
    if (isnan(ms) || isnan(a)) return JS_FALSE;
    return JS_NewBool(ctx, magic == DC_BEFORE ? ms < a : ms > a);
}

/* diff units in ms (months/years handled by calendar math, sentinel 0). */
static const double js_date_unit_ms[8] = {
    1.0, 1000.0, 60000.0, 3600000.0, 86400000.0, 604800000.0, 0.0, 0.0,
};
enum { DU_MS, DU_S, DU_MIN, DU_H, DU_D, DU_W, DU_MON, DU_Y };
enum { DM_SINCE, DM_UNTIL, DM_AGO, DM_FROMNOW };

/* calendar months between two field sets (b - a), with day-of-month adjust. */
static double date_month_diff(const double a[9], const double b[9])
{
    double m = (b[0] - a[0]) * 12 + (b[1] - a[1]);
    if (m > 0 && b[2] < a[2]) m -= 1;
    else if (m < 0 && b[2] > a[2]) m += 1;
    return m;
}

static JSValue js_date_ext_diff(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    int unit = magic / 4, mode = magic % 4;
    double self_ms, other_ms, from_ms, to_ms, d;
    if (JS_ThisTimeValue(ctx, &self_ms, this_val)) return JS_EXCEPTION;
    if (mode == DM_AGO || mode == DM_FROMNOW) {
        other_ms = (double)date_now();
    } else {
        if (JS_ToFloat64(ctx, &other_ms, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    }
    if (isnan(self_ms) || isnan(other_ms)) return JS_NewFloat64(ctx, NAN);
    /* diff = to - from.  Since(o)=this-o, FromNow=this-now (both to=this);
     * Until(o)=o-this, Ago=now-this (both to=other). */
    if (mode == DM_SINCE || mode == DM_FROMNOW) { from_ms = other_ms; to_ms = self_ms; }
    else                                        { from_ms = self_ms;  to_ms = other_ms; }
    if (unit == DU_MON || unit == DU_Y) {
        double fa[9], fb[9], mdiff;
        JSValue da = JS_NewDate(ctx, from_ms), db = JS_NewDate(ctx, to_ms);
        int ok = !JS_IsException(da) && !JS_IsException(db)
                 && get_date_fields(ctx, da, fa, 1, 0) > 0
                 && get_date_fields(ctx, db, fb, 1, 0) > 0;
        JS_FreeValue(ctx, da); JS_FreeValue(ctx, db);
        if (!ok) return JS_NewFloat64(ctx, NAN);
        mdiff = date_month_diff(fa, fb);
        return JS_NewFloat64(ctx, unit == DU_Y ? trunc(mdiff / 12) : trunc(mdiff));
    }
    d = (to_ms - from_ms) / js_date_unit_ms[unit];
    return JS_NewFloat64(ctx, trunc(d));
}

static JSValue js_date_ext_add(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    double n, ms, f[9];
    int st;
    if (JS_ToFloat64(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    if (magic == DU_MON || magic == DU_Y) {
        st = date_this_fields(ctx, this_val, f);
        if (st < 0) return JS_EXCEPTION;
        if (st == 1) return JS_NewDate(ctx, NAN);
        if (magic == DU_Y) f[0] += n; else f[1] += n;
        return JS_NewDate(ctx, set_date_fields(f, 1));
    }
    if (JS_ThisTimeValue(ctx, &ms, this_val)) return JS_EXCEPTION;
    return JS_NewDate(ctx, ms + n * js_date_unit_ms[magic]);
}

enum { DB_DAY, DB_WEEK, DB_MONTH, DB_YEAR };

static JSValue js_date_ext_boundary(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    int unit = magic / 2, end = magic % 2;
    double f[9];
    int st;
    (void)argc; (void)argv;
    st = date_this_fields(ctx, this_val, f);
    if (st < 0) return JS_EXCEPTION;
    if (st == 1) return JS_NewDate(ctx, NAN);
    if (unit == DB_YEAR)  { f[1] = end ? 11 : 0; }
    if (unit == DB_YEAR || unit == DB_MONTH) { f[2] = 1; }
    if (unit == DB_WEEK)  { f[2] -= f[7]; }         /* back to Sunday */
    if (end) {
        if (unit == DB_WEEK)  f[2] += 6;
        if (unit == DB_MONTH) { f[2] = month_days[(int)f[1]] + ((int)f[1] == 1 ? (int)(days_in_year((int64_t)f[0]) - 365) : 0); }
        if (unit == DB_YEAR)  f[2] = 31;
        f[3] = 23; f[4] = 59; f[5] = 59; f[6] = 999;
    } else {
        f[3] = f[4] = f[5] = f[6] = 0;
    }
    return JS_NewDate(ctx, set_date_fields(f, 1));
}

/* advance/rewind({years,months,weeks,days,hours,minutes,seconds,milliseconds}).
 * magic 1 = rewind (negate). */
static JSValue js_date_ext_advance(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    static const char * const keys[8] = {
        "years","months","weeks","days","hours","minutes","seconds","milliseconds" };
    double f[9];
    int st, i, sign = magic ? -1 : 1;
    JSValueConst spec = argc > 0 ? argv[0] : JS_UNDEFINED;
    double vals[8] = {0,0,0,0,0,0,0,0};
    for (i = 0; i < 8; i++) {
        JSValue v = JS_GetPropertyStr(ctx, spec, keys[i]);
        if (JS_IsException(v)) return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) { if (JS_ToFloat64(ctx, &vals[i], v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; } }
        JS_FreeValue(ctx, v);
    }
    st = date_this_fields(ctx, this_val, f);
    if (st < 0) return JS_EXCEPTION;
    if (st == 1) return JS_NewDate(ctx, NAN);
    f[0] += sign * vals[0];                          /* years  */
    f[1] += sign * vals[1];                          /* months */
    f[2] += sign * (vals[2] * 7 + vals[3]);          /* weeks + days */
    f[3] += sign * vals[4];                          /* hours */
    f[4] += sign * vals[5];                          /* minutes */
    f[5] += sign * vals[6];                          /* seconds */
    f[6] += sign * vals[7];                          /* ms */
    return JS_NewDate(ctx, set_date_fields(f, 1));
}

static JSValue js_date_ext_clone(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    double ms;
    (void)argc; (void)argv;
    if (JS_ThisTimeValue(ctx, &ms, this_val)) return JS_EXCEPTION;
    return JS_NewDate(ctx, ms);
}

/* format(mask) — {token} substitution (yyyy yy MM M dd d HH H hh h mm m ss s
 * SSS Mon Month dow Weekday tt TT). No mask -> ISO-ish default. */
static JSValue js_date_ext_format(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    double f[9];
    int st, y, mo, d, h, mi, s, ms, wd, h12;
    const char *mask = NULL;
    char def[40];
    StringBuffer b_s, *b = &b_s;
    const char *p;
    (void)magic;
    st = date_this_fields(ctx, this_val, f);
    if (st < 0) return JS_EXCEPTION;
    if (st == 1) return js_new_string8(ctx, "Invalid Date");
    y = (int)f[0]; mo = (int)f[1]; d = (int)f[2]; h = (int)f[3];
    mi = (int)f[4]; s = (int)f[5]; ms = (int)f[6]; wd = (int)f[7];
    h12 = h % 12; if (h12 == 0) h12 = 12;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        mask = JS_ToCString(ctx, argv[0]);
        if (!mask) return JS_EXCEPTION;
    } else {
        snprintf(def, sizeof(def), "%04d-%02d-%02d %02d:%02d:%02d", y, mo + 1, d, h, mi, s);
        return js_new_string8(ctx, def);
    }
    if (string_buffer_init(ctx, b, (int)strlen(mask))) { JS_FreeCString(ctx, mask); return JS_EXCEPTION; }
    p = mask;
    while (*p) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            char tok[16], out[32];
            const char *rep = NULL;
            int tn;
            if (!e) { if (string_buffer_putc8(b, '{')) goto fail; p++; continue; }
            tn = (int)(e - p - 1);
            if (tn <= 0 || tn >= (int)sizeof(tok)) { p = e + 1; continue; }
            memcpy(tok, p + 1, tn); tok[tn] = 0;
            out[0] = 0;
            if      (!strcmp(tok, "yyyy")) snprintf(out, sizeof(out), "%04d", y);
            else if (!strcmp(tok, "yy"))   snprintf(out, sizeof(out), "%02d", ((y % 100) + 100) % 100);
            else if (!strcmp(tok, "MM"))   snprintf(out, sizeof(out), "%02d", mo + 1);
            else if (!strcmp(tok, "M"))    snprintf(out, sizeof(out), "%d", mo + 1);
            else if (!strcmp(tok, "dd"))   snprintf(out, sizeof(out), "%02d", d);
            else if (!strcmp(tok, "d"))    snprintf(out, sizeof(out), "%d", d);
            else if (!strcmp(tok, "HH"))   snprintf(out, sizeof(out), "%02d", h);
            else if (!strcmp(tok, "H"))    snprintf(out, sizeof(out), "%d", h);
            else if (!strcmp(tok, "hh"))   snprintf(out, sizeof(out), "%02d", h12);
            else if (!strcmp(tok, "h"))    snprintf(out, sizeof(out), "%d", h12);
            else if (!strcmp(tok, "mm"))   snprintf(out, sizeof(out), "%02d", mi);
            else if (!strcmp(tok, "m"))    snprintf(out, sizeof(out), "%d", mi);
            else if (!strcmp(tok, "ss"))   snprintf(out, sizeof(out), "%02d", s);
            else if (!strcmp(tok, "s"))    snprintf(out, sizeof(out), "%d", s);
            else if (!strcmp(tok, "SSS"))  snprintf(out, sizeof(out), "%03d", ms);
            else if (!strcmp(tok, "tt"))   rep = (h < 12) ? "am" : "pm";
            else if (!strcmp(tok, "TT"))   rep = (h < 12) ? "AM" : "PM";
            else if (!strcmp(tok, "Mon"))     rep = js_date_month_abbr[mo];
            else if (!strcmp(tok, "Month"))   rep = js_date_month_full[mo];
            else if (!strcmp(tok, "dow"))     rep = js_date_day_abbr[wd];
            else if (!strcmp(tok, "Weekday")) rep = js_date_day_full[wd];
            if (!rep) rep = out;
            if (string_buffer_puts8(b, rep)) goto fail;
            p = e + 1;
        } else {
            if (string_buffer_putc8(b, (uint8_t)*p)) goto fail;
            p++;
        }
    }
    JS_FreeCString(ctx, mask);
    return string_buffer_end(b);
 fail:
    string_buffer_free(b);
    JS_FreeCString(ctx, mask);
    return JS_EXCEPTION;
}

/* relative() — English "N units ago" / "in N units" / "just now". */
static JSValue js_date_ext_relative(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    static const struct { double ms; const char *name; } U[] = {
        { 31557600000.0, "year" }, { 2629800000.0, "month" }, { 604800000.0, "week" },
        { 86400000.0, "day" }, { 3600000.0, "hour" }, { 60000.0, "minute" }, { 1000.0, "second" },
    };
    double ms, delta, ad;
    long long n;
    unsigned i;
    int past;
    char out[64];
    (void)argc; (void)argv; (void)magic;
    if (JS_ThisTimeValue(ctx, &ms, this_val)) return JS_EXCEPTION;
    if (isnan(ms)) return js_new_string8(ctx, "Invalid Date");
    delta = (double)date_now() - ms;
    past = delta >= 0;
    ad = fabs(delta);
    if (ad < 1000.0) return js_new_string8(ctx, "just now");
    for (i = 0; i < countof(U) - 1; i++) if (ad >= U[i].ms) break;
    n = (long long)(ad / U[i].ms);
    snprintf(out, sizeof(out), past ? "%lld %s%s ago" : "in %lld %s%s",
             n, U[i].name, n == 1 ? "" : "s");
    return js_new_string8(ctx, out);
}

static JSValue js_date_ext_iso(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return get_date_string(ctx, this_val, 0, NULL, 0x23);   /* toISOString */
}

static const JSCFunctionListEntry js_date_ext_funcs[] = {
    /* predicates */
    JS_CFUNC_MAGIC_DEF("isValid",     0, js_date_ext_pred, DP_VALID ),
    JS_CFUNC_MAGIC_DEF("isToday",     0, js_date_ext_pred, DP_TODAY ),
    JS_CFUNC_MAGIC_DEF("isYesterday", 0, js_date_ext_pred, DP_YESTERDAY ),
    JS_CFUNC_MAGIC_DEF("isTomorrow",  0, js_date_ext_pred, DP_TOMORROW ),
    JS_CFUNC_MAGIC_DEF("isFuture",    0, js_date_ext_pred, DP_FUTURE ),
    JS_CFUNC_MAGIC_DEF("isPast",      0, js_date_ext_pred, DP_PAST ),
    JS_CFUNC_MAGIC_DEF("isWeekday",   0, js_date_ext_pred, DP_WEEKDAY ),
    JS_CFUNC_MAGIC_DEF("isWeekend",   0, js_date_ext_pred, DP_WEEKEND ),
    JS_CFUNC_MAGIC_DEF("isLeapYear",  0, js_date_ext_pred, DP_LEAP ),
    JS_CFUNC_MAGIC_DEF("isSunday",    0, js_date_ext_pred, DP_DOW_BASE + 0 ),
    JS_CFUNC_MAGIC_DEF("isMonday",    0, js_date_ext_pred, DP_DOW_BASE + 1 ),
    JS_CFUNC_MAGIC_DEF("isTuesday",   0, js_date_ext_pred, DP_DOW_BASE + 2 ),
    JS_CFUNC_MAGIC_DEF("isWednesday", 0, js_date_ext_pred, DP_DOW_BASE + 3 ),
    JS_CFUNC_MAGIC_DEF("isThursday",  0, js_date_ext_pred, DP_DOW_BASE + 4 ),
    JS_CFUNC_MAGIC_DEF("isFriday",    0, js_date_ext_pred, DP_DOW_BASE + 5 ),
    JS_CFUNC_MAGIC_DEF("isSaturday",  0, js_date_ext_pred, DP_DOW_BASE + 6 ),
    JS_CFUNC_MAGIC_DEF("isJanuary",   0, js_date_ext_pred, DP_MON_BASE + 0 ),
    JS_CFUNC_MAGIC_DEF("isFebruary",  0, js_date_ext_pred, DP_MON_BASE + 1 ),
    JS_CFUNC_MAGIC_DEF("isMarch",     0, js_date_ext_pred, DP_MON_BASE + 2 ),
    JS_CFUNC_MAGIC_DEF("isApril",     0, js_date_ext_pred, DP_MON_BASE + 3 ),
    JS_CFUNC_MAGIC_DEF("isMay",       0, js_date_ext_pred, DP_MON_BASE + 4 ),
    JS_CFUNC_MAGIC_DEF("isJune",      0, js_date_ext_pred, DP_MON_BASE + 5 ),
    JS_CFUNC_MAGIC_DEF("isJuly",      0, js_date_ext_pred, DP_MON_BASE + 6 ),
    JS_CFUNC_MAGIC_DEF("isAugust",    0, js_date_ext_pred, DP_MON_BASE + 7 ),
    JS_CFUNC_MAGIC_DEF("isSeptember", 0, js_date_ext_pred, DP_MON_BASE + 8 ),
    JS_CFUNC_MAGIC_DEF("isOctober",   0, js_date_ext_pred, DP_MON_BASE + 9 ),
    JS_CFUNC_MAGIC_DEF("isNovember",  0, js_date_ext_pred, DP_MON_BASE + 10 ),
    JS_CFUNC_MAGIC_DEF("isDecember",  0, js_date_ext_pred, DP_MON_BASE + 11 ),
    /* query */
    JS_CFUNC_MAGIC_DEF("getWeekday",   0, js_date_ext_query, DQ_WEEKDAY ),
    JS_CFUNC_MAGIC_DEF("getISOWeek",   0, js_date_ext_query, DQ_ISOWEEK ),
    JS_CFUNC_MAGIC_DEF("daysInMonth",  0, js_date_ext_query, DQ_DAYSINMONTH ),
    /* compare */
    JS_CFUNC_MAGIC_DEF("isBefore",  1, js_date_ext_cmp, DC_BEFORE ),
    JS_CFUNC_MAGIC_DEF("isAfter",   1, js_date_ext_cmp, DC_AFTER ),
    JS_CFUNC_MAGIC_DEF("isBetween", 2, js_date_ext_cmp, DC_BETWEEN ),
    /* diffs: unit*4 + mode {Since,Until,Ago,FromNow} */
    JS_CFUNC_MAGIC_DEF("millisecondsSince",   1, js_date_ext_diff, DU_MS*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("millisecondsUntil",   1, js_date_ext_diff, DU_MS*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("millisecondsAgo",     0, js_date_ext_diff, DU_MS*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("millisecondsFromNow", 0, js_date_ext_diff, DU_MS*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("secondsSince",   1, js_date_ext_diff, DU_S*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("secondsUntil",   1, js_date_ext_diff, DU_S*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("secondsAgo",     0, js_date_ext_diff, DU_S*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("secondsFromNow", 0, js_date_ext_diff, DU_S*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("minutesSince",   1, js_date_ext_diff, DU_MIN*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("minutesUntil",   1, js_date_ext_diff, DU_MIN*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("minutesAgo",     0, js_date_ext_diff, DU_MIN*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("minutesFromNow", 0, js_date_ext_diff, DU_MIN*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("hoursSince",   1, js_date_ext_diff, DU_H*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("hoursUntil",   1, js_date_ext_diff, DU_H*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("hoursAgo",     0, js_date_ext_diff, DU_H*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("hoursFromNow", 0, js_date_ext_diff, DU_H*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("daysSince",   1, js_date_ext_diff, DU_D*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("daysUntil",   1, js_date_ext_diff, DU_D*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("daysAgo",     0, js_date_ext_diff, DU_D*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("daysFromNow", 0, js_date_ext_diff, DU_D*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("weeksSince",   1, js_date_ext_diff, DU_W*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("weeksUntil",   1, js_date_ext_diff, DU_W*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("weeksAgo",     0, js_date_ext_diff, DU_W*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("weeksFromNow", 0, js_date_ext_diff, DU_W*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("monthsSince",   1, js_date_ext_diff, DU_MON*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("monthsUntil",   1, js_date_ext_diff, DU_MON*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("monthsAgo",     0, js_date_ext_diff, DU_MON*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("monthsFromNow", 0, js_date_ext_diff, DU_MON*4 + DM_FROMNOW ),
    JS_CFUNC_MAGIC_DEF("yearsSince",   1, js_date_ext_diff, DU_Y*4 + DM_SINCE ),
    JS_CFUNC_MAGIC_DEF("yearsUntil",   1, js_date_ext_diff, DU_Y*4 + DM_UNTIL ),
    JS_CFUNC_MAGIC_DEF("yearsAgo",     0, js_date_ext_diff, DU_Y*4 + DM_AGO ),
    JS_CFUNC_MAGIC_DEF("yearsFromNow", 0, js_date_ext_diff, DU_Y*4 + DM_FROMNOW ),
    /* add (immutable) */
    JS_CFUNC_MAGIC_DEF("addMilliseconds", 1, js_date_ext_add, DU_MS ),
    JS_CFUNC_MAGIC_DEF("addSeconds",      1, js_date_ext_add, DU_S ),
    JS_CFUNC_MAGIC_DEF("addMinutes",      1, js_date_ext_add, DU_MIN ),
    JS_CFUNC_MAGIC_DEF("addHours",        1, js_date_ext_add, DU_H ),
    JS_CFUNC_MAGIC_DEF("addDays",         1, js_date_ext_add, DU_D ),
    JS_CFUNC_MAGIC_DEF("addWeeks",        1, js_date_ext_add, DU_W ),
    JS_CFUNC_MAGIC_DEF("addMonths",       1, js_date_ext_add, DU_MON ),
    JS_CFUNC_MAGIC_DEF("addYears",        1, js_date_ext_add, DU_Y ),
    /* boundaries: unit*2 + end */
    JS_CFUNC_MAGIC_DEF("beginningOfDay",   0, js_date_ext_boundary, DB_DAY*2 + 0 ),
    JS_CFUNC_MAGIC_DEF("endOfDay",         0, js_date_ext_boundary, DB_DAY*2 + 1 ),
    JS_CFUNC_MAGIC_DEF("beginningOfWeek",  0, js_date_ext_boundary, DB_WEEK*2 + 0 ),
    JS_CFUNC_MAGIC_DEF("endOfWeek",        0, js_date_ext_boundary, DB_WEEK*2 + 1 ),
    JS_CFUNC_MAGIC_DEF("beginningOfMonth", 0, js_date_ext_boundary, DB_MONTH*2 + 0 ),
    JS_CFUNC_MAGIC_DEF("endOfMonth",       0, js_date_ext_boundary, DB_MONTH*2 + 1 ),
    JS_CFUNC_MAGIC_DEF("beginningOfYear",  0, js_date_ext_boundary, DB_YEAR*2 + 0 ),
    JS_CFUNC_MAGIC_DEF("endOfYear",        0, js_date_ext_boundary, DB_YEAR*2 + 1 ),
    /* manipulate + format */
    JS_CFUNC_MAGIC_DEF("advance", 1, js_date_ext_advance, 0 ),
    JS_CFUNC_MAGIC_DEF("rewind",  1, js_date_ext_advance, 1 ),
    JS_CFUNC_DEF("clone",    0, js_date_ext_clone ),
    JS_CFUNC_MAGIC_DEF("format", 1, js_date_ext_format, 0 ),
    JS_CFUNC_MAGIC_DEF("relative", 0, js_date_ext_relative, 0 ),
    JS_CFUNC_DEF("iso",      0, js_date_ext_iso ),
};

static const JSCFunctionListEntry js_date_proto_funcs[] = {
    JS_CFUNC_DEF("valueOf", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("toString", 0, get_date_string, 0x13 ),
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, js_date_Symbol_toPrimitive ),
    JS_CFUNC_MAGIC_DEF("toUTCString", 0, get_date_string, 0x03 ),
    JS_ALIAS_DEF("toGMTString", "toUTCString" ),
    JS_CFUNC_MAGIC_DEF("toISOString", 0, get_date_string, 0x23 ),
    JS_CFUNC_MAGIC_DEF("toDateString", 0, get_date_string, 0x11 ),
    JS_CFUNC_MAGIC_DEF("toTimeString", 0, get_date_string, 0x12 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, get_date_string, 0x33 ),
    JS_CFUNC_MAGIC_DEF("toLocaleDateString", 0, get_date_string, 0x31 ),
    JS_CFUNC_MAGIC_DEF("toLocaleTimeString", 0, get_date_string, 0x32 ),
    JS_CFUNC_DEF("getTimezoneOffset", 0, js_date_getTimezoneOffset ),
    JS_CFUNC_DEF("getTime", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("getYear", 0, get_date_field, 0x101 ),
    JS_CFUNC_MAGIC_DEF("getFullYear", 0, get_date_field, 0x01 ),
    JS_CFUNC_MAGIC_DEF("getUTCFullYear", 0, get_date_field, 0x00 ),
    JS_CFUNC_MAGIC_DEF("getMonth", 0, get_date_field, 0x11 ),
    JS_CFUNC_MAGIC_DEF("getUTCMonth", 0, get_date_field, 0x10 ),
    JS_CFUNC_MAGIC_DEF("getDate", 0, get_date_field, 0x21 ),
    JS_CFUNC_MAGIC_DEF("getUTCDate", 0, get_date_field, 0x20 ),
    JS_CFUNC_MAGIC_DEF("getHours", 0, get_date_field, 0x31 ),
    JS_CFUNC_MAGIC_DEF("getUTCHours", 0, get_date_field, 0x30 ),
    JS_CFUNC_MAGIC_DEF("getMinutes", 0, get_date_field, 0x41 ),
    JS_CFUNC_MAGIC_DEF("getUTCMinutes", 0, get_date_field, 0x40 ),
    JS_CFUNC_MAGIC_DEF("getSeconds", 0, get_date_field, 0x51 ),
    JS_CFUNC_MAGIC_DEF("getUTCSeconds", 0, get_date_field, 0x50 ),
    JS_CFUNC_MAGIC_DEF("getMilliseconds", 0, get_date_field, 0x61 ),
    JS_CFUNC_MAGIC_DEF("getUTCMilliseconds", 0, get_date_field, 0x60 ),
    JS_CFUNC_MAGIC_DEF("getDay", 0, get_date_field, 0x71 ),
    JS_CFUNC_MAGIC_DEF("getUTCDay", 0, get_date_field, 0x70 ),
    JS_CFUNC_DEF("setTime", 1, js_date_setTime ),
    JS_CFUNC_MAGIC_DEF("setMilliseconds", 1, set_date_field, 0x671 ),
    JS_CFUNC_MAGIC_DEF("setUTCMilliseconds", 1, set_date_field, 0x670 ),
    JS_CFUNC_MAGIC_DEF("setSeconds", 2, set_date_field, 0x571 ),
    JS_CFUNC_MAGIC_DEF("setUTCSeconds", 2, set_date_field, 0x570 ),
    JS_CFUNC_MAGIC_DEF("setMinutes", 3, set_date_field, 0x471 ),
    JS_CFUNC_MAGIC_DEF("setUTCMinutes", 3, set_date_field, 0x470 ),
    JS_CFUNC_MAGIC_DEF("setHours", 4, set_date_field, 0x371 ),
    JS_CFUNC_MAGIC_DEF("setUTCHours", 4, set_date_field, 0x370 ),
    JS_CFUNC_MAGIC_DEF("setDate", 1, set_date_field, 0x231 ),
    JS_CFUNC_MAGIC_DEF("setUTCDate", 1, set_date_field, 0x230 ),
    JS_CFUNC_MAGIC_DEF("setMonth", 2, set_date_field, 0x131 ),
    JS_CFUNC_MAGIC_DEF("setUTCMonth", 2, set_date_field, 0x130 ),
    JS_CFUNC_DEF("setYear", 1, js_date_setYear ),
    JS_CFUNC_MAGIC_DEF("setFullYear", 3, set_date_field, 0x031 ),
    JS_CFUNC_MAGIC_DEF("setUTCFullYear", 3, set_date_field, 0x030 ),
    JS_CFUNC_DEF("toJSON", 1, js_date_toJSON ),
};

JSValue JS_NewDate(JSContext *ctx, double epoch_ms)
{
    JSValue obj = js_create_from_ctor(ctx, JS_UNDEFINED, JS_CLASS_DATE);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_SetObjectData(ctx, obj, __JS_NewFloat64(ctx, time_clip(epoch_ms)));
    return obj;
}

int JS_AddIntrinsicDate(JSContext *ctx)
{
    JSValue obj;

    /* Date */
    obj = JS_NewCConstructor(ctx, JS_CLASS_DATE, "Date",
                                    js_date_constructor, 7, JS_CFUNC_constructor_or_func, 0,
                                    JS_UNDEFINED,
                                    js_date_funcs, countof(js_date_funcs),
                                    js_date_proto_funcs, countof(js_date_proto_funcs),
                                    0);
    if (JS_IsException(obj))
        return -1;
    /* non-ES Date.prototype extensions, non-enumerable. */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_DATE],
                               js_date_ext_funcs, countof(js_date_ext_funcs));
    JS_FreeValue(ctx, obj);
    return 0;
}

/* eval */

int JS_AddIntrinsicEval(JSContext *ctx)
{
    ctx->eval_internal = __JS_EvalInternal;
    return 0;
}

