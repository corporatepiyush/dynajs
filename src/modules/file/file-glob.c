#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

typedef struct {
    char* pattern;
    size_t len;
    int has_wildcard;
    dyn_glob_alts alts;
} dyn_gl_t;

static void dyn_gl_dispose(void* native)
{
    dyn_gl_t* g = (dyn_gl_t*)native;
    if (!g)
        return;
    free(g->pattern);
    dyn_glob_alts_free(&g->alts);
    free(g);
}

void dyn_gl_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_gl_dispose(JS_GetOpaque(val, dyn_gl_class_id));
}

JSValue dyn_gl_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g;
    const char* pat;
    size_t plen;
    int erc;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Glob(pattern) requires a string");
    pat = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    g = (dyn_gl_t*)calloc(1, sizeof(*g));
    if (!g) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    g->pattern = (char*)malloc(plen + 1);
    if (!g->pattern) {
        JS_FreeCString(ctx, pat);
        free(g);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(g->pattern, pat, plen);
    g->pattern[plen] = '\0';
    g->len = plen;
    erc = dyn_glob_expand(g->pattern, &g->alts);
    JS_FreeCString(ctx, pat);
    if (erc) {
        free(g->pattern);
        free(g);
        return erc == -2 ? JS_ThrowRangeError(ctx, "Glob: brace expansion yields more than %d patterns", DYN_GLOB_MAX_ALTS) : JS_ThrowOutOfMemory(ctx);
    }
    g->has_wildcard = g->alts.n > 1 || dyn_glob_has_wildcard(g->pattern);
    return dyn_plain_wrap(ctx, new_target, dyn_gl_class_id, g, dyn_gl_dispose);
}

static dyn_gl_t* dyn_gl_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_gl_t*)JS_GetOpaque2(ctx, v, dyn_gl_class_id);
}

JSValue dyn_gl_matches(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    const char* path;

    if (!g)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "matches(path) requires a Path");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    {
        int m = dyn_glob_match_alts(&g->alts, path);
        dyn_path_unborrow(ctx, path);
        return JS_NewBool(ctx, m);
    }
}

JSValue dyn_gl_expand(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    JSValueConst a[2];
    JSValue pat, opts, r;

    if (!g)
        return JS_EXCEPTION;
    pat = JS_NewStringLen(ctx, g->pattern, g->len);
    if (JS_IsException(pat))
        return pat;
    opts = JS_UNDEFINED;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        opts = JS_NewObject(ctx);
        if (JS_IsException(opts)) {
            JS_FreeValue(ctx, pat);
            return opts;
        }
        JS_DefinePropertyValueStr(ctx, opts, "cwd", JS_DupValue(ctx, argv[0]), JS_PROP_C_W_E);
    }
    a[0] = pat;
    a[1] = opts;
    r = dyn_fs_glob(ctx, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, pat);
    JS_FreeValue(ctx, opts);
    return r;
}

JSValue dyn_gl_filter(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    JSValue out;
    int64_t len = 0, i;
    uint32_t k = 0;

    if (!g)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "filter(paths[]) requires an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        const char* path;
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        path = dyn_path_arg(ctx, e, "element");
        if (!path) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        int hit = dyn_glob_match_alts(&g->alts, path);
        dyn_path_unborrow(ctx, path);
        if (hit)
            JS_DefinePropertyValueUint32(ctx, out, k++, e, JS_PROP_C_W_E);
        else
            JS_FreeValue(ctx, e);
    }
    return out;
}

JSValue dyn_gl_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewStringLen(ctx, g->pattern, g->len);
    return JS_NewBool(ctx, g->has_wildcard);
}

static int dyn_gl_match_once(const char* pat, const char* path)
{
    dyn_glob_alts a;
    int rc = dyn_glob_expand(pat, &a), m;
    if (rc)
        return rc;
    m = dyn_glob_match_alts(&a, path);
    dyn_glob_alts_free(&a);
    return m;
}

JSValue dyn_gl_static_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *path, *pat;
    size_t plen;
    JSValue ret;
    int m;
    (void)this_val;

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Glob.match(path, pattern): pattern must be a string");
    if (JS_IsString(argv[0])) {
        path = JS_ToCString(ctx, argv[0]);
        if (!path)
            return JS_EXCEPTION;
        pat = JS_ToCStringLen(ctx, &plen, argv[1]);
        if (!pat) {
            JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
        m = dyn_gl_match_once(pat, path);
        JS_FreeCString(ctx, pat);
        JS_FreeCString(ctx, path);
    } else {
        path = dyn_path_arg(ctx, argv[0], "Glob.match(path, pattern)");
        if (!path)
            return JS_EXCEPTION;
        pat = JS_ToCStringLen(ctx, &plen, argv[1]);
        if (!pat) {
            dyn_path_unborrow(ctx, path);
            return JS_EXCEPTION;
        }
        m = dyn_gl_match_once(pat, path);
        JS_FreeCString(ctx, pat);
        dyn_path_unborrow(ctx, path);
    }
    if (m < 0)
        return m == -2 ? JS_ThrowRangeError(ctx, "Glob.match: brace expansion yields more than %d patterns", DYN_GLOB_MAX_ALTS) : JS_ThrowOutOfMemory(ctx);
    ret = JS_NewBool(ctx, m);
    return ret;
}

#endif
