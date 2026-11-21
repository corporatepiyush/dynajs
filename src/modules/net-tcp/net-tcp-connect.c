#include "net-tcp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

static const char* const tcp_cli_keys[] = { "path", "host", "port", "maxConnections", "idleTimeoutMs",
    "connectTimeoutMs", "tls", "highWaterMark", "decoder" };

static const char* const tcp_eb_keys[] = { "fallbackMs", "tls" };

static void tcp_hold(JSContext* ctx, dyn_tcp_t* t, JSValueConst obj)
{
    t->client = 1;
    t->self_pending = JS_DupValue(ctx, obj);
    JS_AddShutdownSweep(t->rt, tcp_hold_sweep, t);
}

void tcp_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_tcp_conn_t* c = (dyn_tcp_conn_t*)ud;
    dyn_tcp_t* t;
    JSContext* ctx;
    (void)aio;
    (void)buf;
    (void)len;

    if (!c->owner) {
        free(c);
        return;
    }
    t = c->owner;
    t->pending_conn = NULL;
    ctx = t->ctx;

    t->connect_deadline_ms = 0;
    if (res < 0) {
        dyn_aio_t* a = t->aio;
        int fd = c->fd;
        c->closed = 1;
        dyn_aio_close(a, fd);
        tcp_conn_drop(JS_GetRuntime(ctx), c);
        tcp_report_connect(t, JS_UNDEFINED, strerror(-res));
        return;
    }
    if (tcp_conn_start(t, c) < 0) {
        tcp_release_pending(JS_GetRuntime(ctx), t);
        tcp_report_connect(t, JS_UNDEFINED, "connection setup failed");
        return;
    }
#ifdef CONFIG_TLS
    if (t->tls_ctx) {
        char err[192];
        c->tls = dyn_tls_conn_new(t->tls_ctx, t->tls_servername,
            err, sizeof err);
        if (!c->tls) {
            tls_fail(c, err);
            return;
        }
        if (dyn_tls_handshake(c->tls) < 0 || tls_flush(c) < 0) {
            tls_fail(c, "TLS handshake failed to start");
            return;
        }
        return;
    }
#endif
    tcp_report_connect(t, c->jsobj, NULL);
}

static int tcp_set_handlers(JSContext* ctx, dyn_tcp_t* t, JSValueConst h)
{
    JSValue v[TCP_NHANDLERS];
    if (tcp_collect_handlers(ctx, h, v) < 0)
        return -1;
    tcp_install_handlers(t, v);
    return 0;
}

#ifndef CONFIG_TLS
static int tcp_tls_asked(JSContext* ctx, JSValueConst o)
{
    JSValue tv = JS_GetPropertyStr(ctx, o, "tls");
    int asked;
    if (JS_IsException(tv))
        return -1;
    asked = !JS_IsUndefined(tv) && !JS_IsNull(tv) && !(JS_IsBool(tv) && !JS_ToBool(ctx, tv));
    JS_FreeValue(ctx, tv);
    return asked;
}
#endif

JSValue dyn_tcp_connect_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    dyn_tcp_conn_t* c;
    const char* host = NULL;
    int64_t port = 0;
    int fd;
    JSValue v, res;
    (void)this_val;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "connect(options, handlers) needs options");
    if (dyn_opts_strict(ctx, argv[0], tcp_cli_keys, (int)countof(tcp_cli_keys)))
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[0], "path");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        const char* up = tcp_unix_path(ctx, v);
        JS_FreeValue(ctx, v);
        if (!up)
            return JS_EXCEPTION;
        t = tcp_new(ctx);
        if (!t) {
            JS_FreeCString(ctx, up);
            return JS_EXCEPTION;
        }
        if (tcp_read_bounds(ctx, t, argv[0], 1) < 0) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
#ifdef CONFIG_TLS
        if (tcp_setup_tls(ctx, t, argv[0], NULL) < 0) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
#else
        {
            int asked = tcp_tls_asked(ctx, argv[0]);
            if (asked) {
                JS_FreeCString(ctx, up);
                dyn_tcp_dispose(t);
                return asked < 0 ? JS_EXCEPTION : dyn_tls_missing(ctx, "connect", "tls");
            }
        }
#endif
        if ((argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) || tcp_check_decoder(ctx, t, "connect") < 0) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
        c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
        if (!c) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_ThrowOutOfMemory(ctx);
        }
        c->owner = t;
        c->refs = 1;
        c->jsobj = JS_UNDEFINED;
        c->hwm = t->hwm;
        c->tap = JS_UNDEFINED;
        fd = dyn_aio_unix_connect(t->aio, up, tcp_on_connect, c);
        JS_FreeCString(ctx, up);
        if (fd < 0) {
            free(c);
            dyn_tcp_dispose(t);
            return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
        }
        c->fd = fd;
        t->started = 1;
        {
            JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t,
                dyn_tcp_dispose);
            if (!JS_IsException(obj))
                tcp_hold(ctx, t, obj);
            return obj;
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "host");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        host = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!host)
            return JS_EXCEPTION;
    }
    v = JS_GetPropertyStr(ctx, argv[0], "port");
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
        JS_FreeValue(ctx, v);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (port < 1 || port > 65535) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connect: port must be 1..65535");
    }

    t = tcp_new(ctx);
    if (!t) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    if (tcp_read_bounds(ctx, t, argv[0], 1) < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (tcp_setup_tls(ctx, t, argv[0], host) < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    {
        int asked = tcp_tls_asked(ctx, argv[0]);
        if (asked) {
            if (host)
                JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return asked < 0 ? JS_EXCEPTION : dyn_tls_missing(ctx, "connect", "tls");
        }
    }
#endif
    if ((argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) || tcp_check_decoder(ctx, t, "connect") < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }

    c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_tcp_dispose(t);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;
    c->tap = JS_UNDEFINED;

    fd = dyn_aio_connect(t->aio, host ? host : "127.0.0.1", (uint16_t)port,
        tcp_on_connect, c);
    if (host)
        JS_FreeCString(ctx, host);
    if (fd < 0) {
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
    }
    c->fd = fd;
    t->pending_conn = c;
    t->started = 1;
    if (tcp_arm_sweep(t) < 0) {
        dyn_aio_close(t->aio, fd);
        c->fd = -1;
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx,
            "connect: the backend cannot arm a clock, so connectTimeoutMs "
            "would never fire");
    }
    res = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t, dyn_tcp_dispose);
    if (!JS_IsException(res))
        tcp_hold(ctx, t, res);
    return res;
}

JSValue dyn_tcp_connect_happy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    dyn_tcp_conn_t* c;
    const char* host;
    int64_t port, fallback = 250;
    char ips[2][INET6_ADDRSTRLEN];
    JSValue v, res;
    eb_dns_t* dns = NULL;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "connectHappy(host, port, opts, handlers)");
    host = JS_ToCString(ctx, argv[0]);
    if (!host)
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &port, argv[1])) {
        JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    if (port < 1 || port > 65535) {
        JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connectHappy: port must be 1..65535");
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], tcp_eb_keys, 2)) {
            JS_FreeCString(ctx, host);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "fallbackMs");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &fallback, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, host);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (fallback < 1) {
        JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connectHappy: fallbackMs must be >= 1");
    }
    memset(ips, 0, sizeof(ips));
    if (eb_resolve(host, (uint16_t)port, ips, 1) < 0) {
        memset(ips, 0, sizeof(ips));
        dns = (eb_dns_t*)calloc(1, sizeof(*dns));
        if (!dns || !(dns->host = strdup(host))) {
            free(dns);
            JS_FreeCString(ctx, host);
            return JS_ThrowOutOfMemory(ctx);
        }
        dns->port = (uint16_t)port;
        dns->fallback = (uint64_t)fallback;
    } else if (!ips[0][0] && !ips[1][0]) {
        JS_FreeCString(ctx, host);
        return JS_ThrowInternalError(ctx, "connectHappy: no usable addresses");
    }

    t = tcp_new(ctx);
    if (!t) {
        if (dns)
            eb_dns_free(dns);
        JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }

    if (argc > 3 && JS_IsObject(argv[3])) {
        if (tcp_set_handlers(ctx, t, argv[3]) < 0) {
            JS_FreeCString(ctx, host);
            if (dns)
                eb_dns_free(dns);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
    } else if (argc > 2 && JS_IsObject(argv[2])) {
        if (tcp_set_handlers(ctx, t, argv[2]) < 0) {
            JS_FreeCString(ctx, host);
            if (dns)
                eb_dns_free(dns);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
    }
#ifdef CONFIG_TLS
    if (argc > 2 && JS_IsObject(argv[2]) && tcp_setup_tls(ctx, t, argv[2], host) < 0) {
        JS_FreeCString(ctx, host);
        if (dns)
            eb_dns_free(dns);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    if (argc > 2 && JS_IsObject(argv[2])) {
        int asked = tcp_tls_asked(ctx, argv[2]);
        if (asked) {
            JS_FreeCString(ctx, host);
            if (dns)
                eb_dns_free(dns);
            dyn_tcp_dispose(t);
            return asked < 0 ? JS_EXCEPTION : dyn_tls_missing(ctx, "connectHappy", "tls");
        }
    }
#endif
    JS_FreeCString(ctx, host);

    c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        if (dns)
            eb_dns_free(dns);
        dyn_tcp_dispose(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;
    c->tap = JS_UNDEFINED;
    c->fd = -1;
    t->pending_conn = c;
    t->started = 1;
    if (dns) {
        int r;
        dns->owner = t;
        t->eb_dns = dns;
        dns->sync = 1;
        r = dyn_aio_offload(t->aio, eb_dns_work, eb_dns_done, dns);
        dns->sync = 0;
        if (r != 0) {
            int failed = dns->failed;
            if (r == 1 && !failed)
                memcpy(ips, dns->ips, sizeof(ips));
            t->eb_dns = NULL;
            eb_dns_free(dns);
            dns = NULL;
            if (r < 0 || failed) {
                t->pending_conn = NULL;
                free(c);
                dyn_tcp_dispose(t);
                return JS_ThrowInternalError(ctx, "connectHappy: DNS resolution failed");
            }
        }
    }
    if (!dns && eb_arm(t, ips, (uint16_t)port, (uint64_t)fallback) == 0) {
        t->pending_conn = NULL;
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connectHappy: connect failed");
    }
    if (dyn_net_on_drain(eb_sweep, t) < 0) {
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx,
            "connectHappy: the backend cannot arm a clock, so fallbackMs "
            "would never fire");
    }
    t->hooked = 1;
    res = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t, dyn_tcp_dispose);
    if (!JS_IsException(res))
        tcp_hold(ctx, t, res);
    return res;
}

#endif
