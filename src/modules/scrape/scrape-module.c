#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
JSClassID dyn_rb_class_id;

static const JSClassDef dyn_rb_class = {
    "Robots",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_rb_proto[] = {
    JS_CFUNC_DEF("allows", 1, dyn_rb_allows),
    JS_CFUNC_DEF("crawlDelay", 0, dyn_rb_delay),
    JS_CFUNC_DEF("sitemaps", 0, dyn_rb_sitemaps),
    JS_CGETSET_DEF("ruleCount", dyn_rb_rules, NULL),
};

JSClassID dyn_ex_class_id;

static const JSClassDef dyn_ex_class = {
    "Extractor",
    .finalizer = dyn_ex_finalizer,
    .gc_mark = dyn_ex_mark,
};

static const JSCFunctionListEntry dyn_ex_proto[] = {
    JS_CFUNC_DEF("run", 1, dyn_ex_run),
};

JSClassID dyn_fe_class_id;

fe_t* fe_live(JSContext* ctx, JSValueConst fv)
{
    JSClassID id = (JSClassID)0;
    DynResource* r = (DynResource*)JS_GetAnyOpaque(fv, &id);
    (void)ctx;
    if (!r || id != dyn_fe_class_id || r->closed)
        return NULL;
    return (fe_t*)r->native;
}

static const JSClassDef dyn_fe_class = {
    "Fetcher",
    .finalizer = dyn_res_finalizer,
};

int fe_num_prop(JSContext* ctx, JSValueConst o, const char* k, int dflt,
    int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int64_t r = dflt;
    int rc = 0;
    if (JS_IsException(v))
        rc = -1;
    else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_ToInt64(ctx, &r, v))
            rc = -1;
        else if (r < INT32_MIN || r > INT32_MAX) {
            JS_ThrowRangeError(ctx, "Fetcher: %s is out of range", k);
            rc = -1;
        }
    }
    JS_FreeValue(ctx, v);
    *out = (int)r;
    return rc;
}

int fe_status(JSContext* ctx, JSValueConst res)
{
    JSValue v = JS_GetPropertyStr(ctx, res, "status");
    int32_t st = 0;
    int rc;
    rc = JS_ToInt32(ctx, &st, v);
    JS_FreeValue(ctx, v);
    return rc ? -1 : (int)st;
}

void sc_sb_init(sc_sb_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

void sc_sb_free(sc_sb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void sc_sb_put(sc_sb_t* b, const char* s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char* np;
        while (nc < b->n + n)
            nc *= 2;
        np = (char*)realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void sc_sb_puts(sc_sb_t* b, const char* s) { sc_sb_put(b, s, strlen(s)); }

static JSValue dyn_scrape_noop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

int dyn_scrape_promise_swallow(JSContext* ctx, JSValue v)
{
    JSAtom catom = JS_NewAtom(ctx, "catch");
    JSValue cf = JS_GetProperty(ctx, v, catom);
    int rc = 0;
    JS_FreeAtom(ctx, catom);
    if (JS_IsException(cf)) {
        rc = -1;
    } else if (JS_IsFunction(ctx, cf)) {
        JSValue noop = JS_NewCFunction(ctx, dyn_scrape_noop, "", 0);
        JSValue caught = JS_IsException(noop) ? JS_EXCEPTION : JS_Call(ctx, cf, v, 1, (JSValueConst*)&noop);
        if (JS_IsException(caught))
            rc = -1;
        JS_FreeValue(ctx, caught);
        JS_FreeValue(ctx, noop);
    }
    JS_FreeValue(ctx, cf);
    JS_FreeValue(ctx, v);
    return rc;
}

static const JSCFunctionListEntry dyn_fe_proto[] = {
    JS_CFUNC_DEF("stats", 0, dyn_fe_stats),
};

JSClassID dyn_cr_class_id;

static const JSClassDef dyn_cr_class = {
    "Crawl",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_cr_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_cr_start),
    JS_CFUNC_DEF("next", 0, dyn_cr_next),
    JS_CFUNC_DEF("pages", 0, dyn_cr_self),
    JS_CFUNC_DEF("serialize", 0, dyn_cr_serialize),
};

static int dyn_scrape_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_rb_class_id, &dyn_rb_class,
            dyn_rb_proto, countof(dyn_rb_proto),
            dyn_rb_ctor, "Robots")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_cr_class_id, &dyn_cr_class,
            dyn_cr_proto, countof(dyn_cr_proto),
            dyn_cr_ctor, "Crawl")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_cr_class_id);
        if (!JS_IsObject(proto))
            return -1;
        {
            JSValue ctor = JS_GetPropertyStr(ctx, proto, "constructor");
            JS_FreeValue(ctx, proto);
            if (JS_IsException(ctor) || !JS_IsFunction(ctx, ctor)) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
            if (JS_DefinePropertyValueStr(ctx, ctor, "resume",
                    JS_NewCFunction(ctx, dyn_cr_resume,
                        "resume", 2),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
            JS_FreeValue(ctx, ctor);
        }
    }
    if (dyn_register_class(ctx, m, &dyn_fe_class_id, &dyn_fe_class,
            dyn_fe_proto, countof(dyn_fe_proto),
            dyn_fe_ctor, "Fetcher")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ex_class_id, &dyn_ex_class,
            dyn_ex_proto, countof(dyn_ex_proto),
            dyn_ex_ctor, "Extractor")
        < 0)
        return -1;
    {
        JSValue sm = JS_NewObject(ctx);
        if (JS_IsException(sm))
            return -1;
        if (JS_DefinePropertyValueStr(ctx, sm, "parse",
                JS_NewCFunction(ctx, dyn_sitemap_parse,
                    "parse", 1),
                JS_PROP_C_W_E)
                < 0
            || fe_engine_install(ctx, sm) < 0
            || JS_SetModuleExport(ctx, m, "Sitemap", sm) < 0) {
            JS_FreeValue(ctx, sm);
            return -1;
        }
    }
    return 0;
}

int js_nat_init_scrape(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:scrape", dyn_scrape_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Robots");
    JS_AddModuleExport(ctx, m, "Extractor");
    JS_AddModuleExport(ctx, m, "Fetcher");
    JS_AddModuleExport(ctx, m, "Crawl");
    JS_AddModuleExport(ctx, m, "Sitemap");
    return 0;
}
#endif
