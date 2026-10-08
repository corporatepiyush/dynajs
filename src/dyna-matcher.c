#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h"
#include "core/dyn-ac.h"
#include "core/dyn-hash.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_MATCH_MAX_SCAN_STEPS (1000ULL * 1000ULL * 1000ULL)

#define DYN_AC_MAX_HITS (1u << 20)

#define DYN_MATCH_MAX_PATTERN_BYTES (16 * 1024 * 1024)

static size_t dyn_m_units(const uint8_t* p, size_t nbytes)
{
    size_t i = 0, units = 0;
    while (i < nbytes) {
        uint8_t c = p[i];
        if (c < 0x80) {
            i += 1;
            units += 1;
        } else if (c < 0xE0) {
            i += 2;
            units += 1;
        } else if (c < 0xF0) {
            i += 3;
            units += 1;
        } else {
            i += 4;
            units += 2;
        }
    }
    return units;
}

typedef struct {
    const uint8_t* data;
    size_t len;
    const char* owned;
    JSValue str;
    int direct;
} dyn_m_text_t;

static int dyn_m_text(JSContext* ctx, JSValueConst v, dyn_m_text_t* t,
    int ascii_pattern)
{
    JSValue s = JS_ToString(ctx, v), lv;
    int64_t units;

    if (JS_IsException(s))
        return -1;
    t->data = ascii_pattern ? JS_GetNarrowStringBytes(ctx, s, &t->len) : NULL;
    if (t->data) {
        t->owned = NULL;
        t->direct = 1;
        t->str = s;
        return 0;
    }
    t->data = NULL;
    lv = JS_GetPropertyStr(ctx, s, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &units, lv)) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, s);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    t->owned = JS_ToCStringLen(ctx, &t->len, s);
    if (!t->owned) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    t->data = (const uint8_t*)t->owned;
    t->direct = (t->len == (size_t)units);
    t->str = s;
    return 0;
}

static void dyn_m_text_free(JSContext* ctx, dyn_m_text_t* t)
{
    if (t->owned)
        JS_FreeCString(ctx, t->owned);
    JS_FreeValue(ctx, t->str);
}

static int64_t dyn_m_off(const dyn_m_text_t* t, size_t byte_off)
{
    if (t->direct)
        return (int64_t)byte_off;
    return (int64_t)dyn_m_units(t->data, byte_off);
}

static size_t dyn_m_byte_off(const dyn_m_text_t* t, int64_t units)
{
    size_t i = 0;
    int64_t u = 0;
    while (i < t->len && u < units) {
        uint8_t c = t->data[i];
        size_t step = c < 0x80 ? 1 : c < 0xE0 ? 2
            : c < 0xF0                        ? 3
                                              : 4;
        i += step;
        u += step == 4 ? 2 : 1;
    }
    return u >= units ? i : (size_t)-1;
}

enum { MATCH_KMP,
    MATCH_BMH };

typedef struct {
    uint8_t* pat;
    size_t plen;
    size_t ulen;
    int algo;
    int algo_name;
    int ascii;
} dyn_matcher_t;

static void dyn_matcher_free(void* native)
{
    dyn_matcher_t* m = (dyn_matcher_t*)native;
    if (!m)
        return;
    free(m->pat);
    free(m);
}

static JSClassID dyn_matcher_class_id;
static void dyn_matcher_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_matcher_free(JS_GetOpaque(val, dyn_matcher_class_id));
}
static const JSClassDef dyn_matcher_class = {
    "Matcher",
    .finalizer = dyn_matcher_finalizer,
};

static JSValue dyn_matcher_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_matcher_t* m;
    const char* pat;
    size_t plen;
    int algo = MATCH_KMP;
    int algo_name = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Matcher(pattern) requires a pattern");
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue av = JS_GetPropertyStr(ctx, argv[1], "algo");
        if (JS_IsException(av))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(av)) {
            const char* s = JS_ToCString(ctx, av);
            JS_FreeValue(ctx, av);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "kmp")) {
                algo = MATCH_KMP;
                algo_name = 0;
            } else if (!strcmp(s, "bmh")) {
                algo = MATCH_BMH;
                algo_name = 1;
            } else if (!strcmp(s, "boyer-moore")) {
                algo = MATCH_BMH;
                algo_name = 2;
            } else {
                JS_FreeCString(ctx, s);
                return JS_ThrowRangeError(ctx, "algo must be \"kmp\", \"bmh\", or \"boyer-moore\"");
            }
            JS_FreeCString(ctx, s);
        } else {
            JS_FreeValue(ctx, av);
        }
    }
    pat = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    m = (dyn_matcher_t*)malloc(sizeof(*m));
    if (!m) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    m->plen = plen;
    m->algo = algo;
    m->algo_name = algo_name;
    m->pat = (uint8_t*)malloc(plen ? plen : 1);
    if (!m->pat) {
        free(m);
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(m->pat, pat, plen);
    JS_FreeCString(ctx, pat);
    m->ascii = 1;
    for (size_t k = 0; k < plen; k++)
        if (m->pat[k] & 0x80) {
            m->ascii = 0;
            break;
        }
    m->ulen = m->ascii ? plen : dyn_m_units(m->pat, plen);
    return dyn_plain_wrap(ctx, new_target, dyn_matcher_class_id, m, dyn_matcher_free);
}

static size_t matcher_find(const dyn_matcher_t* m, const uint8_t* t, size_t tlen,
    size_t start)
{
    size_t plen = m->plen, rel;
    if (start > tlen || plen > tlen - start)
        return SIZE_MAX;
    if (plen == 0)
        return start;
    rel = simd.strfind(t + start, tlen - start, m->pat, plen);
    return rel == SIZE_MAX ? SIZE_MAX : start + rel;
}

static dyn_matcher_t* matcher_of(JSContext* ctx, JSValueConst t)
{
    return (dyn_matcher_t*)dyn_plain_get(ctx, t, dyn_matcher_class_id);
}

static JSValue dyn_matcher_budget_error(JSContext* ctx)
{
    return JS_ThrowRangeError(ctx,
        "overlapping scan exceeds its %u byte-step budget; the pattern "
        "matches too densely to count or enumerate",
        (unsigned)DYN_MATCH_MAX_SCAN_STEPS);
}

static JSValue dyn_matcher_scan(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_matcher_t* m;
    dyn_m_text_t t;
    size_t pos, start = 0;
    int64_t count = 0, first = -1, from = 0;

    if (magic == 0 && argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &from, argv[1]))
            return JS_EXCEPTION;
        if (from < 0)
            from = 0;
    }

    m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, m->ascii))
        return JS_EXCEPTION;
    if (m->plen == 0) {
        if (magic == 0 && from > 0) {
            int64_t units = t.direct ? (int64_t)t.len
                                     : (int64_t)dyn_m_units(t.data, t.len);
            dyn_m_text_free(ctx, &t);
            return JS_NewInt64(ctx, from < units ? from : units);
        }
        dyn_m_text_free(ctx, &t);
        if (magic == 0)
            return JS_NewInt32(ctx, 0);
        return magic == 1 ? JS_TRUE : JS_NewInt32(ctx, 0);
    }
    if (from > 0) {
        if (t.direct) {
            if (from > (int64_t)t.len) {
                dyn_m_text_free(ctx, &t);
                return JS_NewInt32(ctx, -1);
            }
            start = (size_t)from;
        } else {
            start = dyn_m_byte_off(&t, from);
            if (start == (size_t)-1) {
                dyn_m_text_free(ctx, &t);
                return JS_NewInt32(ctx, -1);
            }
        }
    }
    pos = matcher_find(m, t.data, t.len, start);
    if (pos != SIZE_MAX) {
        first = dyn_m_off(&t, pos);
        if (magic == 2) {
            uint64_t steps = 0;
            size_t mfrom = 0;
            while (pos != SIZE_MAX) {
                count++;
                steps += (uint64_t)(pos - mfrom) + (uint64_t)m->plen;
                if (steps > DYN_MATCH_MAX_SCAN_STEPS) {
                    dyn_m_text_free(ctx, &t);
                    return dyn_matcher_budget_error(ctx);
                }
                mfrom = pos + 1;
                pos = matcher_find(m, t.data, t.len, mfrom);
            }
        }
    }
    dyn_m_text_free(ctx, &t);
    if (magic == 0)
        return JS_NewInt64(ctx, first);
    if (magic == 1)
        return JS_NewBool(ctx, first >= 0);
    return JS_NewInt64(ctx, count);
}

static JSValue dyn_matcher_all_in(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matcher_t* m;
    dyn_m_text_t t;
    size_t pos;
    JSValue arr;
    uint32_t count = 0;
    (void)argc;

    m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, m->ascii))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_m_text_free(ctx, &t);
        return arr;
    }
    if (m->plen > 0) {
        uint64_t steps = 0;
        size_t from = 0;
        pos = matcher_find(m, t.data, t.len, 0);
        while (pos != SIZE_MAX) {
            if (JS_DefinePropertyValueUint32(ctx, arr, count++,
                    JS_NewInt64(ctx, dyn_m_off(&t, pos)), JS_PROP_C_W_E)
                < 0) {
                dyn_m_text_free(ctx, &t);
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            steps += (uint64_t)(pos - from) + (uint64_t)m->plen;
            if (steps > DYN_MATCH_MAX_SCAN_STEPS) {
                dyn_m_text_free(ctx, &t);
                JS_FreeValue(ctx, arr);
                return dyn_matcher_budget_error(ctx);
            }
            from = pos + 1;
            pos = matcher_find(m, t.data, t.len, from);
        }
    }
    dyn_m_text_free(ctx, &t);
    return arr;
}

static JSValue dyn_matcher_replace_all_in(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matcher_t* m;
    const char *text, *repl;
    size_t tlen, rlen, pos, last = 0, cap, used = 0;
    char* buf;
    JSValue out;
    (void)argc;

    text = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    repl = JS_ToCStringLen(ctx, &rlen, argv[1]);
    if (!repl) {
        JS_FreeCString(ctx, text);
        return JS_EXCEPTION;
    }
    m = matcher_of(ctx, this_val);
    if (!m) {
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return JS_EXCEPTION;
    }
    if (m->plen == 0) {
        out = JS_NewStringLen(ctx, text, tlen);
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return out;
    }
    {
        size_t nmatch = 0;
        pos = matcher_find(m, (const uint8_t*)text, tlen, 0);
        while (pos != SIZE_MAX) {
            nmatch++;
            pos = matcher_find(m, (const uint8_t*)text, tlen, pos + m->plen);
        }
        if (nmatch && rlen > m->plen && nmatch > (SIZE_MAX - tlen) / (rlen - m->plen)) {
            JS_FreeCString(ctx, text);
            JS_FreeCString(ctx, repl);
            return JS_ThrowRangeError(ctx, "replacement result is too large");
        }
        cap = tlen + (rlen > m->plen ? nmatch * (rlen - m->plen) : 0);
    }
    buf = (char*)malloc(cap ? cap : 1);
    if (!buf) {
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return JS_ThrowOutOfMemory(ctx);
    }
    pos = matcher_find(m, (const uint8_t*)text, tlen, 0);
    while (pos != SIZE_MAX) {
        memcpy(buf + used, text + last, pos - last);
        used += pos - last;
        memcpy(buf + used, repl, rlen);
        used += rlen;
        last = pos + m->plen;
        pos = matcher_find(m, (const uint8_t*)text, tlen, last);
    }
    memcpy(buf + used, text + last, tlen - last);
    used += tlen - last;
    out = JS_NewStringLen(ctx, buf, used);
    free(buf);
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, repl);
    return out;
}

static JSValue dyn_matcher_length(JSContext* ctx, JSValueConst this_val)
{
    dyn_matcher_t* m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->ulen);
}

static JSValue dyn_matcher_algo(JSContext* ctx, JSValueConst this_val)
{
    dyn_matcher_t* m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    {
        static const char* const names[] = { "kmp", "bmh", "boyer-moore" };
        return JS_NewString(ctx, names[m->algo_name]);
    }
}

static const JSCFunctionListEntry dyn_matcher_proto[] = {
    JS_CFUNC_MAGIC_DEF("firstIn", 1, dyn_matcher_scan, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, dyn_matcher_scan, 1),
    JS_CFUNC_MAGIC_DEF("countIn", 1, dyn_matcher_scan, 2),
    JS_CFUNC_DEF("allIn", 1, dyn_matcher_all_in),
    JS_CFUNC_DEF("replaceAllIn", 2, dyn_matcher_replace_all_in),
    JS_CGETSET_DEF("length", dyn_matcher_length, NULL),
    JS_CGETSET_DEF("algo", dyn_matcher_algo, NULL),
};

static JSClassID dyn_ac_class_id;

static void dyn_ac_free_v(void* native)
{
    dyn_ac_free((dyn_ac_t*)native);
}

static void dyn_ac_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_ac_free_v(JS_GetOpaque(val, dyn_ac_class_id));
}
static const JSClassDef dyn_ac_class = {
    "MultiMatcher",
    .finalizer = dyn_ac_finalizer,
};

static JSValue dyn_ac_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_ac_t* a;
    int64_t n, i;
    uint8_t** pats = NULL;
    size_t* lens = NULL;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new MultiMatcher(patterns[])");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n < 1)
        return JS_ThrowRangeError(ctx, "at least one pattern is required");
    if (n > 65536)
        return JS_ThrowRangeError(ctx, "at most 65536 patterns");

    pats = (uint8_t**)calloc((size_t)n, sizeof(*pats));
    lens = (size_t*)calloc((size_t)n, sizeof(*lens));
    if (!pats || !lens) {
        free(pats);
        free(lens);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        int64_t total = 0;
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            const char* s;
            size_t sl;
            if (JS_IsException(e))
                goto fail_pats;
            s = JS_ToCStringLen(ctx, &sl, e);
            JS_FreeValue(ctx, e);
            if (!s)
                goto fail_pats;
            if (sl == 0) {
                JS_FreeCString(ctx, s);
                JS_ThrowRangeError(ctx, "an empty pattern matches everywhere and "
                                        "nowhere; it is not a pattern");
                goto fail_pats;
            }
            total += (int64_t)sl;
            if (total > (int64_t)DYN_MATCH_MAX_PATTERN_BYTES) {
                JS_FreeCString(ctx, s);
                JS_ThrowRangeError(ctx, "total pattern bytes exceed %d; "
                                        "split the pattern set",
                    (int)DYN_MATCH_MAX_PATTERN_BYTES);
                goto fail_pats;
            }
            pats[i] = (uint8_t*)malloc(sl);
            if (!pats[i]) {
                JS_FreeCString(ctx, s);
                JS_ThrowOutOfMemory(ctx);
                goto fail_pats;
            }
            memcpy(pats[i], s, sl);
            lens[i] = sl;
            JS_FreeCString(ctx, s);
        }
    }

    a = dyn_ac_new((size_t)n);
    if (!a) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_pats;
    }
    for (i = 0; i < n; i++) {
        if (dyn_ac_insert(a, pats[i], lens[i], (int)i) < 0) {
            dyn_ac_free(a);
            JS_ThrowOutOfMemory(ctx);
            goto fail_pats;
        }
    }
    if (dyn_ac_build(a) < 0) {
        dyn_ac_free(a);
        JS_ThrowOutOfMemory(ctx);
        goto fail_pats;
    }
    for (i = 0; i < n; i++)
        free(pats[i]);
    free(pats);
    free(lens);
    return dyn_plain_wrap(ctx, new_target, dyn_ac_class_id, a, dyn_ac_free_v);

fail_pats:
    if (pats) {
        for (i = 0; i < n; i++)
            free(pats[i]);
        free(pats);
    }
    free(lens);
    return JS_EXCEPTION;
}

static dyn_ac_t* ac_of(JSContext* ctx, JSValueConst t)
{
    return (dyn_ac_t*)dyn_plain_get(ctx, t, dyn_ac_class_id);
}

typedef struct {
    JSContext* ctx;
    const dyn_ac_t* a;
    const dyn_m_text_t* t;
    JSValue arr;
    int64_t n;
    int64_t first_at;
    uint32_t count;
    int first_pat;
    int failed;
    uint64_t steps;
    size_t last_end;
} dyn_ac_sink_t;

static int dyn_ac_budget(dyn_ac_sink_t* s, size_t end_byte)
{
    s->steps += (uint64_t)(end_byte >= s->last_end
                               ? end_byte - s->last_end
                               : 0)
        + 1;
    s->last_end = end_byte;
    if (s->steps > DYN_MATCH_MAX_SCAN_STEPS) {
        s->failed = 2;
        return 1;
    }
    return 0;
}

static int dyn_ac_emit_first(void* ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t* s = (dyn_ac_sink_t*)ud;
    s->first_pat = pat;
    s->first_at = dyn_m_off(s->t, end_byte - s->a->plen[pat]);
    return 1;
}

static int dyn_ac_emit_count(void* ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t* s = (dyn_ac_sink_t*)ud;
    (void)pat;
    s->n++;
    return dyn_ac_budget(s, end_byte);
}

static int dyn_ac_emit_all(void* ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t* s = (dyn_ac_sink_t*)ud;
    JSValue hit;
    if (s->count >= DYN_AC_MAX_HITS) {
        s->failed = 2;
        return 1;
    }
    if (dyn_ac_budget(s, end_byte))
        return 1;
    hit = JS_NewObject(s->ctx);
    if (JS_IsException(hit)) {
        s->failed = 1;
        return 1;
    }
    if (JS_DefinePropertyValueStr(s->ctx, hit, "index",
            JS_NewInt32(s->ctx, pat), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(s->ctx, hit, "at",
               JS_NewInt64(s->ctx, dyn_m_off(s->t, end_byte - s->a->plen[pat])),
               JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueUint32(s->ctx, s->arr, s->count++, hit,
               JS_PROP_C_W_E)
            < 0) {
        s->failed = 1;
        return 1;
    }
    return 0;
}

static JSValue dyn_ac_scan(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_ac_t* a;
    dyn_m_text_t t;
    dyn_ac_sink_t sink;
    JSValue ret;
    (void)argc;

    a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, a->ascii))
        return JS_EXCEPTION;
    memset(&sink, 0, sizeof(sink));
    sink.ctx = ctx;
    sink.a = a;
    sink.t = &t;
    sink.first_pat = -1;
    sink.first_at = -1;
    sink.arr = JS_UNDEFINED;

    if (magic == 3) {
        sink.arr = JS_NewArray(ctx);
        if (JS_IsException(sink.arr)) {
            dyn_m_text_free(ctx, &t);
            return JS_EXCEPTION;
        }
        dyn_ac_run(a, t.data, t.len, dyn_ac_emit_all, &sink);
        dyn_m_text_free(ctx, &t);
        if (sink.failed) {
            JS_FreeValue(ctx, sink.arr);
            return sink.failed == 2 ? dyn_matcher_budget_error(ctx)
                                    : JS_EXCEPTION;
        }
        return sink.arr;
    }
    if (magic == 2) {
        dyn_ac_run(a, t.data, t.len, dyn_ac_emit_count, &sink);
        dyn_m_text_free(ctx, &t);
        if (sink.failed == 2)
            return dyn_matcher_budget_error(ctx);
        return JS_NewInt64(ctx, sink.n);
    }
    dyn_ac_run(a, t.data, t.len, dyn_ac_emit_first, &sink);
    dyn_m_text_free(ctx, &t);
    if (magic == 1)
        return JS_NewBool(ctx, sink.first_pat >= 0);
    if (sink.first_pat < 0)
        return JS_NULL;
    ret = JS_NewObject(ctx);
    if (JS_IsException(ret))
        return ret;
    if (JS_DefinePropertyValueStr(ctx, ret, "index",
            JS_NewInt32(ctx, sink.first_pat), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, ret, "at",
               JS_NewInt64(ctx, sink.first_at), JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    return ret;
}

typedef struct {
    char* buf;
    size_t len, cap;
} dyn_m_buf_t;

static int dyn_m_buf_put(dyn_m_buf_t* b, const char* p, size_t n)
{
    if (n > (size_t)-1 - b->len)
        return -1;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        char* nb;
        while (ncap < b->len + n) {
            if (ncap > (size_t)-1 / 2)
                return -1;
            ncap <<= 1;
        }
        nb = (char*)realloc(b->buf, ncap ? ncap : 1);
        if (!nb)
            return -1;
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    return 0;
}

typedef struct {
    JSContext* ctx;
    const dyn_ac_t* a;
    size_t last_end;
    dyn_m_buf_t out;
    JSValue repl_fn;
    const char* repl_str;
    size_t repl_len;
    const char* text;
    size_t tlen;
    int failed;
    int oom;
} dyn_ac_repl_t;

static int dyn_ac_emit_repl(void* ud, int pat, size_t end_byte)
{
    dyn_ac_repl_t* s = (dyn_ac_repl_t*)ud;
    size_t plen, start;

    if (s->oom)
        return 1;
    plen = s->a->plen[pat];
    if (end_byte > s->tlen || plen > end_byte)
        return 0;
    start = end_byte - plen;
    if (start < s->last_end)
        return 0;
    if (start > s->last_end && dyn_m_buf_put(&s->out, s->text + s->last_end, start - s->last_end) < 0) {
        s->oom = 1;
        return 1;
    }

    if (JS_IsUndefined(s->repl_fn)) {
        if (dyn_m_buf_put(&s->out, s->repl_str, s->repl_len) < 0) {
            s->oom = 1;
            return 1;
        }
    } else {
        JSValue args[2], r;
        args[0] = JS_NewStringLen(s->ctx, s->text + start, plen);
        args[1] = JS_NewInt32(s->ctx, pat);
        if (JS_IsException(args[0])) {
            JS_FreeValue(s->ctx, args[1]);
            s->failed = 1;
            return 1;
        }
        r = JS_Call(s->ctx, s->repl_fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(s->ctx, args[0]);
        JS_FreeValue(s->ctx, args[1]);
        if (JS_IsException(r)) {
            s->failed = 1;
            return 1;
        }
        {
            size_t n;
            const char* rs = JS_ToCStringLen(s->ctx, &n, r);
            JS_FreeValue(s->ctx, r);
            if (!rs) {
                s->failed = 1;
                return 1;
            }
            if (dyn_m_buf_put(&s->out, rs, n) < 0)
                s->oom = 1;
            JS_FreeCString(s->ctx, rs);
            if (s->oom)
                return 1;
        }
    }
    s->last_end = end_byte;
    return 0;
}

static JSValue dyn_ac_replace_all_in(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ac_t* a;
    dyn_ac_repl_t s;
    JSValue ret;
    int is_fn;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "replaceAllIn(text, repl): two arguments required");

    memset(&s, 0, sizeof(s));
    s.ctx = ctx;
    s.repl_fn = JS_UNDEFINED;

    s.text = JS_ToCStringLen(ctx, &s.tlen, argv[0]);
    if (!s.text)
        return JS_EXCEPTION;
    is_fn = JS_IsFunction(ctx, argv[1]);
    if (is_fn) {
        s.repl_fn = JS_DupValue(ctx, argv[1]);
    } else {
        s.repl_str = JS_ToCStringLen(ctx, &s.repl_len, argv[1]);
        if (!s.repl_str) {
            JS_FreeCString(ctx, s.text);
            return JS_EXCEPTION;
        }
    }

    a = ac_of(ctx, this_val);
    if (!a) {
        JS_FreeCString(ctx, s.text);
        if (s.repl_str)
            JS_FreeCString(ctx, s.repl_str);
        JS_FreeValue(ctx, s.repl_fn);
        return JS_EXCEPTION;
    }
    s.a = a;

    dyn_ac_run(a, (const uint8_t*)s.text, s.tlen, dyn_ac_emit_repl, &s);
    if (!s.failed && !s.oom && dyn_m_buf_put(&s.out, s.text + s.last_end, s.tlen - s.last_end) < 0)
        s.oom = 1;

    JS_FreeCString(ctx, s.text);
    if (s.repl_str)
        JS_FreeCString(ctx, s.repl_str);
    JS_FreeValue(ctx, s.repl_fn);
    if (s.failed) {
        free(s.out.buf);
        return JS_EXCEPTION;
    }
    if (s.oom) {
        free(s.out.buf);
        return JS_ThrowOutOfMemory(ctx);
    }
    ret = JS_NewStringLen(ctx, s.out.buf ? s.out.buf : "", s.out.len);
    free(s.out.buf);
    return ret;
}

static JSValue dyn_ac_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_ac_t* a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)a->n_pat);
}

static JSValue dyn_ac_states(JSContext* ctx, JSValueConst this_val)
{
    dyn_ac_t* a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)a->n_states);
}

static const JSCFunctionListEntry dyn_ac_proto[] = {
    JS_CFUNC_MAGIC_DEF("firstIn", 1, dyn_ac_scan, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, dyn_ac_scan, 1),
    JS_CFUNC_MAGIC_DEF("countIn", 1, dyn_ac_scan, 2),
    JS_CFUNC_MAGIC_DEF("allIn", 1, dyn_ac_scan, 3),
    JS_CFUNC_DEF("replaceAllIn", 2, dyn_ac_replace_all_in),
    JS_CGETSET_DEF("size", dyn_ac_size, NULL),
    JS_CGETSET_DEF("states", dyn_ac_states, NULL),
};

#include "dyna-approx.inc.c"
#include "dyna-diff.inc.c"

static const JSCFunctionListEntry dyn_matcher_funcs[] = {
    JS_CFUNC_DEF("Levenshtein", 2, dyn_levenshtein),
    JS_CFUNC_DEF("DiceCoefficient", 2, dyn_dice),
    JS_CFUNC_DEF("JaroWinkler", 2, dyn_jaro_winkler),
    JS_CFUNC_DEF("DamerauLevenshtein", 2, dyn_damerau_levenshtein),
    JS_CFUNC_MAGIC_DEF("DiffLines", 2, dyn_diff, DYN_TOK_LINES),
    JS_CFUNC_MAGIC_DEF("DiffWords", 2, dyn_diff, DYN_TOK_WORDS),
    JS_CFUNC_MAGIC_DEF("DiffChars", 2, dyn_diff, DYN_TOK_CHARS),
#ifdef DYN_APPROX_REFERENCE
    JS_CFUNC_DEF("LevenshteinReference", 2, dyn_levenshtein_ref),
#endif
};

static int dyn_matcher_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_matcher_class_id,
            &dyn_matcher_class, dyn_matcher_proto,
            countof(dyn_matcher_proto), dyn_matcher_ctor,
            "Matcher")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ac_class_id, &dyn_ac_class,
            dyn_ac_proto, countof(dyn_ac_proto),
            dyn_ac_ctor, "MultiMatcher")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_matcher_funcs,
        countof(dyn_matcher_funcs));
}

int js_nat_init_matcher(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:matcher", dyn_matcher_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Matcher");
    JS_AddModuleExport(ctx, m, "MultiMatcher");
    return JS_AddModuleExportList(ctx, m, dyn_matcher_funcs,
        countof(dyn_matcher_funcs));
}

#endif
