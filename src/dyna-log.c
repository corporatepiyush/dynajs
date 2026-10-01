#include "dyna-nat.h"
#include "dyna-simd-kernels.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_LOG_MAX_LINE (64u * 1024u)
#define DYN_LOG_TAIL_RESERVE 16

enum { DYN_LV_TRACE,
    DYN_LV_DEBUG,
    DYN_LV_INFO,
    DYN_LV_WARN,
    DYN_LV_ERROR,
    DYN_LV_FATAL,
    DYN_LV_SILENT };

static const char* const DYN_LV_NAME[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "silent"
};

static const struct {
    const char* name;
    size_t len;
    char padded[7];
} DYN_LV[countof(DYN_LV_NAME)] = {
    { "trace", 5, "trace" },
    { "debug", 5, "debug" },
    { "info", 4, "info " },
    { "warn", 4, "warn " },
    { "error", 5, "error" },
    { "fatal", 5, "fatal" },
    { "silent", 6, "silent" },
};

enum { DYN_TS_EPOCH,
    DYN_TS_ISO,
    DYN_TS_NONE };

enum { DYN_FMT_JSON,
    DYN_FMT_TEXT };

typedef struct dyn_log_sink dyn_log_sink_t;

typedef struct {
    int level;
    int ts_mode;
    int fmt;
    char* name;
    char* base;
    size_t base_len;
    char* base_text;
    size_t base_text_len;
    dyn_log_sink_t* sink;
    JSValue dest_fn;
    int64_t emit_count;
    int64_t sample_n;
    int pid_on, host_on;
    int64_t cached_sec;
    char cached_iso[20];
} dyn_logger_t;

static JSClassID dyn_logger_class_id;

static void dyn_sink_unref(dyn_log_sink_t* s);

static void dyn_logger_free(void* p)
{
    dyn_logger_t* L = (dyn_logger_t*)p;
    dyn_sink_unref(L->sink);
    free(L->name);
    free(L->base);
    free(L->base_text);
    free(L);
}

static void dyn_logger_free_rt(JSRuntime* rt, void* p)
{
    dyn_logger_t* L = (dyn_logger_t*)p;
    if (!L)
        return;
    JS_FreeValueRT(rt, L->dest_fn);
    L->dest_fn = JS_UNDEFINED;
    dyn_logger_free(p);
}

static void dyn_logger_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_logger_t* L = (dyn_logger_t*)JS_GetOpaque(val, dyn_logger_class_id);
    if (L)
        dyn_logger_free_rt(rt, L);
}

static void dyn_logger_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_logger_t* L = (dyn_logger_t*)JS_GetOpaque(val, dyn_logger_class_id);
    if (L && !JS_IsUndefined(L->dest_fn))
        JS_MarkValue(rt, L->dest_fn, mark_func);
}

static JSClassDef dyn_logger_class = {
    "Logger",
    .finalizer = dyn_logger_finalizer,
    .gc_mark = dyn_logger_mark,
};

static int dyn_level_of(const char* s)
{
    size_t i;
    for (i = 0; i < countof(DYN_LV_NAME); i++)
        if (strcmp(s, DYN_LV_NAME[i]) == 0)
            return (int)i;
    return -1;
}

#define DYN_LINE_INLINE 512

typedef struct {
    char* p;
    size_t n, cap;
    int truncated;
    char inln[DYN_LINE_INLINE];
} dyn_line_t;

static void dyn_line_init(dyn_line_t* b)
{
    b->p = b->inln;
    b->n = 0;
    b->cap = DYN_LINE_INLINE;
    b->truncated = 0;
}

static void dyn_line_free(dyn_line_t* b)
{
    if (b->p != b->inln) {
        free(b->p);
        b->p = b->inln;
        b->cap = DYN_LINE_INLINE;
    }
    b->n = 0;
    b->truncated = 0;
}

static size_t dyn_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))
            nc *= 2;
        else if (nc < (1u << 20))
            nc += nc / 2;
        else
            nc += nc / 4;
    }
    return nc;
}

static void dyn_line_put(dyn_line_t* b, const char* s, size_t n)
{
    size_t limit = DYN_LOG_MAX_LINE - DYN_LOG_TAIL_RESERVE;
    if (b->truncated)
        return;
    if (b->n + n > limit) {
        n = (b->n < limit) ? limit - b->n : 0;
        b->truncated = 1;
    }
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char* np;
        if (b->p == b->inln) {
            np = (char*)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char*)realloc(b->p, nc);
        }
        if (!np) {
            b->truncated = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_line_puts(dyn_line_t* b, const char* s)
{
    dyn_line_put(b, s, strlen(s));
}

static void dyn_line_force(dyn_line_t* b, const char* s, size_t n)
{
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char* np;
        if (b->p == b->inln) {
            np = (char*)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char*)realloc(b->p, nc);
        }
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static const uint8_t DYN_LOG_ESC_BM[32] = {
    0xff, 0xff, 0xff, 0xff,
    0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00
};
#define DYN_LOG_ESC(c) (DYN_LOG_ESC_BM[(c) >> 3] & (1u << ((c) & 7)))
#define DYN_LOG_SIMD_MIN 64

enum { DYN_ESC_CTRL,
    DYN_ESC_JSON };

static void dyn_line_escape(dyn_line_t* b, const char* s, size_t n, int mode)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (!DYN_LOG_ESC(c)) {
            size_t run = i;
            if (n - i >= DYN_LOG_SIMD_MIN) {
                size_t k = simd.find_bitmap((const uint8_t*)s + i, n - i,
                    DYN_LOG_ESC_BM);
                i = (k == (size_t)-1) ? n : i + k;
            } else {
                while (i < n && !DYN_LOG_ESC((unsigned char)s[i]))
                    i++;
            }
            dyn_line_put(b, s + run, i - run);
            if (i >= n)
                break;
            c = (unsigned char)s[i];
        }
        if (mode == DYN_ESC_JSON && c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            dyn_line_put(b, (s[i + 2] == 0xA8) ? "\\u2028" : "\\u2029", 6);
            i += 3;
            continue;
        }
        switch (c) {
        case '\n':
            dyn_line_puts(b, "\\n");
            break;
        case '\r':
            dyn_line_puts(b, "\\r");
            break;
        case '\t':
            dyn_line_puts(b, "\\t");
            break;
        case '\b':
            dyn_line_puts(b, "\\b");
            break;
        case '\f':
            dyn_line_puts(b, "\\f");
            break;
        case '"':
            dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\"" : "\"");
            break;
        case '\\':
            dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\\" : "\\");
            break;
        case 0x7F:
            if (mode == DYN_ESC_JSON)
                dyn_line_put(b, "\\u007f", 6);
            else
                dyn_line_put(b, (const char*)&c, 1);
            break;
        default:
            if (c < 0x20) {
                char e[6] = { '\\', 'u', '0', '0', 0, 0 };
                e[4] = HEX[(c >> 4) & 0xF];
                e[5] = HEX[c & 0xF];
                dyn_line_put(b, e, 6);
            } else {
                dyn_line_put(b, (const char*)&c, 1);
            }
        }
        i++;
    }
}

static void dyn_line_json_str(dyn_line_t* b, const char* s, size_t n)
{
    dyn_line_put(b, "\"", 1);
    dyn_line_escape(b, s, n, DYN_ESC_JSON);
    dyn_line_put(b, "\"", 1);
}

static const char* dyn_str_bytes(JSContext* ctx, const char** ps, size_t* plen,
    JSValueConst v)
{
    const char* cs = JS_ToCStringLen(ctx, plen, v);
    *ps = cs;
    return cs;
}

static void dyn_line_esc_ctrl(dyn_line_t* b, const char* s, size_t n)
{
    dyn_line_escape(b, s, n, DYN_ESC_CTRL);
}

static void dyn_line_i64(dyn_line_t* b, int64_t v)
{
    char tmp[24];
    size_t n = i64toa(tmp, v);
    dyn_line_put(b, tmp, n);
}

static int64_t dyn_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char* dyn_hostname(void)
{
    static char host[256];
    static int have;
    if (!have) {
        have = 1;
        if (gethostname(host, sizeof host - 1) != 0)
            host[0] = '\0';
        host[sizeof host - 1] = '\0';
    }
    return host;
}

static const char* dyn_iso_sec(dyn_logger_t* L, int64_t ms, int* millis_out)
{
    int64_t sec = ms / 1000;
    if (sec != L->cached_sec) {
        struct tm tmv;
        time_t t = (time_t)sec;
        if (!gmtime_r(&t, &tmv))
            memset(&tmv, 0, sizeof tmv);
        snprintf(L->cached_iso, sizeof L->cached_iso,
            "%04d-%02d-%02dT%02d:%02d:%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        L->cached_sec = sec;
    }
    *millis_out = (int)(ms % 1000);
    return L->cached_iso;
}

static void dyn_line_time(dyn_line_t* b, dyn_logger_t* L, int64_t ms)
{
    if (L->ts_mode == DYN_TS_NONE)
        return;
    if (L->ts_mode == DYN_TS_EPOCH) {
        dyn_line_put(b, "\"time\":", 7);
        dyn_line_i64(b, ms);
        dyn_line_put(b, ",", 1);
        return;
    }
    {
        int m;
        const char* iso = dyn_iso_sec(L, ms, &m);
        char m3[3] = { (char)('0' + m / 100),
            (char)('0' + (m / 10) % 10),
            (char)('0' + m % 10) };
        dyn_line_put(b, "\"time\":\"", 8);
        dyn_line_put(b, iso, 19);
        dyn_line_put(b, ".", 1);
        dyn_line_put(b, m3, 3);
        dyn_line_put(b, "Z\",", 3);
    }
}

#define DYN_LOG_MAX_DEPTH 8

typedef struct {
    const void* p[DYN_LOG_MAX_DEPTH];
    int n;
} dyn_seen_t;

typedef struct {
    JSPropertyEnum* tab;
    uint32_t len;
    int heap;
    JSPropertyEnum stackbuf[32];
} dyn_props_t;
static int dyn_props_get(JSContext* ctx, JSValueConst v, dyn_props_t* p);
static void dyn_props_free(JSContext* ctx, dyn_props_t* p);
static const char* dyn_key_bytes(JSContext* ctx, size_t* plen, JSAtom atom,
    int* owned);

static void dyn_line_value(JSContext* ctx, dyn_line_t* b, JSValueConst v,
    int depth, dyn_seen_t* seen);

static void dyn_line_number(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    int64_t i64;
    double d;
    char dbuf[64];
    JSDTOATempMem dm;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        dyn_line_i64(b, (int64_t)JS_VALUE_GET_INT(v));
        return;
    }
    if (!JS_ToInt64(ctx, &i64, v)) {
        double back;
        if (!JS_ToFloat64(ctx, &back, v) && back == (double)i64) {
            dyn_line_i64(b, i64);
            return;
        }
    }
    if (JS_ToFloat64(ctx, &d, v)) {
        dyn_line_puts(b, "null");
        return;
    }
    if (!isfinite(d)) {
        dyn_line_puts(b, "null");
        return;
    }
    {
        int n = js_dtoa(dbuf, d, 10, 0, JS_DTOA_FORMAT_FREE, &dm);
        dyn_line_put(b, dbuf, (size_t)n);
    }
}

static void dyn_line_value(JSContext* ctx, dyn_line_t* b, JSValueConst v,
    int depth, dyn_seen_t* seen)
{
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        dyn_line_puts(b, "null");
        return;
    }
    if (JS_IsBool(v)) {
        dyn_line_puts(b, JS_ToBool(ctx, v) ? "true" : "false");
        return;
    }
    if (JS_IsNumber(v)) {
        dyn_line_number(ctx, b, v);
        return;
    }
    if (JS_IsString(v)) {
        size_t n;
        const char* cs = NULL;
        const char* np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) {
            dyn_line_puts(b, "null");
            return;
        }
        dyn_line_json_str(b, np, n);
        if (cs)
            JS_FreeCString(ctx, cs);
        return;
    }
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v)) {
        dyn_line_puts(b, "null");
        return;
    }
    {
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
        if (JS_ToInt64(ctx, &len, lv))
            len = 0;
        JS_FreeValue(ctx, lv);
        if (len > 1000)
            len = 1000;
        dyn_line_put(b, "[", 1);
        for (k = 0; k < len; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
            if (k)
                dyn_line_put(b, ",", 1);
            dyn_line_value(ctx, b, e, depth + 1, seen);
            JS_FreeValue(ctx, e);
            if (b->truncated)
                break;
        }
        dyn_line_put(b, "]", 1);
        seen->n--;
        return;
    }
    {
        dyn_props_t p;
        uint32_t i;
        int wrote = 0;
        if (dyn_props_get(ctx, v, &p) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_line_puts(b, "\"[unserializable]\"");
            seen->n--;
            return;
        }
        dyn_line_put(b, "{", 1);
        for (i = 0; i < p.len && !b->truncated; i++) {
            JSValue pv = JS_GetProperty(ctx, v, p.tab[i].atom);
            const char* ks;
            size_t kn;
            int owned;
            if (JS_IsException(pv)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                continue;
            }
            if (JS_IsUndefined(pv)) {
                JS_FreeValue(ctx, pv);
                continue;
            }
            ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
            if (ks) {
                if (wrote)
                    dyn_line_put(b, ",", 1);
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, depth + 1, seen);
                wrote = 1;
                if (owned)
                    JS_FreeCString(ctx, ks);
            }
            JS_FreeValue(ctx, pv);
        }
        dyn_line_put(b, "}", 1);
        dyn_props_free(ctx, &p);
        seen->n--;
    }
}

static int dyn_json_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int dyn_json_punct(char c)
{
    return c == '"' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':';
}

static void dyn_line_json_repair(dyn_line_t* b)
{
    size_t safe = 1;
    char cstack[32];
    int sp = 0;
    int in_str = 0, in_key = 0, esc = 0, expect_key = 0;
    size_t nn = b->n;
    int k;

    for (size_t i = 0; i < b->n; i++) {
        char c = b->p[i];
        if (in_str) {
            if (esc) {
                esc = 0;
                continue;
            }
            if (c == '\\') {
                esc = 1;
                continue;
            }
            if (c == '"') {
                int was_key = in_key;
                in_str = in_key = 0;
                if (!was_key)
                    safe = i + 1;
            }
            continue;
        }
        switch (c) {
        case '"':
            in_str = 1;
            in_key = expect_key;
            break;
        case ':':
            expect_key = 0;
            break;
        case ',':
            if (i && !dyn_json_punct(b->p[i - 1]))
                safe = i;
            expect_key = sp > 0 && cstack[sp - 1] == '{';
            break;
        case '{':
            if (sp < (int)countof(cstack)) {
                cstack[sp] = '{';
                sp++;
            }
            expect_key = 1;
            safe = i + 1;
            break;
        case '[':
            if (sp < (int)countof(cstack)) {
                cstack[sp] = '[';
                sp++;
            }
            expect_key = 0;
            safe = i + 1;
            break;
        case '}':
        case ']':
            if (sp > 0)
                sp--;
            expect_key = sp > 0 && cstack[sp - 1] == '{';
            safe = i + 1;
            break;
        default:
            break;
        }
    }

    if (in_str && !in_key) {
        size_t hex = 0;
        while (hex < 4 && nn > hex && dyn_json_hex(b->p[nn - 1 - hex]))
            hex++;
        if (hex > 0 && hex < 4 && nn >= hex + 2 && b->p[nn - 1 - hex] == 'u' && b->p[nn - 2 - hex] == '\\')
            nn -= hex + 2;
        {
            size_t bs = 0;
            while (nn > bs && b->p[nn - 1 - bs] == '\\')
                bs++;
            if (bs & 1)
                nn--;
        }
        while (nn > safe && ((unsigned char)b->p[nn - 1] & 0xC0) == 0x80)
            nn--;
        if (nn > safe && (unsigned char)b->p[nn - 1] >= 0xC0)
            nn--;
        b->n = nn;
        dyn_line_force(b, "...", 3);
        dyn_line_force(b, "\"", 1);
    } else {
        b->n = safe;
    }
    for (k = sp - 1; k >= 0; k--)
        dyn_line_force(b, cstack[k] == '{' ? "}" : "]", 1);
    dyn_line_force(b, "\n", 1);
}

static int dyn_key_reserved(dyn_logger_t* L, const char* k, size_t n,
    int reserve_err)
{
    unsigned char c0 = (unsigned char)k[0];
    if (n == 0)
        return 0;
    if (c0 < 'e' || c0 > 't')
        return 0;
    switch (c0) {
    case 't':
        return L->ts_mode != DYN_TS_NONE && n == 4 && memcmp(k, "time", 4) == 0;
    case 'l':
        return n == 5 && memcmp(k, "level", 5) == 0;
    case 'm':
        return n == 3 && memcmp(k, "msg", 3) == 0;
    case 'n':
        return L->name && n == 4 && memcmp(k, "name", 4) == 0;
    case 'p':
        return L->pid_on && n == 3 && memcmp(k, "pid", 3) == 0;
    case 'h':
        return L->host_on && n == 8 && memcmp(k, "hostname", 8) == 0;
    case 'e':
        return reserve_err && n == 3 && memcmp(k, "err", 3) == 0;
    default:
        return 0;
    }
}

#define DYN_FAST_KEYS 32

static int dyn_props_get(JSContext* ctx, JSValueConst v, dyn_props_t* p)
{
    int rc = JS_GetOwnFastProps(ctx, v, p->stackbuf, DYN_FAST_KEYS, &p->len);
    if (rc == 0) {
        p->tab = p->stackbuf;
        p->heap = 0;
        return 0;
    }
    if (rc == -2) {
        p->tab = (JSPropertyEnum*)malloc(p->len * sizeof *p->tab);
        if (!p->tab) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (JS_GetOwnFastProps(ctx, v, p->tab, p->len, &p->len) == 0) {
            p->heap = 1;
            return 0;
        }
        free(p->tab);
    }
    p->tab = NULL;
    p->heap = 2;
    if (JS_GetOwnPropertyNames(ctx, &p->tab, &p->len, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    return 0;
}

static void dyn_props_free(JSContext* ctx, dyn_props_t* p)
{
    uint32_t i;
    if (p->heap == 2) {
        JS_FreePropertyEnum(ctx, p->tab, p->len);
        return;
    }
    for (i = 0; i < p->len; i++)
        JS_FreeAtom(ctx, p->tab[i].atom);
    if (p->heap == 1)
        free(p->tab);
}

static const char* dyn_key_bytes(JSContext* ctx, size_t* plen, JSAtom atom,
    int* owned)
{
    const char* ks = JS_AtomBorrowASCII(ctx, plen, atom);
    *owned = 0;
    if (ks)
        return ks;
    ks = JS_AtomToCStringLen(ctx, plen, atom);
    *owned = ks != NULL;
    return ks;
}

static int dyn_line_fields(JSContext* ctx, dyn_line_t* b, dyn_logger_t* L,
    JSValueConst v, int reserve_err)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_line_puts(b, "\"fields\":\"[unserializable]\",");
        return 0;
    }
    for (i = 0; i < p.len && !b->truncated; i++) {
        JSValue pv;
        const char* ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(pv)) {
            JS_FreeValue(ctx, pv);
            continue;
        }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, reserve_err)) {
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, 1, &seen);
                dyn_line_put(b, ",", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

static int dyn_line_error(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    const char* cs;
    JSValue f;
    dyn_line_puts(b, "\"err\":{");
    f = JS_GetPropertyStr(ctx, v, "name");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, "\"type\":");
    dyn_line_json_str(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
    if (cs)
        JS_FreeCString(ctx, cs);
    f = JS_GetPropertyStr(ctx, v, "message");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, ",\"message\":");
    dyn_line_json_str(b, cs ? cs : "", cs ? strlen(cs) : 0);
    if (cs)
        JS_FreeCString(ctx, cs);
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
    {
        dyn_props_t p;
        uint32_t i;
        dyn_seen_t seen;
        seen.n = 0;
        if (dyn_props_get(ctx, v, &p) >= 0) {
            for (i = 0; i < p.len && !b->truncated; i++) {
                JSValue pv;
                const char* ks;
                size_t kn;
                int owned;
                int skip = 0;
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    skip = (kn == 4 && memcmp(ks, "name", 4) == 0) || (kn == 7 && memcmp(ks, "message", 7) == 0) || (kn == 5 && memcmp(ks, "stack", 5) == 0);
                    if (owned)
                        JS_FreeCString(ctx, ks);
                }
                if (skip)
                    continue;
                pv = JS_GetProperty(ctx, v, p.tab[i].atom);
                if (JS_IsException(pv)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    continue;
                }
                if (JS_IsUndefined(pv)) {
                    JS_FreeValue(ctx, pv);
                    continue;
                }
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    dyn_line_put(b, ",", 1);
                    dyn_line_json_str(b, ks, kn);
                    dyn_line_put(b, ":", 1);
                    seen.n = 0;
                    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
                    dyn_line_value(ctx, b, pv, 1, &seen);
                    if (owned)
                        JS_FreeCString(ctx, ks);
                }
                JS_FreeValue(ctx, pv);
            }
            dyn_props_free(ctx, &p);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    dyn_line_puts(b, "},");
    return 0;
}

#define DYN_LOG_PATH_MAX 4096

struct dyn_log_sink {
    struct dyn_log_sink* next;
    int refs;
    int fd;
    char* path;
    uint64_t max_size;
    int64_t freq_ms;
    int named_date;
    int numbered;
    int keep_count;
    int use_symlink;
    int64_t period_ms;
    char* base;
    char* ext;
    char* active;
    uint64_t written;
    char* buf;
    size_t bufn, bufcap;
    int64_t last_retry;
    uint64_t write_errors;
};

static dyn_log_sink_t* dyn_sinks;

static int dyn_write_all(int fd, const char* p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t wr = write(fd, p + off, n - off);
        if (wr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)wr;
    }
    return 0;
}

static void dyn_sink_write_failed(dyn_log_sink_t* s, int fd)
{
    char msg[192];
    int w;
    s->write_errors++;
    if (s->write_errors != 1 || fd == STDERR_FILENO)
        return;
    w = snprintf(msg, sizeof msg,
        "dyna:log: write to %s failed (%s); log lines are being lost\n",
        s->active ? s->active : (s->path ? s->path : "?"),
        strerror(errno));
    if (w > 0)
        (void)dyn_write_all(STDERR_FILENO, msg, (size_t)w);
}

static int dyn_mkdir_p(const char* path)
{
    char tmp[DYN_LOG_PATH_MAX];
    size_t n = strlen(path);
    char* p;
    if (n == 0 || n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static const char* dyn_split_dir(const char* path, char* dir, size_t cap)
{
    const char* slash = strrchr(path, '/');
    size_t n;
    if (!slash) {
        dir[0] = '\0';
        return path;
    }
    n = (size_t)(slash - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return slash + 1;
}

static int64_t dyn_period_of(int64_t freq_ms, int64_t now)
{
    return now - now % freq_ms;
}

static void dyn_fmt_date(int64_t period_ms, int hourly, char* out, size_t cap)
{
    time_t t = (time_t)(period_ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        memset(&tmv, 0, sizeof tmv);
    if (hourly)
        snprintf(out, cap, "%04d-%02d-%02dT%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
    else
        snprintf(out, cap, "%04d-%02d-%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static int dyn_sink_name(const dyn_log_sink_t* s, unsigned n, char* out,
    size_t cap)
{
    char date[32];
    int w;
    date[0] = '\0';
    if (s->named_date)
        dyn_fmt_date(s->period_ms, s->freq_ms == 3600000, date, sizeof date);
    if (s->named_date && s->numbered)
        w = snprintf(out, cap, "%s.%s.%u%s", s->base, date, n, s->ext);
    else if (s->named_date)
        w = snprintf(out, cap, "%s.%s%s", s->base, date, s->ext);
    else if (s->numbered)
        w = snprintf(out, cap, "%s.%u%s", s->base, n, s->ext);
    else
        w = snprintf(out, cap, "%s%s", s->base, s->ext);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

static unsigned dyn_sink_scan_max_n(const dyn_log_sink_t* s)
{
    char dir[DYN_LOG_PATH_MAX];
    char date[32];
    const char* bn = dyn_split_dir(s->base, dir, sizeof dir);
    size_t bnl = strlen(bn), dl = 0;
    unsigned maxn = 0;
    DIR* d = opendir(dir[0] ? dir : ".");
    struct dirent* de;

    if (s->named_date) {
        dyn_fmt_date(s->period_ms, s->freq_ms == 3600000, date, sizeof date);
        dl = strlen(date);
    }
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        const char* mid = de->d_name + bnl;
        unsigned v = 0;
        int digits = 0;
        if (strncmp(de->d_name, bn, bnl) != 0 || *mid != '.')
            continue;
        mid++;
        if (dl) {
            if (strncmp(mid, date, dl) != 0 || mid[dl] != '.')
                continue;
            mid += dl + 1;
        }
        while (*mid >= '0' && *mid <= '9') {
            v = v * 10u + (unsigned)(*mid - '0');
            mid++;
            digits++;
        }
        if (digits && strcmp(mid, s->ext) == 0 && v > maxn)
            maxn = v;
    }
    closedir(d);
    return maxn;
}

static int dyn_sink_open_numbered(dyn_log_sink_t* s, char* out, size_t cap)
{
    unsigned start = dyn_sink_scan_max_n(s);
    unsigned n;
    for (n = start + 1; n < start + 4096; n++) {
        int fd;
        if (dyn_sink_name(s, n, out, cap) < 0)
            return -1;
        fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

static void dyn_sink_flush(dyn_log_sink_t* s)
{
    if (s->buf && s->bufn) {
        int rc;
        if (s->fd >= 0) {
            rc = dyn_write_all(s->fd, s->buf, s->bufn);
        } else {
            rc = dyn_write_all(STDERR_FILENO, s->buf, s->bufn);
        }
        if (rc < 0)
            dyn_sink_write_failed(s, s->fd >= 0 ? s->fd : STDERR_FILENO);
        s->bufn = 0;
    }
}

static void dyn_sink_prune(dyn_log_sink_t* s);

static void dyn_sink_rotate(dyn_log_sink_t* s, int64_t now)
{
    char path[DYN_LOG_PATH_MAX];
    int fd;
    struct stat st;

    dyn_sink_flush(s);
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s->active);
    s->active = NULL;
    s->written = 0;
    if (s->freq_ms)
        s->period_ms = dyn_period_of(s->freq_ms, now);
    if (s->numbered)
        fd = dyn_sink_open_numbered(s, path, sizeof path);
    else {
        if (dyn_sink_name(s, 0, path, sizeof path) < 0) {
            s->last_retry = now;
            return;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        s->last_retry = now;
        return;
    }
    s->fd = fd;
    if (fstat(fd, &st) == 0)
        s->written = (uint64_t)st.st_size;
    s->active = strdup(path);
    if (s->use_symlink && s->active) {
        const char* abn = strrchr(s->active, '/');
        char tmp[DYN_LOG_PATH_MAX];
        abn = abn ? abn + 1 : s->active;
        if (snprintf(tmp, sizeof tmp, "%s.lnk", s->path) < (int)sizeof tmp) {
            unlink(tmp);
            if (symlink(abn, tmp) == 0)
                rename(tmp, s->path);
        }
    }
    dyn_sink_prune(s);
}

static void dyn_sink_prune(dyn_log_sink_t* s)
{
    char dir[DYN_LOG_PATH_MAX];
    const char* bn;
    size_t bnl, extl;
    DIR* d;
    typedef struct {
        char* name;
        char date[32];
        long num;
    } dyn_rot_t;
    dyn_rot_t* ents = NULL;
    int cnt = 0, cap = 256, i;
    struct dirent* de;
    const char* abn2 = s->active ? strrchr(s->active, '/') : NULL;

    if (!s->keep_count || !s->base)
        return;
    bn = dyn_split_dir(s->base, dir, sizeof dir);
    bnl = strlen(bn);
    extl = strlen(s->ext);
    abn2 = s->active ? (abn2 ? abn2 + 1 : s->active) : NULL;
    d = opendir(dir[0] ? dir : ".");
    if (d) {
        ents = (dyn_rot_t*)malloc((size_t)cap * sizeof *ents);
        while ((de = readdir(d)) != NULL) {
            const char *mid, *dot;
            size_t mlen, k;
            if (ents && cnt >= cap) {
                dyn_rot_t* ne;
                if (cap >= 4096)
                    continue;
                ne = (dyn_rot_t*)realloc(ents, (size_t)(cap * 2) * sizeof *ne);
                if (!ne)
                    continue;
                ents = ne;
                cap *= 2;
            }
            if (abn2 && strcmp(de->d_name, abn2) == 0)
                continue;
            if (strncmp(de->d_name, bn, bnl) != 0 || de->d_name[bnl] != '.')
                continue;
            mlen = strlen(de->d_name);
            if (mlen < bnl + 1 + extl || strcmp(de->d_name + mlen - extl, s->ext) != 0)
                continue;
            mid = de->d_name + bnl + 1;
            mlen -= bnl + 1 + extl;
            if (!mlen)
                continue;
            for (k = 0; k < mlen; k++) {
                char c = mid[k];
                if (!((c >= '0' && c <= '9') || c == '-' || c == 'T' || c == '.'))
                    break;
            }
            if (k != mlen)
                continue;
            if (!ents)
                continue;
            dot = memchr(mid, '.', mlen);
            ents[cnt].name = strdup(de->d_name);
            if (!ents[cnt].name)
                continue;
            if (dot) {
                size_t dl = (size_t)(dot - mid);
                if (dl >= sizeof ents[cnt].date)
                    dl = sizeof ents[cnt].date - 1;
                memcpy(ents[cnt].date, mid, dl);
                ents[cnt].date[dl] = '\0';
                ents[cnt].num = strtol(dot + 1, NULL, 10);
            } else {
                ents[cnt].date[0] = '\0';
                ents[cnt].num = strtol(mid, NULL, 10);
            }
            cnt++;
        }
        closedir(d);
    }
    if (ents && cnt > s->keep_count) {
        int over = cnt - s->keep_count;
        for (i = 1; i < cnt; i++) {
            dyn_rot_t key = ents[i];
            int j = i - 1;
            while (j >= 0 && (strcmp(ents[j].date, key.date) > 0 || (strcmp(ents[j].date, key.date) == 0 && ents[j].num > key.num))) {
                ents[j + 1] = ents[j];
                j--;
            }
            ents[j + 1] = key;
        }
        for (i = 0; i < over; i++) {
            char full[DYN_LOG_PATH_MAX];
            if (snprintf(full, sizeof full, "%s/%s",
                    dir[0] ? dir : ".", ents[i].name)
                < (int)sizeof full)
                unlink(full);
        }
    }
    if (ents) {
        for (i = 0; i < cnt; i++)
            free(ents[i].name);
        free(ents);
    }
}

static void dyn_sink_maybe_rotate(dyn_log_sink_t* s, int64_t now, size_t n)
{
    if (s->fd < 0) {
        if (now >= s->last_retry && now - s->last_retry >= 1000)
            dyn_sink_rotate(s, now);
        return;
    }
    if ((s->freq_ms && now >= s->period_ms + s->freq_ms) || (s->max_size && s->written > 0 && s->written + n > s->max_size))
        dyn_sink_rotate(s, now);
}

static void dyn_sink_write(dyn_log_sink_t* s, int64_t now,
    const char* data, size_t n)
{
    if (!s) {
        (void)dyn_write_all(STDERR_FILENO, data, n);
        return;
    }
    if (s->max_size || s->freq_ms)
        dyn_sink_maybe_rotate(s, now, n);
    if (s->fd < 0) {
        if (dyn_write_all(STDERR_FILENO, data, n) < 0)
            dyn_sink_write_failed(s, STDERR_FILENO);
        return;
    }
    if (!s->buf) {
        if (dyn_write_all(s->fd, data, n) < 0)
            dyn_sink_write_failed(s, s->fd);
        s->written += n;
        return;
    }
    if (s->bufn + n > s->bufcap)
        dyn_sink_flush(s);
    if (n > s->bufcap) {
        if (dyn_write_all(s->fd, data, n) < 0)
            dyn_sink_write_failed(s, s->fd);
        s->written += n;
        return;
    }
    memcpy(s->buf + s->bufn, data, n);
    s->bufn += n;
    s->written += n;
    if (s->bufn == s->bufcap)
        dyn_sink_flush(s);
}

static void dyn_sink_free(dyn_log_sink_t* s)
{
    dyn_log_sink_t** pp;
    for (pp = &dyn_sinks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    dyn_sink_flush(s);
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s->path);
    free(s->base);
    free(s->ext);
    free(s->active);
    free(s);
}

static void dyn_sink_unref(dyn_log_sink_t* s)
{
    if (!s)
        return;
    if (--s->refs > 0)
        return;
    dyn_sink_free(s);
}

static void dyn_sinks_atexit(void)
{
    dyn_log_sink_t* s;
    for (s = dyn_sinks; s; s = s->next)
        dyn_sink_flush(s);
}

static int dyn_parse_size(JSContext* ctx, JSValueConst v, uint64_t* out)
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

static int dyn_parse_frequency(JSContext* ctx, JSValueConst v, int64_t* out,
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

static dyn_log_sink_t* dyn_sink_new(JSContext* ctx, const char* path,
    uint64_t max_size, int64_t freq_ms,
    int named_date, int keep_count,
    int use_symlink, size_t bufcap, int do_mkdir)
{
    dyn_log_sink_t *s, *found;
    int rotation;
    int64_t now;

    for (found = dyn_sinks; found; found = found->next) {
        if (strcmp(found->path, path) == 0)
            break;
    }
    if (found) {
        const char* which = NULL;
        if (found->max_size != max_size)
            which = "rollover.size";
        else if (found->freq_ms != freq_ms)
            which = "rollover.frequency";
        else if (found->named_date != named_date)
            which = "rollover.frequency";
        else if (found->keep_count != keep_count)
            which = "rollover.count";
        else if (found->use_symlink != use_symlink)
            which = "rollover.symlink";
        else if (found->bufcap != bufcap)
            which = "buffer";
        if (which) {
            JS_ThrowRangeError(ctx,
                "Logger: dest is already open with a different %s", which);
            return NULL;
        }
        found->refs++;
        return found;
    }

    s = (dyn_log_sink_t*)calloc(1, sizeof *s);
    if (!s) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    s->path = strdup(path);
    if (!s->path) {
        free(s);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    s->max_size = max_size;
    s->freq_ms = freq_ms;
    s->named_date = named_date;
    s->numbered = max_size != 0 || (freq_ms != 0 && !named_date);
    s->keep_count = keep_count;
    s->use_symlink = use_symlink;
    s->fd = -1;
    rotation = max_size != 0 || freq_ms != 0;

    if (do_mkdir) {
        char dir[DYN_LOG_PATH_MAX];
        dyn_split_dir(path, dir, sizeof dir);
        if (dir[0] && dyn_mkdir_p(dir) != 0) {
            JS_ThrowRangeError(ctx, "Logger: cannot mkdir %s: %s", dir,
                strerror(errno));
            free(s->path);
            free(s);
            return NULL;
        }
    }

    now = dyn_now_ms();
    if (rotation) {
        const char* slash = strrchr(path, '/');
        const char* dot = strrchr(slash ? slash : path, '.');
        const char* bn0 = slash ? slash + 1 : path;
        size_t bl;
        if (dot && dot > bn0) {
            bl = (size_t)(dot - path);
            s->ext = strdup(dot);
        } else {
            bl = strlen(path);
            s->ext = strdup(".log");
        }
        s->base = (char*)malloc(bl + 1);
        if (!s->base || !s->ext) {
            int oom = !s->base || !s->ext;
            free(s->base);
            free(s->ext);
            free(s->path);
            free(s);
            if (oom)
                JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        memcpy(s->base, path, bl);
        s->base[bl] = '\0';
        if (freq_ms)
            s->period_ms = dyn_period_of(freq_ms, now);
        if (s->numbered) {
            char p0[DYN_LOG_PATH_MAX];
            int fd = dyn_sink_open_numbered(s, p0, sizeof p0);
            if (fd < 0) {
                JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", p0,
                    strerror(errno));
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->fd = fd;
            s->active = strdup(p0);
        } else {
            char p0[DYN_LOG_PATH_MAX];
            if (dyn_sink_name(s, 0, p0, sizeof p0) < 0) {
                JS_ThrowRangeError(ctx, "Logger: dest path too long");
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->fd = open(p0, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (s->fd < 0) {
                JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", p0,
                    strerror(errno));
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->active = strdup(p0);
        }
        if (!s->active) {
            close(s->fd);
            s->fd = -1;
            free(s->base);
            free(s->ext);
            free(s->path);
            free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
    } else {
        s->base = strdup(path);
        s->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (s->fd < 0) {
            JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", path,
                strerror(errno));
            free(s->base);
            free(s->path);
            free(s);
            return NULL;
        }
        s->active = strdup(path);
        if (!s->active) {
            close(s->fd);
            s->fd = -1;
            free(s->base);
            free(s->path);
            free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
    }
    {
        struct stat st;
        if (fstat(s->fd, &st) == 0)
            s->written = (uint64_t)st.st_size;
    }
    if (rotation) {
        if (s->use_symlink && s->active) {
            const char* abn = strrchr(s->active, '/');
            char tmp[DYN_LOG_PATH_MAX];
            abn = abn ? abn + 1 : s->active;
            if (snprintf(tmp, sizeof tmp, "%s.lnk", s->path) < (int)sizeof tmp) {
                unlink(tmp);
                if (symlink(abn, tmp) == 0)
                    rename(tmp, s->path);
            }
        }
        dyn_sink_prune(s);
    }
    if (bufcap) {
        s->buf = (char*)malloc(bufcap);
        if (!s->buf) {
            dyn_sink_free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        s->bufcap = bufcap;
    }
    s->refs = 1;
    s->next = dyn_sinks;
    dyn_sinks = s;
    {
        static int atexit_done;
        if (!atexit_done) {
            atexit_done = 1;
            atexit(dyn_sinks_atexit);
        }
    }
    return s;
}

static void dyn_text_value(JSContext* ctx, dyn_line_t* b, JSValueConst v);

static void dyn_text_fields(JSContext* ctx, dyn_line_t* b, dyn_logger_t* L,
    JSValueConst v, int reserve_err)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return;
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    for (i = 0; i < p.len && !b->truncated; i++) {
        JSValue pv;
        const char* ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(pv)) {
            JS_FreeValue(ctx, pv);
            continue;
        }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, reserve_err)) {
                dyn_line_esc_ctrl(b, ks, kn);
                dyn_line_put(b, "=", 1);
                dyn_text_value(ctx, b, pv);
                dyn_line_put(b, " ", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
}

static void dyn_emit_text(JSContext* ctx, dyn_logger_t* L, dyn_line_t* b,
    int64_t now, int magic, int has_err,
    JSValueConst err_v, int has_fields,
    JSValueConst fields_v, const char* msg, size_t msg_n)
{
    if (L->ts_mode == DYN_TS_EPOCH) {
        dyn_line_i64(b, now);
        dyn_line_put(b, " ", 1);
    } else if (L->ts_mode == DYN_TS_ISO) {
        int m;
        const char* iso = dyn_iso_sec(L, now, &m);
        char m3[3] = { (char)('0' + m / 100),
            (char)('0' + (m / 10) % 10),
            (char)('0' + m % 10) };
        dyn_line_put(b, iso, 19);
        dyn_line_put(b, ".", 1);
        dyn_line_put(b, m3, 3);
        dyn_line_put(b, "Z ", 2);
    }
    dyn_line_put(b, DYN_LV[magic].padded, 5);
    dyn_line_put(b, " ", 1);
    if (L->name) {
        dyn_line_puts(b, L->name);
        dyn_line_puts(b, ": ");
    }
    dyn_line_esc_ctrl(b, msg ? msg : "", msg ? msg_n : 0);
    if (has_err) {
        const char* cs;
        JSValue f = JS_GetPropertyStr(ctx, err_v, "name");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_put(b, " ", 1);
        dyn_line_esc_ctrl(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
        if (cs)
            JS_FreeCString(ctx, cs);
        f = JS_GetPropertyStr(ctx, err_v, "message");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_puts(b, ": ");
        dyn_line_esc_ctrl(b, cs ? cs : "", cs ? strlen(cs) : 0);
        if (cs)
            JS_FreeCString(ctx, cs);
    }
    if (L->base_text || has_fields)
        dyn_line_put(b, " ", 1);
    if (L->base_text)
        dyn_line_put(b, L->base_text, L->base_text_len);
    if (has_fields)
        dyn_text_fields(ctx, b, L, fields_v, has_err);
    if (b->truncated) {
        dyn_line_force(b, "...", 3);
        dyn_line_force(b, "\n", 1);
    } else if (!b->n || b->p[b->n - 1] == ' ') {
        if (b->n)
            b->p[b->n - 1] = '\n';
        else
            dyn_line_put(b, "\n", 1);
    } else {
        dyn_line_force(b, "\n", 1);
    }
}

static dyn_logger_t* dyn_logger_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_logger_t*)dyn_plain_get(ctx, v, dyn_logger_class_id);
}

static JSValue dyn_log_emit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    dyn_line_t b;
    int has_err = 0, has_fields = 0, msg_idx = 0;
    const char* msg = NULL;
    const char* msg_owned = NULL;
    size_t msg_n = 0;
    int64_t now;

    if (!L)
        return JS_EXCEPTION;
    if (magic < L->level)
        return JS_UNDEFINED;
    if (L->sample_n > 1 && ++L->emit_count % L->sample_n != 0)
        return JS_UNDEFINED;
    now = dyn_now_ms();

    dyn_line_init(&b);
    if (argc > 0 && JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0])) {
        if (JS_IsError(ctx, argv[0])) {
            has_err = 1;
            msg_idx = 1;
            if (argc > 1 && JS_IsObject(argv[1]) && !JS_IsFunction(ctx, argv[1])) {
                has_fields = 1;
                msg_idx = 2;
            }
        } else {
            has_fields = 1;
            msg_idx = 1;
        }
    }
    if (argc > msg_idx && JS_IsString(argv[msg_idx])) {
        msg = dyn_str_bytes(ctx, &msg_owned, &msg_n, argv[msg_idx]);
    }

    if (L->fmt == DYN_FMT_TEXT) {
        dyn_emit_text(ctx, L, &b, now, magic, has_err,
            has_err ? argv[0] : JS_UNDEFINED,
            has_fields, has_fields ? argv[has_err ? 1 : 0] : JS_UNDEFINED,
            msg, msg_n);
    } else {
        dyn_line_put(&b, "{", 1);
        dyn_line_time(&b, L, now);
        dyn_line_puts(&b, "\"level\":\"");
        dyn_line_puts(&b, DYN_LV[magic].name);
        dyn_line_puts(&b, "\",");
        if (L->name) {
            dyn_line_puts(&b, "\"name\":");
            dyn_line_json_str(&b, L->name, strlen(L->name));
            dyn_line_put(&b, ",", 1);
        }
        if (L->base)
            dyn_line_put(&b, L->base, L->base_len);
        if (has_err)
            dyn_line_error(ctx, &b, argv[0]);
        if (has_fields) {
            if (dyn_line_fields(ctx, &b, L, argv[has_err ? 1 : 0], has_err) < 0)
                goto fail;
        }
        dyn_line_puts(&b, "\"msg\":");
        if (msg) {
            dyn_line_json_str(&b, msg, msg_n);
        } else if (argc > msg_idx && !JS_IsUndefined(argv[msg_idx])) {

            dyn_seen_t seen;
            seen.n = 0;
            dyn_line_value(ctx, &b, argv[msg_idx], 0, &seen);
        } else {
            dyn_line_json_str(&b, "", 0);
        }
        if (!b.truncated) {
            dyn_line_puts(&b, "}\n");
        }
        if (b.truncated)
            dyn_line_json_repair(&b);
    }

    if (!JS_IsUndefined(L->dest_fn)) {
        JSValue line = JS_NewStringLen(ctx, b.p, b.n);
        JSValue res;
        if (JS_IsException(line))
            goto fail;
        res = JS_Call(ctx, L->dest_fn, JS_UNDEFINED, 1, &line);
        JS_FreeValue(ctx, line);
        if (JS_IsException(res))
            goto fail;
        JS_FreeValue(ctx, res);
    } else {
        dyn_sink_write(L->sink, now, b.p, b.n);
    }
    if (msg_owned)
        JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_UNDEFINED;
fail:
    if (msg_owned)
        JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_EXCEPTION;
}

static void dyn_text_value(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    if (JS_IsString(v)) {
        size_t n;
        const char* cs = NULL;
        const char* np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) {
            dyn_line_puts(b, "null");
            return;
        }
        dyn_line_esc_ctrl(b, np, n);
        if (cs)
            JS_FreeCString(ctx, cs);
        return;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        dyn_line_puts(b, "null");
        return;
    }
    if (JS_IsBool(v)) {
        dyn_line_puts(b, JS_ToBool(ctx, v) ? "true" : "false");
        return;
    }
    if (JS_IsNumber(v)) {
        dyn_line_number(ctx, b, v);
        return;
    }
    {
        dyn_line_t s2;
        dyn_seen_t seen;
        dyn_line_init(&s2);
        seen.n = 0;
        dyn_line_value(ctx, &s2, v, 0, &seen);
        dyn_line_put(b, s2.p, s2.n);
        dyn_line_free(&s2);
    }
}

static int dyn_base_build(JSContext* ctx, JSValueConst v, dyn_logger_t* L,
    dyn_line_t* jb, dyn_line_t* tb)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    for (i = 0; i < p.len && !jb->truncated; i++) {
        JSValue pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        const char* ks;
        size_t kn;
        int owned;
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(pv)) {
            JS_FreeValue(ctx, pv);
            continue;
        }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, 0)) {
                dyn_line_json_str(jb, ks, kn);
                dyn_line_put(jb, ":", 1);
                dyn_line_value(ctx, jb, pv, 1, &seen);
                dyn_line_put(jb, ",", 1);
                dyn_line_esc_ctrl(tb, ks, kn);
                dyn_line_put(tb, "=", 1);
                dyn_text_value(ctx, tb, pv);
                dyn_line_put(tb, " ", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

static int dyn_opts_check(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const dyn_logger_ctor_keys[] = {
    "level",
    "name",
    "timestamp",
    "format",
    "pid",
    "hostname",
    "base",
    "mkdir",
    "buffer",
    "sample",
    "rollover",
    "dest",
};
static const char* const dyn_logger_rollover_keys[] = {
    "size",
    "frequency",
    "count",
    "symlink",
};
static const char* const dyn_logger_child_keys[] = { "level" };

static int dyn_logger_opts(JSContext* ctx, JSValueConst opts, dyn_logger_t* L)
{
    JSValue v;
    const char* s;
    dyn_line_t jb, tb;

    L->level = DYN_LV_INFO;
    L->ts_mode = DYN_TS_EPOCH;
    L->fmt = DYN_FMT_JSON;
    L->name = NULL;
    L->base = NULL;
    L->base_len = 0;
    L->base_text = NULL;
    L->base_text_len = 0;
    L->pid_on = 0;
    L->host_on = 0;
    L->cached_sec = -1;
    L->cached_iso[0] = 0;
    if (!JS_IsObject(opts))
        return 0;
    if (dyn_opts_check(ctx, opts, dyn_logger_ctor_keys,
            countof(dyn_logger_ctor_keys)))
        return -1;

    v = JS_GetPropertyStr(ctx, opts, "level");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
    } else {
        int lv;
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        lv = dyn_level_of(s);
        JS_FreeCString(ctx, s);
        if (lv < 0) {
            JS_ThrowRangeError(ctx, "new Logger({ level }): unknown level");
            return -1;
        }
        L->level = lv;
    }

    v = JS_GetPropertyStr(ctx, opts, "name");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            L->name = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "timestamp");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            if (strcmp(s, "iso") == 0)
                L->ts_mode = DYN_TS_ISO;
            else if (strcmp(s, "epoch") == 0 || strcmp(s, "epochMs") == 0)
                L->ts_mode = DYN_TS_EPOCH;
            else {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx,
                    "new Logger({ timestamp }): unknown timestamp");
                return -1;
            }
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsBool(v) && !JS_ToBool(ctx, v)) {
        L->ts_mode = DYN_TS_NONE;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "format");
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            if (strcmp(s, "text") == 0)
                L->fmt = DYN_FMT_TEXT;
            else if (strcmp(s, "json") != 0) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "new Logger({ format }): unknown format");
                return -1;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "pid");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        L->pid_on = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "hostname");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        L->host_on = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    dyn_line_init(&jb);
    dyn_line_init(&tb);
    if (L->pid_on) {
        dyn_line_puts(&jb, "\"pid\":");
        dyn_line_i64(&jb, (int64_t)getpid());
        dyn_line_put(&jb, ",", 1);
        dyn_line_puts(&tb, "pid=");
        dyn_line_i64(&tb, (int64_t)getpid());
        dyn_line_put(&tb, " ", 1);
    }
    if (L->host_on) {
        const char* h = dyn_hostname();
        dyn_line_puts(&jb, "\"hostname\":");
        dyn_line_json_str(&jb, h, strlen(h));
        dyn_line_put(&jb, ",", 1);
        dyn_line_puts(&tb, "hostname=");
        dyn_line_esc_ctrl(&tb, h, strlen(h));
        dyn_line_put(&tb, " ", 1);
    }
    v = JS_GetPropertyStr(ctx, opts, "base");
    if (dyn_base_build(ctx, v, L, &jb, &tb) < 0) {
        JS_FreeValue(ctx, v);
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (jb.n) {
        L->base = (char*)malloc(jb.n);
        if (!L->base) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(L->base, jb.p, jb.n);
        L->base_len = jb.n;
    }
    if (tb.n) {
        L->base_text = (char*)malloc(tb.n);
        if (!L->base_text) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(L->base_text, tb.p, tb.n);
        L->base_text_len = tb.n;
    }
    dyn_line_free(&jb);
    dyn_line_free(&tb);

    {
        char* dest = NULL;
        uint64_t max_size = 0;
        int64_t freq_ms = 0;
        int named_date = 0, keep_count = 0, use_symlink = 0, do_mkdir = 0;
        int has_rollover = 0;
        size_t bufcap = 0;

        v = JS_GetPropertyStr(ctx, opts, "mkdir");
        if (!JS_IsUndefined(v) && !JS_IsNull(v))
            do_mkdir = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "buffer");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (JS_IsBool(v)) {
            if (JS_ToBool(ctx, v))
                bufcap = 64u * 1024u;
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (d != 0 && (!(d >= 1) || d > 1048576.0)) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "Logger buffer: use true, 0 or 1..1048576 bytes");
                return -1;
            }
            bufcap = (size_t)d;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "sample");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!JS_IsNumber(v) || d != (double)(int64_t)d || d < 1 || d > 4294967296.0) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx,
                    "new Logger({ sample }): sample must be an integer 1..2^32");
                return -1;
            }
            L->sample_n = (int64_t)d;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "rollover");
        if (JS_IsObject(v)) {
            JSValue r;
            if (dyn_opts_check(ctx, v, dyn_logger_rollover_keys,
                    countof(dyn_logger_rollover_keys))) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            r = JS_GetPropertyStr(ctx, v, "size");
            if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                if (dyn_parse_size(ctx, r, &max_size) < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "frequency");
            if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                if (dyn_parse_frequency(ctx, r, &freq_ms, &named_date) < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "count");
            if (JS_IsNumber(r)) {
                int32_t c;
                if (JS_ToInt32(ctx, &c, r) || c < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    JS_ThrowRangeError(ctx, "Logger rollover.count: use 0..2^31-1");
                    return -1;
                }
                keep_count = c;
            } else if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "Logger rollover.count: must be a number");
                return -1;
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "symlink");
            if (!JS_IsUndefined(r) && !JS_IsNull(r))
                use_symlink = JS_ToBool(ctx, r);
            JS_FreeValue(ctx, r);
            has_rollover = 1;
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "new Logger({ rollover }): must be an object");
            return -1;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "dest");
        if (JS_IsString(v)) {
            const char* cs = JS_ToCString(ctx, v);
            if (cs) {
                dest = strdup(cs);
                JS_FreeCString(ctx, cs);
            }
            if (!dest) {
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        } else if (JS_IsFunction(ctx, v)) {
            if (has_rollover) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx,
                    "new Logger({ rollover }): rollover needs a dest path");
                return -1;
            }
            L->dest_fn = JS_DupValue(ctx, v);
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                "new Logger({ dest }): dest must be a string path or a function");
            return -1;
        }
        JS_FreeValue(ctx, v);

        if (has_rollover && !dest) {
            JS_ThrowTypeError(ctx,
                "new Logger({ rollover }): rollover needs a dest path");
            return -1;
        }
        if (dest) {
            L->sink = dyn_sink_new(ctx, dest, max_size, freq_ms, named_date,
                keep_count, use_symlink, bufcap, do_mkdir);
            free(dest);
            if (!L->sink)
                return -1;
        }
    }
    return 0;
}

static JSValue dyn_logger_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = (dyn_logger_t*)calloc(1, sizeof *L);
    if (!L)
        return JS_ThrowOutOfMemory(ctx);
    L->dest_fn = JS_UNDEFINED;
    L->sample_n = 1;
    if (dyn_logger_opts(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, L) < 0) {
        dyn_logger_free_rt(JS_GetRuntime(ctx), L);
        return JS_EXCEPTION;
    }
    return dyn_plain_wrap(ctx, new_target, dyn_logger_class_id, L, dyn_logger_free);
}

static JSValue dyn_logger_child(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val), *C;
    dyn_line_t jb, tb;
    int level;

    if (!L)
        return JS_EXCEPTION;
    level = L->level;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue v;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx,
                "Logger.child: options must be an object");
        if (dyn_opts_check(ctx, argv[1], dyn_logger_child_keys,
                countof(dyn_logger_child_keys)))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            if (!s) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            level = dyn_level_of(s);
            JS_FreeCString(ctx, s);
            if (level < 0) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "Logger.child({ level }): unknown level");
            }
        } else if (!JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "Logger.child({ level }): level must be a string");
        }
        JS_FreeValue(ctx, v);
    }
    dyn_line_init(&jb);
    dyn_line_init(&tb);
    if (L->base)
        dyn_line_put(&jb, L->base, L->base_len);
    if (L->base_text)
        dyn_line_put(&tb, L->base_text, L->base_text_len);
    if (argc > 0 && dyn_base_build(ctx, argv[0], L, &jb, &tb) < 0) {
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return JS_EXCEPTION;
    }
    C = (dyn_logger_t*)calloc(1, sizeof *C);
    if (!C) {
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return JS_ThrowOutOfMemory(ctx);
    }
    C->level = level;
    C->ts_mode = L->ts_mode;
    C->fmt = L->fmt;
    C->pid_on = L->pid_on;
    C->host_on = L->host_on;
    C->cached_sec = -1;
    C->name = L->name ? strdup(L->name) : NULL;
    C->sink = L->sink;
    if (C->sink)
        C->sink->refs++;
    C->dest_fn = JS_IsUndefined(L->dest_fn) ? JS_UNDEFINED
                                            : JS_DupValue(ctx, L->dest_fn);
    C->sample_n = L->sample_n;
    C->emit_count = 0;
    if (jb.n) {
        C->base = (char*)malloc(jb.n);
        if (!C->base) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free_rt(JS_GetRuntime(ctx), C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base, jb.p, jb.n);
        C->base_len = jb.n;
    }
    if (tb.n) {
        C->base_text = (char*)malloc(tb.n);
        if (!C->base_text) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free_rt(JS_GetRuntime(ctx), C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base_text, tb.p, tb.n);
        C->base_text_len = tb.n;
    }
    dyn_line_free(&jb);
    dyn_line_free(&tb);
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_logger_class_id, C, dyn_logger_free);
}

static JSValue dyn_logger_enabled(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    const char* s;
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

static JSValue dyn_logger_get_level(JSContext* ctx, JSValueConst this_val)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewString(ctx, DYN_LV_NAME[L->level]);
}

static JSValue dyn_logger_set_level(JSContext* ctx, JSValueConst this_val,
    JSValueConst val)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    const char* s;
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

static JSValue dyn_logger_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!L)
        return JS_EXCEPTION;
    if (L->sink)
        dyn_sink_flush(L->sink);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_logger_proto[] = {
    JS_CFUNC_MAGIC_DEF("trace", 1, dyn_log_emit, DYN_LV_TRACE),
    JS_CFUNC_MAGIC_DEF("debug", 1, dyn_log_emit, DYN_LV_DEBUG),
    JS_CFUNC_MAGIC_DEF("info", 1, dyn_log_emit, DYN_LV_INFO),
    JS_CFUNC_MAGIC_DEF("warn", 1, dyn_log_emit, DYN_LV_WARN),
    JS_CFUNC_MAGIC_DEF("error", 1, dyn_log_emit, DYN_LV_ERROR),
    JS_CFUNC_MAGIC_DEF("fatal", 1, dyn_log_emit, DYN_LV_FATAL),
    JS_CFUNC_DEF("child", 2, dyn_logger_child),
    JS_CFUNC_DEF("enabled", 1, dyn_logger_enabled),
    JS_CFUNC_DEF("flush", 0, dyn_logger_flush),
    JS_CGETSET_DEF("level", dyn_logger_get_level, dyn_logger_set_level),
};

static JSValue dyn_debug_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    const char* ns;
    if (!magic)
        return JS_UNDEFINED;
    ns = JS_ToCString(ctx, data[0]);
    if (!ns)
        return JS_EXCEPTION;
    {
        dyn_line_t b;
        int i;
        dyn_line_init(&b);
        dyn_line_esc_ctrl(&b, ns, strlen(ns));
        for (i = 0; i < argc; i++) {
            size_t n;
            const char* s = JS_ToCStringLen(ctx, &n, argv[i]);
            dyn_line_put(&b, " ", 1);
            if (s) {
                dyn_line_esc_ctrl(&b, s, n);
                JS_FreeCString(ctx, s);
            }
        }
        dyn_line_force(&b, "\n", 1);
        (void)dyn_write_all(STDERR_FILENO, b.p, b.n);
        dyn_line_free(&b);
    }
    JS_FreeCString(ctx, ns);
    return JS_UNDEFINED;
}

static int dyn_debug_match(const char* env, const char* ns)
{
    size_t nsn = strlen(ns);
    int on = 0;
    const char* pat = env;
    while (*pat) {
        const char* e = pat;
        const char* p;
        size_t len;
        int neg = 0, m;
        const char* star;
        while (*e && *e != ',')
            e++;
        len = (size_t)(e - pat);
        p = pat;
        if (len && *p == '-') {
            neg = 1;
            p++;
            len--;
        }
        while (len && *p == ' ') {
            p++;
            len--;
        }
        while (len && p[len - 1] == ' ')
            len--;
        if (len) {
            star = memchr(p, '*', len);
            if (star) {
                size_t pre = (size_t)(star - p);
                size_t suf = len - pre - 1;
                m = nsn >= pre + suf && memcmp(p, ns, pre) == 0 && (suf == 0 || memcmp(p + pre + 1, ns + nsn - suf, suf) == 0);
            } else {
                m = nsn == len && memcmp(p, ns, len) == 0;
            }
            if (m)
                on = !neg;
        }
        if (!*e)
            break;
        pat = e + 1;
    }
    return on;
}

static JSValue dyn_debug(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
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

static const JSCFunctionListEntry dyn_log_funcs[] = {
    JS_CFUNC_DEF("Debug", 1, dyn_debug),
};

static int dyn_log_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_logger_class_id,
            &dyn_logger_class, dyn_logger_proto,
            countof(dyn_logger_proto), dyn_logger_ctor,
            "Logger")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

int js_nat_init_log(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:log", dyn_log_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Logger");
    return JS_AddModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

#endif
