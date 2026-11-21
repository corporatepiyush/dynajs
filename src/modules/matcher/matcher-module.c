#include "matcher-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

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

size_t dyn_m_units(const uint8_t* p, size_t nbytes)
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

int dyn_m_text(JSContext* ctx, JSValueConst v, dyn_m_text_t* t,
    int ascii_pattern)
{
    JSValue s = JS_ToString(ctx, v), lv;
    int64_t units;

    t->cur_byte = 0;
    t->cur_units = 0;

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

int dyn_m_buf_put(dyn_m_buf_t* b, const char* p, size_t n)
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

static const JSCFunctionListEntry dyn_matcher_funcs[] = {
    JS_CFUNC_DEF("Levenshtein", 2, dyn_levenshtein),
    JS_CFUNC_DEF("DiceCoefficient", 2, dyn_dice),
    JS_CFUNC_DEF("JaroWinkler", 2, dyn_jaro_winkler),
    JS_CFUNC_DEF("DamerauLevenshtein", 2, dyn_damerau_levenshtein),
    JS_CFUNC_DEF("SmithWaterman", 2, dyn_smith_waterman),
    JS_CFUNC_MAGIC_DEF("DiffLines", 2, dyn_diff, DYN_TOK_LINES),
    JS_CFUNC_MAGIC_DEF("DiffWords", 2, dyn_diff, DYN_TOK_WORDS),
    JS_CFUNC_MAGIC_DEF("DiffChars", 2, dyn_diff, DYN_TOK_CHARS),
    JS_CFUNC_DEF("UnifiedDiff", 2, dyn_unified_diff),
    JS_CFUNC_DEF("ApplyPatch", 2, dyn_apply_patch),
    JS_CFUNC_DEF("stem", 1, dyn_matcher_stem),
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
