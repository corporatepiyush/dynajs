#include "matcher-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

#define DYN_AC_MAX_HITS (1u << 20)
#define DYN_MATCH_MAX_PATTERN_BYTES (16 * 1024 * 1024)

typedef struct {
    JSContext* ctx;
    const dyn_ac_t* a;
    dyn_m_text_t* t;
    JSValue arr;
    int64_t n;
    int64_t first_at;
    uint32_t count;
    int first_pat;
    int failed;
    uint64_t steps;
    size_t last_end;
} dyn_ac_sink_t;

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

JSClassID dyn_ac_class_id;

static void dyn_ac_free_v(void* native)
{
    dyn_ac_free((dyn_ac_t*)native);
}

static void dyn_ac_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_ac_free_v(JS_GetOpaque(val, dyn_ac_class_id));
}

const JSClassDef dyn_ac_class = {
    "MultiMatcher",
    .finalizer = dyn_ac_finalizer,
};

JSValue dyn_ac_ctor(JSContext* ctx, JSValueConst new_target, int argc,
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

typedef struct {
    uint32_t end;
    int32_t id;
} dyn_ac_slot_t;

typedef struct {
    const dyn_ac_t* a;
    int32_t* st;
    int32_t* en;
    int32_t* id;
    size_t cap;
    uint64_t total;
    size_t pos, sweep, maxlen, w;
    dyn_ac_slot_t* ring;
    uint64_t steps;
    size_t last_end;
    int failed;
} dyn_ac_span_t;

static int dyn_ac_span_budget(dyn_ac_span_t* s, size_t end_byte)
{
    s->steps += (uint64_t)(end_byte >= s->last_end ? end_byte - s->last_end : 0) + 1;
    s->last_end = end_byte;
    if (s->steps > DYN_MATCH_MAX_SCAN_STEPS) {
        s->failed = 1;
        return 1;
    }
    return 0;
}

static void dyn_ac_span_put(dyn_ac_span_t* s, size_t start, size_t end, int pat)
{
    if (s->total < s->cap) {
        s->st[s->total] = (int32_t)start;
        s->en[s->total] = (int32_t)end;
        s->id[s->total] = pat;
    }
    s->total++;
}

static int dyn_ac_emit_span(void* ud, int pat, size_t end_byte)
{
    dyn_ac_span_t* s = (dyn_ac_span_t*)ud;
    if (dyn_ac_span_budget(s, end_byte))
        return 1;
    dyn_ac_span_put(s, end_byte - s->a->plen[pat], end_byte, pat);
    return 0;
}

static void dyn_ac_span_sweep(dyn_ac_span_t* s, size_t limit)
{
    while (s->sweep < limit) {
        dyn_ac_slot_t* r = &s->ring[s->sweep % s->w];
        if (r->end && s->sweep >= s->pos) {
            dyn_ac_span_put(s, s->sweep, r->end, r->id);
            s->pos = r->end;
        }
        r->end = 0;
        s->sweep++;
    }
}

static int dyn_ac_emit_longest(void* ud, int pat, size_t end_byte)
{
    dyn_ac_span_t* s = (dyn_ac_span_t*)ud;
    size_t start = end_byte - s->a->plen[pat];
    dyn_ac_slot_t* r;
    if (dyn_ac_span_budget(s, end_byte))
        return 1;
    if (end_byte > s->maxlen)
        dyn_ac_span_sweep(s, end_byte - s->maxlen);
    r = &s->ring[start % s->w];
    if (!r->end || end_byte > r->end) {
        r->end = (uint32_t)end_byte;
        r->id = pat;
    }
    return 0;
}

static int dyn_ac_view(JSContext* ctx, JSValueConst v, int type, const char* what, uint8_t** p, size_t* len)
{
    JSValue buf = JS_UNDEFINED;
    double off = 0, bl = 0;
    int vt = -1, r;
    size_t size = 0;
    uint8_t* base;
    r = JS_GetViewInfo(ctx, v, &buf, &off, &bl, &vt);
    if (r < 0)
        return -1;
    if (r == 0 || vt != type) {
        if (r > 0)
            JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "matchSpansInto: %s must be %s", what, type == JS_TYPED_ARRAY_UINT8 ? "a Uint8Array" : "an Int32Array");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "matchSpansInto: %s is detached", what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "matchSpansInto: %s is out of bounds of its buffer", what);
        return -1;
    }
    JS_FreeValue(ctx, buf);
    *p = base + (size_t)off;
    *len = (size_t)bl;
    return 0;
}

static int dyn_ac_span_opts(JSContext* ctx, JSValueConst o, int* longest)
{
    JSPropertyEnum* props = NULL;
    uint32_t n = 0, i;
    JSValue v;
    *longest = 0;
    if (JS_IsUndefined(o))
        return 0;
    if (!JS_IsObject(o) || JS_IsArray(ctx, o) > 0 || JS_IsFunction(ctx, o)) {
        JS_ThrowTypeError(ctx, "matchSpansInto: opts must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &props, &n, o, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, props[i].atom);
        int ok = k && !strcmp(k, "longestOnly");
        if (!k) {
            JS_FreePropertyEnum(ctx, props, n);
            return -1;
        }
        if (!ok) {
            JS_ThrowTypeError(ctx, "matchSpansInto: unknown option \"%s\" (valid: longestOnly)", k);
            JS_FreeCString(ctx, k);
            JS_FreePropertyEnum(ctx, props, n);
            return -1;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, props, n);
    v = JS_GetPropertyStr(ctx, o, "longestOnly");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsBool(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "matchSpansInto: opts.longestOnly must be a boolean");
        return -1;
    }
    *longest = JS_ToBool(ctx, v);
    return 0;
}

static int dyn_ac_overlap(const uint8_t* a, size_t al, const uint8_t* b, size_t bl)
{
    return al && bl && a < b + bl && b < a + al;
}

static JSValue dyn_ac_match_spans_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_ac_t* a;
    dyn_ac_span_t s;
    uint8_t *t, *p[3];
    size_t tl, l[3], i, j;
    int longest;
    static const char* const names[3] = { "outStarts", "outEnds", "outIds" };
    (void)argc;
    a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    if (dyn_ac_span_opts(ctx, argv[4], &longest))
        return JS_EXCEPTION;
    if (dyn_ac_view(ctx, argv[0], JS_TYPED_ARRAY_UINT8, "buf", &t, &tl))
        return JS_EXCEPTION;
    for (i = 0; i < 3; i++) {
        if (dyn_ac_view(ctx, argv[i + 1], JS_TYPED_ARRAY_INT32, names[i], &p[i], &l[i]))
            return JS_EXCEPTION;
        if (JS_CheckWritableBuffer(ctx, argv[i + 1]) < 0)
            return JS_EXCEPTION;
    }
    if (l[0] != l[1] || l[0] != l[2])
        return JS_ThrowTypeError(ctx, "matchSpansInto: outStarts, outEnds and outIds must have the same length (%zu, %zu, %zu)", l[0] / 4, l[1] / 4, l[2] / 4);
    if (tl > (size_t)INT32_MAX)
        return JS_ThrowRangeError(ctx, "matchSpansInto: buf is longer than 2^31-1 bytes, which Int32 offsets cannot hold");
    for (i = 0; i < 3; i++) {
        if (dyn_ac_overlap(t, tl, p[i], l[i]))
            return JS_ThrowTypeError(ctx, "matchSpansInto: %s shares memory with buf", names[i]);
        for (j = i + 1; j < 3; j++)
            if (dyn_ac_overlap(p[i], l[i], p[j], l[j]))
                return JS_ThrowTypeError(ctx, "matchSpansInto: %s and %s share memory", names[i], names[j]);
    }
    memset(&s, 0, sizeof(s));
    s.a = a;
    s.st = (int32_t*)(void*)p[0];
    s.en = (int32_t*)(void*)p[1];
    s.id = (int32_t*)(void*)p[2];
    s.cap = l[0] / 4;
    if (!longest) {
        dyn_ac_run(a, t, tl, dyn_ac_emit_span, &s);
    } else {
        for (i = 0; i < a->n_pat; i++)
            if (a->plen[i] > s.maxlen)
                s.maxlen = a->plen[i];
        s.w = (s.maxlen < tl ? s.maxlen : tl) + 1;
        s.ring = (dyn_ac_slot_t*)calloc(s.w, sizeof(*s.ring));
        if (!s.ring)
            return JS_ThrowOutOfMemory(ctx);
        dyn_ac_run(a, t, tl, dyn_ac_emit_longest, &s);
        if (!s.failed)
            dyn_ac_span_sweep(&s, tl);
        free(s.ring);
    }
    if (s.failed)
        return dyn_matcher_budget_error(ctx);
    return JS_NewInt64(ctx, (int64_t)s.total);
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

const JSCFunctionListEntry dyn_ac_proto[] = {
    JS_CFUNC_MAGIC_DEF("firstIn", 1, dyn_ac_scan, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, dyn_ac_scan, 1),
    JS_CFUNC_MAGIC_DEF("countIn", 1, dyn_ac_scan, 2),
    JS_CFUNC_MAGIC_DEF("allIn", 1, dyn_ac_scan, 3),
    JS_CFUNC_DEF("replaceAllIn", 2, dyn_ac_replace_all_in),
    JS_CFUNC_DEF("matchSpansInto", 5, dyn_ac_match_spans_into),
    JS_CGETSET_DEF("size", dyn_ac_size, NULL),
    JS_CGETSET_DEF("states", dyn_ac_states, NULL),
};

#endif
