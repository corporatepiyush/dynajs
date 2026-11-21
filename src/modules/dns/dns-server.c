#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

#define DNS_AMP_FACTOR 4
#define DNS_RRL_SLOTS 64
#define DNS_RRL_PER_SEC 20

typedef struct {
    uint32_t addr;
    uint32_t tokens;
    uint64_t refilled_ms;
} dns_rrl_t;

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    uint16_t port;
    int started;
    JSValue handler;
    dns_rrl_t rrl[DNS_RRL_SLOTS];
    uint8_t in_cb, closing;
} dyn_dnssrv_t;

static const char* const dns_server_keys[] = { "port", "host" };

static void dyn_dnssrv_teardown(dyn_dnssrv_t* s);

void dyn_dnssrv_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_dnssrv_class_id);
    dyn_dnssrv_t* s;

    if (!res || res->closed || !res->native)
        return;
    s = (dyn_dnssrv_t*)res->native;
    JS_MarkValue(rt, s->handler, mark_func);
}

static uint32_t dns_rrl_key(const struct sockaddr* peer)
{
    if (peer->sa_family == AF_INET) {
        uint32_t a = ((const struct sockaddr_in*)peer)->sin_addr.s_addr;
        return a ? a : 1u;
    }
    if (peer->sa_family == AF_INET6) {
        const uint8_t* p = ((const struct sockaddr_in6*)peer)->sin6_addr.s6_addr;
        uint32_t h = 2166136261u;
        int i;
        for (i = 0; i < 16; i++) {
            h ^= p[i];
            h *= 16777619u;
        }
        return h ? h : 1u;
    }
    return 0;
}

static int dns_rrl_allow(dyn_dnssrv_t* s, uint32_t addr)
{
    uint64_t now = dyn_timer_now_ms();
    int i, free_slot = -1, oldest = 0;

    for (i = 0; i < DNS_RRL_SLOTS; i++) {
        if (s->rrl[i].addr == addr) {
            uint64_t elapsed = now - s->rrl[i].refilled_ms;
            if (elapsed >= 1000) {
                s->rrl[i].tokens = DNS_RRL_PER_SEC;
                s->rrl[i].refilled_ms = now;
            }
            if (s->rrl[i].tokens == 0)
                return 0;
            s->rrl[i].tokens--;
            return 1;
        }
        if (!s->rrl[i].addr && free_slot < 0)
            free_slot = i;
        if (s->rrl[i].refilled_ms < s->rrl[oldest].refilled_ms)
            oldest = i;
    }
    i = (free_slot >= 0) ? free_slot : oldest;
    s->rrl[i].addr = addr;
    s->rrl[i].tokens = DNS_RRL_PER_SEC - 1;
    s->rrl[i].refilled_ms = now;
    return 1;
}

static void dnssrv_on_query(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, const struct sockaddr* peer,
    unsigned peerlen, void* ud)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)ud;
    JSContext* ctx = s->ctx;
    dyn_dns_hdr_t h;
    char qname[DYN_DNS_MAX_NAME + 1];
    uint8_t out[512];
    uint16_t qtype;
    int after, off, n_ans = 0;
    JSValue r;
    (void)aio;

    if (res < 0 || len < DYN_DNS_HDR_LEN || !peer)
        return;
    {
        uint32_t k = dns_rrl_key(peer);
        if (k && !dns_rrl_allow(s, k))
            return;
    }
    if (dyn_dns_hdr_decode(buf, len, &h) < 0 || (h.flags & 0x8000))
        return;

    after = dyn_dns_name_decode(buf, len, DYN_DNS_HDR_LEN, qname, sizeof(qname));
    if (after < 0 || (size_t)after + 4 > len)
        return;
    qtype = (uint16_t)((buf[after] << 8) | buf[after + 1]);

    off = dyn_dns_begin_response(buf, len, 0, out, sizeof(out));
    if (off < 0)
        return;

    if (JS_IsFunction(ctx, s->handler)) {
        JSValueConst a[2];
        a[0] = JS_NewString(ctx, qname);
        a[1] = JS_NewInt32(ctx, qtype);
        s->in_cb = 1;
        r = JS_Call(ctx, s->handler, JS_UNDEFINED, 2, a);
        s->in_cb = 0;
        if (s->closing) {
            JS_FreeValue(ctx, (JSValue)a[0]);
            if (JS_IsException(r))
                dyn_exc_drop(ctx);
            JS_FreeValue(ctx, r);
            dyn_dnssrv_teardown(s);
            return;
        }
        JS_FreeValue(ctx, (JSValue)a[0]);
        if (JS_IsException(r)) {
            dyn_exc_drop(ctx);
            JS_FreeValue(ctx, r);
            return;
        }
        if (JS_IsString(r)) {
            const char* ip = JS_ToCString(ctx, r);
            uint8_t v4[4], v6[16];
            if (ip) {
                if (qtype == DYN_DNS_T_A && inet_pton(AF_INET, ip, v4) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                        DYN_DNS_T_A, 60, v4, 4);
                    if (no > 0) {
                        off = no;
                        n_ans = 1;
                    }
                } else if (qtype == DYN_DNS_T_AAAA && inet_pton(AF_INET6, ip, v6) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                        DYN_DNS_T_AAAA, 60, v6, 16);
                    if (no > 0) {
                        off = no;
                        n_ans = 1;
                    }
                }
                JS_FreeCString(ctx, ip);
            }
        }
        JS_FreeValue(ctx, r);
    }
    dyn_dns_set_ancount(out, (uint16_t)n_ans);

    if ((size_t)off > (size_t)len * DNS_AMP_FACTOR) {
        out[2] |= 0x02;
        dyn_dns_set_ancount(out, 0);
        off = (int)(DYN_DNS_HDR_LEN + ((size_t)after + 4 - DYN_DNS_HDR_LEN));
    }
    dyn_aio_sendto(s->aio, s->fd, out, (size_t)off, peer, peerlen);
}

static void dyn_dnssrv_teardown(dyn_dnssrv_t* s)
{
    if (!s)
        return;
    if (s->aio) {
        if (s->fd >= 0)
            dyn_aio_close(s->aio, s->fd);
        dyn_net_reactor_release_rt(s->rt);
    }
    JS_FreeValueRT(s->rt, s->handler);
    free(s);
}

static void dyn_dnssrv_dispose(void* native)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)native;
    if (!s)
        return;
    if (s->in_cb) {
        s->closing = 1;
        return;
    }
    dyn_dnssrv_teardown(s);
}

JSValue dyn_dnssrv_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_dnssrv_t* s;
    int64_t port = 0;
    const char* host = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    JSValue v;

    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], dns_server_keys, 2))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
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
    }
    if (port < 0 || port > 65535) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "DNSServer: port must be 0..65535");
    }
    s = (dyn_dnssrv_t*)calloc(1, sizeof(*s));
    if (!s) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->ctx = ctx;
    s->rt = JS_GetRuntime(ctx);
    s->fd = -1;
    s->handler = JS_UNDEFINED;
    s->aio = dyn_net_reactor_acquire(ctx);
    if (!s->aio) {
        free(s);
        if (host)
            JS_FreeCString(ctx, host);
        return dyn_net_throw_no_reactor(ctx, "DNSServer");
    }
    s->fd = dyn_aio_udp_bind(s->aio, host ? host : "127.0.0.1", (uint16_t)port);
    if (host)
        JS_FreeCString(ctx, host);
    if (s->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(s);
        return JS_ThrowInternalError(ctx, "DNSServer: bind failed");
    }
    if (getsockname(s->fd, (struct sockaddr*)&sa, &sl) == 0)
        s->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, new_target, dyn_dnssrv_class_id, s, dyn_dnssrv_dispose);
}

JSValue dyn_dnssrv_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)dyn_res_native(ctx, this_val,
        dyn_dnssrv_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (argc > 0) {
        JS_FreeValue(ctx, s->handler);
        s->handler = JS_DupValue(ctx, argv[0]);
    }
    if (!s->started) {
        if (dyn_aio_recvfrom(s->aio, s->fd, dnssrv_on_query, s) < 0)
            return JS_ThrowInternalError(ctx, "DNSServer: recv failed");
        s->started = 1;
    }
    if (dyn_res_pin(ctx, this_val, dyn_dnssrv_class_id) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

JSValue dyn_dnssrv_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)dyn_res_native(ctx, this_val,
        dyn_dnssrv_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

#endif
