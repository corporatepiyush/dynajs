#include "net-tcp-internal.h"
#include <sys/stat.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#define TCP_HWM_DEFAULT (4u << 20)
#define TCP_HWM_MAX (1u << 30)

static const char* const tcp_srv_keys[] = { "port", "path", "maxConnections", "idleTimeoutMs", "tls", "highWaterMark", "decoder" };

static const char* const tcp_handler_keys[] = { "connect", "data", "close", "drain", "frame", "error" };

JSClassID dyn_tcp_class_id;

JSClassID dyn_tcp_conn_class_id;

static JSValue dyn_net_handler_throws_getter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, (int64_t)dyn_net_handler_throw_count());
}

static void dyn_tcp_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_tcp_class_id);
    dyn_tcp_t* t = (r && !r->closed) ? (dyn_tcp_t*)r->native : NULL;
    dyn_tcp_conn_t* c;

    if (!t)
        return;
    JS_MarkValue(rt, t->h_connect, mark_func);
    JS_MarkValue(rt, t->h_data, mark_func);
    JS_MarkValue(rt, t->h_close, mark_func);
    JS_MarkValue(rt, t->h_drain, mark_func);
    JS_MarkValue(rt, t->h_frame, mark_func);
    JS_MarkValue(rt, t->h_error, mark_func);
    for (c = t->conns; c; c = c->lnext)
        JS_MarkValue(rt, c->jsobj, mark_func);
}

static const JSClassDef dyn_tcp_class = {
    "TCPServer",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_tcp_gc_mark,
};

static const JSClassDef dyn_tcp_conn_class = {
    "TCPConn",
    .finalizer = dyn_tcp_conn_finalizer,
    .gc_mark = dyn_tcp_conn_mark,
};

void tcp_hold_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_tcp_t* t = (dyn_tcp_t*)opaque;
    JSValue v = t->self_pending;
    (void)ctx;
    t->self_pending = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
}

void tcp_listen_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_tcp_t* t = (dyn_tcp_t*)opaque;
    JSValue v = t->self_listen;
    (void)ctx;
    t->self_listen = JS_UNDEFINED;
    t->self_weak = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
}

#ifdef CONFIG_TLS
int tls_flush(dyn_tcp_conn_t* c);
#endif

static JSValue tcp_unhold_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static void tcp_pin_live(dyn_tcp_t* t)
{
    if (t->client || !JS_IsUndefined(t->self_listen) || JS_IsUndefined(t->self_weak))
        return;
    t->self_listen = JS_DupValue(t->ctx, t->self_weak);
    JS_AddShutdownSweep(t->rt, tcp_listen_sweep, t);
}

void tcp_list_unlink(dyn_tcp_conn_t* c)
{
    dyn_tcp_t* t = c->owner;
    if (c->lprev)
        c->lprev->lnext = c->lnext;
    else if (t->conns == c)
        t->conns = c->lnext;
    if (c->lnext)
        c->lnext->lprev = c->lprev;
    c->lnext = c->lprev = NULL;
    if (t->nconns > 0)
        t->nconns--;
}

static const JSCFunctionListEntry dyn_tcp_conn_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_tcp_conn_write),
    JS_CFUNC_DEF("close", 0, dyn_tcp_conn_close_method),
    JS_CGETSET_DEF("bufferedAmount", dyn_tcp_conn_buffered, NULL),
    JS_CFUNC_MAGIC_DEF("pause", 0, dyn_tcp_conn_pause, 0),
    JS_CFUNC_MAGIC_DEF("resume", 0, dyn_tcp_conn_pause, 1),
    JS_CGETSET_DEF("paused", dyn_tcp_conn_paused, NULL),
    JS_CFUNC_DEF("end", 0, dyn_tcp_conn_end),
    JS_CFUNC_DEF("streams", 1, dyn_tcp_conn_streams),
    JS_CFUNC_DEF("setDecoder", 1, dyn_tcp_conn_set_decoder),
};

void tcp_report_connect(dyn_tcp_t* t, JSValueConst obj, const char* err)
{
    JSContext* ctx = t->ctx;
    JSValue h = JS_DupValue(ctx, t->h_connect);
    JSValue pend = JS_UNDEFINED;

    if (err || !t->conns) {
        pend = t->self_pending;
        t->self_pending = JS_UNDEFINED;
        JS_RemoveShutdownSweep(t->rt, tcp_hold_sweep, t);
    }

    if (err && !t->conns && !t->pending_conn && !t->released
        && t->listen_fd < 0) {
        if (t->hooked) {
            dyn_net_off_drain(t);
            t->hooked = 0;
        }
        t->released = 1;
        dyn_net_reactor_release(t->ctx);
    }

    if (JS_IsFunction(ctx, h)) {
        JSValueConst a[2];
        JSValue r, msg = err ? JS_NewString(ctx, err) : JS_NULL;
        a[0] = err ? JS_NULL : obj;
        a[1] = msg;
        t->in_cb++;
        r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
        t->in_cb--;
        (void)tcp_gone(t);
        dyn_net_handler_result(ctx, r, NET_HK_TCP_CONNECT);
        JS_FreeValue(ctx, msg);
    }
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, pend);
}

#ifdef CONFIG_TLS
void tls_fail(dyn_tcp_conn_t* c, const char* fallback)
{
    dyn_tcp_t* t = c->owner;
    JSContext* ctx = t->ctx;
    dyn_aio_t* a = t->aio;
    const char* e = c->tls ? dyn_tls_error(c->tls) : NULL;
    char msg[192];
    int fd = c->fd;

    snprintf(msg, sizeof msg, "%s", e ? e : fallback);
    if (!c->closed) {
        c->closed = 1;
        tcp_list_unlink(c);
        dyn_aio_close(a, fd);
        tcp_conn_drop(JS_GetRuntime(ctx), c);
    }
    tcp_report_connect(t, JS_UNDEFINED, msg);
}

int tls_flush(dyn_tcp_conn_t* c)
{
    uint8_t out[DYN_TLS_RECORD_MAX] DYN_IO_SCRATCH;
    int n;
    if (c->closed || !c->owner)
        return 0;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0) {
        if (dyn_aio_send(c->owner->aio, c->fd, out, (unsigned)n, DYN_AIO_HOLD,
                tcp_conn_sent, c)
            < 0)
            return -1;
    }
    return 0;
}
#endif

void tcp_sweep(void* udata)
{
    dyn_tcp_t* t = (dyn_tcp_t*)udata;
    dyn_tcp_conn_t *c, *next;
    uint64_t now = dyn_timer_now_ms();

    if (t->connect_deadline_ms && now >= t->connect_deadline_ms) {
        JSContext* ctx = t->ctx;
        t->connect_deadline_ms = 0;
        if (JS_IsFunction(ctx, t->h_connect)) {
            JSValueConst a[2] = { JS_NULL,
                JS_NewString(ctx, "connect timed out") };
            t->in_cb++;
            JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 2, a);
            t->in_cb--;
            dyn_net_handler_result(ctx, r, NET_HK_TCP_CONNECT);
            JS_FreeValue(ctx, a[1]);
            if (tcp_gone(t))
                return;
        }
        if (tcp_gone(t))
            return;
        for (c = t->conns; c; c = next) {
            next = c->lnext;
            tcp_conn_close(c);
        }
        if (t->pending_conn) {
            dyn_tcp_conn_t* pc = t->pending_conn;
            t->pending_conn = NULL;
            pc->closed = 1;
            pc->owner = NULL;
            if (t->aio && pc->fd >= 0)
                dyn_aio_close(t->aio, pc->fd);
            pc->fd = -1;
            free(pc);
        }
        tcp_client_idle(t);
        tcp_release_pending(JS_GetRuntime(ctx), t);
        return;
    }
    if (!t->idle_ms)
        return;
    for (c = t->conns; c; c = next) {
        next = c->lnext;
        if (!c->paused && now - c->last_ms >= t->idle_ms) {
            t->n_idle_closed++;
            tcp_conn_close(c);
            if (tcp_gone(t))
                return;
        }
    }
}

static void tcp_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_tcp_t* t = (dyn_tcp_t*)ud;
    dyn_tcp_conn_t* c;
    (void)buf;
    (void)len;

    if (res < 0)
        return;
    if (t->max_conns && t->nconns >= t->max_conns) {
        t->n_refused++;
        dyn_aio_close(aio, res);
        return;
    }
    c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_aio_close(aio, res);
        return;
    }
    c->owner = t;
    c->fd = res;
    c->refs = 1;
    c->hwm = t->hwm;
    c->tap = JS_UNDEFINED;
#ifdef CONFIG_TLS
    if (t->tls_ctx) {
        char terr[192];
        c->tls = dyn_tls_conn_accept(t->tls_ctx, terr, sizeof terr);
        if (!c->tls) {
            free(c);
            dyn_aio_close(aio, res);
            return;
        }
    }
#endif
    c->jsobj = JS_UNDEFINED;
    if (tcp_conn_start(t, c) < 0)
        return;
    {
        int defer_connect = 0;
#ifdef CONFIG_TLS
        defer_connect = t->tls_ctx != NULL;
#endif
        if (!defer_connect && JS_IsFunction(t->ctx, t->h_connect)) {
            JSContext* ctx = t->ctx;
            JSValueConst a[1] = { c->jsobj };
            t->in_cb++;
            JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 1, a);
            t->in_cb--;
            int gone = tcp_gone(t);
            dyn_net_handler_result(ctx, r, NET_HK_TCP_CONNECT);
            if (gone)
                return;
        }
    }
}


void tcp_client_idle(dyn_tcp_t* t)
{
    JSValue v;

    if (!t->client || t->conns || t->pending_conn || t->closing)
        return;
    if (!t->released) {
        if (t->hooked) {
            dyn_net_off_drain(t);
            t->hooked = 0;
        }
        t->released = 1;
        dyn_net_reactor_release(t->ctx);
    }
    if (JS_IsUndefined(t->self_pending))
        return;
    v = t->self_pending;
    if (JS_EnqueueJob(t->ctx, tcp_unhold_job, 1, (JSValueConst*)&v) < 0)
        return;
    t->self_pending = JS_UNDEFINED;
    JS_RemoveShutdownSweep(t->rt, tcp_hold_sweep, t);
    JS_FreeValue(t->ctx, v);
}

static void tcp_detach_conns(dyn_tcp_t* t)
{
    JSRuntime* rt = t->rt;
    dyn_tcp_conn_t *c = t->conns, *next;
    int i;

    if (t->pending_conn) {
        dyn_tcp_conn_t* pc = t->pending_conn;
        t->pending_conn = NULL;
        pc->closed = 1;
        pc->owner = NULL;
        if (t->aio && pc->fd >= 0)
            dyn_aio_close(t->aio, pc->fd);
        pc->fd = -1;
        free(pc);
    }
    for (i = 0; i < 2; i++)
        if (t->eb[i].fd >= 0) {
            dyn_aio_close(t->aio, t->eb[i].fd);
            t->eb[i].fd = -1;
        }
    if (t->eb_dns) {
        t->eb_dns->owner = NULL;
        if (t->aio && !t->eb_dns->held) {
            dyn_net_reactor_hold();
            t->eb_dns->held = 1;
            t->eb_dns->rt = t->rt;
        }
        t->eb_dns = NULL;
    }
    t->eb_done = 1;
    t->conns = NULL;
    t->nconns = 0;
    while (c) {
        next = c->lnext;
        c->lnext = c->lprev = NULL;
        if (!c->closed) {
            c->closed = 1;
            if (t->aio && c->fd >= 0)
                dyn_aio_close(t->aio, c->fd);
            c->fd = -1;
            c->owner = NULL;
            tcp_conn_drop(rt, c);
        } else {
            c->owner = NULL;
        }
        c = next;
    }
}

void dyn_tcp_dispose(void* native)
{
    dyn_tcp_t* t = (dyn_tcp_t*)native;
    if (!t)
        return;
    if (t->in_cb) {
        t->closing = 1;
        return;
    }
    dyn_tcp_teardown(t);
}

void dyn_tcp_teardown(dyn_tcp_t* t)
{
    if (!t)
        return;
    if (t->rt)
        tcp_release_pending(t->rt, t);
    if (t->rt)
        tcp_release_listen(t->rt, t);
    t->self_weak = JS_UNDEFINED;
    if (t->hooked) {
        dyn_net_off_drain(t);
        t->hooked = 0;
    }
    tcp_detach_conns(t);
    if (t->aio && !t->released) {
        if (t->listen_fd >= 0)
            dyn_aio_close(t->aio, t->listen_fd);
        dyn_net_reactor_release_rt(t->rt);
    }
    if (t->path) {
        struct stat st;
        if (t->path_bound && lstat(t->path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == t->path_dev && st.st_ino == t->path_ino)
            unlink(t->path);
        free(t->path);
    }
    JS_FreeValueRT(t->rt, t->h_connect);
    JS_FreeValueRT(t->rt, t->h_data);
    JS_FreeValueRT(t->rt, t->h_close);
    JS_FreeValueRT(t->rt, t->h_drain);
    JS_FreeValueRT(t->rt, t->h_frame);
    JS_FreeValueRT(t->rt, t->h_error);
    frame_spec_unref(t->spec);
#ifdef CONFIG_TLS
    dyn_tls_ctx_free(t->tls_ctx);
    free(t->tls_servername);
#endif
    free(t);
}

dyn_tcp_t* tcp_new(JSContext* ctx)
{
    dyn_tcp_t* t = (dyn_tcp_t*)calloc(1, sizeof(*t));
    if (!t) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    t->ctx = ctx;
    t->rt = JS_GetRuntime(ctx);
    t->listen_fd = -1;
    t->h_connect = t->h_data = t->h_close = t->h_drain = t->h_frame = t->h_error = JS_UNDEFINED;
    t->hwm = TCP_HWM_DEFAULT;
    t->self_pending = JS_UNDEFINED;
    t->eb[0].owner = t;
    t->self_listen = JS_UNDEFINED;
    t->eb[0].idx = 0;
    t->eb[0].fd = -1;
    t->eb[1].owner = t;
    t->eb[1].idx = 1;
    t->eb[1].fd = -1;
    t->aio = dyn_net_reactor_acquire(ctx);
    if (!t->aio) {
        free(t);
        dyn_net_throw_no_reactor(ctx, "TCPServer");
        return NULL;
    }
    (void)dyn_aio_pool_warm(t->aio);
    return t;
}

int tcp_read_bounds(JSContext* ctx, dyn_tcp_t* t, JSValueConst o,
    int is_client)
{
    JSValue v;
    int64_t n;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "decoder");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        dyn_frame_spec_t* s = frame_spec_from(ctx, v, is_client ? "connect" : "TCPServer");
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        frame_spec_unref(t->spec);
        t->spec = frame_spec_ref(s);
    } else {
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, o, "maxConnections");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n < 0 || n > 1000000) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "maxConnections must be 0..1000000");
            return -1;
        }
        t->max_conns = (int)n;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "highWaterMark");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n < 1 || n > TCP_HWM_MAX) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "highWaterMark must be 1..%d",
                TCP_HWM_MAX);
            return -1;
        }
        t->hwm = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "idleTimeoutMs");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n < 0) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "idleTimeoutMs must be >= 0");
            return -1;
        }
        t->idle_ms = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);
    if (is_client) {
        v = JS_GetPropertyStr(ctx, o, "connectTimeoutMs");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &n, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (n < 0) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "connectTimeoutMs must be >= 0");
                return -1;
            }
            if (n > 0)
                t->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)n;
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

const char* tcp_unix_path(JSContext* ctx, JSValueConst v)
{
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s)
        return NULL;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "path contains a NUL byte: bind() would use "
                               "only the part before it");
        return NULL;
    }
    return s;
}

int tcp_collect_handlers(JSContext* ctx, JSValueConst h, JSValue out[TCP_NHANDLERS])
{
    int i;

    for (i = 0; i < TCP_NHANDLERS; i++)
        out[i] = JS_UNDEFINED;
    if (!JS_IsObject(h))
        return 0;
    if (dyn_opts_strict(ctx, h, tcp_handler_keys, TCP_NHANDLERS))
        return -1;
    for (i = 0; i < TCP_NHANDLERS; i++) {
        out[i] = JS_GetPropertyStr(ctx, h, tcp_handler_keys[i]);
        if (JS_IsException(out[i])) {
            while (--i >= 0) {
                JS_FreeValue(ctx, out[i]);
                out[i] = JS_UNDEFINED;
            }
            return -1;
        }
    }
    return 0;
}

void tcp_install_handlers(dyn_tcp_t* t, JSValue v[TCP_NHANDLERS])
{
    JSValue old[TCP_NHANDLERS];
    JSValue* slot[TCP_NHANDLERS] = { &t->h_connect, &t->h_data, &t->h_close, &t->h_drain, &t->h_frame, &t->h_error };
    int i;
    for (i = 0; i < TCP_NHANDLERS; i++) {
        old[i] = *slot[i];
        *slot[i] = v[i];
        v[i] = JS_UNDEFINED;
    }
    {
        JSRuntime* rt = JS_GetRuntime(t->ctx);
        for (i = 0; i < TCP_NHANDLERS; i++)
            JS_FreeValueRT(rt, old[i]);
    }
}

int tcp_check_decoder(JSContext* ctx, dyn_tcp_t* t, const char* fn)
{
    if (t->spec && !JS_IsFunction(ctx, t->h_frame)) {
        JS_ThrowTypeError(ctx, "%s: a decoder needs a frame handler (frames replace data events)", fn);
        return -1;
    }
    return 0;
}

static JSValue dyn_tcp_server_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    int64_t port = 0;
    char* upath = NULL;
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        if (dyn_opts_strict(ctx, argv[0], tcp_srv_keys, (int)countof(tcp_srv_keys)))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "path");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* up = tcp_unix_path(ctx, v);
            JS_FreeValue(ctx, v);
            if (!up)
                return JS_EXCEPTION;
            upath = strdup(up);
            JS_FreeCString(ctx, up);
            if (!upath)
                return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (port < 0 || port > 65535) {
        free(upath);
        return JS_ThrowRangeError(ctx, "TCPServer: port must be 0..65535");
    }
    t = tcp_new(ctx);
    if (!t) {
        free(upath);
        return JS_EXCEPTION;
    }
    t->port = (uint16_t)port;
    if (tcp_read_bounds(ctx, t, o, 0) < 0) {
        free(upath);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (tcp_setup_tls_server(ctx, t, o) < 0) {
        free(upath);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    {
        JSValue tv = JS_IsObject(o) ? JS_GetPropertyStr(ctx, o, "tls")
                                    : JS_UNDEFINED;
        int asked = JS_IsException(tv) ? -1 : JS_IsObject(tv);
        JS_FreeValue(ctx, tv);
        if (asked) {
            free(upath);
            dyn_tcp_dispose(t);
            return asked < 0 ? JS_EXCEPTION : dyn_tls_missing(ctx, "TCPServer", "tls");
        }
    }
#endif
    t->path = upath;
    return dyn_res_wrap(ctx, new_target, dyn_tcp_class_id, t, dyn_tcp_dispose);
}

static JSValue dyn_tcp_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_tcp_t* t = (dyn_tcp_t*)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    if (!t)
        return JS_EXCEPTION;
    if (t->started)
        return JS_UNDEFINED;
    if (argc > 0) {
        JSValue hv[TCP_NHANDLERS];
        if (tcp_collect_handlers(ctx, argv[0], hv) < 0)
            return JS_EXCEPTION;
        t = (dyn_tcp_t*)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
        if (!t) {
            int i;
            for (i = 0; i < TCP_NHANDLERS; i++)
                JS_FreeValue(ctx, hv[i]);
            return JS_EXCEPTION;
        }
        tcp_install_handlers(t, hv);
    }
    if (tcp_check_decoder(ctx, t, t->path ? "IpcServer" : "TCPServer") < 0)
        return JS_EXCEPTION;
    t->listen_fd = t->path ? dyn_aio_unix_listen(t->aio, t->path, 128)
                           : dyn_aio_listen(t->aio, "0.0.0.0", t->port, 512);
    if (t->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "%s: listen failed",
            t->path ? "IpcServer" : "TCPServer");
    if (t->path) {
        struct stat st;
        if (lstat(t->path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            t->path_bound = 1;
            t->path_dev = st.st_dev;
            t->path_ino = st.st_ino;
        }
    }
    if (tcp_arm_sweep(t) < 0) {
        dyn_aio_close(t->aio, t->listen_fd);
        t->listen_fd = -1;
        return JS_ThrowInternalError(ctx,
            "%s: the backend cannot arm a clock, so idleTimeoutMs would "
            "never fire",
            t->path ? "IpcServer" : "TCPServer");
    }
    if (t->path) {
        dyn_aio_accept(t->aio, t->listen_fd, tcp_on_accept, t);
        t->started = 1;
        t->self_weak = this_val;
        tcp_pin_live(t);
        return JS_UNDEFINED;
    }
    if (getsockname(t->listen_fd, (struct sockaddr*)&sa, &sl) == 0)
        t->port = ntohs(sa.sin_port);
    dyn_aio_accept(t->aio, t->listen_fd, tcp_on_accept, t);
    t->started = 1;
    t->self_weak = this_val;
    tcp_pin_live(t);
    return JS_UNDEFINED;
}

static JSValue dyn_tcp_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_tcp_t* t = (dyn_tcp_t*)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    if (!t)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, t->port);
}

static const JSCFunctionListEntry dyn_tcp_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_tcp_start),
    JS_CGETSET_DEF("port", dyn_tcp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_statics[] = {
    JS_CFUNC_DEF("connect", 1, dyn_tcp_connect_method),
};

JSClassID dyn_udp_class_id;

static const JSClassDef dyn_udp_class = {
    "UDPSocket",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_udp_gc_mark,
};

static const JSCFunctionListEntry dyn_udp_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_udp_start),
    JS_CFUNC_DEF("send", 3, dyn_udp_send),
    JS_CGETSET_DEF("port", dyn_udp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_funcs[] = {
    JS_CFUNC_DEF("connectHappy", 2, dyn_tcp_connect_happy),
    JS_CFUNC_DEF("swallowedHandlerThrows", 0, dyn_net_handler_throws_getter),
};

int dyn_tcp_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;

    if (dyn_register_class(ctx, m, &dyn_tcp_class_id, &dyn_tcp_class,
            dyn_tcp_proto, countof(dyn_tcp_proto),
            dyn_tcp_server_ctor, "TCPServer")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyn_tcp_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsException(ctor))
        return -1;
    JS_SetPropertyFunctionList(ctx, ctor, dyn_tcp_statics,
        countof(dyn_tcp_statics));
    JS_FreeValue(ctx, ctor);

    if (dyn_register_class(ctx, m, &dyn_udp_class_id, &dyn_udp_class,
            dyn_udp_proto, countof(dyn_udp_proto),
            dyn_udp_ctor, "UDPSocket")
        < 0)
        return -1;

    if (dyn_frame_spec_register(ctx) < 0)
        return -1;
    JS_NewClassID(&dyn_tcp_conn_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), dyn_tcp_conn_class_id,
            &dyn_tcp_conn_class)
        < 0)
        return -1;
    {
        JSValue cp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, cp, dyn_tcp_conn_proto,
            countof(dyn_tcp_conn_proto));
        JS_SetClassProto(ctx, dyn_tcp_conn_class_id, cp);
    }
    if (JS_SetModuleExportList(ctx, m, dyn_frame_funcs, 3) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
}

void dyn_tcp_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "TCPServer");
    JS_AddModuleExport(ctx, m, "UDPSocket");
    JS_AddModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
    JS_AddModuleExportList(ctx, m, dyn_frame_funcs, 3);
}

#endif
