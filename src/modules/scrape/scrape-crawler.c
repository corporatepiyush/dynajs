#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#define CR_STATE_VERSION 1
#define CR_MAX_PENDING 10000
#define CR_MAX_CONCURRENCY 16

typedef struct {
    int refs;
    int crawl_alive;
} cr_owner_t;

#define CR_FS_HOP 0
#define CR_FS_EXCHANGE 1
#define CR_SIDX_EMPTY 0xffffffffu
#define CR_FLIGHT_MAGIC 0xCAFEF11Du

static const char* const sc_crawl_keys[] = { "maxPages", "maxDepth", "sameHost",
    "linkField", "baseField",
    "canonicalField", "relField",
    "robotsField", "concurrency" };

static int cr_int_prop(JSContext* ctx, JSValueConst o, const char* k,
    int dflt, int lo, int hi, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d = 0;
    int is_int;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = dflt;
        return 0;
    }
    is_int = JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) && !isnan(d) && d != HUGE_VAL && d != -HUGE_VAL && d == floor(d);
    JS_FreeValue(ctx, v);
    if (!is_int) {
        JS_ThrowTypeError(ctx, "Crawl: %s must be an integer", k);
        return -1;
    }
    if (d < (double)lo || d > (double)hi) {
        JS_ThrowRangeError(ctx,
            "Crawl: %s must be between %d and %d (got %.17g) -- the "
            "polite part is per host, but %g parallel fetches is a stampede",
            k, lo, hi, d, d);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static JSValue fe_stats_proxy(JSContext* ctx, const char* p)
{
    const char* sep = strstr(p, "://");
    const char* auth = sep ? sep + 3 : p;
    const char* end = auth + strcspn(auth, "/?#");
    const char* at = NULL;
    const char* k;
    char* b;
    size_t pre, post;
    JSValue r;

    for (k = auth; k < end; k++)
        if (*k == '@')
            at = k;
    if (!at)
        return JS_NewString(ctx, p);
    pre = (size_t)(auth - p);
    post = strlen(at + 1);
    b = (char*)malloc(pre + 4 + post + 1);
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(b, p, pre);
    memcpy(b + pre, "***@", 4);
    memcpy(b + pre + 4, at + 1, post + 1);
    r = JS_NewString(ctx, b);
    free(b);
    return r;
}

static JSValue fe_stats_ca(JSContext* ctx, const char* ca)
{
    if (strstr(ca, "-----BEGIN") || strchr(ca, '\n'))
        return JS_NewString(ctx, "<pem-redacted>");
    return JS_NewString(ctx, ca);
}

JSValue dyn_fe_stats(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    fe_t* f = (fe_t*)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "fetched", JS_NewFloat64(ctx, f->fetched), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "skippedByRobots", JS_NewFloat64(ctx, f->skipped_robots), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "retried", JS_NewFloat64(ctx, f->retried), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "throttledMs", JS_NewFloat64(ctx, f->throttled_ms), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bytes", JS_NewFloat64(ctx, f->bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "revalidated", JS_NewFloat64(ctx, f->revalidated), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "savedBytes", JS_NewFloat64(ctx, f->saved_bytes), JS_PROP_C_W_E);
    if (f->proxy) {
        JSValue pv = fe_stats_proxy(ctx, f->proxy);
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, o, "proxy", pv, JS_PROP_C_W_E);
    } else {
        JS_DefinePropertyValueStr(ctx, o, "proxy", JS_NULL, JS_PROP_C_W_E);
    }
    if (f->ca) {
        JSValue cv = fe_stats_ca(ctx, f->ca);
        if (JS_IsException(cv)) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, o, "ca", cv, JS_PROP_C_W_E);
    } else {
        JS_DefinePropertyValueStr(ctx, o, "ca", JS_NULL, JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, o, "poolSize",
        JS_NewInt32(ctx, f->pool_size ? f->pool_size : 4), JS_PROP_C_W_E);
    return o;
}

static void sc_sb_json_str(sc_sb_t* b, const char* s)
{
    size_t i;
    sc_sb_put(b, "\"", 1);
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            sc_sb_put(b, e, 2);
        } else if (c < 0x20) {
            char e[7];
            snprintf(e, sizeof e, "\\u%04x", c);
            sc_sb_puts(b, e);
        } else {
            sc_sb_put(b, (const char*)&s[i], 1);
        }
    }
    sc_sb_put(b, "\"", 1);
}

static void cr_serialize_u64(sc_sb_t* b, const char* key, uint64_t v)
{
    char num[24];
    snprintf(num, sizeof num, ",\"%s\":%llu", key, (unsigned long long)v);
    sc_sb_puts(b, num);
}

static void cr_serialize_str(sc_sb_t* b, const char* key, const char* s)
{
    sc_sb_puts(b, ",\"");
    sc_sb_puts(b, key);
    sc_sb_puts(b, "\":");
    sc_sb_json_str(b, s ? s : "");
}

static void cr_flight_free(JSContext* ctx, cr_flight_t* fl);

static void cr_flight_free_ex(cr_flight_t* fl, JSContext* ctx,
    JSRuntime* rt);

static void cr_release(cr_t* c)
{
    cr_owner_t* own = (cr_owner_t*)c->owner;
    if (--own->refs == 0) {
        free(own);
        free(c);
    }
}

static void cr_park_fail(JSContext* ctx, JSRuntime* rt,
    JSValue* resolve, JSValue* reject,
    const char* what)
{
    if (JS_IsUndefined(*resolve) && JS_IsUndefined(*reject))
        return;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc)) {
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, what), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            exc = JS_GetException(ctx);
        }
        r = JS_Call(ctx, *reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, *resolve);
        JS_FreeValue(ctx, *reject);
    } else {
        JS_FreeValueRT(rt, *resolve);
        JS_FreeValueRT(rt, *reject);
    }
    *resolve = JS_UNDEFINED;
    *reject = JS_UNDEFINED;
}

static void cr_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    cr_t* c = (cr_t*)opaque;
    cr_owner_t* own = c ? (cr_owner_t*)c->owner : NULL;
    cr_next_wait_t* waiters;
    cr_flight_t *flights, *done;
    JSValue sp, pe;

    if (!c || !own)
        return;
    own->crawl_alive = 0;
    waiters = c->wait_head;
    flights = c->flights;
    done = c->done_head;
    sp = c->self_pending;
    pe = c->pending_err;
    c->wait_head = c->wait_tail = NULL;
    c->flights = NULL;
    c->n_flights = 0;
    c->done_head = c->done_tail = NULL;
    c->n_done = 0;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    while (waiters) {
        cr_next_wait_t* w = waiters;
        waiters = w->next;
        cr_park_fail(ctx, rt, &w->resolve, &w->reject,
            "Crawl: next() aborted at engine shutdown");
        free(w);
        cr_release(c);
    }
    while (flights) {
        cr_flight_t* fl = flights;
        flights = fl->next;
        cr_flight_free_ex(fl, ctx, rt);
    }
    while (done) {
        cr_flight_t* fl = done;
        done = fl->next;
        cr_flight_free_ex(fl, ctx, rt);
    }
    if (ctx) {
        JS_FreeValue(ctx, sp);
        JS_FreeValue(ctx, pe);
    } else {
        JS_FreeValueRT(rt, sp);
        JS_FreeValueRT(rt, pe);
    }
}

static void cr_dispose(void* native)
{
    cr_t* c = (cr_t*)native;
    cr_owner_t* own;
    size_t i;
    if (!c)
        return;
    JS_RemoveShutdownSweep(c->rt, cr_sweep, c);
    for (i = 0; i < c->qn; i++)
        free(c->q[i].url);
    free(c->q);
    for (i = 0; i < c->sn; i++)
        free(c->seen[i]);
    free(c->seen);
    free(c->sidx);
    free(c->link_field);
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    own = (cr_owner_t*)c->owner;
    if (own) {
        own->crawl_alive = 0;
        while (c->wait_head) {
            cr_next_wait_t* w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            out = JS_NewObject(c->ctx);
            JS_DefinePropertyValueStr(c->ctx, out, "done", JS_TRUE,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(c->ctx, out, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            r = JS_Call(c->ctx, w->resolve, JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(c->ctx, r);
            JS_FreeValue(c->ctx, out);
            JS_FreeValue(c->ctx, w->resolve);
            JS_FreeValue(c->ctx, w->reject);
            free(w);
            cr_release(c);
        }
        c->wait_tail = NULL;
        while (c->done_head) {
            cr_flight_t* fl = c->done_head;
            c->done_head = fl->next;
            cr_flight_free(c->ctx, fl);
        }
        c->done_tail = NULL;
        c->n_done = 0;
        c->flights = NULL;
        c->n_flights = 0;
    }
    JS_FreeValue(c->ctx, c->self_pending);
    JS_FreeValue(c->ctx, c->pending_err);
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    if (own)
        cr_release(c);
    else
        free(c);
}

static int cr_is_nofollow(const char* rel)
{
    size_t n;
    if (!rel)
        return 0;
    n = strlen(rel);
    if (n < 8)
        return 0;
    {
        const char* q = rel;
        while ((size_t)(q - rel) + 8 <= n) {
            if (!strncasecmp(q, "nofollow", 8)) {
                char a = (q > rel && q[-1] != ' ' && q[-1] != '\t' && q[-1] != ',') ? 'x' : ' ';
                char b = (q[8] && q[8] != ' ' && q[8] != '\t' && q[8] != ',') ? 'x' : ' ';
                if (a == ' ' && b == ' ')
                    return 1;
            }
            q++;
        }
    }
    return 0;
}

static void cr_norm_host(const char* host, int https, char* out, size_t cap)
{
    size_t n = strlen(host), keep = n, i;
    if (n && host[0] == '[') {
        const char* cl = strchr(host, ']');
        if (cl)
            keep = (size_t)(cl - host) + 1;
    } else {
        const char* c = strchr(host, ':');
        if (c) {
            const char* pfx = https ? ":443" : ":80";
            keep = (size_t)(c - host);
            if (strcmp(c, pfx))
                keep = n;
        }
    }
    if (keep >= cap)
        keep = cap - 1;
    for (i = 0; i < keep; i++)
        out[i] = (char)((host[i] >= 'A' && host[i] <= 'Z') ? host[i] + 32
                                                           : host[i]);
    out[keep] = 0;
}

static long cr_key(const char* url, char* out, size_t cap)
{
    const char* sp = strstr(url, "://");
    const char* path;
    size_t sl, al, keep, i, tl;
    int https;
    if (!sp)
        return -1;
    https = !strncmp(url, "https:", 6);
    sl = (size_t)(sp - url) + 3;
    path = strchr(sp + 3, '/');
    if (!path)
        path = strchr(sp + 3, '?');
    if (!path)
        path = url + strlen(url);
    al = (size_t)(path - (sp + 3));
    keep = al;
    if (al && (sp + 3)[0] == '[') {
        const char* cl = memchr(sp + 3, ']', al);
        if (cl)
            keep = (size_t)(cl - (sp + 3)) + 1;
    } else {
        const char* c = memchr(sp + 3, ':', al);
        if (c) {
            size_t pl = (size_t)(c - (sp + 3));
            size_t pn = al - pl;
            if ((!https && pn == 3 && !memcmp(c, ":80", 3)) || (https && pn == 4 && !memcmp(c, ":443", 4)))
                keep = pl;
        }
    }
    tl = (size_t)(url + strlen(url) - path);
    if (sl + keep + tl + 1 > cap)
        return -1;
    for (i = 0; i < sl; i++)
        out[i] = (char)((url[i] >= 'A' && url[i] <= 'Z') ? url[i] + 32
                                                         : url[i]);
    for (i = 0; i < keep; i++) {
        char c = (sp + 3)[i];
        out[sl + i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    memcpy(out + sl + keep, path, tl);
    out[sl + keep + tl] = 0;
    return (long)(sl + keep + tl);
}

static uint32_t cr_url_hash(const char* u)
{
    uint32_t h = 2166136261u;
    const unsigned char* p = (const unsigned char*)u;
    for (; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static int cr_seen(cr_t* c, const char* u)
{
    size_t i;
    if (c->sidx_mask) {
        uint32_t h = cr_url_hash(u);
        size_t slot = h & c->sidx_mask;
        for (;;) {
            uint32_t idx = c->sidx[slot];
            if (idx == CR_SIDX_EMPTY)
                return 0;
            if (!strcmp(c->seen[idx], u))
                return 1;
            slot = (slot + 1) & c->sidx_mask;
        }
    }
    for (i = 0; i < c->sn; i++)
        if (!strcmp(c->seen[i], u))
            return 1;
    return 0;
}

static uint32_t* cr_sidx_alloc(size_t n)
{
    uint32_t* t = (uint32_t*)malloc(n * sizeof *t);
    if (!t)
        return NULL;
    memset(t, 0xff, n * sizeof *t);
    return t;
}

static int cr_mark_seen(cr_t* c, const char* u)
{
    uint32_t h;
    size_t slot;
    if (c->sn == c->scap) {
        size_t nc = c->scap ? c->scap * 2 : 32;
        char** ns = (char**)realloc(c->seen, nc * sizeof(*ns));
        if (!ns)
            return -1;
        c->seen = ns;
        c->scap = nc;
    }
    c->seen[c->sn] = strdup(u);
    if (!c->seen[c->sn])
        return -1;
    h = cr_url_hash(u);
    if (!c->sidx) {
        c->sidx = cr_sidx_alloc(64);
        if (c->sidx)
            c->sidx_mask = 63;
    }
    if (c->sidx_mask) {
        if ((c->sn + 1) * 4 > (c->sidx_mask + 1) * 3) {
            size_t n2 = (c->sidx_mask + 1) * 2;
            uint32_t* nt = cr_sidx_alloc(n2);
            size_t i, s2;
            if (nt) {
                for (i = 0; i < c->sn; i++) {
                    s2 = cr_url_hash(c->seen[i]) & (n2 - 1);
                    while (nt[s2] != CR_SIDX_EMPTY)
                        s2 = (s2 + 1) & (n2 - 1);
                    nt[s2] = (uint32_t)i;
                }
                free(c->sidx);
                c->sidx = nt;
                c->sidx_mask = n2 - 1;
            }
        }
        slot = h & c->sidx_mask;
        while (c->sidx[slot] != CR_SIDX_EMPTY)
            slot = (slot + 1) & c->sidx_mask;
        c->sidx[slot] = (uint32_t)c->sn;
    }
    c->sn++;
    return 0;
}

static int cr_push(cr_t* c, const char* u, int depth)
{
    if (c->qn - c->qhead >= CR_MAX_PENDING)
        return 1;
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16;
        cr_item_t* nq = (cr_item_t*)realloc(c->q, nc * sizeof(*nq));
        if (!nq)
            return -1;
        c->q = nq;
        c->qcap = nc;
    }
    c->q[c->qn].url = strdup(u);
    if (!c->q[c->qn].url)
        return -1;
    c->q[c->qn].depth = depth;
    c->qn++;
    return 0;
}

static int cr_install_async_iterator(JSContext* ctx, JSValue obj);

JSValue dyn_cr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    cr_t* c;
    JSValue opt, lf = JS_UNDEFINED, obj;
    const char* s;
    int v_maxpages, v_maxdepth, v_samehost, v_conc;
    cr_owner_t* own;
    (void)nt;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Crawl(fetcher[, { maxPages, maxDepth, sameHost, linkField }])");
    c = (cr_t*)calloc(1, sizeof(*c));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    own = (cr_owner_t*)calloc(1, sizeof *own);
    if (!own) {
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    own->refs = 1;
    own->crawl_alive = 1;
    c->owner = own;
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->concurrency = 1;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    opt = (argc > 1 && JS_IsObject(argv[1])) ? JS_DupValue(ctx, argv[1])
                                             : JS_NewObject(ctx);
    if (dyn_opts_strict(ctx, opt, sc_crawl_keys, 9))
        goto fail;
    if (fe_num_prop(ctx, opt, "maxPages", 100, &v_maxpages) || fe_num_prop(ctx, opt, "maxDepth", 2, &v_maxdepth) || fe_num_prop(ctx, opt, "sameHost", 1, &v_samehost) || cr_int_prop(ctx, opt, "concurrency", 1, 1, CR_MAX_CONCURRENCY, &v_conc))
        goto fail;
    c->max_pages = v_maxpages;
    c->max_depth = v_maxdepth;
    c->same_host = v_samehost ? 1 : 0;
    c->concurrency = v_conc;

    lf = JS_GetPropertyStr(ctx, opt, "linkField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    c->link_field = strdup(s ? s : "links");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    if (!c->link_field)
        goto fail;
    lf = JS_GetPropertyStr(ctx, opt, "baseField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->base_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->base_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "canonicalField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->canonical_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->canonical_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "relField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->rel_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->rel_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "robotsField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->robots_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->robots_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    if (c->max_pages < 0)
        c->max_pages = 0;
    if (c->max_depth < 0)
        c->max_depth = 0;

    obj = dyn_res_wrap(ctx, nt, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj))
        return obj;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_sweep, c);
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    if (cr_install_async_iterator(ctx, obj) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;

fail:
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    free(c->link_field);
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    free(c->owner);
    free(c);
    return JS_EXCEPTION;
}

static int cr_install_async_iterator(JSContext* ctx, JSValue obj)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, g, "Symbol");
    JSValue ai = JS_IsException(sym) ? JS_EXCEPTION
        : JS_IsObject(sym)           ? JS_GetPropertyStr(ctx, sym, "asyncIterator")
                                     : JS_UNDEFINED;
    JSValue pages;
    JSAtom aatom;
    int rc = 0;
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
    if (JS_IsException(ai))
        return -1;
    if (!JS_IsSymbol(ai)) {
        JS_FreeValue(ctx, ai);
        return 0;
    }
    aatom = JS_ValueToAtom(ctx, ai);
    JS_FreeValue(ctx, ai);
    if (aatom == JS_ATOM_NULL)
        return -1;
    pages = JS_GetPropertyStr(ctx, obj, "pages");
    if (JS_IsException(pages))
        rc = -1;
    else if (JS_IsFunction(ctx, pages))
        rc = JS_DefinePropertyValue(ctx, obj, aatom, pages, JS_PROP_C_W_E) < 0 ? -1 : 0;
    else
        JS_FreeValue(ctx, pages);
    JS_FreeAtom(ctx, aatom);
    return rc;
}

JSValue dyn_cr_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    const char* seed;
    const char* path;
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "start(seed[, extractor])");
    seed = JS_ToCString(ctx, argv[0]);
    if (!seed)
        return JS_EXCEPTION;
    if (strlen(seed) + 1 > 1024) {
        JS_FreeCString(ctx, seed);
        return JS_ThrowRangeError(ctx, "Crawl: seed url is too long");
    }
    {
        char sbuf[1024];
        int https;
        memcpy(sbuf, seed, strlen(seed) + 1);
        JS_FreeCString(ctx, seed);
        {
            char* frag = strchr(sbuf, '#');
            if (frag)
                *frag = 0;
        }
        https = fe_split(sbuf, c->seed_host, sizeof c->seed_host, &path);
        if (https < 0)
            return JS_ThrowTypeError(ctx, "Crawl: seed must be an http(s) url");
        {
            char nh[300];
            cr_norm_host(c->seed_host, https, nh, sizeof nh);
            memcpy(c->seed_host, nh, strlen(nh) + 1);
        }
        if (cr_push(c, sbuf, 0) < 0)
            return JS_ThrowOutOfMemory(ctx);
        {
            char key[1024];
            if (cr_key(sbuf, key, sizeof key) >= 0) {
                if (cr_mark_seen(c, key) < 0)
                    return JS_ThrowOutOfMemory(ctx);
            } else if (cr_mark_seen(c, sbuf) < 0) {
                return JS_ThrowOutOfMemory(ctx);
            }
        }
    }
    c->started = 1;
    if (argc > 1 && JS_IsObject(argv[1]))
        JS_DefinePropertyValueStr(ctx, this_val, "_extractor",
            JS_DupValue(ctx, argv[1]), 0);
    if (argc > 2 && JS_IsFunction(ctx, argv[2]))
        JS_DefinePropertyValueStr(ctx, this_val, "_parse",
            JS_DupValue(ctx, argv[2]), 0);
    return JS_DupValue(ctx, this_val);
}

static uint32_t cr_len(JSContext* ctx, JSValueConst arr)
{
    uint32_t n;
    if (dyn_js_len_u32(ctx, arr, &n))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return n;
}

static JSValue cr_make_page(JSContext* ctx, cr_t* c, JSValueConst this_val,
    const char* url, int depth, JSValue res);

static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c);

JSValue dyn_cr_next(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    return dyn_cr_next_async(ctx, this_val, c);
}

static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c);

static JSValue cr_make_page(JSContext* ctx, cr_t* c, JSValueConst this_val,
    const char* url, int depth, JSValue res)
{
    JSValue extractor, page;
    int page_status, page_nofollow = 0, gx = 0;

    page = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, page, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, page, "depth", JS_NewInt32(ctx, depth), JS_PROP_C_W_E);
    page_status = 0;
    {
        JSValue st = JS_GetPropertyStr(ctx, res, "status");
        if (JS_IsException(st) || JS_ToInt32(ctx, &page_status, st)) {
            JS_FreeValue(ctx, st);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, page, "status", st, JS_PROP_C_W_E);
    }

    {
        JSValue rd = JS_GetPropertyStr(ctx, res, "robotsDirectives");
        if (JS_IsException(rd)) {
            JS_FreeValue(ctx, rd);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
        if (JS_IsArray(ctx, rd))
            JS_DefinePropertyValueStr(ctx, page, "robots",
                JS_DupValue(ctx, rd), JS_PROP_C_W_E);
        JS_FreeValue(ctx, rd);
    }

    extractor = JS_GetPropertyStr(ctx, this_val, "_extractor");
    if (JS_IsException(extractor)) {
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, page);
        return JS_EXCEPTION;
    }
    if (JS_IsObject(extractor)) {
        JSValue raw = JS_GetPropertyStr(ctx, res, "body");
        JSValue parse = JS_IsException(raw) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, this_val, "_parse");
        JSValue body, a[2], ev;
        JSAtom m;
        if (JS_IsException(raw) || JS_IsException(parse)) {
            JS_FreeValue(ctx, raw);
            JS_FreeValue(ctx, extractor);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
        if (JS_IsFunction(ctx, parse)) {
            JSValueConst pa[1];
            pa[0] = raw;
            body = JS_Call(ctx, parse, JS_UNDEFINED, 1, pa);
            JS_FreeValue(ctx, raw);
            if (JS_IsException(body)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                body = JS_UNDEFINED;
            }
        } else {
            body = raw;
        }
        JS_FreeValue(ctx, parse);
        m = JS_NewAtom(ctx, "run");
        a[0] = body;
        a[1] = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, a[1], "base", JS_NewString(ctx, url), JS_PROP_C_W_E);
        ev = JS_Invoke(ctx, extractor, m, 2, (JSValueConst*)a);
        JS_FreeAtom(ctx, m);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, body);
        if (!JS_IsException(ev) && JS_IsObject(ev)) {
            JSValue val = JS_GetPropertyStr(ctx, ev, "value");
            if (JS_IsException(val)) {
                gx = 1;
                val = JS_UNDEFINED;
            } else {
                JS_DefinePropertyValueStr(ctx, page, "value", JS_DupValue(ctx, val), JS_PROP_C_W_E);
            }
            if (c->robots_field && JS_IsObject(val)) {
                JSValue rv = JS_GetPropertyStr(ctx, val, c->robots_field);
                if (JS_IsException(rv)) {
                    gx = 1;
                    rv = JS_UNDEFINED;
                }
                if (JS_IsString(rv)) {
                    const char* rs = JS_ToCString(ctx, rv);
                    if (rs) {
                        char* dirs = fe_parse_robots_tag(rs, NULL);
                        JS_FreeCString(ctx, rs);
                        if (dirs) {
                            size_t k = 0;
                            JSValue rb = JS_GetPropertyStr(ctx, page, "robots");
                            if (JS_IsException(rb))
                                gx = 1;
                            else if (!JS_IsArray(ctx, rb)) {
                                JS_FreeValue(ctx, rb);
                                rb = JS_NewArray(ctx);
                                if (!JS_IsException(rb))
                                    JS_DefinePropertyValueStr(ctx, page, "robots",
                                        JS_DupValue(ctx, rb),
                                        JS_PROP_C_W_E);
                            }
                            while (dirs[k] && !JS_IsException(rb)) {
                                size_t b = k;
                                uint32_t m2 = 0, j;
                                int dup2 = 0;
                                while (dirs[k] && dirs[k] != '\n')
                                    k++;
                                m2 = cr_len(ctx, rb);
                                for (j = 0; j < m2 && !dup2; j++) {
                                    JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                                    const char* ts = JS_ToCString(ctx, t);
                                    if (!ts)
                                        JS_FreeValue(ctx, JS_GetException(ctx));
                                    if (ts) {
                                        if (k - b == strlen(ts) && !strncasecmp(ts, dirs + b, k - b))
                                            dup2 = 1;
                                        JS_FreeCString(ctx, ts);
                                    }
                                    JS_FreeValue(ctx, t);
                                }
                                if (!dup2)
                                    JS_DefinePropertyValueUint32(ctx, rb, m2,
                                        JS_NewStringLen(ctx, dirs + b, k - b), JS_PROP_C_W_E);
                                if (dirs[k])
                                    k++;
                            }
                            JS_FreeValue(ctx, rb);
                            free(dirs);
                        }
                    }
                }
                JS_FreeValue(ctx, rv);
            }
            {
                JSValue rb = gx ? JS_UNDEFINED : JS_GetPropertyStr(ctx, page, "robots");
                if (JS_IsException(rb)) {
                    gx = 1;
                    rb = JS_UNDEFINED;
                }
                if (JS_IsArray(ctx, rb)) {
                    uint32_t m2 = cr_len(ctx, rb), j;
                    for (j = 0; j < m2 && !page_nofollow; j++) {
                        JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                        const char* ts = JS_ToCString(ctx, t);
                        if (!ts)
                            JS_FreeValue(ctx, JS_GetException(ctx));
                        if (ts) {
                            if (!strcasecmp(ts, "nofollow"))
                                page_nofollow = 1;
                            JS_FreeCString(ctx, ts);
                        }
                        JS_FreeValue(ctx, t);
                    }
                }
                JS_FreeValue(ctx, rb);
            }
            if (page_status >= 200 && page_status < 300 && depth < c->max_depth && JS_IsObject(val) && !page_nofollow && !gx) {
                const char *ckey = url, *ccs = NULL, *canon_s = NULL;
                JSValue cval = JS_UNDEFINED, cval2 = JS_UNDEFINED;
                int dup = 0;
                if (c->canonical_field) {
                    cval = JS_GetPropertyStr(ctx, val, c->canonical_field);
                    if (JS_IsException(cval)) {
                        gx = 1;
                        cval = JS_UNDEFINED;
                    }
                    if (JS_IsString(cval)) {
                        ccs = JS_ToCString(ctx, cval);
                        if (ccs && (!strncasecmp(ccs, "http://", 7) || !strncasecmp(ccs, "https://", 8)) && strcmp(ccs, url))
                            ckey = ccs;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                            ? cr_seen(c, nk)
                            : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                if (!gx && ckey == url) {
                    cval2 = JS_GetPropertyStr(ctx, res, "canonicalUrl");
                    if (JS_IsException(cval2)) {
                        gx = 1;
                        cval2 = JS_UNDEFINED;
                    }
                    if (JS_IsString(cval2)) {
                        canon_s = JS_ToCString(ctx, cval2);
                        if (canon_s && strcmp(canon_s, url))
                            ckey = canon_s;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                            ? cr_seen(c, nk)
                            : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                if (!dup && !gx) {
                    char bbuf[1024];
                    const char* ebase = url;
                    JSValue bval = JS_UNDEFINED;
                    if (c->base_field) {
                        bval = JS_GetPropertyStr(ctx, val, c->base_field);
                        if (JS_IsException(bval)) {
                            gx = 1;
                            bval = JS_UNDEFINED;
                        }
                        if (JS_IsString(bval)) {
                            const char* bsv = JS_ToCString(ctx, bval);
                            if (bsv) {
                                if (!strncasecmp(bsv, "http://", 7) || !strncasecmp(bsv, "https://", 8))
                                    ebase = bsv;
                                else if (fe_resolve(url, bsv,
                                             bbuf, sizeof bbuf)
                                    > 0)
                                    ebase = bbuf;
                                JS_FreeCString(ctx, bsv);
                            }
                        }
                        JS_FreeValue(ctx, bval);
                    }
                    JSValue links = gx ? JS_UNDEFINED : JS_GetPropertyStr(ctx, val, c->link_field);
                    JSValue rels = JS_UNDEFINED;
                    int have_rels = 0;
                    if (JS_IsException(links)) {
                        gx = 1;
                        links = JS_UNDEFINED;
                    }
                    if (c->rel_field && !gx) {
                        rels = JS_GetPropertyStr(ctx, val, c->rel_field);
                        if (JS_IsException(rels)) {
                            gx = 1;
                            rels = JS_UNDEFINED;
                        }
                        have_rels = JS_IsArray(ctx, rels);
                    }
                    if (!gx && JS_IsArray(ctx, links)) {
                        uint32_t i, n = cr_len(ctx, links);
                        for (i = 0; i < n; i++) {
                            JSValue e = JS_GetPropertyUint32(ctx, links, i);
                            const char* ls = JS_ToCString(ctx, e);
                            if (!ls)
                                JS_FreeValue(ctx, JS_GetException(ctx));
                            if (ls) {
                                char lh[300], resolved[1024];
                                const char* lp;
                                long rl;
                                if (have_rels) {
                                    JSValue rv = JS_GetPropertyUint32(ctx, rels, i);
                                    if (JS_IsException(rv)) {
                                        gx = 1;
                                        JS_FreeCString(ctx, ls);
                                        JS_FreeValue(ctx, e);
                                        break;
                                    }
                                    if (JS_IsString(rv)) {
                                        const char* rs = JS_ToCString(ctx, rv);
                                        if (rs && cr_is_nofollow(rs)) {
                                            JS_FreeCString(ctx, rs);
                                            JS_FreeValue(ctx, rv);
                                            JS_FreeCString(ctx, ls);
                                            JS_FreeValue(ctx, e);
                                            continue;
                                        }
                                        if (rs)
                                            JS_FreeCString(ctx, rs);
                                    }
                                    JS_FreeValue(ctx, rv);
                                }
                                if (!ls[0] || ls[0] == '#' || !strncasecmp(ls, "javascript:", 11) || !strncasecmp(ls, "mailto:", 7) || !strncasecmp(ls, "tel:", 4) || !strncasecmp(ls, "data:", 5)) {
                                    JS_FreeCString(ctx, ls);
                                    JS_FreeValue(ctx, e);
                                    continue;
                                }
                                if (fe_resolve(ebase, ls,
                                        resolved, sizeof resolved)
                                    > 0)
                                    rl = (long)strlen(resolved);
                                else
                                    rl = -1;
                                JS_FreeCString(ctx, ls);
                                if (rl <= 0 || fe_split(resolved, lh, sizeof lh, &lp) < 0 || (strncmp(resolved, "http://", 7) && strncmp(resolved, "https://", 8))) {
                                    JS_FreeValue(ctx, e);
                                    continue;
                                }
                                if (c->same_host) {
                                    char nh[300];
                                    cr_norm_host(lh, resolved[4] == 's',
                                        nh, sizeof nh);
                                    if (strcmp(nh, c->seed_host)) {
                                        JS_FreeValue(ctx, e);
                                        continue;
                                    }
                                }
                                {
                                    char key[1024];
                                    if (cr_key(resolved, key, sizeof key) < 0) {
                                        JS_FreeValue(ctx, e);
                                        continue;
                                    }
                                    if (!cr_seen(c, key)) {
                                        int rc = cr_mark_seen(c, key);
                                        if (rc == 0) {
                                            rc = cr_push(c, resolved, depth + 1);
                                            if (rc > 0)
                                                c->dropped++;
                                        }
                                    }
                                }
                            }
                            JS_FreeValue(ctx, e);
                        }
                    }
                    JS_FreeValue(ctx, links);
                    JS_FreeValue(ctx, rels);
                }
                if (ccs)
                    JS_FreeCString(ctx, ccs);
                JS_FreeValue(ctx, cval);
                if (canon_s)
                    JS_FreeCString(ctx, canon_s);
                JS_FreeValue(ctx, cval2);
            }
            JS_FreeValue(ctx, val);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, ev);
    }
    JS_FreeValue(ctx, extractor);
    JS_FreeValue(ctx, res);
    if (gx) {
        JS_FreeValue(ctx, page);
        return JS_EXCEPTION;
    }

    c->emitted++;
    return page;
}

static void cr_notify(cr_t* c);

static void cr_dispatch(cr_t* c, JSValueConst this_val);

static void cr_flight_run(cr_flight_t* fl);

static void cr_flight_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void cr_flight_destroy(cr_flight_t* fl, JSRuntime* rt)
{
    cr_t* c = fl->c;
    int claim = !fl->claim_released;
    JS_RemoveShutdownSweep(rt, cr_flight_sweep, fl);
    JS_ShutdownUndeferFree(rt, fl);
    fl->magic = 0;
    free(fl);
    if (claim)
        cr_release(c);
}

static void cr_flight_release_pins(cr_flight_t* fl, JSContext* ctx,
    JSRuntime* rt)
{
    JSValue* pins = &fl->crawl;
    int i;
    for (i = 0; i < 7; i++) {
        if (ctx)
            JS_FreeValue(ctx, pins[i]);
        else
            JS_FreeValueRT(rt, pins[i]);
        pins[i] = JS_UNDEFINED;
    }
}

static void cr_flight_free_ex(cr_flight_t* fl, JSContext* ctx, JSRuntime* rt)
{
    if (!fl || fl->pins_released)
        return;
    fl->pins_released = 1;
    if (!rt && ctx)
        rt = JS_GetRuntime(ctx);
    JS_RemoveShutdownSweep(rt, cr_flight_sweep, fl);
    cr_flight_release_pins(fl, ctx, rt);
    if (fl->park_claims > 0) {
        fl->graveyard = 1;
        if (!fl->claim_released) {
            fl->claim_released = 1;
            cr_release(fl->c);
        }
        JS_ShutdownDeferFree(rt, fl);
        return;
    }
    cr_flight_destroy(fl, rt);
}

static void cr_flight_free(JSContext* ctx, cr_flight_t* fl)
{
    cr_flight_free_ex(fl, ctx, NULL);
}

static void cr_flight_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    cr_flight_t* fl = (cr_flight_t*)opaque;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return;
    cr_flight_free_ex(fl, ctx, rt);
}

static JSValue cr_flight_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data);

static JSValue cr_flight_park(JSContext* ctx, cr_flight_t* fl, JSValue pr)
{
    JSValue funcs[2], promise, pthen, dptr, onres, onrej, tr, upthen;
    JSValueConst thenargs[2];

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, pr);
        return JS_EXCEPTION;
    }
    dptr = JS_NewInt64(ctx, (int64_t)(intptr_t)fl);
    onres = JS_NewCFunctionData(ctx, cr_flight_settle, 1, 0, 1, &dptr);
    onrej = JS_NewCFunctionData(ctx, cr_flight_settle, 1, 1, 1, &dptr);
    JS_FreeValue(ctx, dptr);
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    thenargs[0] = onres;
    thenargs[1] = onrej;
    tr = JS_IsException(pthen) ? JS_EXCEPTION : JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onres);
    JS_FreeValue(ctx, onrej);
    if (JS_IsException(tr)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, pr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, tr);
    upthen = JS_GetPropertyStr(ctx, pr, "then");
    if (JS_IsException(upthen)) {
        JSValue exc = JS_UNDEFINED;
        if (JS_HasUncatchableException(ctx))
            dyn_exc_drop(ctx);
        else
            exc = JS_GetException(ctx);
        tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
        JS_FreeValue(ctx, tr);
        JS_FreeValue(ctx, exc);
    } else if (JS_IsFunction(ctx, upthen)) {
        tr = JS_Call(ctx, upthen, pr, 2, (JSValueConst*)funcs);
        if (JS_IsException(tr)) {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, tr);
            tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
            JS_FreeValue(ctx, tr);
            JS_FreeValue(ctx, exc);
        } else {
            JS_FreeValue(ctx, tr);
        }
    } else {
        JSValue undef = JS_UNDEFINED;
        JS_FreeValue(ctx, JS_GetException(ctx));
        tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &undef);
        JS_FreeValue(ctx, tr);
    }
    JS_FreeValue(ctx, upthen);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, promise);
    fl->pending = pr;
    fl->park_claims++;
    return JS_UNDEFINED;
}


static void cr_notify(cr_t* c);

static void cr_flight_finish(cr_flight_t* fl, int failed)
{
    JSContext* ctx = fl->ctx;
    cr_t* c = fl->c;
    cr_owner_t* own = (cr_owner_t*)c->owner;
    JSValue page = JS_UNDEFINED;

    if (fl->finished)
        return;
    fl->finished = 1;
    if (failed) {
        if (JS_IsException(fl->res)) {
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, fl->err);
                fl->err = JS_GetException(ctx);
            } else if (JS_IsUndefined(fl->err)) {
                fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
                fl->err = JS_GetException(ctx);
            }
            fl->res = JS_UNDEFINED;
        } else if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);
        } else if (JS_IsUndefined(fl->err)) {
            fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
            fl->err = JS_GetException(ctx);
        }
    }
    {
        cr_flight_t** pp = &c->flights;
        while (*pp && *pp != fl)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = fl->next;
            c->n_flights--;
        }
    }
    if (!own->crawl_alive) {
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    if (!failed) {
        page = cr_make_page(ctx, c, fl->crawl, fl->cur, fl->depth,
            fl->res);
        fl->res = JS_UNDEFINED;
    }
    if (failed || JS_IsException(page)) {
        if (!failed) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);
        }
        JS_FreeValue(ctx, page);
    } else {
        fl->res = page;
    }
    if (!own->crawl_alive) {
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    fl->next = NULL;
    if (c->done_tail)
        c->done_tail->next = fl;
    else
        c->done_head = fl;
    c->done_tail = fl;
    c->n_done++;
    {
        cr_notify(c);
        cr_dispatch(c, c->self_pending);
        if (c->n_flights == 0 && !JS_IsUndefined(c->self_pending)) {
            JS_FreeValue(ctx, c->self_pending);
            c->self_pending = JS_UNDEFINED;
        }
    }
}

static void cr_flight_fe_gone(cr_flight_t* fl)
{
    JSContext* ctx = fl->ctx;
    JS_FreeValue(ctx, fl->err);
    fl->err = JS_ThrowTypeError(ctx,
        "Crawl: the fetcher was closed while the crawl was in flight");
    fl->err = JS_GetException(ctx);
    cr_flight_finish(fl, 1);
}

static int cr_fe_closed(JSContext* ctx, JSValueConst fv)
{
    JSClassID id = (JSClassID)0;
    JS_GetAnyOpaque(fv, &id);
    return id == dyn_fe_class_id && !fe_live(ctx, fv);
}

static JSValue cr_flight_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    int64_t p = 0;
    cr_flight_t* fl;
    (void)this_val;
    if (JS_ToInt64(ctx, &p, data[0]))
        return JS_EXCEPTION;
    fl = (cr_flight_t*)(intptr_t)p;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return JS_UNDEFINED;
    if (fl->park_claims > 0)
        fl->park_claims--;
    if (fl->graveyard) {
        if (fl->park_claims == 0)
            cr_flight_destroy(fl, JS_GetRuntime(ctx));
        return JS_UNDEFINED;
    }
    if (fl->finished)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, fl->pending);
    fl->pending = JS_UNDEFINED;
    if (magic && cr_fe_closed(ctx, fl->fetcher)) {
        cr_flight_fe_gone(fl);
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, fl->res);
    fl->res = JS_UNDEFINED;
    if (magic) {
        JS_FreeValue(ctx, fl->err);
        fl->err = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
        cr_flight_finish(fl, 1);
        return JS_UNDEFINED;
    }
    fl->res = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    if (JS_IsObject(fl->res)) {
        JSValue uv = JS_GetPropertyStr(ctx, fl->res, "url");
        const char* us = !JS_IsException(uv) && JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
        if (us && strlen(us) < sizeof fl->cur)
            snprintf(fl->cur, sizeof fl->cur, "%s", us);
        if (us)
            JS_FreeCString(ctx, us);
        JS_FreeValue(ctx, uv);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, fl->res);
            fl->res = JS_UNDEFINED;
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_UNDEFINED;
            if (JS_HasUncatchableException(ctx))
                dyn_exc_drop(ctx);
            else
                fl->err = JS_GetException(ctx);
            cr_flight_finish(fl, 1);
            return JS_UNDEFINED;
        }
    }
    cr_flight_finish(fl, 0);
    return JS_UNDEFINED;
}

static void cr_flight_run(cr_flight_t* fl)
{
    JSContext* ctx = fl->ctx;
    JSValue url, pr, th;
    JSAtom m;

    if (cr_fe_closed(ctx, fl->fetcher)) {
        cr_flight_fe_gone(fl);
        return;
    }
    url = JS_NewString(ctx, fl->cur);
    if (JS_IsException(url)) {
        cr_flight_finish(fl, 1);
        return;
    }
    m = JS_NewAtom(ctx, "get");
    pr = JS_Invoke(ctx, fl->fetcher, m, 1, (JSValueConst*)&url);
    JS_FreeAtom(ctx, m);
    JS_FreeValue(ctx, url);
    if (JS_IsException(pr)) {
        cr_flight_finish(fl, 1);
        return;
    }
    th = JS_IsObject(pr) ? JS_GetPropertyStr(ctx, pr, "then") : JS_UNDEFINED;
    if (JS_IsException(th)) {
        JS_FreeValue(ctx, pr);
        cr_flight_finish(fl, 1);
        return;
    }
    if (!JS_IsFunction(ctx, th)) {
        JSValue funcs[2], np, r;
        np = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(np)) {
            JS_FreeValue(ctx, th);
            JS_FreeValue(ctx, pr);
            cr_flight_finish(fl, 1);
            return;
        }
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&pr);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, pr);
        pr = np;
    }
    JS_FreeValue(ctx, th);
    fl->state = CR_FS_EXCHANGE;
    if (JS_IsException(cr_flight_park(ctx, fl, pr)))
        cr_flight_finish(fl, 1);
}

static void cr_notify(cr_t* c)
{
    JSContext* ctx = c->ctx;
    while (c->wait_head && c->done_head) {
        cr_next_wait_t* w = c->wait_head;
        cr_flight_t* fl = c->done_head;
        JSValue out, r;
        c->wait_head = w->next;
        if (!c->wait_head)
            c->wait_tail = NULL;
        c->done_head = fl->next;
        if (!c->done_head)
            c->done_tail = NULL;
        c->n_done--;
        if (!JS_IsUndefined(fl->err)) {
            JSValueConst ea[1] = { fl->err };
            r = JS_Call(ctx, w->reject, JS_UNDEFINED, 1, ea);
        } else {
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "value", fl->res,
                JS_PROP_C_W_E);
            fl->res = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, w->resolve);
        JS_FreeValue(ctx, w->reject);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        free(w);
        cr_release(c);
    }
    if (!c->done_head && c->n_flights == 0 && (!c->started || c->qhead >= c->qn || c->emitted >= c->max_pages)) {
        while (c->wait_head) {
            cr_next_wait_t* w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            if (!c->wait_head)
                c->wait_tail = NULL;
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, w->resolve);
            JS_FreeValue(ctx, w->reject);
            free(w);
            cr_release(c);
        }
        c->wait_tail = NULL;
    }
}

static void cr_dispatch(cr_t* c, JSValueConst this_val)
{
    JSContext* ctx = c->ctx;
    cr_owner_t* own = (cr_owner_t*)c->owner;
    size_t waiters = 0;
    cr_next_wait_t* w;

    for (w = c->wait_head; w; w = w->next)
        waiters++;
    while (own->crawl_alive && c->n_flights < (size_t)c->concurrency && c->n_flights + c->n_done < waiters + (size_t)c->concurrency - 1 && c->qhead < c->qn && (size_t)c->emitted + c->n_flights + c->n_done < (size_t)c->max_pages) {
        cr_flight_t* fl = (cr_flight_t*)calloc(1, sizeof *fl);
        char* u;
        if (!fl)
            return;
        own->refs++;
        fl->ctx = ctx;
        fl->c = c;
        fl->crawl = JS_DupValue(ctx, this_val);
        fl->res = JS_UNDEFINED;
        fl->pending = JS_UNDEFINED;
        fl->err = JS_UNDEFINED;
        fl->fetcher = JS_UNDEFINED;
        fl->client = JS_UNDEFINED;
        fl->uh = JS_UNDEFINED;
        if (c->n_flights == 0 && JS_IsUndefined(c->self_pending))
            c->self_pending = JS_DupValue(ctx, this_val);
        fl->state = CR_FS_HOP;
        fl->magic = CR_FLIGHT_MAGIC;
        JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_flight_sweep, fl);
        fl->seq = ++c->flight_seq;
        u = c->q[c->qhead].url;
        fl->depth = c->q[c->qhead].depth;
        c->q[c->qhead].url = NULL;
        c->qhead++;
        snprintf(fl->cur, sizeof fl->cur, "%s", u);
        free(u);
        fl->next = c->flights;
        c->flights = fl;
        c->n_flights++;
        fl->fetcher = JS_GetPropertyStr(ctx, this_val, "_fetcher");
        if (!JS_IsException(fl->fetcher))
            fl->client = JS_GetPropertyStr(ctx, fl->fetcher, "_client");
        if (!JS_IsException(fl->fetcher) && !JS_IsException(fl->client))
            fl->uh = JS_GetPropertyStr(ctx, fl->fetcher, "_headers");
        if (JS_IsException(fl->fetcher) || JS_IsException(fl->client)
            || JS_IsException(fl->uh)) {
            cr_flight_finish(fl, 1);
            if (!own->crawl_alive)
                return;
            continue;
        }
        if (!own->crawl_alive) {
            cr_flight_finish(fl, 1);
            return;
        }
        cr_flight_run(fl);
        if (!own->crawl_alive)
            return;
    }
}

static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c)
{
    JSValue funcs[2], promise;

    if (!JS_IsUndefined(c->pending_err)) {
        JSValueConst ea[1] = { c->pending_err };
        JSValue r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx, c->pending_err);
            c->pending_err = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, ea);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, c->pending_err);
        c->pending_err = JS_UNDEFINED;
        return promise;
    }
    if (c->done_head) {
        cr_flight_t* fl = c->done_head;
        JSValue out, r;
        c->done_head = fl->next;
        if (!c->done_head)
            c->done_tail = NULL;
        c->n_done--;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise)) {
            if (!JS_IsUndefined(fl->err))
                JS_FreeValue(ctx, JS_GetException(ctx));
            cr_flight_free(ctx, fl);
            return promise;
        }
        if (!JS_IsUndefined(fl->err)) {
            JSValueConst ea[1] = { fl->err };
            r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, ea);
        } else {
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "value", fl->res,
                JS_PROP_C_W_E);
            fl->res = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        cr_flight_free(ctx, fl);
        return promise;
    }
    if (!c->started || (c->qhead >= c->qn && c->n_flights == 0) || c->emitted >= c->max_pages) {
        JSValue out, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        out = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&out);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (!((cr_owner_t*)c->owner)->crawl_alive) {
        JSValue exc, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        exc = JS_ThrowTypeError(ctx, "Crawl: next() on a closed crawl");
        exc = JS_GetException(ctx);
        r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    {
        cr_next_wait_t* w = (cr_next_wait_t*)malloc(sizeof *w);
        cr_owner_t* own = (cr_owner_t*)c->owner;
        if (!w) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        own->refs++;
        w->resolve = funcs[0];
        w->reject = funcs[1];
        w->next = NULL;
        if (c->wait_tail)
            c->wait_tail->next = w;
        else
            c->wait_head = w;
        c->wait_tail = w;
    }
    cr_dispatch(c, this_val);
    return promise;
}

JSValue dyn_cr_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cr_serialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    sc_sb_t b;
    JSValue out;
    size_t i;
    (void)argc;
    (void)argv;

    if (!c)
        return JS_EXCEPTION;
    if (!c->started)
        return JS_ThrowTypeError(ctx,
            "Crawl.serialize: the crawl has no state yet -- start(seed) first");
    sc_sb_init(&b);
    sc_sb_puts(&b, "{\"v\":");
    {
        char num[16];
        snprintf(num, sizeof num, "%d", CR_STATE_VERSION);
        sc_sb_puts(&b, num);
    }
    cr_serialize_u64(&b, "maxPages", (uint64_t)c->max_pages);
    cr_serialize_u64(&b, "maxDepth", (uint64_t)c->max_depth);
    cr_serialize_u64(&b, "sameHost", (uint64_t)c->same_host);
    cr_serialize_u64(&b, "concurrency", (uint64_t)c->concurrency);
    cr_serialize_u64(&b, "emitted", (uint64_t)c->emitted);
    cr_serialize_u64(&b, "dropped", (uint64_t)c->dropped);
    cr_serialize_str(&b, "seedHost", c->seed_host);
    cr_serialize_str(&b, "linkField", c->link_field);
    cr_serialize_str(&b, "baseField", c->base_field);
    cr_serialize_str(&b, "canonicalField", c->canonical_field);
    cr_serialize_str(&b, "relField", c->rel_field);
    cr_serialize_str(&b, "robotsField", c->robots_field);
    sc_sb_puts(&b, ",\"q\":[");
    for (i = c->qhead; i < c->qn; i++) {
        char d[16];
        if (i > c->qhead)
            sc_sb_put(&b, ",", 1);
        sc_sb_put(&b, "[", 1);
        sc_sb_json_str(&b, c->q[i].url);
        snprintf(d, sizeof d, ",%d]", c->q[i].depth);
        sc_sb_puts(&b, d);
    }
    sc_sb_puts(&b, "],\"seen\":[");
    for (i = 0; i < c->sn; i++) {
        if (i)
            sc_sb_put(&b, ",", 1);
        sc_sb_json_str(&b, c->seen[i]);
    }
    sc_sb_puts(&b, "]}");
    if (b.oom) {
        sc_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    sc_sb_free(&b);
    return out;
}

static int cr_state_str(JSContext* ctx, JSValueConst o, const char* key,
    char** out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    const char* s;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return -2;
    }
    s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    JS_FreeValue(ctx, v);
    if (!s) {
        JS_ThrowTypeError(ctx, "Crawl.resume: \"%s\" must be a string", key);
        return -1;
    }
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

JSValue dyn_cr_resume(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    cr_t* c;
    JSValue st, obj;
    JSValueConst nt = JS_UNDEFINED;
    int64_t v64;
    uint32_t i, nq = 0, nsn = 0;
    int rc;
    (void)this_val;

    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Crawl.resume(fetcher, state[, extractor[, parse]]): the state is "
            "the string serialize() returned");
    {
        size_t slen;
        const char* sj = JS_ToCStringLen(ctx, &slen, argv[1]);
        if (!sj)
            return JS_EXCEPTION;
        st = JS_ParseJSON(ctx, sj, slen, "<crawl-state>");
        JS_FreeCString(ctx, sj);
    }
    if (JS_IsException(st))
        return JS_ThrowTypeError(ctx,
            "Crawl.resume: state is not valid JSON -- refusing to half-restore");
    if (!JS_IsObject(st)) {
        JS_FreeValue(ctx, st);
        return JS_ThrowTypeError(ctx, "Crawl.resume: state must be a JSON object");
    }

    c = (cr_t*)calloc(1, sizeof(*c));
    if (!c) {
        JS_FreeValue(ctx, st);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->concurrency = 1;

#define CR_FAIL(msg)                                  \
    do {                                              \
        JS_ThrowTypeError(ctx, "Crawl.resume: " msg); \
        goto fail;                                    \
    } while (0)

    {
        JSValue v = JS_GetPropertyStr(ctx, st, "v");
        int32_t vv = 0;
        int bad = JS_IsException(v) || JS_ToInt32(ctx, &vv, v);
        JS_FreeValue(ctx, v);
        if (bad)
            goto fail_pending;
        if (vv != CR_STATE_VERSION)
            CR_FAIL("state version mismatch (wrong runtime or corrupt file)");
    }
    {
        int v_mp, v_md, v_sh, v_cc;
        if (fe_num_prop(ctx, st, "maxPages", 100, &v_mp) || fe_num_prop(ctx, st, "maxDepth", 2, &v_md) || fe_num_prop(ctx, st, "sameHost", 1, &v_sh) || fe_num_prop(ctx, st, "concurrency", 1, &v_cc))
            goto fail_pending;
        if (v_mp < 0 || v_md < 0 || v_cc < 1 || v_cc > CR_MAX_CONCURRENCY)
            CR_FAIL("state bounds out of range");
        c->max_pages = v_mp;
        c->max_depth = v_md;
        c->same_host = v_sh ? 1 : 0;
        c->concurrency = v_cc;
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "emitted");
        int bad = JS_IsException(v) || JS_ToInt64(ctx, &v64, v);
        JS_FreeValue(ctx, v);
        if (bad)
            goto fail_pending;
        if (v64 < 0 || v64 > (int64_t)c->max_pages)
            CR_FAIL("emitted counter out of range");
        c->emitted = (int)v64;
        v = JS_GetPropertyStr(ctx, st, "dropped");
        bad = JS_IsException(v) || JS_ToInt64(ctx, &v64, v);
        JS_FreeValue(ctx, v);
        if (bad)
            goto fail_pending;
        if (v64 < 0)
            CR_FAIL("dropped counter out of range");
        c->dropped = (size_t)v64;
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "seedHost");
        const char* s;
        if (JS_IsException(v))
            goto fail_pending;
        s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        if (!s) {
            JS_FreeValue(ctx, v);
            CR_FAIL("state has no seedHost (never started?)");
        }
        {
            size_t sl = strlen(s);
            if (sl + 1 > sizeof c->seed_host || sl == 0) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                CR_FAIL("seedHost out of range");
            }
        }
        memcpy(c->seed_host, s, strlen(s) + 1);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }
    {
        rc = cr_state_str(ctx, st, "linkField", &c->link_field);
        if (rc == -1)
            goto fail_pending;
        if (rc == -2)
            CR_FAIL("state has no linkField");
        if (!c->link_field[0])
            CR_FAIL("linkField is empty");
        rc = cr_state_str(ctx, st, "baseField", &c->base_field);
        if (rc == -1)
            goto fail_pending;
        if (rc == -2 && c->base_field) {
            free(c->base_field);
            c->base_field = NULL;
        }
        rc = cr_state_str(ctx, st, "canonicalField", &c->canonical_field);
        if (rc == -1)
            goto fail_pending;
        rc = cr_state_str(ctx, st, "relField", &c->rel_field);
        if (rc == -1)
            goto fail_pending;
        rc = cr_state_str(ctx, st, "robotsField", &c->robots_field);
        if (rc == -1)
            goto fail_pending;
    }
    {
        JSValue q = JS_GetPropertyStr(ctx, st, "q");
        JSValue qlen;
        if (JS_IsException(q))
            goto fail_pending;
        if (!JS_IsArray(ctx, q)) {
            JS_FreeValue(ctx, q);
            CR_FAIL("state.q must be an array");
        }
        qlen = JS_GetPropertyStr(ctx, q, "length");
        if (JS_IsException(qlen) || JS_ToUint32(ctx, &nq, qlen)) {
            JS_FreeValue(ctx, qlen);
            JS_FreeValue(ctx, q);
            goto fail_pending;
        }
        JS_FreeValue(ctx, qlen);
        if (nq > CR_MAX_PENDING) {
            JS_FreeValue(ctx, q);
            CR_FAIL("state.q exceeds the frontier cap");
        }
        for (i = 0; i < nq; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, q, i);
            JSValue uv, dv;
            const char* us = NULL;
            int32_t dep = 0;
            int bad;
            if (JS_IsException(e) || !JS_IsArray(ctx, e)) {
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, q);
                CR_FAIL("state.q entries must be [url, depth] pairs");
            }
            uv = JS_GetPropertyUint32(ctx, e, 0);
            dv = JS_GetPropertyUint32(ctx, e, 1);
            bad = JS_IsException(uv) || JS_IsException(dv);
            if (!bad) {
                us = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
                bad = !us || JS_ToInt32(ctx, &dep, dv) || dep < 0 || strlen(us) >= 1024 || (strncmp(us, "http://", 7) && strncmp(us, "https://", 8));
            }
            JS_FreeValue(ctx, dv);
            JS_FreeValue(ctx, uv);
            JS_FreeValue(ctx, e);
            if (bad) {
                if (us)
                    JS_FreeCString(ctx, us);
                JS_FreeValue(ctx, q);
                CR_FAIL("state.q entry malformed");
            }
            if (cr_push(c, us, dep) < 0) {
                JS_FreeCString(ctx, us);
                JS_FreeValue(ctx, q);
                CR_FAIL("out of memory restoring the frontier");
            }
            JS_FreeCString(ctx, us);
        }
        JS_FreeValue(ctx, q);
    }
    {
        JSValue sn = JS_GetPropertyStr(ctx, st, "seen");
        JSValue slen;
        if (JS_IsException(sn))
            goto fail_pending;
        if (!JS_IsArray(ctx, sn)) {
            JS_FreeValue(ctx, sn);
            CR_FAIL("state.seen must be an array");
        }
        slen = JS_GetPropertyStr(ctx, sn, "length");
        if (JS_IsException(slen) || JS_ToUint32(ctx, &nsn, slen)) {
            JS_FreeValue(ctx, slen);
            JS_FreeValue(ctx, sn);
            goto fail_pending;
        }
        JS_FreeValue(ctx, slen);
        for (i = 0; i < nsn; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sn, i);
            const char* es;
            int bad;
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, sn);
                goto fail_pending;
            }
            es = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
            bad = !es || strlen(es) >= 1024;
            JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (bad) {
                JS_FreeValue(ctx, sn);
                CR_FAIL("state.seen entry malformed");
            }
        }
        for (i = 0; i < nsn; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sn, i);
            const char* es = JS_ToCString(ctx, e);
            int mrc;
            JS_FreeValue(ctx, e);
            mrc = es ? cr_seen(c, es) ? 0 : cr_mark_seen(c, es) : -1;
            if (es)
                JS_FreeCString(ctx, es);
            if (mrc < 0) {
                JS_FreeValue(ctx, sn);
                CR_FAIL("out of memory restoring the visited set");
            }
        }
        JS_FreeValue(ctx, sn);
    }
    JS_FreeValue(ctx, st);
#undef CR_FAIL

    c->started = 1;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    {
        cr_owner_t* own = (cr_owner_t*)calloc(1, sizeof *own);
        if (!own) {
            cr_dispose(c);
            return JS_ThrowOutOfMemory(ctx);
        }
        own->refs = 1;
        own->crawl_alive = 1;
        c->owner = own;
        c->ctx = ctx;
        c->rt = JS_GetRuntime(ctx);
    }
    obj = dyn_res_wrap(ctx, nt, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj))
        return obj;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_sweep, c);
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    if (argc > 2 && JS_IsObject(argv[2]))
        JS_DefinePropertyValueStr(ctx, obj, "_extractor",
            JS_DupValue(ctx, argv[2]), 0);
    if (argc > 3 && JS_IsFunction(ctx, argv[3]))
        JS_DefinePropertyValueStr(ctx, obj, "_parse",
            JS_DupValue(ctx, argv[3]), 0);
    if (cr_install_async_iterator(ctx, obj) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;

fail_pending:
fail:
    JS_FreeValue(ctx, st);
    cr_dispose(c);
    return JS_EXCEPTION;
}
#endif
