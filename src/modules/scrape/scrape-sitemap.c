#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#define SM_MAX_BYTES (16u << 20)
#define SM_MAX_LIST_URLS FE_SM_MAX_URLS
#define SM_MAX_LIST_CHILDREN 1000

static int sm_scan(JSContext* ctx, const char* p, size_t n, fe_sm_doc_t* out)
{
    size_t off = 0;
    char msg[160];
    int rc = fe_sm_scan(p, n, out, &off);
    if (rc == FE_SM_OK)
        return 0;
    if (rc == FE_SM_E_OOM) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    fe_sm_format_error(rc, off, msg, sizeof msg);
    if (rc == FE_SM_E_TOO_MANY)
        JS_ThrowRangeError(ctx, "%s", msg);
    else
        JS_ThrowSyntaxError(ctx, "%s", msg);
    return -1;
}

JSValue dyn_sitemap_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    fe_sm_doc_t d;
    JSValue arr;
    size_t len = 0, i;
    const char* xml;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Sitemap.parse(xml): xml must be a string");
    xml = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!xml)
        return JS_EXCEPTION;
    if (len > SM_MAX_BYTES) {
        JS_FreeCString(ctx, xml);
        return JS_ThrowRangeError(ctx, "Sitemap.parse: xml exceeds %d bytes",
            (int)SM_MAX_BYTES);
    }
    {
        size_t k = 0;
        while (k < len && (xml[k] == ' ' || xml[k] == '\t' || xml[k] == '\r' || xml[k] == '\n'))
            k++;
        if (k >= len || xml[k] != '<') {
            JS_FreeCString(ctx, xml);
            return JS_ThrowTypeError(ctx,
                "Sitemap.parse: input is not an XML document (a robots.txt "
                "is not a sitemap -- Robots.sitemaps() lists the sitemap URLs)");
        }
    }
    if (sm_scan(ctx, xml, len, &d) < 0) {
        JS_FreeCString(ctx, xml);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, xml);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        fe_sm_doc_free(&d);
        return JS_EXCEPTION;
    }
    for (i = 0; i < d.n; i++) {
        JSValue u = JS_NewString(ctx, d.v[i]);
        if (JS_IsException(u) || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, u, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, u);
            JS_FreeValue(ctx, arr);
            fe_sm_doc_free(&d);
            return JS_EXCEPTION;
        }
    }
    fe_sm_doc_free(&d);
    return arr;
}

JSValue fe_sm_locs(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    JSValue res = argv[1], body, arr, out;
    const char *body_s, *url;
    size_t body_n = 0, k;
    fe_sm_doc_t d;
    int32_t st = 0;

    (void)this_val;
    (void)argc;
    if (!JS_IsObject(res))
        return JS_ThrowTypeError(ctx, "Sitemap.list: the fetcher returned no response");
    {
        JSValue sv = JS_GetPropertyStr(ctx, res, "status");
        int rc = 0;
        if (JS_IsException(sv))
            return sv;
        if (!JS_IsUndefined(sv))
            rc = JS_ToInt32(ctx, &st, sv);
        JS_FreeValue(ctx, sv);
        if (rc)
            return JS_EXCEPTION;
        if (st < 200 || st > 299) {
            url = JS_ToCString(ctx, argv[0]);
            if (!url)
                return JS_EXCEPTION;
            JS_ThrowRangeError(ctx,
                "Sitemap.list: %s answered HTTP %d (only a 2xx body can be "
                "parsed as a sitemap)",
                url, st);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }
    body = JS_GetPropertyStr(ctx, res, "body");
    if (JS_IsException(body))
        return body;
    body_s = JS_ToCStringLen(ctx, &body_n, body);
    JS_FreeValue(ctx, body);
    if (!body_s)
        return JS_EXCEPTION;
    if (body_n > SM_MAX_BYTES) {
        JS_FreeCString(ctx, body_s);
        return JS_ThrowRangeError(ctx, "Sitemap.list: sitemap exceeds %d bytes",
            (int)SM_MAX_BYTES);
    }
    if (sm_scan(ctx, body_s, body_n, &d) < 0) {
        JS_FreeCString(ctx, body_s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, body_s);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        fe_sm_doc_free(&d);
        return JS_EXCEPTION;
    }
    for (k = 0; k < d.n; k++) {
        JSValue u = JS_NewString(ctx, d.v[k]);
        if (JS_IsException(u) || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)k, u, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, u);
            JS_FreeValue(ctx, arr);
            fe_sm_doc_free(&d);
            return JS_EXCEPTION;
        }
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, arr);
        fe_sm_doc_free(&d);
        return out;
    }
    JS_DefinePropertyValueUint32(ctx, out, 0, arr, JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, 1, JS_NewBool(ctx, d.is_index), JS_PROP_C_W_E);
    fe_sm_doc_free(&d);
    return out;
}

static JSValue sm_make_fetcher(JSContext* ctx, JSValueConst opts)
{
    static const char src[] = "import { Fetcher } from \"dyna:scrape\";\n"
                              "try { globalThis.__dyna_scrape_fef = new Fetcher(\n"
                              "    globalThis.__dyna_scrape_feopts ?? { agent: \"dyna-sitemap/1.0\" }); }\n"
                              "catch (e) { globalThis.__dyna_scrape_fef = undefined; }\n";
    JSValue r, v = JS_UNDEFINED, g, fobj;
    JSPromiseStateEnum st;

    if (JS_IsObject(opts)) {
        g = JS_GetGlobalObject(ctx);
        JS_DefinePropertyValueStr(ctx, g, "__dyna_scrape_feopts", JS_DupValue(ctx, opts), JS_PROP_C_W_E);
        JS_FreeValue(ctx, g);
    }
    r = JS_Eval(ctx, src, strlen(src), "<dyna:scrape sitemap fetcher>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        goto sweep;
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v))
        goto sweep;
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        dyn_scrape_promise_swallow(ctx, v);
        goto sweep;
    }
    if (dyn_scrape_promise_swallow(ctx, v) < 0)
        return JS_EXCEPTION;
    g = JS_GetGlobalObject(ctx);
    fobj = JS_GetPropertyStr(ctx, g, "__dyna_scrape_fef");
    {
        JSAtom a1 = JS_NewAtom(ctx, "__dyna_scrape_fef");
        JS_DeleteProperty(ctx, g, a1, 0);
        JS_FreeAtom(ctx, a1);
    }
    {
        JSAtom a2 = JS_NewAtom(ctx, "__dyna_scrape_feopts");
        JS_DeleteProperty(ctx, g, a2, 0);
        JS_FreeAtom(ctx, a2);
    }
    JS_FreeValue(ctx, g);
    if (JS_IsException(fobj))
        return fobj;
    if (!JS_IsObject(fobj)) {
        JS_FreeValue(ctx, fobj);
        goto sweep;
    }
    return fobj;
sweep:
    {
        JSValue g2 = JS_GetGlobalObject(ctx);
        JSAtom a1 = JS_NewAtom(ctx, "__dyna_scrape_fef");
        JS_DeleteProperty(ctx, g2, a1, 0);
        JS_FreeAtom(ctx, a1);
        JSAtom a2 = JS_NewAtom(ctx, "__dyna_scrape_feopts");
        JS_DeleteProperty(ctx, g2, a2, 0);
        JS_FreeAtom(ctx, a2);
        JS_FreeValue(ctx, g2);
    }
    return JS_ThrowTypeError(ctx,
        "Sitemap.list: the module's own Fetcher could not be constructed "
        "-- pass a Fetcher (or an options bag) explicitly");
}

JSValue fe_sm_source(JSContext* ctx, int argc, JSValueConst* argv, int* own)
{
    *own = 1;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSAtom gatom = JS_NewAtom(ctx, "get");
        JSValue gf = JS_GetProperty(ctx, argv[1], gatom);
        JS_FreeAtom(ctx, gatom);
        if (JS_IsException(gf))
            return gf;
        if (JS_IsFunction(ctx, gf)) {
            JS_FreeValue(ctx, gf);
            *own = 0;
            return JS_DupValue(ctx, argv[1]);
        }
        JS_FreeValue(ctx, gf);
        return sm_make_fetcher(ctx, argv[1]);
    }
    return sm_make_fetcher(ctx, JS_UNDEFINED);
}

int fe_sm_limits(int which)
{
    return which ? (int)SM_MAX_LIST_URLS : (int)SM_MAX_LIST_CHILDREN;
}
#endif
