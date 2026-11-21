#include "net-tcp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

static const char* const udp_ctor_keys[] = { "port", "host" };

static const char* const udp_start_keys[] = { "message" };

static _Thread_local dyn_udp_t** udp_grave;

static _Thread_local int udp_n_grave, udp_cap_grave;

static _Thread_local int udp_in_delivery;

static void dyn_udp_shell_release(dyn_udp_t* u)
{
    dyn_nat_untrack(sizeof(*u));
    free(u);
}

static void udp_grave_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_udp_t* u = (dyn_udp_t*)opaque;
    (void)ctx;
    if (!u || !u->dead)
        return;
    dyn_nat_untrack(sizeof(*u));
    JS_ShutdownDeferFree(rt, u);
}

static void udp_grave_flush(void* unused)
{
    int i;
    (void)unused;
    for (i = 0; i < udp_n_grave; i++) {
        dyn_udp_t* u = udp_grave[i];
        JS_RemoveShutdownSweep(u->rt, udp_grave_sweep, u);
        JS_ShutdownUndeferFree(u->rt, u);
        dyn_udp_shell_release(u);
    }
    udp_n_grave = 0;
    free(udp_grave);
    udp_grave = NULL;
    udp_cap_grave = 0;
}

void dyn_udp_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_udp_class_id);
    dyn_udp_t* u = (r && !r->closed) ? (dyn_udp_t*)r->native : NULL;

    if (!u)
        return;
    JS_MarkValue(rt, u->h_message, mark_func);
}

static void dyn_udp_teardown(dyn_udp_t* u)
{
    if (u->aio) {
        if (u->fd >= 0)
            dyn_aio_close(u->aio, u->fd);
        dyn_net_reactor_release_rt(u->rt);
        u->aio = NULL;
    }
    JS_FreeValueRT(u->rt, u->h_message);
    u->h_message = JS_UNDEFINED;
}

static void dyn_udp_dispose(void* native)
{
    dyn_udp_t* u = (dyn_udp_t*)native;
    if (!u)
        return;
    dyn_udp_teardown(u);
    if (udp_in_delivery) {
        u->dead = 1;
        if (udp_n_grave == udp_cap_grave) {
            int cap = udp_cap_grave ? udp_cap_grave * 2 : 8;
            dyn_udp_t** n = (dyn_udp_t**)realloc(udp_grave,
                (size_t)cap * sizeof(*n));
            if (!n) {
                return;
            }
            udp_grave = n;
            udp_cap_grave = cap;
        }
        udp_grave[udp_n_grave++] = u;
        JS_AddShutdownSweep(u->rt, udp_grave_sweep, u);
        (void)dyn_net_on_drain_notick(udp_grave_flush, NULL);
        return;
    }
    dyn_udp_shell_release(u);
}

static void udp_on_message(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, const struct sockaddr* peer,
    unsigned peerlen, void* ud)
{
    dyn_udp_t* u = (dyn_udp_t*)ud;
    JSContext* ctx;
    char ip[64] = "";
    JSValue ab, u8, from, r, h;
    JSValueConst a[2];
    (void)aio;
    (void)peerlen;

    if (u->dead)
        return;
    ctx = u->ctx;
    udp_in_delivery++;
    h = JS_DupValue(ctx, u->h_message);
    if (res < 0 || !JS_IsFunction(ctx, h))
        goto out;
    ab = JS_NewArrayBufferCopy(ctx, buf, len);
    if (JS_IsException(ab))
        goto out;
    {
        JSValueConst ta[3] = { ab, JS_NewInt32(ctx, 0),
            JS_NewInt32(ctx, (int)len) };
        u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8))
        goto out;
    from = JS_NewObject(ctx);
    if (JS_IsException(from)) {
        JS_FreeValue(ctx, u8);
        goto out;
    }
    if (peer && peer->sa_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)peer;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        JS_DefinePropertyValueStr(ctx, from, "address", JS_NewString(ctx, ip),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, from, "port",
            JS_NewInt32(ctx, ntohs(sin->sin_port)),
            JS_PROP_C_W_E);
    } else if (peer && peer->sa_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (const struct sockaddr_in6*)peer;
        if (getnameinfo(peer, sizeof(*s6), ip, sizeof(ip), NULL, 0, NI_NUMERICHOST) != 0)
            ip[0] = 0;
        JS_DefinePropertyValueStr(ctx, from, "address", JS_NewString(ctx, ip),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, from, "port",
            JS_NewInt32(ctx, ntohs(s6->sin6_port)),
            JS_PROP_C_W_E);
    }
    a[0] = u8;
    a[1] = from;
    r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
    dyn_net_handler_result(ctx, r, NET_HK_UDP_MESSAGE);
    JS_FreeValue(ctx, u8);
    JS_FreeValue(ctx, from);
out:
    JS_FreeValue(ctx, h);
    udp_in_delivery--;
}

JSValue dyn_udp_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_udp_t* u;
    int64_t port = 0;
    const char* host = NULL;
    struct sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    JSValue v;

    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], udp_ctor_keys, 2))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        host = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (port < 0 || port > 65535) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "UDPSocket: port must be 0..65535");
    }
    u = (dyn_udp_t*)calloc(1, sizeof(*u));
    if (!u) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_nat_track(sizeof(*u))) {
        free(u);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    u->ctx = ctx;
    u->rt = JS_GetRuntime(ctx);
    u->fd = -1;
    u->h_message = JS_UNDEFINED;
    u->aio = dyn_net_reactor_acquire(ctx);
    if (!u->aio) {
        dyn_udp_shell_release(u);
        if (host)
            JS_FreeCString(ctx, host);
        return dyn_net_throw_no_reactor(ctx, "UDPSocket");
    }
    u->fd = dyn_aio_udp_bind(u->aio, host, (uint16_t)port);
    if (host)
        JS_FreeCString(ctx, host);
    if (u->fd < 0) {
        dyn_net_reactor_release(ctx);
        dyn_udp_shell_release(u);
        return JS_ThrowInternalError(ctx, "UDPSocket: bind failed");
    }
    if (getsockname(u->fd, (struct sockaddr*)&sa, &sl) == 0) {
        u->v6 = sa.ss_family == AF_INET6;
        u->port = sa.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6*)&sa)->sin6_port)
                                           : ntohs(((struct sockaddr_in*)&sa)->sin_port);
    }
    return dyn_res_wrap(ctx, new_target, dyn_udp_class_id, u, dyn_udp_dispose);
}

JSValue dyn_udp_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_udp_t* u;
    JSValue h = JS_UNDEFINED;
    int have_h = 0;

    if (argc > 0 && dyn_opts_strict(ctx, argv[0], udp_start_keys, 1))
        return JS_EXCEPTION;
    if (argc > 0 && JS_IsObject(argv[0])) {
        h = JS_GetPropertyStr(ctx, argv[0], "message");
        if (JS_IsException(h))
            return JS_EXCEPTION;
        have_h = 1;
    }
    u = (dyn_udp_t*)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) {
        if (have_h)
            JS_FreeValue(ctx, h);
        return JS_EXCEPTION;
    }
    if (have_h) {
        JS_FreeValue(ctx, u->h_message);
        u->h_message = h;
    }
    if (!u->started) {
        if (dyn_aio_recvfrom(u->aio, u->fd, udp_on_message, u) < 0)
            return JS_ThrowInternalError(ctx, "UDPSocket: recvfrom failed");
        u->started = 1;
    }
    if (dyn_res_pin(ctx, this_val, dyn_udp_class_id) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static const char* udp_errno_code(int e)
{
    switch (e) {
    case EPIPE:
        return "EPIPE";
    case EAGAIN:
        return "EAGAIN";
    case ENOBUFS:
        return "ENOBUFS";
    case EMSGSIZE:
        return "EMSGSIZE";
    case EHOSTUNREACH:
        return "EHOSTUNREACH";
    case ENETUNREACH:
        return "ENETUNREACH";
    case ENETDOWN:
        return "ENETDOWN";
    case EHOSTDOWN:
        return "EHOSTDOWN";
    case EACCES:
        return "EACCES";
    case EPERM:
        return "EPERM";
    case ECONNREFUSED:
        return "ECONNREFUSED";
    case EADDRNOTAVAIL:
        return "EADDRNOTAVAIL";
    case EAFNOSUPPORT:
        return "EAFNOSUPPORT";
    case EINVAL:
        return "EINVAL";
    case EBADF:
        return "EBADF";
    case ENOTSOCK:
        return "ENOTSOCK";
    default:
        return NULL;
    }
}

static JSValue udp_throw_send(JSContext* ctx, int e)
{
    char msg[160];
    const char* code = udp_errno_code(e);
    JSValue err;

    snprintf(msg, sizeof(msg), "UDPSocket: send failed: %s (%s)", strerror(e), code ? code : "errno");
    JS_ThrowInternalError(ctx, "%s", msg);
    err = JS_GetException(ctx);
    JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e), JS_PROP_C_W_E);
    if (code)
        JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

JSValue dyn_udp_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_udp_t* u;
    struct sockaddr_storage sa;
    socklen_t sl = 0;
    const char *host = NULL, *str = NULL;
    int64_t port = 0;
    size_t len = 0;
    const uint8_t* p;
    int n;
    int se;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "send(data, host, port)");
    p = JS_GetBufferKind(argv[0]) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &len, argv[0]) : NULL;
    if (!p) {
        size_t voff = 0, bpe = 0;
        JSValue abv = JS_GetArrayBufferView(ctx, argv[0], &voff, &len, &bpe);
        if (!JS_IsException(abv)) {
            size_t ablen = 0;
            const uint8_t* base = JS_GetArrayBuffer(ctx, &ablen, abv);
            JS_FreeValue(ctx, abv);
            if (!base)
                return JS_EXCEPTION;
            if (voff > ablen)
                voff = ablen;
            if (len > ablen - voff)
                len = ablen - voff;
            p = base + voff;
        } else {
            if (JS_HasUncatchableException(ctx))
                return JS_EXCEPTION;
            JS_FreeValue(ctx, JS_GetException(ctx));
            if (!JS_IsString(argv[0]))
                return JS_ThrowTypeError(ctx,
                    "send: data must be a string or a byte view (BytesInput: "
                    "string | Uint8Array | Int8Array | Uint8ClampedArray | "
                    "DataView | ArrayBuffer)");
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
            p = (const uint8_t*)str;
        }
    }
    host = JS_ToCString(ctx, argv[1]);
    if (!host) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (JS_ToInt64(ctx, &port, argv[2])) {
        JS_FreeCString(ctx, host);
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    u = (dyn_udp_t*)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) {
        JS_FreeCString(ctx, host);
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (port < 1 || port > 65535) {
        JS_FreeCString(ctx, host);
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowRangeError(ctx, "send: port must be 1..65535");
    }
    n = dyn_net_udp_addr(host, (uint16_t)port, u->v6 ? AF_INET6 : AF_INET, &sa, &sl);
    JS_FreeCString(ctx, host);
    if (n < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, n == -2 ? "send: an IPv6 host needs a UDPSocket bound to an IPv6 host" : "send: host must be an IPv4 or IPv6 address");
    }
    n = dyn_aio_sendto(u->aio, u->fd, p, len, (struct sockaddr*)&sa, sl);
    se = errno;
    if (str)
        JS_FreeCString(ctx, str);
    if (n < 0)
        return udp_throw_send(ctx, se);
    return JS_NewInt32(ctx, n);
}

JSValue dyn_udp_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_udp_t* u = (dyn_udp_t*)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, u->port);
}

#endif
