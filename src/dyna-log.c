/* dyna:log -- leveled structured logging. The Logger is a compiled capability:
   the level, name, base fields and format are parsed once and the per-call path
   only appends. Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"
#include "dyna-simd-kernels.h"   /* simd.find_bitmap for the escape scan */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A log line is not a data dump. An unbounded one is how a log statement takes
   down a service, so the line is truncated rather than allowed to grow. */
#define DYN_LOG_MAX_LINE (64u * 1024u)

enum { DYN_LV_TRACE, DYN_LV_DEBUG, DYN_LV_INFO, DYN_LV_WARN, DYN_LV_ERROR,
       DYN_LV_FATAL, DYN_LV_SILENT };

static const char *const DYN_LV_NAME[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "silent"
};

/* Timestamp rendering. `epochMs` and `iso` are the two shapes a log consumer
   actually parses; false omits the field entirely. */
enum { DYN_TS_EPOCH, DYN_TS_ISO, DYN_TS_NONE };

typedef struct {
    int level;
    int ts_mode;
    char *name;             /* may be NULL */
    char *base;             /* pre-serialized `"k":v,...` prefix, may be NULL */
    size_t base_len;
    /* The formatted second is cached: a service logging 100k lines/s renders
       the date once per second, not once per line. */
    int64_t cached_sec;
    char cached_iso[20];   /* "2026-07-31T13:33:30" + NUL */
} dyn_logger_t;

static JSClassID dyn_logger_class_id;

static void dyn_logger_free(void *p)
{
    dyn_logger_t *L = (dyn_logger_t *)p;
    free(L->name);
    free(L->base);
    free(L);
}

static void dyn_logger_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_logger_t *L = (dyn_logger_t *)JS_GetOpaque(val, dyn_logger_class_id);
    (void)rt;
    if (L)
        dyn_logger_free(L);
}

static JSClassDef dyn_logger_class = {
    "Logger", .finalizer = dyn_logger_finalizer,
};

static int dyn_level_of(const char *s)
{
    size_t i;
    for (i = 0; i < countof(DYN_LV_NAME); i++)
        if (strcmp(s, DYN_LV_NAME[i]) == 0)
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------ line buffer */

typedef struct { char *p; size_t n, cap; int truncated; } dyn_line_t;

/* Overshoot costs ABSOLUTE bytes, so the factor decays with size: doubling is
   free while small and wastes megabytes at scale. */
static size_t dyn_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static void dyn_line_put(dyn_line_t *b, const char *s, size_t n)
{
    if (b->truncated)
        return;
    if (b->n + n > DYN_LOG_MAX_LINE) {
        n = (b->n < DYN_LOG_MAX_LINE) ? DYN_LOG_MAX_LINE - b->n : 0;
        b->truncated = 1;
    }
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char *np;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->truncated = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_line_puts(dyn_line_t *b, const char *s)
{
    dyn_line_put(b, s, strlen(s));
}

/* A JSON string literal. The escape set is JSON's, so a message containing a
   quote or a newline cannot break the line framing a consumer relies on. */
/* Bytes that end a clean run. ONE table, shared with the switch below it. */
/* ONE table for both paths -- a bitmap the SIMD scan takes as-is and the
   scalar loop tests directly, so the two cannot drift apart.
   Members: 0x00-0x1F, '"', '\\'. */
static const uint8_t DYN_LOG_ESC_BM[32] = { 0xff, 0xff, 0xff, 0xff, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
#define DYN_LOG_ESC(c) (DYN_LOG_ESC_BM[(c) >> 3] & (1u << ((c) & 7)))
/* Below this the indirect call outweighs the scan (the CSV-field lesson). */
#define DYN_LOG_SIMD_MIN 64

static void dyn_line_json_str(dyn_line_t *b, const char *s, size_t n)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i;
    dyn_line_put(b, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!DYN_LOG_ESC(c)) {                 /* bulk-copy the clean run */
            size_t run = i;
            if (n - i >= DYN_LOG_SIMD_MIN) {
                size_t k = simd.find_bitmap((const uint8_t *)s + i, n - i,
                                            DYN_LOG_ESC_BM);
                i = (k == (size_t)-1) ? n : i + k;
            } else {
                while (i < n && !DYN_LOG_ESC((unsigned char)s[i]))
                    i++;
            }
            dyn_line_put(b, s + run, i - run);
            if (i >= n) break;
            c = (unsigned char)s[i];
        }
        switch (c) {
        case '"':  dyn_line_puts(b, "\\\""); break;
        case '\\': dyn_line_puts(b, "\\\\"); break;
        case '\n': dyn_line_puts(b, "\\n");  break;
        case '\r': dyn_line_puts(b, "\\r");  break;
        case '\t': dyn_line_puts(b, "\\t");  break;
        case '\b': dyn_line_puts(b, "\\b");  break;
        case '\f': dyn_line_puts(b, "\\f");  break;
        default:
            if (c < 0x20) {
                char e[6] = { '\\', 'u', '0', '0', 0, 0 };
                e[4] = HEX[(c >> 4) & 0xF];
                e[5] = HEX[c & 0xF];
                dyn_line_put(b, e, 6);
            } else {
                dyn_line_put(b, (const char *)&c, 1);
            }
        }
    }
    dyn_line_put(b, "\"", 1);
}

/* Render the timestamp, reusing the cached second when it has not changed. */
static void dyn_line_time(dyn_line_t *b, dyn_logger_t *L)
{
    struct timespec ts;
    /* `"time":"2026-07-31T13:33:30.038Z",` is 34 bytes; at 32 snprintf
       truncated mid-field and emitted malformed JSON. */
    char num[64];
    if (L->ts_mode == DYN_TS_NONE)
        return;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;
    if (L->ts_mode == DYN_TS_EPOCH) {
        int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        snprintf(num, sizeof num, "\"time\":%lld,", (long long)ms);
        dyn_line_puts(b, num);
        return;
    }
    if (ts.tv_sec != L->cached_sec) {
        struct tm tmv;
        time_t t = (time_t)ts.tv_sec;
        gmtime_r(&t, &tmv);
        snprintf(L->cached_iso, sizeof L->cached_iso,
                 "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        L->cached_sec = ts.tv_sec;
    }
    snprintf(num, sizeof num, "\"time\":\"%s.%03dZ\",", L->cached_iso,
             (int)(ts.tv_nsec / 1000000));
    dyn_line_puts(b, num);
}

/* A log line is not a data dump: past this depth the value is elided rather
   than walked, which also bounds the recursion on caller-supplied objects. */
#define DYN_LOG_MAX_DEPTH 8

/* The ancestor chain, for cycle detection. Depth is capped, so this is a fixed
   array and the scan is at most DYN_LOG_MAX_DEPTH pointer compares. */
typedef struct { const void *p[DYN_LOG_MAX_DEPTH]; int n; } dyn_seen_t;

static void dyn_line_value(JSContext *ctx, dyn_line_t *b, JSValueConst v,
                           int depth, dyn_seen_t *seen);

/* An integer without going through the engine's number formatter. */
static void dyn_line_i64(dyn_line_t *b, int64_t v)
{
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { tmp[--i] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) tmp[--i] = '-';
    dyn_line_put(b, tmp + i, sizeof tmp - (size_t)i);
}

static void dyn_line_number(JSContext *ctx, dyn_line_t *b, JSValueConst v)
{
    int32_t i32;
    double d;
    const char *cs;
    JSValue sv;
    if (!JS_ToInt32(ctx, &i32, v)) {           /* the common case: a small int */
        double back;
        if (!JS_ToFloat64(ctx, &back, v) && back == (double)i32) {
            dyn_line_i64(b, i32);
            return;
        }
    }
    if (JS_ToFloat64(ctx, &d, v)) { dyn_line_puts(b, "null"); return; }
    if (d != d || d > 1e308 || d < -1e308) {   /* JSON has no NaN or Infinity */
        dyn_line_puts(b, "null");
        return;
    }
    sv = JS_NewFloat64(ctx, d);
    cs = JS_ToCString(ctx, sv);
    JS_FreeValue(ctx, sv);
    if (cs) { dyn_line_puts(b, cs); JS_FreeCString(ctx, cs); }
    else      dyn_line_puts(b, "null");
}

/* Emit one value. Objects and arrays recurse to DYN_LOG_MAX_DEPTH; past it, and
   for a cycle, the value is elided rather than followed -- a logger that hangs
   on a cyclic field takes down the thing it exists to observe. */
static void dyn_line_value(JSContext *ctx, dyn_line_t *b, JSValueConst v,
                           int depth, dyn_seen_t *seen)
{
    if (JS_IsNull(v) || JS_IsUndefined(v)) { dyn_line_puts(b, "null"); return; }
    if (JS_IsBool(v)) { dyn_line_puts(b, JS_ToBool(ctx, v) ? "true" : "false"); return; }
    if (JS_IsNumber(v)) { dyn_line_number(ctx, b, v); return; }
    if (JS_IsString(v)) {
        size_t n;
        const char *cs = JS_ToCStringLen(ctx, &n, v);
        if (!cs) { dyn_line_puts(b, "null"); return; }
        dyn_line_json_str(b, cs, n);
        JS_FreeCString(ctx, cs);
        return;
    }
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v)) { dyn_line_puts(b, "null"); return; }
    {   /* a value that is its own ancestor is a cycle, not depth */
        int k;
        for (k = 0; k < seen->n; k++)
            if (seen->p[k] == JS_VALUE_GET_PTR(v)) {
                dyn_line_puts(b, "\"[Circular]\"");
                return;
            }
    }
    if (depth >= DYN_LOG_MAX_DEPTH || seen->n >= DYN_LOG_MAX_DEPTH) {
        dyn_line_puts(b, "\"[deep]\"");
        return;
    }
    seen->p[seen->n++] = JS_VALUE_GET_PTR(v);
    if (JS_IsArray(ctx, v)) {
        int64_t len = 0, k;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_ToInt64(ctx, &len, lv)) len = 0;
        JS_FreeValue(ctx, lv);
        if (len > 1000) len = 1000;            /* a log line is not a data dump */
        dyn_line_put(b, "[", 1);
        for (k = 0; k < len; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
            if (k) dyn_line_put(b, ",", 1);
            dyn_line_value(ctx, b, e, depth + 1, seen);
            JS_FreeValue(ctx, e);
            if (b->truncated) break;
        }
        dyn_line_put(b, "]", 1);
        seen->n--;
        return;
    }
    {
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0, i;
        int wrote = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_line_puts(b, "\"[unserializable]\"");
            seen->n--;
            return;
        }
        dyn_line_put(b, "{", 1);
        for (i = 0; i < len && !b->truncated; i++) {
            JSValue kv, pv = JS_GetProperty(ctx, v, tab[i].atom);
            const char *ks;
            size_t kn;
            if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
            if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
            kv = JS_AtomToString(ctx, tab[i].atom);
            ks = JS_IsException(kv) ? NULL : JS_ToCStringLen(ctx, &kn, kv);
            JS_FreeValue(ctx, kv);
            if (ks) {
                if (wrote) dyn_line_put(b, ",", 1);
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, depth + 1, seen);
                wrote = 1;
                JS_FreeCString(ctx, ks);
            }
            JS_FreeValue(ctx, pv);
        }
        dyn_line_put(b, "}", 1);
        JS_FreePropertyEnum(ctx, tab, len);
        seen->n--;
    }
}

/* The caller's fields join THIS line rather than nesting under one, so the
   members are emitted directly instead of serializing an object and splicing
   its braces off. */
static int dyn_line_fields(JSContext *ctx, dyn_line_t *b, JSValueConst v)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_line_puts(b, "\"fields\":\"[unserializable]\",");
        return 0;
    }
    for (i = 0; i < len && !b->truncated; i++) {
        JSValue kv, pv = JS_GetProperty(ctx, v, tab[i].atom);
        const char *ks;
        size_t kn;
        if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
        kv = JS_AtomToString(ctx, tab[i].atom);
        ks = JS_IsException(kv) ? NULL : JS_ToCStringLen(ctx, &kn, kv);
        JS_FreeValue(ctx, kv);
        if (ks) {
            dyn_line_json_str(b, ks, kn);
            dyn_line_put(b, ":", 1);
            dyn_line_value(ctx, b, pv, 1, &seen);
            dyn_line_put(b, ",", 1);
            JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return 0;
}

/* An Error serializes to {type,message,stack}, which is what a log consumer
   expects; JSON.stringify of an Error is `{}` and loses everything. */
static int dyn_line_error(JSContext *ctx, dyn_line_t *b, JSValueConst v)
{
    const char *cs;
    JSValue f;
    dyn_line_puts(b, "\"err\":{");
    f = JS_GetPropertyStr(ctx, v, "name");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, "\"type\":");
    dyn_line_json_str(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
    if (cs) JS_FreeCString(ctx, cs);
    f = JS_GetPropertyStr(ctx, v, "message");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, ",\"message\":");
    dyn_line_json_str(b, cs ? cs : "", cs ? strlen(cs) : 0);
    if (cs) JS_FreeCString(ctx, cs);
    f = JS_GetPropertyStr(ctx, v, "stack");
    if (!JS_IsException(f) && !JS_IsUndefined(f)) {
        cs = JS_ToCString(ctx, f);
        if (cs) {
            dyn_line_puts(b, ",\"stack\":");
            dyn_line_json_str(b, cs, strlen(cs));
            JS_FreeCString(ctx, cs);
        }
    }
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, "},");
    return 0;
}

static dyn_logger_t *dyn_logger_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_logger_t *)dyn_plain_get(ctx, v, dyn_logger_class_id);
}

/* One emit. magic is the level. Argument shapes, pino's: (msg), (fields, msg),
   (err, msg) -- an Error first argument is detected, not declared. */
static JSValue dyn_log_emit(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    dyn_line_t b;
    int has_obj = 0, msg_idx = 0;
    const char *msg = NULL;
    size_t msg_n = 0;

    if (!L)
        return JS_EXCEPTION;
    if (magic < L->level)                    /* the gate, before any work */
        return JS_UNDEFINED;

    b.p = NULL; b.n = 0; b.cap = 0; b.truncated = 0;
    if (argc > 0 && JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0])) {
        has_obj = 1;
        msg_idx = 1;
    }
    if (argc > msg_idx && JS_IsString(argv[msg_idx]))
        msg = JS_ToCStringLen(ctx, &msg_n, argv[msg_idx]);

    dyn_line_put(&b, "{", 1);
    dyn_line_time(&b, L);
    dyn_line_puts(&b, "\"level\":\"");
    dyn_line_puts(&b, DYN_LV_NAME[magic]);
    dyn_line_puts(&b, "\",");
    if (L->name) {
        dyn_line_puts(&b, "\"name\":");
        dyn_line_json_str(&b, L->name, strlen(L->name));
        dyn_line_put(&b, ",", 1);
    }
    if (L->base)
        dyn_line_put(&b, L->base, L->base_len);
    if (has_obj) {
        if (JS_IsError(ctx, argv[0]))
            dyn_line_error(ctx, &b, argv[0]);
        else if (dyn_line_fields(ctx, &b, argv[0]) < 0)
            goto fail;
    }
    dyn_line_puts(&b, "\"msg\":");
    dyn_line_json_str(&b, msg ? msg : "", msg ? msg_n : 0);
    dyn_line_puts(&b, "}\n");

    /* ONE write(2) per line. fwrite gives no guarantee of one syscall, so two
       processes appending to one fd can interleave mid-line; fflush changes
       when, not how many. Under PIPE_BUF a single write is atomic. */
    {
        size_t off = 0;
        while (off < b.n) {
            ssize_t wr = write(STDERR_FILENO, b.p + off, b.n - off);
            if (wr < 0) {
                if (errno == EINTR)
                    continue;
                break;                       /* a broken log must not kill the caller */
            }
            off += (size_t)wr;
        }
    }
    if (msg) JS_FreeCString(ctx, msg);
    free(b.p);
    return JS_UNDEFINED;
 fail:
    if (msg) JS_FreeCString(ctx, msg);
    free(b.p);
    return JS_EXCEPTION;
}

/* Pre-serialize a base-field object into the `"k":v,` prefix every line memcpys. */
static int dyn_base_prefix(JSContext *ctx, JSValueConst v, char **out, size_t *outn)
{
    JSValue s;
    const char *cs;
    size_t n;
    *out = NULL; *outn = 0;
    if (!JS_IsObject(v))
        return 0;
    s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s))
        return -1;
    cs = JS_ToCStringLen(ctx, &n, s);
    JS_FreeValue(ctx, s);
    if (!cs)
        return -1;
    if (n > 2 && cs[0] == '{') {
        *out = (char *)malloc(n);            /* n-2 body + one comma */
        if (!*out) { JS_FreeCString(ctx, cs); return -1; }
        memcpy(*out, cs + 1, n - 2);
        (*out)[n - 2] = ',';
        *outn = n - 1;
    }
    JS_FreeCString(ctx, cs);
    return 0;
}

static int dyn_logger_opts(JSContext *ctx, JSValueConst opts, dyn_logger_t *L)
{
    JSValue v;
    const char *s;

    L->level = DYN_LV_INFO;
    L->ts_mode = DYN_TS_EPOCH;
    L->name = NULL; L->base = NULL; L->base_len = 0;
    L->cached_sec = -1;
    L->cached_iso[0] = 0;
    if (!JS_IsObject(opts))
        return 0;

    v = JS_GetPropertyStr(ctx, opts, "level");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            int lv = dyn_level_of(s);
            JS_FreeCString(ctx, s);
            if (lv < 0) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "new Logger({ level }): unknown level");
                return -1;
            }
            L->level = lv;
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "name");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) { L->name = strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "timestamp");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            L->ts_mode = (strcmp(s, "iso") == 0) ? DYN_TS_ISO : DYN_TS_EPOCH;
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsBool(v) && !JS_ToBool(ctx, v)) {
        L->ts_mode = DYN_TS_NONE;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "base");
    if (dyn_base_prefix(ctx, v, &L->base, &L->base_len) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue dyn_logger_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_logger_t *L = (dyn_logger_t *)calloc(1, sizeof *L);
    if (!L)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_logger_opts(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, L) < 0) {
        dyn_logger_free(L);
        return JS_EXCEPTION;
    }
    return dyn_plain_wrap(ctx, dyn_logger_class_id, L, dyn_logger_free);
}

/* child(fields) -> a new Logger with those fields appended to the base prefix.
   The prefix is serialized once here, not per line. */
static JSValue dyn_logger_child(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val), *C;
    char *add = NULL;
    size_t addn = 0;

    if (!L)
        return JS_EXCEPTION;
    if (argc > 0 && dyn_base_prefix(ctx, argv[0], &add, &addn) < 0)
        return JS_EXCEPTION;
    C = (dyn_logger_t *)calloc(1, sizeof *C);
    if (!C) { free(add); return JS_ThrowOutOfMemory(ctx); }
    C->level = L->level;
    C->ts_mode = L->ts_mode;
    C->cached_sec = -1;
    C->name = L->name ? strdup(L->name) : NULL;
    C->base_len = L->base_len + addn;
    if (C->base_len) {
        C->base = (char *)malloc(C->base_len);
        if (!C->base) { free(add); dyn_logger_free(C); return JS_ThrowOutOfMemory(ctx); }
        if (L->base) memcpy(C->base, L->base, L->base_len);
        if (add) memcpy(C->base + L->base_len, add, addn);
    }
    free(add);
    return dyn_plain_wrap(ctx, dyn_logger_class_id, C, dyn_logger_free);
}

static JSValue dyn_logger_enabled(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    const char *s;
    int lv;
    if (!L)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Logger.enabled(level): level must be a string");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    lv = dyn_level_of(s);
    JS_FreeCString(ctx, s);
    if (lv < 0)
        return JS_ThrowRangeError(ctx, "Logger.enabled(level): unknown level");
    return JS_NewBool(ctx, lv >= L->level);
}

static JSValue dyn_logger_get_level(JSContext *ctx, JSValueConst this_val)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewString(ctx, DYN_LV_NAME[L->level]);
}

static JSValue dyn_logger_set_level(JSContext *ctx, JSValueConst this_val,
                                    JSValueConst val)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    const char *s;
    int lv;
    if (!L)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s)
        return JS_EXCEPTION;
    lv = dyn_level_of(s);
    JS_FreeCString(ctx, s);
    if (lv < 0)
        return JS_ThrowRangeError(ctx, "Logger.level: unknown level");
    L->level = lv;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_logger_proto[] = {
    JS_CFUNC_MAGIC_DEF("trace", 2, dyn_log_emit, DYN_LV_TRACE),
    JS_CFUNC_MAGIC_DEF("debug", 2, dyn_log_emit, DYN_LV_DEBUG),
    JS_CFUNC_MAGIC_DEF("info",  2, dyn_log_emit, DYN_LV_INFO),
    JS_CFUNC_MAGIC_DEF("warn",  2, dyn_log_emit, DYN_LV_WARN),
    JS_CFUNC_MAGIC_DEF("error", 2, dyn_log_emit, DYN_LV_ERROR),
    JS_CFUNC_MAGIC_DEF("fatal", 2, dyn_log_emit, DYN_LV_FATAL),
    JS_CFUNC_DEF("child", 1, dyn_logger_child),
    JS_CFUNC_DEF("enabled", 1, dyn_logger_enabled),
    JS_CGETSET_DEF("level", dyn_logger_get_level, dyn_logger_set_level),
};

/* ------------------------------------------------------------------ Debug */

/* Debug(namespace) -> a function that logs only when DEBUG matches. The match
   is computed ONCE here, not per call: an unmatched Debug must cost nothing. */
static JSValue dyn_debug_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    const char *ns, *msg;
    if (!magic)                              /* namespace not enabled */
        return JS_UNDEFINED;
    ns = JS_ToCString(ctx, data[0]);
    if (!ns)
        return JS_EXCEPTION;
    msg = (argc > 0) ? JS_ToCString(ctx, argv[0]) : NULL;
    fprintf(stderr, "%s %s\n", ns, msg ? msg : "");
    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, ns);
    return JS_UNDEFINED;
}

/* DEBUG=a,b:*  -- comma-separated globs, `*` matches any tail. */
static int dyn_debug_match(const char *pat, const char *ns)
{
    size_t nsn = strlen(ns);          /* hoisted: ns cannot change in the loop */
    while (*pat) {
        const char *e = pat;          /* one pass, not strchr then strlen */
        size_t len;
        while (*e && *e != ',') e++;
        len = (size_t)(e - pat);
        while (len && (*pat == ' ')) { pat++; len--; }
        if (len && pat[len - 1] == '*') {
            if (strncmp(pat, ns, len - 1) == 0)
                return 1;
        } else if (len && nsn == len && strncmp(pat, ns, len) == 0) {
            return 1;
        }
        if (!e)
            break;
        pat = e + 1;
    }
    return 0;
}

static JSValue dyn_debug(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const char *ns, *env;
    JSValue data[1], fn;
    int on;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Debug(namespace): namespace must be a string");
    ns = JS_ToCString(ctx, argv[0]);
    if (!ns)
        return JS_EXCEPTION;
    env = getenv("DEBUG");
    on = (env && *env) ? dyn_debug_match(env, ns) : 0;
    JS_FreeCString(ctx, ns);
    data[0] = JS_DupValue(ctx, argv[0]);
    fn = JS_NewCFunctionData(ctx, dyn_debug_write, 1, on, 1, data);
    JS_FreeValue(ctx, data[0]);
    return fn;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_log_funcs[] = {
    JS_CFUNC_DEF("Debug", 1, dyn_debug),
};

static int dyn_log_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_logger_class_id,
                                 &dyn_logger_class, dyn_logger_proto,
                                 countof(dyn_logger_proto), dyn_logger_ctor,
                                 "Logger") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

int js_nat_init_log(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:log", dyn_log_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Logger");
    return JS_AddModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_LOG */
