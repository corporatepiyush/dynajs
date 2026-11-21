#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-mod.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { OP_RS_START, OP_RS_PULL, OP_RS_CANCEL, OP_WS_START, OP_WS_WRITE, OP_WS_CLOSE, OP_WS_ABORT, OP_CLOSE };
enum { OP_LIS_CLOSE, OP_UDP_SEND, OP_UDP_CLOSE };

static void conn_open(void* ud, wg_tconn_t* tc);
static void conn_data(void* ud, wg_tconn_t* tc, const uint8_t* d, size_t n);
static void conn_eof(void* ud, wg_tconn_t* tc);
static void conn_writable(void* ud, wg_tconn_t* tc);
static void conn_close(void* ud, wg_tconn_t* tc, int err);
static void lis_open(void* ud, wg_tconn_t* tc);

static const wg_tcp_cb_t CONN_CB = { conn_open, conn_data, conn_eof, conn_writable, conn_close };
static const wg_tcp_cb_t LIS_CB = { lis_open, NULL, NULL, NULL, NULL };

static void unl(wg_tlisten_t* l)
{
    if (l)
        wg_tcp_unlisten(l);
}

static void fv(JSRuntime* rt, JSValue* v)
{
    JSValue x = *v;
    *v = JS_UNDEFINED;
    JS_FreeValueRT(rt, x);
}

static int ctl_call(JSContext* ctx, JSValueConst ctl, const char* method, int argc, JSValueConst* argv)
{
    JSValue fn, r;
    int rc = 0;
    if (wgm_in_final || !JS_IsObject(ctl))
        return -1;
    fn = JS_GetPropertyStr(ctx, ctl, method);
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    r = JS_Call(ctx, fn, ctl, argc, argv);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        rc = -1;
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
    return rc;
}

static double ctl_desired(JSContext* ctx, JSValueConst ctl)
{
    JSValue d = JS_GetPropertyStr(ctx, ctl, "desiredSize");
    double v = 1;
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 1;
    }
    if (JS_IsNumber(d))
        JS_ToFloat64(ctx, &v, d);
    JS_FreeValue(ctx, d);
    return v;
}

static wgm_conn_t* conn_new(wgm_t* w, wg_tconn_t* tc)
{
    wgm_conn_t* c = (wgm_conn_t*)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->w = w;
    c->tc = tc;
    c->ctl = c->wctl = c->dres = c->drej = c->wres = c->wrej = c->cres = c->crej = JS_UNDEFINED;
    if (wgm_slot_add(w, WGM_CONN, c, &c->id) < 0) {
        free(c);
        return NULL;
    }
    return c;
}

static void conn_settle_all(wgm_conn_t* c, int err, const char* msg)
{
    wgm_t* w = c->w;
    JSContext* ctx = w->ctx;
    if (!JS_IsUndefined(c->drej))
        wgm_settle(w, &c->dres, &c->drej, 1, wgm_err(ctx, "WireGuard: connect: %s", msg));
    if (JS_IsObject(c->ctl) && !c->rdone) {
        if (err) {
            JSValue e = wgm_err(ctx, "WireGuard: %s", msg);
            ctl_call(ctx, c->ctl, "error", 1, (JSValueConst*)&e);
            JS_FreeValue(ctx, e);
        } else
            ctl_call(ctx, c->ctl, "close", 0, NULL);
    }
    c->rdone = 1;
    if (!JS_IsUndefined(c->wrej))
        wgm_settle(w, &c->wres, &c->wrej, 1, wgm_err(ctx, "WireGuard: %s", msg));
    if (!JS_IsUndefined(c->crej))
        wgm_settle(w, &c->cres, &c->crej, err, err ? wgm_err(ctx, "WireGuard: %s", msg) : JS_UNDEFINED);
    if (err && JS_IsObject(c->wctl)) {
        JSValue e = wgm_err(ctx, "WireGuard: %s", msg);
        ctl_call(ctx, c->wctl, "error", 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, e);
    }
    fv(w->rt, &c->ctl);
    fv(w->rt, &c->wctl);
}

static void conn_free(wgm_conn_t* c)
{
    wgm_t* w = c->w;
    JSRuntime* rt = w->rt;
    fv(rt, &c->ctl);
    fv(rt, &c->wctl);
    fv(rt, &c->dres);
    fv(rt, &c->drej);
    fv(rt, &c->wres);
    fv(rt, &c->wrej);
    fv(rt, &c->cres);
    fv(rt, &c->crej);
    wgm_slot_del(w, c->id);
    free(c->pend);
    free(c);
}

static JSValue conn_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data);

static JSValue size_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_NewInt32(ctx, 1);
    return JS_GetPropertyStr(ctx, argv[0], "byteLength");
}

static JSValue make_stream(JSContext* ctx, const char* ctor_name, JSValueConst* data, int readable)
{
    JSValue g = JS_GetGlobalObject(ctx), ctor, src, strat, r = JS_EXCEPTION;
    JSValueConst args[2];
    ctor = JS_GetPropertyStr(ctx, g, ctor_name);
    JS_FreeValue(ctx, g);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return JS_ThrowTypeError(ctx, "WireGuard: %s is not available", ctor_name);
    }
    src = JS_NewObject(ctx);
    strat = JS_NewObject(ctx);
    if (readable) {
        JS_SetPropertyStr(ctx, src, "start", JS_NewCFunctionData(ctx, conn_fn, 1, OP_RS_START, 2, data));
        JS_SetPropertyStr(ctx, src, "pull", JS_NewCFunctionData(ctx, conn_fn, 1, OP_RS_PULL, 2, data));
        JS_SetPropertyStr(ctx, src, "cancel", JS_NewCFunctionData(ctx, conn_fn, 1, OP_RS_CANCEL, 2, data));
        JS_SetPropertyStr(ctx, strat, "highWaterMark", JS_NewInt32(ctx, WGM_RS_HWM));
        JS_SetPropertyStr(ctx, strat, "size", JS_NewCFunction(ctx, size_fn, "size", 1));
    } else {
        JS_SetPropertyStr(ctx, src, "start", JS_NewCFunctionData(ctx, conn_fn, 1, OP_WS_START, 2, data));
        JS_SetPropertyStr(ctx, src, "write", JS_NewCFunctionData(ctx, conn_fn, 2, OP_WS_WRITE, 2, data));
        JS_SetPropertyStr(ctx, src, "close", JS_NewCFunctionData(ctx, conn_fn, 0, OP_WS_CLOSE, 2, data));
        JS_SetPropertyStr(ctx, src, "abort", JS_NewCFunctionData(ctx, conn_fn, 1, OP_WS_ABORT, 2, data));
        JS_SetPropertyStr(ctx, strat, "highWaterMark", JS_NewInt32(ctx, 1));
    }
    args[0] = src;
    args[1] = strat;
    r = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, strat);
    JS_FreeValue(ctx, ctor);
    return r;
}

static JSValue conn_obj(wgm_t* w, wgm_conn_t* c)
{
    JSContext* ctx = w->ctx;
    JSValue o = JS_NewObject(ctx), data[2], rs, ws;
    wg_ipaddr_t la, ra;
    uint16_t lp, rp;
    data[0] = w->obj;
    data[1] = JS_NewUint32(ctx, c->id);
    rs = make_stream(ctx, "ReadableStream", data, 1);
    ws = JS_IsException(rs) ? JS_EXCEPTION : make_stream(ctx, "WritableStream", data, 0);
    if (JS_IsException(ws)) {
        JS_FreeValue(ctx, rs);
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    wg_tcp_endpoints(c->tc, &la, &lp, &ra, &rp);
    JS_DefinePropertyValueStr(ctx, o, "readable", rs, JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "writable", ws, JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "remoteAddress", wgm_ip_str(ctx, &ra), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "remotePort", JS_NewInt32(ctx, rp), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "localAddress", wgm_ip_str(ctx, &la), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "localPort", JS_NewInt32(ctx, lp), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, o, "close", JS_NewCFunctionData(ctx, conn_fn, 0, OP_CLOSE, 2, data), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return o;
}

static void conn_abort(wgm_conn_t* c, const char* msg)
{
    wg_tconn_t* tc = c->tc;
    wgm_t* w = c->w;
    if (!tc)
        return;
    c->tc = NULL;
    wg_tcp_set_cb(tc, NULL, NULL);
    wg_tcp_abort(tc);
    conn_settle_all(c, 1, msg);
    conn_free(c);
    wgm_kick(w);
}

static void flush_pend(wgm_conn_t* c)
{
    while (c->tc && c->poff < c->pn) {
        size_t k = wg_tcp_write(c->tc, c->pend + c->poff, c->pn - c->poff);
        if (!k)
            return;
        c->poff += k;
    }
    if (!c->tc || c->poff < c->pn)
        return;
    free(c->pend);
    c->pend = NULL;
    c->pn = c->poff = 0;
    if (!JS_IsUndefined(c->wres))
        wgm_settle(c->w, &c->wres, &c->wrej, 0, JS_UNDEFINED);
    if (c->fin_wanted) {
        c->fin_wanted = 0;
        wg_tcp_shutdown(c->tc);
        if (!JS_IsUndefined(c->cres))
            wgm_settle(c->w, &c->cres, &c->crej, 0, JS_UNDEFINED);
    }
}

static JSValue resolved(JSContext* ctx)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f), r;
    if (JS_IsException(p))
        return p;
    r = JS_Call(ctx, f[0], JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    return p;
}

static JSValue pending(JSContext* ctx, JSValue* res, JSValue* rej)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p))
        return p;
    *res = f[0];
    *rej = f[1];
    return p;
}

static JSValue conn_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    wgm_t* w = wgm_of(data[0]);
    uint32_t id = 0;
    wgm_conn_t* c;
    (void)this_val;
    JS_ToUint32(ctx, &id, data[1]);
    c = w && !w->closing ? (wgm_conn_t*)wgm_slot_get(w, WGM_CONN, id) : NULL;
    switch (magic) {
    case OP_RS_START:
    case OP_WS_START:
        if (c && argc > 0) {
            JSValue* slot = magic == OP_RS_START ? &c->ctl : &c->wctl;
            fv(w->rt, slot);
            *slot = JS_DupValue(ctx, argv[0]);
        }
        return JS_UNDEFINED;
    case OP_RS_PULL:
        if (c && c->paused && c->tc) {
            c->paused = 0;
            wg_tcp_pause(c->tc, 0);
            wgm_kick(w);
        }
        return JS_UNDEFINED;
    case OP_RS_CANCEL:
        if (c) {
            c->rdone = 1;
            fv(w->rt, &c->ctl);
            conn_abort(c, "readable cancelled");
        }
        return JS_UNDEFINED;
    case OP_WS_WRITE: {
        const uint8_t* p;
        size_t n, k;
        const char* cs;
        JSValue r;
        if (!c || !c->tc)
            return JS_ThrowTypeError(ctx, "WireGuard: connection closed");
        if (wgm_bytes(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &p, &n, &cs, "WireGuardConn.write") < 0)
            return JS_EXCEPTION;
        k = wg_tcp_write(c->tc, p, n);
        if (k < n) {
            c->pend = (uint8_t*)malloc(n - k);
            if (!c->pend) {
                if (cs)
                    JS_FreeCString(ctx, cs);
                return JS_ThrowOutOfMemory(ctx);
            }
            memcpy(c->pend, p + k, n - k);
            c->pn = n - k;
            c->poff = 0;
            r = pending(ctx, &c->wres, &c->wrej);
        } else
            r = resolved(ctx);
        if (cs)
            JS_FreeCString(ctx, cs);
        wgm_kick(w);
        return r;
    }
    case OP_WS_CLOSE:
        if (!c || !c->tc)
            return JS_ThrowTypeError(ctx, "WireGuard: connection closed");
        if (c->pn) {
            c->fin_wanted = 1;
            return pending(ctx, &c->cres, &c->crej);
        }
        wg_tcp_shutdown(c->tc);
        wgm_kick(w);
        return JS_UNDEFINED;
    case OP_WS_ABORT:
    case OP_CLOSE:
        if (c)
            conn_abort(c, magic == OP_CLOSE ? "connection closed" : "writable aborted");
        return JS_UNDEFINED;
    default:
        return JS_UNDEFINED;
    }
}

static void conn_open(void* ud, wg_tconn_t* tc)
{
    wgm_conn_t* c = (wgm_conn_t*)ud;
    wgm_t* w = c->w;
    JSValue o;
    (void)tc;
    if (w->closing || !c->dialing)
        return;
    c->dialing = 0;
    o = conn_obj(w, c);
    if (JS_IsException(o)) {
        JSValue e = JS_GetException(w->ctx);
        wgm_settle(w, &c->dres, &c->drej, 1, e);
        conn_abort(c, "out of memory");
        return;
    }
    wgm_settle(w, &c->dres, &c->drej, 0, o);
}

static void conn_data(void* ud, wg_tconn_t* tc, const uint8_t* d, size_t n)
{
    wgm_conn_t* c = (wgm_conn_t*)ud;
    JSContext* ctx = c->w->ctx;
    JSValue u8;
    if (c->w->closing || c->rdone || !JS_IsObject(c->ctl))
        return;
    u8 = dyn_nat_u8_copy(ctx, d, n);
    if (JS_IsException(u8)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    ctl_call(ctx, c->ctl, "enqueue", 1, (JSValueConst*)&u8);
    JS_FreeValue(ctx, u8);
    if (!c->paused && JS_IsObject(c->ctl) && ctl_desired(ctx, c->ctl) <= 0) {
        c->paused = 1;
        wg_tcp_pause(tc, 1);
    }
}

static void conn_eof(void* ud, wg_tconn_t* tc)
{
    wgm_conn_t* c = (wgm_conn_t*)ud;
    (void)tc;
    if (c->w->closing || c->rdone)
        return;
    c->rdone = 1;
    ctl_call(c->w->ctx, c->ctl, "close", 0, NULL);
    fv(c->w->rt, &c->ctl);
}

static void conn_writable(void* ud, wg_tconn_t* tc)
{
    wgm_conn_t* c = (wgm_conn_t*)ud;
    (void)tc;
    if (!c->w->closing)
        flush_pend(c);
}

static const char* err_text(int err)
{
    if (err == -ECONNREFUSED)
        return "connection refused";
    if (err == -ETIMEDOUT)
        return "timed out";
    if (err == -ECONNRESET)
        return "connection reset";
    return err ? "connection failed" : "connection closed";
}

static void conn_close(void* ud, wg_tconn_t* tc, int err)
{
    wgm_conn_t* c = (wgm_conn_t*)ud;
    (void)tc;
    c->tc = NULL;
    if (c->w->closing)
        return;
    conn_settle_all(c, err != 0 || c->dialing, err_text(err));
    conn_free(c);
}

static void lis_open(void* ud, wg_tconn_t* tc)
{
    wgm_lis_t* l = (wgm_lis_t*)ud;
    wgm_t* w = l->w;
    JSContext* ctx = w->ctx;
    wgm_conn_t* c;
    JSValue o, r;
    if (w->closing || !(c = conn_new(w, tc))) {
        wg_tcp_abort(tc);
        return;
    }
    wg_tcp_set_cb(tc, &CONN_CB, c);
    o = conn_obj(w, c);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        conn_abort(c, "out of memory");
        return;
    }
    r = JS_Call(ctx, l->handler, JS_UNDEFINED, 1, (JSValueConst*)&o);
    if (JS_IsException(r))
        wgm_call_error(w);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, o);
}

JSValue wgm_dial(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w = wgm_of(this_val);
    wg_ipaddr_t dst;
    uint16_t port;
    wgm_conn_t* c;
    JSValue p;
    if (!w || w->closing)
        return JS_ThrowTypeError(ctx, "WireGuard: closed");
    if (!wg_glue_tcp(w->g))
        return JS_ThrowTypeError(ctx, "WireGuard.dial: needs the userspace stack (with tun the kernel routes TCP)");
    if (wgm_ip_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &dst, "WireGuard.dial") < 0 || wgm_port_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 1, &port, "WireGuard.dial") < 0)
        return JS_EXCEPTION;
    if ((dst.v6 && !w->has6) || (!dst.v6 && !w->has4))
        return JS_ThrowTypeError(ctx, "WireGuard.dial: no %s address configured", dst.v6 ? "IPv6" : "IPv4");
    c = conn_new(w, NULL);
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    p = pending(ctx, &c->dres, &c->drej);
    if (JS_IsException(p)) {
        conn_free(c);
        return p;
    }
    if (!wg_dev_routable(wg_glue_dev(w->g), dst.v6, dst.a)) {
        wgm_settle(w, &c->dres, &c->drej, 1, wgm_err(ctx, "WireGuard: connect: no route to host (no peer's allowedIPs covers it)"));
        conn_free(c);
        return p;
    }
    c->dialing = 1;
    c->tc = wg_tcp_connect(wg_glue_tcp(w->g), &dst, port, &CONN_CB, c);
    if (!c->tc) {
        wgm_settle(w, &c->dres, &c->drej, 1, wgm_err(ctx, "WireGuard: connect: no route or out of ports"));
        conn_free(c);
        return p;
    }
    wgm_kick(w);
    return p;
}

static JSValue obj_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    wgm_t* w = wgm_of(data[0]);
    uint32_t id = 0;
    (void)this_val;
    JS_ToUint32(ctx, &id, data[1]);
    if (magic == OP_LIS_CLOSE) {
        wgm_lis_t* l = w ? (wgm_lis_t*)wgm_slot_get(w, WGM_LIS, id) : NULL;
        if (l) {
            unl(l->l4);
            unl(l->l6);
            JS_FreeValue(ctx, l->handler);
            wgm_slot_del(w, l->id);
            free(l);
        }
        return JS_UNDEFINED;
    }
    {
        wgm_udp_t* u = w && !w->closing ? (wgm_udp_t*)wgm_slot_get(w, WGM_UDP, id) : NULL;
        if (magic == OP_UDP_CLOSE) {
            if (u) {
                wg_udp_close(u->u4);
                wg_udp_close(u->u6);
                JS_FreeValue(ctx, u->handler);
                wgm_slot_del(w, u->id);
                free(u);
            }
            return JS_UNDEFINED;
        }
        {
            const uint8_t* p;
            size_t n;
            const char* cs;
            wg_ipaddr_t dst;
            uint16_t port;
            wg_udp_t* s;
            int rc;
            if (!u)
                return JS_ThrowTypeError(ctx, "WireGuardUdp.send: socket closed");
            if (wgm_ip_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &dst, "WireGuardUdp.send") < 0 || wgm_port_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, 1, &port, "WireGuardUdp.send") < 0)
                return JS_EXCEPTION;
            s = dst.v6 ? u->u6 : u->u4;
            if (!s)
                return JS_ThrowTypeError(ctx, "WireGuardUdp.send: no %s address configured", dst.v6 ? "IPv6" : "IPv4");
            if (wgm_bytes(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &p, &n, &cs, "WireGuardUdp.send") < 0)
                return JS_EXCEPTION;
            if (n > WGM_UDP_MAX) {
                if (cs)
                    JS_FreeCString(ctx, cs);
                return JS_ThrowRangeError(ctx, "WireGuardUdp.send: datagram of %zu bytes exceeds 65507", n);
            }
            rc = wg_udp_sendto(s, &dst, port, p, n);
            if (cs)
                JS_FreeCString(ctx, cs);
            wgm_kick(w);
            return JS_NewBool(ctx, rc >= 0);
        }
    }
}

static JSValue small_obj(wgm_t* w, uint32_t id, uint16_t port, int kind)
{
    JSContext* ctx = w->ctx;
    JSValue o = JS_NewObject(ctx), data[2];
    data[0] = w->obj;
    data[1] = JS_NewUint32(ctx, id);
    JS_DefinePropertyValueStr(ctx, o, "port", JS_NewInt32(ctx, port), JS_PROP_ENUMERABLE);
    if (kind == WGM_UDP)
        JS_DefinePropertyValueStr(ctx, o, "send", JS_NewCFunctionData(ctx, obj_fn, 3, OP_UDP_SEND, 2, data), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_DefinePropertyValueStr(ctx, o, "close", JS_NewCFunctionData(ctx, obj_fn, 0, kind == WGM_UDP ? OP_UDP_CLOSE : OP_LIS_CLOSE, 2, data), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return o;
}

JSValue wgm_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w = wgm_of(this_val);
    uint16_t port;
    wgm_lis_t* l;
    if (!w || w->closing)
        return JS_ThrowTypeError(ctx, "WireGuard: closed");
    if (!wg_glue_tcp(w->g))
        return JS_ThrowTypeError(ctx, "WireGuard.listen: needs the userspace stack (with tun the kernel routes TCP)");
    if (wgm_port_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 1, &port, "WireGuard.listen") < 0)
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "WireGuard.listen: handler must be a function");
    l = (wgm_lis_t*)calloc(1, sizeof *l);
    if (!l)
        return JS_ThrowOutOfMemory(ctx);
    l->w = w;
    l->port = port;
    if ((w->has4 && !(l->l4 = wg_tcp_listen(wg_glue_tcp(w->g), 0, port, 128, &LIS_CB, l))) || (w->has6 && !(l->l6 = wg_tcp_listen(wg_glue_tcp(w->g), 1, port, 128, &LIS_CB, l)))) {
        unl(l->l4);
        free(l);
        return JS_ThrowTypeError(ctx, "WireGuard.listen: port %u is in use", port);
    }
    if (wgm_slot_add(w, WGM_LIS, l, &l->id) < 0) {
        unl(l->l4);
        unl(l->l6);
        free(l);
        return JS_ThrowOutOfMemory(ctx);
    }
    l->handler = JS_DupValue(ctx, argv[1]);
    return small_obj(w, l->id, port, WGM_LIS);
}

static void udp_rx(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    wgm_udp_t* u = (wgm_udp_t*)ud;
    wgm_t* w = u->w;
    JSContext* ctx = w->ctx;
    JSValue a[3], r;
    (void)s;
    if (w->closing)
        return;
    a[0] = dyn_nat_u8_copy(ctx, d, n);
    if (JS_IsException(a[0])) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    a[1] = wgm_ip_str(ctx, src);
    a[2] = JS_NewInt32(ctx, sport);
    r = JS_Call(ctx, u->handler, JS_UNDEFINED, 3, (JSValueConst*)a);
    if (JS_IsException(r))
        wgm_call_error(w);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
}

JSValue wgm_udp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w = wgm_of(this_val);
    uint16_t port;
    wgm_udp_t* u;
    if (!w || w->closing)
        return JS_ThrowTypeError(ctx, "WireGuard: closed");
    if (!wg_glue_ip(w->g))
        return JS_ThrowTypeError(ctx, "WireGuard.udp: needs the userspace stack (with tun the kernel routes UDP)");
    if (wgm_port_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0, &port, "WireGuard.udp") < 0)
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "WireGuard.udp: handler must be a function");
    u = (wgm_udp_t*)calloc(1, sizeof *u);
    if (!u)
        return JS_ThrowOutOfMemory(ctx);
    u->w = w;
    if (w->has4 && (u->u4 = wg_udp_bind(wg_glue_ip(w->g), 0, port, udp_rx, u)) != NULL)
        port = wg_udp_port(u->u4);
    if (w->has6)
        u->u6 = wg_udp_bind(wg_glue_ip(w->g), 1, port, udp_rx, u);
    if ((w->has4 && !u->u4) || (w->has6 && !u->u6) || wgm_slot_add(w, WGM_UDP, u, &u->id) < 0) {
        wg_udp_close(u->u4);
        wg_udp_close(u->u6);
        free(u);
        return JS_ThrowTypeError(ctx, "WireGuard.udp: port %u is in use", port);
    }
    u->port = port;
    u->handler = JS_DupValue(ctx, argv[1]);
    return small_obj(w, u->id, port, WGM_UDP);
}

void wgm_conns_shutdown(wgm_t* w)
{
    uint32_t i;
    for (i = 0; i < w->nslots; i++) {
        wgm_slot_t* s = &w->slots[i];
        if (!s->p)
            continue;
        if (s->kind == WGM_CONN) {
            wgm_conn_t* c = (wgm_conn_t*)s->p;
            if (c->tc) {
                wg_tcp_set_cb(c->tc, NULL, NULL);
                wg_tcp_abort(c->tc);
                c->tc = NULL;
            }
            conn_settle_all(c, 1, "closed");
            conn_free(c);
        } else if (s->kind == WGM_LIS) {
            wgm_lis_t* l = (wgm_lis_t*)s->p;
            unl(l->l4);
            unl(l->l6);
            JS_FreeValueRT(w->rt, l->handler);
            free(l);
        } else {
            wgm_udp_t* u = (wgm_udp_t*)s->p;
            wg_udp_close(u->u4);
            wg_udp_close(u->u6);
            JS_FreeValueRT(w->rt, u->handler);
            free(u);
        }
        s->p = NULL;
    }
}

void wgm_conns_mark(JSRuntime* rt, wgm_t* w, JS_MarkFunc* mf)
{
    uint32_t i;
    for (i = 0; i < w->nslots; i++) {
        wgm_slot_t* s = &w->slots[i];
        if (!s->p)
            continue;
        if (s->kind == WGM_CONN) {
            wgm_conn_t* c = (wgm_conn_t*)s->p;
            JS_MarkValue(rt, c->ctl, mf);
            JS_MarkValue(rt, c->wctl, mf);
            JS_MarkValue(rt, c->dres, mf);
            JS_MarkValue(rt, c->drej, mf);
            JS_MarkValue(rt, c->wres, mf);
            JS_MarkValue(rt, c->wrej, mf);
            JS_MarkValue(rt, c->cres, mf);
            JS_MarkValue(rt, c->crej, mf);
        } else if (s->kind == WGM_LIS)
            JS_MarkValue(rt, ((wgm_lis_t*)s->p)->handler, mf);
        else
            JS_MarkValue(rt, ((wgm_udp_t*)s->p)->handler, mf);
    }
}

#endif
