#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_APP_SSE_MAX 1024

static const char* const http_sse_handler_keys[] = { "open", "close" };

void dyn_sse_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(val, dyn_sse_class_id);
    (void)rt;
    free(s);
}

static int dyn_sse_frame(dyn_iobuf_t* f, const char* data, size_t dlen,
    const char* event, size_t elen)
{
    size_t i = 0;

    if (event) {
        if (dyn_iobuf_append(f, "event: ", 7) < 0
            || dyn_iobuf_append(f, event, elen) < 0
            || dyn_iobuf_append(f, "\n", 1) < 0)
            return -1;
    }
    for (;;) {
        size_t ls = i;
        while (i < dlen && data[i] != '\n')
            i++;
        if (dyn_iobuf_append(f, "data: ", 6) < 0
            || dyn_iobuf_append(f, data + ls, i - ls) < 0
            || dyn_iobuf_append(f, "\n", 1) < 0)
            return -1;
        if (i >= dlen)
            break;
        i++;
    }
    return dyn_iobuf_append(f, "\n", 1);
}

JSValue dyn_sse_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(this_val, dyn_sse_class_id);
    const char *data = NULL, *event = NULL;
    size_t dlen = 0, elen = 0;
    dyn_iobuf_t f;

    if (!s)
        return JS_ThrowTypeError(ctx, "not an SseConn");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "send(data[, event])");
    data = JS_ToCStringLen(ctx, &dlen, argv[0]);
    if (!data)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        event = JS_ToCStringLen(ctx, &elen, argv[1]);
        if (!event) {
            JS_FreeCString(ctx, data);
            return JS_EXCEPTION;
        }
        if (memchr(event, '\n', elen) || memchr(event, '\r', elen)) {
            JS_FreeCString(ctx, event);
            JS_FreeCString(ctx, data);
            return JS_ThrowTypeError(ctx, "SseConn.send: an event name cannot "
                                          "contain a newline");
        }
    }
    if (memchr(data, '\r', dlen)) {
        if (event)
            JS_FreeCString(ctx, event);
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "SseConn.send: data cannot contain \\r");
    }
    if (s->conn && !s->conn->closed) {
        dyn_iobuf_init(&f);
        if (dyn_sse_frame(&f, data, dlen, event, elen) < 0) {
            dyn_app_conn_close(s->conn);
        } else if (dyn_aio_queued(s->conn->app->aio, s->conn->fd) + f.len
            > DYN_HTTP_OUTBOUND_MAX) {
            dyn_app_conn_close(s->conn);
        } else {
            dyn_aio_send(s->conn->app->aio, s->conn->fd, f.data, f.len, DYN_AIO_HOLD,
                NULL, NULL);
            s->conn->last_ms = dyn_timer_now_ms();
        }
        dyn_iobuf_free(&f);
    } else {
        s->after_close += (int64_t)dlen;
    }
    if (event)
        JS_FreeCString(ctx, event);
    JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

JSValue dyn_sse_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(this_val, dyn_sse_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_ThrowTypeError(ctx, "not an SseConn");
    if (s->conn && !s->conn->closed)
        dyn_app_conn_close(s->conn);
    return JS_UNDEFINED;
}

JSValue dyn_sse_get_buffered(JSContext* ctx, JSValueConst this_val)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(this_val, dyn_sse_class_id);
    if (!s)
        return JS_ThrowTypeError(ctx, "not an SseConn");
    if (!s->conn || s->conn->closed)
        return JS_NewInt64(ctx, s->after_close);
    return JS_NewInt64(ctx, (int64_t)dyn_aio_queued(s->conn->app->aio, s->conn->fd));
}

void dyn_app_sse_handshake(dyn_app_conn_t* c,
    const dyn_app_route_t* rt)
{
    JSContext* ctx = c->app->ctx;
    static const char sl[] = "HTTP/1.1 200 OK\r\n";
    static const char rest[] = "Content-Type: text/event-stream\r\n"
                               "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
    char hdr[sizeof(sl) + HTTPK_DATE_LINE_LEN + sizeof(rest)];
    const char* date = dyn_http_date(NULL, 0);
    size_t dl = strlen(date), hl;
    dyn_sse_t* s;
    JSValue obj, oh;

    if (c->app->n_sse >= DYN_APP_SSE_MAX) {
        dyn_app_send_err(c, 503, "{\"error\":\"too many event streams\"}");
        dyn_app_conn_close(c);
        return;
    }
    s = (dyn_sse_t*)malloc(sizeof(*s));
    if (!s) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    s->conn = c;
    s->after_close = 0;
    c->sse_native = s;
    obj = JS_NewObjectClass(ctx, dyn_sse_class_id);
    JS_SetOpaque(obj, s);
    c->sse_this = obj;
    hl = sizeof(sl) - 1;
    memcpy(hdr, sl, hl);
    memcpy(hdr + hl, date, dl);
    hl += dl;
    memcpy(hdr + hl, rest, sizeof(rest) - 1);
    hl += sizeof(rest) - 1;
    dyn_aio_send(c->app->aio, c->fd, hdr, hl, DYN_AIO_HOLD, NULL, NULL);
    c->is_sse = 1;
    c->app->n_sse++;
    c->sse_handlers = JS_DupValue(ctx, rt->handler);
    dyn_app_met_response(c, 0);
    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
    }
    JS_FreeValue(ctx, oh);
}

static void dyn_app_stream_stop(dyn_app_conn_t* c);

static void dyn_app_stream_finished(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len,
    void* udata);

static void dyn_app_stream_stop(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;
    if (!st)
        return;
    c->st = NULL;
    JS_FreeValue(ctx, st->src);
    JS_FreeValue(ctx, st->readfn);
    JS_FreeValue(ctx, st->chunk);
    JS_FreeValue(ctx, st->pending);
    free(st->frame);
    free(st->ct);
    free(st);
    if (!c->closed)
        dyn_app_pump_resume(c);
    dyn_app_conn_unref(c);
}

void dyn_app_stream_enter(dyn_app_stream_t* st) { st->depth++; }

static void dyn_app_stream_reap(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st || !st->retiring || st->depth > 0 || !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_stop(c);
}

void dyn_app_stream_retire(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st)
        return;
    st->done = 1;
    st->retiring = 1;
    dyn_app_stream_reap(c);
}

void dyn_app_stream_leave(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st || --st->depth > 0)
        return;
    dyn_app_stream_reap(c);
}

void dyn_app_stream_abort(dyn_app_conn_t* c)
{
    if (!c->st)
        return;
    dyn_app_stream_retire(c);
}

static int dyn_app_stream_on_count(dyn_app_conn_t* c, double n)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;

    if (n < 0 || n > (double)DYN_APP_STREAM_CHUNK)
        return -1;
    if (n == 0) {
        JSValue cf = JS_GetPropertyStr(ctx, st->src, "close");
        if (JS_IsFunction(ctx, cf))
            dyn_call_drop(ctx, cf, st->src, 0, NULL);
        JS_FreeValue(ctx, cf);
        if (st->done || c->closed)
            return 0;
        st->done = 1;
        memcpy(st->frame, "0\r\n\r\n", 5);
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame, 5, DYN_AIO_HOLD,
                dyn_app_stream_finished, c)
            < 0) {
            st->in_send = 0;
            dyn_app_stream_finished(NULL, -ECONNRESET, NULL, 0, c);
            return 0;
        }
        return (st->in_send || st->retiring) ? 0 : 1;
    }
    {
        size_t un = (size_t)n;
        size_t voff, vlen, vbpe, ab_size = 0;
        int hl;
        JSValue ab;
        uint8_t* base;
        ab = JS_GetArrayBufferView(ctx, st->chunk, &voff, &vlen, &vbpe);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return -1;
        if (un > vlen || voff > ab_size || un > ab_size - voff)
            return -1;
        hl = snprintf((char*)st->frame, 16, "%zx\r\n", un);
        if (hl <= 0 || (size_t)hl + un + 2 > st->frame_cap)
            return -1;
        memcpy(st->frame + hl, base + voff, un);
        st->frame[hl + un] = '\r';
        st->frame[hl + un + 1] = '\n';
        st->total += un;
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame,
                (unsigned)(hl + un + 2), DYN_AIO_HOLD,
                dyn_app_stream_sent, c)
            < 0) {
            st->in_send = 0;
            return -1;
        }
        return (st->in_send || st->retiring) ? 0 : 1;
    }
}

static void dyn_app_stream_fail(dyn_app_conn_t* c)
{
    if (!c->st || c->st->done || c->closed)
        return;
    c->st->done = 1;
    dyn_app_conn_close(c);
    dyn_app_stream_retire(c);
}

static JSValue dyn_app_stream_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    int64_t ptr = 0;
    dyn_app_conn_t* c;
    dyn_app_stream_t* st;
    double n = -1;
    (void)this_val;
    if (JS_ToInt64(ctx, &ptr, data[0]))
        return JS_EXCEPTION;
    c = (dyn_app_conn_t*)(uintptr_t)ptr;
    st = c->st;
    if (!st)
        return JS_UNDEFINED;
    dyn_app_stream_enter(st);
    JS_FreeValue(ctx, st->pending);
    st->pending = JS_UNDEFINED;
    if (st->done || c->closed) {
        dyn_app_stream_retire(c);
    } else {
        st->busy = 0;
        if (!magic && argc > 0 && JS_IsNumber(argv[0])) {
            if (JS_ToFloat64(ctx, &n, argv[0]) < 0) {
                dyn_app_stream_leave(c);
                return JS_EXCEPTION;
            }
        }
        {
            int keep = magic ? -1 : dyn_app_stream_on_count(c, n);
            if (keep == 1)
                dyn_app_stream_resume(c);
            else if (keep != 0)
                dyn_app_stream_fail(c);
        }
    }
    dyn_app_stream_leave(c);
    return JS_UNDEFINED;
}

void dyn_app_stream_sent(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_stream_t* st = c->st;
    int inline_cb;
    (void)aio;
    (void)buf;
    (void)len;
    if (!st)
        return;
    inline_cb = st->depth > 0;
    dyn_app_stream_enter(st);
    if (res < 0) {
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
    } else {
        st->in_send = 0;
        if (!inline_cb)
            dyn_app_stream_resume(c);
    }
    dyn_app_stream_leave(c);
}

static void dyn_app_stream_finished(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len,
    void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_stream_t* st = c->st;
    (void)aio;
    (void)buf;
    (void)len;
    if (!st)
        return;
    dyn_app_stream_enter(st);
    if (res < 0) {
        dyn_app_conn_close(c);
    } else {
        st->in_send = 0;
        st->done = 1;
        dyn_app_met_response(c, (double)st->total);
        if (c->close_after)
            dyn_app_conn_close(c);
    }
    dyn_app_stream_retire(c);
    dyn_app_stream_leave(c);
}

void dyn_app_stream_resume(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;

    if (!st || st->busy || !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_enter(st);
    st->busy = 1;
    while (!st->done && !c->closed) {
        JSValue pr, v;
        JSPromiseStateEnum ps;
        double n = -1;
        int keep;

        pr = JS_Call(ctx, st->readfn, st->src, 1, (JSValueConst*)&st->chunk);
        if (JS_IsException(pr)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;
        }
        if (st->done || c->closed) {
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;
        }
        ps = JS_PromiseState(ctx, pr);
        if (ps == JS_PROMISE_REJECTED) {
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;
        }
        if (ps == JS_PROMISE_PENDING) {
            JSValue funcs[2], promise, pthen, dptr, onres, onrej, tr;
            JSValueConst thenargs[2];
            promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(promise)) {
                dyn_app_read_abandon(ctx, pr);
                JS_FreeValue(ctx, pr);
                break;
            }
            dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)c);
            onres = JS_NewCFunctionData(ctx, dyn_app_stream_settle, 1, 0,
                1, &dptr);
            onrej = JS_NewCFunctionData(ctx, dyn_app_stream_settle, 1, 1,
                1, &dptr);
            JS_FreeValue(ctx, dptr);
            pthen = JS_GetPropertyStr(ctx, promise, "then");
            thenargs[0] = onres;
            thenargs[1] = onrej;
            tr = JS_Call(ctx, pthen, promise, 2, thenargs);
            JS_FreeValue(ctx, pthen);
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            if (JS_IsException(tr)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, funcs[0]);
                JS_FreeValue(ctx, funcs[1]);
                JS_FreeValue(ctx, promise);
                dyn_app_read_abandon(ctx, pr);
                JS_FreeValue(ctx, pr);
                break;
            }
            JS_FreeValue(ctx, tr);
            if (dyn_app_read_attach(ctx, pr, funcs[0], funcs[1]) < 0) {
                JSValue undef = JS_UNDEFINED;
                tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &undef);
                JS_FreeValue(ctx, tr);
            }
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            st->pending = pr;
            st->busy = 0;
            goto out;
        }
        v = JS_PromiseResult(ctx, pr);
        JS_FreeValue(ctx, pr);
        if (JS_IsNumber(v)) {
            if (JS_ToFloat64(ctx, &n, v) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, v);
                break;
            }
        }
        JS_FreeValue(ctx, v);
        keep = dyn_app_stream_on_count(c, n);
        if (keep == 1)
            continue;
        if (keep == 0) {
            st->busy = 0;
            goto out;
        }
        break;
    }
    st->busy = 0;
    dyn_app_stream_fail(c);
out:
    dyn_app_stream_leave(c);
}

JSValue dyn_app_sse_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "sse(path, {open,close})");
    if (dyn_opts_strict(ctx, argv[1], http_sse_handler_keys, 2))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_SSE;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}
#endif
