/*
 * TCP client and server, part of dyna:net.
 *
 * Both run ENTIRELY on the JS thread via the dyn_aio reactor, like App: a
 * handler sees a zero-copy view of the received bytes, valid only for the
 * duration of the call. Nothing here is offloaded to the pool -- the reactor
 * already knows when a socket is ready, and handing a ready socket to another
 * thread costs more than the read.
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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dyna-tls.h"      /* no-op without CONFIG_TLS */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ==================================================================== *
 *  strict options *
 * ==================================================================== */

/* Every options bag in this module is checked against its valid-key table
 * BEFORE any option is read or connection work starts: an unknown key throws
 * a TypeError naming the key AND the full valid set, instead of being
 * silently ignored. Same shape as the pilot dyn_opts_strict in
 * dyna-file.c: a non-object bag counts as absent; own ENUMERABLE STRING
 * keys only (symbol and inherited keys are invisible); a Proxy ownKeys trap
 * that throws propagates; an array bag's "0","1",... keys are unknown ->
 * throws. */
static int dyn_opts_strict(JSContext *ctx, JSValueConst opts,
                           const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { /* OOM converting the key */
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            /* Size the valid list, then build it: no fixed buffer to
             * overflow, no silently truncated message. */
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
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

static const char *const tcp_srv_keys[] =
    { "port", "path", "maxConnections", "idleTimeoutMs", "tls", "highWaterMark" };
static const char *const tcp_cli_keys[] =
    { "path", "host", "port", "maxConnections", "idleTimeoutMs",
      "connectTimeoutMs", "tls", "highWaterMark" };
static const char *const tcp_tls_srv_keys[] =
    { "cert", "key", "alpn", "ca", "requestCert" };
static const char *const tcp_tls_cli_keys[] =
    { "ca", "servername", "minVersion", "rejectUnauthorized", "cert", "key",
      "alpn" };
static const char *const tcp_handler_keys[] = { "connect", "data", "close", "drain" };
static const char *const tcp_eb_keys[] = { "fallbackMs", "tls" };
static const char *const udp_ctor_keys[]  = { "port", "host" };
static const char *const udp_start_keys[] = { "message" };

/* backpressure. dyn_aio_send copies whatever the kernel will not take,
 * and before a fast producer writing to a slow peer queued without
 * bound. The default cap matches dyn:http's outbound bound: 4 MiB per
 * connection. A write that would push the queued bytes past the cap is
 * REFUSED with a TypeError -- never silently dropped, never queued
 * unbounded -- and the `drain` handler fires once when the queue empties,
 * which is the producer's signal to resume. */
#define TCP_HWM_DEFAULT (4u << 20)
#define TCP_HWM_MAX     (1u << 30)

/* from dyna-libc.h: fold a reactor into the JS event loop */
void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata);

typedef struct dyn_tcp dyn_tcp_t;

/* One address-family attempt of a connectHappy race. */
typedef struct eb_attempt {
    dyn_tcp_t *owner;
    int idx;                    /* 0 = IPv4, 1 = IPv6 */
    int fd;                     /* the attempt's socket; -1 = not armed/closed */
} eb_attempt_t;

/* One connection. `refs` mirrors App's discipline: the live socket holds one,
 * and the JS object handed to a handler holds another, so a handler that
 * retains its `conn` cannot outlive the struct. */
typedef struct dyn_tcp_conn {
    dyn_tcp_t *owner;
    int fd;
    int closed;
    int refs;
    int drain_armed;      /*bytes queued -> the emptying fires drain */
    uint64_t hwm;         /*this connection's outbound cap (bytes) */
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
    /* The RUNTIME, kept for teardown: main() frees the context in
       JS_FreeContext (dyna-cli.c) BEFORE JS_FreeRuntime, so the finalizers
       the runtime's last GC sweep runs -- including this resource's when it
       is deliberately unclosed at exit (the liveness contract) -- would read
       through a dangling ctx. Shutdown-path frees use rt; ctx stays for the
 live-callback paths. (ASan,: heap-use-after-free on the
       JSContext inside dyn_tcp_teardown.) */
    JSRuntime *rt;
    dyn_aio_t *aio;
    int listen_fd;        /* -1 for a client */
    uint16_t port;
    int started;
    char *path;           /* AF_UNIX endpoint; NULL for TCP */
    JSValue h_connect, h_data, h_close, h_drain;   /* handlers (dup'd) */
    uint64_t hwm;         /*per-connection outbound cap (bytes) */
    /* A connect in flight holds the wrapper object ITSELF. Without it,
       `TCPServer.connect(opts, handlers)` with the return value discarded is
       collected mid-connect: the callbacks never fire and the reactor keeps an
       fd whose handler is gone, so the loop stops progressing and an unrelated
       setTimeout never runs either. Released in tcp_on_connect on BOTH paths,
       so the cycle never outlives the operation -- a permanent self-reference
       would be uncollectable. */
    JSValue self_pending;

    /* The connect's conn is NOT on `conns` yet -- tcp_conn_start links it only
       from tcp_on_connect -- so tcp_detach_conns walks straight past it and a
       close() mid-connect leaves an armed fd whose udata points at freed
       memory. Detached separately. */
    dyn_tcp_conn_t *pending_conn;

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
    int released;               /* a failed connect already returned the ref */
    uint64_t n_refused;         /* connections dropped by max_conns */
    uint64_t n_idle_closed;     /* connections closed by the idle sweep */
    /* Happy Eyeballs (connectHappy) state: two sockets, one per address
       family, raced in parallel; first success wins and the loser is closed.
       eb_done gates every path so a late completion, the timeout sweep or a
       dispose cannot report after the race has settled. */
    eb_attempt_t eb[2];
    int in_cb;
    int closing;
    uint64_t eb_deadline_ms;
    int eb_done;
    int eb_err;                 /* last attempt failure, reported if all fail */
#ifdef CONFIG_TLS
    dyn_tls_ctx_t *tls_ctx;     /* NULL = plaintext */
    char *tls_servername;       /* drives BOTH SNI and hostname verification */
#endif
};

static JSClassID dyn_tcp_class_id;
static JSClassID dyn_tcp_conn_class_id;

/* ---- fix3: observable swallowed handler throws --------------------------
 *
 * Product doctrine (documented in API.md): an exception inside a net event
 * handler (TCP connect/data/close, UDP message) must not tear down the event
 * loop, so the throw is swallowed. A swallow that is INVISIBLE is its own
 * bug class, so every swallow increments a process-wide counter exposed as
 * net.swallowedHandlerThrows() and printed by `dynajs -d`. Under -d the
 * FIRST throw of each handler kind also prints a one-line stderr diagnostic
 * with the error; default runs (no -d) stay bit-identical to the old
 * silently-swallowing behavior. */
static _Atomic(uint64_t) dyn_net_handler_throws;
static _Atomic(unsigned) dyn_net_throw_diag_seen; /* bit per handler kind */
static int dyn_net_debug; /* -d: enable the once-per-kind stderr diagnostic */

enum {
    NET_HK_TCP_CONNECT = 0, NET_HK_TCP_DATA, NET_HK_TCP_CLOSE, NET_HK_UDP_MESSAGE,
    NET_HK_TCP_DRAIN,
    NET_HK_N
};
static const char *const dyn_net_handler_kind_names[NET_HK_N] = {
    "TCPSocket connect", "TCPSocket data", "TCPSocket close", "UDPSocket message",
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

static JSValue dyn_net_handler_throws_getter(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* A FUNCTION export, not a getter: JS_SetModuleExportList handles
       JS_DEF_CFUNC only (its switch aborts on other def types). */
    return JS_NewInt64(ctx, (int64_t)dyn_net_handler_throws);
}

/* Classify a handler-invocation result: count and (debug-gated) diagnose a
   swallowed throw, then free it. r is consumed exactly like JS_FreeValue. */
static void dyn_net_handler_result(JSContext *ctx, JSValue r, int kind)
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
                    const char *str = JS_ToCString(ctx, s);
                    fprintf(stderr, ": %s", str ? str : "(unreadable)");
                    JS_FreeCString(ctx, str);
                }
                JS_FreeValue(ctx, s);
                /* Re-throw the error so downstream accounting (unhandled
                 * rejection tracking, -d leak dump) sees the same pending
                 * exception state as the non-debug path. */
                JS_Throw(ctx, err);
            }
            fprintf(stderr, " (further throws counted silently;"
                    " see net.swallowedHandlerThrows())\n");
        }
    }
    /* A swallow must SWALLOW: JS_Call left the error in the context's
     * pending-exception slot and JS_FreeValue(JS_EXCEPTION) does not clear
     * it, so it used to stay pending and poison the next unrelated call --
     * observed as a stale data-handler throw resurfacing from a later
     * constructor. Retrieve-and-drop HERE so the state is genuinely clean
     * (the debug path above already restored what it printed). */
    {
        JSValue err = JS_GetException(ctx);
        JS_FreeValue(ctx, err);
    }
    JS_FreeValue(ctx, r);
}


/* The live conns' JS objects are anchored ONLY through this class: the
   struct list holds them, and a C struct is invisible to the GC. Without
   this mark the cycle collector reclaims a TCPConn the handler did not
   retain -- its finalizer unsets jsobj and tcp_deliver then SILENTLY
   DROPS the connection's data, so a working server reads as a dead one.
   The conn's own gc_mark cannot help: nothing JS-visible reaches IT. */
static void dyn_tcp_gc_mark(JSRuntime *rt, JSValueConst val,
                            JS_MarkFunc *mark_func)
{
    DynResource *r = (DynResource *)JS_GetOpaque(val, dyn_tcp_class_id);
    dyn_tcp_t *t = (r && !r->closed) ? (dyn_tcp_t *)r->native : NULL;
    dyn_tcp_conn_t *c;

    if (!t)
        return;
    /* The handlers and self_pending are STRONG references this native struct
     * holds; without marking them the resource->t->handler->env->resource
     * cycle is invisible to the collector -- neither side can be freed and
     * the finalizer that would break the cycle never runs (every sibling
     * class marks its handlers; this one marked only the conns' jsobj). */
    JS_MarkValue(rt, t->h_connect, mark_func);
    JS_MarkValue(rt, t->h_data, mark_func);
    JS_MarkValue(rt, t->h_close, mark_func);
    JS_MarkValue(rt, t->h_drain, mark_func);
    JS_MarkValue(rt, t->self_pending, mark_func);
    for (c = t->conns; c; c = c->lnext)
        JS_MarkValue(rt, c->jsobj, mark_func);
}

/* Framework-owned resource (has close()/closed), unlike TCPConn which is a
 * plain object handed to handlers. */
static const JSClassDef dyn_tcp_class = {
    "TCPServer", .finalizer = dyn_res_finalizer, .gc_mark = dyn_tcp_gc_mark,
};

static void dyn_tcp_teardown(dyn_tcp_t *t);

static int tcp_gone(dyn_tcp_t *t)
{
    if (t->closing) {
        dyn_tcp_teardown(t);
        return 1;
    }
    return 0;
}

/* ---- connection ------------------------------------------------------- */

static void dyn_tcp_conn_finalizer(JSRuntime *rt, JSValue val);

/* The pair (struct, conn object) is owned by PLAIN REFCOUNTING: the struct
   holds c->jsobj (rc 1) and the socket's ref; close and the object's
   finalizer each drop one, and whoever drops the last frees both. A
   gc_mark here would mark the object ITSELF -- the collector then decrefs
   that self-edge during its cycle phase and either asserts or frees a
   live object. Reachability is instead provided by the OWNER's gc_mark
   (dyn_tcp_gc_mark), which marks each live conn from the client object. */
static const JSClassDef dyn_tcp_conn_class = {
    "TCPConn", .finalizer = dyn_tcp_conn_finalizer,
};

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

/* Drop the struct's anchors -- the conn object and the socket ref -- in the
   order that is safe if EITHER frees the struct first: freeing jsobj can
   re-enter the finalizer (which drops the object ref), and dropping the
   socket ref can be the last one. Runs at close and at dispose-time detach,
   the two moments a live conn stops being live. */
static void tcp_conn_drop(JSRuntime *rt, dyn_tcp_conn_t *c)
{
    /* RT form: drops also run from teardown (tcp_detach_conns), which at
       JS_FreeRuntime happens after the contexts are freed. Live callers pass
       JS_GetRuntime(ctx). */
    JS_FreeValueRT(rt, c->jsobj);
    c->jsobj = JS_UNDEFINED;
    if (--c->refs == 0) {
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
        if (--c->refs == 0) {
            /* Same teardown as tcp_conn_unref: dropping the last reference
               HERE (which detach made the common case) must still free the TLS
               engine, or every closed TLS connection leaks an SSL*. */
#ifdef CONFIG_TLS
            dyn_tls_conn_free(c->tls);   /* NULL-safe */
#endif
            free(c);
        }
    }
}

static void tcp_release_pending(JSRuntime *rt, dyn_tcp_t *t);
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
        dyn_tcp_t *own = c->owner;
        JSValueConst a[1] = { c->jsobj };
        own->in_cb = 1;
        JSValue r = JS_Call(ctx, h, JS_UNDEFINED, 1, a);
        own->in_cb = 0;
        if (tcp_gone(own)) {
            /* The handler closed the owner, whose teardown walks only
               t->conns -- this conn was unlinked above, so detach never
               dropped it, and returning without the drop leaks the struct
               and its JS object (measured: one TCPConn per TLS client
               closed from inside the close handler). c survives teardown
               (it is not on the list); the caller's refs >= 1 stays for
               its own unref. */
            dyn_net_handler_result(ctx, r, NET_HK_TCP_CLOSE);
            JS_FreeValue(ctx, h);
            tcp_conn_drop(JS_GetRuntime(ctx), c);
            return;
        }
        dyn_net_handler_result(ctx, r, NET_HK_TCP_CLOSE);
    }
    JS_FreeValue(ctx, h);
    tcp_conn_drop(JS_GetRuntime(ctx), c);       /* drop the socket's own ref + the jsobj anchor */
}

/*one write's bytes all landed (or the send failed). Fires `drain`
 * exactly once per emptying: armed when a write left bytes queued, fired
 * here when the queue reaches zero, cleared by the fire. Reads ONLY
 * c->closed / c->owner -- the reactor never dispatches into a freed conn
 * (its fd was closed first), the same entry guard tcp_on_recv carries. A
 * completion for a detached (owner-less) conn or a failed send reports
 * nothing: there is no drain after a close. */
static void tcp_conn_sent(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    dyn_tcp_conn_t *c = (dyn_tcp_conn_t *)ud;
    (void)aio; (void)buf; (void)len;

    if (!c->owner || res < 0 || c->closed || !c->drain_armed)
        return;
    if (dyn_aio_queued(c->owner->aio, c->fd) > 0)
        return;                    /* more bytes still queued: not empty */
    c->drain_armed = 0;
    {
        JSContext *ctx = c->owner->ctx;
        JSValue h = JS_DupValue(ctx, c->owner->h_drain);
        if (JS_IsFunction(ctx, h) && !JS_IsUndefined(c->jsobj)) {
            dyn_tcp_t *own = c->owner;
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

static JSValue dyn_tcp_conn_buffered(JSContext *ctx, JSValueConst this_val)
{
    dyn_tcp_conn_t *c = JS_GetOpaque(this_val, dyn_tcp_conn_class_id);
    (void)ctx;
    /* Bytes the kernel has not taken: the active send remainder plus the
     * write queue. A closed connection has nothing meaningful pending. */
    if (!c || c->closed || !c->owner)
        return JS_NewInt64(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)dyn_aio_queued(c->owner->aio, c->fd));
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
            /* A string argument lands here, and JS_GetArrayBuffer left its
             * "ArrayBuffer object expected" PENDING in the context. Clear it
             * before taking the string path: returning success with a pending
             * exception poisons the next unrelated call -- a strict-
             * options checkpoint in a later constructor observed exactly
             * that (tests/test_db_lifecycle.js fd churn). */
            JS_FreeValue(ctx, JS_GetException(ctx));
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
    /* backpressure: REFUSE, never queue unboundedly. The check runs
     * BEFORE any byte moves, so a refused write leaves `bufferedAmount`
     * untouched; the `drain` handler fires when the queue empties. A
     * single write larger than the cap is refused on an EMPTY queue too
     * -- chunk it (the cap bounds what we are willing to hold for a peer
     * that will not read, and one giant write would defeat it). */
    if (c->hwm) {
        size_t queued = (size_t)dyn_aio_queued(c->owner->aio, c->fd);
        if (queued + len > c->hwm) {
            if (str) JS_FreeCString(ctx, str);
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
        if (rc == 0 && dyn_aio_queued(c->owner->aio, c->fd) > 0)
            c->drain_armed = 1;
    } else
#endif
    /* No copy: dyn_aio_send copies whatever it cannot put out inline, and
     * argv[0] roots these bytes for the whole call. The completion is what
     * fires `drain` when the queue empties. */
    rc = dyn_aio_send(c->owner->aio, c->fd, p, len, 0, tcp_conn_sent, c);
    if (rc == 0 && dyn_aio_queued(c->owner->aio, c->fd) > 0)
        c->drain_armed = 1;
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
    JS_CGETSET_DEF("bufferedAmount", dyn_tcp_conn_buffered, NULL),
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

    /* A failed connect is TERMINAL for a client: the fd is closed, the conn
     * dropped, and there is no reconnect API -- so the tick hook and the
     * reactor ref are pure liveness leaks, and a script that forgets close()
     * hangs the loop forever on the 250 ms tick (same fix as pg/redis
     * fail_all). BEFORE the handler: it may close() this object, and dispose
     * skips its own release via `released`. */
    if (err && !t->conns && !t->pending_conn && !t->released
        && t->listen_fd < 0) {    /* S0: a SERVER with a live listener must NOT
                                     release the reactor here -- a TLS accept
                                     whose handshake fails reports through this
                                     same path, and releasing freed the shared
                                     kqueue while the listener stayed open:
                                     every later accept queued silently forever
                                     (wedge; second reactor user masked it). */
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
        tcp_conn_drop(JS_GetRuntime(ctx), c);          /* socket ref + the conn-object anchor */
    }
    tcp_report_connect(t, JS_UNDEFINED, msg);
}

/* dyn_aio_send COPIES whatever it cannot put out inline, so a stack buffer is
   safe to hand it. 16 KiB is one TLS record. */
static int tls_flush(dyn_tcp_conn_t *c)
{
    uint8_t out[16384];
    int n;
    /* Detached by dispose: no socket to flush to and no owner to read. Not an
       error -- callers read -1 as a handshake failure. */
    if (c->closed || !c->owner)
        return 0;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0) {
        /* the completion fires `drain` for TLS conns exactly as for
           plaintext; during the handshake c->drain_armed is 0, so the
           callback is a no-op there */
        if (dyn_aio_send(c->owner->aio, c->fd, out, (unsigned)n, 0,
                         tcp_conn_sent, c) < 0)
            return -1;
    }
    return 0;
}

/* `tls: true` or `tls: { ca, servername, alpn, minVersion,
   rejectUnauthorized }`. Returns 0 -- including when no TLS was asked for --
   or -1 with an exception pending. */
/* Server side: `tls: { cert, key, alpn, ca, requestCert }`, all PEM paths.
   `requestCert` turns on mTLS: the client MUST present a certificate chain
   that verifies against `ca`. Returns 0 including "no tls asked for", or -1
   with an exception pending. */
static int tcp_setup_tls_server(JSContext *ctx, dyn_tcp_t *t, JSValueConst opts)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "tls");
    const char *cert = NULL, *key = NULL, *ca = NULL;
    char *alpn = NULL, err[256];
    dyn_tls_srv_opts_t so;
    JSValue w;
    int rc = -1, request_cert = 0;

    if (!JS_IsObject(v)) { JS_FreeValue(ctx, v); return 0; }
    /*the tls sub-bag is strict. Nothing is owned yet: `v` is the only
     * live JSValue and it is released here. */
    if (dyn_opts_strict(ctx, v, tcp_tls_srv_keys, 5)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
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
    w = JS_GetPropertyStr(ctx, v, "ca");
    if (JS_IsString(w)) ca = JS_ToCString(ctx, w);
    JS_FreeValue(ctx, w);
    w = JS_GetPropertyStr(ctx, v, "requestCert");
    if (JS_ToBool(ctx, w) == 1)          /* undefined/null/false all decline */
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
    if (!t->tls_ctx) { JS_ThrowInternalError(ctx, "TCPServer: tls: %s", err); goto done; }
    rc = 0;
done:
    if (cert) JS_FreeCString(ctx, cert);
    if (key) JS_FreeCString(ctx, key);
    if (ca) JS_FreeCString(ctx, ca);
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
    const char *cert = NULL, *key = NULL;
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
        /*the tls sub-bag is strict. Nothing is owned yet: `v` is the
         * only live JSValue and it is released here. */
        if (dyn_opts_strict(ctx, v, tcp_tls_cli_keys, 7)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
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
        /* What the client PRESENTS when the peer verifies clients (mTLS). */
        w = JS_GetPropertyStr(ctx, v, "cert");
        if (JS_IsString(w)) cert = JS_ToCString(ctx, w);
        JS_FreeValue(ctx, w);
        w = JS_GetPropertyStr(ctx, v, "key");
        if (JS_IsString(w)) key = JS_ToCString(ctx, w);
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
                    size_t slen = strlen(s);
                    size_t have = alpn ? strlen(alpn) : 0;
                    char *nw = (char *)realloc(alpn, have + slen + 2);
                    if (nw) {
                        alpn = nw;
                        if (have) alpn[have++] = ',';
                        memcpy(alpn + have, s, slen + 1);  /* incl. NUL */
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
    if (cert) JS_FreeCString(ctx, cert);
    if (key) JS_FreeCString(ctx, key);
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
    JSContext *ctx;
    /* A handler on this stack can have closed the server: the TLS record loop
       re-enters here for the SECOND record after tcp_detach_conns nulled the
       owner. A NULL deref here does not reliably fault -- it can re-execute
       forever at 100% CPU with no signal. */
    if (c->closed || !c->owner)
        return;
    ctx = c->owner->ctx;
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
                dyn_tcp_t *own = c->owner;
                int gone;
                JSValue r;
                own->in_cb = 1;
                r = JS_Call(ctx, own->h_data, JS_UNDEFINED, 2, a);
                own->in_cb = 0;
                /* Snapshot BEFORE freeing: tcp_gone can tear the server down,
                 * which frees c -- and the old early-return skipped the u8/ab
                 * releases, leaking one payload copy per close-in-handler. */
                gone = tcp_gone(own);
                dyn_net_handler_result(ctx, r, NET_HK_TCP_DATA);
                JS_FreeValue(ctx, u8);
                JS_FreeValue(ctx, ab);
                if (gone)
                    return;
                /* Stamp AFTER the handler returned, not on arrival: bytes
                 * that never reach an application are not progress, and a
                 * clock reset in the arrival path is what makes a slowloris
                 * look permanently active. */
                c->last_ms = dyn_timer_now_ms();
                return;
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
    (void)aio;
    /* The guard EVERY sibling entry point carries first (tcp_deliver :558,
       tls_flush :411, tcp_on_connect :769): a completion dequeued by the
       shared reactor after close()/dispose can re-enter here with the owner
       detached (owner=NULL) or the struct freed. Reading c->owner->ctx before
       it is exactly the use-after-free/busy-spin class this file documents
       fixing elsewhere; bail without touching the owner. `c` itself is still
       allocated here (the reactor never dispatches into a freed conn -- its fd
       was closed first), so c->closed is safe to read. */
    if (c->closed || !c->owner)
        return;
    JSContext *ctx = c->owner->ctx;

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
        for (c = t->conns; c; c = next) { next = c->lnext; tcp_conn_close(c); }
        /* The in-flight connect is NOT on t->conns. Without this it stays
           armed past the deadline, and a connect that lands (or fails) later
           reports a SECOND connect outcome after the timeout was already
           reported. dyn_aio_close retires the operation without a completion,
           so the struct is freed here rather than by its completion. */
        if (t->pending_conn) {
            dyn_tcp_conn_t *pc = t->pending_conn;
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
    c->hwm = t->hwm;                  /*the owner's outbound cap */
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
        JSContext *ctx = t->ctx;
        JSValueConst a[1] = { c->jsobj };
        /* The SAME in_cb guard every other handler site carries: without it a
           connect handler that calls server.close() disposed the owner
           immediately (in_cb == 0), the teardown freed t, and the
           dyn_net_handler_result below then read t->ctx out of freed memory
           (ASan heap-use-after-free, probes/tcp_uaf.js). ctx is a local now,
           and tcp_gone completes a close the handler deferred. */
        t->in_cb = 1;
        JSValue r = JS_Call(ctx, t->h_connect, JS_UNDEFINED, 1, a);
        t->in_cb = 0;
        int gone = tcp_gone(t);
        dyn_net_handler_result(ctx, r, NET_HK_TCP_CONNECT);
        if (gone)
            return;
    }
}

/* Drop the in-flight connect's self-reference exactly once. Takes the
   RUNTIME: the teardown path runs from a finalizer during JS_FreeRuntime,
   where the context is already gone but rt is not (see struct dyn_tcp.rt). */
static void tcp_release_pending(JSRuntime *rt, dyn_tcp_t *t)
{
    JSValue v = t->self_pending;
    t->self_pending = JS_UNDEFINED;   /* clear FIRST: the free can collect */
    JS_FreeValueRT(rt, v);
}

static void tcp_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, void *ud)
{
    dyn_tcp_conn_t *c = (dyn_tcp_conn_t *)ud;
    dyn_tcp_t *t;
    JSContext *ctx;
    (void)aio; (void)buf; (void)len;

    /* The server was disposed while this connect was in flight: dispose
       detached us and closed the fd, so there is nothing to report to and
       nobody left owning this struct. */
    if (!c->owner) {
        free(c);
        return;
    }
    t = c->owner;
    t->pending_conn = NULL;         /* the operation has landed */
    ctx = t->ctx;

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
        tcp_conn_drop(JS_GetRuntime(ctx), c);          /* socket ref + the conn-object anchor */
        tcp_report_connect(t, JS_UNDEFINED, strerror(-res));
        return;
    }
    if (tcp_conn_start(t, c) < 0) {
        tcp_release_pending(JS_GetRuntime(ctx), t);
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

/* ---- Happy Eyeballs (connectHappy) ------------------------------------ */

/* Resolve host into ONE address per family (the first A and the first AAAA).
 * Returns 0, or -1 when resolution fails; the caller checks out[0]/out[1]. */
static int eb_resolve(const char *host, uint16_t port,
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
            const struct sockaddr_in *sa =
                (const struct sockaddr_in *)ai->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, out[0], INET6_ADDRSTRLEN);
        } else if (out[1][0] == 0 && ai->ai_family == AF_INET6) {
            const struct sockaddr_in6 *sa =
                (const struct sockaddr_in6 *)ai->ai_addr;
            inet_ntop(AF_INET6, &sa->sin6_addr, out[1], INET6_ADDRSTRLEN);
        }
    }
    freeaddrinfo(res);
    return 0;
}

/* Settle the race with a failure. Read everything from `t` BEFORE the report:
 * the handler may close the resource and dispose the struct. */
static void eb_fail(dyn_tcp_t *t, const char *msg)
{
    JSContext *ctx = t->ctx;
    dyn_aio_t *aio = t->aio;
    dyn_tcp_conn_t *c = t->pending_conn;
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

/* The fallbackMs deadline: neither family connected in time. */
static void eb_sweep(void *udata)
{
    dyn_tcp_t *t = (dyn_tcp_t *)udata;
    if (t->eb_done || t->eb_deadline_ms == 0)
        return;
    if (dyn_timer_now_ms() < t->eb_deadline_ms)
        return;
    eb_fail(t, "connectHappy: timed out");
}

/* One attempt's connect completion. The FIRST success wins; the loser's socket
 * is closed here and its completion, should it still arrive, bails on eb_done.
 * A failure is reported only once BOTH attempts have settled without a winner:
 * the sibling may still be connecting. SO_ERROR semantics are the reactor's
 * (aio_dispatch reads getsockopt), so a refused socket arrives as res < 0. */
static void eb_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    eb_attempt_t *att = (eb_attempt_t *)ud;
    dyn_tcp_t *t = att->owner;
    int fd = att->fd;
    int idx = att->idx;
    (void)buf; (void)len;

    att->fd = -1;                /* the fd leaves the attempt's ownership now */
    if (t->eb_done) {            /* superseded: the sibling won or race ended */
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
            return;              /* the sibling is still racing */
        snprintf(msg, sizeof(msg), "connectHappy: connect failed: %s",
                 strerror(t->eb_err));
        eb_fail(t, msg);
        return;
    }
    /* winner: close the loser, hand the fd to the conn, run the normal path */
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

/* ---- TCPServer -------------------------------------------------------- */

/* Detach every live connection from a server being freed. The reactor is
   SHARED, so a connection left armed delivers its next event into a freed
   dyn_tcp_t -- and conn.write() reaches the same freed pointer from JS. */
static void tcp_detach_conns(dyn_tcp_t *t)
{
    /* The RUNTIME, not the context: this runs from dyn_tcp_teardown, i.e.
       from the finalizer, and during JS_FreeRuntime the contexts are freed
       (in JS_FreeContext, before the final GC sweep) while rt is alive. */
    JSRuntime *rt = t->rt;
    dyn_tcp_conn_t *c = t->conns, *next;
    int i;

    /* The in-flight connect first: it is NOT on this list. dyn_aio_close
       RETIRES the operation without a completion on either backend (the
       readiness backend drops it; the io_uring backend drops the stale CQE on
       the r_op guard -- "callers above this adapter free their per-connect
       state at close"), so the completion that this used to hand ownership to
       never arrives and the struct leaked (LSan: one dyn_tcp_conn_t per
       connect in flight at teardown). Free it HERE, exactly as the connect
       deadline sweep does. */
    if (t->pending_conn) {
        dyn_tcp_conn_t *pc = t->pending_conn;
        t->pending_conn = NULL;
        pc->closed = 1;
        pc->owner = NULL;
        if (t->aio && pc->fd >= 0)
            dyn_aio_close(t->aio, pc->fd);
        pc->fd = -1;
        free(pc);
    }
    /* The race's sockets, closed here: their completions never fire after
       close (the loser is closed on purpose), so nothing else frees their
       state -- and the winner may be this struct's only live socket. */
    for (i = 0; i < 2; i++)
        if (t->eb[i].fd >= 0) {
            dyn_aio_close(t->aio, t->eb[i].fd);
            t->eb[i].fd = -1;
        }
    t->eb_done = 1;
    t->conns = NULL;
    t->nconns = 0;
    while (c) {
        next = c->lnext;                 /* read first: the drop can free c */
        c->lnext = c->lprev = NULL;
        if (!c->closed) {
            c->closed = 1;               /* the guard every owner read sits behind */
            if (t->aio && c->fd >= 0)
                dyn_aio_close(t->aio, c->fd);
            c->fd = -1;
            c->owner = NULL;
            tcp_conn_drop(rt, c);       /* socket ref + the conn-object anchor */
        } else {
            c->owner = NULL;
        }
        c = next;
    }
}

static void dyn_tcp_teardown(dyn_tcp_t *t);

static void dyn_tcp_dispose(void *native)
{
    dyn_tcp_t *t = (dyn_tcp_t *)native;
    if (!t)
        return;
    if (t->in_cb) {
        t->closing = 1;
        return;
    }
    dyn_tcp_teardown(t);
}

static void dyn_tcp_teardown(dyn_tcp_t *t)
{
    if (!t)
        return;
    /* An explicit close() while a connect is still in flight: the self-
       reference would otherwise keep the object -- and the loop -- alive for
       ever, since a connect to a black hole never completes. Freeing
       JS_UNDEFINED (the settled case) is a no-op. */
    if (t->rt)
        tcp_release_pending(t->rt, t);
    if (t->hooked) {
        dyn_net_off_drain(t);   /* keyed by udata; must go before the free */
        t->hooked = 0;
    }
    /* BEFORE the reactor release: detaching needs t->aio, and a connection left
       armed on the shared reactor is a use-after-free on its next event. */
    tcp_detach_conns(t);
    if (t->aio && !t->released) {
        if (t->listen_fd >= 0)
            dyn_aio_close(t->aio, t->listen_fd);
        dyn_net_reactor_release_rt(t->rt);   /* shared: only the last user frees */
    }
    /* Unlink OUR endpoint on teardown: a socket file left behind makes the
     * next bind fail, and the stale-file unlink at bind time is a hijack
     * window we should not widen by leaving litter. */
    if (t->path) {
        unlink(t->path);
        free(t->path);
    }
    /* RT-based frees: teardown runs from the runtime's final GC sweep, which
       happens AFTER JS_FreeContext has already freed the context (see main)
a ctx read here was the heap-use-after-free (ASan-only
       abort). */
    JS_FreeValueRT(t->rt, t->h_connect);
    JS_FreeValueRT(t->rt, t->h_data);
    JS_FreeValueRT(t->rt, t->h_close);
    JS_FreeValueRT(t->rt, t->h_drain);
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
    t->rt = JS_GetRuntime(ctx);
    t->listen_fd = -1;
    t->h_connect = t->h_data = t->h_close = t->h_drain = JS_UNDEFINED;
    t->hwm = TCP_HWM_DEFAULT;
    t->self_pending = JS_UNDEFINED;
    t->eb[0].owner = t; t->eb[0].idx = 0; t->eb[0].fd = -1;
    t->eb[1].owner = t; t->eb[1].idx = 1; t->eb[1].fd = -1;
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
    v = JS_GetPropertyStr(ctx, o, "highWaterMark");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &n, v)) { JS_FreeValue(ctx, v); return -1; }
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

/* Returns 0, or -1 with the unknown-key TypeError pending. The caller
 * must abort the connect -- a typo'd handler name used to silently never
 * fire while the connection proceeded anyway. */
/* Phase 1 of handler installation: pull the four handler values into locals,
 * touching nothing native. A handler bag is not a plain record -- its getters
 * run arbitrary JS, and a getter that closes this resource frees the native
 * struct the caller is holding (heap-use-after-free, the UDPSocket start()
 * shape). Every caller collects FIRST, then installs onto a freshly resolved
 * handle. Returns 0, or -1 having thrown (out[] holds only defined values). */
static int tcp_collect_handlers(JSContext *ctx, JSValueConst h, JSValue out[4])
{
    int i;
    static const char *const names[4] = { "connect", "data", "close", "drain" };

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

/* Phase 2: install the collected values onto `t`, dropping what it held. */
static void tcp_install_handlers(dyn_tcp_t *t, JSValue v[4])
{
    JSValue old[4];
    int i;
    old[0] = t->h_connect; old[1] = t->h_data;
    old[2] = t->h_close;   old[3] = t->h_drain;
    t->h_connect = v[0]; t->h_data = v[1];
    t->h_close = v[2];   t->h_drain = v[3];
    for (i = 0; i < 4; i++)
        v[i] = JS_UNDEFINED;
    /* AFTER the swap: freeing can run finalizers, and they must see the new
       handlers, not a half-installed set. */
    {
        JSRuntime *rt = JS_GetRuntime(t->ctx);
        for (i = 0; i < 4; i++)
            JS_FreeValueRT(rt, old[i]);
    }
}

static int tcp_set_handlers(JSContext *ctx, dyn_tcp_t *t, JSValueConst h)
{
    JSValue v[4];
    if (tcp_collect_handlers(ctx, h, v) < 0)
        return -1;
    tcp_install_handlers(t, v);
    return 0;
}

static JSValue dyn_tcp_server_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    dyn_tcp_t *t;
    int64_t port = 0;
    const char *upath = NULL;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        /*reject unknown keys before any option is read or bound. */
        if (dyn_opts_strict(ctx, argv[0], tcp_srv_keys, 6))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
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
    return dyn_res_wrap(ctx, new_target, dyn_tcp_class_id, t, dyn_tcp_dispose);
}

static JSValue dyn_tcp_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_tcp_t *t = (dyn_tcp_t *)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    if (!t) return JS_EXCEPTION;
    if (t->started) return JS_UNDEFINED;
    /*the check runs BEFORE the listen, so a refused bag leaves the
     * socket closed and not listening -- the same contract connect()'s
     * identical call below enforces. */
    /* The handlers are COLLECTED first and installed onto a re-resolved
       handle: a getter in the bag can run arbitrary JS and close() this
       server, and installing into the freed struct afterwards was a
       heap-use-after-free (the UDPSocket start() shape, ASan). */
    if (argc > 0) {
        JSValue hv[4];
        if (tcp_collect_handlers(ctx, argv[0], hv) < 0)
            return JS_EXCEPTION;
        t = (dyn_tcp_t *)dyn_res_native(ctx, this_val, dyn_tcp_class_id);
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
    /*reject unknown keys before anything is coerced or connected. */
    if (dyn_opts_strict(ctx, argv[0], tcp_cli_keys, 8))
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[0], "path");
    if (!JS_IsUndefined(v)) {                 /* AF_UNIX: no host, no port */
        const char *up = tcp_unix_path(ctx, v);
        JS_FreeValue(ctx, v);
        if (!up) return JS_EXCEPTION;
        t = tcp_new(ctx);
        if (!t) { JS_FreeCString(ctx, up); return JS_ThrowOutOfMemory(ctx); }
        if (argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) {
            JS_FreeCString(ctx, up);
            dyn_tcp_dispose(t);
            return JS_EXCEPTION;
        }
        c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
        if (!c) { JS_FreeCString(ctx, up); dyn_tcp_dispose(t); return JS_ThrowOutOfMemory(ctx); }
        c->owner = t; c->refs = 1; c->jsobj = JS_UNDEFINED; c->hwm = t->hwm;
        fd = dyn_aio_unix_connect(t->aio, up, tcp_on_connect, c);
        JS_FreeCString(ctx, up);
        if (fd < 0) {
            free(c); dyn_tcp_dispose(t);
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
    if (argc > 1 && tcp_set_handlers(ctx, t, argv[1]) < 0) {
        if (host) JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }

    c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_tcp_dispose(t); if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    c->owner = t;
    c->refs = 1;
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;

    fd = dyn_aio_connect(t->aio, host ? host : "127.0.0.1", (uint16_t)port,
                         tcp_on_connect, c);
    if (host) JS_FreeCString(ctx, host);
    if (fd < 0) {
        free(c);
        dyn_tcp_dispose(t);
        return JS_ThrowInternalError(ctx, "connect: %s", strerror(errno));
    }
    c->fd = fd;
    t->pending_conn = c;        /* not on t->conns until tcp_on_connect */
    t->started = 1;
    /* Arm AFTER the connect is submitted: a deadline armed earlier could fire
     * against a socket that does not exist yet. */
    if (tcp_arm_sweep(t) < 0) {
        /* `c` is already the reactor's completion udata: freeing it here hands
           tcp_on_connect freed memory on the next drain. Close the fd to
           retire the operation and let dispose detach the struct. */
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

/* connectHappy(host, port, opts, handlers) -- Happy Eyeballs (RFC 8305, simplified) as a
 * parallel race: both address families are resolved and connect at once, the
 * first success wins and the loser is closed. `opts.fallbackMs` (default 250)
 * is the deadline for the whole race. Returns the same resource connect does;
 * a sync error names the failure (DNS vs no-address vs backend clock). */
static JSValue dyn_tcp_connect_happy(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_tcp_t *t;
    dyn_tcp_conn_t *c;
    const char *host;
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
        /*reject unknown keys; `host` is already an owned C string. */
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
    if (argc > 2 && JS_IsObject(argv[2]) &&
        tcp_setup_tls(ctx, t, argv[2], host) < 0) {
        JS_FreeCString(ctx, host);
        dyn_tcp_dispose(t);
        return JS_EXCEPTION;
    }
#else
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue tv = JS_GetPropertyStr(ctx, argv[2], "tls");
        int asked = !JS_IsUndefined(tv) && !JS_IsNull(tv) &&
                    !(JS_IsBool(tv) && !JS_ToBool(ctx, tv));
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

    c = (dyn_tcp_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_tcp_dispose(t); return JS_ThrowOutOfMemory(ctx); }
    c->owner = t;
    c->refs = 1;                  /* taken by the conn when the race lands */
    c->jsobj = JS_UNDEFINED;
    c->hwm = t->hwm;
    c->fd = -1;                   /* owned by the race until a winner is chosen */
    t->pending_conn = c;
    t->started = 1;
    t->eb_deadline_ms = dyn_timer_now_ms() + (uint64_t)fallback;

    if (ips[0][0]) {
        fd = dyn_aio_connect(t->aio, ips[0], (uint16_t)port,
                             eb_on_connect, &t->eb[0]);
        if (fd >= 0) { t->eb[0].fd = fd; n_armed++; }
    }
    if (ips[1][0]) {
        fd = dyn_aio_connect(t->aio, ips[1], (uint16_t)port,
                             eb_on_connect, &t->eb[1]);
        if (fd >= 0) { t->eb[1].fd = fd; n_armed++; }
    }
    if (n_armed == 0) {
        t->pending_conn = NULL;   /* no completion will ever free it */
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

/* NO "close" here: dyn_res_class_common installs the framework's close() on
 * every resource proto, and defining our own first makes the framework's
 * redefinition abort. Teardown belongs in the dispose callback, which close()
 * already runs. */
static const JSCFunctionListEntry dyn_tcp_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_tcp_start),
    JS_CGETSET_DEF("port", dyn_tcp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_statics[] = {
    JS_CFUNC_DEF("connect", 1, dyn_tcp_connect_method),
};


/* ---- UDPSocket --------------------------------------------------------- */

typedef struct dyn_udp {
    JSValue h_message;
    JSContext *ctx;
    /* The RUNTIME, for teardown: a dispose reached from the runtime's final
       GC sweep happens AFTER JS_FreeContext has freed the context (see main),
       so every free in the teardown path must go through the rt form. */
    JSRuntime *rt;
    dyn_aio_t *aio;
    int fd;
    int started;
    /* A close() that landed inside this socket's own delivery: the shell is
       freed at the END of the drain pass, not at close (see udp_grave). Stale
       deliveries still holding the pointer see this and drop. */
    int dead;
    uint16_t port;
} dyn_udp_t;

_Static_assert(sizeof(dyn_udp_t) <= 56,
               "dyn_udp_t regained padding: reorder largest-first");

/* Self-close while a delivery is in flight. The adapter's datagram loop
 * captures the socket pointer BEFORE invoking the callback and only re-checks
 * the ARMING SLOT afterwards -- so a handler that closes this socket and
 * immediately allocates another one gets the descriptor back (lowest-free-fd
 * reuse is deterministic), the slot reads AIO_OP_RECVFROM again, and the loop
 * keeps delivering the queued datagram burst to the captured pointer. Freeing
 * the struct at close is then a heap-use-after-free (ASan: the read lands in
 * udp_on_message via JS_Call on the freed handler). So a close from inside a
 * delivery defers `free` to a later pass boundary -- after every delivery
 * loop of the pass is done and before the process can idle -- and the shell
 * carries `dead` so any stale delivery that still holds it drops instead of
 * reading freed state. Outside a delivery there is no loop holding the
 * pointer: free is immediate. The graveyard is a side array, not an
 * intrusive link: the socket struct keeps its size budget (see the assert
 * above).
 *
 * The free point is RE-HOMED onto the engine's shutdown machinery: every
 * park registers the shell's flush as a SHUTDOWN SWEEP (udp_grave_sweep
 * below; JS_AddShutdownSweep is idempotent on (func, opaque), so re-parks
 * are no-ops and the sweep registry's pass budget bounds the pathology)
 * whose teardown body hands the shell to JS_ShutdownDeferFree -- the engine
 * frees it at the very end of JS_FreeRuntime, when no delivery can run
 * again. That is the free point the drain pass could not guarantee: the
 * old flush registration (dyn_net_on_drain) arms the backend's clock for
 * the first registrant, and a backend with no clock (poll(2)) answered -2
 * and registered NOTHING -- parked shells then had no free point before
 * exit. The mid-run flush still runs from the pass boundary, but through
 * dyn_net_on_drain_notick: no permanent 250ms NET_TICK is armed for the
 * graveyard, and a quiet loop that never runs another pass leaves the shell
 * to the sweeps. The flush itself unregisters the sweep and cancels the
 * defer before freeing -- "the module frees it earlier" is exactly the
 * engine's JS_RemoveShutdownSweep/JS_ShutdownUndeferFree pair.
 *
 * Re-registration at EVERY park stays load-bearing: the reactor's hook table
 * is reset whenever the last net resource closes and the reactor is freed,
 * so a one-shot latch lost the flush for the life of the next reactor and
 * its shells were never freed (2 per round, unbounded, invisible to LSan
 * because the thread-local grave array still reached them). The same rule
 * holds for the sweep registrations -- a latch there loses the teardown
 * defer, and a shell parked at exit is then freed by nobody (visible to
 * LSan, tests/test_net_udp_grave.js). */
static _Thread_local dyn_udp_t **udp_grave;
static _Thread_local int udp_n_grave, udp_cap_grave;
static _Thread_local int udp_in_delivery;

/* Give one shell's bytes back to the ledger and the heap. The shell is a
   PLAIN block tracked with dyn_nat_track (see the ctor): plain free() is
   what frees it -- which is what makes the engine's deferred-free list a
   legal owner of a parked shell (a dyn_nat_* block would hand plain free()
   an interior pointer). Every MODULE-side free goes through here, so the
   untrack cannot be missed; the teardown path untracks in the sweep and
   lets the engine own the plain free. */
static void dyn_udp_shell_release(dyn_udp_t *u)
{
    dyn_nat_untrack(sizeof(*u));
    free(u);
}

/* The teardown body of one parked shell: hand it to the engine's
 * deferred-free list. Runs at the LAST JS_FreeContext (and once more as the
 * JS_FreeRuntime straggler pass for parks made by that drain); `ctx` is
 * irrelevant -- the shell is already pinless (dispose ran the full
 * teardown) and the call is pure bookkeeping, legal with ctx == NULL too.
 * The engine frees every deferred block (plain free -- see
 * dyn_udp_shell_release) after the final job drain, so a stale delivery --
 * which can only be named from a delivery loop, and no loop runs during or
 * after teardown -- reads the dead flag in live memory or never runs at
 * all; it can never read freed memory. The ledger claim goes back HERE,
 * exactly once per shell: the sweep entry is unlinked as it runs, so the
 * engine calls this body once, and the flush removes the entry before its
 * own release -- the two untracks can never both run. */
static void udp_grave_sweep(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    dyn_udp_t *u = (dyn_udp_t *)opaque;
    (void)ctx;
    if (!u || !u->dead)
        return;
    dyn_nat_untrack(sizeof(*u));
    JS_ShutdownDeferFree(rt, u);
}

/* The pass-boundary flush (mid-run free point): the delivery loops of THIS
 * drain pass have all returned -- dyn_net_drain runs its hooks only after
 * dyn_aio_drain -- so nothing can name these shells any more and each can
 * go back to the ledger. The sweep registration and the defer are cancelled
 * first: a sweep left dangling would run at teardown on FREED memory and
 * free it a second time. */
static void udp_grave_flush(void *unused)
{
    int i;
    (void)unused;
    for (i = 0; i < udp_n_grave; i++) {
        dyn_udp_t *u = udp_grave[i];
        JS_RemoveShutdownSweep(u->rt, udp_grave_sweep, u);
        JS_ShutdownUndeferFree(u->rt, u);
        dyn_udp_shell_release(u);
    }
    udp_n_grave = 0;
    /* Return the array to the heap too: an exit-time holdout would show up
       as a leak of the graveyard itself. The hook itself STAYS registered
       (it is re-armed, deduped, at the next park): unregistering from inside
       the drain-hook loop would shift that loop's array under it, and an
       empty flush is one null walk. */
    free(udp_grave);
    udp_grave = NULL;
    udp_cap_grave = 0;
}

static JSClassID dyn_udp_class_id;
/* fix2: the struct holds h_message as a STRONG JS reference, so the class
   must mark it: without this the collector cannot see the C-held ref and
   treats the handler as a GC root (its refcount is never decrefed), which
   keeps the handler's closure -> module scope -> socket chain alive across
   every cycle collection. Observable at uncaught-exception exit:
   JS_FreeRuntime's gc_obj_list assert aborts because the socket/handler
   graph is never collected and dyn_udp_dispose never runs. Same class of
   bug/fix as dyn_tcp_gc_mark above ("every sibling class marks its
   handlers"). */
static void dyn_udp_gc_mark(JSRuntime *rt, JSValueConst val,
                            JS_MarkFunc *mark_func)
{
    DynResource *r = (DynResource *)JS_GetOpaque(val, dyn_udp_class_id);
    dyn_udp_t *u = (r && !r->closed) ? (dyn_udp_t *)r->native : NULL;

    if (!u)
        return;
    JS_MarkValue(rt, u->h_message, mark_func);
}

static const JSClassDef dyn_udp_class = {
    "UDPSocket", .finalizer = dyn_res_finalizer, .gc_mark = dyn_udp_gc_mark,
};

/* Everything the close owes the kernel and the runtime -- except the shell
   free. Split out so the self-close path (see udp_grave) can run it
   immediately and still park the shell. */
static void dyn_udp_teardown(dyn_udp_t *u)
{
    if (u->aio) {
        if (u->fd >= 0)
            dyn_aio_close(u->aio, u->fd);
        /* rt, not ctx: a dispose from the runtime's final sweep runs after
           JS_FreeContext, and reading the dead context here was the same
           teardown use-after-free the TCP path already fixed. */
        dyn_net_reactor_release_rt(u->rt);
        u->aio = NULL;
    }
    JS_FreeValueRT(u->rt, u->h_message);
    u->h_message = JS_UNDEFINED;
}

static void dyn_udp_dispose(void *native)
{
    dyn_udp_t *u = (dyn_udp_t *)native;
    if (!u)
        return;
    dyn_udp_teardown(u);
    if (udp_in_delivery) {
        /* Park the shell (see udp_grave). TWO registrations, both re-done at
           EVERY park and both idempotent:
           - the shutdown sweep (udp_grave_sweep): the ENGINE-side free
             point. It hands the shell to JS_ShutdownDeferFree at teardown,
             so a shell parked after the last drain pass -- the quiet-exit
             case, and every clock-less (poll) backend pass -- is still
             freed exactly once, at the end of JS_FreeRuntime. A one-shot
             latch here loses the defer for every shell after the first
             (see the comment above; tests/test_net_udp_grave.js).
           - the mid-run flush, dyn_net_on_drain_notick: frees at the NEXT
             pass boundary without arming the reactor's clock. The reactor
             may have been freed since the last park, which resets the hook
             table -- a latch there was the original S0 leak. */
        u->dead = 1;
        if (udp_n_grave == udp_cap_grave) {
            int cap = udp_cap_grave ? udp_cap_grave * 2 : 8;
            dyn_udp_t **n = (dyn_udp_t **)realloc(udp_grave,
                                                  (size_t)cap * sizeof(*n));
            if (!n) {
                /* Cannot park: leak the shell rather than free it under a
                   loop that still holds the pointer. */
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

static void udp_on_message(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, const struct sockaddr *peer,
                           unsigned peerlen, void *ud)
{
    dyn_udp_t *u = (dyn_udp_t *)ud;
    JSContext *ctx;
    char ip[64] = "";
    JSValue ab, u8, from, r, h;
    JSValueConst a[2];
    (void)aio; (void)peerlen;

    if (u->dead)
        return;   /* stale delivery for a socket closed mid-flight (udp_grave) */
    ctx = u->ctx;
    udp_in_delivery++;
    h = JS_DupValue(ctx, u->h_message);
    if (res < 0 || !JS_IsFunction(ctx, h))
        goto out;
    /* A COPY: the adapter reuses its shared recv buffer as soon as we return,
     * and a handler may retain what it is given. */
    ab = JS_NewArrayBufferCopy(ctx, buf, len);
    if (JS_IsException(ab))
        goto out;
    /* A Uint8Array for the same reason as the stream handler: an ArrayBuffer
     * indexes to undefined instead of erroring. */
    {
        JSValueConst ta[3] = { ab, JS_NewInt32(ctx, 0),
                               JS_NewInt32(ctx, (int)len) };
        u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8))
        goto out;
    from = JS_NewObject(ctx);
    if (JS_IsException(from)) { JS_FreeValue(ctx, u8); goto out; }
    if (peer && peer->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)peer;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        /* DEFINE, not set: one datagram, two properties, no chain walk. */
        JS_DefinePropertyValueStr(ctx, from, "address", JS_NewString(ctx, ip),
                                  JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, from, "port",
                                  JS_NewInt32(ctx, ntohs(sin->sin_port)),
                                  JS_PROP_C_W_E);
    }
    a[0] = u8; a[1] = from;
    /* The handler is HELD (DupValue above) for the whole call: it may close
       this socket, and the close drops the resource's own reference to the
       function -- calling through u->h_message directly then kept executing
       bytecode from a freed object (the ASan read is inside JS_CallInternal). */
    r = JS_Call(ctx, h, JS_UNDEFINED, 2, a);
    dyn_net_handler_result(ctx, r, NET_HK_UDP_MESSAGE);
    JS_FreeValue(ctx, u8);
    JS_FreeValue(ctx, from);
out:
    JS_FreeValue(ctx, h);
    udp_in_delivery--;
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

    if (argc > 0 && JS_IsObject(argv[0])) {
        /*reject unknown keys before any option is read or bind. */
        if (dyn_opts_strict(ctx, argv[0], udp_ctor_keys, 2))
            return JS_EXCEPTION;
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
    /* Ledger-VISIBLE, not ledger-allocated (plain calloc + dyn_nat_track,
       not dyn_nat_calloc): a shell parked in the graveyard is module-native
       memory the JAVASCRIPT heap cannot see, so it rides the
       memoryUsage().nativeSize ledger -- the S0 recycle row and
       test_udp_grave_sweep.c read its whole life there. But its TEARDOWN
       free point is the engine's deferred-free list (JS_ShutdownDeferFree,
       the re-homed graveyard below), which frees with PLAIN free(): a
       dyn_nat_* block carries the ledger header BEFORE the payload, and
       plain free() on the payload hand libmalloc an interior pointer (it
       dies on its main_address assert -- found by this exact row). The
       dyn_nat_track form is the ledger without the header; every free of
       the shell goes through dyn_udp_shell_release (or, at teardown, the
       engine's plain free after the sweep's untrack). The cap applies like
       every other converted site. */
    u = (dyn_udp_t *)calloc(1, sizeof(*u));
    if (!u) { if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    if (dyn_nat_track(sizeof(*u))) {
        free(u);
        if (host) JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    u->ctx = ctx;
    u->rt = JS_GetRuntime(ctx);
    u->fd = -1;
    u->h_message = JS_UNDEFINED;
    u->aio = dyn_net_reactor_acquire(ctx);
    if (!u->aio) { dyn_udp_shell_release(u); if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    u->fd = dyn_aio_udp_bind(u->aio, host, (uint16_t)port);
    if (host) JS_FreeCString(ctx, host);
    if (u->fd < 0) {
        dyn_net_reactor_release(ctx);
        dyn_udp_shell_release(u);
        return JS_ThrowInternalError(ctx, "UDPSocket: bind failed");
    }
    if (getsockname(u->fd, (struct sockaddr *)&sa, &sl) == 0)
        u->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, new_target, dyn_udp_class_id, u, dyn_udp_dispose);
}

static JSValue dyn_udp_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_udp_t *u;
    JSValue h = JS_UNDEFINED;
    int have_h = 0;

    /* Coerce EVERYTHING before touching the native handle (dyna-nat.h's
     * rule): the option bag's getters run arbitrary JS, and one that calls
     * close() frees the struct -- writing the handler into it afterwards was
     * a heap-use-after-free (ASan, dyn_udp_start). The bag is read first, the
     * handle resolved after; a close() during the read is then reported as
     * "use of a closed native resource" by the resolve. still refuses a
     * typo'd handler name BEFORE the receive path is armed. */
    if (argc > 0 && dyn_opts_strict(ctx, argv[0], udp_start_keys, 1))
        return JS_EXCEPTION;
    if (argc > 0 && JS_IsObject(argv[0])) {
        h = JS_GetPropertyStr(ctx, argv[0], "message");
        if (JS_IsException(h))
            return JS_EXCEPTION;
        have_h = 1;
    }
    u = (dyn_udp_t *)dyn_res_native(ctx, this_val, dyn_udp_class_id);
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

static JSValue dyn_udp_send(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_udp_t *u;
    struct sockaddr_in sa;
    const char *host = NULL, *str = NULL;
    int64_t port = 0;
    size_t len = 0;
    const uint8_t *p;
    int n;

    if (argc < 3)
        return JS_ThrowTypeError(ctx, "send(data, host, port)");
    /* Coerce EVERY argument before touching the native handle: coercion can run
     * user JS that closes this socket (dyna-nat.h's rule). */
    p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        /* Byte VIEWS (Uint8Array, DataView, subclasses) are byte payloads
         * too: JS_GetArrayBufferView resolves any view to its backing
         * buffer + byte offset/length (the dyna:asn1 / dyna:encoding
         * pattern). Plain JS_GetArrayBuffer alone rejects views, so the old
         * code fell through to the STRING path and stringified the view:
         * send(new Uint8Array([104,105])) went out as the 7 TEXT bytes
         * "104,105". */
        size_t voff = 0, bpe = 0;
        JSValue abv = JS_GetArrayBufferView(ctx, argv[0], &voff, &len, &bpe);
        if (!JS_IsException(abv)) {
            size_t ablen = 0;
            const uint8_t *base = JS_GetArrayBuffer(ctx, &ablen, abv);
            JS_FreeValue(ctx, abv);
            if (!base) return JS_EXCEPTION;
            if (voff > ablen) voff = ablen;
            if (len > ablen - voff) len = ablen - voff;
            p = base + voff;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx)); /* not a view either */
            /* BORROWED, not copied: sendto writes inline and never queues
             * (dyna-aio.h), so the string only has to outlive the call --
             * which freeing after it does. The old malloc+memcpy per send
             * bought nothing. */
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str) return JS_EXCEPTION;
            p = (const uint8_t *)str;
        }
    }
    host = JS_ToCString(ctx, argv[1]);
    if (!host) { if (str) JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    if (JS_ToInt64(ctx, &port, argv[2])) {
        JS_FreeCString(ctx, host); if (str) JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    u = (dyn_udp_t *)dyn_res_native(ctx, this_val, dyn_udp_class_id);
    if (!u) { JS_FreeCString(ctx, host); if (str) JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    if (port < 1 || port > 65535) {
        JS_FreeCString(ctx, host); if (str) JS_FreeCString(ctx, str);
        return JS_ThrowRangeError(ctx, "send: port must be 1..65535");
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        JS_FreeCString(ctx, host); if (str) JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, "send: host must be an IPv4 address");
    }
    JS_FreeCString(ctx, host);
    n = dyn_aio_sendto(u->aio, u->fd, p, len, (struct sockaddr *)&sa, sizeof(sa));
    if (str) JS_FreeCString(ctx, str);
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
    JS_CFUNC_DEF("start", 0, dyn_udp_start),
    JS_CFUNC_DEF("send", 3, dyn_udp_send),
    JS_CGETSET_DEF("port", dyn_udp_get_port, NULL),
};

static const JSCFunctionListEntry dyn_tcp_funcs[] = {
    JS_CFUNC_DEF("connectHappy", 2, dyn_tcp_connect_happy),
    JS_CFUNC_DEF("swallowedHandlerThrows", 0, dyn_net_handler_throws_getter),
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
    return JS_SetModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
}

void dyn_tcp_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "TCPServer");
    JS_AddModuleExport(ctx, m, "UDPSocket");
    JS_AddModuleExportList(ctx, m, dyn_tcp_funcs, countof(dyn_tcp_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
