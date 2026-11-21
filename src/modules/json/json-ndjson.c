#include "json-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_JSON)

static JSValue dyn_ndjson_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* src;
    char* copy;
    JSRuntime* rt;
    size_t n, pos;
    JSValue out;
    uint32_t k = 0;
    uint64_t lineno = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Ndjson.parse(text): text must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    rt = JS_GetRuntime(ctx);
    copy = (char*)js_malloc_rt(rt, n + 1);
    if (!copy) {
        JS_FreeCString(ctx, src);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(copy, src, n);
    copy[n] = '\0';
    JS_FreeCString(ctx, src);

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        js_free_rt(rt, copy);
        return JS_EXCEPTION;
    }
    for (pos = 0; pos < n;) {
        char *line = copy + pos, saved;
        const char* nl = (const char*)memchr(copy + pos, '\n', n - pos);
        size_t llen = nl ? (size_t)(nl - line) : n - pos;
        size_t j = 0;
        JSValue val;

        pos += llen + 1;
        lineno++;
        if (llen && line[llen - 1] == '\r')
            llen--;
        while (j < llen && (line[j] == ' ' || line[j] == '\t' || line[j] == '\r'))
            j++;
        if (j == llen)
            continue;
        saved = line[llen];
        line[llen] = '\0';
        val = JS_ParseJSON(ctx, line, llen, "<dyna:json:ndjson>");
        line[llen] = saved;
        if (JS_IsException(val)) {
            JSValue exc = JS_GetException(ctx);
            JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
            const char* m = JS_IsString(msg) ? JS_ToCString(ctx, msg) : NULL;
            JS_FreeValue(ctx, exc);
            JS_ThrowSyntaxError(ctx, "Ndjson.parse: line %llu: %s",
                (unsigned long long)lineno,
                m ? m : "invalid JSON");
            if (m)
                JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, msg);
            JS_FreeValue(ctx, out);
            js_free_rt(rt, copy);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, out, k++, val,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, out);
            js_free_rt(rt, copy);
            return JS_EXCEPTION;
        }
    }
    js_free_rt(rt, copy);
    return out;
}

static JSValue dyn_ndjson_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt;
    JSValue lenv;
    uint64_t len, i;
    char* buf = NULL;
    size_t buflen = 0, bufcap = 0;

    (void)this_val;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx,
            "Ndjson.stringify(items): items must be an array");
    lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToIndex(ctx, &len, lenv)) {
        JS_FreeValue(ctx, lenv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    rt = JS_GetRuntime(ctx);
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue s;
        const char* cs;
        size_t cl;

        if (JS_IsException(v)) {
            js_free_rt(rt, buf);
            return JS_EXCEPTION;
        }
        s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, v);
        if (JS_IsException(s)) {
            js_free_rt(rt, buf);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(s)) {
            JS_FreeValue(ctx, s);
            js_free_rt(rt, buf);
            return JS_ThrowTypeError(ctx,
                "Ndjson.stringify: item %llu has no JSON form "
                "(undefined, a function or a symbol)",
                (unsigned long long)(i + 1));
        }
        cs = JS_ToCStringLen(ctx, &cl, s);
        JS_FreeValue(ctx, s);
        if (!cs) {
            js_free_rt(rt, buf);
            return JS_EXCEPTION;
        }
        if (buflen + cl + 1 > bufcap) {
            size_t ncap = bufcap ? bufcap : 256;
            char* nb;
            while (ncap < buflen + cl + 1)
                ncap *= 2;
            nb = (char*)js_realloc_rt(rt, buf, ncap);
            if (!nb) {
                JS_FreeCString(ctx, cs);
                js_free_rt(rt, buf);
                return JS_ThrowOutOfMemory(ctx);
            }
            buf = nb;
            bufcap = ncap;
        }
        memcpy(buf + buflen, cs, cl);
        buflen += cl;
        buf[buflen++] = '\n';
        JS_FreeCString(ctx, cs);
    }
    {
        JSValue out = JS_NewStringLen(ctx, buf ? buf : "", buflen);
        js_free_rt(rt, buf);
        return out;
    }
}

const JSCFunctionListEntry dyn_ndjson_funcs[] = {
    JS_CFUNC_DEF("parse", 1, dyn_ndjson_parse),
    JS_CFUNC_DEF("stringify", 1, dyn_ndjson_stringify),
};

#endif
