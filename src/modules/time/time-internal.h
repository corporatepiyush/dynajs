#ifndef TIME_INTERNAL_H
#define TIME_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-timek.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_NS_PER_US 1000LL
#define DYN_NS_PER_MS 1000000LL
#define DYN_NS_PER_SEC 1000000000LL
#define DYN_NS_PER_MIN 60000000000LL
#define DYN_NS_PER_HOUR 3600000000000LL
#define DYN_MAX_SAFE_INT 9007199254740991LL
#define DYN_TIME_MIN_YEAR (-271821)
#define DYN_TIME_MAX_YEAR 275760

int dyn_time_utoa(uint64_t v, char* out);

int dyn_time_utoa_pad(uint64_t v, int width, char* out);

static inline int dyn_time_fmt_frac(uint64_t frac, int width, char* out)
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

typedef struct DynTimeBuf {
    JSContext* ctx;
    uint8_t* data;
    size_t len, cap;
} DynTimeBuf;

static inline int dyn_time_buf_init(JSContext* ctx, DynTimeBuf* b, size_t hint)
{
    b->ctx = ctx;
    b->len = 0;
    b->cap = hint > 0 ? hint : 16;
    b->data = js_malloc(ctx, b->cap);
    if (!b->data) {
        b->cap = 0;
        return -1;
    }
    return 0;
}

int dyn_time_buf_reserve(DynTimeBuf* b, size_t extra);

static inline int dyn_time_buf_put(DynTimeBuf* b, const void* src, size_t n)
{
    if (dyn_time_buf_reserve(b, n))
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static inline void dyn_time_buf_free(DynTimeBuf* b)
{
    js_free(b->ctx, b->data);
    b->data = NULL;
}

JSValue dyn_time_duration_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_parse_duration(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_parse_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_parse_duration_secs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const char* const dyn_time_month_abbr[12];

extern const char* const dyn_time_weekday_abbr[7];

int dyn_time_read_int(JSContext* ctx, JSValueConst v, const char* name,
    int64_t* out);

int dyn_time_read_offset(JSContext* ctx, JSValueConst opts,
    int64_t* poff);

JSValue dyn_time_format_rfc3339(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_format_unix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_time_format_class_id;

void dyn_time_format_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_time_format_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_time_format_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_format_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_format_layout(JSContext* ctx, JSValueConst this_val);

JSValue dyn_time_parse_rfc3339(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int64_t tp_days_from_civil(int64_t y, int m, int d);

int tp_days_in_month(int64_t y, int m);

typedef struct {
    int64_t y;
    int m, d;
} tp_date_t;

_Static_assert(sizeof(tp_date_t) == 16, "tp_date_t regained padding");

typedef struct {
    int64_t months, days, ms_part;
} tp_dur_t;

_Static_assert(sizeof(tp_dur_t) == 24, "tp_dur_t regained padding");

extern JSClassID dyn_pdate_class_id;

extern JSClassID dyn_dur_class_id;

extern const JSClassDef dyn_pdate_class;

extern const JSClassDef dyn_dur_class;

JSValue tp_new_dur3(JSContext* ctx, JSValueConst new_target,
    int64_t months, int64_t days, int64_t ms_part);

JSValue dyn_pdate_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_pdate_from(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_dur_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_pdate_proto[14];

extern const JSCFunctionListEntry dyn_dur_proto[6];

#define TP_DAY_MS 86400000

typedef struct {
    int32_t ms;
} tp_time_t;

_Static_assert(sizeof(tp_time_t) == 4, "tp_time_t is no longer one integer");

extern JSClassID dyn_ptime_class_id;

extern const JSClassDef dyn_ptime_class;

JSValue dyn_ptime_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_ptime_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_ptime_proto[9];

typedef struct {
    tp_date_t d;
    int32_t ms;
} tp_dt_t;

_Static_assert(sizeof(tp_dt_t) == 24, "tp_dt_t regained padding");

extern JSClassID dyn_pdt_class_id;

extern const JSClassDef dyn_pdt_class;

JSValue tp_new_dt(JSContext* ctx, JSValueConst new_target,
    int64_t y, int m, int d, int32_t ms);

JSValue dyn_pdt_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_pdt_proto[15];

JSValue dyn_time_duration_ms(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_time_duration_secs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#define DYN_DP_MAX_NAMES 12

extern JSClassID dyn_dp_class_id;

void dyn_dp_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_dp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_dp_parse_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_dp_locale_get(JSContext* ctx, JSValueConst this_val);

JSValue dyn_dp_dayfirst_get(JSContext* ctx, JSValueConst this_val);

#define DYN_RRULE_LIMIT_NONE 0

int dyn_rrule_register(JSContext* ctx, JSModuleDef* m);

#endif

#endif
