#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

static const char* const sc_robots_keys[] = { "agent" };

static double sc_robots_atod(const char* buf, const char** end)
{
    JSATODTempMem tmp;
    return js_atod(buf, end, 10, 0, &tmp);
}

static void rb_res_dispose(void* p)
{
    rb_free((rb_t*)p);
}

JSValue dyn_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    rb_t* r;
    const char *txt, *agent = NULL;
    size_t tn = 0;
    JSValue av = JS_UNDEFINED;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Robots(text[, { agent }])");
    txt = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!txt)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_robots_keys, 1)) {
            JS_FreeCString(ctx, txt);
            return JS_EXCEPTION;
        }
        av = JS_GetPropertyStr(ctx, argv[1], "agent");
        if (!JS_IsUndefined(av))
            agent = JS_ToCString(ctx, av);
    }
    r = (rb_t*)calloc(1, sizeof(*r));
    if (!r) {
        JS_FreeCString(ctx, txt);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->delay = -1.0;
    if (rb_parse(r, txt, tn, agent ? agent : "*", sc_robots_atod) < 0) {
        rb_free(r);
        if (agent)
            JS_FreeCString(ctx, agent);
        JS_FreeValue(ctx, av);
        JS_FreeCString(ctx, txt);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (agent)
        JS_FreeCString(ctx, agent);
    JS_FreeValue(ctx, av);
    JS_FreeCString(ctx, txt);

    return dyn_res_wrap(ctx, new_target, dyn_rb_class_id, r, rb_res_dispose);
}

JSValue dyn_rb_allows(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box;
    rb_t* r;
    const char* p;
    size_t pn = 0;
    int verdict;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "allows(path)");
    p = JS_ToCStringLen(ctx, &pn, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    r = box ? (rb_t*)box->native : NULL;
    if (!r) {
        JS_FreeCString(ctx, p);
        return JS_EXCEPTION;
    }
    verdict = rb_allows_path(r, p, pn);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, verdict);
}

JSValue dyn_rb_delay(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    return r->delay < 0 ? JS_NULL : JS_NewFloat64(ctx, r->delay);
}

JSValue dyn_rb_sitemaps(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    JSValue a;
    size_t i;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < r->n_site; i++)
        JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i,
            JS_NewString(ctx, r->sitemaps[i]), JS_PROP_C_W_E);
    return a;
}

JSValue dyn_rb_rules(JSContext* ctx, JSValueConst this_val)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->n);
}

int fe_robots_step(JSContext* ctx, JSValueConst fv, const char* host0,
    char* url, size_t cap, JSValue res, int failed, int hop)
{
    fe_t* f = fe_live(ctx, fv);
    fe_host_t* h = f ? fe_host(f, host0) : NULL;
    double robots_ttl;
    int st;
    if (!h) {
        JS_FreeValue(ctx, res);
        return 0;
    }
    robots_ttl = f->robots_ttl_ms;
    if (failed) {
        JS_FreeValue(ctx, res);
        h->robots_unreachable = 1;
        h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
        return 0;
    }
    st = fe_status(ctx, res);
    if (st < 0) {
        JS_FreeValue(ctx, res);
        return -1;
    }
    f = fe_live(ctx, fv);
    h = f ? fe_host(f, host0) : NULL;
    if (!h) {
        JS_FreeValue(ctx, res);
        return 0;
    }
    if (st >= 300 && st < 400 && hop < 2) {
        JSValue hold = JS_UNDEFINED;
        char nxt[600], host2[300];
        const char *pp, *loc = fe_header(ctx, res, "location", &hold);
        int ok = 0;
        if (!loc && JS_HasException(ctx)) {
            JS_FreeValue(ctx, hold);
            JS_FreeValue(ctx, res);
            return -1;
        }
        f = fe_live(ctx, fv);
        h = f ? fe_host(f, host0) : NULL;
        if (!h) {
            if (loc)
                JS_FreeCString(ctx, loc);
            JS_FreeValue(ctx, hold);
            JS_FreeValue(ctx, res);
            return 0;
        }
        if (loc && fe_resolve(url, loc, nxt, sizeof nxt) > 0 && fe_split(nxt, host2, sizeof host2, &pp) >= 0
            && strlen(nxt) < cap) {
            ok = 1;
            if (!f->allow_private_hosts && fe_host_is_private(host2))
                ok = 0;
            else if (fe_scheme_is_https(url) && !fe_scheme_is_https(nxt))
                ok = 0;
        }
        if (loc)
            JS_FreeCString(ctx, loc);
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, res);
        if (ok) {
            snprintf(url, cap, "%s", nxt);
            return 1;
        }
        h->robots_unreachable = 1;
        h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
        return 0;
    }
    if (st >= 200 && st < 300) {
        JSValue b = JS_GetPropertyStr(ctx, res, "body");
        size_t bn = 0;
        const char* txt = JS_ToCStringLen(ctx, &bn, b);
        if (txt) {
            rb_t* r;
            f = fe_live(ctx, fv);
            h = f ? fe_host(f, host0) : NULL;
            if (!h) {
                JS_FreeCString(ctx, txt);
                JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, res);
                return 0;
            }
            r = (rb_t*)calloc(1, sizeof(rb_t));
            if (r) {
                r->delay = -1;
                if (rb_parse(r, txt, bn, f->agent, sc_robots_atod) < 0) {
                    rb_free(r);
                    h->robots_unreachable = 1;
                } else {
                    if (h->robots)
                        rb_free(h->robots);
                    h->robots = r;
                    h->robots_ok = 1;
                }
            }
            JS_FreeCString(ctx, txt);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, b);
    } else if (st >= 500 && st < 600) {
        h->robots_unreachable = 1;
    } else if (st < 400 || st >= 600) {
        h->robots_unreachable = 1;
    }
    f = fe_live(ctx, fv);
    h = f ? fe_host(f, host0) : NULL;
    if (h)
        h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
    JS_FreeValue(ctx, res);
    return 0;
}
#endif
