#include "net-tcp-internal.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

static void tcp_frames_clear(JSRuntime* rt, dyn_tcp_conn_t* c)
{
    while (c->fq_len > 0) {
        JSValue v = c->fq[c->fq_head];
        c->fq[c->fq_head] = JS_UNDEFINED;
        c->fq_head = (c->fq_head + 1) % c->fq_cap;
        c->fq_len--;
        JS_FreeValueRT(rt, v);
    }
    c->fq_head = 0;
}

void tcp_conn_release(JSRuntime* rt, dyn_tcp_conn_t* c)
{
    tcp_frames_clear(rt, c);
    free(c->fq);
    dyn_frame_free(c->dec);
#ifdef CONFIG_TLS
    dyn_tls_conn_free(c->tls);
#endif
    free(c);
}

static void tcp_conn_unref(JSContext* ctx, dyn_tcp_conn_t* c)
{
    if (--c->refs == 0) {
        JS_FreeValue(ctx, c->jsobj);
        tcp_conn_release(JS_GetRuntime(ctx), c);
    }
}

static int tcp_has_tap(dyn_tcp_conn_t* c)
{
    return JS_VALUE_GET_TAG(c->tap) == JS_TAG_OBJECT;
}

static void tcp_tap_call(JSContext* ctx, dyn_tcp_conn_t* c, int kind,
    JSValueConst bytes)
{
    JSValue tap = JS_DupValue(ctx, c->tap);
    JSValueConst a[2] = { JS_NewInt32(ctx, kind), bytes };
    dyn_tcp_t* own = c->owner;
    JSValue r;
    own->in_cb++;
    r = JS_Call(ctx, tap, JS_UNDEFINED, 2, a);
    own->in_cb--;
    JS_FreeValue(ctx, tap);
    dyn_net_handler_result(ctx, r, kind == 0 ? NET_HK_TCP_DATA : kind == 1 ? NET_HK_TCP_DRAIN : NET_HK_TCP_CLOSE);
}

void tcp_conn_drop(JSRuntime* rt, dyn_tcp_conn_t* c)
{
    JSValue tap = c->tap;
    c->tap = JS_UNDEFINED;
    JS_FreeValueRT(rt, tap);
    tcp_frames_clear(rt, c);
    JS_FreeValueRT(rt, c->jsobj);
    c->jsobj = JS_UNDEFINED;
    if (--c->refs == 0)
        tcp_conn_release(rt, c);
}

void dyn_tcp_conn_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(val, dyn_tcp_conn_class_id);
    (void)rt;
    if (c) {
        JSValue tap = c->tap;
        c->tap = JS_UNDEFINED;
        JS_FreeValueRT(rt, tap);
        tcp_frames_clear(rt, c);
        c->jsobj = JS_UNDEFINED;
        if (--c->refs == 0)
            tcp_conn_release(rt, c);
    }
}

void tcp_conn_close(dyn_tcp_conn_t* c)
{
    JSContext* ctx;
    dyn_aio_t* aio;
    dyn_tcp_t* own;
    JSValue h;
    int fd;

    if (c->closed)
        return;
    own = c->owner;
    ctx = own->ctx;
    aio = own->aio;
    fd = c->fd;
    h = JS_DupValue(ctx, own->h_close);

    c->closed = 1;
    tcp_list_unlink(c);
    dyn_aio_close(aio, fd);
    if (tcp_has_tap(c) && !JS_IsUndefined(c->jsobj)) {
        c->refs++;
        tcp_tap_call(ctx, c, 2, JS_UNDEFINED);
        if (tcp_gone(own)) {
            JS_FreeValue(ctx, h);
            tcp_conn_drop(JS_GetRuntime(ctx), c);
            tcp_conn_unref(ctx, c);
            return;
        }
        tcp_conn_unref(ctx, c);
    }
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
        JSValueConst a[1] = { c->jsobj };
        own->in_cb++;
        JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
        own->in_cb--;
        if (tcp_gone(own)) {
            dyn_net_handler_result(ctx, r, NET_HK_TCP_CLOSE);
            JS_FreeValue(ctx, h);
            tcp_conn_drop(JS_GetRuntime(ctx), c);
            return;
        }
        dyn_net_handler_result(ctx, r, NET_HK_TCP_CLOSE);
    }
    JS_FreeValue(ctx, h);
    tcp_conn_drop(JS_GetRuntime(ctx), c);
    tcp_client_idle(own);
}

void tcp_conn_sent(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_tcp_conn_t* c = (dyn_tcp_conn_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (!c->owner || res < 0 || c->closed || !c->drain_armed)
        return;
    if (dyn_aio_queued(c->owner->aio, c->fd) > 0)
        return;
    c->drain_armed = 0;
    if (tcp_has_tap(c) && !JS_IsUndefined(c->jsobj)) {
        dyn_tcp_t* own = c->owner;
        tcp_tap_call(own->ctx, c, 1, JS_UNDEFINED);
        tcp_gone(own);
        return;
    }
    {
        JSContext* ctx = c->owner->ctx;
        JSValue h = JS_DupValue(ctx, c->owner->h_drain);
        if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
            dyn_tcp_t* own = c->owner;
            JSValueConst a[1] = { c->jsobj };
            own->in_cb++;
            JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
            own->in_cb--;
            int gone = tcp_gone(own);
            dyn_net_handler_result(ctx, r, NET_HK_TCP_DRAIN);
            if (gone) {
                JS_FreeValue(ctx, h);
                return;
            }
        }
        JS_FreeValue(ctx, h);
    }
}

JSValue dyn_tcp_conn_buffered(JSContext* ctx, JSValueConst this_val)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)ctx;
    if (!c || c->closed || !c->owner)
        return JS_NewInt64(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)dyn_aio_queued(c->owner->aio, c->fd));
}

JSValue dyn_tcp_conn_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c;
    size_t len = 0, off = 0, bpe = 0;
    const uint8_t* p = NULL;
    const char* str = NULL;
    JSValue ab;
    int rc, bkind;

    bkind = JS_GetBufferKind(argv[0]);
    ab = bkind == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe) : JS_EXCEPTION;
    if (!JS_IsException(ab)) {
        size_t total = 0;
        uint8_t* base = JS_GetArrayBuffer(ctx, &total, ab);
        if (!base) {
            JS_FreeValue(ctx, ab);
            return JS_EXCEPTION;
        }
        p = base + off;
    } else {
        if (bkind == JS_BUFFER_KIND_VIEW)
            JS_FreeValue(ctx, JS_GetException(ctx));
        p = bkind == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &len, argv[0]) : NULL;
        if (!p) {
            if (bkind == JS_BUFFER_KIND_BUFFER)
                JS_FreeValue(ctx, JS_GetException(ctx));
            if (!JS_IsString(argv[0]))
                return JS_ThrowTypeError(ctx,
                    "TCPConn.write: data must be a string or a byte view "
                    "(BytesInput: string | Uint8Array | Int8Array | "
                    "Uint8ClampedArray | DataView | ArrayBuffer)");
            ab = JS_UNDEFINED;
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
            p = (const uint8_t*)str;
        } else {
            ab = JS_DupValue(ctx, argv[0]);
        }
    }

    c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    if (!c || c->closed) {
        if (str)
            JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "TCPConn: connection is closed");
    }
    if (c->hwm) {
        size_t queued = (size_t)dyn_aio_queued(c->owner->aio, c->fd);
        if (queued > c->hwm || len > c->hwm - queued) {
            if (str)
                JS_FreeCString(ctx, str);
            JS_FreeValue(ctx, ab);
            return JS_ThrowTypeError(ctx,
                "TCPConn: write refused: %llu bytes queued + %llu more "
                "exceeds the high-water mark (%llu); wait for drain",
                (unsigned long long)queued, (unsigned long long)len,
                (unsigned long long)c->hwm);
        }
    }
#ifdef CONFIG_TLS
    if (c->tls) {
        if (!c->tls_up) {
            if (str)
                JS_FreeCString(ctx, str);
            JS_FreeValue(ctx, ab);
            return JS_ThrowTypeError(ctx,
                "TCPConn: write before the TLS handshake completed "
                "-- write from the connect handler");
        }
        size_t woff = 0;
        rc = 0;
        while (woff < (size_t)len) {
            int w = dyn_tls_write(c->tls, p + woff, (size_t)len - woff);
            if (w <= 0) {
                rc = -1;
                break;
            }
            woff += (size_t)w;
            if (tls_flush(c) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc == 0 && dyn_aio_queued(c->owner->aio, c->fd) > 0)
            c->drain_armed = 1;
    } else
#endif
        rc = dyn_aio_send(c->owner->aio, c->fd, p, len,
            (JS_IsFunction(ctx, c->owner->h_drain) || tcp_has_tap(c) || len >= DYN_AIO_HOLD_DIRECT) ? 0 : DYN_AIO_HOLD, tcp_conn_sent, c);
    if (rc == 0 && dyn_aio_queued(c->owner->aio, c->fd) > 0)
        c->drain_armed = 1;
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, ab);
    if (rc < 0)
        return JS_ThrowInternalError(ctx, "TCPConn: send failed");
    return JS_UNDEFINED;
}

JSValue dyn_tcp_conn_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)ctx;
    (void)argc;
    (void)argv;
    if (c && !c->closed)
        tcp_conn_close(c);
    return JS_UNDEFINED;
}

static void tcp_frame_sink(void* ud, int ev, const uint8_t* p, uint64_t n)
{
    dyn_tcp_conn_t* c = (dyn_tcp_conn_t*)ud;
    JSContext* ctx = c->owner->ctx;
    JSValue u8;
    if (c->frame_err)
        return;
    if (c->fq_len == c->fq_cap) {
        size_t nc = c->fq_cap ? c->fq_cap * 2 : 8, i;
        JSValue* nq = (JSValue*)malloc(nc * sizeof(*nq));
        if (!nq) {
            c->frame_err = DYN_FRAME_ENOMEM;
            return;
        }
        for (i = 0; i < c->fq_len; i++)
            nq[i] = c->fq[(c->fq_head + i) % c->fq_cap];
        free(c->fq);
        c->fq = nq;
        c->fq_cap = nc;
        c->fq_head = 0;
    }
    if (ev) {
        char msg[200];
        int k = snprintf(msg, sizeof msg, "frame decoder: ");
        dyn_frame_describe(c->dec, ev, n, msg + k, sizeof msg - (size_t)k);
        u8 = JS_NewString(ctx, msg);
    } else {
        u8 = dyn_nat_u8_copy(ctx, p, (size_t)n);
    }
    if (JS_IsException(u8)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        c->frame_err = DYN_FRAME_ENOMEM;
        return;
    }
    c->fq[(c->fq_head + c->fq_len) % c->fq_cap] = u8;
    c->fq_len++;
}

static void tcp_frame_error(dyn_tcp_conn_t* c)
{
    dyn_tcp_t* own = c->owner;
    JSContext* ctx = own->ctx;
    JSValue h = JS_DupValue(ctx, own->h_error);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
        char msg[160];
        JSValue m, r;
        JSValueConst a[2];
        snprintf(msg, sizeof msg, "frame decoder: %s", dyn_frame_strerror(c->frame_err));
        m = JS_NewString(ctx, msg);
        a[0] = c->jsobj;
        a[1] = m;
        own->in_cb++;
        r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
        own->in_cb--;
        JS_FreeValue(ctx, m);
        if (tcp_gone(own)) {
            dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
            JS_FreeValue(ctx, h);
            return;
        }
        dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
    }
    JS_FreeValue(ctx, h);
    if (!c->closed)
        tcp_conn_close(c);
}

static void tcp_frames_pump(dyn_tcp_conn_t* c)
{
    JSContext* ctx;
    if (c->pumping || c->closed || !c->owner)
        return;
    ctx = c->owner->ctx;
    c->pumping = 1;
    c->refs++;
    while (!c->closed && !c->paused && c->owner && c->fq_len > 0) {
        dyn_tcp_t* own = c->owner;
        JSValue v = c->fq[c->fq_head];
        JSValue h = JS_DupValue(ctx, own->h_frame);
        c->fq[c->fq_head] = JS_UNDEFINED;
        c->fq_head = (c->fq_head + 1) % c->fq_cap;
        c->fq_len--;
        c->last_ms = dyn_timer_now_ms();
        if (JS_IsString(v)) {
            JS_FreeValue(ctx, h);
            h = JS_DupValue(ctx, own->h_error);
        }
        if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
            JSValueConst a[2] = { c->jsobj, v };
            JSValue r;
            own->in_cb++;
            r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
            own->in_cb--;
            dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
        }
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, v);
        if (tcp_gone(own))
            break;
    }
    if (!c->closed && c->owner && !c->paused && c->fq_len == 0) {
        if (c->frame_err && !c->frame_err_told)
            tcp_frame_error(c);
        else if (c->frame_err)
            tcp_conn_close(c);
        else if (c->eof_pending)
            tcp_conn_close(c);
    }
    c->pumping = 0;
    tcp_conn_unref(ctx, c);
}

static void tcp_frames_eof(dyn_tcp_conn_t* c)
{
    c->eof_pending = 1;
    if (!c->frame_err) {
        int fr = dyn_frame_finish(c->dec, tcp_frame_sink, c);
        if (fr < 0 && !c->frame_err) {
            c->frame_err = fr;
            c->frame_err_told = 1;
        }
    }
    tcp_frames_pump(c);
}

static void tcp_deliver_frames(dyn_tcp_conn_t* c, const uint8_t* buf, unsigned len)
{
    int r;
    c->last_ms = dyn_timer_now_ms();
    if (!c->frame_err) {
        r = dyn_frame_feed(c->dec, buf, len, tcp_frame_sink, c);
        if (r < 0 && !c->frame_err) {
            c->frame_err = r;
            c->frame_err_told = 1;
        }
    }
    tcp_frames_pump(c);
}

static void tcp_deliver(dyn_tcp_conn_t* c, const uint8_t* buf, unsigned len)
{
    JSContext* ctx;
    if (c->closed || !c->owner)
        return;
    if (c->dec) {
        tcp_deliver_frames(c, buf, len);
        return;
    }
    ctx = c->owner->ctx;
    if ((tcp_has_tap(c) || JS_IsFunction(ctx, c->owner->h_data)) && !JS_IsUndefined(c->jsobj)) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, buf, len);
        if (!JS_IsException(ab)) {
            JSValueConst ta[3] = { ab, JS_NewInt32(ctx, 0),
                JS_NewInt32(ctx, (int)len) };
            JSValue u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
            if (!JS_IsException(u8)) {
                JSValueConst a[2] = { c->jsobj, u8 };
                dyn_tcp_t* own = c->owner;
                int gone;
                JSValue r;
                c->last_ms = dyn_timer_now_ms();
                if (tcp_has_tap(c)) {
                    tcp_tap_call(ctx, c, 0, u8);
                    gone = tcp_gone(own);
                } else {
                    own->in_cb++;
                    r = JS_Call(ctx, own->h_data, JS_UNDEFINED, 2, a);
                    own->in_cb--;
                    gone = tcp_gone(own);
                    dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
                }
                JS_FreeValue(ctx, u8);
                JS_FreeValue(ctx, ab);
                (void)gone;
                return;
            }
            JS_FreeValue(ctx, u8);
        }
        JS_FreeValue(ctx, ab);
    }
}

static void tcp_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_tcp_conn_t* c = (dyn_tcp_conn_t*)ud;
    (void)aio;
    if (c->closed || !c->owner)
        return;
    JSContext* ctx = c->owner->ctx;

    if (res == 0 && tcp_has_tap(c) && !JS_IsUndefined(c->jsobj)) {
        dyn_aio_cancel(c->owner->aio, tcp_on_recv, c);
        c->paused = 1;
        c->eof = 1;
        dyn_tcp_t* own = c->owner;
        c->refs++;
        tcp_tap_call(ctx, c, 3, JS_UNDEFINED);
        tcp_gone(own);
        tcp_conn_unref(ctx, c);
        return;
    }
    if (res == 0 && c->dec && !JS_IsUndefined(c->jsobj)) {
        dyn_aio_cancel(c->owner->aio, tcp_on_recv, c);
        tcp_frames_eof(c);
        return;
    }
    if (res <= 0) {
        tcp_conn_close(c);
        return;
    }
    c->refs++;
#ifdef CONFIG_TLS
    if (c->tls) {
        uint8_t pt[16384] DYN_IO_SCRATCH;
        int n = 0;

        if (dyn_tls_feed(c->tls, buf, len) != 0) {
            tcp_conn_close(c);
            tcp_conn_unref(ctx, c);
            return;
        }
        if (!c->tls_up) {
            int st = dyn_tls_handshake(c->tls);
            if (tls_flush(c) < 0)
                st = -1;
            if (st < 0) {
                tls_fail(c, "TLS handshake failed");
                tcp_conn_unref(ctx, c);
                return;
            }
            if (st == 0) {
                tcp_conn_unref(ctx, c);
                return;
            }
            c->tls_up = 1;
            c->last_ms = dyn_timer_now_ms();
            tcp_report_connect(c->owner, c->jsobj, NULL);
        }
        c->delivering = 1;
        while (!c->paused && !c->closed && (n = dyn_tls_read(c->tls, pt, sizeof pt)) > 0)
            tcp_deliver(c, pt, (unsigned)n);
        c->delivering = 0;
        if (n < 0 && !c->closed && c->dec && dyn_tls_peer_closed(c->tls)) {
            dyn_aio_cancel(c->owner->aio, tcp_on_recv, c);
            tcp_frames_eof(c);
            tcp_conn_unref(ctx, c);
            return;
        }
        if (n < 0 && !c->closed) {
            tcp_conn_close(c);
            tcp_conn_unref(ctx, c);
            return;
        }
        tls_flush(c);
        tcp_conn_unref(ctx, c);
        return;
    }
#endif
    tcp_deliver(c, buf, len);
    tcp_conn_unref(ctx, c);
}

static int tcp_recv_arm(dyn_tcp_conn_t* c)
{
    return dyn_aio_recv(c->owner->aio, c->fd, 0, 1, tcp_on_recv, c);
}

JSValue dyn_tcp_conn_pause(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)argc;
    (void)argv;
    if (!c || c->closed || !c->owner || c->eof)
        return JS_UNDEFINED;
#ifdef CONFIG_TLS
    if (c->tls && !c->tls_up)
        return JS_ThrowTypeError(ctx, "TCPConn: pause/resume before the TLS handshake completed");
#endif
    if (magic == 0) {
#ifdef CONFIG_IO_URING
        return JS_UNDEFINED;
#endif
        if (c->paused)
            return JS_UNDEFINED;
        dyn_aio_cancel(c->owner->aio, tcp_on_recv, c);
        c->paused = 1;
        return JS_UNDEFINED;
    }
    if (!c->paused)
        return JS_UNDEFINED;
    c->paused = 0;
    c->last_ms = dyn_timer_now_ms();
    if (c->dec) {
        tcp_frames_pump(c);
        if (c->closed || c->paused || c->eof_pending)
            return JS_UNDEFINED;
    }
#ifdef CONFIG_TLS
    if (c->tls && !c->delivering) {
        uint8_t pt[16384] DYN_IO_SCRATCH;
        int n = 0;
        c->refs++;
        c->delivering = 1;
        while (!c->paused && !c->closed && (n = dyn_tls_read(c->tls, pt, sizeof pt)) > 0)
            tcp_deliver(c, pt, (unsigned)n);
        c->delivering = 0;
        if (n < 0 && !c->closed && c->dec && dyn_tls_peer_closed(c->tls))
            tcp_frames_eof(c);
        else if (n < 0 && !c->closed)
            tcp_conn_close(c);
        if (c->closed || c->paused || c->eof_pending) {
            tcp_conn_unref(ctx, c);
            return JS_UNDEFINED;
        }
        tcp_conn_unref(ctx, c);
    }
#endif
    if (tcp_recv_arm(c) < 0) {
        tcp_conn_close(c);
        return JS_ThrowInternalError(ctx, "TCPConn: resume failed");
    }
    return JS_UNDEFINED;
}

JSValue dyn_tcp_conn_paused(JSContext* ctx, JSValueConst this_val)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    return JS_NewBool(ctx, c && !c->closed && c->paused);
}

JSValue dyn_tcp_conn_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)argc;
    (void)argv;
    if (!c || c->closed || !c->owner)
        return JS_UNDEFINED;
    if (dyn_aio_queued(c->owner->aio, c->fd) > 0)
        return JS_ThrowTypeError(ctx, "TCPConn: end() with bytes still queued; wait for drain");
    shutdown(c->fd, SHUT_WR);
    return JS_UNDEFINED;
}

JSValue dyn_tcp_conn_streams(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    JSValueConst args[3];
    JSValue r, tap, out;

    if (!c || c->closed || !c->owner)
        return JS_ThrowTypeError(ctx, "TCPConn: connection is closed");
    if (tcp_has_tap(c))
        return JS_ThrowTypeError(ctx, "TCPConn: streams() was already called for this connection");
    if (c->dec)
        return JS_ThrowTypeError(ctx, "TCPConn: streams() is byte-mode only; this connection has a frame decoder (frames go to the frame handler)");
#ifdef CONFIG_TLS
    if (c->tls && !c->tls_up)
        return JS_ThrowTypeError(ctx, "TCPConn: streams() before the TLS handshake completed -- call it from the connect handler");
#endif
    args[0] = this_val;
    args[1] = argc > 0 ? argv[0] : JS_UNDEFINED;
    args[2] = JS_NewInt64(ctx, (int64_t)c->hwm);
    r = js_streams_call(ctx, "socketStreams", 3, args);
    if (JS_IsException(r))
        return r;
    tap = JS_GetPropertyStr(ctx, r, "tap");
    if (JS_IsException(tap) || !JS_IsFunction(ctx, tap)) {
        int thrown = JS_IsException(tap);
        JS_FreeValue(ctx, tap);
        JS_FreeValue(ctx, r);
        return thrown ? JS_EXCEPTION : JS_ThrowInternalError(ctx, "TCPConn: streams adapter returned no tap");
    }
    out = JS_NewObject(ctx);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, tap);
        JS_FreeValue(ctx, r);
        return out;
    }
    {
        static const char* const sides[2] = { "readable", "writable" };
        int k;
        for (k = 0; k < 2; k++) {
            JSValue s = JS_GetPropertyStr(ctx, r, sides[k]);
            if (JS_IsException(s) || JS_SetPropertyStr(ctx, out, sides[k], s) < 0) {
                JS_FreeValue(ctx, tap);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, out);
                return JS_EXCEPTION;
            }
        }
    }
    JS_FreeValue(ctx, r);
    if (c->closed) {
        JS_FreeValue(ctx, tap);
        return out;
    }
    c->tap = tap;
    return out;
}

void dyn_tcp_conn_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(val, dyn_tcp_conn_class_id);
    size_t i;
    if (!c)
        return;
    JS_MarkValue(rt, c->tap, mark_func);
    for (i = 0; i < c->fq_len; i++)
        JS_MarkValue(rt, c->fq[(c->fq_head + i) % c->fq_cap], mark_func);
}

JSValue dyn_tcp_conn_set_decoder(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    dyn_frame_spec_t* s;
    if (!c || c->closed || !c->owner)
        return JS_ThrowTypeError(ctx, "TCPConn: connection is closed");
    s = frame_spec_from(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "TCPConn.setDecoder");
    if (!s)
        return JS_EXCEPTION;
    if (c->dec)
        return JS_ThrowTypeError(ctx, "TCPConn.setDecoder: this connection already has a frame decoder");
    if (tcp_has_tap(c))
        return JS_ThrowTypeError(ctx, "TCPConn.setDecoder: streams() owns this connection's bytes");
    if (!JS_IsFunction(ctx, c->owner->h_frame))
        return JS_ThrowTypeError(ctx, "TCPConn.setDecoder: the handlers have no frame function");
    c->dec = frame_spec_instantiate(s);
    if (!c->dec)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

int tcp_conn_start(dyn_tcp_t* t, dyn_tcp_conn_t* c)
{
    JSContext* ctx = t->ctx;
    if (t->spec && !c->dec)
        c->dec = frame_spec_instantiate(t->spec);
    if (t->spec && !c->dec) {
#ifdef CONFIG_TLS
        dyn_tls_conn_free(c->tls);
        c->tls = NULL;
#endif
        dyn_aio_close(t->aio, c->fd);
        free(c);
        return -1;
    }
    c->jsobj = JS_NewObjectClass(ctx, (int)dyn_tcp_conn_class_id);
    if (JS_IsException(c->jsobj)) {
        c->jsobj = JS_UNDEFINED;
#ifdef CONFIG_TLS
        dyn_tls_conn_free(c->tls);
        c->tls = NULL;
#endif
        dyn_frame_free(c->dec);
        dyn_aio_close(t->aio, c->fd);
        free(c);
        return -1;
    }
    JS_SetOpaque(c->jsobj, c);
    c->refs++;
    c->last_ms = dyn_timer_now_ms();
    c->lnext = t->conns;
    if (t->conns)
        t->conns->lprev = c;
    t->conns = c;
    t->nconns++;
    if (tcp_recv_arm(c) < 0) {
        tcp_conn_close(c);
        return -1;
    }
    return 0;
}

#endif
