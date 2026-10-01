#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dyna-tls.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const tcp_srv_keys[] = { "port", "path", "maxConnections", "idleTimeoutMs", "tls", "highWaterMark" };
static const char* const tcp_cli_keys[] = { "path", "host", "port", "maxConnections", "idleTimeoutMs",
    "connectTimeoutMs", "tls", "highWaterMark" };
#ifdef CONFIG_TLS
static const char* const tcp_tls_srv_keys[] = { "cert", "key", "alpn", "ca", "requestCert" };
static const char* const tcp_tls_cli_keys[] = { "ca", "servername", "minVersion", "rejectUnauthorized", "cert", "key",
    "alpn" };
#endif
static const char* const tcp_handler_keys[] = { "connect", "data", "close", "drain" };
static const char* const tcp_eb_keys[] = { "fallbackMs", "tls" };
static const char* const udp_ctor_keys[] = { "port", "host" };
static const char* const udp_start_keys[] = { "message" };

#define TCP_HWM_DEFAULT (4u << 20)
#define TCP_HWM_MAX (1u << 30)

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);

typedef struct dyn_tcp dyn_tcp_t;

typedef struct eb_attempt {
    dyn_tcp_t* owner;
    int idx;
    int fd;
} eb_attempt_t;

typedef struct dyn_tcp_conn {
    dyn_tcp_t* owner;
    int fd;
    int closed;
    int refs;
    int drain_armed;
    uint64_t hwm;
    JSValue jsobj;
    struct dyn_tcp_conn *lnext, *lprev;
    uint64_t last_ms;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    int tls_up;
#endif
} dyn_tcp_conn_t;

struct dyn_tcp {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    int started;
    char* path;
    JSValue h_connect, h_data, h_close, h_drain;
    uint64_t hwm;
    JSValue self_pending;

    dyn_tcp_conn_t* pending_conn;

    dyn_tcp_conn_t* conns;
    int nconns;
    int max_conns;
    uint64_t idle_ms;
    uint64_t connect_deadline_ms;
    int hooked;
    int released;
    uint64_t n_refused;
    uint64_t n_idle_closed;
    eb_attempt_t eb[2];
    int in_cb;
    int closing;
    uint64_t eb_deadline_ms;
    int eb_done;
    int eb_err;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls_ctx;
    char* tls_servername;
#endif
};

static JSClassID dyn_tcp_class_id;
static JSClassID dyn_tcp_conn_class_id;

static _Atomic(uint64_t) dyn_net_handler_throws;
static _Atomic(unsigned) dyn_net_throw_diag_seen;
static int dyn_net_debug;

enum {
    NET_HK_TCP_CONNECT = 0,
    NET_HK_TCP_DATA,
    NET_HK_TCP_CLOSE,
    NET_HK_UDP_MESSAGE,
    NET_HK_TCP_DRAIN,
    NET_HK_N
};
static const char* const dyn_net_handler_kind_names[NET_HK_N] = {
    "TCPSocket connect",
    "TCPSocket data",
    "TCPSocket close",
    "UDPSocket message",
    "TCPSocket drain",
};

uint64_t dyn_net_handler_throw_count(void)
{
    return dyn_net_handler_throws;
}

void dyn_net_set_debug(int on)
{
    dyn_net_debug = on;
}

static JSValue dyn_net_handler_throws_getter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, (int64_t)dyn_net_handler_throws);
}

static void dyn_net_handler_result(JSContext* ctx, JSValue r, int kind)
{
    if (!JS_IsException(r)) {
        JS_FreeValue(ctx, r);
        return;
    }
    dyn_net_handler_throws++;
    if (dyn_net_debug) {
        unsigned bit = (unsigned)1 << kind;
        if ((dyn_net_throw_diag_seen & bit) == 0) {
            dyn_net_throw_diag_seen |= bit;
            fprintf(stderr, "dyna: net: swallowed throw from a %s handler",
                dyn_net_handler_kind_names[kind]);
            {
                JSValue err = JS_GetException(ctx);
                JSValue s = JS_ToString(ctx, err);
                if (!JS_IsException(s)) {
                    const char* str = JS_ToCString(ctx, s);
                    fprintf(stderr, ": %s", str ? str : "(unreadable)");
                    JS_FreeCString(ctx, str);
                }
                JS_FreeValue(ctx, s);
                JS_Throw(ctx, err);
            }
            fprintf(stderr, " (further throws counted silently;"
                            " see net.swallowedHandlerThrows())\n");
        }
    }
    {
        JSValue err = JS_GetException(ctx);
        JS_FreeValue(ctx, err);
    }
    JS_FreeValue(ctx, r);
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
    JS_MarkValue(rt, t->self_pending, mark_func);
    for (c = t->conns; c; c = c->lnext)
        JS_MarkValue(rt, c->jsobj, mark_func);
}

static const JSClassDef dyn_tcp_class = {
    "TCPServer",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_tcp_gc_mark,
};

static void dyn_tcp_teardown(dyn_tcp_t* t);

static int tcp_gone(dyn_tcp_t* t)
{
    if (t->closing) {
        dyn_tcp_teardown(t);
        return 1;
    }
    return 0;
}

static void dyn_tcp_conn_finalizer(JSRuntime* rt, JSValue val);

static const JSClassDef dyn_tcp_conn_class = {
    "TCPConn",
    .finalizer = dyn_tcp_conn_finalizer,
};

static void tcp_conn_unref(JSContext* ctx, dyn_tcp_conn_t* c)
{
    if (--c->refs == 0) {
        JS_FreeValue(ctx, c->jsobj);
#ifdef CONFIG_TLS
        dyn_tls_conn_free(c->tls);
#endif
        free(c);
    }
}

static void tcp_conn_drop(JSRuntime* rt, dyn_tcp_conn_t* c)
{
    JS_FreeValueRT(rt, c->jsobj);
    c->jsobj = JS_UNDEFINED;
    if (--c->refs == 0) {
#ifdef CONFIG_TLS
        dyn_tls_conn_free(c->tls);
#endif
        free(c);
    }
}

static void dyn_tcp_conn_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(val, dyn_tcp_conn_class_id);
    (void)rt;
    if (c) {
        c->jsobj = JS_UNDEFINED;
        if (--c->refs == 0) {
#ifdef CONFIG_TLS
            dyn_tls_conn_free(c->tls);
#endif
            free(c);
        }
    }
}

static void tcp_release_pending(JSRuntime* rt, dyn_tcp_t* t);
#ifdef CONFIG_TLS
static int tls_flush(dyn_tcp_conn_t* c);
#endif

static void tcp_list_unlink(dyn_tcp_conn_t* c)
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

static void tcp_conn_close(dyn_tcp_conn_t* c)
{
    JSContext* ctx;
    dyn_aio_t* aio;
    JSValue h;
    int fd;

    if (c->closed)
        return;
    ctx = c->owner->ctx;
    aio = c->owner->aio;
    fd = c->fd;
    h = JS_DupValue(ctx, c->owner->h_close);

    c->closed = 1;
    tcp_list_unlink(c);
    dyn_aio_close(aio, fd);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
        dyn_tcp_t* own = c->owner;
        JSValueConst a[1] = { c->jsobj };
        own->in_cb = 1;
        JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
        own->in_cb = 0;
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
}

static void tcp_conn_sent(dyn_aio_t* aio, int res, const uint8_t* buf,
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
    {
        JSContext* ctx = c->owner->ctx;
        JSValue h = JS_DupValue(ctx, c->owner->h_drain);
        if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
            dyn_tcp_t* own = c->owner;
            JSValueConst a[1] = { c->jsobj };
            own->in_cb = 1;
            JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
            own->in_cb = 0;
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

static JSValue dyn_tcp_conn_buffered(JSContext* ctx, JSValueConst this_val)
{
    dyn_tcp_conn_t* c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)ctx;
    if (!c || c->closed || !c->owner)
        return JS_NewInt64(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)dyn_aio_queued(c->owner->aio, c->fd));
}

static JSValue dyn_tcp_conn_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_conn_t* c;
    size_t len = 0, off = 0, bpe = 0;
    const uint8_t* p = NULL;
    const char* str = NULL;
    JSValue ab;
    int rc;

    ab = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (!JS_IsException(ab)) {
        size_t total = 0;
        uint8_t* base = JS_GetArrayBuffer(ctx, &total, ab);
        if (!base) {
            JS_FreeValue(ctx, ab);
            return JS_EXCEPTION;
        }
        p = base + off;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        p = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (!p) {
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
        if (queued + len > c->hwm) {
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
        rc = dyn_aio_send(c->owner->aio, c->fd, p, len, 0, tcp_conn_sent, c);
    if (rc == 0 && dyn_aio_queued(c->owner->aio, c->fd) > 0)
        c->drain_armed = 1;
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, ab);
    if (rc < 0)
        return JS_ThrowInternalError(ctx, "TCPConn: send failed");
    return JS_UNDEFINED;
}

static JSValue dyn_tcp_conn_close_method(JSContext* ctx, JSValueConst this_val,
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

static const JSCFunctionListEntry dyn_tcp_conn_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_tcp_conn_write),
    JS_CFUNC_DEF("close", 0, dyn_tcp_conn_close_method),
    JS_CGETSET_DEF("bufferedAmount", dyn_tcp_conn_buffered, NULL),
};

static void tcp_report_connect(dyn_tcp_t* t, JSValueConst obj, const char* err)
{
    JSContext* ctx = t->ctx;
    JSValue h = JS_DupValue(ctx, t->h_connect);
    JSValue pend = t->self_pending;
    t->self_pending = JS_UNDEFINED;

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
        t->in_cb = 1;
        r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
        t->in_cb = 0;
        (void)tcp_gone(t);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, msg);
    }
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, pend);
}

#ifdef CONFIG_TLS
static void tls_fail(dyn_tcp_conn_t* c, const char* fallback)
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

static int tls_flush(dyn_tcp_conn_t* c)
{
    uint8_t out[16384];
    int n;
    if (c->closed || !c->owner)
        return 0;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0) {
        if (dyn_aio_send(c->owner->aio, c->fd, out, (unsigned)n, 0,
                tcp_conn_sent, c)
            < 0)
            return -1;
    }
    return 0;
}

static int tcp_setup_tls_server(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    const char *cert = NULL, *key = NULL, *ca = NULL;
    char *alpn = NULL, err[256];
    dyn_tls_srv_opts_t so;
    JSValue w;
    int rc = -1, request_cert = 0;

    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (dyn_opts_strict(ctx, v, tcp_tls_srv_keys, 5)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    w = JS_GetPropertyStr(ctx, v, "cert");
    if (JS_IsString(w))
        cert = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "key");
    if (JS_IsString(w))
        key = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "alpn");
    if (JS_IsString(w)) {
        const char* sa = JS_ToCString(ctx, w);
        if (sa) {
            alpn = strdup(sa);
            JS_FreeCString(ctx, sa);
        }
    }
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "ca");
    if (JS_IsString(w))
        ca = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "requestCert");
    if (JS_ToBool(ctx, w) == 1)
        request_cert = 1;
    JS_FreeValue(ctx, w);
    memset(&so, 0, sizeof so);
    so.cert = cert;
    so.key = key;
    so.alpn = alpn;
    so.ca_file = ca;
    so.request_cert = request_cert;
    if (!cert || !key) {
        JS_ThrowTypeError(ctx, "TCPServer: tls needs both `cert` and `key` "
                               "(PEM paths); there is no self-signed default");
        goto done;
    }
    t->tls_ctx = dyn_tls_ctx_server(&so, err, sizeof err);
    if (!t->tls_ctx) {
        JS_ThrowInternalError(ctx, "TCPServer: tls: %s", err);
        goto done;
    }
    rc = 0;
done:
    if (cert)
        JS_FreeCString(ctx, cert);
    if (key)
        JS_FreeCString(ctx, key);
    if (ca)
        JS_FreeCString(ctx, ca);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}

static int tcp_setup_tls(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts,
    const char* host)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    dyn_tls_opts_t o;
    const char *ca = NULL, *sni = NULL, *minv = NULL;
    const char *cert = NULL, *key = NULL;
    char *alpn = NULL, err[192];
    int rc = -1;

    if (JS_IsUndefined(v) || JS_IsNull(v) || (JS_IsBool(v) && !JS_ToBool(ctx, v))) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.min_version = 12;
    if (JS_IsObject(v)) {
        if (dyn_opts_strict(ctx, v, tcp_tls_cli_keys, 7)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JSValue w;
        w = JS_GetPropertyStr(ctx, v, "ca");
        if (JS_IsString(w))
            ca = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "servername");
        if (JS_IsString(w))
            sni = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "minVersion");
        if (JS_IsString(w))
            minv = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "rejectUnauthorized");
        if (JS_IsBool(w) && !JS_ToBool(ctx, w))
            o.insecure = 1;
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "cert");
        if (JS_IsString(w))
            cert = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "key");
        if (JS_IsString(w))
            key = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "alpn");
        if (JS_IsString(w)) {
            const char* s = JS_ToCString(ctx, w);
            if (s) {
                alpn = strdup(s);
                JS_FreeCString(ctx, s);
            }
        } else if (JS_IsArray(ctx, w)) {
            JSValue lv = JS_GetPropertyStr(ctx, w, "length");
            uint32_t i, n = 0;
            JS_ToUint32(ctx, &n, lv);
            JS_FreeValue(ctx, lv);
            for (i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, w, i);
                const char* s = JS_ToCString(ctx, e);
                if (s) {
                    size_t slen = strlen(s);
                    size_t have = alpn ? strlen(alpn) : 0;
                    char* nw = (char*)realloc(alpn, have + slen + 2);
                    if (nw) {
                        alpn = nw;
                        if (have)
                            alpn[have++] = ',';
                        memcpy(alpn + have, s, slen + 1);
                    }
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, w);
    }
    o.ca_file = ca;
    o.cert = cert;
    o.key = key;
    o.alpn = alpn;
    if (minv && strstr(minv, "1.3"))
        o.min_version = 13;

    t->tls_servername = strdup(sni ? sni : (host ? host : ""));
    t->tls_ctx = dyn_tls_ctx_client(&o, err, sizeof err);
    if (!t->tls_ctx)
        JS_ThrowInternalError(ctx, "tls: %s", err);
    else
        rc = 0;

    if (ca)
        JS_FreeCString(ctx, ca);
    if (cert)
        JS_FreeCString(ctx, cert);
    if (key)
        JS_FreeCString(ctx, key);
    if (sni)
        JS_FreeCString(ctx, sni);
    if (minv)
        JS_FreeCString(ctx, minv);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}
#endif

static void tcp_deliver(dyn_tcp_conn_t* c, const uint8_t* buf, unsigned len)
{
    JSContext* ctx;
    if (c->closed || !c->owner)
        return;
    ctx = c->owner->ctx;
    if (JS_IsFunction(ctx, c->owner->h_data) && !JS_IsUndefined(c->jsobj)) {
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
                own->in_cb = 1;
                r = JS_Call(ctx, own->h_data, JS_UNDEFINED, 2, a);
                own->in_cb = 0;
                gone = tcp_gone(own);
                dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
                JS_FreeValue(ctx, u8);
                JS_FreeValue(ctx, ab);
                if (gone)
                    return;
                c->last_ms = dyn_timer_now_ms();
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

    if (res <= 0) {
        tcp_conn_close(c);
        return;
    }
    c->refs++;
#ifdef CONFIG_TLS
    if (c->tls) {
        uint8_t pt[16384];
        int n;

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
        while ((n = dyn_tls_read(c->tls, pt, sizeof pt)) > 0)
            tcp_deliver(c, pt, (unsigned)n);
        if (n < 0) {
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

static int tcp_conn_start(dyn_tcp_t* t, dyn_tcp_conn_t* c)
{
    JSContext* ctx = t->ctx;
    c->jsobj = JS_NewObjectClass(ctx, (int)dyn_tcp_conn_class_id);
    if (JS_IsException(c->jsobj)) {
        c->jsobj = JS_UNDEFINED;
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
    return dyn_aio_recv(t->aio, c->fd, 0, 1, tcp_on_recv, c);
}

static void tcp_sweep(void* udata)
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
            t->in_cb = 1;
            JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 2, a);
            t->in_cb = 0;
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
        tcp_release_pending(JS_GetRuntime(ctx), t);
        return;
    }
    if (!t->idle_ms)
        return;
    for (c = t->conns; c; c = next) {
        next = c->lnext;
        if (now - c->last_ms >= t->idle_ms) {
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
    if (JS_IsFunction(t->ctx, t->h_connect)) {
        JSContext* ctx = t->ctx;
        JSValueConst a[1] = { c->jsobj };
        t->in_cb = 1;
        JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 1, a);
        t->in_cb = 0;
        int gone = tcp_gone(t);
        dyn_net_handler_result(ctx, r, NET_HK_TCP_CONNECT);
        if (gone)
            return;
    }
}

static void tcp_release_pending(JSRuntime* rt, dyn_tcp_t* t)
{
    JSValue v = t->self_pending;
    t->self_pending = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
}

static void tcp_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_tcp_conn_t* c = (dyn_tcp_conn_t*)ud;
    dyn_tcp_t* t;
    JSContext* ctx;
    (void)aio;
    (void)buf;
    (void)len;

    if (!c->owner) {
        free(c);
        return;
    }
    t = c->owner;
    t->pending_conn = NULL;
    ctx = t->ctx;

    t->connect_deadline_ms = 0;
    if (res < 0) {
        dyn_aio_t* a = t->aio;
        int fd = c->fd;
        c->closed = 1;
        dyn_aio_close(a, fd);
        tcp_conn_drop(JS_GetRuntime(ctx), c);
        tcp_report_connect(t, JS_UNDEFINED, strerror(-res));
        return;
    }
    if (tcp_conn_start(t, c) < 0) {
        tcp_release_pending(JS_GetRuntime(ctx), t);
        return;
    }
#ifdef CONFIG_TLS
    if (t->tls_ctx) {
        char err[192];
        c->tls = dyn_tls_conn_new(t->tls_ctx, t->tls_servername,
            err, sizeof err);
        if (!c->tls) {
            tls_fail(c, err);
            return;
        }
        if (dyn_tls_handshake(c->tls) < 0 || tls_flush(c) < 0) {
            tls_fail(c, "TLS handshake failed to start");
            return;
        }
        return;
    }
#endif
    tcp_report_connect(t, c->jsobj, NULL);
}

static int eb_resolve(const char* host, uint16_t port,
    char out[2][INET6_ADDRSTRLEN])
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        if (out[0][0] == 0 && ai->ai_family == AF_INET) {
            const struct sockaddr_in* sa = (const struct sockaddr_in*)ai->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, out[0], INET6_ADDRSTRLEN);
        } else if (out[1][0] == 0 && ai->ai_family == AF_INET6) {
            const struct sockaddr_in6* sa = (const struct sockaddr_in6*)ai->ai_addr;
            inet_ntop(AF_INET6, &sa->sin6_addr, out[1], INET6_ADDRSTRLEN);
        }
    }
    freeaddrinfo(res);
    return 0;
}

static void eb_fail(dyn_tcp_t* t, const char* msg)
{
    JSContext* ctx = t->ctx;
    dyn_aio_t* aio = t->aio;
    dyn_tcp_conn_t* c = t->pending_conn;
    int i;

    t->pending_conn = NULL;
    t->eb_done = 1;
    t->eb_deadline_ms = 0;
    for (i = 0; i < 2; i++)
        if (t->eb[i].fd >= 0) {
            dyn_aio_close(aio, t->eb[i].fd);
            t->eb[i].fd = -1;
        }
    if (c && !c->closed) {
        c->closed = 1;
        if (c->fd >= 0)
            dyn_aio_close(aio, c->fd);
        c->fd = -1;
        tcp_conn_drop(JS_GetRuntime(ctx), c);
    }
    tcp_report_connect(t, JS_UNDEFINED, msg);
}

static void eb_sweep(void* udata)
{
    dyn_tcp_t* t = (dyn_tcp_t*)udata;
    if (t->eb_done || t->eb_deadline_ms == 0)
        return;
    if (dyn_timer_now_ms() < t->eb_deadline_ms)
        return;
    eb_fail(t, "connectHappy: timed out");
}

static void eb_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    eb_attempt_t* att = (eb_attempt_t*)ud;
    dyn_tcp_t* t = att->owner;
    int fd = att->fd;
    int idx = att->idx;
    (void)buf;
    (void)len;

    att->fd = -1;
    if (t->eb_done) {
        if (fd >= 0)
            dyn_aio_close(aio, fd);
        return;
    }
    if (res != 0) {
        char msg[192];
        if (fd >= 0)
            dyn_aio_close(aio, fd);
        t->eb_err = -res;
        if (t->eb[1 - idx].fd >= 0)
            return;
        snprintf(msg, sizeof(msg), "connectHappy: connect failed: %s",
            strerror(t->eb_err));
        eb_fail(t, msg);
        return;
    }
    t->eb_done = 1;
    t->eb_deadline_ms = 0;
    if (t->eb[1 - idx].fd >= 0) {
        dyn_aio_close(aio, t->eb[1 - idx].fd);
        t->eb[1 - idx].fd = -1;
    }
    if (t->pending_conn) {
        t->pending_conn->fd = fd;
        tcp_on_connect(aio, 0, NULL, 0, t->pending_conn);
    } else if (fd >= 0) {
        dyn_aio_close(aio, fd);
    }
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

static void dyn_tcp_teardown(dyn_tcp_t* t);

static void dyn_tcp_dispose(void* native)
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

static void dyn_tcp_teardown(dyn_tcp_t* t)
{
    if (!t)
        return;
    if (t->rt)
        tcp_release_pending(t->rt, t);
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
        unlink(t->path);
        free(t->path);
    }
    JS_FreeValueRT(t->rt, t->h_connect);
    JS_FreeValueRT(t->rt, t->h_data);
    JS_FreeValueRT(t->rt, t->h_close);
    JS_FreeValueRT(t->rt, t->h_drain);
#ifdef CONFIG_TLS
    dyn_tls_ctx_free(t->tls_ctx);
    free(t->tls_servername);
#endif
    free(t);
}

static dyn_tcp_t* tcp_new(JSContext* ctx)
{
    dyn_tcp_t* t = (dyn_tcp_t*)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->ctx = ctx;
    t->rt = JS_GetRuntime(ctx);
    t->listen_fd = -1;
    t->h_connect = t->h_data = t->h_close = t->h_drain = JS_UNDEFINED;
    t->hwm = TCP_HWM_DEFAULT;
    t->self_pending = JS_UNDEFINED;
    t->eb[0].owner = t;
    t->eb[0].idx = 0;
    t->eb[0].fd = -1;
    t->eb[1].owner = t;
    t->eb[1].idx = 1;
    t->eb[1].fd = -1;
    t->aio = dyn_net_reactor_acquire(ctx);
    if (!t->aio) {
        free(t);
        return NULL;
    }
    return t;
}

static int tcp_arm_sweep(dyn_tcp_t* t)
{
    if (t->hooked || (!t->idle_ms && !t->connect_deadline_ms))
        return 0;
    if (dyn_net_on_drain(tcp_sweep, t) < 0)
        return -1;
    t->hooked = 1;
    return 0;
}

static int tcp_read_bounds(JSContext* ctx, dyn_tcp_t* t, JSValueConst o,
    int is_client)
{
    JSValue v;
    int64_t n;
    if (!JS_IsObject(o))
        return 0;
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

static const char* tcp_unix_path(JSContext* ctx, JSValueConst v)
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

static int tcp_collect_handlers(JSContext* ctx, JSValueConst h, JSValue out[4])
{
    int i;
    static const char* const names[4] = { "connect", "data", "close", "drain" };

    for (i = 0; i < 4; i++)
        out[i] = JS_UNDEFINED;
    if (!JS_IsObject(h))
        return 0;
    if (dyn_opts_strict(ctx, h, tcp_handler_keys, 4))
        return -1;
    for (i = 0; i < 4; i++) {
        out[i] = JS_GetPropertyStr(ctx, h, names[i]);
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

static void tcp_install_handlers(dyn_tcp_t* t, JSValue v[4])
{
    JSValue old[4];
    int i;
    old[0] = t->h_connect;
    old[1] = t->h_data;
    old[2] = t->h_close;
    old[3] = t->h_drain;
    t->h_connect = v[0];
    t->h_data = v[1];
    t->h_close = v[2];
    t->h_drain = v[3];
    for (i = 0; i < 4; i++)
        v[i] = JS_UNDEFINED;
    {
        JSRuntime* rt = JS_GetRuntime(t->ctx);
        for (i = 0; i < 4; i++)
            JS_FreeValueRT(rt, old[i]);
    }
}

static int tcp_set_handlers(JSContext* ctx, dyn_tcp_t* t, JSValueConst h)
{
    JSValue v[4];
    if (tcp_collect_handlers(ctx, h, v) < 0)
        return -1;
    tcp_install_handlers(t, v);
    return 0;
}

static JSValue dyn_tcp_server_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    int64_t port = 0;
    const char* upath = NULL;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        if (dyn_opts_strict(ctx, argv[0], tcp_srv_keys, 6))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "path");
        if (JS_IsUndefined(v)) {
            upath = NULL;
            JS_FreeValue(ctx, v);
        } else {
            upath = tcp_unix_path(ctx, v);
            JS_FreeValue(ctx, v);
            if (!upath)
                return JS_EXCEPTION;
        }
    }
    if (port < 0 || port > 65535)
        return JS_ThrowRangeError(ctx, "TCPServer: port must be 0..65535");
    t = tcp_new(ctx);
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    t->port = (uint16_t)port;
    if (tcp_read_bounds(ctx, t, argv[0], 0) < 0) {
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (tcp_setup_tls_server(ctx, t, argv[0]) < 0) {
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    {
        JSValue tv = JS_GetPropertyStr(ctx, argv[0], "tls");
        int asked = JS_IsObject(tv);
        JS_FreeValue(ctx, tv);
        if (asked) {
            dyn_tcp_dispose(t);
            return JS_ThrowTypeError(ctx,
                "TCPServer: this build has no TLS support; "
                "rebuild with CONFIG_TLS=y");
        }
    }
#endif
    if (upath) {
        t->path = strdup(upath);
        JS_FreeCString(ctx, upath);
        if (!t->path) {
            dyn_tcp_dispose(t);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
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
        JSValue hv[4];
        if (tcp_collect_handlers(ctx, argv[0], hv) < 0)
            return JS_EXCEPTION;
        t = (dyn_tcp_t*)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
        if (!t) {
            int i;
            for (i = 0; i < 4; i++)
                JS_FreeValue(ctx, hv[i]);
            return JS_EXCEPTION;
        }
        tcp_install_handlers(t, hv);
    }
    t->listen_fd = t->path ? dyn_aio_unix_listen(t->aio, t->path, 128)
                           : dyn_aio_listen(t->aio, "0.0.0.0", t->port, 512);
    if (t->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "%s: listen failed",
            t->path ? "IpcServer" : "TCPServer");
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
        return JS_UNDEFINED;
    }
    if (getsockname(t->listen_fd, (struct sockaddr*)&sa, &sl) == 0)
        t->port = ntohs(sa.sin_port);
    dyn_aio_accept(t->aio, t->listen_fd, tcp_on_accept, t);
    t->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_tcp_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_tcp_t* t = (dyn_tcp_t*)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    if (!t)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, t->port);
}

static JSValue dyn_tcp_connect_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    dyn_tcp_conn_t* c;
    const char* host = NULL;
    int64_t port = 0;
    int fd;
    JSValue v, res;
    (void)this_val;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "connect(options, handlers) needs options");
    if (dyn_opts_strict(ctx, argv[0], tcp_cli_keys, 8))
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[0], "path");
    if (!JS_IsUndefined(v)) {
        const char* up = tcp_unix_path(ctx, v);
        JS_FreeValue(ctx, v);
        if (!up)
            return JS_EXCEPTION;
        t = tcp_new(ctx);
        if (!t) {
            JS_FreeCString(ctx, up);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
        c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
        if (!c) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_ThrowOutOfMemory(ctx);
        }
        c->owner = t;
        c->refs = 1;
        c->jsobj = JS_UNDEFINED;
        c->hwm = t->hwm;
        fd = dyn_aio_unix_connect(t->aio, up, tcp_on_connect, c);
        JS_FreeCString(ctx, up);
        if (fd < 0) {
            free(c);
            dyn_tcp_dispose(t);
            return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
        }
        c->fd = fd;
        t->started = 1;
        {
            JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t,
                dyn_tcp_dispose);
            if (!JS_IsException(obj))
                t->self_pending = JS_DupValue(ctx, obj);
            return obj;
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "host");
    host = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "port");
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
        JS_FreeValue(ctx, v);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (port < 1 || port > 65535) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connect: port must be 1..65535");
    }

    t = tcp_new(ctx);
    if (!t) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (tcp_read_bounds(ctx, t, argv[0], 1) < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (tcp_setup_tls(ctx, t, argv[0], host) < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    {
        JSValue tv = JS_GetPropertyStr(ctx, argv[0], "tls");
        int asked = !JS_IsUndefined(tv) && !JS_IsNull(tv) && !(JS_IsBool(tv) && !JS_ToBool(ctx, tv));
        JS_FreeValue(ctx, tv);
        if (asked) {
            if (host)
                JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return JS_ThrowTypeError(ctx,
                "connect: this build has no TLS support; "
                "rebuild with CONFIG_TLS=y");
        }
    }
#endif
    if (argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) {
        if (host)
            JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }

    c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_tcp_dispose(t);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;

    fd = dyn_aio_connect(t->aio, host ? host : "127.0.0.1", (uint16_t)port,
        tcp_on_connect, c);
    if (host)
        JS_FreeCString(ctx, host);
    if (fd < 0) {
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
    }
    c->fd = fd;
    t->pending_conn = c;
    t->started = 1;
    if (tcp_arm_sweep(t) < 0) {
        dyn_aio_close(t->aio, fd);
        c->fd = -1;
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx,
            "connect: the backend cannot arm a clock, so connectTimeoutMs "
            "would never fire");
    }
    res = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t, dyn_tcp_dispose);
    if (!JS_IsException(res))
        t->self_pending = JS_DupValue(ctx, res);
    return res;
}

static JSValue dyn_tcp_connect_happy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tcp_t* t;
    dyn_tcp_conn_t* c;
    const char* host;
    int64_t port, fallback = 250;
    char ips[2][INET6_ADDRSTRLEN];
    JSValue v, res;
    int fd, n_armed = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "connectHappy(host, port, opts, handlers)");
    host = JS_ToCString(ctx, argv[0]);
    if (!host)
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &port, argv[1])) {
        JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    if (port < 1 || port > 65535) {
        JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connectHappy: port must be 1..65535");
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], tcp_eb_keys, 2)) {
            JS_FreeCString(ctx, host);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "fallbackMs");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &fallback, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, host);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (fallback < 1) {
        JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connectHappy: fallbackMs must be >= 1");
    }
    memset(ips, 0, sizeof(ips));
    if (eb_resolve(host, (uint16_t)port, ips) < 0) {
        JS_FreeCString(ctx, host);
        return JS_ThrowInternalError(ctx, "connectHappy: DNS resolution failed");
    }
    if (!ips[0][0] && !ips[1][0]) {
        JS_FreeCString(ctx, host);
        return JS_ThrowInternalError(ctx, "connectHappy: no usable addresses");
    }

    t = tcp_new(ctx);
    if (!t) {
        JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (argc > 3 && JS_IsObject(argv[3])) {
        if (tcp_set_handlers(ctx, t, argv[3]) < 0) {
            JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
    } else if (argc > 2 && JS_IsObject(argv[2])) {
        if (tcp_set_handlers(ctx, t, argv[2]) < 0) {
            JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
    }
#ifdef CONFIG_TLS
    if (argc > 2 && JS_IsObject(argv[2]) && tcp_setup_tls(ctx, t, argv[2], host) < 0) {
        JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue tv = JS_GetPropertyStr(ctx, argv[2], "tls");
        int asked = !JS_IsUndefined(tv) && !JS_IsNull(tv) && !(JS_IsBool(tv) && !JS_ToBool(ctx, tv));
        JS_FreeValue(ctx, tv);
        if (asked) {
            JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return JS_ThrowTypeError(ctx,
                "connectHappy: this build has no TLS support; "
                "rebuild with CONFIG_TLS=y");
        }
    }
#endif
    JS_FreeCString(ctx, host);

    c = (dyn_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_tcp_dispose(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;
    c->fd = -1;
    t->pending_conn = c;
    t->started = 1;
    t->eb_deadline_ms = dyn_timer_now_ms() + (uint64_t)fallback;

    if (ips[0][0]) {
        fd = dyn_aio_connect(t->aio, ips[0], (uint16_t)port,
            eb_on_connect, &t->eb[0]);
        if (fd >= 0) {
            t->eb[0].fd = fd;
            n_armed++;
        }
    }
    if (ips[1][0]) {
        fd = dyn_aio_connect(t->aio, ips[1], (uint16_t)port,
            eb_on_connect, &t->eb[1]);
        if (fd >= 0) {
            t->eb[1].fd = fd;
            n_armed++;
        }
    }
    if (n_armed == 0) {
        t->pending_conn = NULL;
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connectHappy: connect failed");
    }
    if (dyn_net_on_drain(eb_sweep, t) < 0) {
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx,
            "connectHappy: the backend cannot arm a clock, so fallbackMs "
            "would never fire");
    }
    t->hooked = 1;
    res = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_tcp_class_id, t, dyn_tcp_dispose);
    if (!JS_IsException(res))
        t->self_pending = JS_DupValue(ctx, res);
    return res;
}

static const JSCFunctionListEntry dyn_tcp_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_tcp_start),
    JS_CGETSET_DEF("port", dyn_tcp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_statics[] = {
    JS_CFUNC_DEF("connect", 1, dyn_tcp_connect_method),
};

typedef struct dyn_udp {
    JSValue h_message;
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    int started;
    int dead;
    uint16_t port;
} dyn_udp_t;

_Static_assert(sizeof(dyn_udp_t) <= 56,
    "dyn_udp_t regained padding: reorder largest-first");

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

static JSClassID dyn_udp_class_id;
static void dyn_udp_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_udp_class_id);
    dyn_udp_t* u = (r && !r->closed) ? (dyn_udp_t*)r->native : NULL;

    if (!u)
        return;
    JS_MarkValue(rt, u->h_message, mark_func);
}

static const JSClassDef dyn_udp_class = {
    "UDPSocket",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_udp_gc_mark,
};

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

static JSValue dyn_udp_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_udp_t* u;
    int64_t port = 0;
    const char* host = NULL;
    struct sockaddr_in sa;
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
        return JS_ThrowOutOfMemory(ctx);
    }
    u->fd = dyn_aio_udp_bind(u->aio, host, (uint16_t)port);
    if (host)
        JS_FreeCString(ctx, host);
    if (u->fd < 0) {
        dyn_net_reactor_release(ctx);
        dyn_udp_shell_release(u);
        return JS_ThrowInternalError(ctx, "UDPSocket: bind failed");
    }
    if (getsockname(u->fd, (struct sockaddr*)&sa, &sl) == 0)
        u->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, new_target, dyn_udp_class_id, u, dyn_udp_dispose);
}

static JSValue dyn_udp_start(JSContext* ctx, JSValueConst this_val, int argc,
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
    return JS_UNDEFINED;
}

static JSValue dyn_udp_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_udp_t* u;
    struct sockaddr_in sa;
    const char *host = NULL, *str = NULL;
    int64_t port = 0;
    size_t len = 0;
    const uint8_t* p;
    int n;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "send(data, host, port)");
    p = JS_GetArrayBuffer(ctx, &len, argv[0]);
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
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        JS_FreeCString(ctx, host);
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, "send: host must be an IPv4 address");
    }
    JS_FreeCString(ctx, host);
    n = dyn_aio_sendto(u->aio, u->fd, p, len, (struct sockaddr*)&sa, sizeof(sa));
    if (str)
        JS_FreeCString(ctx, str);
    if (n < 0)
        return JS_ThrowInternalError(ctx, "UDPSocket: send failed");
    return JS_NewInt32(ctx, n);
}

static JSValue dyn_udp_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_udp_t* u = (dyn_udp_t*)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, u->port);
}

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
    return JS_SetModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
}

void dyn_tcp_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "TCPServer");
    JS_AddModuleExport(ctx, m, "UDPSocket");
    JS_AddModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
}

#endif
