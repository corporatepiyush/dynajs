#include "time-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#define DYN_TIME_RFC_BUF 96

static inline int dyn_time_read_nsec(JSContext* ctx, JSValueConst v, int64_t* pns)
{
    return dyn_time_read_int(ctx, v, "nsec", pns);
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
    DYN_TOK_OFFSET,
    DYN_TOK_MON_LONG,
    DYN_TOK_WD_LONG
};

static const char* const dyn_time_month_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char* const dyn_time_weekday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
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
    if (rem >= 7 && memcmp(p, "January", 7) == 0) {
        *adv = 7;
        return DYN_TOK_MON_LONG;
    }
    if (rem >= 6 && memcmp(p, "Monday", 6) == 0) {
        *adv = 6;
        return DYN_TOK_WD_LONG;
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

typedef struct {
    int64_t y;
    int mo, d, h, mi, s, wd;
    int off_min;
} DynTimeParts;

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
    case DYN_TOK_MON_LONG:
        return dyn_time_buf_put(out, dyn_time_month_long[t->mo - 1], strlen(dyn_time_month_long[t->mo - 1]));
    case DYN_TOK_WD_LONG:
        return dyn_time_buf_put(out, dyn_time_weekday_long[t->wd], strlen(dyn_time_weekday_long[t->wd]));
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

typedef struct {
    char* layout;
    size_t llen;
    DynTimeTok* tok;
    size_t ntok;
    int off_min;
} DynTimeFormat;

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

static const char* const dyn_rfc3339_keys[] = { "nsec", "offsetMinutes" };

JSValue dyn_time_format_rfc3339(JSContext* ctx, JSValueConst this_val,
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
        if (dyn_opts_strict(ctx, argv[1], dyn_rfc3339_keys, 2))
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

JSValue dyn_time_format_unix(JSContext* ctx, JSValueConst this_val,
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
        if (dyn_opts_strict(ctx, argv[2], fu_keys, 1)) {
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
        } else if (rem >= 7 && memcmp(p, "January", 7) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_month_long[mo - 1], strlen(dyn_time_month_long[mo - 1])))
                goto done;
            i += 7;
        } else if (rem >= 6 && memcmp(p, "Monday", 6) == 0) {
            if (dyn_time_buf_put(&out, dyn_time_weekday_long[wd], strlen(dyn_time_weekday_long[wd])))
                goto done;
            i += 6;
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

static void dyn_time_format_dispose(void* native)
{
    DynTimeFormat* f = (DynTimeFormat*)native;
    if (!f)
        return;
    free(f->layout);
    free(f->tok);
    free(f);
}

void dyn_time_format_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_time_format_dispose(JS_GetOpaque(val, dyn_time_format_class_id));
}

JSValue dyn_time_format_ctor(JSContext* ctx, JSValueConst new_target,
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
        if (dyn_opts_strict(ctx, argv[1], fmt_keys, 1))
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

JSValue dyn_time_format_format(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_time_format_parse(JSContext* ctx, JSValueConst this_val,
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
            if (pos + tk->len > slen)
                goto fail;
            if (tk->len == 1 ? str[pos] != f->layout[tk->off] : memcmp(str + pos, f->layout + tk->off, tk->len) != 0)
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
        case DYN_TOK_MON_LONG:
            for (i = 0; i < 12; i++) {
                size_t ln = strlen(dyn_time_month_long[i]);
                if (pos + ln <= slen && memcmp(str + pos, dyn_time_month_long[i], ln) == 0)
                    break;
            }
            if (i == 12)
                goto fail;
            mo = i + 1;
            pos += strlen(dyn_time_month_long[i]);
            break;
        case DYN_TOK_WD_LONG:
            for (i = 0; i < 7; i++) {
                size_t ln = strlen(dyn_time_weekday_long[i]);
                if (pos + ln <= slen && memcmp(str + pos, dyn_time_weekday_long[i], ln) == 0)
                    break;
            }
            if (i == 7)
                goto fail;
            pos += strlen(dyn_time_weekday_long[i]);
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

JSValue dyn_time_format_layout(JSContext* ctx, JSValueConst this_val)
{
    DynTimeFormat* f = (DynTimeFormat*)dyn_plain_get(ctx, this_val,
        dyn_time_format_class_id);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, f->layout, f->llen);
}

#endif
