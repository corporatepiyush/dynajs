/* dyna:log -- leveled structured logging. The Logger is a compiled capability:
   the level, name, base fields and format are parsed once and the per-call path
   only appends. Full API: see the dyna:* module in dyna-libc.h.

   v2: the emit path was rebuilt around measured hotspots:
     - Numbers: floats were going through JS_NewFloat64 + JS_ToCString (two
       allocations per float field). They now go through js_dtoa directly into a
       stack buffer -- the same formatter the engine's own JSON serializer uses
       (src/builtins/json.inc.c), byte-identical output, zero allocations.
       Integers use i64toa, the engine's pair-digit divider.
     - Strings: flat narrow JSStrings are read zero-copy (JS_GetNarrowStringBytes)
       instead of JS_ToCStringLen's copy; ropes/wide strings fall back.
     - Keys: JS_AtomBorrowASCII reads pure-ASCII key bytes without an allocation
       (the common case); non-ASCII keys fall back to JS_AtomToCStringLen.
     - Object walks: JS_GetOwnFastProps fills a stack buffer, one shape pass, no
       sort, no malloc, for the plain-object case that real field objects are;
       JS_GetOwnPropertyNames remains the fallback for integer keys/exotics.
   The observable behavior is unchanged: same bytes out for every shape, same
   truncation contract, same rollover/retention/symlink semantics. */
#include "dyna-nat.h"
#include "dyna-simd-kernels.h"   /* simd.find_bitmap for the escape scan */
#include "dtoa.h"                /* js_dtoa/i64toa: the engine's own number formatter */

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
#include <math.h>           /* isfinite for the JSON number guard */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A log line is not a data dump. An unbounded one is how a log statement takes
   down a service, so the line is truncated rather than allowed to grow.
   The reserve is what makes the CUT safe, and it is the part that is easy to
   get wrong: a line cut at exactly the cap stopped mid-string with no closing
   quote, no closing brace and no newline -- emitting malformed JSON that
   corrupted the NEXT line's framing too, since the consumer could not find
   this one's end. The tail therefore keeps room for `"…N` (an ellipsis inside
   the string, the close quote, the close brace, the newline) and the emit
   path writes them. */
#define DYN_LOG_MAX_LINE (64u * 1024u)
#define DYN_LOG_TAIL_RESERVE 16

enum { DYN_LV_TRACE, DYN_LV_DEBUG, DYN_LV_INFO, DYN_LV_WARN, DYN_LV_ERROR,
       DYN_LV_FATAL, DYN_LV_SILENT };

static const char *const DYN_LV_NAME[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "silent"
};

/* Level metadata precomputed ONCE, so the emit path never calls strlen or a
   pad loop. `padded` is the name left-aligned to five columns exactly as the
   text format prints it (info -> "info "); len is the name's length. */
static const struct {
    const char *name;        /* == DYN_LV_NAME[i], for callers that need it */
    size_t len;              /* strlen(name) */
    char padded[7];          /* name + spaces to five columns + NUL */
} DYN_LV[countof(DYN_LV_NAME)] = {
    { "trace", 5, "trace" },
    { "debug", 5, "debug" },
    { "info",  4, "info " },
    { "warn",  4, "warn " },
    { "error", 5, "error" },
    { "fatal", 5, "fatal" },
    { "silent",6, "silent" },
};

/* Timestamp rendering. `epochMs` and `iso` are the two shapes a log consumer
   actually parses; false omits the field entirely. */
enum { DYN_TS_EPOCH, DYN_TS_ISO, DYN_TS_NONE };

/* The two renderings a Logger can produce. "json" is the machine format the
 * consumers parse (the default, what the module shipped with); "text" is the
 * human scan of a dev terminal, pino-pretty's job -- built in rather than
 * bolted on because a human format that lives in a second package never
 * inherits the frame's guarantees (escaping, truncation, level gating). */
enum { DYN_FMT_JSON, DYN_FMT_TEXT };

/* One output destination, shared by every Logger that names the same path. */
typedef struct dyn_log_sink dyn_log_sink_t;

typedef struct {
    int level;
    int ts_mode;
    int fmt;                /* DYN_FMT_* */
    char *name;             /* may be NULL */
    char *base;             /* pre-serialized `"k":v,...` prefix, may be NULL */
    size_t base_len;
    char *base_text;        /* pre-serialized `k=v ` prefix for text, may be NULL */
    size_t base_text_len;
    dyn_log_sink_t *sink;   /* NULL => stderr */
    /* Enrichment flags. The pid/hostname values are NOT stored per line --
     * they are folded into the base prefix at construction; these flags only
     * tell the field filter which frame keys are taken. */
    int pid_on, host_on;
    /* The formatted second is cached: a service logging 100k lines/s renders
       the date once per second, not once per line. */
    int64_t cached_sec;
    char cached_iso[20];   /* "2026-07-31T13:33:30" + NUL */
} dyn_logger_t;

static JSClassID dyn_logger_class_id;

static void dyn_sink_unref(dyn_log_sink_t *s);

static void dyn_logger_free(void *p)
{
    dyn_logger_t *L = (dyn_logger_t *)p;
    dyn_sink_unref(L->sink);
    free(L->name);
    free(L->base);
    free(L->base_text);
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

/* Every emitted line used to start life as a malloc: a 60-byte "info" line
 * paid a heap round-trip, and the allocator saw one alloc/free per line at
 * 100k lines/s. The builder starts INLINE and grows to the heap only when a
 * line actually needs it. Measured a wash-ish (an emit is syscall-bound), but
 * it removes an allocation and keeps the no-heap promise for typical lines. */
#define DYN_LINE_INLINE 512

typedef struct {
    char *p;                /* write cursor base: inln[] until it overflows */
    size_t n, cap;
    int truncated;
    char inln[DYN_LINE_INLINE];
} dyn_line_t;

static void dyn_line_init(dyn_line_t *b)
{
    b->p = b->inln;
    b->n = 0;
    b->cap = DYN_LINE_INLINE;
    b->truncated = 0;
}

static void dyn_line_free(dyn_line_t *b)
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
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static void dyn_line_put(dyn_line_t *b, const char *s, size_t n)
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
        char *np;
        if (b->p == b->inln) {
            np = (char *)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char *)realloc(b->p, nc);
        }
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

/* Append past the truncation point -- ONLY for the few bytes that CLOSE a
   truncated line (the closing quote/brace and the newline), whose room
   DYN_LOG_TAIL_RESERVE guarantees. Everything else must go through
   dyn_line_put. */
static void dyn_line_force(dyn_line_t *b, const char *s, size_t n)
{
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char *np;
        if (b->p == b->inln) {
            np = (char *)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char *)realloc(b->p, nc);
        }
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

/* ONE table for both escaper modes -- a bitmap the SIMD scan takes as-is and
   the scalar loop tests directly, so the two cannot drift apart. It is the
   UNION of both modes' escape sets: a byte the active mode does not escape
   is copied raw in the switch. Members: 0x00-0x1F, '"', '\\', DEL (0x7F),
   and 0xE2 (the UTF-8 lead byte of U+2028/U+2029; the sequence is checked in
   full on the flagged-byte path). */
static const uint8_t DYN_LOG_ESC_BM[32] = {
    0xff, 0xff, 0xff, 0xff,   /* 0x00-0x1F: every control character */
    0x04, 0x00, 0x00, 0x00,   /* '"' */
    0x00, 0x00, 0x00, 0x10,   /* '\\' */
    0x00, 0x00, 0x00, 0x80,   /* DEL */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00    /* 0xE2: U+2028/U+2029 lead byte */
};
#define DYN_LOG_ESC(c) (DYN_LOG_ESC_BM[(c) >> 3] & (1u << ((c) & 7)))
/* Below this the indirect call outweighs the scan (the CSV-field lesson). */
#define DYN_LOG_SIMD_MIN 64

/* The two escaper modes. One function (dyn_line_escape below) serves both;
   they used to be two functions with two copies of the same control-char
   switch, and drift between copies is exactly the class of bug A-11 was. */
enum { DYN_ESC_CTRL, DYN_ESC_JSON };

/* The ONE escaper. Which bytes are escaped is the mode's decision:
     DYN_ESC_JSON -- a JSON string literal: controls, '"' and '\\', plus
       DEL (0x7F) and U+2028/U+2029. Those three are valid JSON but hostile
       to consumers that embed a log line in JS source: U+2028/U+2029
       terminate a line literal (pre-ES2019 parsers), and DEL slips past
       byte-level filters, so each is emitted as its \uXXXX escape.
     DYN_ESC_CTRL -- the line-forger defense: controls only, so a Debug()
       call or a text-format field cannot inject a newline and forge a log
       line. '"' and '\\' do not break line framing and DEL/LS/PS are not
       frame characters, so output without controls stays byte-identical. */
static void dyn_line_escape(dyn_line_t *b, const char *s, size_t n, int mode)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
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
        if (mode == DYN_ESC_JSON && c == 0xE2 && i + 2 < n &&
            (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0xA8 ||
             (unsigned char)s[i + 2] == 0xA9)) {
            dyn_line_put(b, (s[i + 2] == 0xA8) ? "\\u2028" : "\\u2029", 6);
            i += 3;
            continue;
        }
        switch (c) {
        case '\n': dyn_line_puts(b, "\\n");  break;
        case '\r': dyn_line_puts(b, "\\r");  break;
        case '\t': dyn_line_puts(b, "\\t");  break;
        case '\b': dyn_line_puts(b, "\\b");  break;
        case '\f': dyn_line_puts(b, "\\f");  break;
        case '"':  dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\"" : "\""); break;
        case '\\': dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\\" : "\\"); break;
        case 0x7F:
            if (mode == DYN_ESC_JSON)
                dyn_line_put(b, "\\u007f", 6);
            else
                dyn_line_put(b, (const char *)&c, 1);
            break;
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
        i++;
    }
}

/* A JSON string literal. The escape set is JSON's, so a message containing a
   quote or a newline cannot break the line framing a consumer relies on. */
static void dyn_line_json_str(dyn_line_t *b, const char *s, size_t n)
{
    dyn_line_put(b, "\"", 1);
    dyn_line_escape(b, s, n, DYN_ESC_JSON);
    dyn_line_put(b, "\"", 1);
}

/* String bytes from a JSValue. This is deliberately a THIN wrapper over
   JS_ToCStringLen: for a flat narrow string with no byte >= 0x80 it returns a
   pointer INTO the JSString (zero-copy), and for everything else it returns an
   owned UTF-8 conversion (ropes, wide strings, latin-1 high bytes). Either
   way the caller MUST JS_FreeCString the result -- JS_ToCStringLen dups the
   value before returning, so the free is balanced. Hand-rolling this with
   JS_GetNarrowStringBytes + a separate high-bit scan made every large message
   pay THREE passes over its bytes (measured 2508 ns vs 968 ns at 4 KiB);
   JS_ToCStringLen performs the same high-bit walk internally, so the escape
   scan in dyn_line_json_str is the only extra pass. */
static const char *dyn_str_bytes(JSContext *ctx, const char **ps, size_t *plen,
                                 JSValueConst v)
{
    const char *cs = JS_ToCStringLen(ctx, plen, v);
    *ps = cs;
    return cs;
}

/* Escape control characters (0x00-0x1F) so a Debug() namespace or message
   cannot inject a newline/CR and forge a log line or fake log frame. The
   escape set is the same control range the Logger's JSON escaper handles
   (dyn_line_json_str), but WITHOUT the surrounding quotes and without touching
   '"' or '\\': those do not break line framing, and leaving them untouched
   keeps the normal (control-free) output byte-identical to before. */
static void dyn_line_esc_ctrl(dyn_line_t *b, const char *s, size_t n)
{
    dyn_line_escape(b, s, n, DYN_ESC_CTRL);
}

/* Many call sites write an integer into the line. i64toa (engine, dtoa.c)
   does the same job with a pair-digit table; no JSString, no allocation. */
static void dyn_line_i64(dyn_line_t *b, int64_t v)
{
    char tmp[24];
    size_t n = i64toa(tmp, v);
    dyn_line_put(b, tmp, n);
}

/* The emit path's clock: one clock_gettime per EMITTED line (the gate above
 * it means suppressed lines pay nothing). */
static int64_t dyn_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The host name, fetched once per process: it cannot change while we run. */
static const char *dyn_hostname(void)
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

/* Render the timestamp, reusing the cached second when it has not changed: a
 * service logging 100k lines/s runs gmtime_r once per second, not per line.
 * Returns the 19-byte "YYYY-MM-DDTHH:MM:SS" cache and the millisecond part. */
static const char *dyn_iso_sec(dyn_logger_t *L, int64_t ms, int *millis_out)
{
    int64_t sec = ms / 1000;
    if (sec != L->cached_sec) {
        struct tm tmv;
        time_t t = (time_t)sec;
        gmtime_r(&t, &tmv);
        snprintf(L->cached_iso, sizeof L->cached_iso,
                 "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        L->cached_sec = sec;
    }
    *millis_out = (int)(ms % 1000);
    return L->cached_iso;
}

static void dyn_line_time(dyn_line_t *b, dyn_logger_t *L, int64_t ms)
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
        const char *iso = dyn_iso_sec(L, ms, &m);
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

/* A log line is not a data dump: past this depth the value is elided rather
   than walked, which also bounds the recursion on caller-supplied objects. */
#define DYN_LOG_MAX_DEPTH 8

/* The ancestor chain, for cycle detection. Depth is capped, so this is a fixed
   array and the scan is at most DYN_LOG_MAX_DEPTH pointer compares. */
typedef struct { const void *p[DYN_LOG_MAX_DEPTH]; int n; } dyn_seen_t;

/* ---- enumerable-key walk, fast by default (defined below) --------------- */
typedef struct {
    JSPropertyEnum *tab;
    uint32_t len;
    int heap;               /* 0 = stackbuf, 1 = malloc (fast, -2), 2 = propertynames */
    JSPropertyEnum stackbuf[32];
} dyn_props_t;
static int dyn_props_get(JSContext *ctx, JSValueConst v, dyn_props_t *p);
static void dyn_props_free(JSContext *ctx, dyn_props_t *p);
static const char *dyn_key_bytes(JSContext *ctx, size_t *plen, JSAtom atom,
                                 int *owned);

static void dyn_line_value(JSContext *ctx, dyn_line_t *b, JSValueConst v,
                           int depth, dyn_seen_t *seen);

static void dyn_line_number(JSContext *ctx, dyn_line_t *b, JSValueConst v)
{
    int64_t i64;
    double d;
    char dbuf[64];
    JSDTOATempMem dm;
    /* A plain integer is already its final decimal form: no double round trip,
       no range checks. The engine's i64toa prints exactly what the value is. */
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        dyn_line_i64(b, (int64_t)JS_VALUE_GET_INT(v));
        return;
    }
    /* Round-trip through double: true when the value IS the integer it
     * truncates to. int64 catches identifiers and big counts the old int32
     * check bounced to the slow engine-formatter path (5e9, 2^40). */
    if (!JS_ToInt64(ctx, &i64, v)) {
        double back;
        if (!JS_ToFloat64(ctx, &back, v) && back == (double)i64) {
            dyn_line_i64(b, i64);
            return;
        }
    }
    if (JS_ToFloat64(ctx, &d, v)) { dyn_line_puts(b, "null"); return; }
    /* JSON has no NaN or Infinity. `d > 1e308` is NOT the check -- Number.MAX_
       VALUE is 1.797e308, finite and renderable, and js_dtoa prints it exactly
       (the OLD code nulled it with this guard; that was a bug this rewrite
       fixes). Only non-finite values map to null. */
    if (!isfinite(d)) {
        dyn_line_puts(b, "null");
        return;
    }
    /* The engine's own exact decimal formatter -- the same one ToString uses
     * (src/builtins/json.inc.c does exactly this). No JSString, no alloc. */
    {
        int n = js_dtoa(dbuf, d, 10, 0, JS_DTOA_FORMAT_FREE, &dm);
        dyn_line_put(b, dbuf, (size_t)n);
    }
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
        const char *cs = NULL;
        const char *np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) { dyn_line_puts(b, "null"); return; }
        dyn_line_json_str(b, np, n);
        if (cs)
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
            const char *ks;
            size_t kn;
            int owned;
            if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
            if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
            ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
            if (ks) {
                if (wrote) dyn_line_put(b, ",", 1);
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, depth + 1, seen);
                wrote = 1;
                if (owned) JS_FreeCString(ctx, ks);
            }
            JS_FreeValue(ctx, pv);
        }
        dyn_line_put(b, "}", 1);
        dyn_props_free(ctx, &p);
        seen->n--;
    }
}

/* The frame's keys -- time, level, name, msg and friends -- are what a log
 * consumer parses to route and frame a line. A caller field named "level"
 * would DUPLICATE a frame key and the last one wins for most parsers, which
 * is a log-forgery primitive: log.info({level:"debug"}, "...") could make a
 * fatal line read as debug. So a caller key that collides with a key the
 * frame actually emits is dropped (silently: a logger never fails the line
 * it is emitting). Reservation is dynamic -- "time" is only taken when the
 * frame writes a time, "pid" only when pid enrichment is on -- so a base
 * field legitimately called pid stays available when the option is off. */
static int dyn_key_reserved(dyn_logger_t *L, const char *k, size_t n,
                            int reserve_err)
{
    /* One load and a range check reject the ~20 first letters that can never
     * name a frame key before any memcmp runs. This matters because the check
     * is per FIELD, and the A/B measured +8% on a 40-field line without it
     * (3168 -> 3429 ns): a per-field cost is not amortised by anything. */
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

/* ---- enumerable-key walk, fast by default ------------------------------- */

/* The own enumerable STRING keys of `v`, in insertion order, with one shape
   pass and no table malloc for the plain-object case that real field objects
   are (JS_GetOwnFastProps). Falls back to JS_GetOwnPropertyNames for integer
   keys/exotics -- where the set and its ordering are whatever the engine
   already produced, so the bytes are unchanged from the property-names path. */
#define DYN_FAST_KEYS 32

/* The own enumerable STRING keys of `v`, in insertion order, with one shape
   pass and no table malloc for the plain-object case that real field objects
   are (JS_GetOwnFastProps). Falls back to JS_GetOwnPropertyNames for integer
   keys/exotics -- where the set and its ordering are whatever the engine
   already produced, so the bytes are unchanged from the property-names path. */

/* Returns 0 on success, -1 on exception. */
static int dyn_props_get(JSContext *ctx, JSValueConst v, dyn_props_t *p)
{
    int rc = JS_GetOwnFastProps(ctx, v, p->stackbuf, DYN_FAST_KEYS, &p->len);
    if (rc == 0) {
        p->tab = p->stackbuf;
        p->heap = 0;
        return 0;
    }
    if (rc == -2) {                     /* wider than the stack buffer */
        p->tab = (JSPropertyEnum *)malloc(p->len * sizeof *p->tab);
        if (!p->tab) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (JS_GetOwnFastProps(ctx, v, p->tab, p->len, &p->len) == 0) {
            p->heap = 1;
            return 0;
        }
        free(p->tab);                   /* -1: the engine already freed the dups */
    }
    /* exotic object or an integer-keyed one: the engine's own walk. This is
       ALSO the byte-for-byte fallback: whatever its ordering, it is the one
       the emits produced before the fast path existed. */
    p->tab = NULL;
    p->heap = 2;
    if (JS_GetOwnPropertyNames(ctx, &p->tab, &p->len, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    return 0;
}

/* The frees are ASYMMETRIC: FastProps dup's atoms into the caller's buffer
   (free each, never JS_FreePropertyEnum -- that would js_free a stack or
   malloc buffer), while JS_GetOwnPropertyNames owns its table (free the
   table and its atoms through JS_FreePropertyEnum). */
static void dyn_props_free(JSContext *ctx, dyn_props_t *p)
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

/* Key bytes: borrowed when the atom is pure ASCII (the common case for log
   fields -- no allocation), otherwise an allocated UTF-8 conversion that the
   caller must JS_FreeCString. *owned says which. */
static const char *dyn_key_bytes(JSContext *ctx, size_t *plen, JSAtom atom,
                                 int *owned)
{
    const char *ks = JS_AtomBorrowASCII(ctx, plen, atom);
    *owned = 0;
    if (ks)
        return ks;
    ks = JS_AtomToCStringLen(ctx, plen, atom);
    *owned = ks != NULL;
    return ks;
}

/* The caller's fields join THIS line rather than nesting under one, so the
   members are emitted directly instead of serializing an object and splicing
   its braces off. */
static int dyn_line_fields(JSContext *ctx, dyn_line_t *b, dyn_logger_t *L,
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
        const char *ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, reserve_err)) {
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, 1, &seen);
                dyn_line_put(b, ",", 1);
            }
            if (owned) JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

/* An Error serializes to {type,message,stack,...extras}, which is what a log
   consumer expects; JSON.stringify of an Error is `{}` and loses everything.
   Extra enumerable props ride along (pino's error serializer): e.code =
   "ENOENT" lands inside err instead of vanishing. */
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
    {   /* everything else the error carries, after the fixed three */
        dyn_props_t p;
        uint32_t i;
        dyn_seen_t seen;
        seen.n = 0;
        if (dyn_props_get(ctx, v, &p) >= 0) {
            for (i = 0; i < p.len && !b->truncated; i++) {
                JSValue pv;
                const char *ks;
                size_t kn;
                int owned;
                int skip = 0;
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    skip = (kn == 4 && memcmp(ks, "name", 4) == 0) ||
                           (kn == 7 && memcmp(ks, "message", 7) == 0) ||
                           (kn == 5 && memcmp(ks, "stack", 5) == 0);
                    if (owned) JS_FreeCString(ctx, ks);
                }
                if (skip)
                    continue;
                pv = JS_GetProperty(ctx, v, p.tab[i].atom);
                if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
                if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    dyn_line_put(b, ",", 1);
                    dyn_line_json_str(b, ks, kn);
                    dyn_line_put(b, ":", 1);
                    seen.n = 0;
                    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
                    dyn_line_value(ctx, b, pv, 1, &seen);
                    if (owned) JS_FreeCString(ctx, ks);
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

/* ------------------------------------------------------------------ sinks */

#define DYN_LOG_PATH_MAX 4096

/* One output destination shared by every Logger that names the same path.
 * A sink owns an fd -- stderr (fd -1 stands in for it) or an open append-mode
 * file -- plus the state rollover needs (bytes written, the current period,
 * the active file's name). Two Loggers on one path share ONE sink: O_APPEND
 * makes two fds safe to interleave, but two INDEPENDENT rollovers would
 * trample each other's numbered names and retention counts. */
struct dyn_log_sink {
    struct dyn_log_sink *next;   /* process-wide registry chain */
    int refs;                    /* Loggers currently holding this sink */
    int fd;                      /* active fd; -1 => write to stderr */
    char *path;                  /* the requested dest path (registry key) */
    /* rollover configuration (immutable once opened) */
    uint64_t max_size;           /* rotate at this many bytes; 0 = off */
    int64_t freq_ms;             /* rotate at this interval; 0 = off */
    int named_date;              /* the active name carries the period's date */
    int numbered;                /* the active name carries a .N counter */
    int keep_count;              /* rotated files to keep; 0 = unlimited */
    int use_symlink;             /* keep `path` itself pointing at the active file */
    /* active-file state */
    int64_t period_ms;           /* epoch ms where the current period began */
    char *base;                  /* dest minus suffixes: base[.date][.N].ext */
    char *ext;                   /* ".log" or the dest's own extension */
    char *active;                /* current file's path; NULL => stderr */
    uint64_t written;            /* bytes written to the active file so far */
    /* Line batching (sonic-boom's minLength): lines accumulate until the
       buffer fills, then land in ONE write. Off by default -- sync writes are
       the crash-safe default -- and never applied to stderr, whose
       one-write-per-line atomicity is a contract with other fd-2 writers. */
    char *buf;
    size_t bufn, bufcap;
    int64_t last_retry;          /* ms of the last failed rotation reopen */
    uint64_t write_errors;       /* write(2) failures seen on this sink */
};

static dyn_log_sink_t *dyn_sinks;    /* every open file destination */

/* Write every byte, EINTR included. Returns 0 when every byte landed, -1
   when write(2) failed. A failed write must not kill the caller (the engine
   ignores SIGPIPE, so EPIPE on a dead stderr arrives here as -1 rather than
   as a signal), but the failure is no longer silently swallowed -- sink
   callers record it via dyn_sink_write_failed. errno still holds the failing
   write's error on return. */
static int dyn_write_all(int fd, const char *p, size_t n)
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

/* A write failure used to vanish entirely: dyn_write_all swallowed it and a
   failed flush still reset the batch buffer, so a full disk silently
   discarded exactly the lines an incident needs. Record it: the FIRST
   failure emits ONE warning on stderr -- and the counter keeps counting
   after, so the damage stays inspectable. The warning is suppressed when
   stderr itself is the destination that failed: it cannot report its own
   loss, and warning about it would only recurse onto the broken fd. */
static void dyn_sink_write_failed(dyn_log_sink_t *s, int fd)
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

/* mkdir -p for the destination's parent: walk the components, mkdir each,
   tolerate EEXIST (a later open() still fails honestly if a component exists
   as a non-directory). */
static int dyn_mkdir_p(const char *path)
{
    char tmp[DYN_LOG_PATH_MAX];
    size_t n = strlen(path);
    char *p;
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

/* Split `path` into the directory part (into `dir`) and the basename
   (returned; a pointer INTO path, nothing to free). */
static const char *dyn_split_dir(const char *path, char *dir, size_t cap)
{
    const char *slash = strrchr(path, '/');
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

/* The period bucket `now` falls in: floor to a multiple of the interval.
   Day and hour boundaries are UTC -- a log file's date segment should not
   depend on the host's timezone drift. */
static int64_t dyn_period_of(int64_t freq_ms, int64_t now)
{
    return now - now % freq_ms;
}

/* "YYYY-MM-DD" (daily) or "YYYY-MM-DDTHH" (hourly) from a bucket start. */
static void dyn_fmt_date(int64_t period_ms, int hourly, char *out, size_t cap)
{
    time_t t = (time_t)(period_ms / 1000);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    if (hourly)
        snprintf(out, cap, "%04d-%02d-%02dT%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
    else
        snprintf(out, cap, "%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

/* The active file's name: base[.date][.N]ext, the extension-last shape
   pino-roll settled on -- `app.2026-08-28.1.log` sorts chronologically by
   name alone, which is what `ls` and every retention scanner rely on. */
static int dyn_sink_name(const dyn_log_sink_t *s, unsigned n, char *out,
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

/* The highest N already used for this sink's CURRENT naming context (date
   included when names carry one). Numbering continues from here: retention
   deletes old files, and a first-free-slot scheme would then REUSE their
   numbers -- making "app.2" newer than "app.4" and breaking the invariant
   that a name sorts chronologically (pino-roll scans for the highest for
   exactly this reason). */
static unsigned dyn_sink_scan_max_n(const dyn_log_sink_t *s)
{
    char dir[DYN_LOG_PATH_MAX];
    char date[32];
    const char *bn = dyn_split_dir(s->base, dir, sizeof dir);
    size_t bnl = strlen(bn), dl = 0;
    unsigned maxn = 0;
    DIR *d = opendir(dir[0] ? dir : ".");
    struct dirent *de;

    if (s->named_date) {
        dyn_fmt_date(s->period_ms, s->freq_ms == 3600000, date, sizeof date);
        dl = strlen(date);
    }
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        const char *mid = de->d_name + bnl;
        unsigned v = 0;
        int digits = 0;
        if (strncmp(de->d_name, bn, bnl) != 0 || *mid != '.')
            continue;
        mid++;
        if (dl) {
            if (strncmp(mid, date, dl) != 0 || mid[dl] != '.')
                continue;               /* another period's numbers do not count */
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

/* Open the next NUMBERED file: continue from the highest existing number
   (O_EXCL still guards against a racing creator). Bounded -- a directory
   with 4095 same-period rotations has earned a fallback. */
static int dyn_sink_open_numbered(dyn_log_sink_t *s, char *out, size_t cap)
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

static void dyn_sink_flush(dyn_log_sink_t *s)
{
    if (s->buf && s->bufn) {
        int rc;
        if (s->fd >= 0) {
            rc = dyn_write_all(s->fd, s->buf, s->bufn);
        } else {
            /* the sink is on its stderr fallback: the buffered lines still
               GO OUT rather than vanishing. Dropping them would lose exactly
               the lines written just before a crash, which are the ones an
               on-call engineer needs most. */
            rc = dyn_write_all(STDERR_FILENO, s->buf, s->bufn);
        }
        /* the batch is dropped either way: a flush runs on the emit path, and
           an emit must never block or grow unbounded on a stuck destination */
        if (rc < 0)
            dyn_sink_write_failed(s, s->fd >= 0 ? s->fd : STDERR_FILENO);
        s->bufn = 0;
    }
}

/* Rotation: flush, close, open the next name, refresh the symlink, prune.
   Failure leaves the sink on stderr (fd -1) and retried once a second --
   a full disk must silence the file, not the process. */
static void dyn_sink_prune(dyn_log_sink_t *s);

static void dyn_sink_rotate(dyn_log_sink_t *s, int64_t now)
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
            s->last_retry = now;         /* arm the retry throttle here too */
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
    /* symlink refresh is best-effort grooming, never emit-critical */
    if (s->use_symlink && s->active) {
        const char *abn = strrchr(s->active, '/');
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

/* Retention: the rotated files of THIS base beyond keep_count go. readdir +
   a name-shape match, oldest first -- the extension-last name sorts
   chronologically, and the N counter sorts numerically so "10" lands after
   "2". A pure-.N sequence (no date) sorts by N with an empty date key.
   Runs after every rotation AND at open, so a restart inherits the
   retention policy without waiting for a rotation. */
static void dyn_sink_prune(dyn_log_sink_t *s)
{
    char dir[DYN_LOG_PATH_MAX];
    const char *bn;
    size_t bnl, extl;
    DIR *d;
    typedef struct { char *name; char date[32]; long num; } dyn_rot_t;
    dyn_rot_t *ents = NULL;
    int cnt = 0, cap = 256, i;
    struct dirent *de;
    const char *abn2 = s->active ? strrchr(s->active, '/') : NULL;

    if (!s->keep_count || !s->base)
        return;
    bn = dyn_split_dir(s->base, dir, sizeof dir);
    bnl = strlen(bn);
    extl = strlen(s->ext);
    abn2 = s->active ? (abn2 ? abn2 + 1 : s->active) : NULL;
    d = opendir(dir[0] ? dir : ".");
    if (d) {
        ents = (dyn_rot_t *)malloc((size_t)cap * sizeof *ents);
        while ((de = readdir(d)) != NULL) {
            const char *mid, *dot;
            size_t mlen, k;
            if (ents && cnt >= cap) {
                dyn_rot_t *ne;
                if (cap >= 4096)
                    continue;
                ne = (dyn_rot_t *)realloc(ents, (size_t)(cap * 2) * sizeof *ne);
                if (!ne)
                    continue;
                ents = ne;
                cap *= 2;
            }
            if (abn2 && strcmp(de->d_name, abn2) == 0)
                continue;                    /* the active file is not rotated */
            if (strncmp(de->d_name, bn, bnl) != 0 || de->d_name[bnl] != '.')
                continue;
            mlen = strlen(de->d_name);
            if (mlen < bnl + 1 + extl ||
                strcmp(de->d_name + mlen - extl, s->ext) != 0)
                continue;
            mid = de->d_name + bnl + 1;
            mlen -= bnl + 1 + extl;
            if (!mlen)
                continue;
            /* the middle must be pure rotation syntax: digits, '-', 'T',
               '.' -- anything else is somebody else's file */
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
                ents[cnt].num = atol(dot + 1);
            } else {
                ents[cnt].date[0] = '\0';
                ents[cnt].num = atol(mid);
            }
            cnt++;
        }
        closedir(d);
    }
    if (ents && cnt > s->keep_count) {
        int over = cnt - s->keep_count;
        /* insertion sort -- the count is small and this is a cold path */
        for (i = 1; i < cnt; i++) {
            dyn_rot_t key = ents[i];
            int j = i - 1;
            while (j >= 0 && (strcmp(ents[j].date, key.date) > 0 ||
                   (strcmp(ents[j].date, key.date) == 0 && ents[j].num > key.num))) {
                ents[j + 1] = ents[j];
                j--;
            }
            ents[j + 1] = key;
        }
        for (i = 0; i < over; i++) {
            char full[DYN_LOG_PATH_MAX];
            if (snprintf(full, sizeof full, "%s/%s",
                         dir[0] ? dir : ".", ents[i].name) < (int)sizeof full)
                unlink(full);
        }
    }
    if (ents) {
        for (i = 0; i < cnt; i++)
            free(ents[i].name);
        free(ents);
    }
}

/* The per-emit gate: rotation is lazy, checked before the line lands. A
   line never straddles two files -- the check runs first, so an oversized
   line OPENS the next file rather than being split. */
static void dyn_sink_maybe_rotate(dyn_log_sink_t *s, int64_t now)
{
    if (s->fd < 0) {
        /* a failed rotation: retry at most once a second, not once a line */
        if (now >= s->last_retry && now - s->last_retry >= 1000)
            dyn_sink_rotate(s, now);
        return;
    }
    if ((s->freq_ms && now >= s->period_ms + s->freq_ms) ||
        (s->max_size && s->written >= s->max_size))
        dyn_sink_rotate(s, now);
}

/* One line into the sink. This is the ONLY writer: stderr fallback, direct
   writes, and the batching buffer all meet here. */
static void dyn_sink_write(dyn_log_sink_t *s, int64_t now,
                           const char *data, size_t n)
{
    if (!s) {
        (void)dyn_write_all(STDERR_FILENO, data, n);
        return;
    }
    if (s->max_size || s->freq_ms)
        dyn_sink_maybe_rotate(s, now);
    if (s->fd < 0) {                    /* rotation fell back to stderr */
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
    if (n > s->bufcap) {                /* one giant line bypasses the buffer */
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

static void dyn_sink_free(dyn_log_sink_t *s)
{
    dyn_log_sink_t **pp;
    for (pp = &dyn_sinks; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    dyn_sink_flush(s);                  /* buffered lines still land */
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s->path);
    free(s->base);
    free(s->ext);
    free(s->active);
    free(s);
}

static void dyn_sink_unref(dyn_log_sink_t *s)
{
    if (!s)
        return;
    if (--s->refs > 0)
        return;
    dyn_sink_free(s);
}

/* Exit-time belt for loggers the GC never reached: buffered lines are lost
   otherwise. Flush is idempotent, so a finalizer that already ran costs
   nothing. */
static void dyn_sinks_atexit(void)
{
    dyn_log_sink_t *s;
    for (s = dyn_sinks; s; s = s->next)
        dyn_sink_flush(s);
}

/* rollover.size: a number is BYTES (pino-roll reads bare numbers as MB,
   which surprises more than it helps); a string carries a 1024-based unit --
   "500k", "10m", "2g", an optional trailing 'b', or plain digits. */
static int dyn_parse_size(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 1) || d > 1099511627776.0) {   /* 1 byte .. 1 TiB */
            JS_ThrowRangeError(ctx, "Logger rollover.size: out of range");
            return -1;
        }
        *out = (uint64_t)d;
        return 0;
    }
    if (JS_IsString(v)) {
        const char *src = JS_ToCString(ctx, v);
        const char *cs = src;
        uint64_t n = 0;
        int digits = 0;
        if (!cs)
            return -1;
        while (*cs >= '0' && *cs <= '9') {
            n = n * 10 + (uint64_t)(*cs - '0');
            cs++;
            digits++;
        }
        if (digits && (*cs == 'k' || *cs == 'K' || *cs == 'm' || *cs == 'M' ||
                       *cs == 'g' || *cs == 'G')) {
            uint64_t mult = (*cs == 'k' || *cs == 'K') ? 1024ull
                          : (*cs == 'm' || *cs == 'M') ? 1024ull * 1024
                          : 1024ull * 1024 * 1024;
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

/* rollover.frequency: "daily" and "hourly" name the period in the file;
   a number of milliseconds rotates on a plain interval (numbered names). */
static int dyn_parse_frequency(JSContext *ctx, JSValueConst v, int64_t *out,
                               int *named_date)
{
    if (JS_IsString(v)) {
        const char *cs = JS_ToCString(ctx, v);
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
        if (!(d >= 1) || d > 3153600000000.0) {   /* 1 ms .. 100 years */
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

/* Open (or find) the sink for `path`. Throws on a failed open, an unreadable
   option, or a second Logger demanding DIFFERENT rollover behavior for a
   path that is already open -- the first config wins, and a silent override
   would corrupt both Loggers' rotation state. */
static dyn_log_sink_t *dyn_sink_new(JSContext *ctx, const char *path,
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
        /* The message names the option that actually differs. "different
           rollover options" was the old text, and it sent a caller hunting
           through rollover when the real mismatch was `buffer` -- a
           diagnostic's prose has to name the thing it is complaining about. */
        const char *which = NULL;
        if (found->max_size != max_size)        which = "rollover.size";
        else if (found->freq_ms != freq_ms)     which = "rollover.frequency";
        else if (found->named_date != named_date) which = "rollover.frequency";
        else if (found->keep_count != keep_count) which = "rollover.count";
        else if (found->use_symlink != use_symlink) which = "rollover.symlink";
        else if (found->bufcap != bufcap)       which = "buffer";
        if (which) {
            JS_ThrowRangeError(ctx,
                "Logger: dest is already open with a different %s", which);
            return NULL;
        }
        found->refs++;
        return found;
    }

    s = (dyn_log_sink_t *)calloc(1, sizeof *s);
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
        /* base/ext split: an extension the dest already had is kept, else
           ".log" is appended to every generated name */
        const char *slash = strrchr(path, '/');
        const char *dot = strrchr(slash ? slash : path, '.');
        const char *bn0 = slash ? slash + 1 : path;
        size_t bl;
        if (dot && dot > bn0) {
            bl = (size_t)(dot - path);
            s->ext = strdup(dot);
        } else {
            bl = strlen(path);
            s->ext = strdup(".log");
        }
        s->base = (char *)malloc(bl + 1);
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
            /* time-only rotation reuses the period's file across restarts */
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
        /* no rotation: the dest is used verbatim, never renamed */
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
        /* a restart re-points the symlink and inherits the retention policy
           without waiting for a rotation */
        if (s->use_symlink && s->active) {
            const char *abn = strrchr(s->active, '/');
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
        s->buf = (char *)malloc(bufcap);
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
    {   /* register the exit flush exactly once, however many sinks open */
        static int atexit_done;
        if (!atexit_done) {
            atexit_done = 1;
            atexit(dyn_sinks_atexit);
        }
    }
    return s;
}

/* One value in the HUMAN format: scalars bare and readable, composites as
 * compact JSON -- the same serializer the JSON format uses, so an object
 * renders identically in both (one code path, two framings). */
static void dyn_text_value(JSContext *ctx, dyn_line_t *b, JSValueConst v);

/* The per-line fields in the human format: `k=v ` pairs, frame-reserved keys
   dropped, composites as compact JSON. */
static void dyn_text_fields(JSContext *ctx, dyn_line_t *b, dyn_logger_t *L,
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
        const char *ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, reserve_err)) {
                dyn_line_esc_ctrl(b, ks, kn);
                dyn_line_put(b, "=", 1);
                dyn_text_value(ctx, b, pv);
                dyn_line_put(b, " ", 1);
            }
            if (owned) JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
}

/* One line in the human format:
     2026-08-28T09:15:02.123Z WARN  api: slow request userId=42
   Single-line and control-escaped like the JSON format: framing is a
   guarantee the text rendering does not get to waive. Composites render as
   compact JSON; an Error prints as `Type: message` -- its stack stays in the
   JSON format, because a terminal line that dumps 30 stack frames is its own
   bug. */
static void dyn_emit_text(JSContext *ctx, dyn_logger_t *L, dyn_line_t *b,
                          int64_t now, int magic, int has_err,
                          JSValueConst err_v, int has_fields,
                          JSValueConst fields_v, const char *msg, size_t msg_n)
{
    if (L->ts_mode == DYN_TS_EPOCH) {
        dyn_line_i64(b, now);
        dyn_line_put(b, " ", 1);
    } else if (L->ts_mode == DYN_TS_ISO) {
        int m;
        const char *iso = dyn_iso_sec(L, now, &m);
        char m3[3] = { (char)('0' + m / 100),
                       (char)('0' + (m / 10) % 10),
                       (char)('0' + m % 10) };
        dyn_line_put(b, iso, 19);
        dyn_line_put(b, ".", 1);
        dyn_line_put(b, m3, 3);
        dyn_line_put(b, "Z ", 2);
    }
    dyn_line_put(b, DYN_LV[magic].padded, 5);   /* level, aligned to five columns */
    dyn_line_put(b, " ", 1);
    if (L->name) {
        dyn_line_puts(b, L->name);
        dyn_line_puts(b, ": ");
    }
    dyn_line_esc_ctrl(b, msg ? msg : "", msg ? msg_n : 0);
    if (has_err) {
        const char *cs;
        JSValue f = JS_GetPropertyStr(ctx, err_v, "name");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_put(b, " ", 1);
        dyn_line_esc_ctrl(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
        if (cs) JS_FreeCString(ctx, cs);
        f = JS_GetPropertyStr(ctx, err_v, "message");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_puts(b, ": ");
        dyn_line_esc_ctrl(b, cs ? cs : "", cs ? strlen(cs) : 0);
        if (cs) JS_FreeCString(ctx, cs);
    }
    /* the field area gets ONE explicit separator from the message part; each
       field carries its own trailing space, and the newline replaces the
       final one below */
    if (L->base_text || has_fields)
        dyn_line_put(b, " ", 1);
    if (L->base_text)
        dyn_line_put(b, L->base_text, L->base_text_len);
    if (has_fields)
        dyn_text_fields(ctx, b, L, fields_v, has_err);
    if (b->truncated) {
        /* a truncated text line gets the same in-band marker and a newline,
           so a human reader sees the cut and a consumer keeps its framing */
        dyn_line_force(b, "...", 3);
        dyn_line_force(b, "\n", 1);
    } else if (!b->n || b->p[b->n - 1] == ' ') {
        /* a field's trailing space yields its seat to the newline: no line
           ever ends in whitespace */
        if (b->n) b->p[b->n - 1] = '\n';
        else      dyn_line_put(b, "\n", 1);
    } else {
        dyn_line_put(b, "\n", 1);
    }
}

static dyn_logger_t *dyn_logger_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_logger_t *)dyn_plain_get(ctx, v, dyn_logger_class_id);
}

/* One emit. magic is the level. Argument shapes, pino's: (msg), (fields, msg),
   (err, msg) -- and (err, fields, msg), so an Error's context can ride along
   without pre-copying its properties. An Error first argument is detected,
   not declared. */
static JSValue dyn_log_emit(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    dyn_line_t b;
    int has_err = 0, has_fields = 0, msg_idx = 0;
    const char *msg = NULL;
    const char *msg_owned = NULL;
    size_t msg_n = 0;
    int64_t now;

    if (!L)
        return JS_EXCEPTION;
    if (magic < L->level)                    /* the gate, before any work */
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
        /* zero-copy for the ASCII case; a conversion only for non-ASCII/wider */
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
        dyn_line_json_str(&b, msg ? msg : "", msg ? msg_n : 0);
        if (b.truncated) {
            /* The cut landed mid-value, and it may have landed INSIDE a
               \uXXXX escape -- appending `...` there yields `\u0...` and a
               line no parser will read (measured: a payload of U+0001 cut at
               the cap produced exactly that). So back up over a partial
               escape before marking, then CLOSE the line: the record stays
               one framed, parseable line whatever the payload was. */
            size_t nn = b.n;
            size_t back = 0;
            while (back < 6 && nn > back) {
                char c = b.p[nn - 1 - back];
                if (c == '\\') { back += 1; break; }   /* a lone backslash */
                if (c >= '0' && c <= '9') { back++; continue; }
                if (c == 'u' || c == 'x') { back++; continue; }
                break;                                  /* not an escape tail */
            }
            b.n = (nn >= back) ? nn - back : 0;
            dyn_line_force(&b, "...", 3);
            dyn_line_force(&b, "\"}\n", 3);
        } else {
            dyn_line_puts(&b, "}\n");
        }
    }

    /* ONE write(2) per line on stderr; a file sink may batch. fwrite gives no
       guarantee of one syscall, so two processes appending to one fd can
       interleave mid-line; fflush changes when, not how many. Under PIPE_BUF
       a single write is atomic. */
    dyn_sink_write(L->sink, now, b.p, b.n);
    if (msg_owned) JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_UNDEFINED;
 fail:
    if (msg_owned) JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_EXCEPTION;
}

/* One value in the HUMAN format: scalars bare and readable, composites as
 * compact JSON -- the same serializer the JSON format uses, so an object
 * renders identically in both (one code path, two framings). */
static void dyn_text_value(JSContext *ctx, dyn_line_t *b, JSValueConst v)
{
    if (JS_IsString(v)) {
        size_t n;
        const char *cs = NULL;
        const char *np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) { dyn_line_puts(b, "null"); return; }
        dyn_line_esc_ctrl(b, np, n);
        if (cs)
            JS_FreeCString(ctx, cs);
        return;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) { dyn_line_puts(b, "null"); return; }
    if (JS_IsBool(v)) { dyn_line_puts(b, JS_ToBool(ctx, v) ? "true" : "false"); return; }
    if (JS_IsNumber(v)) { dyn_line_number(ctx, b, v); return; }
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

/* Walk `v` once and append to BOTH prefixes: the JSON `"k":v,...` prefix the
   emit path memcpys, and the `k=v ` prefix the text format prints. Reserved
   frame keys are dropped from both. The prefix is built once here, not per
   line -- that is the whole point of storing it on the Logger. */
static int dyn_base_build(JSContext *ctx, JSValueConst v, dyn_logger_t *L,
                          dyn_line_t *jb, dyn_line_t *tb)
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
        const char *ks;
        size_t kn;
        int owned;
        if (JS_IsException(pv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, pv); continue; }
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
            if (owned) JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

static int dyn_logger_opts(JSContext *ctx, JSValueConst opts, dyn_logger_t *L)
{
    JSValue v;
    const char *s;
    dyn_line_t jb, tb;

    L->level = DYN_LV_INFO;
    L->ts_mode = DYN_TS_EPOCH;
    L->fmt = DYN_FMT_JSON;
    L->name = NULL; L->base = NULL; L->base_len = 0;
    L->base_text = NULL; L->base_text_len = 0;
    L->pid_on = 0; L->host_on = 0;
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
            /* "epochMs" is the default, but an unknown string used to fall
               into it silently -- asymmetric with level/format, which throw.
               A typo (timestmp: "epoch") then produced a log whose time
               shape no consumer was told to expect. */
            if (strcmp(s, "iso") == 0)
                L->ts_mode = DYN_TS_ISO;
            else if (strcmp(s, "epochMs") == 0)
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

    /* The frame prefix, built ONCE here: pid and hostname fold in ahead of
       the caller's base fields, and every later line memcpys the result. */
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
        const char *h = dyn_hostname();
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
        L->base = (char *)malloc(jb.n);
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
        L->base_text = (char *)malloc(tb.n);
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

    /* ---- destination ---- */
    {
        char *dest = NULL;
        uint64_t max_size = 0;
        int64_t freq_ms = 0;
        int named_date = 0, keep_count = 0, use_symlink = 0, do_mkdir = 0;
        int has_rollover = 0;
        size_t bufcap = 0;

        v = JS_GetPropertyStr(ctx, opts, "mkdir");
        if (!JS_IsUndefined(v) && !JS_IsNull(v))
            do_mkdir = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);

        /* line batching: true => 64 KiB, a number is a byte size. stderr
           ignores it (see the sink struct comment). */
        v = JS_GetPropertyStr(ctx, opts, "buffer");
        if (JS_IsBool(v)) {
            if (JS_ToBool(ctx, v))
                bufcap = 64u * 1024u;
        } else if (JS_IsNumber(v)) {
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

        /* everything fallible is parsed BEFORE dest is copied, so no error
           path below can leak the strdup */
        v = JS_GetPropertyStr(ctx, opts, "rollover");
        if (JS_IsObject(v)) {
            JSValue r;
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

        /* dest goes LAST: after it is copied, nothing between here and the
           sink call can fail-and-return */
        v = JS_GetPropertyStr(ctx, opts, "dest");
        if (JS_IsString(v)) {
            const char *cs = JS_ToCString(ctx, v);
            if (cs) {
                dest = strdup(cs);          /* outlives the JSValue */
                JS_FreeCString(ctx, cs);
            }
            if (!dest) {
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "new Logger({ dest }): dest must be a string path");
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
   The prefix is serialized once here, not per line.
   child(fields, { level }) -> the child is BORN at that level. `level` as a
   FIELD stays a reserved (dropped) frame key -- a child could previously
   never ask for a different level at all, and a field named level must not
   silently become one, so the override is an explicit option. */
static JSValue dyn_logger_child(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
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
        v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
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
    /* the parent's prefix first, the child's fields appended after it */
    if (L->base)
        dyn_line_put(&jb, L->base, L->base_len);
    if (L->base_text)
        dyn_line_put(&tb, L->base_text, L->base_text_len);
    if (argc > 0 && dyn_base_build(ctx, argv[0], L, &jb, &tb) < 0) {
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return JS_EXCEPTION;
    }
    C = (dyn_logger_t *)calloc(1, sizeof *C);
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
    C->sink = L->sink;                     /* children share the destination */
    if (C->sink)
        C->sink->refs++;
    if (jb.n) {
        C->base = (char *)malloc(jb.n);
        if (!C->base) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free(C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base, jb.p, jb.n);
        C->base_len = jb.n;
    }
    if (tb.n) {
        C->base_text = (char *)malloc(tb.n);
        if (!C->base_text) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free(C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base_text, tb.p, tb.n);
        C->base_text_len = tb.n;
    }
    dyn_line_free(&jb);
    dyn_line_free(&tb);
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

/* flush() -- buffered file output lands NOW. A no-op on stderr, which is
   unbuffered by contract (its one-write-per-line atomicity). The one call a
   crash-tolerant service makes before it decides an event is durable. */
static JSValue dyn_logger_flush(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!L)
        return JS_EXCEPTION;
    if (L->sink)
        dyn_sink_flush(L->sink);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_logger_proto[] = {
    JS_CFUNC_MAGIC_DEF("trace", 1, dyn_log_emit, DYN_LV_TRACE),
    JS_CFUNC_MAGIC_DEF("debug", 1, dyn_log_emit, DYN_LV_DEBUG),
    JS_CFUNC_MAGIC_DEF("info",  1, dyn_log_emit, DYN_LV_INFO),
    JS_CFUNC_MAGIC_DEF("warn",  1, dyn_log_emit, DYN_LV_WARN),
    JS_CFUNC_MAGIC_DEF("error", 1, dyn_log_emit, DYN_LV_ERROR),
    JS_CFUNC_MAGIC_DEF("fatal", 1, dyn_log_emit, DYN_LV_FATAL),
    JS_CFUNC_DEF("child", 2, dyn_logger_child),
    JS_CFUNC_DEF("enabled", 1, dyn_logger_enabled),
    JS_CFUNC_DEF("flush", 0, dyn_logger_flush),
    JS_CGETSET_DEF("level", dyn_logger_get_level, dyn_logger_set_level),
};

/* ------------------------------------------------------------------ Debug */

/* Debug(namespace) -> a function that logs only when DEBUG matches. The match
   is computed ONCE here, not per call: an unmatched Debug must cost nothing. */
static JSValue dyn_debug_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    const char *ns;
    if (!magic)                              /* namespace not enabled */
        return JS_UNDEFINED;
    ns = JS_ToCString(ctx, data[0]);
    if (!ns)
        return JS_EXCEPTION;
    {
        /* ONE write(2), like the Logger: fwrite gives no guarantee of a
           single syscall, so a Debug line could interleave MID-LINE with a
           log line on the same fd and corrupt both for a line consumer.
           Multiple arguments join with a space, debug's printf-ish habit. */
        dyn_line_t b;
        int i;
        dyn_line_init(&b);
        /* Namespace and message are control-escaped so a single Debug() call
           cannot forge a log line or inject a fake log frame: the Logger
           escapes control chars for exactly this reason, and Debug must match
           that guarantee. '"' and '\\' are left alone (they do not break line
           framing, so ordinary output is unchanged). */
        dyn_line_esc_ctrl(&b, ns, strlen(ns));
        for (i = 0; i < argc; i++) {
            size_t n;
            const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
            dyn_line_put(&b, " ", 1);
            if (s) {
                dyn_line_esc_ctrl(&b, s, n);
                JS_FreeCString(ctx, s);
            }
        }
        dyn_line_put(&b, "\n", 1);
        (void)dyn_write_all(STDERR_FILENO, b.p, b.n);
        dyn_line_free(&b);
    }
    JS_FreeCString(ctx, ns);
    return JS_UNDEFINED;
}

/* DEBUG=a,b:*,-a:secret -- comma-separated patterns, LAST match wins (node
 * debug's rule: specificity by ordering). One '*' may sit anywhere in a
 * pattern and matches any span; a leading '-' negates the pattern. The
 * namespace cannot change inside the loop, so its length hoists. */
static int dyn_debug_match(const char *env, const char *ns)
{
    size_t nsn = strlen(ns);
    int on = 0;
    const char *pat = env;
    while (*pat) {
        const char *e = pat;
        const char *p;
        size_t len;
        int neg = 0, m;
        const char *star;
        while (*e && *e != ',')
            e++;
        len = (size_t)(e - pat);
        p = pat;
        if (len && *p == '-') { neg = 1; p++; len--; }
        while (len && *p == ' ') { p++; len--; }
        while (len && p[len - 1] == ' ') len--;
        if (len) {
            star = memchr(p, '*', len);
            if (star) {
                size_t pre = (size_t)(star - p);
                size_t suf = len - pre - 1;
                m = nsn >= pre + suf && memcmp(p, ns, pre) == 0 &&
                    (suf == 0 || memcmp(p + pre + 1, ns + nsn - suf, suf) == 0);
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
