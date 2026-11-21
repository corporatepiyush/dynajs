#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT 30000
#define DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT 5000
#define DYN_HTTP_SERVER_FRAME_MAX (16 * 1024 - 1)
#define DYN_APP_OUT_STALL_MS 10000
#define DYN_APP_SSE_IDLE_MULT 10
#define DYN_APP_UP_IN_MAX (4 * 1024 * 1024)
#define DYN_APP_UP_RATE_BYTES (16 * 1024)

typedef struct {
    int listen_fd;
    uint16_t port;
    int backlog;
    int num_workers;
    int req_timeout_ms;

    dyn_route_t* routes;
    size_t n_routes;
    dyn_route_idx_t* route_idx;

    struct dyn_http_async** async;
    int* lfds;
    int n_async;
    int reuseport;
    int started;
    JSContext* ctx;
} dyn_http_server_t;

static const char* const http_srv_keys[] = { "port", "workers", "backlog", "requestTimeoutMs", "host", "routes" };

static void dyn_http_server_stop_internal(dyn_http_server_t* s)
{
    int i;
    if (!s->started)
        return;
    for (i = 0; i < s->n_async; i++) {
        dyn_http_async_stop_internal(s->async[i]);
        free(s->async[i]);
        if (s->lfds[i] != s->listen_fd)
            close(s->lfds[i]);
    }
    free(s->async);
    free(s->lfds);
    s->async = NULL;
    s->lfds = NULL;
    s->n_async = 0;
    s->started = 0;
}

static void dyn_http_server_dispose(void* native)
{
    dyn_http_server_t* s = (dyn_http_server_t*)native;
    size_t i;

    dyn_http_server_stop_internal(s);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    if (s->routes) {
        for (i = 0; i < s->n_routes; i++) {
            free(s->routes[i].path);
            free(s->routes[i].content_type);
            free(s->routes[i].body);
        }
        free(s->routes);
    }
    dyn_route_index_free(s->route_idx);
    free(s);
}

#ifdef __linux__
#define DYN_HTTP_HAVE_REUSEPORT 1
#endif

static void dyn_http_reuseport(int fd, int on)
{
#ifdef DYN_HTTP_HAVE_REUSEPORT
    if (on)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#else
    (void)fd;
    (void)on;
#endif
}

int dyn_http_reuseport_for(int workers)
{
#ifdef DYN_HTTP_HAVE_REUSEPORT
    return workers > 1;
#else
    (void)workers;
    return 0;
#endif
}

int dyn_http_listen_clone(int lfd, int backlog)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int fd, on = 1;
    if (getsockname(lfd, (struct sockaddr*)&ss, &sl) < 0)
        return -1;
    fd = dyn_net_socket_cloexec(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    dyn_http_reuseport(fd, 1);
#ifdef IPV6_V6ONLY
    if (ss.ss_family == AF_INET6) {
        int v6only = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
#endif
    if (bind(fd, (struct sockaddr*)&ss, sl) < 0 || listen(fd, backlog > 0 ? backlog : 1024) < 0
        || dyn_net_set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_http_bind(const char* host, uint16_t* pport, int backlog)
{
    return dyn_http_bind_ex(host, pport, backlog, 0);
}

int dyn_http_bind_ex(const char* host, uint16_t* pport, int backlog, int reuseport)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, on = 1;

    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
#ifdef IPV6_V6ONLY
        int v6only = 0;
#endif
        {
            const char* h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf))
                hl = sizeof(hbuf) - 1;
            if (h[0] == '[') {
                h++;
                if (hl > 0)
                    hl--;
                if (hl > 0 && h[hl - 1] == ']')
                    hl--;
            }
            memcpy(hbuf, h, hl);
            hbuf[hl] = '\0';
        }
        fd = dyn_net_socket_cloexec(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        dyn_http_reuseport(fd, reuseport);
#ifdef IPV6_V6ONLY
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(*pport);
        if (inet_pton(AF_INET6, hbuf, &sa6.sin6_addr) != 1 || bind(fd, (struct sockaddr*)&sa6, sizeof(sa6)) < 0 || listen(fd, backlog > 0 ? backlog : 1024) < 0) {
            close(fd);
            return -1;
        }
        goto resolved;
    }

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)*pport);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? 0 : AI_PASSIVE;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = dyn_net_socket_cloexec(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        dyn_http_reuseport(fd, reuseport);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

resolved:
    if (*pport == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr*)&sin, &sl) == 0)
            *pport = ntohs(sin.sin_port);
    }
    return fd;
}

JSValue dyn_http_server_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char* host_c = NULL;
    char* host_dup = NULL;
    int32_t port = 0, workers = 1, backlog = 0;
    int32_t req_timeout_ms = DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT;
    JSPropertyEnum* tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        if (dyn_opts_strict(ctx, opts, http_srv_keys, 6)) {
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &workers, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "requestTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &req_timeout_ms, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            host_c = JS_ToCString(ctx, v);
            if (!host_c) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(host_c);
            JS_FreeCString(ctx, host_c);
        }
        JS_FreeValue(ctx, v);

        routes_val = JS_GetPropertyStr(ctx, opts, "routes");
        if (JS_IsException(routes_val)) {
            free(host_dup);
            return JS_EXCEPTION;
        }
    }

    if (port < 0 || port > 65535) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowRangeError(ctx, "port must be in [0, 65535]");
    }
    if (workers < 1)
        workers = 1;
    if (workers > 64)
        workers = 64;
    if (backlog <= 0)
        backlog = SOMAXCONN;
    if (req_timeout_ms < 0)
        req_timeout_ms = 0;

    s = (dyn_http_server_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->num_workers = workers;
    s->backlog = backlog;
    s->req_timeout_ms = req_timeout_ms;
    s->ctx = ctx;

    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t*)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char* path = JS_AtomToCString(ctx, tab[i].atom);
            JSValue rv;
            if (!path)
                goto fail_enum;
            rv = JS_GetProperty(ctx, routes_val, tab[i].atom);
            if (JS_IsException(rv)) {
                JS_FreeCString(ctx, path);
                goto fail_enum;
            }
            if (dyn_route_copy(ctx, path, rv, &s->routes[s->n_routes]) < 0) {
                JS_FreeCString(ctx, path);
                JS_FreeValue(ctx, rv);
                goto fail_enum;
            }
            s->n_routes++;
            JS_FreeCString(ctx, path);
            JS_FreeValue(ctx, rv);
        }
        JS_FreePropertyEnum(ctx, tab, n_routes);
        tab = NULL;
    }
    JS_FreeValue(ctx, routes_val);
    routes_val = JS_UNDEFINED;
    s->route_idx = dyn_route_index_build(s->routes, s->n_routes);

    bound_port = (uint16_t)port;
    s->reuseport = dyn_http_reuseport_for(workers);
    listen_fd = dyn_http_bind_ex(host_dup, &bound_port, backlog, s->reuseport);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        dyn_http_server_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, new_target, dyn_http_server_class_id, s,
        dyn_http_server_dispose);

oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_server_dispose(s);
    return JS_EXCEPTION;
}

JSValue dyn_http_server_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    s->async = (dyn_http_async_t**)calloc((size_t)s->num_workers, sizeof(*s->async));
    s->lfds = (int*)calloc((size_t)s->num_workers, sizeof(*s->lfds));
    if (!s->async || !s->lfds) {
        free(s->async);
        free(s->lfds);
        s->async = NULL;
        s->lfds = NULL;
        return JS_ThrowOutOfMemory(ctx);
    }
    s->started = 1;
    dyn_net_set_nonblock(s->listen_fd);
    while (s->n_async < s->num_workers) {
        dyn_http_async_t* a = (dyn_http_async_t*)calloc(1, sizeof(*a));
        if (!a) {
            dyn_http_server_stop_internal(s);
            return JS_ThrowOutOfMemory(ctx);
        }
        a->ctx = s->ctx;
        a->listen_fd = s->listen_fd;
        if (s->reuseport && s->n_async > 0) {
            int cfd = dyn_http_listen_clone(s->listen_fd, s->backlog);
            if (cfd >= 0)
                a->listen_fd = cfd;
        }
        a->routes = s->routes;
        a->n_routes = s->n_routes;
        a->route_idx = s->route_idx;
        a->borrowed = 1;
        a->max_req = DYN_HTTP_SERVER_FRAME_MAX;
        a->idle_ms = s->req_timeout_ms
            ? (uint64_t)(s->req_timeout_ms < DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT
                      ? s->req_timeout_ms
                      : DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT)
            : 0;
        a->max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
        atomic_init(&a->stop_flag, 0);
        atomic_init(&a->spawn_ok, 0);
        atomic_init(&a->n_refused, 0);
        if (dyn_http_async_spawn(a) < 0) {
            if (a->listen_fd != s->listen_fd)
                close(a->listen_fd);
            free(a);
            dyn_http_server_stop_internal(s);
            return JS_ThrowInternalError(
                ctx, "HTTPServer.start: failed to start the reactor thread");
        }
        s->lfds[s->n_async] = a->listen_fd;
        s->async[s->n_async++] = a;
        if (s->n_async == 1) {
            if (!dyn_net_reactor_acquire(ctx)) {
                dyn_http_server_stop_internal(s);
                return dyn_net_throw_no_reactor(ctx, "HTTPServer");
            }
            a->reactor_held = 1;
        }
    }
    if (dyn_res_pin(ctx, this_val, dyn_http_server_class_id) < 0) {
        dyn_http_server_stop_internal(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

JSValue dyn_http_server_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_server_stop_internal(s);
    dyn_res_unpin(JS_GetRuntime(ctx), this_val, dyn_http_server_class_id);
    return JS_UNDEFINED;
}

JSValue dyn_http_server_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

void dyn_app_conn_unlink(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    if (c->lprev)
        ((dyn_app_conn_t*)c->lprev)->lnext = c->lnext;
    else if (app->conns == c)
        app->conns = c->lnext;
    else
        return;
    if (c->lnext)
        ((dyn_app_conn_t*)c->lnext)->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
}

void dyn_app_conn_unref(dyn_app_conn_t* c)
{
    if (--c->refs == 0) {
        dyn_app_t* app = c->app;
        dyn_app_conn_unlink(c);
        if (c->up) {
            close(c->up->fd);
            unlink(c->up->path);
            free(c->up->path);
            JS_FreeValue(app->ctx, c->up->handler);
            free(c->up);
        }
        JS_FreeValue(app->ctx, c->ws_handlers);
        JS_FreeValue(app->ctx, c->ws_this);
        JS_FreeValue(app->ctx, c->sse_handlers);
        JS_FreeValue(app->ctx, c->sse_this);
        dyn_iobuf_free(&c->ws_frag);
        dyn_iobuf_free(&c->in);
        app->out_bytes -= c->out_q;
        if (c->is_sse && app->n_sse > 0)
            app->n_sse--;
        free(c);
        if (--app->n_conns == 0 && app->dispose_called && !app->in_sweep) {
            if (app->timers_orphan) {
                dyn_timers_free(app->timers_orphan);
                app->timers_orphan = NULL;
            }
            dyn_net_off_drain(app);
            free(app);
        }
    }
}

void dyn_app_conn_close(dyn_app_conn_t* c)
{
    if (c->closed)
        return;
    c->closed = 1;
    dyn_app_stream_abort(c);
    if (c->proxy) {
        dyn_app_proxy_t* px = (dyn_app_proxy_t*)c->proxy;
        px->sent = 1;
        dyn_proxy_fail(px, 0, "");
    }
    if (c->is_ws && JS_IsObject(c->ws_handlers)) {
        JSContext* ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->ws_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[3] = { c->ws_this,
                JS_NewInt32(ctx, c->close_code ? c->close_code : 1000),
                JS_NewStringLen(ctx, "", 0) };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 3, a);
            JS_FreeValue(ctx, a[2]);
        }
        JS_FreeValue(ctx, ch);
    }
    if (c->is_ws && c->ws_native)
        c->ws_native->conn = NULL;
    if (c->is_sse && JS_IsObject(c->sse_handlers)) {
        JSContext* ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->sse_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[1] = { c->sse_this };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, ch);
    }
    if (c->is_sse && c->sse_native)
        c->sse_native->conn = NULL;
    dyn_aio_close(c->app->aio, c->fd);
    dyn_app_conn_unref(c);
}

void dyn_app_shutdown_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_app_t* app = (dyn_app_t*)opaque;
    dyn_app_conn_t* c;
    (void)ctx;
    for (c = (dyn_app_conn_t*)app->conns; c; c = (dyn_app_conn_t*)c->lnext) {
        JSValue v[5] = { c->ws_handlers, c->ws_this, c->sse_handlers, c->sse_this, JS_UNDEFINED };
        size_t i;
        if (c->ws_native)
            c->ws_native->conn = NULL;
        if (c->sse_native)
            c->sse_native->conn = NULL;
        c->ws_native = NULL;
        c->sse_native = NULL;
        c->ws_handlers = c->ws_this = c->sse_handlers = c->sse_this = JS_UNDEFINED;
        if (c->up) {
            v[4] = c->up->handler;
            c->up->handler = JS_UNDEFINED;
        }
        for (i = 0; i < 5; i++)
            JS_FreeValueRT(rt, v[i]);
    }
}

static void dyn_app_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)ud;
    (void)aio;
    if (res <= 0) {
        dyn_app_conn_close(c);
        return;
    }
    if (c->linger) {
        c->linger_left -= (int64_t)len;
        if (c->linger_left < 0)
            dyn_app_conn_close(c);
        return;
    }
    c->refs++;
    if (dyn_iobuf_append(&c->in, buf, len) < 0) {
        dyn_app_conn_close(c);
    } else if (dyn_iobuf_rlen(&c->in)
        > (c->up ? (size_t)DYN_APP_UP_IN_MAX : (size_t)DYN_ACONN_MAX_REQ)) {
        dyn_app_conn_close(c);
    } else {
        size_t before = dyn_iobuf_rlen(&c->in);
        if (c->up) {
            dyn_app_upload_drain(c);
            c->up_win_bytes += (int64_t)len;
            if (c->up_win_bytes >= DYN_APP_UP_RATE_BYTES) {
                c->up_win_bytes = 0;
                c->last_ms = dyn_timer_now_ms();
            }
            while (!c->closed && !c->up && dyn_iobuf_rlen(&c->in) > 0) {
                size_t had = dyn_iobuf_rlen(&c->in);
                dyn_iobuf_ensure_nul(&c->in);
                dyn_app_process(c);
                if (!c->closed && !c->up)
                    dyn_iobuf_compact(&c->in);
                if (dyn_iobuf_rlen(&c->in) >= had)
                    break;
            }
        } else if (c->is_ws) {
            dyn_app_ws_process(c);
        } else if (c->is_sse) {
            dyn_iobuf_reset(&c->in);
        } else {
            dyn_iobuf_ensure_nul(&c->in);
            dyn_app_process(c);
            if (!c->closed && c->is_ws)
                dyn_app_ws_process(c);
        }
        if (!c->closed) {
            dyn_iobuf_compact(&c->in);
            if (!c->up && dyn_iobuf_rlen(&c->in) < before)
                c->last_ms = dyn_timer_now_ms();
        }
    }
    dyn_app_conn_unref(c);
}

static void dyn_app_idle_sweep(void* arg)
{
    dyn_app_t* app = (dyn_app_t*)arg;
    uint64_t now = dyn_timer_now_ms();
    dyn_app_conn_t *c, *next;

    app->in_sweep = 1;
    c = (dyn_app_conn_t*)app->conns;
    if (c)
        c->refs++;
    while (c) {
        uint64_t idle_ms;
        next = (dyn_app_conn_t*)c->lnext;
        if (next)
            next->refs++;
        if (!c->closed) {
            dyn_app_out_account(c);
            if (c->out_q != c->idle_q)
                c->last_ms = now;
            c->idle_q = c->out_q;
        }
        idle_ms = c->is_sse ? app->idle_ms * DYN_APP_SSE_IDLE_MULT
                            : app->idle_ms;
        if (!c->closed && ((idle_ms && now - c->last_ms >= idle_ms) || (c->linger && now >= c->linger_until))) {
            dyn_app_conn_unlink(c);
            dyn_app_conn_close(c);
        } else if (!c->closed && c->out_stalled) {
            if (c->out_q && c->out_q < c->stall_q) {
                c->stall_q = c->out_q;
                c->stall_ms = now;
            } else if (now - c->stall_ms >= DYN_APP_OUT_STALL_MS) {
                dyn_app_conn_unlink(c);
                dyn_app_conn_close(c);
            } else if (!c->file_inflight && c->out_q < DYN_APP_OUT_LOW
                && app->out_bytes < DYN_APP_OUT_TOTAL) {
                dyn_app_out_unstall(c);
                dyn_app_pump_resume(c);
            }
        }
        dyn_app_conn_unref(c);
        c = next;
    }
    app->in_sweep = 0;
    if (!app->dispose_called && app->timers)
        app->sweep = dyn_timer_add(app->timers, now, 1000,
            dyn_app_idle_sweep, app);
}

static void dyn_app_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_t* app = (dyn_app_t*)ud;
    dyn_app_conn_t* c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (app->max_conns && app->n_conns >= app->max_conns) {
        dyn_aio_close(aio, res);
        return;
    }
    c = (dyn_app_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_aio_close(aio, res);
        return;
    }
    c->app = app;
    c->fd = res;
    c->refs = 1;
    c->ws_handlers = JS_UNDEFINED;
    c->ws_this = JS_UNDEFINED;
    c->sse_handlers = JS_UNDEFINED;
    c->sse_this = JS_UNDEFINED;
    dyn_iobuf_init(&c->ws_frag);
    dyn_iobuf_init(&c->in);
    c->last_ms = dyn_timer_now_ms();
    c->lnext = app->conns;
    if (app->conns)
        ((dyn_app_conn_t*)app->conns)->lprev = c;
    app->conns = c;
    app->n_conns++;
    dyn_aio_recv(aio, res, 0, 1, dyn_app_on_recv, c);
}

static void dyn_app_drain(void* udata)
{
    dyn_app_t* app = (dyn_app_t*)udata;
    if (app->timers)
        dyn_timer_run(app->timers, dyn_timer_now_ms());
    if (app->timers_orphan) {
        dyn_timers_free(app->timers_orphan);
        app->timers_orphan = NULL;
    }
    if (app->dispose_called && !app->in_sweep && app->n_conns == 0) {
        dyn_net_off_drain(app);
        free(app);
    }
}

JSValue dyn_app_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    (void)argc;
    (void)argv;
    if (!app)
        return JS_EXCEPTION;
    if (app->started)
        return JS_UNDEFINED;
    if (!app->aio) {
        app->aio = dyn_net_reactor_acquire(ctx);
        if (!app->aio)
            return dyn_net_throw_no_reactor(ctx, "App");
        (void)dyn_aio_pool_warm(app->aio);
    }
    app->listen_fd = dyn_aio_listen(app->aio,
        app->listen_host ? app->listen_host
                         : "0.0.0.0",
        app->port,
        app->backlog > 0 ? app->backlog : 1024);
    if (app->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "App: listen failed");
    if (app->port == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(app->listen_fd, (struct sockaddr*)&sin, &sl) == 0)
            app->port = ntohs(sin.sin_port);
    }
    dyn_aio_accept(app->aio, app->listen_fd, dyn_app_on_accept, app);
    if (!app->timers) {
        app->timers = dyn_timers_new();
        if (!app->timers)
            return JS_ThrowOutOfMemory(ctx);
        app->sweep = dyn_timer_add(app->timers, dyn_timer_now_ms(), 1000,
            dyn_app_idle_sweep, app);
    }
    {
        int rc = dyn_net_on_drain(dyn_app_drain, app);
        if (rc == -2)
            return JS_ThrowInternalError(ctx,
                "App: this reactor backend cannot arm a periodic timer, so the "
                "idle-timeout sweep would never run");
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
    }
    app->started = 1;
    if (JS_AddShutdownSweep(JS_GetRuntime(ctx), dyn_app_shutdown_sweep, app) < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_res_pin(ctx, this_val, dyn_app_class_id) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}
#endif
