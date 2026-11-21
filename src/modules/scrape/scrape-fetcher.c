#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
static const char* const sc_fetcher_keys[] = { "agent", "client", "headers",
    "robots", "minDelayMs", "retries",
    "maxRedirects", "maxBodyBytes",
    "allowPrivateHosts", "revalidate",
    "robotsTtlMs", "allowInsecureDowngrade",
    "proxy", "ca", "poolSize", "cookieJar" };

static void fe_dispose(void* native)
{
    fe_t* f = (fe_t*)native;
    size_t i;
    if (!f)
        return;
    for (i = 0; i < f->n_hosts; i++) {
        free(f->hosts[i].host);
        if (f->hosts[i].robots)
            rb_free(f->hosts[i].robots);
    }
    free(f->hosts);
    for (i = 0; i < f->n_cache; i++)
        fe_cache_clear(&f->cache[i]);
    free(f->agent);
    free(f->proxy);
    free(f->ca);
    dyn_cookiejar_unref(f->jar);
    free(f);
}

static int fe_strict_int_prop(JSContext* ctx, JSValueConst o, const char* k,
    int dflt, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d = 0;
    int ok;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = dflt;
        return 0;
    }
    ok = JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0;
    JS_FreeValue(ctx, v);
    if (!ok || !(d >= -2147483648.0 && d <= 2147483647.0) || d != (double)(int)d) {
        JS_ThrowTypeError(ctx, "Fetcher: %s must be an integer", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static const char* fe_credential_header(JSContext* ctx, JSValueConst hv,
    char* buf, size_t cap)
{
    static const char* const cred[] = {
        "authorization", "cookie", "proxy-authorization", "set-cookie"
    };
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;

    buf[0] = '\0';
    if (!JS_IsObject(hv))
        return NULL;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, hv,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return NULL;
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        size_t j;
        if (!k)
            continue;
        for (j = 0; j < countof(cred); j++) {
            if (!strcasecmp(k, cred[j])) {
                snprintf(buf, cap, "%s", k);
                JS_FreeCString(ctx, k);
                goto out;
            }
        }
        JS_FreeCString(ctx, k);
    }
out:
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    return buf[0] ? buf : NULL;
}

JSValue fe_auto_client(JSContext* ctx, double max_body)
{
    char src[256];
    JSValue r, v = JS_UNDEFINED, g, hobj, client;
    JSPromiseStateEnum st;
    const char* holder = "__dyna_scrape_client";

    snprintf(src, sizeof src,
        "import { HTTPClient } from \"dyna:http\";\n"
        "try { globalThis.%s = new HTTPClient(%d); }\n"
        "catch (e) { globalThis.%s = undefined; }\n",
        holder, max_body >= (double)(1 << 30) ? (1 << 30) : (int)max_body,
        holder);
    r = JS_Eval(ctx, src, strlen(src), "<dyna:scrape fetcher client>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:http available in this build?) -- pass "
            "`client` explicitly to inject your own");
    }
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:http available in this build?) -- pass "
            "`client` explicitly to inject your own");
    }
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        dyn_scrape_promise_swallow(ctx, v);
        JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:http available in this build?) -- pass "
            "`client` explicitly to inject your own");
        return JS_EXCEPTION;
    }
    if (dyn_scrape_promise_swallow(ctx, v) < 0)
        return JS_EXCEPTION;
    g = JS_GetGlobalObject(ctx);
    hobj = JS_GetPropertyStr(ctx, g, holder);
    client = JS_IsException(hobj) ? JS_EXCEPTION : JS_IsObject(hobj) ? JS_DupValue(ctx, hobj) : JS_UNDEFINED;
    JS_FreeValue(ctx, hobj);
    {
        JSAtom hatom = JS_NewAtom(ctx, holder);
        JS_DeleteProperty(ctx, g, hatom, 0);
        JS_FreeAtom(ctx, hatom);
    }
    JS_FreeValue(ctx, g);
    if (JS_IsException(client))
        return client;
    if (!JS_IsObject(client)) {
        JS_FreeValue(ctx, client);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed -- pass `client` explicitly to inject your own");
    }
    return client;
}

JSValue dyn_fe_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    fe_t* f;
    JSValue av, cv, hv;
    const char* agent = NULL;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Fetcher({ agent, client? }): `agent` is required; omit "
            "`client` to use the built-in HTTPClient, or pass one to inject");
    if (dyn_opts_strict(ctx, argv[0], sc_fetcher_keys, 16))
        return JS_EXCEPTION;
    av = JS_GetPropertyStr(ctx, argv[0], "agent");
    if (JS_IsException(av))
        return JS_EXCEPTION;
    if (JS_IsString(av))
        agent = JS_ToCString(ctx, av);
    JS_FreeValue(ctx, av);
    if (!agent || !*agent) {
        if (agent)
            JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `agent` is required and must be a non-empty string, e.g. "
            "\"mybot/1.0 (+https://example.test/bot)\". There is no default: a "
            "shared one is indistinguishable from anonymous and tells an "
            "operator nothing about who to contact.");
    }
    cv = JS_GetPropertyStr(ctx, argv[0], "client");
    if (JS_IsException(cv)) {
        JS_FreeCString(ctx, agent);
        return JS_EXCEPTION;
    }
    hv = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (JS_IsException(hv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeCString(ctx, agent);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(cv) && !JS_IsNull(cv) && !JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `client` must be an object (a dyna:http HTTPClient, or "
            "a mock with request(method, url, body, headers)) -- omit it or "
            "pass null to use the built-in HTTPClient");
    }
    if (!JS_IsUndefined(hv) && !JS_IsNull(hv) && !JS_IsObject(hv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `headers` must be an object of extra request headers");
    }
    {
        char cred[64];
        if (fe_credential_header(ctx, hv, cred, sizeof cred)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            JS_FreeCString(ctx, agent);
            return JS_ThrowTypeError(ctx,
                "Fetcher: `headers` may not carry credentials (got \"%s\") -- "
                "extra headers are sent to every host, including redirect "
                "targets and robots.txt pre-fetches; fetch authenticated "
                "endpoints through dyna:http instead",
                cred);
        }
    }
    f = (fe_t*)calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->agent = strdup(agent);
    JS_FreeCString(ctx, agent);
    if (!f->agent) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        free(f);
        return JS_ThrowOutOfMemory(ctx);
    }

    {
        int v_robots, v_delay, v_retries, v_maxred, v_maxbody;
        int v_priv, v_reval, v_ttl, v_insec, v_pool;
        if (fe_num_prop(ctx, argv[0], "robots", 1, &v_robots) || fe_num_prop(ctx, argv[0], "minDelayMs", 1000, &v_delay) || fe_num_prop(ctx, argv[0], "retries", 3, &v_retries) || fe_num_prop(ctx, argv[0], "maxRedirects", 5, &v_maxred) || fe_num_prop(ctx, argv[0], "maxBodyBytes", 8 << 20, &v_maxbody) || fe_num_prop(ctx, argv[0], "allowPrivateHosts", 0, &v_priv) || fe_num_prop(ctx, argv[0], "revalidate", 1, &v_reval) || fe_num_prop(ctx, argv[0], "robotsTtlMs", 86400000, &v_ttl) || fe_strict_int_prop(ctx, argv[0], "poolSize", 4, &v_pool) || fe_num_prop(ctx, argv[0], "allowInsecureDowngrade", 0, &v_insec)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_EXCEPTION;
        }
        f->robots_on = v_robots ? 1 : 0;
        f->min_delay_ms = v_delay;
        f->retries = v_retries;
        f->max_redirects = v_maxred;
        f->max_body = (double)v_maxbody;
        if (v_maxbody < 0) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_ThrowTypeError(ctx,
                "Fetcher: maxBodyBytes must be a non-negative number of "
                "bytes");
        }
        f->allow_private_hosts = v_priv ? 1 : 0;
        f->revalidate = v_reval ? 1 : 0;
        f->robots_ttl_ms = (double)v_ttl;
        if (f->robots_ttl_ms < 0)
            f->robots_ttl_ms = 0;
        f->allow_insecure_downgrade = v_insec ? 1 : 0;
        f->pool_size = v_pool;
        if (f->pool_size < 1 || f->pool_size > 64) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_ThrowRangeError(ctx,
                "Fetcher: poolSize must be an integer from 1 to 64");
        }
    }
    {
        JSValue pv = JS_GetPropertyStr(ctx, argv[0], "proxy");
        JSValue qv = JS_GetPropertyStr(ctx, argv[0], "ca");
        if (JS_IsException(pv) || JS_IsException(qv)) {
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, qv);
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(pv) && !JS_IsNull(pv)) {
            const char* s;
            if (!JS_IsString(pv)) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a string URL");
            }
            s = JS_ToCString(ctx, pv);
            if (!s) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a non-empty string");
            }
            f->proxy = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->proxy) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        if (!JS_IsUndefined(qv) && !JS_IsNull(qv)) {
            const char* s;
            if (!JS_IsString(qv)) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a string path");
            }
            s = JS_ToCString(ctx, qv);
            if (!s) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a non-empty string");
            }
            f->ca = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->ca) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, qv);
    }
    if (f->retries < 0)
        f->retries = 0;
    if (f->max_redirects < 0)
        f->max_redirects = 0;
    if (f->min_delay_ms < 0)
        f->min_delay_ms = 0;
    {
        JSValue jv = JS_GetPropertyStr(ctx, argv[0], "cookieJar");
        dyn_cookiejar_t* jar = NULL;
        if (!JS_IsException(jv) && !JS_IsUndefined(jv) && !JS_IsNull(jv)) {
            if (JS_IsObject(cv))
                JS_ThrowTypeError(ctx,
                    "Fetcher: cookieJar applies to the built-in client; give "
                    "an injected client its jar with client.useCookies(jar)");
            else
                jar = dyn_cookiejar_of(ctx, jv, "Fetcher");
        }
        if (JS_IsException(jv) || JS_HasException(ctx)) {
            JS_FreeValue(ctx, jv);
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f->proxy);
            free(f->ca);
            free(f);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, jv);
        f->jar = dyn_cookiejar_ref(jar);
    }
    if (!JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        cv = fe_auto_client(ctx, f->max_body);
        if (JS_IsException(cv)) {
            JS_FreeValue(ctx, hv);
            dyn_cookiejar_unref(f->jar);
            free(f->agent);
            free(f->proxy);
            free(f->ca);
            free(f);
            return JS_EXCEPTION;
        }
        if (f->jar)
            hc_client_adopt_jar(cv, f->jar);
    }
    if (dyn_os_entropy(&f->rng, sizeof f->rng) != 0)
        f->rng = ((uint64_t)dyn_timer_now_ms() << 17) ^ (uintptr_t)f ^ 0x9E3779B97F4A7C15ULL;
    {
        JSValue obj = dyn_res_wrap(ctx, nt, dyn_fe_class_id, f, fe_dispose);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            return obj;
        }
        JS_DefinePropertyValueStr(ctx, obj, "_client", cv, 0);
        JS_DefinePropertyValueStr(ctx, obj, "_headers",
            JS_IsObject(hv) ? hv : JS_UNDEFINED, 0);
        return obj;
    }
}
#endif
