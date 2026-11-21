#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#define FE_STREAM_TIMEOUT_MS 15000

static fe_t* fe_arg(JSContext* ctx, JSValueConst fv, int mode)
{
    fe_t* f = fe_live(ctx, fv);
    if (!f)
        JS_ThrowTypeError(ctx, "Fetcher: closed while %s() was in flight",
            mode == 1 ? "getStream" : mode == 2 ? "crawl" : "get");
    return f;
}

static int fe_mode(JSContext* ctx, JSValueConst v)
{
    int32_t m = 0;
    if (JS_IsBool(v))
        return JS_ToBool(ctx, v);
    JS_ToInt32(ctx, &m, v);
    return m;
}

static JSValue en_live(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    return fe_arg(ctx, argv[0], fe_mode(ctx, argv[1])) ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue en_begin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char cur[1024];
    const char* u;
    char* frag;
    (void)this_val;
    (void)argc;
    if (!fe_arg(ctx, argv[0], 0))
        return JS_EXCEPTION;
    u = JS_ToCString(ctx, argv[1]);
    if (!u)
        return JS_EXCEPTION;
    if (strlen(u) + 1 > sizeof cur) {
        JS_FreeCString(ctx, u);
        return JS_ThrowRangeError(ctx, "Fetcher: url is too long");
    }
    snprintf(cur, sizeof cur, "%s", u);
    JS_FreeCString(ctx, u);
    frag = strchr(cur, '#');
    if (frag)
        *frag = 0;
    return JS_NewString(ctx, cur);
}

static JSValue en_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char host[300];
    const char *cur, *path = NULL;
    fe_t* f = fe_arg(ctx, argv[0], 0);
    int https;
    JSValue arr;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    cur = JS_ToCString(ctx, argv[1]);
    if (!cur)
        return JS_EXCEPTION;
    https = fe_split(cur, host, sizeof host, &path);
    if (https < 0) {
        JS_ThrowTypeError(ctx, "Fetcher: only http:// and https:// urls (got %.60s)", cur);
        JS_FreeCString(ctx, cur);
        return JS_EXCEPTION;
    }
    if (!f->allow_private_hosts && fe_host_is_private(host)) {
        JS_ThrowTypeError(ctx,
            "Fetcher: %s://%.60s is a private/loopback/link-local host; "
            "pass allowPrivateHosts: true to fetch it",
            https ? "https" : "http", host);
        JS_FreeCString(ctx, cur);
        return JS_EXCEPTION;
    }
    if (!fe_host(f, host)) {
        JS_FreeCString(ctx, cur);
        return JS_ThrowOutOfMemory(ctx);
    }
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewString(ctx, host), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewBool(ctx, https), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 2, JS_NewString(ctx, path ? path : "/"), JS_PROP_C_W_E);
    }
    JS_FreeCString(ctx, cur);
    return arr;
}

static fe_host_t* en_host(JSContext* ctx, JSValueConst fv, JSValueConst hv, fe_t** pf)
{
    fe_t* f = fe_arg(ctx, fv, 0);
    const char* host;
    fe_host_t* h;
    if (!f)
        return NULL;
    host = JS_ToCString(ctx, hv);
    if (!host)
        return NULL;
    h = fe_host(f, host);
    JS_FreeCString(ctx, host);
    if (!h)
        JS_ThrowOutOfMemory(ctx);
    if (pf)
        *pf = f;
    return h;
}

static JSValue en_robots_due(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = NULL;
    fe_host_t* h = en_host(ctx, argv[0], argv[1], &f);
    (void)this_val;
    (void)argc;
    if (!h)
        return JS_EXCEPTION;
    if (!f->robots_on)
        return JS_FALSE;
    return JS_NewBool(ctx, !h->robots_tried || (double)dyn_timer_now_ms() >= h->robots_next_ms);
}

static JSValue en_robots_begin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char url[600];
    fe_host_t* h = en_host(ctx, argv[0], argv[1], NULL);
    (void)this_val;
    (void)argc;
    if (!h)
        return JS_EXCEPTION;
    h->robots_tried = 1;
    snprintf(url, sizeof url, "%s%s/robots.txt", JS_ToBool(ctx, argv[2]) ? "https://" : "http://", h->host);
    return JS_NewString(ctx, url);
}

static JSValue en_robots_step(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char url[600], host[300];
    const char *u, *hs;
    int32_t hop = 0;
    int rc;
    (void)this_val;
    (void)argc;
    if (!fe_live(ctx, argv[0]))
        return JS_NULL;
    u = JS_ToCString(ctx, argv[2]);
    hs = u ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!u || !hs) {
        if (u)
            JS_FreeCString(ctx, u);
        return JS_EXCEPTION;
    }
    snprintf(url, sizeof url, "%s", u);
    snprintf(host, sizeof host, "%s", hs);
    JS_FreeCString(ctx, u);
    JS_FreeCString(ctx, hs);
    hop = dyn_int_or(argv[5], 0);
    rc = fe_robots_step(ctx, argv[0], host, url, sizeof url, JS_DupValue(ctx, argv[3]), JS_ToBool(ctx, argv[4]), hop);
    if (rc < 0)
        return JS_EXCEPTION;
    return rc ? JS_NewString(ctx, url) : JS_NULL;
}

static JSValue en_robots_blocked(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = NULL;
    fe_host_t* h = en_host(ctx, argv[0], argv[1], &f);
    const char* path;
    int blocked;
    (void)this_val;
    (void)argc;
    if (!h)
        return JS_EXCEPTION;
    if (!f->robots_on)
        return JS_FALSE;
    if (h->robots_unreachable && h->robots_ok)
        h->robots_unreachable = 0;
    path = JS_ToCString(ctx, argv[2]);
    if (!path)
        return JS_EXCEPTION;
    blocked = h->robots_unreachable || (h->robots && !rb_allows_path(h->robots, path, strlen(path)));
    JS_FreeCString(ctx, path);
    if (blocked)
        f->skipped_robots++;
    return JS_NewBool(ctx, blocked);
}

static JSValue en_skipped(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    JSValue ret;
    const char* cur;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    cur = JS_ToCString(ctx, argv[1]);
    if (!cur)
        return JS_EXCEPTION;
    ret = JS_NewObject(ctx);
    if (!JS_IsException(ret)) {
        JS_DefinePropertyValueStr(ctx, ret, "status", JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ret, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ret, "skippedByRobots", JS_TRUE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ret, "contentType", JS_NewString(ctx, ""), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ret, "body", JS_NewString(ctx, ""), JS_PROP_C_W_E);
    }
    JS_FreeCString(ctx, cur);
    return ret;
}

static JSValue en_claim(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = NULL;
    fe_host_t* h = en_host(ctx, argv[0], argv[1], &f);
    double now, base, floor_ms;
    (void)this_val;
    (void)argc;
    if (!h)
        return JS_EXCEPTION;
    floor_ms = fe_host_floor_ms(f, h);
    now = (double)dyn_timer_now_ms();
    base = h->next_ok_ms > now ? h->next_ok_ms : now;
    h->next_ok_ms = base + floor_ms;
    if (base > now)
        f->throttled_ms += base - now;
    return JS_NewFloat64(ctx, base - now);
}

static JSValue en_touch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = NULL;
    fe_host_t* h = en_host(ctx, argv[0], argv[1], &f);
    double next;
    (void)this_val;
    (void)argc;
    if (!h)
        return JS_EXCEPTION;
    next = (double)dyn_timer_now_ms() + fe_host_floor_ms(f, h);
    if (next > h->next_ok_ms)
        h->next_ok_ms = next;
    return JS_UNDEFINED;
}

static JSValue en_validators(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    fe_cache_ent_t* ce;
    const char* cur;
    JSValue arr;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    if (!f->revalidate)
        return JS_NULL;
    cur = JS_ToCString(ctx, argv[1]);
    if (!cur)
        return JS_EXCEPTION;
    ce = fe_cache_find(f, cur);
    JS_FreeCString(ctx, cur);
    if (!ce || (!ce->etag && !ce->lm))
        return JS_NULL;
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        JS_DefinePropertyValueUint32(ctx, arr, 0, ce->etag ? JS_NewString(ctx, ce->etag) : JS_NULL, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 1, ce->lm ? JS_NewString(ctx, ce->lm) : JS_NULL, JS_PROP_C_W_E);
    }
    return arr;
}

static JSValue en_headers(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    JSValueConst client = argv[1], uh = argv[2];
    JSValue hdrs;
    const char* url;
    int stream = JS_ToBool(ctx, argv[6]);
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    url = JS_ToCString(ctx, argv[3]);
    if (!url)
        return JS_EXCEPTION;
#ifdef CONFIG_TLS
    if (f->ca && !strncmp(url, "https://", 8) && !(stream ? f->ca_adopted_stream : f->ca_adopted)) {
        char terr[160];
        if (hc_client_adopt_ca(client, f->ca, terr, sizeof terr) < 0) {
            JS_FreeCString(ctx, url);
            return JS_ThrowTypeError(ctx, "Fetcher: ca: %s", terr);
        }
        if (stream)
            f->ca_adopted_stream = 1;
        else
            f->ca_adopted = 1;
    }
#else
    (void)client;
    (void)stream;
#endif
    JS_FreeCString(ctx, url);
    hdrs = JS_NewObject(ctx);
    if (JS_IsException(hdrs))
        return hdrs;
    if (JS_IsObject(uh)) {
        JSPropertyEnum* tab = NULL;
        uint32_t n = 0, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, uh, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, hdrs);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++) {
            JSValue v = JS_GetProperty(ctx, uh, tab[i].atom);
            size_t vn = 0, kn = 0;
            const char *vs, *ks;
            int bad;
            if (JS_IsException(v))
                break;
            vs = JS_ToCStringLen(ctx, &vn, v);
            bad = !vs;
            ks = bad ? NULL : JS_AtomToCStringLen(ctx, &kn, tab[i].atom);
            bad = bad || !ks;
            if (!bad && (!httpk_value_ok(vs, vn) || !httpk_name_ok(ks, kn))) {
                JS_ThrowTypeError(ctx,
                    "Fetcher: header \"%s\" name or value contains a "
                    "byte that would split the request or is not a "
                    "valid HTTP field name",
                    ks);
                bad = 1;
            }
            if (ks)
                JS_FreeCString(ctx, ks);
            if (vs)
                JS_FreeCString(ctx, vs);
            if (bad) {
                JS_FreeValue(ctx, v);
                break;
            }
            JS_DefinePropertyValue(ctx, hdrs, tab[i].atom, v, JS_PROP_C_W_E);
        }
        {
            uint32_t k;
            for (k = 0; k < n; k++)
                JS_FreeAtom(ctx, tab[k].atom);
            js_free(ctx, tab);
        }
        if (i < n) {
            JS_FreeValue(ctx, hdrs);
            return JS_EXCEPTION;
        }
    }
    if (JS_IsString(argv[4]))
        JS_DefinePropertyValueStr(ctx, hdrs, "If-None-Match", JS_DupValue(ctx, argv[4]), JS_PROP_C_W_E);
    if (JS_IsString(argv[5]))
        JS_DefinePropertyValueStr(ctx, hdrs, "If-Modified-Since", JS_DupValue(ctx, argv[5]), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, hdrs, "User-Agent", JS_NewString(ctx, f->agent), JS_PROP_C_W_E);
    return hdrs;
}

static JSValue en_status(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int st;
    (void)this_val;
    (void)argc;
    st = fe_status(ctx, argv[0]);
    return st < 0 ? JS_EXCEPTION : JS_NewInt32(ctx, st);
}

static JSValue en_retry_wait(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    int32_t st = 0, attempt = 0;
    JSValue hold = JS_UNDEFINED;
    const char* ra = NULL;
    double back;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    st = dyn_int_or(argv[1], 0);
    attempt = dyn_int_or(argv[2], 0);
    if (st != 0 && JS_IsObject(argv[3])) {
        ra = fe_header(ctx, argv[3], "retry-after", &hold);
        if (!ra && JS_HasException(ctx)) {
            JS_FreeValue(ctx, hold);
            return JS_EXCEPTION;
        }
        f = fe_arg(ctx, argv[0], 0);
        if (!f) {
            if (ra)
                JS_FreeCString(ctx, ra);
            JS_FreeValue(ctx, hold);
            return JS_EXCEPTION;
        }
    }
    back = fe_retry_wait(f, st, attempt, ra);
    if (ra)
        JS_FreeCString(ctx, ra);
    JS_FreeValue(ctx, hold);
    return JS_NewFloat64(ctx, back);
}

static JSValue en_retried(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    f->retried++;
    return JS_UNDEFINED;
}

static JSValue en_redirect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    char cur[1024];
    const char *u, *loc = NULL;
    JSValue hold = JS_UNDEFINED;
    int32_t st = 0, hop = 0;
    int rr;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    u = JS_ToCString(ctx, argv[1]);
    if (!u)
        return JS_EXCEPTION;
    snprintf(cur, sizeof cur, "%s", u);
    JS_FreeCString(ctx, u);
    st = dyn_int_or(argv[2], 0);
    hop = dyn_int_or(argv[4], 0);
    if (st >= 300 && st < 400 && JS_IsObject(argv[3])) {
        loc = fe_header(ctx, argv[3], "location", &hold);
        if (!loc && JS_HasException(ctx)) {
            JS_FreeValue(ctx, hold);
            return JS_EXCEPTION;
        }
        f = fe_arg(ctx, argv[0], 0);
        if (!f) {
            if (loc)
                JS_FreeCString(ctx, loc);
            JS_FreeValue(ctx, hold);
            return JS_EXCEPTION;
        }
    }
    rr = fe_redirect_step(ctx, f, cur, sizeof cur, st, loc, hop);
    if (loc)
        JS_FreeCString(ctx, loc);
    JS_FreeValue(ctx, hold);
    if (rr < 0)
        return JS_EXCEPTION;
    return rr > 0 ? JS_NewString(ctx, cur) : JS_NULL;
}

static JSValue en_finish(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 0);
    const char* cur;
    int32_t st = 0;
    JSValue r;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    cur = JS_ToCString(ctx, argv[1]);
    if (!cur)
        return JS_EXCEPTION;
    st = dyn_int_or(argv[3], 0);
    r = fe_finish_response(ctx, argv[0], f, cur, JS_DupValue(ctx, argv[2]), st);
    JS_FreeCString(ctx, cur);
    return r;
}

static JSValue en_declared_too_big(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_live(ctx, argv[0]);
    (void)this_val;
    (void)argc;
    JS_ThrowRangeError(ctx, "Fetcher: declared Content-Length exceeds maxBodyBytes (%.0f)",
        f ? f->max_body : 0.0);
    return JS_GetException(ctx);
}

static JSValue en_finish_stream(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 1);
    JSValue hold = JS_UNDEFINED;
    const char* clv;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    clv = fe_header(ctx, argv[2], "content-length", &hold);
    if (!clv && JS_HasException(ctx)) {
        JS_FreeValue(ctx, hold);
        return JS_EXCEPTION;
    }
    if (clv) {
        double decl = httpk_delta_seconds(clv, strlen(clv));
        JS_FreeCString(ctx, clv);
        JS_FreeValue(ctx, hold);
        f = fe_arg(ctx, argv[0], 1);
        if (!f)
            return JS_EXCEPTION;
        if (decl > f->max_body) {
            JSValue r = JS_Invoke(ctx, argv[2], JS_NewAtom(ctx, "close"), 0, NULL);
            JS_FreeValue(ctx, r);
            return JS_ThrowRangeError(ctx,
                "Fetcher: declared Content-Length %.0f exceeds maxBodyBytes", decl);
        }
    } else {
        JS_FreeValue(ctx, hold);
    }
    f->fetched++;
    JS_SetPropertyStr(ctx, argv[2], "url", JS_DupValue(ctx, argv[1]));
    return JS_DupValue(ctx, argv[2]);
}

static JSValue en_too_big(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue m;
    const char* s;
    int big = 0;
    (void)this_val;
    (void)argc;
    if (!JS_IsObject(argv[0]))
        return JS_FALSE;
    m = JS_GetPropertyStr(ctx, argv[0], "message");
    if (JS_IsException(m))
        return JS_EXCEPTION;
    s = JS_IsString(m) ? JS_ToCString(ctx, m) : NULL;
    if (s) {
        big = strstr(s, "response too large") != NULL;
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, m);
    return JS_NewBool(ctx, big);
}

static JSValue en_no_response(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue m = JS_IsObject(argv[0]) ? JS_GetPropertyStr(ctx, argv[0], "message") : JS_UNDEFINED;
    const char* s;
    int32_t n = 1;
    (void)this_val;
    (void)argc;
    if (JS_IsException(m))
        return JS_EXCEPTION;
    s = JS_IsString(m) ? JS_ToCString(ctx, m) : NULL;
    n = dyn_int_or(argv[1], 1);
    JS_ThrowRangeError(ctx, "Fetcher: no response (%.400s) after %d attempt%s",
        s ? s : "connection failed", n, n > 1 ? "s" : "");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, m);
    return JS_GetException(ctx);
}

static JSValue en_peer_ok(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* ip;
    int priv;
    (void)this_val;
    if (argc < 1)
        return JS_FALSE;
    ip = JS_ToCString(ctx, argv[0]);
    if (!ip)
        return JS_EXCEPTION;
    priv = fe_host_is_private(ip);
    JS_FreeCString(ctx, ip);
    return JS_NewBool(ctx, !priv);
}

static JSValue en_stream_client(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    fe_t* f = fe_arg(ctx, argv[0], 1);
    JSValue sc, r, a[1];
    JSAtom m;
    (void)this_val;
    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    sc = JS_GetPropertyStr(ctx, argv[0], "_sclient");
    if (JS_IsException(sc) || JS_IsObject(sc))
        return sc;
    sc = fe_auto_client(ctx, f->max_body);
    if (JS_IsException(sc))
        return sc;
    f = fe_arg(ctx, argv[0], 1);
    if (!f) {
        JS_FreeValue(ctx, sc);
        return JS_EXCEPTION;
    }
    a[0] = JS_NewInt32(ctx, FE_STREAM_TIMEOUT_MS);
    m = JS_NewAtom(ctx, "setTimeout");
    r = JS_Invoke(ctx, sc, m, 1, (JSValueConst*)a);
    JS_FreeAtom(ctx, m);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, sc);
        return r;
    }
    JS_FreeValue(ctx, r);
    if (!f->allow_private_hosts
        && JS_SetPropertyStr(ctx, sc, "onConnect", JS_NewCFunction(ctx, en_peer_ok, "onConnect", 1)) < 0) {
        JS_FreeValue(ctx, sc);
        return JS_EXCEPTION;
    }
    if (f->jar)
        hc_client_adopt_jar(sc, f->jar);
    JS_DefinePropertyValueStr(ctx, argv[0], "_sclient", JS_DupValue(ctx, sc), 0);
    return sc;
}

enum { FR_GET, FR_STREAM, FR_ROBOTS, FR_SITEMAP };
enum { FS_HOP, FS_ROBOTS_DONE, FS_ATTEMPT, FS_GOT, FR_REQ, FR_RESP, SM_TOP, SM_NEXT, SM_KID };

typedef struct {
    int kind, state, hop, attempt, st, own, settled;
    uint32_t kid;
    JSValue fe, client, uh, promise, res, aux, aux2, url;
} fr_t;

#define FR_NVALS 8

static JSClassID fr_class_id;

static JSValue* fr_vals(fr_t* r) { return &r->fe; }

static void fr_finalizer(JSRuntime* rt, JSValue val)
{
    fr_t* r = (fr_t*)JS_GetOpaque(val, fr_class_id);
    int i;
    if (!r)
        return;
    for (i = 0; i < FR_NVALS; i++)
        JS_FreeValueRT(rt, fr_vals(r)[i]);
    free(r);
}

static void fr_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    fr_t* r = (fr_t*)JS_GetOpaque(val, fr_class_id);
    int i;
    if (!r)
        return;
    for (i = 0; i < FR_NVALS; i++)
        JS_MarkValue(rt, fr_vals(r)[i], mark);
}

static JSClassDef fr_class = { "FetcherRun", fr_finalizer, fr_mark, NULL, NULL };

static JSValue fr_new(JSContext* ctx, int kind, fr_t** out)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue obj;
    fr_t* r;
    int i;
    if (!fr_class_id)
        JS_NewClassID(&fr_class_id);
    if (!JS_IsRegisteredClass(rt, fr_class_id) && JS_NewClass(rt, fr_class_id, &fr_class) < 0)
        return JS_EXCEPTION;
    r = (fr_t*)calloc(1, sizeof *r);
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < FR_NVALS; i++)
        fr_vals(r)[i] = JS_UNDEFINED;
    r->kind = kind;
    obj = JS_NewObjectClass(ctx, (int)fr_class_id);
    if (JS_IsException(obj)) {
        free(r);
        return obj;
    }
    JS_SetOpaque(obj, r);
    r->promise = JS_NewPromisePending(ctx);
    if (JS_IsException(r->promise)) {
        r->promise = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    *out = r;
    return obj;
}

static void fr_set(JSContext* ctx, JSValue* slot, JSValue v)
{
    JS_FreeValue(ctx, *slot);
    *slot = v;
}

static void fr_step(JSContext* ctx, JSValueConst robj, fr_t* r, int is_reject, JSValueConst arg);

static JSValue fr_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    fr_t* r = (fr_t*)JS_GetOpaque(data, fr_class_id);
    (void)magic;
    if (r && !r->settled)
        fr_step(ctx, data, r, is_reject, arg);
    return JS_UNDEFINED;
}

static void fr_await(JSContext* ctx, JSValueConst robj, fr_t* r, int state, JSValue v)
{
    JSValue rx = JS_NewPromiseReaction(ctx, fr_rx, robj, 0);
    r->state = state;
    if (JS_IsException(rx) || JS_PromiseUponResolvedWith(ctx, v, rx) < 0) {
        JS_FreeValue(ctx, rx);
        JS_FreeValue(ctx, v);
        rx = JS_GetException(ctx);
        fr_step(ctx, robj, r, 1, rx);
        JS_FreeValue(ctx, rx);
        return;
    }
    JS_FreeValue(ctx, rx);
    JS_FreeValue(ctx, v);
}

static JSValue fr_pause(JSContext* ctx, double ms)
{
    JSValue g = JS_GetGlobalObject(ctx), st, funcs[2], p, a[2], c;
    if (ms > 60000)
        ms = 60000;
    st = JS_GetPropertyStr(ctx, g, "setTimeout");
    JS_FreeValue(ctx, g);
    if (JS_IsException(st))
        return st;
    if (!JS_IsFunction(ctx, st)) {
        JS_FreeValue(ctx, st);
        return JS_ThrowTypeError(ctx, "Fetcher: setTimeout is unavailable");
    }
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, st);
        return p;
    }
    a[0] = funcs[0];
    a[1] = JS_NewFloat64(ctx, ms);
    c = JS_Call(ctx, st, JS_UNDEFINED, 2, (JSValueConst*)a);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (JS_IsException(c)) {
        JS_FreeValue(ctx, p);
        return c;
    }
    JS_FreeValue(ctx, c);
    return p;
}

static void fr_close_res(JSContext* ctx, fr_t* r)
{
    JSAtom m;
    JSValue v;
    if (r->kind != FR_STREAM || !JS_IsObject(r->res))
        return;
    m = JS_NewAtom(ctx, "close");
    v = JS_Invoke(ctx, r->res, m, 0, NULL);
    JS_FreeAtom(ctx, m);
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
}

static void fr_settle(JSContext* ctx, fr_t* r, JSValue v, int is_reject)
{
    JSValue p = r->promise;
    if (r->settled) {
        JS_FreeValue(ctx, v);
        return;
    }
    r->settled = 1;
    r->promise = JS_UNDEFINED;
    if (r->kind == FR_SITEMAP && r->own) {
        JSAtom m = JS_NewAtom(ctx, "close");
        JSValue c = JS_Invoke(ctx, r->fe, m, 0, NULL);
        JS_FreeAtom(ctx, m);
        if (JS_IsException(c))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, c);
    }
    if (r->kind == FR_ROBOTS) {
        JSValue rl = JS_GetPropertyStr(ctx, r->fe, "_rload");
        if (JS_IsException(rl))
            rl = JS_UNDEFINED;
        else if (JS_IsObject(rl)) {
            JSAtom a = JS_ValueToAtom(ctx, r->aux);
            if (a != JS_ATOM_NULL) {
                JS_DeleteProperty(ctx, rl, a, 0);
                JS_FreeAtom(ctx, a);
            }
        }
        JS_FreeValue(ctx, rl);
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    dyn_settle(ctx, p, v, is_reject);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    fr_set(ctx, &r->res, JS_UNDEFINED);
    fr_set(ctx, &r->aux2, JS_UNDEFINED);
}

static void fr_fail(JSContext* ctx, fr_t* r)
{
    fr_settle(ctx, r, JS_GetException(ctx), 1);
}

static JSValue fr_invoke(JSContext* ctx, JSValueConst obj, const char* name, int n, JSValueConst* a)
{
    JSAtom m = JS_NewAtom(ctx, name);
    JSValue v = JS_Invoke(ctx, obj, m, n, a);
    JS_FreeAtom(ctx, m);
    return v;
}

static JSValue fr_robots_wait(JSContext* ctx, fr_t* g);

static int fr_len(JSContext* ctx, JSValueConst o, uint32_t* n)
{
    JSValue l = JS_GetPropertyStr(ctx, o, "length");
    int rc = JS_ToUint32(ctx, n, l);
    JS_FreeValue(ctx, l);
    return rc;
}

static double fr_num(JSContext* ctx, JSValue v)
{
    double d = JS_IsNumber(v) ? dyn_num_value(v) : 0;
    JS_FreeValue(ctx, v);
    return d;
}

static JSValue fr_skipped_stream(JSContext* ctx, JSValueConst url);

static void fr_step_fetch(JSContext* ctx, JSValueConst robj, fr_t* r, int is_reject, JSValueConst arg)
{
    JSValue v, a[7];
    int stream = r->kind == FR_STREAM;
    JSValue mode = JS_NewInt32(ctx, r->kind);
    for (;;) {
        switch (r->state) {
        case FS_HOP:
            a[0] = r->fe;
            a[1] = r->url;
            v = en_target(ctx, JS_UNDEFINED, 2, a);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            fr_set(ctx, &r->aux, v);
            a[1] = JS_GetPropertyUint32(ctx, r->aux, 0);
            if (JS_IsException(a[1]))
                return fr_fail(ctx, r);
            v = en_robots_due(ctx, JS_UNDEFINED, 2, a);
            JS_FreeValue(ctx, a[1]);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            if (JS_ToBool(ctx, v)) {
                v = fr_robots_wait(ctx, r);
                if (JS_IsException(v))
                    return fr_fail(ctx, r);
                return fr_await(ctx, robj, r, FS_ROBOTS_DONE, v);
            }
            r->state = FS_ROBOTS_DONE;
            is_reject = 0;
            continue;
        case FS_ROBOTS_DONE:
            if (is_reject)
                return fr_settle(ctx, r, JS_DupValue(ctx, arg), 1);
            a[0] = r->fe;
            a[1] = mode;
            v = en_live(ctx, JS_UNDEFINED, 2, a);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            a[1] = JS_GetPropertyUint32(ctx, r->aux, 0);
            if (JS_IsException(a[1]))
                return fr_fail(ctx, r);
            a[2] = JS_GetPropertyUint32(ctx, r->aux, 2);
            if (JS_IsException(a[2])) {
                JS_FreeValue(ctx, a[1]);
                return fr_fail(ctx, r);
            }
            v = en_robots_blocked(ctx, JS_UNDEFINED, 3, a);
            JS_FreeValue(ctx, a[2]);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, a[1]);
                return fr_fail(ctx, r);
            }
            if (JS_ToBool(ctx, v)) {
                JS_FreeValue(ctx, a[1]);
                a[1] = r->url;
                v = stream ? fr_skipped_stream(ctx, r->url) : en_skipped(ctx, JS_UNDEFINED, 2, a);
                if (JS_IsException(v))
                    return fr_fail(ctx, r);
                return fr_settle(ctx, r, v, 0);
            }
            v = en_claim(ctx, JS_UNDEFINED, 2, a);
            JS_FreeValue(ctx, a[1]);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            {
                double w = fr_num(ctx, v);
                if (w > 0) {
                    v = fr_pause(ctx, w);
                    if (JS_IsException(v))
                        return fr_fail(ctx, r);
                    return fr_await(ctx, robj, r, FS_ATTEMPT, v);
                }
            }
            r->state = FS_ATTEMPT;
            continue;
        case FS_ATTEMPT: {
            JSValue vd = JS_NULL, hdrs, call;
            a[0] = r->fe;
            a[1] = mode;
            v = en_live(ctx, JS_UNDEFINED, 2, a);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            if (r->kind == FR_GET) {
                a[1] = r->url;
                vd = en_validators(ctx, JS_UNDEFINED, 2, a);
                if (JS_IsException(vd))
                    return fr_fail(ctx, r);
            }
            a[1] = r->client;
            a[2] = r->uh;
            a[3] = r->url;
            a[4] = JS_IsObject(vd) ? JS_GetPropertyUint32(ctx, vd, 0) : JS_NULL;
            a[5] = JS_IsObject(vd) && !JS_IsException(a[4]) ? JS_GetPropertyUint32(ctx, vd, 1) : JS_NULL;
            if (JS_IsException(a[4]) || JS_IsException(a[5])) {
                JS_FreeValue(ctx, a[4]);
                JS_FreeValue(ctx, a[5]);
                JS_FreeValue(ctx, vd);
                return fr_fail(ctx, r);
            }
            a[6] = JS_NewBool(ctx, stream);
            hdrs = en_headers(ctx, JS_UNDEFINED, 7, a);
            JS_FreeValue(ctx, a[4]);
            JS_FreeValue(ctx, a[5]);
            JS_FreeValue(ctx, vd);
            if (JS_IsException(hdrs))
                return fr_fail(ctx, r);
            if (stream) {
                a[0] = r->url;
                a[1] = hdrs;
                call = fr_invoke(ctx, r->client, "getStream", 2, a);
            } else {
                a[0] = JS_NewString(ctx, "GET");
                a[1] = r->url;
                a[2] = JS_UNDEFINED;
                a[3] = hdrs;
                call = fr_invoke(ctx, r->client, "request", 4, a);
                JS_FreeValue(ctx, a[0]);
            }
            JS_FreeValue(ctx, hdrs);
            if (JS_IsException(call)) {
                JSValue e = JS_GetException(ctx);
                r->state = FS_GOT;
                fr_step_fetch(ctx, robj, r, 1, e);
                JS_FreeValue(ctx, e);
                return;
            }
            return fr_await(ctx, robj, r, FS_GOT, call);
        }
        case FS_GOT: {
            double back;
            a[0] = r->fe;
            a[1] = mode;
            v = en_live(ctx, JS_UNDEFINED, 2, a);
            if (JS_IsException(v)) {
                if (!is_reject && stream && JS_IsObject(arg)) {
                    fr_set(ctx, &r->res, JS_DupValue(ctx, arg));
                    fr_close_res(ctx, r);
                }
                return fr_fail(ctx, r);
            }
            a[1] = JS_GetPropertyUint32(ctx, r->aux, 0);
            if (JS_IsException(a[1]))
                return fr_fail(ctx, r);
            v = en_touch(ctx, JS_UNDEFINED, 2, a);
            JS_FreeValue(ctx, a[1]);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            if (is_reject) {
                fr_set(ctx, &r->res, JS_UNDEFINED);
                if (stream) {
                    v = en_too_big(ctx, JS_UNDEFINED, 1, &arg);
                    if (JS_IsException(v))
                        return fr_fail(ctx, r);
                    if (JS_ToBool(ctx, v))
                        return fr_settle(ctx, r, en_declared_too_big(ctx, JS_UNDEFINED, 1, &r->fe), 1);
                }
                a[1] = JS_NewInt32(ctx, 0);
                a[2] = JS_NewInt32(ctx, r->attempt);
                a[3] = JS_UNDEFINED;
                v = en_retry_wait(ctx, JS_UNDEFINED, 4, a);
                if (JS_IsException(v))
                    return fr_fail(ctx, r);
                back = fr_num(ctx, v);
                if (back < 0) {
                    if (stream) {
                        a[0] = arg;
                        a[1] = JS_NewInt32(ctx, r->attempt + 1);
                        v = en_no_response(ctx, JS_UNDEFINED, 2, a);
                        if (JS_IsException(v))
                            return fr_fail(ctx, r);
                        return fr_settle(ctx, r, v, 1);
                    }
                    return fr_settle(ctx, r, JS_DupValue(ctx, arg), 1);
                }
            } else {
                fr_set(ctx, &r->res, JS_DupValue(ctx, arg));
                v = en_status(ctx, JS_UNDEFINED, 1, &r->res);
                if (JS_IsException(v)) {
                    fr_close_res(ctx, r);
                    return fr_fail(ctx, r);
                }
                r->st = (int)fr_num(ctx, v);
                a[1] = JS_NewInt32(ctx, r->st);
                a[2] = JS_NewInt32(ctx, r->attempt);
                a[3] = r->res;
                v = en_retry_wait(ctx, JS_UNDEFINED, 4, a);
                back = JS_IsException(v) ? 0 : fr_num(ctx, v);
                if (JS_IsException(v)) {
                    fr_close_res(ctx, r);
                    return fr_fail(ctx, r);
                }
                if (back < 0) {
                    a[1] = r->url;
                    a[2] = JS_NewInt32(ctx, r->st);
                    a[3] = r->res;
                    a[4] = JS_NewInt32(ctx, r->hop);
                    v = en_redirect(ctx, JS_UNDEFINED, 5, a);
                    if (JS_IsException(v)) {
                        fr_close_res(ctx, r);
                        return fr_fail(ctx, r);
                    }
                    if (JS_IsString(v)) {
                        fr_close_res(ctx, r);
                        fr_set(ctx, &r->res, JS_UNDEFINED);
                        fr_set(ctx, &r->url, v);
                        r->hop++;
                        r->attempt = 0;
                        r->state = FS_HOP;
                        continue;
                    }
                    a[1] = r->url;
                    a[2] = r->res;
                    a[3] = JS_NewInt32(ctx, r->st);
                    v = stream ? en_finish_stream(ctx, JS_UNDEFINED, 3, a) : en_finish(ctx, JS_UNDEFINED, 4, a);
                    if (JS_IsException(v))
                        return fr_fail(ctx, r);
                    return fr_settle(ctx, r, v, 0);
                }
                fr_close_res(ctx, r);
                fr_set(ctx, &r->res, JS_UNDEFINED);
            }
            en_retried(ctx, JS_UNDEFINED, 1, &r->fe);
            r->attempt++;
            v = fr_pause(ctx, back);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            return fr_await(ctx, robj, r, FS_ATTEMPT, v);
        }
        default:
            return fr_settle(ctx, r, JS_ThrowTypeError(ctx, "Fetcher: bad state"), 1);
        }
    }
}

static void fr_step_robots(JSContext* ctx, JSValueConst robj, fr_t* r, int is_reject, JSValueConst arg)
{
    JSValue v, a[7], call;
    for (;;) {
        if (r->state == FR_REQ) {
            a[0] = r->fe;
            a[1] = r->client;
            a[2] = r->uh;
            a[3] = r->url;
            a[4] = JS_NULL;
            a[5] = JS_NULL;
            a[6] = JS_FALSE;
            v = en_headers(ctx, JS_UNDEFINED, 7, a);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                is_reject = 1;
                arg = JS_UNDEFINED;
                r->state = FR_RESP;
                continue;
            }
            a[0] = JS_NewString(ctx, "GET");
            a[1] = r->url;
            a[2] = JS_UNDEFINED;
            a[3] = v;
            call = fr_invoke(ctx, r->client, "request", 4, a);
            JS_FreeValue(ctx, a[0]);
            JS_FreeValue(ctx, v);
            if (JS_IsException(call)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                is_reject = 1;
                arg = JS_UNDEFINED;
                r->state = FR_RESP;
                continue;
            }
            return fr_await(ctx, robj, r, FR_RESP, call);
        }
        a[0] = r->fe;
        a[1] = r->aux;
        a[2] = r->url;
        a[3] = is_reject ? JS_UNDEFINED : arg;
        a[4] = JS_NewBool(ctx, is_reject);
        a[5] = JS_NewInt32(ctx, r->hop);
        v = en_robots_step(ctx, JS_UNDEFINED, 6, a);
        if (JS_IsException(v))
            return fr_fail(ctx, r);
        if (JS_IsString(v) && r->hop < 2) {
            fr_set(ctx, &r->url, v);
            r->hop++;
            r->state = FR_REQ;
            continue;
        }
        JS_FreeValue(ctx, v);
        return fr_settle(ctx, r, JS_UNDEFINED, 0);
    }
}

static JSValue fr_robots_wait(JSContext* ctx, fr_t* g)
{
    JSValue rl, host, p, robj, a[3];
    JSAtom hk;
    fr_t* r;
    rl = JS_GetPropertyStr(ctx, g->fe, "_rload");
    if (JS_IsException(rl))
        return rl;
    if (!JS_IsObject(rl)) {
        JS_FreeValue(ctx, rl);
        rl = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(rl))
            return rl;
        if (JS_DefinePropertyValueStr(ctx, g->fe, "_rload", JS_DupValue(ctx, rl), 0) < 0) {
            JS_FreeValue(ctx, rl);
            return JS_EXCEPTION;
        }
    }
    host = JS_GetPropertyUint32(ctx, g->aux, 0);
    if (JS_IsException(host)) {
        JS_FreeValue(ctx, rl);
        return JS_EXCEPTION;
    }
    hk = JS_ValueToAtom(ctx, host);
    if (hk == JS_ATOM_NULL) {
        JS_FreeValue(ctx, host);
        JS_FreeValue(ctx, rl);
        return JS_EXCEPTION;
    }
    p = JS_GetProperty(ctx, rl, hk);
    if (JS_IsObject(p) || JS_IsException(p)) {
        JS_FreeAtom(ctx, hk);
        JS_FreeValue(ctx, host);
        JS_FreeValue(ctx, rl);
        return p;
    }
    robj = fr_new(ctx, FR_ROBOTS, &r);
    if (JS_IsException(robj)) {
        JS_FreeAtom(ctx, hk);
        JS_FreeValue(ctx, host);
        JS_FreeValue(ctx, rl);
        return robj;
    }
    r->fe = JS_DupValue(ctx, g->fe);
    r->client = JS_GetPropertyStr(ctx, g->fe, "_client");
    r->uh = JS_GetPropertyStr(ctx, g->fe, "_headers");
    r->aux = host;
    a[0] = g->fe;
    a[1] = host;
    a[2] = JS_GetPropertyUint32(ctx, g->aux, 1);
    r->url = JS_IsException(a[2]) ? JS_EXCEPTION : en_robots_begin(ctx, JS_UNDEFINED, 3, a);
    JS_FreeValue(ctx, a[2]);
    if (JS_IsException(r->url) || JS_IsException(r->client) || JS_IsException(r->uh)) {
        r->url = JS_UNDEFINED;
        JS_FreeAtom(ctx, hk);
        JS_FreeValue(ctx, rl);
        JS_FreeValue(ctx, robj);
        return JS_EXCEPTION;
    }
    p = JS_DupValue(ctx, r->promise);
    JS_SetProperty(ctx, rl, hk, JS_DupValue(ctx, p));
    JS_FreeAtom(ctx, hk);
    JS_FreeValue(ctx, rl);
    r->state = FR_REQ;
    fr_step_robots(ctx, robj, r, 0, JS_UNDEFINED);
    JS_FreeValue(ctx, robj);
    return p;
}

static void fr_step_sitemap(JSContext* ctx, JSValueConst robj, fr_t* r, int is_reject, JSValueConst arg)
{
    JSValue v, a[2], list;
    uint32_t n = 0, m = 0, i;
    for (;;) {
        switch (r->state) {
        case SM_TOP:
        case SM_KID:
            if (is_reject)
                return fr_settle(ctx, r, JS_DupValue(ctx, arg), 1);
            a[0] = r->state == SM_TOP ? r->url : r->res;
            a[1] = arg;
            v = fe_sm_locs(ctx, JS_UNDEFINED, 2, a);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            list = JS_GetPropertyUint32(ctx, v, 0);
            if (JS_IsException(list)) {
                JS_FreeValue(ctx, v);
                return fr_fail(ctx, r);
            }
            if (r->state == SM_TOP) {
                JSValue ix = JS_GetPropertyUint32(ctx, v, 1);
                int index;
                JS_FreeValue(ctx, v);
                if (JS_IsException(ix)) {
                    JS_FreeValue(ctx, list);
                    return fr_fail(ctx, r);
                }
                index = JS_ToBool(ctx, ix);
                JS_FreeValue(ctx, ix);
                if (!index)
                    return fr_settle(ctx, r, list, 0);
                if (fr_len(ctx, list, &n) < 0 || n > (uint32_t)fe_sm_limits(0)) {
                    JS_FreeValue(ctx, list);
                    if (!JS_HasException(ctx))
                        JS_ThrowRangeError(ctx, "Sitemap.list: more than %d child sitemaps in the index", fe_sm_limits(0));
                    return fr_fail(ctx, r);
                }
                fr_set(ctx, &r->aux, list);
                fr_set(ctx, &r->aux2, JS_NewArray(ctx));
                r->kid = 0;
                r->state = SM_NEXT;
                continue;
            }
            JS_FreeValue(ctx, v);
            if (fr_len(ctx, list, &n) < 0 || fr_len(ctx, r->aux2, &m) < 0) {
                JS_FreeValue(ctx, list);
                return fr_fail(ctx, r);
            }
            if (n > (uint32_t)fe_sm_limits(1) - m) {
                JS_FreeValue(ctx, list);
                JS_ThrowRangeError(ctx, "Sitemap.list: more than %d URLs across the sitemapindex", fe_sm_limits(1));
                return fr_fail(ctx, r);
            }
            for (i = 0; i < n; i++) {
                JSValue it = JS_GetPropertyUint32(ctx, list, i);
                if (JS_IsException(it) || JS_DefinePropertyValueUint32(ctx, r->aux2, m + i, it, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, list);
                    return fr_fail(ctx, r);
                }
            }
            JS_FreeValue(ctx, list);
            r->kid++;
            r->state = SM_NEXT;
            continue;
        case SM_NEXT:
            if (fr_len(ctx, r->aux, &n) < 0)
                return fr_fail(ctx, r);
            if (r->kid >= n)
                return fr_settle(ctx, r, JS_DupValue(ctx, r->aux2), 0);
            v = JS_GetPropertyUint32(ctx, r->aux, r->kid);
            fr_set(ctx, &r->res, JS_ToString(ctx, v));
            JS_FreeValue(ctx, v);
            if (JS_IsException(r->res)) {
                r->res = JS_UNDEFINED;
                return fr_fail(ctx, r);
            }
            v = fr_invoke(ctx, r->fe, "get", 1, &r->res);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            return fr_await(ctx, robj, r, SM_KID, v);
        default:
            v = fr_invoke(ctx, r->fe, "get", 1, &r->url);
            if (JS_IsException(v))
                return fr_fail(ctx, r);
            return fr_await(ctx, robj, r, SM_TOP, v);
        }
    }
}

static void fr_step(JSContext* ctx, JSValueConst robj, fr_t* r, int is_reject, JSValueConst arg)
{
    JSValue pin = JS_DupValue(ctx, robj);
    if (r->kind == FR_ROBOTS)
        fr_step_robots(ctx, robj, r, is_reject, arg);
    else if (r->kind == FR_SITEMAP)
        fr_step_sitemap(ctx, robj, r, is_reject, arg);
    else
        fr_step_fetch(ctx, robj, r, is_reject, arg);
    JS_FreeValue(ctx, pin);
}

static JSValue fr_ss_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_PromiseResolveValue(ctx, JS_NewInt32(ctx, 0));
}

static JSValue fr_ss_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    if (JS_IsObject(this_val) && JS_SetPropertyStr(ctx, this_val, "closed", JS_TRUE) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue fr_skipped_stream(JSContext* ctx, JSValueConst url)
{
    JSValue o = JS_NewObject(ctx), g, sym, dis, cl;
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "status", JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "statusText", JS_NewString(ctx, ""), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ok", JS_FALSE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "headers", JS_NewObject(ctx), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "url", JS_DupValue(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "contentType", JS_NewString(ctx, ""), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "skippedByRobots", JS_TRUE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "closed", JS_FALSE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "read", JS_NewCFunction(ctx, fr_ss_read, "read", 1), JS_PROP_C_W_E);
    cl = JS_NewCFunction(ctx, fr_ss_close, "close", 0);
    JS_DefinePropertyValueStr(ctx, o, "close", JS_DupValue(ctx, cl), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "dispose", JS_DupValue(ctx, cl), JS_PROP_C_W_E);
    g = JS_GetGlobalObject(ctx);
    sym = JS_GetPropertyStr(ctx, g, "Symbol");
    dis = JS_IsException(sym) ? JS_EXCEPTION : JS_IsObject(sym) ? JS_GetPropertyStr(ctx, sym, "dispose") : JS_UNDEFINED;
    if (JS_IsException(dis)) {
        JS_FreeValue(ctx, sym);
        JS_FreeValue(ctx, g);
        JS_FreeValue(ctx, cl);
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    if (JS_IsSymbol(dis)) {
        JSAtom a = JS_ValueToAtom(ctx, dis);
        JS_DefinePropertyValue(ctx, o, a, JS_DupValue(ctx, cl), JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, dis);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, cl);
    return o;
}

static JSValue fe_fetch_entry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int stream)
{
    const char* method = stream ? "getStream" : "get";
    JSValue u, a[2], robj, t, p;
    fr_t* r;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(url)", method);
    if (!dyn_res_native(ctx, this_val, dyn_fe_class_id))
        return JS_EXCEPTION;
    a[0] = this_val;
    a[1] = JS_ToString(ctx, argv[0]);
    if (JS_IsException(a[1]))
        return a[1];
    u = en_begin(ctx, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, a[1]);
    if (JS_IsException(u))
        return u;
    a[1] = u;
    t = en_target(ctx, JS_UNDEFINED, 2, a);
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, u);
        return t;
    }
    JS_FreeValue(ctx, t);
    robj = fr_new(ctx, stream ? FR_STREAM : FR_GET, &r);
    if (JS_IsException(robj)) {
        JS_FreeValue(ctx, u);
        return robj;
    }
    r->url = u;
    r->fe = JS_DupValue(ctx, this_val);
    r->uh = JS_GetPropertyStr(ctx, this_val, "_headers");
    if (!JS_IsException(r->uh))
        r->client = stream ? en_stream_client(ctx, JS_UNDEFINED, 1, &r->fe) : JS_GetPropertyStr(ctx, this_val, "_client");
    if (JS_IsException(r->uh) || JS_IsException(r->client)) {
        r->uh = JS_IsException(r->uh) ? JS_UNDEFINED : r->uh;
        r->client = JS_IsException(r->client) ? JS_UNDEFINED : r->client;
        JS_FreeValue(ctx, robj);
        return JS_EXCEPTION;
    }
    p = JS_DupValue(ctx, r->promise);
    r->state = FS_HOP;
    fr_step(ctx, robj, r, 0, JS_UNDEFINED);
    JS_FreeValue(ctx, robj);
    return p;
}

static JSValue sm_list_entry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue u, src, robj, p;
    int own = 0;
    fr_t* r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Sitemap.list(url[, fetcherOrOpts])");
    u = JS_ToString(ctx, argv[0]);
    if (JS_IsException(u))
        return u;
    src = fe_sm_source(ctx, argc, argv, &own);
    if (JS_IsException(src)) {
        JS_FreeValue(ctx, u);
        return src;
    }
    robj = fr_new(ctx, FR_SITEMAP, &r);
    if (JS_IsException(robj)) {
        JS_FreeValue(ctx, u);
        JS_FreeValue(ctx, src);
        return robj;
    }
    r->url = u;
    r->fe = src;
    r->own = own;
    r->state = -1;
    p = JS_DupValue(ctx, r->promise);
    fr_step(ctx, robj, r, 0, JS_UNDEFINED);
    JS_FreeValue(ctx, robj);
    return p;
}

int fe_engine_install(JSContext* ctx, JSValueConst sitemap)
{
    JSValue proto = JS_GetClassProto(ctx, dyn_fe_class_id);
    int rc = -1;
    if (JS_IsObject(proto)
        && JS_DefinePropertyValueStr(ctx, proto, "get", JS_NewCFunctionMagic(ctx, fe_fetch_entry, "get", 1, JS_CFUNC_generic_magic, 0), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0
        && JS_DefinePropertyValueStr(ctx, proto, "getStream", JS_NewCFunctionMagic(ctx, fe_fetch_entry, "getStream", 1, JS_CFUNC_generic_magic, 1), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0
        && JS_DefinePropertyValueStr(ctx, sitemap, "list", JS_NewCFunction(ctx, sm_list_entry, "list", 1), JS_PROP_C_W_E) >= 0)
        rc = 0;
    JS_FreeValue(ctx, proto);
    return rc;
}
#endif
