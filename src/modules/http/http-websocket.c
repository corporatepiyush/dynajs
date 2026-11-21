#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
static void dyn_ws_send_frame(dyn_app_conn_t* c, int opcode, const uint8_t* data,
    size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10];
    size_t hn;
    int i;
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        h[1] = (uint8_t)len;
        hn = 2;
    } else if (len <= 0xffff) {
        h[1] = 126;
        h[2] = (len >> 8) & 0xff;
        h[3] = len & 0xff;
        hn = 4;
    } else {
        h[1] = 127;
        for (i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
        hn = 10;
    }
    dyn_iobuf_append(&f, h, hn);
    if (len)
        dyn_iobuf_append(&f, data, len);
    if (dyn_aio_queued(c->app->aio, c->fd) + f.len > DYN_HTTP_OUTBOUND_MAX) {
        dyn_iobuf_free(&f);
        dyn_app_conn_close(c);
        return;
    }
    dyn_aio_send(c->app->aio, c->fd, f.data, f.len, DYN_AIO_HOLD, NULL, NULL);
    dyn_iobuf_free(&f);
}

void dyn_ws_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(val, dyn_ws_class_id);
    (void)rt;
    free(w);
}

uint8_t* dyn_ws_bytes_view(JSContext* ctx, JSValueConst v, size_t* plen,
    int* pbinary)
{
    uint8_t* base;
    size_t off = 0, blen = 0, ab = 0;
    JSValue buf;

    *pbinary = 0;
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE)
        return NULL;
    buf = JS_GetArrayBufferView(ctx, v, &off, &blen, NULL);
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return NULL;
        }
        if (off > ab)
            off = ab;
        if (blen > ab - off)
            blen = ab - off;
        *plen = blen;
        *pbinary = 1;
        return base + off;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *plen = ab;
        *pbinary = 1;
        return base;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
}

JSValue dyn_ws_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(this_val, dyn_ws_class_id);
    const char* str = NULL;
    uint8_t* abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w)
        return JS_ThrowTypeError(ctx, "not a WsConn");
    if (argc < 1)
        return JS_UNDEFINED;
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str)
            return JS_EXCEPTION;
    } else {
        abuf = dyn_ws_bytes_view(ctx, argv[0], &len, &binary);
        if (!abuf && !binary) {
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
        }
    }
    if (w->conn && !w->conn->closed)
        dyn_ws_send_frame(w->conn, binary ? 2 : 1,
            str ? (const uint8_t*)str : abuf, len);
    else
        w->after_close += (int64_t)len;
    if (str)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

JSValue dyn_ws_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(this_val, dyn_ws_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_ThrowTypeError(ctx, "not a WsConn");
    if (w->conn && !w->conn->closed) {
        dyn_ws_send_frame(w->conn, 8, NULL, 0);
        dyn_app_conn_close(w->conn);
    }
    return JS_UNDEFINED;
}

JSValue dyn_ws_get_buffered(JSContext* ctx, JSValueConst this_val)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(this_val, dyn_ws_class_id);
    if (!w)
        return JS_ThrowTypeError(ctx, "not a WsConn");
    if (!w->conn || w->conn->closed)
        return JS_NewInt64(ctx, w->after_close);
    return JS_NewInt64(ctx, (int64_t)dyn_aio_queued(w->conn->app->aio, w->conn->fd));
}

static int dyn_ws_handshake_valid(const char* base, size_t head_len)
{
    const char *key, *upg, *conn, *ver;
    size_t keylen = 0, upglen = 0, connlen = 0, verlen = 0, b;

    if (head_len < 5 || memcmp(base, "GET ", 4) != 0)
        return 0;
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    upg = dyn_req_header(base, head_len, "upgrade", &upglen);
    conn = dyn_req_header(base, head_len, "connection", &connlen);
    ver = dyn_req_header(base, head_len, "sec-websocket-version", &verlen);
    if (!key || keylen != 24)
        return 0;
    for (b = 0; b < keylen; b++) {
        char ch = key[b];
        int okc = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!okc)
            return 0;
    }
    if (!httpk_list_has(upg, upglen, "websocket"))
        return 0;
    if (!httpk_list_has(conn, connlen, "upgrade"))
        return 0;
    while (verlen > 0 && (*ver == ' ' || *ver == '\t')) {
        ver++;
        verlen--;
    }
    while (verlen > 0 && (ver[verlen - 1] == ' ' || ver[verlen - 1] == '\t'))
        verlen--;
    return verlen == 2 && ver[0] == '1' && ver[1] == '3';
}

int dyn_app_ws_handshake(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len)
{
    JSContext* ctx = c->app->ctx;
    const char* key;
    size_t keylen = 0;
    char accept[32], resp[256];
    int rn;
    dyn_ws_t* w;
    JSValue obj, oh;

    if (!dyn_ws_handshake_valid(base, head_len))
        return 0;
    oh = JS_GetPropertyStr(ctx, rt->handler, "upgrade");
    if (JS_IsFunction(ctx, oh)) {
        static const char* const hdr[] = { "origin", "host", "cookie", "sec-websocket-protocol" };
        static const char* const prop[] = { "origin", "host", "cookie", "protocol" };
        JSValue info = JS_NewObject(ctx), r;
        int allow, i;
        for (i = 0; i < 4 && !JS_IsException(info); i++) {
            size_t vlen = 0;
            const char* v = dyn_req_header(base, head_len, hdr[i], &vlen);
            JS_DefinePropertyValueStr(ctx, info, prop[i],
                v ? JS_NewStringLen(ctx, v, vlen) : JS_NULL, JS_PROP_C_W_E);
        }
        r = JS_Call(ctx, oh, JS_UNDEFINED, 1, (JSValueConst*)&info);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            allow = 0;
        } else {
            allow = JS_ToBool(ctx, r) > 0;
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, info);
        if (!allow) {
            JS_FreeValue(ctx, oh);
            return -1;
        }
    }
    JS_FreeValue(ctx, oh);
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    dws_accept(key, keylen, accept);
    rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    dyn_aio_send(c->app->aio, c->fd, resp, (size_t)rn, DYN_AIO_HOLD, NULL, NULL);

    c->is_ws = 1;
    c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
    c->ws_frag_frames = 0;
    c->ws_handlers = JS_DupValue(ctx, rt->handler);
    w = (dyn_ws_t*)malloc(sizeof(*w));
    if (!w)
        return 1;
    w->conn = c;
    w->after_close = 0;
    c->ws_native = w;
    obj = JS_NewObjectClass(ctx, dyn_ws_class_id);
    JS_SetOpaque(obj, w);
    c->ws_this = obj;

    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
    }
    JS_FreeValue(ctx, oh);
    return 1;
}

static void dyn_app_ws_dispatch_msg(dyn_app_conn_t* c, int opcode,
    const uint8_t* payload, size_t plen)
{
    JSContext* ctx = c->app->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, c->ws_handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        JS_FreeValue(ctx, mh);
        {
            uint8_t cl[2] = { 0x03, 0xef };
            dyn_ws_send_frame(c, 8, cl, 2);
        }
        c->close_code = 1007;
        dyn_app_conn_close(c);
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? dyn_nat_u8_copy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char*)payload, plen);
        JSValueConst args[3] = { c->ws_this, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

void dyn_app_ws_process(dyn_app_conn_t* c)
{
    for (;;) {
        uint8_t* p = dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        int opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t *mask, *payload;

        int fin;
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
        if (!masked) {
            dyn_app_conn_close(c);
            return;
        }
        if (opcode >= 0x8 && (plen > 125 || !fin)) {
            dyn_app_conn_close(c);
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) {
            dyn_app_conn_close(c);
            return;
        }
        if (avail < hdr + 4 + plen)
            return;
        mask = p + hdr;
        payload = p + hdr + 4;
        {
            uint32_t m32;
            uint64_t m64;
            size_t k = 0, n8 = (size_t)plen & ~(size_t)7;
            memcpy(&m32, mask, 4);
            m64 = ((uint64_t)m32 << 32) | m32;
            for (; k < n8; k += 8) {
                uint64_t v;
                memcpy(&v, payload + k, 8);
                v ^= m64;
                memcpy(payload + k, &v, 8);
            }
            for (i = (uint32_t)k; i < plen; i++)
                payload[i] ^= mask[i & 3];
        }
        frame_total = hdr + 4 + (size_t)plen;

        if (opcode == 0x8) {
            dyn_ws_send_frame(c, 8, NULL, 0);
            dyn_iobuf_consume(&c->in, frame_total);
            dyn_app_conn_close(c);
            return;
        } else if (opcode == 0x9) {
            if (--c->ws_ctl_budget < 0) {
                dyn_app_conn_close(c);
                return;
            }
            dyn_ws_send_frame(c, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {
        } else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
            int frag_active = c->ws_frag_op != 0;
            if ((opcode == 0x0 && !frag_active) || (opcode != 0x0 && frag_active)) {
                dyn_app_conn_close(c);
                return;
            }
            if (opcode != 0x0 && fin) {
                c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_app_ws_dispatch_msg(c, opcode, payload, (size_t)plen);
            } else {
                if (opcode != 0x0) {
                    c->ws_frag_op = opcode;
                    c->ws_frag_frames = 0;
                }
                if (++c->ws_frag_frames > DYN_WS_MAX_FRAGMENTS || dyn_iobuf_rlen(&c->ws_frag) + plen > DYN_ACONN_MAX_REQ || dyn_iobuf_append(&c->ws_frag, payload, (size_t)plen) < 0) {
                    dyn_app_conn_close(c);
                    return;
                }
                if (fin) {
                    c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
                    dyn_app_ws_dispatch_msg(c, c->ws_frag_op,
                        dyn_iobuf_rdata(&c->ws_frag),
                        dyn_iobuf_rlen(&c->ws_frag));
                    dyn_iobuf_reset(&c->ws_frag);
                    c->ws_frag_op = 0;
                    c->ws_frag_frames = 0;
                }
            }
        }
        dyn_iobuf_consume(&c->in, frame_total);
        if (c->closed)
            return;
    }
}

JSValue dyn_app_ws_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "ws(path, {open,message,close})");
    if (dyn_opts_strict(ctx, argv[1], http_ws_handler_keys, 4))
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
    nr->type = APP_WS;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}
#endif
