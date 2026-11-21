#include "proxy-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROXY)

#define PXY_DEFAULT_MAX_CONNS 8192
#define PXY_DEFAULT_CONNECT_MS 10000
#define PXY_HOST_MAX 255

static const char* const pxy_ctor_keys[] = { "port", "host", "maxConns", "idleTimeoutMs", "connectTimeoutMs", "upstream" };

int pxy_read_opts(JSContext* ctx, dyn_pxy_t* p, JSValueConst o)
{
    JSValue v;
    int32_t n;
    uint32_t i, cnt;
    JSValue up;

    if (dyn_opts_strict(ctx, o, pxy_ctor_keys, 6))
        return -1;

    p->max_conns = PXY_DEFAULT_MAX_CONNS;
    p->connect_ms = PXY_DEFAULT_CONNECT_MS;

    v = JS_GetPropertyStr(ctx, o, "host");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        size_t hlen;
        const char* hs = JS_ToCStringLen(ctx, &hlen, v);
        JS_FreeValue(ctx, v);
        if (!hs)
            return -1;
        if (hlen == 0 || hlen > PXY_HOST_MAX || memchr(hs, 0, hlen)) {
            JS_FreeCString(ctx, hs);
            JS_ThrowRangeError(ctx, "TCPProxy: host must be a 1..%d character address", PXY_HOST_MAX);
            return -1;
        }
        free(p->bind_host);
        p->bind_host = strdup(hs);
        JS_FreeCString(ctx, hs);
        if (!p->bind_host) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    } else {
        JS_FreeValue(ctx, v);
    }

    v = JS_GetPropertyStr(ctx, o, "port");
    if (JS_IsException(v))
        return -1;
    if (JS_ToInt32(ctx, &n, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (n < 0 || n > 65535) {
        JS_ThrowRangeError(ctx, "listen port out of range");
        return -1;
    }
    p->port = (uint16_t)n;

    v = JS_GetPropertyStr(ctx, o, "maxConns");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt32(ctx, &n, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n >= 0)
            p->max_conns = n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, o, "idleTimeoutMs");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt32(ctx, &n, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n > 0)
            p->idle_ms = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, o, "connectTimeoutMs");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt32(ctx, &n, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n >= 0)
            p->connect_ms = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);

    up = JS_GetPropertyStr(ctx, o, "upstream");
    if (JS_IsException(up))
        return -1;
    if (JS_IsUndefined(up)) {
        JS_ThrowTypeError(ctx, "TCPProxy needs an upstream");
        return -1;
    }
    if (JS_IsArray(ctx, up)) {
        JSValue jl = JS_GetPropertyStr(ctx, up, "length");
        if (JS_ToUint32(ctx, &cnt, jl) < 0) {
            JS_FreeValue(ctx, jl);
            JS_FreeValue(ctx, up);
            return -1;
        }
        JS_FreeValue(ctx, jl);
    } else {
        cnt = 1;
    }
    if (cnt == 0) {
        JS_FreeValue(ctx, up);
        JS_ThrowTypeError(ctx, "TCPProxy needs at least one upstream");
        return -1;
    }
    p->up_host = (char**)calloc(cnt, sizeof(char*));
    p->up_port = (uint16_t*)calloc(cnt, sizeof(uint16_t));
    p->up_sa = (struct sockaddr_storage*)calloc(cnt, sizeof(*p->up_sa));
    p->up_salen = (socklen_t*)calloc(cnt, sizeof(socklen_t));
    p->up_fam = (int*)calloc(cnt, sizeof(int));
    if (!p->up_host || !p->up_port || !p->up_sa || !p->up_salen || !p->up_fam) {
        JS_FreeValue(ctx, up);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < cnt; i++) {
        JSValue e = JS_IsArray(ctx, up) ? JS_GetPropertyUint32(ctx, up, i)
                                        : JS_DupValue(ctx, up);
        int rc;
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, up);
            return -1;
        }
        rc = pxy_read_upstream(ctx, p, e, (int)i);
        JS_FreeValue(ctx, e);
        if (rc < 0) {
            JS_FreeValue(ctx, up);
            return -1;
        }
        p->n_up = (int)i + 1;
    }
    JS_FreeValue(ctx, up);
    return 0;
}

#endif
