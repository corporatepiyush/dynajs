#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
typedef struct wsc_hs wsc_hs_t;

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    int fd;
    JSValue handlers;
    JSValue self;
    dyn_iobuf_t in;
    dyn_iobuf_t frag;
    int frag_op, frag_frames, ctl_budget;
    int refs;
    int closed;
    int close_sent;
    wsc_hs_t* hs;
    int dispose_deferred;
    int release_deferred;
} dyn_wsc_t;

struct wsc_hs {
    dyn_wsc_t* w;
    char* url;
    dyn_url_t u;
    char* http_url;
    char key_b64[32], accept[32];
    dyn_bytes_t req;
    int fd;
    char head[16384];
    size_t got, hdr_end;
    int err;
    int connected;
    int in_tls_handshake;
    int tls_attached;
    const uint8_t* extra;
    size_t extra_len;
    uint64_t deadline_ms;
    struct wsc_hs* next;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    dyn_tls_ctx_t* tls_ctx;
    char tls_err[192];
#endif
};

static void dyn_wsc_close_internal(dyn_wsc_t* w, int code, const char* reason);

static void dyn_wsc_unref(dyn_wsc_t* w)
{
    if (--w->refs != 0)
        return;
    if (w->hs) {
        w->dispose_deferred = 1;
        return;
    }
    dyn_iobuf_free(&w->in);
    dyn_iobuf_free(&w->frag);
    free(w);
}

static void dyn_wsc_send_frame(dyn_wsc_t* w, int opcode, const uint8_t* data,
    size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10], mask[4];
    size_t hn, i, off;

    if (w->closed)
        return;
    if (dyn_aio_queued(w->aio, w->fd) + len + 16 > DYN_HTTP_OUTBOUND_MAX) {
        dyn_wsc_close_internal(w, 1008, "output backpressure");
        return;
    }
    if (dyn_os_entropy(mask, sizeof mask) < 0) {
        w->closed = 1;
        return;
    }
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (uint8_t)(opcode & 0x0f);
    if (len < 126) {
        h[1] = 0x80 | (uint8_t)len;
        hn = 2;
    } else if (len <= 0xffff) {
        h[1] = 0x80 | 126;
        h[2] = (len >> 8) & 0xff;
        h[3] = len & 0xff;
        hn = 4;
    } else {
        h[1] = 0x80 | 127;
        for (i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
        hn = 10;
    }
    if (dyn_iobuf_append(&f, h, hn) < 0 || dyn_iobuf_append(&f, mask, 4) < 0) {
        dyn_iobuf_free(&f);
        dyn_wsc_close_internal(w, 1011, "out of memory while sending");
        return;
    }
    off = f.len;
    if (len > 0 && dyn_iobuf_append(&f, data, len) < 0) {
        dyn_iobuf_free(&f);
        dyn_wsc_close_internal(w, 1011, "out of memory while sending");
        return;
    }
    if (len) {
        uint8_t* p;
        uint32_t m32;
        uint64_t m64;
        size_t k = 0, n8 = len & ~(size_t)7;
        p = f.data + off;
        memcpy(&m32, mask, 4);
        m64 = ((uint64_t)m32 << 32) | m32;
        for (; k < n8; k += 8) {
            uint64_t v;
            memcpy(&v, p + k, 8);
            v ^= m64;
            memcpy(p + k, &v, 8);
        }
        for (i = k; i < len; i++)
            p[i] ^= mask[i & 3];
    }
    dyn_aio_send(w->aio, w->fd, f.data, f.len, DYN_AIO_HOLD, NULL, NULL);
    dyn_iobuf_free(&f);
}

static void dyn_wsc_dispatch_msg(dyn_wsc_t* w, int opcode,
    const uint8_t* payload, size_t plen)
{
    JSContext* ctx = w->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, w->handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        JS_FreeValue(ctx, mh);
        dyn_wsc_close_internal(w, 1007, "invalid UTF-8 in text message");
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? dyn_nat_u8_copy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char*)payload, plen);
        JSValueConst args[3] = { w->self, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

static void dyn_wsc_process(dyn_wsc_t* w)
{
    if (w->closed)
        return;
    for (;;) {
        uint8_t* p = dyn_iobuf_rdata(&w->in);
        size_t avail = dyn_iobuf_rlen(&w->in);
        int fin, opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t* payload;

        if (avail < 2)
            return;
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        plen = p[1] & 0x7f;
        hdr = 2;
        if (plen == 126) {
            if (avail < 4)
                return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10)
                return;
            plen = 0;
            for (i = 0; i < 8; i++)
                plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (masked || (opcode >= 0x8 && (plen > 125 || !fin))) {
            dyn_wsc_close_internal(w, 1002, "protocol error");
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) {
            dyn_wsc_close_internal(w, 1009, "too big");
            return;
        }
        if (avail < hdr + plen)
            return;
        payload = p + hdr;
        frame_total = hdr + (size_t)plen;

        if (opcode == 0x8) {
            int code = 1000;
            if (plen >= 2)
                code = (payload[0] << 8) | payload[1];
            if (!w->close_sent) {
                w->close_sent = 1;
                dyn_wsc_send_frame(w, 8, payload, (size_t)plen);
            }
            dyn_iobuf_consume(&w->in, frame_total);
            dyn_wsc_close_internal(w, code, "");
            return;
        } else if (opcode == 0x9) {
            if (--w->ctl_budget < 0) {
                dyn_wsc_close_internal(w, 1002, "control flood");
                return;
            }
            dyn_wsc_send_frame(w, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {
        } else if (opcode <= 0x2) {
            int frag_active = w->frag_op != 0;
            if ((opcode == 0x0 && !frag_active)
                || (opcode != 0x0 && frag_active)) {
                dyn_wsc_close_internal(w, 1002, "protocol error");
                return;
            }
            if (opcode != 0x0 && fin) {
                w->ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_wsc_dispatch_msg(w, opcode, payload, (size_t)plen);
            } else {
                if (opcode != 0x0) {
                    w->frag_op = opcode;
                    w->frag_frames = 0;
                }
                if (++w->frag_frames > DYN_WS_MAX_FRAGMENTS || dyn_iobuf_rlen(&w->frag) + plen > DYN_ACONN_MAX_REQ || dyn_iobuf_append(&w->frag, payload, (size_t)plen) < 0) {
                    dyn_wsc_close_internal(w, 1009, "too big");
                    return;
                }
                if (fin) {
                    w->ctl_budget = DYN_WS_CTL_BUDGET;
                    dyn_wsc_dispatch_msg(w, w->frag_op,
                        dyn_iobuf_rdata(&w->frag),
                        dyn_iobuf_rlen(&w->frag));
                    dyn_iobuf_reset(&w->frag);
                    w->frag_op = 0;
                    w->frag_frames = 0;
                }
            }
        }
        dyn_iobuf_consume(&w->in, frame_total);
        if (w->closed)
            return;
    }
}

static void dyn_wsc_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_wsc_t* w = (dyn_wsc_t*)ud;
    (void)aio;
    if (res <= 0) {
        dyn_wsc_close_internal(w, 1006, "abnormal closure");
        return;
    }
    w->refs++;
    if (dyn_iobuf_append(&w->in, buf, len) < 0
        || dyn_iobuf_rlen(&w->in) > DYN_ACONN_MAX_REQ) {
        dyn_wsc_close_internal(w, 1009, "too big");
        dyn_wsc_unref(w);
        return;
    }
    dyn_wsc_process(w);
    dyn_wsc_unref(w);
}

static void dyn_wsc_shutdown_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_wsc_t* w = (dyn_wsc_t*)opaque;
    JSValue h = w->handlers, s = w->self;
    (void)ctx;
    w->handlers = JS_UNDEFINED;
    w->self = JS_UNDEFINED;
    JS_FreeValueRT(rt, h);
    JS_FreeValueRT(rt, s);
}

static void dyn_wsc_close_internal(dyn_wsc_t* w, int code, const char* reason)
{
    JSContext* ctx;
    JSValue ch;

    if (w->closed)
        return;
    w->closed = 1;
    w->refs++;
    ctx = w->ctx;
    if (w->fd >= 0) {
        dyn_aio_close(w->aio, w->fd);
        w->fd = -1;
    }
    JS_RemoveShutdownSweep(JS_GetRuntime(ctx), dyn_wsc_shutdown_sweep, w);
    ch = JS_IsObject(w->handlers) ? JS_GetPropertyStr(ctx, w->handlers, "close") : JS_UNDEFINED;
    if (JS_IsFunction(ctx, ch)) {
        JSValueConst a[3] = { w->self, JS_NewInt32(ctx, code),
            JS_NewString(ctx, reason) };
        dyn_call_drop(ctx, ch, JS_UNDEFINED, 3, a);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, a[2]);
    }
    JS_FreeValue(ctx, ch);
    JS_FreeValue(ctx, w->handlers);
    w->handlers = JS_UNDEFINED;
    JS_FreeValue(ctx, w->self);
    w->self = JS_UNDEFINED;
    if (w->hs) {
        w->release_deferred = 1;
    } else {
        dyn_http_async_release(w->ctx);
    }
    dyn_wsc_unref(w);
}

JSValue dyn_wsc_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_wsc_t* w = (dyn_wsc_t*)dyn_res_native(ctx, this_val,
        dyn_wsc_class_id);
    const char* str = NULL;
    uint8_t* abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_UNDEFINED;
    w->refs++;
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) {
            dyn_wsc_unref(w);
            return JS_EXCEPTION;
        }
    } else {
        abuf = dyn_ws_bytes_view(ctx, argv[0], &len, &binary);
        if (!abuf && !binary) {
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str) {
                dyn_wsc_unref(w);
                return JS_EXCEPTION;
            }
        }
    }
    if (!w->closed)
        dyn_wsc_send_frame(w, binary ? 2 : 1,
            str ? (const uint8_t*)str : abuf, len);
    dyn_wsc_unref(w);
    if (str)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static void dyn_wsc_dispose(void* native)
{
    dyn_wsc_t* w = (dyn_wsc_t*)native;
    if (!w)
        return;
    w->refs++;
    if (!w->closed) {
        if (!w->close_sent && w->fd >= 0) {
            w->close_sent = 1;
            dyn_wsc_send_frame(w, 8, NULL, 0);
        }
        dyn_wsc_close_internal(w, 1000, "");
    }
    dyn_wsc_unref(w);
    dyn_wsc_unref(w);
}

static void wsc_hs_done(void* arg);

static _Thread_local wsc_hs_t* wsc_hs_pending;

int wsc_hs_any_pending(void)
{
    return wsc_hs_pending != NULL;
}

static void wsc_hs_finish(wsc_hs_t* hs, int err)
{
    wsc_hs_t** pp = &wsc_hs_pending;
    while (*pp && *pp != hs)
        pp = &(*pp)->next;
    if (*pp)
        *pp = hs->next;
    hs->next = NULL;
    hc_async_hook_sync();
    hs->err = err;
    wsc_hs_done(hs);
}

void wsc_hs_sweep(uint64_t now)
{
    wsc_hs_t* hs;
again:
    for (hs = wsc_hs_pending; hs; hs = hs->next) {
        if (now >= hs->deadline_ms) {
            wsc_hs_finish(hs, hs->connected ? DYN_HTTP_ERR_RECV : DYN_HTTP_ERR_CONNECT);
            goto again;
        }
    }
}

static int wsc_hs_prepare(wsc_hs_t* hs)
{
    uint8_t key_raw[16];
    char hhost[320];
    size_t klen;

    if (dyn_os_entropy(key_raw, sizeof key_raw) < 0)
        return -1;
    klen = dyn_codec_base64_encode(key_raw, sizeof key_raw, hs->key_b64);
    hs->key_b64[klen] = '\0';
    if (hs->u.port == 80)
        snprintf(hhost, sizeof hhost, "%s", hs->u.host);
    else
        snprintf(hhost, sizeof hhost, "%s:%u", hs->u.host, (unsigned)hs->u.port);
    if (dyn_bytes_append(&hs->req, "GET ", 4) < 0
        || dyn_bytes_append(&hs->req, hs->u.path, hs->u.path_len) < 0
        || dyn_bytes_append(&hs->req, " HTTP/1.1\r\nHost: ", 17) < 0
        || dyn_bytes_append(&hs->req, hhost, strlen(hhost)) < 0
        || dyn_bytes_append(&hs->req, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                      "Sec-WebSocket-Key: ",
               62)
            < 0
        || dyn_bytes_append(&hs->req, hs->key_b64, klen) < 0
        || dyn_bytes_append(&hs->req, "\r\nSec-WebSocket-Version: 13\r\n\r\n", 31) < 0)
        return DYN_HTTP_ERR_OOM;
    return 0;
}

static void wsc_hs_send_request(wsc_hs_t* hs)
{
    hs->in_tls_handshake = 0;
    if (dyn_aio_send(hs->w->aio, hs->fd, hs->req.data, hs->req.len, 0, NULL, NULL) < 0)
        wsc_hs_finish(hs, DYN_HTTP_ERR_SEND);
}

#ifdef CONFIG_TLS
static void wsc_hs_on_tls(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    wsc_hs_t* hs = (wsc_hs_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    if (res < 0) {
        snprintf(hs->tls_err, sizeof hs->tls_err, "%s", dyn_tls_error(hs->tls));
        wsc_hs_finish(hs, DYN_HTTP_ERR_TLS);
        return;
    }
    wsc_hs_send_request(hs);
}
#endif

static void wsc_hs_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    wsc_hs_t* hs = (wsc_hs_t*)ud;
    size_t room, take;
    const char* m;
    (void)aio;

    if (res <= 0) {
#ifdef CONFIG_TLS
        if (hs->tls)
            snprintf(hs->tls_err, sizeof hs->tls_err, "%s", dyn_tls_error(hs->tls));
        if (hs->in_tls_handshake) {
            wsc_hs_finish(hs, DYN_HTTP_ERR_TLS);
            return;
        }
#endif
        wsc_hs_finish(hs, DYN_HTTP_ERR_RECV);
        return;
    }
    room = sizeof hs->head - hs->got;
    take = len < room ? len : room;
    memcpy(hs->head + hs->got, buf, take);
    hs->got += take;
    m = dyn_memfind(hs->head, hs->got, "\r\n\r\n", 4);
    if (!m) {
        if (hs->got >= sizeof hs->head)
            wsc_hs_finish(hs, -1);
        return;
    }
    hs->hdr_end = (size_t)(m - hs->head) + 4;
    hs->extra = buf + take;
    hs->extra_len = len - take;
    wsc_hs_finish(hs, 0);
}

static void wsc_hs_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    wsc_hs_t* hs = (wsc_hs_t*)ud;
    (void)buf;
    (void)len;

    if (res < 0) {
        wsc_hs_finish(hs, res == -EINVAL ? DYN_HTTP_ERR_RESOLVE : DYN_HTTP_ERR_CONNECT);
        return;
    }
    hs->connected = 1;
    hs->deadline_ms = dyn_timer_now_ms() + DYN_HTTP_DEFAULT_TIMEOUT_MS;
    if (dyn_aio_recv(aio, hs->fd, 0, 1, wsc_hs_on_recv, hs) < 0) {
        wsc_hs_finish(hs, DYN_HTTP_ERR_RECV);
        return;
    }
#ifdef CONFIG_TLS
    if (hs->tls) {
        if (dyn_aio_tls_attach(aio, hs->fd, hs->tls, wsc_hs_on_tls, hs) < 0) {
            snprintf(hs->tls_err, sizeof hs->tls_err, "cannot attach TLS");
            wsc_hs_finish(hs, DYN_HTTP_ERR_TLS);
            return;
        }
        hs->tls_attached = 1;
        hs->in_tls_handshake = 1;
        if (dyn_aio_tls_start(aio, hs->fd) < 0) {
            snprintf(hs->tls_err, sizeof hs->tls_err, "%s", dyn_tls_error(hs->tls));
            wsc_hs_finish(hs, DYN_HTTP_ERR_TLS);
        }
        return;
    }
#endif
    wsc_hs_send_request(hs);
}

static void wsc_hs_start(wsc_hs_t* hs)
{
    char hbuf3[256];
    int err, fd;

    hs->fd = -1;
    hs->deadline_ms = dyn_timer_now_ms() + DYN_HTTP_DEFAULT_TIMEOUT_MS;
    hs->next = wsc_hs_pending;
    wsc_hs_pending = hs;
    hc_async_hook_sync();
    err = wsc_hs_prepare(hs);
    if (err != 0) {
        wsc_hs_finish(hs, err);
        return;
    }
    fd = dyn_aio_connect(hs->w->aio, dyn_host_bare(hs->u.host, hbuf3, sizeof hbuf3),
        hs->u.port, wsc_hs_on_connect, hs);
    if (fd < 0) {
        wsc_hs_finish(hs, errno == EINVAL ? DYN_HTTP_ERR_RESOLVE : DYN_HTTP_ERR_CONNECT);
        return;
    }
    hs->fd = fd;
}

static void wsc_hs_done(void* arg)
{
    wsc_hs_t* hs = (wsc_hs_t*)arg;
    dyn_wsc_t* w = hs->w;
    JSContext* ctx = w->ctx;
    char why[384];
    int fail = 1, code = 1006;
    int rel_deferred;
    w->refs++;

    rel_deferred = w->release_deferred;
    w->release_deferred = 0;

    w->hs = NULL;
    why[0] = 0;
    if (hs->err == 0) {
        const char* sp = memchr(hs->head, ' ', hs->hdr_end);
        const char* acc;
        size_t acclen = 0;
        int status = 0;
        if (!sp) {
            snprintf(why, sizeof why, "handshake failed: no status line");
        } else {
            while (hs->head + hs->hdr_end > ++sp && *sp >= '0' && *sp <= '9')
                status = status * 10 + (*sp - '0');
            if (status != 101) {
                snprintf(why, sizeof why, "handshake failed: status %d", status);
            } else {
                acc = dyn_req_header(hs->head, hs->hdr_end,
                    "sec-websocket-accept", &acclen);
                dws_accept(hs->key_b64, strlen(hs->key_b64), hs->accept);
                if (!acc || acclen != strlen(hs->accept)
                    || memcmp(acc, hs->accept, acclen) != 0) {
                    snprintf(why, sizeof why,
                        "handshake failed: bad Sec-WebSocket-Accept");
                } else {
                    fail = 0;
                }
            }
        }
    } else if (hs->err == -1) {
        snprintf(why, sizeof why, "handshake failed: response headers too large");
    } else {
#ifdef CONFIG_TLS
        if (hs->tls_err[0])
            snprintf(why, sizeof why, "HTTP WS %s failed: %s (%s)",
                hs->url ? hs->url : "", hc_err_name(hs->err),
                hs->tls_err);
        else
#endif
            snprintf(why, sizeof why, "HTTP WS %s failed: %s",
                hs->url ? hs->url : "", hc_err_name(hs->err));
    }

    if (!fail && !w->closed) {
        JSValue oh;
        w->fd = hs->fd;
        if (hs->got > hs->hdr_end)
            dyn_iobuf_append(&w->in, hs->head + hs->hdr_end,
                hs->got - hs->hdr_end);
        if (hs->extra_len)
            dyn_iobuf_append(&w->in, hs->extra, hs->extra_len);
        dyn_aio_recv_retarget(w->aio, hs->fd, dyn_wsc_on_recv, w);
        oh = JS_GetPropertyStr(ctx, w->handlers, "open");
        if (JS_IsFunction(ctx, oh)) {
            JSValueConst a[1] = { w->self };
            dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, oh);
        dyn_wsc_process(w);
    } else {
        if (hs->fd >= 0) {
            dyn_aio_close(w->aio, hs->fd);
#ifdef CONFIG_TLS
            if (hs->tls_attached)
                hs->tls = NULL;
#endif
        }
        if (!w->closed) {
            dyn_res_mark_closed(JS_GetRuntime(ctx), w->self, dyn_wsc_class_id);
            dyn_wsc_close_internal(w, code, why);
            dyn_wsc_unref(w);
        }
    }
    free(hs->req.data);
    free(hs->http_url);
    free(hs->url);
#ifdef CONFIG_TLS
    if (hs->tls_ctx)
        dyn_tls_ctx_free(hs->tls_ctx);
    if (hs->tls && !hs->tls_attached)
        dyn_tls_conn_free(hs->tls);
#endif
    free(hs);
    dyn_wsc_unref(w);
    if (rel_deferred) {
        dyn_http_async_release(ctx);
    }
}

JSValue dyn_wsc_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_wsc_t* w = NULL;
    wsc_hs_t* hs = NULL;
    const char* url = NULL;
    dyn_url_t u;
    __maybe_unused char hbuf3[256];
    JSValue obj;
    int is_wss = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    if (dyn_opts_strict(ctx, argv[1], http_ws_handler_keys, 3))
        return JS_EXCEPTION;
    url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    is_wss = (strncmp(url, "wss://", 6) == 0);
    if (!is_wss && strncmp(url, "ws://", 5) != 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "WsClient: the URL scheme must be ws://");
    }

    w = (dyn_wsc_t*)calloc(1, sizeof(*w));
    if (!w)
        goto oom;
    w->ctx = ctx;
    w->fd = -1;
    w->refs = 1;
    w->aio = dyn_net_reactor_acquire(ctx);
    if (!w->aio) {
        dyn_net_throw_no_reactor(ctx, "WsClient");
        goto fail;
    }
    (void)dyn_aio_pool_warm(w->aio);
    w->handlers = JS_DupValue(ctx, argv[1]);
    w->ctl_budget = DYN_WS_CTL_BUDGET;
    dyn_iobuf_init(&w->in);
    dyn_iobuf_init(&w->frag);

    hs = (wsc_hs_t*)calloc(1, sizeof(*hs));
    if (!hs)
        goto oom;
    hs->w = w;
    hs->url = strdup(url);
    hs->http_url = (char*)malloc(strlen(url) + 3);
    if (!hs->url || !hs->http_url)
        goto oom;
    snprintf(hs->http_url, strlen(url) + 3,
        is_wss ? "https://%s" : "http://%s",
        url + (is_wss ? 6 : 5));
    if (dyn_parse_url(hs->http_url, &u) < 0) {
        dyn_http_throw(ctx, DYN_HTTP_ERR_URL, "WS", url, NULL);
        goto fail;
    }
    hs->u = u;
#ifdef CONFIG_TLS
    if (is_wss) {
        dyn_tls_opts_t to;
        memset(&to, 0, sizeof to);
        hs->tls_ctx = dyn_tls_ctx_client(&to, hs->tls_err,
            sizeof hs->tls_err);
        if (!hs->tls_ctx) {
            char msg[256];
            snprintf(msg, sizeof msg, "WsClient: TLS setup failed: %s",
                hs->tls_err);
            JS_ThrowTypeError(ctx, "%s", msg);
            goto fail;
        }
        hs->tls = dyn_tls_conn_new(hs->tls_ctx,
            dyn_host_bare(u.host, hbuf3, sizeof hbuf3), hs->tls_err,
            sizeof hs->tls_err);
        if (!hs->tls) {
            char msg[256];
            snprintf(msg, sizeof msg, "WsClient: TLS setup failed: %s",
                hs->tls_err);
            dyn_tls_ctx_free(hs->tls_ctx);
            hs->tls_ctx = NULL;
            JS_ThrowTypeError(ctx, "%s", msg);
            goto fail;
        }
        if (u.port == 0)
            u.port = 443;
        hs->u.port = u.port;
    }
#else
    if (is_wss) {
        dyn_tls_missing(ctx, "WebSocket", "wss");
        goto fail;
    }
#endif

    if (JS_AddShutdownSweep(JS_GetRuntime(ctx), dyn_wsc_shutdown_sweep, w) < 0)
        goto oom;
    dyn_http_async_hook(ctx);
    obj = dyn_res_wrap(ctx, new_target, dyn_wsc_class_id, w, dyn_wsc_dispose);
    if (JS_IsException(obj)) {
        JS_RemoveShutdownSweep(JS_GetRuntime(ctx), dyn_wsc_shutdown_sweep, w);
        w = NULL;
        goto fail;
    }
    w->self = JS_DupValue(ctx, obj);
    w->hs = hs;
    wsc_hs_start(hs);
    hs = NULL;

    JS_FreeCString(ctx, url);
    return obj;

oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    if (hs) {
        free(hs->req.data);
        free(hs->http_url);
        free(hs->url);
        free(hs);
    }
    if (w) {
        if (w->aio)
            dyn_net_reactor_release(ctx);
        JS_FreeValue(ctx, w->handlers);
        dyn_iobuf_free(&w->in);
        dyn_iobuf_free(&w->frag);
        free(w);
    }
    JS_FreeCString(ctx, url);
    return JS_EXCEPTION;
}
#endif
