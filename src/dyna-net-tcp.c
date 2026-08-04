/*
 * TCP client and server, part of dyna:net.
 *
 * Both run ENTIRELY on the JS thread via the dyn_aio reactor, like App: a
 * handler sees a zero-copy view of the received bytes, valid only for the
 * duration of the call. Nothing here is offloaded to the pool -- the reactor
 * already knows when a socket is ready, and handing a ready socket to another
 * thread costs more than the read (NET_PLAN.md 2.1).
 *
 *   const s = new TCPServer({ port: 9000 });
 *   s.start({ data: (c, bytes) => c.write(bytes), close: (c) => {} });
 *
 *   TCPServer.connect({ host: "127.0.0.1", port: 9000 },
 *                     { connect: (c, err) => c.write("hi"), data: ... });
 */
#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"   /* dyn_timer_now_ms: the sweep clock */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dyna-tls.h"      /* no-op without CONFIG_TLS */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* from dyna-libc.h: fold a reactor into the JS event loop */
void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata);

typedef struct dyn_tcp dyn_tcp_t;

/* One connection. `refs` mirrors App's discipline: the live socket holds one,
 * and the JS object handed to a handler holds another, so a handler that
 * retains its `conn` cannot outlive the struct. */
typedef struct dyn_tcp_conn {
    dyn_tcp_t *owner;
    int fd;
    int closed;
    int refs;
    JSValue jsobj;        /* the TCPConn given to handlers; JS_UNDEFINED if none */
    /* Intrusive list of a server's live connections, so the idle sweep is
     * O(live) rather than a scan of anything. */
    struct dyn_tcp_conn *lnext, *lprev;
    uint64_t last_ms;     /* stamped on PROGRESS -- see tcp_sweep */
#ifdef CONFIG_TLS
    /* NULL = plaintext. While this is set and the handshake is unfinished the
       socket carries ciphertext only and no `data` handler has run. */
    dyn_tls_conn_t *tls;
    int tls_up;           /* handshake finished and `connect` already fired */
#endif
} dyn_tcp_conn_t;

struct dyn_tcp {
    JSContext *ctx;
    dyn_aio_t *aio;
    int listen_fd;        /* -1 for a client */
    uint16_t port;
    int started;
    char *path;           /* AF_UNIX endpoint; NULL for TCP */
    JSValue h_connect, h_data, h_close;   /* handlers (dup'd) */
    /* A connect in flight holds the wrapper object ITSELF. Without it,
       `TCPServer.connect(opts, handlers)` with the return value discarded is
       collected mid-connect: the callbacks never fire and the reactor keeps an
       fd whose handler is gone, so the loop stops progressing and an unrelated
       setTimeout never runs either. Released in tcp_on_connect on BOTH paths,
       so the cycle never outlives the operation -- a permanent self-reference
       would be uncollectable. */
    JSValue self_pending;

    /* ---- bounds. A peer that opens connections and holds them silently
       costs a descriptor, a calloc and a JS object each, and before this
       nothing capped the count or reclaimed an idle one (CWE-400). App has
       had idleTimeoutMs since the slowloris fix; this is the same shape. */
    dyn_tcp_conn_t *conns;      /* live connections, for the sweep */
    int nconns;
    int max_conns;              /* 0 = unbounded (the documented default) */
    uint64_t idle_ms;           /* 0 = off */
    uint64_t connect_deadline_ms;  /* client side; 0 = off */
    int hooked;                 /* a drain hook is armed for this server */
    uint64_t n_refused;         /* connections dropped by max_conns */
    uint64_t n_idle_closed;     /* connections closed by the idle sweep */
#ifdef CONFIG_TLS
    dyn_tls_ctx_t *tls_ctx;     /* NULL = plaintext */
    char *tls_servername;       /* drives BOTH SNI and hostname verification */
#endif
};

static JSClassID dyn_tcp_class_id;
static JSClassID dyn_tcp_conn_class_id;

/* Framework-owned resource (has close()/closed), unlike TCPConn which is a
 * plain object handed to handlers. */
static const JSClassDef dyn_tcp_class = {
    "TCPServer", .finalizer = dyn_res_finalizer,
};

/* ---- connection ------------------------------------------------------- */

static void tcp_conn_unref(JSContext *ctx, dyn_tcp_conn_t *c)
{
    if (--c->refs == 0) {
        JS_FreeValue(ctx, c->jsobj);
#ifdef CONFIG_TLS
        dyn_tls_conn_free(c->tls);   /* NULL-safe */
#endif
        free(c);
    }
}

static void dyn_tcp_conn_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_tcp_conn_t *c = JS_GetOpaque(val, dyn_tcp_conn_class_id);
    (void)rt;
    if (c) {
        c->jsobj = JS_UNDEFINED;   /* the object is going away, not the struct */
        if (--c->refs == 0)
            free(c);
    }
}

/* The struct holds a strong ref to the very object whose opaque points back at
 * it -- a cycle no refcount can break. gc_mark is what lets the cycle collector
 * see that edge and reclaim the pair; without it the objects survive to
 * JS_FreeRuntime and trip its gc_obj_list assertion. dyna-nat.h states the rule:
 * a native holding JSValues supplies a gc_mark. */
static void dyn_tcp_conn_gc_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    dyn_tcp_conn_t *c = JS_GetOpaque(val, dyn_tcp_conn_class_id);
    if (c)
        JS_MarkValue(rt, c->jsobj, mark_func);
}

static const JSClassDef dyn_tcp_conn_class = {
    "TCPConn",
    .finalizer = dyn_tcp_conn_finalizer,
    .gc_mark = dyn_tcp_conn_gc_mark,
};

static void tcp_release_pending(JSContext *ctx, dyn_tcp_t *t);
#ifdef CONFIG_TLS
static int tls_flush(dyn_tcp_conn_t *c);   /* used by write, defined below */
#endif

static void tcp_list_unlink(dyn_tcp_conn_t *c)
{
    dyn_tcp_t *t = c->owner;
    if (c->lprev) c->lprev->lnext = c->lnext;
    else if (t->conns == c) t->conns = c->lnext;
    if (c->lnext) c->lnext->lprev = c->lprev;
    c->lnext = c->lprev = NULL;
    if (t->nconns > 0) t->nconns--;
}

static void tcp_conn_close(dyn_tcp_conn_t *c)
{
    JSContext *ctx;
    dyn_aio_t *aio;
    JSValue h;
    int fd;

    if (c->closed)
        return;
    /* READ EVERYTHING FROM THE OWNER FIRST, and hold a reference to the
       handler: `close(){ server.close() }` frees the owner from inside this
       call, and the old code then read c->owner->aio. ASan: heap-use-after-free
       here, freed by dyn_res_release. The handler value must be dup'd too --
       dispose frees it, leaving a dangling JSValue we were about to call. */
    ctx = c->owner->ctx;
    aio = c->owner->aio;
    fd  = c->fd;
    h   = JS_DupValue(ctx, c->owner->h_close);

    c->closed = 1;               /* before dyn_aio_close: completing an
                                    in-flight recv re-enters here */
    tcp_list_unlink(c);
    dyn_aio_close(aio, fd);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
        JSValueConst a[1] = { c->jsobj };
        JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, h);
    tcp_conn_unref(ctx, c);      /* drop the socket's own ref */
}

static JSValue dyn_tcp_conn_write(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_tcp_conn_t *c;
    size_t len = 0, off = 0, bpe = 0;
    const uint8_t *p = NULL;
    const char *str = NULL;
    JSValue ab;
    int rc;

    /* Coerce FIRST, resolve the connection after: a toString hook runs
     * arbitrary JS and can close this socket underneath us (Appendix A). */
    ab = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (!JS_IsException(ab)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab);
        if (!base) { JS_FreeValue(ctx, ab); return JS_EXCEPTION; }
        p = base + off;                 /* a VIEW, not the whole buffer */
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        p = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (!p) {
            ab = JS_UNDEFINED;
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
            p = (const uint8_t *)str;
        } else {
            ab = JS_DupValue(ctx, argv[0]);
        }
    }

    c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    if (!c || c->closed) {
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "TCPConn: connection is closed");
    }
#ifdef CONFIG_TLS
    if (c->tls) {
        /* REFUSE rather than queue: a write before the handshake would have
           to be buffered somewhere with no bound, and the caller has a
           `connect` handler that fires exactly when writing becomes legal. */
        if (!c->tls_up) {
            if (str) JS_FreeCString(ctx, str);
            JS_FreeValue(ctx, ab);
            return JS_ThrowTypeError(ctx,
                "TCPConn: write before the TLS handshake completed "
                "-- write from the connect handler");
        }
        /* LOOP: dyn_tls_write clamps to an int, so a large buffer is a
           partial write and treating it as success drops the tail. */
        size_t off = 0;
        rc = 0;
        while (off < (size_t)len) {
            int w = dyn_tls_write(c->tls, p + off, (size_t)len - off);
            if (w <= 0) { rc = -1; break; }
            off += (size_t)w;
            if (tls_flush(c) != 0) { rc = -1; break; }
        }
    } else
#endif
    /* No copy: dyn_aio_send copies whatever it cannot put out inline, and
     * argv[0] roots these bytes for the whole call. */
    rc = dyn_aio_send(c->owner->aio, c->fd, p, len, 0, NULL, NULL);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, ab);
    if (rc < 0)
        return JS_ThrowInternalError(ctx, "TCPConn: send failed");
    return JS_UNDEFINED;
}

static JSValue dyn_tcp_conn_close_method(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    dyn_tcp_conn_t *c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)ctx; (void)argc; (void)argv;
    if (c && !c->closed)
        tcp_conn_close(c);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_tcp_conn_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_tcp_conn_write),
    JS_CFUNC_DEF("close", 0, dyn_tcp_conn_close_method),
};

/* Report a connect outcome -- plaintext or TLS, success or failure -- through
   ONE handler, with the error in the second argument, so a caller cannot
   forget the failure path exists.

   NOTHING may read `t` after the callback: a handler calling conn.close()
   disposes the native struct immediately and holding a JS reference does not
   stop it (ASan found this as a use-after-free on t->self_pending). So the
   handler and the pending self-reference are taken NOW, and the latter is
   released from the local afterwards -- which keeps the wrapper alive across
   the call exactly as the old ordering did. */
static void tcp_report_connect(dyn_tcp_t *t, JSValueConst obj, const char *err)
{
    JSContext *ctx = t->ctx;
    JSValue h = JS_DupValue(ctx, t->h_connect);
    JSValue pend = t->self_pending;
    t->self_pending = JS_UNDEFINED;

    if (JS_IsFunction(ctx, h)) {
        JSValueConst a[2];
        JSValue r, msg = err ? JS_NewString(ctx, err) : JS_NULL;
        a[0] = err ? JS_NULL : obj;
        a[1] = msg;
        r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, msg);
    }
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, pend);
}

/* ---- TLS ---------------------------------------------------------------
 * The engine is a state machine over two buffers, so the whole integration is
 * three moves: flush what it queued, feed what arrived, drain what it decoded.
 * Nothing here blocks. */
#ifdef CONFIG_TLS
/* A handshake that never completed is NOT a connection that closed: report it
   through `connect(null, err)` like any other connect failure and do not fire
   `close`, because the caller never received a usable conn. Everything the
   owner provides is read BEFORE the report, and the report is the LAST
   statement -- a handler calling conn.close() disposes the owner. Copy the
   message too: it lives in the engine, which the teardown frees. */
static void tls_fail(dyn_tcp_conn_t *c, const char *fallback)
{
    dyn_tcp_t *t = c->owner;
    JSContext *ctx = t->ctx;
    dyn_aio_t *a = t->aio;
    const char *e = c->tls ? dyn_tls_error(c->tls) : NULL;
    char msg[192];
    int fd = c->fd;

    snprintf(msg, sizeof msg, "%s", e ? e : fallback);
    if (!c->closed) {
        c->closed = 1;
        tcp_list_unlink(c);
        dyn_aio_close(a, fd);
        tcp_conn_unref(ctx, c);          /* the socket's own ref */
    }
    tcp_report_connect(t, JS_UNDEFINED, msg);
}

/* dyn_aio_send COPIES whatever it cannot put out inline, so a stack buffer is
   safe to hand it. 16 KiB is one TLS record. */
static int tls_flush(dyn_tcp_conn_t *c)
{
    uint8_t out[16384];
    int n;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0) {
        if (dyn_aio_send(c->owner->aio, c->fd, out, (unsigned)n, 0,
                         NULL, NULL) < 0)
            return -1;
    }
    return 0;
}

/* `tls: true` or `tls: { ca, servername, alpn, minVersion,
   rejectUnauthorized }`. Returns 0 -- including when no TLS was asked for --
   or -1 with an exception pending. */
/* Server side: `tls: { cert, key, alpn }`, all PEM paths. Returns 0 including
   "no tls asked for", or -1 with an exception pending. */
static int tcp_setup_tls_server(JSContext *ctx, dyn_tcp_t *t, JSValueConst opts)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    const char *cert = NULL, *key = NULL;
    char *alpn = NULL, err[192];
    JSValue w;
    int rc = -1;

    if (!JS_IsObject(v)) { JS_FreeValue(ctx, v); return 0; }
    w = JS_GetPropertyStr(ctx, v, "cert");
    if (JS_IsString(w)) cert = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "key");
    if (JS_IsString(w)) key = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "alpn");
    if (JS_IsString(w)) {
        const char *sa = JS_ToCString(ctx, w);
        if (sa) { alpn = strdup(sa); JS_FreeCString(ctx, sa); }
    }
    JS_FreeValue(ctx, w);
    if (!cert || !key) {
        JS_ThrowTypeError(ctx, "TCPServer: tls needs both `cert` and `key` "
                               "(PEM paths); there is no self-signed default");
        goto done;
    }
    t->tls_ctx = dyn_tls_ctx_server(cert, key, alpn, err, sizeof err);
    if (!t->tls_ctx) { JS_ThrowInternalError(ctx, "TCPServer: tls: %s", err); goto done; }
    rc = 0;
done:
    if (cert) JS_FreeCString(ctx, cert);
    if (key) JS_FreeCString(ctx, key);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}

static int tcp_setup_tls(JSContext *ctx, dyn_tcp_t *t, JSValueConst opts,
                         const char *host)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    dyn_tls_opts_t o;
    const char *ca = NULL, *sni = NULL, *minv = NULL;
    char *alpn = NULL, err[192];
    int rc = -1;

    if (JS_IsUndefined(v) || JS_IsNull(v) ||
        (JS_IsBool(v) && !JS_ToBool(ctx, v))) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.min_version = 12;
    if (JS_IsObject(v)) {
        JSValue w;
        w = JS_GetPropertyStr(ctx, v, "ca");
        if (JS_IsString(w)) ca = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "servername");
        if (JS_IsString(w)) sni = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "minVersion");
        if (JS_IsString(w)) minv = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        /* Insecure is opt-OUT, written in full, and never a default. */
        w = JS_GetPropertyStr(ctx, v, "rejectUnauthorized");
        if (JS_IsBool(w) && !JS_ToBool(ctx, w)) o.insecure = 1;
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "alpn");
        if (JS_IsString(w)) {
            const char *s = JS_ToCString(ctx, w);
            if (s) { alpn = strdup(s); JS_FreeCString(ctx, s); }
        } else if (JS_IsArray(ctx, w)) {
            JSValue lv = JS_GetPropertyStr(ctx, w, "length");
            uint32_t i, n = 0;
            JS_ToUint32(ctx, &n, lv);
            JS_FreeValue(ctx, lv);
            for (i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, w, i);
                const char *s = JS_ToCString(ctx, e);
                if (s) {
                    size_t have = alpn ? strlen(alpn) : 0;
                    char *nw = (char *)realloc(alpn, have + strlen(s) + 2);
                    if (nw) {
                        alpn = nw;
                        if (have) { alpn[have] = ','; alpn[have + 1] = 0; }
                        else alpn[0] = 0;
                        strcat(alpn, s);
                    }
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, w);
    }
    o.ca_file = ca;
    o.alpn = alpn;
    if (minv && strstr(minv, "1.3"))
        o.min_version = 13;

    /* The name verified against the certificate defaults to the host asked
       for. Verification with no name would leave the certificate bound to
       nothing. */
    t->tls_servername = strdup(sni ? sni : (host ? host : ""));
    t->tls_ctx = dyn_tls_ctx_client(&o, err, sizeof err);
    if (!t->tls_ctx)
        JS_ThrowInternalError(ctx, "tls: %s", err);
    else
        rc = 0;

    if (ca) JS_FreeCString(ctx, ca);
    if (sni) JS_FreeCString(ctx, sni);
    if (minv) JS_FreeCString(ctx, minv);
    free(alpn);
    JS_FreeValue(ctx, v);
    return rc;
}
#endif /* CONFIG_TLS */

/* ---- reactor callbacks ------------------------------------------------ */

/* Hand bytes to the JS `data` handler. Shared by the plaintext and TLS paths
   so the copy rule, the Uint8Array rule and the progress stamp cannot drift
   apart between them. */
static void tcp_deliver(dyn_tcp_conn_t *c, const uint8_t *buf, unsigned len)
{
    JSContext *ctx = c->owner->ctx;
    if (JS_IsFunction(ctx, c->owner->h_data) && !JS_IsUndefined(c->jsobj)) {
        /* A COPY, not a view: the adapter recycles its shared recv buffer as
         * soon as this returns, and a JS handler can retain what it is given.
         * A Uint8Array, not the bare ArrayBuffer: an ArrayBuffer has no
         * `.length` and no indexing, so `b[i]` reads undefined and a caller's
         * loop silently does nothing. */
        JSValue ab = JS_NewArrayBufferCopy(ctx, buf, len);
        if (!JS_IsException(ab)) {
            /* THREE arguments: with one, the view's length defaults to 0 and
             * the handler silently receives an empty array. */
            JSValueConst ta[3] = { ab, JS_NewInt32(ctx, 0),
                                   JS_NewInt32(ctx, (int)len) };
            JSValue u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
            if (!JS_IsException(u8)) {
                JSValueConst a[2] = { c->jsobj, u8 };
                JSValue r = JS_Call(ctx, c->owner->h_data, JS_UNDEFINED, 2, a);
                JS_FreeValue(ctx, r);
                /* Stamp AFTER the handler returned, not on arrival: bytes
                 * that never reach an application are not progress, and a
                 * clock reset in the arrival path is what makes a slowloris
                 * look permanently active. */
                c->last_ms = dyn_timer_now_ms();
            }
            JS_FreeValue(ctx, u8);
        }
        JS_FreeValue(ctx, ab);
    }
}

static void tcp_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                        unsigned len, void *ud)
{
    dyn_tcp_conn_t *c = (dyn_tcp_conn_t *)ud;
    JSContext *ctx = c->owner->ctx;
    (void)aio;

    if (res <= 0) { tcp_conn_close(c); return; }
    c->refs++;                        /* a handler may close the connection */
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
            if (tls_flush(c) < 0) st = -1;   /* flush BEFORE judging: the
                                                engine's reply must go out even
                                                on the last round */
            if (st < 0) {
                tls_fail(c, "TLS handshake failed");
                tcp_conn_unref(ctx, c);   /* our own ref; owner is gone-safe */
                return;
            }
            if (st == 0) { tcp_conn_unref(ctx, c); return; }  /* needs more */
            c->tls_up = 1;
            c->last_ms = dyn_timer_now_ms();
            tcp_report_connect(c->owner, c->jsobj, NULL);
        }
        /* Drain EVERY decoded record: one socket read routinely carries
           several, and stopping at the first strands the rest until more
           ciphertext happens to arrive. */
        while ((n = dyn_tls_read(c->tls, pt, sizeof pt)) > 0)
            tcp_deliver(c, pt, (unsigned)n);
        if (n < 0) {
            tcp_conn_close(c);
            tcp_conn_unref(ctx, c);
            return;
        }
        tls_flush(c);              /* a read can queue a key update or alert */
        tcp_conn_unref(ctx, c);
        return;
    }
#endif
    tcp_deliver(c, buf, len);
    tcp_conn_unref(ctx, c);
}

/* Build the TCPConn JS object and arm the recv. Returns 0, or -1 having closed. */
static int tcp_conn_start(dyn_tcp_t *t, dyn_tcp_conn_t *c)
{
    JSContext *ctx = t->ctx;
    c->jsobj = JS_NewObjectClass(ctx, (int)dyn_tcp_conn_class_id);
    if (JS_IsException(c->jsobj)) {
        c->jsobj = JS_UNDEFINED;
        dyn_aio_close(t->aio, c->fd);
        free(c);
        return -1;
    }
    JS_SetOpaque(c->jsobj, c);
    c->refs++;                        /* the JS object's ref */
    c->last_ms = dyn_timer_now_ms();
    c->lnext = t->conns;
    if (t->conns) t->conns->lprev = c;
    t->conns = c;
    t->nconns++;
    return dyn_aio_recv(t->aio, c->fd, 0, /*multishot=*/1, tcp_on_recv, c);
}

/* Close connections that have made no PROGRESS for idle_ms.
 *
 * "Progress" is deliberately NOT byte arrival: the attack this exists to stop
 * delivers bytes forever without ever completing anything, so a clock reset in
 * the recv callback makes the attacker look permanently active and the defence
 * inert while appearing implemented. TCPServer has no protocol of its own, so
 * progress is "the data handler was invoked and returned" -- the point at
 * which the application above has been given something to act on. Sweeping a
 * connection whose handler is running would be wrong, and cannot happen: the
 * sweep runs on the loop thread between drains. */
static void tcp_sweep(void *udata)
{
    dyn_tcp_t *t = (dyn_tcp_t *)udata;
    dyn_tcp_conn_t *c, *next;
    uint64_t now = dyn_timer_now_ms();

    if (t->connect_deadline_ms && now >= t->connect_deadline_ms) {
        JSContext *ctx = t->ctx;
        t->connect_deadline_ms = 0;
        /* Report to the SAME handler as success, with an error, so a caller
         * cannot forget the failure path exists -- as tcp_on_connect does. */
        if (JS_IsFunction(ctx, t->h_connect)) {
            JSValueConst a[2] = { JS_NULL,
                                  JS_NewString(ctx, "connect timed out") };
            JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 2, a);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, a[1]);
        }
        for (c = t->conns; c; c = next) { next = c->lnext; tcp_conn_close(c); }
        tcp_release_pending(ctx, t);
        return;
    }
    if (!t->idle_ms)
        return;
    for (c = t->conns; c; c = next) {
        next = c->lnext;
        if (now - c->last_ms >= t->idle_ms) {
            t->n_idle_closed++;
            tcp_conn_close(c);
        }
    }
}

static void tcp_on_accept(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    dyn_tcp_t *t = (dyn_tcp_t *)ud;
    dyn_tcp_conn_t *c;
    (void)buf; (void)len;

    if (res < 0)
        return;
    /* Refuse BEFORE allocating: the point of the cap is that a peer cannot
     * make the process spend a descriptor, a calloc and a JS object per
     * connection. Closing the accepted fd is the refusal -- there is no
     * protocol here to send a "busy" in. */
    if (t->max_conns && t->nconns >= t->max_conns) {
        t->n_refused++;
        dyn_aio_close(aio, res);
        return;
    }
    c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_aio_close(aio, res); return; }
    c->owner = t;
    c->fd = res;
    c->refs = 1;                      /* the live socket */
#ifdef CONFIG_TLS
    if (t->tls_ctx) {
        /* Server side: the engine goes on immediately and `data` cannot fire
           until the handshake completes, exactly as on the client. A peer that
           never finishes it is closed by the idle sweep like any other. */
        char terr[192];
        c->tls = dyn_tls_conn_accept(t->tls_ctx, terr, sizeof terr);
        if (!c->tls) { free(c); dyn_aio_close(aio, res); return; }
    }
#endif
    c->jsobj = JS_UNDEFINED;
    if (tcp_conn_start(t, c) < 0)
        return;
    if (JS_IsFunction(t->ctx, t->h_connect)) {
        JSValueConst a[1] = { c->jsobj };
        JSValue r = JS_Call(t->ctx, t->h_connect, JS_UNDEFINED, 1, a);
        JS_FreeValue(t->ctx, r);
    }
}

/* Drop the in-flight connect's self-reference exactly once. */
static void tcp_release_pending(JSContext *ctx, dyn_tcp_t *t)
{
    JSValue v = t->self_pending;
    t->self_pending = JS_UNDEFINED;   /* clear FIRST: the free can collect */
    JS_FreeValue(ctx, v);
}

static void tcp_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, void *ud)
{
    dyn_tcp_conn_t *c = (dyn_tcp_conn_t *)ud;
    dyn_tcp_t *t = c->owner;
    JSContext *ctx = t->ctx;
    (void)aio; (void)buf; (void)len;

    /* Disarm FIRST and on every path: a deadline left armed past completion
     * makes the sweep tear down a healthy connection some time later. */
    t->connect_deadline_ms = 0;
    if (res < 0) {
        /* Tear the socket down BEFORE reporting: the handler may close the
           server, after which neither t->aio nor c may be touched. */
        dyn_aio_t *a = t->aio;
        int fd = c->fd;
        c->closed = 1;
        dyn_aio_close(a, fd);
        tcp_conn_unref(ctx, c);
        tcp_report_connect(t, JS_UNDEFINED, strerror(-res));
        return;
    }
    if (tcp_conn_start(t, c) < 0) {
        tcp_release_pending(ctx, t);
        return;
    }
#ifdef CONFIG_TLS
    if (t->tls_ctx) {
        /* The TCP connect is only half of it. Build the engine, put the
           ClientHello on the wire, and report NOTHING yet: `connect` fires
           from tcp_on_recv when the handshake completes, and the pending
           self-reference stays held until then. */
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

/* ---- TCPServer -------------------------------------------------------- */

/* Detach every live connection from a server being freed. The reactor is
   SHARED, so a connection left armed delivers its next event into a freed
   dyn_tcp_t -- and conn.write() reaches the same freed pointer from JS. */
static void tcp_detach_conns(dyn_tcp_t *t)
{
    JSContext *ctx = t->ctx;
    dyn_tcp_conn_t *c = t->conns, *next;

    t->conns = NULL;
    t->nconns = 0;
    while (c) {
        next = c->lnext;                 /* read first: the unref can free c */
        c->lnext = c->lprev = NULL;
        if (!c->closed) {
            c->closed = 1;               /* the guard every owner read sits behind */
            if (t->aio && c->fd >= 0)
                dyn_aio_close(t->aio, c->fd);
            c->fd = -1;
            c->owner = NULL;
            tcp_conn_unref(ctx, c);      /* the live socket's own ref */
        } else {
            c->owner = NULL;
        }
        c = next;
    }
}

static void dyn_tcp_dispose(void *native)
{
    dyn_tcp_t *t = (dyn_tcp_t *)native;
    if (!t)
        return;
    /* An explicit close() while a connect is still in flight: the self-
       reference would otherwise keep the object -- and the loop -- alive for
       ever, since a connect to a black hole never completes. Freeing
       JS_UNDEFINED (the settled case) is a no-op. */
    if (t->ctx)
        tcp_release_pending(t->ctx, t);
    if (t->hooked) {
        dyn_net_off_drain(t);   /* keyed by udata; must go before the free */
        t->hooked = 0;
    }
    /* BEFORE the reactor release: detaching needs t->aio, and a connection left
       armed on the shared reactor is a use-after-free on its next event. */
    tcp_detach_conns(t);
    if (t->aio) {
        if (t->listen_fd >= 0)
            dyn_aio_close(t->aio, t->listen_fd);
        dyn_net_reactor_release(t->ctx);   /* shared: only the last user frees */
    }
    /* Unlink OUR endpoint on teardown: a socket file left behind makes the
     * next bind fail, and the stale-file unlink at bind time is a hijack
     * window we should not widen by leaving litter. */
    if (t->path) {
        unlink(t->path);
        free(t->path);
    }
    JS_FreeValue(t->ctx, t->h_connect);
    JS_FreeValue(t->ctx, t->h_data);
    JS_FreeValue(t->ctx, t->h_close);
#ifdef CONFIG_TLS
    /* AFTER the connections: each dyn_tls_conn_t was made from this SSL_CTX
       and OpenSSL refcounts it, but freeing the context first would still be
       wrong the day that stops being true. */
    dyn_tls_ctx_free(t->tls_ctx);
    free(t->tls_servername);
#endif
    free(t);
}

static dyn_tcp_t *tcp_new(JSContext *ctx)
{
    dyn_tcp_t *t = (dyn_tcp_t *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->ctx = ctx;
    t->listen_fd = -1;
    t->h_connect = t->h_data = t->h_close = JS_UNDEFINED;
    t->self_pending = JS_UNDEFINED;
    t->aio = dyn_net_reactor_acquire(ctx);
    if (!t->aio) { free(t); return NULL; }
    return t;
}

/* Arm the drain hook once, lazily -- registering one also arms the backend's
 * periodic wakeup, so a server that asked for neither bound pays nothing. The
 * return is CHECKED: a backend that cannot arm a clock gives a sweep that runs
 * only when other traffic wakes the loop, which is exactly the quiet peer the
 * timeout exists for. */
static int tcp_arm_sweep(dyn_tcp_t *t)
{
    if (t->hooked || (!t->idle_ms && !t->connect_deadline_ms))
        return 0;
    if (dyn_net_on_drain(tcp_sweep, t) < 0)
        return -1;
    t->hooked = 1;
    return 0;
}

/* Read the shared bound options. Both a server and a client use idle/max;
 * only a client uses connectTimeoutMs. */
static int tcp_read_bounds(JSContext *ctx, dyn_tcp_t *t, JSValueConst o,
                           int is_client)
{
    JSValue v;
    int64_t n;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "maxConnections");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) { JS_FreeValue(ctx, v); return -1; }
        if (n < 0 || n > 1000000) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "maxConnections must be 0..1000000");
            return -1;
        }
        t->max_conns = (int)n;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "idleTimeoutMs");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) { JS_FreeValue(ctx, v); return -1; }
        if (n < 0) { JS_FreeValue(ctx, v);
                     JS_ThrowRangeError(ctx, "idleTimeoutMs must be >= 0");
                     return -1; }
        t->idle_ms = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);
    if (is_client) {
        v = JS_GetPropertyStr(ctx, o, "connectTimeoutMs");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &n, v)) { JS_FreeValue(ctx, v); return -1; }
            if (n < 0) { JS_FreeValue(ctx, v);
                         JS_ThrowRangeError(ctx, "connectTimeoutMs must be >= 0");
                         return -1; }
            if (n > 0)
                t->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)n;
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

/* An AF_UNIX path from JS. POISON NUL: JS_ToCString stops at an embedded NUL,
 * so `"/tmp/a\0/evil.sock"` reaches bind() as `/tmp/a` -- it fits sun_path, it
 * binds, and the caller believes it bound the path it wrote. Refuse instead;
 * the length check downstream cannot see this because strlen already lost it.
 * BOTH entry points (server bind and connect) route through here, so a check
 * added to one cannot be missing from the other.
 * Returns an owned C string (JS_FreeCString), or NULL having thrown. */
static const char *tcp_unix_path(JSContext *ctx, JSValueConst v)
{
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
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

static void tcp_set_handlers(JSContext *ctx, dyn_tcp_t *t, JSValueConst h)
{
    JSValue v;
    if (!JS_IsObject(h))
        return;
    v = JS_GetPropertyStr(ctx, h, "connect");
    JS_FreeValue(ctx, t->h_connect); t->h_connect = v;
    v = JS_GetPropertyStr(ctx, h, "data");
    JS_FreeValue(ctx, t->h_data);    t->h_data = v;
    v = JS_GetPropertyStr(ctx, h, "close");
    JS_FreeValue(ctx, t->h_close);   t->h_close = v;
}

static JSValue dyn_tcp_server_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    dyn_tcp_t *t;
    int64_t port = 0;
    const char *upath = NULL;
    (void)new_target;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "path");   /* AF_UNIX endpoint */
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
    if (tcp_read_bounds(ctx, t, argv[0], /*is_client=*/0) < 0) {
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
        /* REFUSE BY NAME rather than listen in the clear on a port the caller
           asked to be encrypted. */
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
        if (!t->path) { dyn_tcp_dispose(t); return JS_ThrowOutOfMemory(ctx); }
    }
    return dyn_res_wrap(ctx, dyn_tcp_class_id, t, dyn_tcp_dispose);
}

static JSValue dyn_tcp_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_tcp_t *t = (dyn_tcp_t *)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    if (!t) return JS_EXCEPTION;
    if (t->started) return JS_UNDEFINED;
    if (argc > 0) tcp_set_handlers(ctx, t, argv[0]);
    t->listen_fd = t->path ? dyn_aio_unix_listen(t->aio, t->path, 128)
                           : dyn_aio_listen(t->aio, "0.0.0.0", t->port, 512);
    if (t->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "%s: listen failed",
                                     t->path ? "IpcServer" : "TCPServer");
    /* Do NOT swallow this: a backend that cannot arm a clock leaves the idle
     * sweep running only when other traffic wakes the loop -- which is exactly
     * the silent peer idleTimeoutMs exists to close. */
    if (tcp_arm_sweep(t) < 0) {
        dyn_aio_close(t->aio, t->listen_fd);
        t->listen_fd = -1;
        return JS_ThrowInternalError(ctx,
            "%s: the backend cannot arm a clock, so idleTimeoutMs would "
            "never fire", t->path ? "IpcServer" : "TCPServer");
    }
    if (t->path) {                      /* no port to resolve for AF_UNIX */
        dyn_aio_accept(t->aio, t->listen_fd, tcp_on_accept, t);
        t->started = 1;
        return JS_UNDEFINED;
    }
    /* Resolve an OS-assigned port so `port: 0` is usable, which is what every
     * test wants and what App deliberately does not do. */
    if (getsockname(t->listen_fd, (struct sockaddr *)&sa, &sl) == 0)
        t->port = ntohs(sa.sin_port);
    dyn_aio_accept(t->aio, t->listen_fd, tcp_on_accept, t);
    t->started = 1;   /* the shared reactor is already folded into the loop */
    return JS_UNDEFINED;
}

static JSValue dyn_tcp_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_tcp_t *t = (dyn_tcp_t *)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    if (!t) return JS_EXCEPTION;
    return JS_NewInt32(ctx, t->port);
}

/* connect({host, port}, handlers) -- a static on TCPServer's sibling class. */
static JSValue dyn_tcp_connect_method(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_tcp_t *t;
    dyn_tcp_conn_t *c;
    const char *host = NULL;
    int64_t port = 0;
    int fd;
    JSValue v, res;
    (void)this_val;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "connect(options, handlers) needs options");
    v = JS_GetPropertyStr(ctx, argv[0], "path");
    if (!JS_IsUndefined(v)) {                 /* AF_UNIX: no host, no port */
        const char *up = tcp_unix_path(ctx, v);
        JS_FreeValue(ctx, v);
        if (!up) return JS_EXCEPTION;
        t = tcp_new(ctx);
        if (!t) { JS_FreeCString(ctx, up); return JS_ThrowOutOfMemory(ctx); }
        if (argc > 1) tcp_set_handlers(ctx, t, argv[1]);
        c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
        if (!c) { JS_FreeCString(ctx, up); dyn_tcp_dispose(t); return JS_ThrowOutOfMemory(ctx); }
        c->owner = t; c->refs = 1; c->jsobj = JS_UNDEFINED;
        fd = dyn_aio_unix_connect(t->aio, up, tcp_on_connect, c);
        JS_FreeCString(ctx, up);
        if (fd < 0) {
            free(c); dyn_tcp_dispose(t);
            return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
        }
        c->fd = fd;
        t->started = 1;
        {
            JSValue obj = dyn_res_wrap(ctx, dyn_tcp_class_id, t,
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
        if (host) JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (port < 1 || port > 65535) {
        if (host) JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "connect: port must be 1..65535");
    }

    t = tcp_new(ctx);
    if (!t) { if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    if (tcp_read_bounds(ctx, t, argv[0], /*is_client=*/1) < 0) {
        if (host) JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (tcp_setup_tls(ctx, t, argv[0], host) < 0) {
        if (host) JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    {
        /* REFUSE BY NAME. Connecting in the clear to something asked for in
           TLS is worse than not working, and the caller cannot see it. */
        JSValue tv = JS_GetPropertyStr(ctx, argv[0], "tls");
        int asked = !JS_IsUndefined(tv) && !JS_IsNull(tv) &&
                    !(JS_IsBool(tv) && !JS_ToBool(ctx, tv));
        JS_FreeValue(ctx, tv);
        if (asked) {
            if (host) JS_FreeCString(ctx, host);
            dyn_tcp_dispose(t);
            return JS_ThrowTypeError(ctx,
                "connect: this build has no TLS support; "
                "rebuild with CONFIG_TLS=y");
        }
    }
#endif
    if (argc > 1) tcp_set_handlers(ctx, t, argv[1]);

    c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_tcp_dispose(t); if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;

    fd = dyn_aio_connect(t->aio, host ? host : "127.0.0.1", (uint16_t)port,
                         tcp_on_connect, c);
    if (host) JS_FreeCString(ctx, host);
    if (fd < 0) {
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
    }
    c->fd = fd;
    t->started = 1;
    /* Arm AFTER the connect is submitted: a deadline armed earlier could fire
     * against a socket that does not exist yet. */
    if (tcp_arm_sweep(t) < 0) {
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx,
            "connect: the backend cannot arm a clock, so connectTimeoutMs "
            "would never fire");
    }
    res = dyn_res_wrap(ctx, dyn_tcp_class_id, t, dyn_tcp_dispose);
    if (!JS_IsException(res))
        t->self_pending = JS_DupValue(ctx, res);
    return res;
}

/* NO "close" here: dyn_res_class_common installs the framework's close() on
 * every resource proto, and defining our own first makes the framework's
 * redefinition abort. Teardown belongs in the dispose callback, which close()
 * already runs. */
static const JSCFunctionListEntry dyn_tcp_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_tcp_start),
    JS_CGETSET_DEF("port", dyn_tcp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_statics[] = {
    JS_CFUNC_DEF("connect", 2, dyn_tcp_connect_method),
};


/* ---- UDPSocket --------------------------------------------------------- */

typedef struct {
    JSValue h_message;
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd;
    int started;
    uint16_t port;
} dyn_udp_t;

_Static_assert(sizeof(dyn_udp_t) <= 48,
               "dyn_udp_t regained padding: reorder largest-first");

static JSClassID dyn_udp_class_id;
static const JSClassDef dyn_udp_class = {
    "UDPSocket", .finalizer = dyn_res_finalizer,
};

static void dyn_udp_dispose(void *native)
{
    dyn_udp_t *u = (dyn_udp_t *)native;
    if (!u)
        return;
    if (u->aio) {
        if (u->fd >= 0)
            dyn_aio_close(u->aio, u->fd);
        dyn_net_reactor_release(u->ctx);
    }
    JS_FreeValue(u->ctx, u->h_message);
    free(u);
}

static void udp_on_message(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, const struct sockaddr *peer,
                           unsigned peerlen, void *ud)
{
    dyn_udp_t *u = (dyn_udp_t *)ud;
    JSContext *ctx = u->ctx;
    char ip[64] = "";
    JSValue ab, u8, from, r;
    JSValueConst a[2];
    (void)aio; (void)peerlen;

    if (res < 0 || !JS_IsFunction(ctx, u->h_message))
        return;
    /* A COPY: the adapter reuses its shared recv buffer as soon as we return,
     * and a handler may retain what it is given. */
    ab = JS_NewArrayBufferCopy(ctx, buf, len);
    if (JS_IsException(ab))
        return;
    /* A Uint8Array for the same reason as the stream handler: an ArrayBuffer
     * indexes to undefined instead of erroring. */
    {
        JSValueConst ta[3] = { ab, JS_NewInt32(ctx, 0),
                               JS_NewInt32(ctx, (int)len) };
        u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8))
        return;
    from = JS_NewObject(ctx);
    if (JS_IsException(from)) { JS_FreeValue(ctx, u8); return; }
    if (peer && peer->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)peer;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        JS_SetPropertyStr(ctx, from, "address", JS_NewString(ctx, ip));
        JS_SetPropertyStr(ctx, from, "port",
                          JS_NewInt32(ctx, ntohs(sin->sin_port)));
    }
    a[0] = u8; a[1] = from;
    r = JS_Call(ctx, u->h_message, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, u8);
    JS_FreeValue(ctx, from);
}

static JSValue dyn_udp_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_udp_t *u;
    int64_t port = 0;
    const char *host = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    JSValue v;
    (void)new_target;

    if (argc > 0 && JS_IsObject(argv[0])) {
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v); return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        host = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (port < 0 || port > 65535) {
        if (host) JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "UDPSocket: port must be 0..65535");
    }
    u = (dyn_udp_t *)calloc(1, sizeof(*u));
    if (!u) { if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    u->ctx = ctx;
    u->fd = -1;
    u->h_message = JS_UNDEFINED;
    u->aio = dyn_net_reactor_acquire(ctx);
    if (!u->aio) { free(u); if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    u->fd = dyn_aio_udp_bind(u->aio, host, (uint16_t)port);
    if (host) JS_FreeCString(ctx, host);
    if (u->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(u);
        return JS_ThrowInternalError(ctx, "UDPSocket: bind failed");
    }
    if (getsockname(u->fd, (struct sockaddr *)&sa, &sl) == 0)
        u->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, dyn_udp_class_id, u, dyn_udp_dispose);
}

static JSValue dyn_udp_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_udp_t *u = (dyn_udp_t *)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) return JS_EXCEPTION;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "message");
        JS_FreeValue(ctx, u->h_message);
        u->h_message = v;
    }
    if (!u->started) {
        if (dyn_aio_recvfrom(u->aio, u->fd, udp_on_message, u) < 0)
            return JS_ThrowInternalError(ctx, "UDPSocket: recvfrom failed");
        u->started = 1;
    }
    return JS_UNDEFINED;
}

static JSValue dyn_udp_send(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_udp_t *u;
    struct sockaddr_in sa;
    const char *host = NULL;
    int64_t port = 0;
    size_t len = 0;
    const uint8_t *p;
    char *owned = NULL;
    int n;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "send(data, host, port)");
    /* Coerce EVERY argument before touching the native handle: coercion can run
     * user JS that closes this socket (dyna-nat.h's rule). */
    p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        size_t sl2;
        const char *str = JS_ToCStringLen(ctx, &sl2, argv[0]);
        if (!str) return JS_EXCEPTION;
        owned = (char *)malloc(sl2 ? sl2 : 1);
        if (!owned) { JS_FreeCString(ctx, str); return JS_ThrowOutOfMemory(ctx); }
        memcpy(owned, str, sl2);
        JS_FreeCString(ctx, str);
        p = (const uint8_t *)owned;
        len = sl2;
    }
    host = JS_ToCString(ctx, argv[1]);
    if (!host) { free(owned); return JS_EXCEPTION; }
    if (JS_ToInt64(ctx, &port, argv[2])) {
        JS_FreeCString(ctx, host); free(owned); return JS_EXCEPTION;
    }
    u = (dyn_udp_t *)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) { JS_FreeCString(ctx, host); free(owned); return JS_EXCEPTION; }
    if (port < 1 || port > 65535) {
        JS_FreeCString(ctx, host); free(owned);
        return JS_ThrowRangeError(ctx, "send: port must be 1..65535");
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        JS_FreeCString(ctx, host); free(owned);
        return JS_ThrowTypeError(ctx, "send: host must be an IPv4 address");
    }
    JS_FreeCString(ctx, host);
    n = dyn_aio_sendto(u->aio, u->fd, p, len, (struct sockaddr *)&sa, sizeof(sa));
    free(owned);
    if (n < 0)
        return JS_ThrowInternalError(ctx, "UDPSocket: send failed");
    return JS_NewInt32(ctx, n);
}

static JSValue dyn_udp_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_udp_t *u = (dyn_udp_t *)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) return JS_EXCEPTION;
    return JS_NewInt32(ctx, u->port);
}

static const JSCFunctionListEntry dyn_udp_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_udp_start),
    JS_CFUNC_DEF("send", 3, dyn_udp_send),
    JS_CGETSET_DEF("port", dyn_udp_get_port, NULL),
};

int dyn_tcp_register(JSContext *ctx, JSModuleDef *m)
{
    JSValue proto, ctor;

    if (dyn_register_class(ctx, m, &dyn_tcp_class_id, &dyn_tcp_class,
                           dyn_tcp_proto, countof(dyn_tcp_proto),
                           dyn_tcp_server_ctor, "TCPServer") < 0)
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
                           dyn_udp_ctor, "UDPSocket") < 0)
        return -1;

    /* TCPConn is internal: handed to handlers, never constructed by a caller. */
    JS_NewClassID(&dyn_tcp_conn_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), dyn_tcp_conn_class_id,
                    &dyn_tcp_conn_class) < 0)
        return -1;
    {
        JSValue cp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, cp, dyn_tcp_conn_proto,
                                   countof(dyn_tcp_conn_proto));
        JS_SetClassProto(ctx, dyn_tcp_conn_class_id, cp);
    }
    return 0;
}

void dyn_tcp_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "TCPServer");
    JS_AddModuleExport(ctx, m, "UDPSocket");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
