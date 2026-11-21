#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

#define DNS_CACHE_TTL_CAP 86400

static const char* const dns_resolver_keys[] = { "server", "port", "timeoutMs", "ttl" };

void dyn_resolver_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_resolver_class_id);
    dyn_resolver_t* r;
    int i;

    if (!res || res->closed || !res->native)
        return;
    r = (dyn_resolver_t*)res->native;
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (r->q[i].used) {
            JS_MarkValue(rt, r->q[i].cb, mark_func);
            JS_MarkValue(rt, r->q[i].resolve, mark_func);
            JS_MarkValue(rt, r->q[i].reject, mark_func);
        }
    for (i = 0; i < DNS_CACHE_SLOTS; i++)
        if (r->cache[i].used)
            JS_MarkValue(rt, r->cache[i].records, mark_func);
}

static void dyn_resolver_dispose(void* native)
{
    dyn_resolver_t* r = (dyn_resolver_t*)native;
    if (!r)
        return;
    if (r->in_callback) {
        r->closing = 1;
        return;
    }
    dyn_resolver_teardown(r);
}

void dyn_resolver_teardown(dyn_resolver_t* r)
{
    JSContext* ctx = r->ctx;
    int i;
    dyn_net_off_drain(r);
    if (r->timers) {
        dyn_timers_free(r->timers);
        r->timers = NULL;
    }
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (r->q[i].used) {
            if (r->q[i].tcp_buf) {
                free(r->q[i].tcp_buf);
                r->q[i].tcp_buf = NULL;
            }
            if (r->q[i].tcp_fd >= 0) {
                dyn_aio_close(r->aio, r->q[i].tcp_fd);
                r->q[i].tcp_fd = -1;
            }
            if (!dns_in_final && r->q[i].is_promise && !JS_IsUndefined(r->q[i].reject)) {
                JSValue e = JS_NewError(ctx), rr;
                if (!JS_IsException(e)) {
                    JS_DefinePropertyValueStr(ctx, e, "message",
                        JS_NewString(ctx, "DNSResolver: closed"), JS_PROP_C_W_E);
                } else {
                    dyn_exc_drop(ctx);
                    e = JS_NewString(ctx, "DNSResolver: closed");
                }
                rr = JS_Call(ctx, r->q[i].reject, JS_UNDEFINED, 1,
                    (JSValueConst*)&e);
                if (JS_IsException(rr))
                    dyn_exc_drop(ctx);
                else
                    JS_FreeValue(ctx, rr);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValueRT(r->rt, r->q[i].cb);
            JS_FreeValueRT(r->rt, r->q[i].resolve);
            JS_FreeValueRT(r->rt, r->q[i].reject);
            r->q[i].used = 0;
        }
    for (i = 0; i < DNS_CACHE_SLOTS; i++)
        dns_cache_free_entry_rt(r->rt, &r->cache[i]);
    if (r->aio) {
        if (r->fd >= 0)
            dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release_rt(r->rt);
    }
    free(r);
}

JSValue dyn_resolver_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* server = NULL;
    int64_t port = 53, timeout = 5000, ttl = 0;
    JSValue v;

    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], dns_resolver_keys, 4))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "server");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            server = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!server)
                return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "timeoutMs");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &timeout, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "ttl");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &ttl, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (port < 1 || port > 65535 || timeout < 1 || timeout > 4294967295LL) {
        if (server)
            JS_FreeCString(ctx, server);
        return JS_ThrowRangeError(ctx, "DNSResolver: bad port or timeoutMs");
    }
    if (ttl < 0)
        ttl = 0;
    if (ttl > DNS_CACHE_TTL_CAP)
        ttl = DNS_CACHE_TTL_CAP;

    r = (dyn_resolver_t*)calloc(1, sizeof(*r));
    if (!r) {
        if (server)
            JS_FreeCString(ctx, server);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->ctx = ctx;
    r->rt = JS_GetRuntime(ctx);
    r->fd = -1;
    r->timeout_ms = (uint32_t)timeout;
    r->ttl_cap_s = (uint32_t)ttl;
    memset(&r->server, 0, sizeof(r->server));
    r->server.sin_family = AF_INET;
    r->server.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server ? server : "127.0.0.1",
            &r->server.sin_addr)
        != 1) {
        if (server)
            JS_FreeCString(ctx, server);
        free(r);
        return JS_ThrowTypeError(ctx, "DNSResolver: server must be an IPv4 address");
    }
    if (server)
        JS_FreeCString(ctx, server);

    r->aio = dyn_net_reactor_acquire(ctx);
    if (!r->aio) {
        free(r);
        return dyn_net_throw_no_reactor(ctx, "Resolver");
    }
    r->fd = dyn_aio_udp_bind(r->aio, "0.0.0.0", 0);
    if (r->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowInternalError(ctx, "DNSResolver: bind failed");
    }
    if (connect(r->fd, (struct sockaddr*)&r->server, sizeof(r->server)) != 0) {
        dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowInternalError(ctx, "DNSResolver: connect failed");
    }
    r->timers = dyn_timers_new();
    if (!r->timers) {
        dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        JSValue obj = dyn_res_wrap(ctx, new_target, dyn_resolver_class_id, r, dyn_resolver_dispose);
        if (!JS_IsException(obj))
            r->self = obj;
        return obj;
    }
}

static int dns_reject_pending(JSContext* ctx, JSValueConst reject)
{
    JSValue exc, r;
    if (JS_HasUncatchableException(ctx))
        return -1;
    exc = JS_GetException(ctx);
    r = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
    JS_FreeValue(ctx, exc);
    if (!JS_IsException(r)) {
        JS_FreeValue(ctx, r);
        return 0;
    }
    if (JS_HasUncatchableException(ctx))
        return -1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 0;
}

JSValue dyn_resolver_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    int64_t type = DYN_DNS_T_A;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "query(name, type, callback)");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &type, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    return dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 0,
        argc >= 3 ? argv[2] : JS_UNDEFINED,
        JS_UNDEFINED, JS_UNDEFINED);
}

JSValue dyn_resolver_lookup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    int64_t type = DYN_DNS_T_A;
    JSValueConst cb = JS_UNDEFINED;
    int ncoerce = argc;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "lookup(name, type?, callback?)");
    if (argc >= 2 && JS_IsFunction(ctx, argv[argc - 1])) {
        cb = argv[argc - 1];
        ncoerce = argc - 1;
    }
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (ncoerce >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &type, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    if (!JS_IsUndefined(cb)) {
        return dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 0, cb,
            JS_UNDEFINED, JS_UNDEFINED);
    }
    {
        JSValue funcs[2], promise, sub;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        sub = dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 1, JS_UNDEFINED,
            funcs[0], funcs[1]);
        if (JS_IsException(sub) && dns_reject_pending(ctx, funcs[1]) < 0) {
            JS_FreeValue(ctx, promise);
            promise = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, sub);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
}

static JSValue dns_resolve_typed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int64_t type,
    int shape, const char* what)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    JSValue funcs[2], promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(name)", what);
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, name);
        return promise;
    }
    {
        JSValue sub = dns_submit(ctx, r, name, type, shape, 1, JS_UNDEFINED,
            funcs[0], funcs[1]);
        if (JS_IsException(sub) && dns_reject_pending(ctx, funcs[1]) < 0) {
            JS_FreeValue(ctx, promise);
            promise = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, sub);
    }
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

JSValue dyn_resolver_cname(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_CNAME,
        DNS_SHAPE_STRINGS, "resolveCname");
}

JSValue dyn_resolver_mx(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_MX,
        DNS_SHAPE_MX, "resolveMx");
}

JSValue dyn_resolver_txt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_TXT,
        DNS_SHAPE_TXT, "resolveTxt");
}

JSValue dyn_resolver_ns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_NS,
        DNS_SHAPE_STRINGS, "resolveNs");
}

#endif
