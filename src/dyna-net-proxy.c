/*
 * dyna:net -- TCPProxy, an L4 (byte) reverse proxy.
 *
 * L4 and L7 are different programs, not one with a flag. This one never parses
 * the payload: accept, connect upstream, forward bytes both ways. The HTTP
 * proxy re-serialises from its own parse and lives elsewhere.
 *
 * No JS runs on the data path. A forwarded byte goes recv -> send inside the
 * reactor callback, so a proxied connection costs no JS calls at all.
 *
 * ZERO-COPY IS LINUX-ONLY AND IS NOT BUILT. splice(2) is the socket-to-socket
 * primitive; Darwin has no equivalent (its sendfile is file->socket only), so
 * the portable path buffers in userspace.
 */
#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"   /* dyn_timer_now_ms: the sweep clock */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

dyn_aio_t *dyn_net_reactor_acquire(JSContext *ctx);
void dyn_net_reactor_release(JSContext *ctx);
int dyn_net_on_drain(void (*fn)(void *), void *udata);
void dyn_net_off_drain(void *udata);

/* Bytes queued toward one peer before its source is paused, and the level it
 * must fall to before reading resumes. Hysteresis, not one mark: a single
 * threshold pauses and resumes on alternate sends. */
#define PXY_HIGH_WATER (256 * 1024)
#define PXY_LOW_WATER  (64 * 1024)

typedef struct dyn_pxy dyn_pxy_t;

/* One proxied connection: two fds and the state that says which halves are
 * still open. `refs` covers the two in-flight recv arms plus the pending
 * connect, so a completion landing after teardown finds the struct alive. */
typedef struct dyn_pxy_pair {
    dyn_pxy_t *owner;
    int cfd, ufd;               /* client, upstream; -1 once closed */
    int refs;
    unsigned c_eof : 1;         /* client sent FIN */
    unsigned u_eof : 1;         /* upstream sent FIN */
    unsigned c_paused : 1;      /* client recv disarmed by back-pressure */
    unsigned u_paused : 1;
    unsigned connected : 1;
    unsigned gone : 1;          /* unlinked from the live list */
    size_t to_c, to_u;          /* bytes queued toward that fd */
    uint64_t last_ms;           /* stamped on PROGRESS, not on arrival */
    uint64_t deadline_ms;       /* connect deadline; 0 once connected */
    struct dyn_pxy_pair *lnext, *lprev;
} dyn_pxy_pair_t;

struct dyn_pxy {
    JSContext *ctx;
    dyn_aio_t *aio;
    int listen_fd;
    uint16_t port;
    int started;
    int hooked;

    char **up_host;             /* upstream pool, round-robin */
    uint16_t *up_port;
    int n_up, next_up;

    int max_conns;              /* 0 = unbounded */
    uint64_t idle_ms;           /* 0 = off */
    uint64_t connect_ms;        /* 0 = off */

    dyn_pxy_pair_t *pairs;
    int npairs;
    uint64_t n_accepted, n_refused, n_idle_closed, n_connect_failed;
    uint64_t bytes_up, bytes_down;
};

static JSClassID dyn_pxy_class_id;

static const JSClassDef dyn_pxy_class = {
    "TCPProxy", .finalizer = dyn_res_finalizer,
};

/* ---- pair lifetime ---------------------------------------------------- */

static void pxy_unlink(dyn_pxy_t *p, dyn_pxy_pair_t *pr)
{
    if (pr->gone)
        return;
    if (pr->lprev)
        pr->lprev->lnext = pr->lnext;
    else
        p->pairs = pr->lnext;
    if (pr->lnext)
        pr->lnext->lprev = pr->lprev;
    pr->gone = 1;
    p->npairs--;
}

static void pxy_pair_unref(dyn_pxy_pair_t *pr)
{
    if (--pr->refs == 0)
        free(pr);
}

static void pxy_recv_client(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *udata);
static void pxy_recv_upstream(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *udata);
static void pxy_sent_client(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *udata);
static void pxy_sent_upstream(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *udata);
static void pxy_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, void *udata);

/* Close both halves and drop the list reference. Completions already queued
 * still land; they see cfd/ufd of -1 and do nothing. */
static void pxy_pair_close(dyn_pxy_pair_t *pr)
{
    dyn_pxy_t *p = pr->owner;

    int cfd = pr->cfd, ufd = pr->ufd;

    /* Unlink BEFORE closing. Closing completes queued sends with an error and
       those callbacks re-enter here; `gone` is what stops the recursion, so it
       has to be set before the first close rather than after the last. */
    if (pr->gone)
        return;
    pr->cfd = pr->ufd = -1;
    pxy_unlink(p, pr);
    /* Each armed recv holds a reference for as long as it is registered, and
       dyn_aio_close tears an armed recv down WITHOUT delivering its completion
       (only sends get one), so its ref would strand here -- every closed pair
       leaked refs >= 2 and never freed. Release each still-registered recv's
       ref via dyn_aio_cancel, whose >0 return is the authoritative test: a
       recv already released by a pause, or one an arm-failure never placed, is
       not released twice. A still-pending connect is the same shape: close
       never completes it either, so a deadline/dispose close must cancel it or
       its ref strands too. */
    if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0) pr->refs--;
    if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0) pr->refs--;
    if (dyn_aio_cancel(p->aio, pxy_on_connect, pr) > 0) pr->refs--;
    if (cfd >= 0) dyn_aio_close(p->aio, cfd);
    if (ufd >= 0) dyn_aio_close(p->aio, ufd);
    pxy_pair_unref(pr);
}

/* ---- forwarding ------------------------------------------------------- */

/* A FIN on one side closes only that direction: the peer may still have a
 * reply in flight. Tearing the pair down here is the classic L4 bug that
 * truncates responses to a client that shuts down its write half. */
static void pxy_half_close(dyn_pxy_pair_t *pr, int from_client)
{
    if (from_client) {
        pr->c_eof = 1;
        if (pr->ufd >= 0)
            shutdown(pr->ufd, SHUT_WR);
    } else {
        pr->u_eof = 1;
        if (pr->cfd >= 0)
            shutdown(pr->cfd, SHUT_WR);
    }
    if (pr->c_eof && pr->u_eof)
        pxy_pair_close(pr);
}

static void pxy_forward(dyn_pxy_pair_t *pr, const uint8_t *buf, size_t len,
                        int to_upstream)
{
    dyn_pxy_t *p = pr->owner;
    int dst = to_upstream ? pr->ufd : pr->cfd;

    if (dst < 0 || len == 0)
        return;
    /* Charge the bytes BEFORE the send. dyn_aio_send completes INLINE whenever
       the socket takes everything, running the drain -- which subtracts -- so
       adding afterwards makes the counter only ever grow and the proxy pauses
       for good the first time it crosses the mark. */
    if (to_upstream) { pr->to_u += len; p->bytes_up += len; }
    else             { pr->to_c += len; p->bytes_down += len; }

    /* dyn_aio_send copies what it cannot put out inline, so `buf` (a borrowed
       pool view) is safe to hand over without a copy of our own. Take the
       reference first: an inline completion unrefs before this returns. */
    pr->refs++;
    if (dyn_aio_send(p->aio, dst, buf, len, 0,
                     to_upstream ? pxy_sent_upstream : pxy_sent_client, pr) < 0) {
        pxy_pair_unref(pr);
        pxy_pair_close(pr);
        return;
    }
    /* Pause the SOURCE, not the destination: the client is the one producing
       faster than upstream drains. Cancel does not deliver a completion, so
       the recv's reference is released here. */
    if (to_upstream) {
        if (pr->to_u >= PXY_HIGH_WATER && !pr->c_paused && pr->cfd >= 0) {
            if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0) {
                pr->c_paused = 1;
                pr->refs--;
            }
        }
    } else {
        if (pr->to_c >= PXY_HIGH_WATER && !pr->u_paused && pr->ufd >= 0) {
            if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0) {
                pr->u_paused = 1;
                pr->refs--;
            }
        }
    }
    pr->last_ms = dyn_timer_now_ms();  /* progress, not arrival */
}

static void pxy_drained(dyn_pxy_pair_t *pr, int res, int to_upstream)
{
    dyn_pxy_t *p = pr->owner;
    size_t done = res > 0 ? (size_t)res : 0;

    if (to_upstream) {
        pr->to_u = pr->to_u > done ? pr->to_u - done : 0;
        if (pr->c_paused && pr->to_u <= PXY_LOW_WATER && pr->cfd >= 0) {
            pr->c_paused = 0;
            pr->refs++;
            if (dyn_aio_recv(p->aio, pr->cfd, 0, 1, pxy_recv_client, pr) < 0) {
                pr->refs--;
                pxy_pair_close(pr);
            }
        }
    } else {
        pr->to_c = pr->to_c > done ? pr->to_c - done : 0;
        if (pr->u_paused && pr->to_c <= PXY_LOW_WATER && pr->ufd >= 0) {
            pr->u_paused = 0;
            pr->refs++;
            if (dyn_aio_recv(p->aio, pr->ufd, 0, 1, pxy_recv_upstream, pr) < 0) {
                pr->refs--;
                pxy_pair_close(pr);
            }
        }
    }
    if (res < 0)
        pxy_pair_close(pr);
    else
        pr->last_ms = dyn_timer_now_ms();
}

static void pxy_sent_upstream(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *udata)
{
    dyn_pxy_pair_t *pr = (dyn_pxy_pair_t *)udata;
    (void)aio; (void)buf; (void)len;
    pxy_drained(pr, res, 1);
    pxy_pair_unref(pr);
}

static void pxy_sent_client(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *udata)
{
    dyn_pxy_pair_t *pr = (dyn_pxy_pair_t *)udata;
    (void)aio; (void)buf; (void)len;
    pxy_drained(pr, res, 0);
    pxy_pair_unref(pr);
}

static void pxy_recv_client(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *udata)
{
    dyn_pxy_pair_t *pr = (dyn_pxy_pair_t *)udata;
    (void)aio;
    if (pr->cfd < 0)
        return;
    if (res == 0) { pxy_half_close(pr, 1); return; }
    if (res < 0)  { pxy_pair_close(pr); return; }
    pxy_forward(pr, buf, len, 1);
}

static void pxy_recv_upstream(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *udata)
{
    dyn_pxy_pair_t *pr = (dyn_pxy_pair_t *)udata;
    (void)aio;
    if (pr->ufd < 0)
        return;
    if (res == 0) { pxy_half_close(pr, 0); return; }
    if (res < 0)  { pxy_pair_close(pr); return; }
    pxy_forward(pr, buf, len, 0);
}

/* ---- connect / accept -------------------------------------------------- */

static void pxy_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, void *udata)
{
    dyn_pxy_pair_t *pr = (dyn_pxy_pair_t *)udata;
    dyn_pxy_t *p = pr->owner;
    (void)aio; (void)buf; (void)len;

    if (res < 0 || pr->cfd < 0) {
        p->n_connect_failed++;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
        return;
    }
    pr->connected = 1;
    pr->deadline_ms = 0;
    pr->last_ms = dyn_timer_now_ms();
    /* one ref per recv arm, taken before arming (a recv can complete inline);
       released in pxy_pair_close, which cancels whatever is still registered */
    pr->refs += 2;
    if (dyn_aio_recv(p->aio, pr->cfd, 0, 1, pxy_recv_client, pr) < 0) {
        pr->refs -= 2;                 /* neither arm was placed */
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
        return;
    }
    if (dyn_aio_recv(p->aio, pr->ufd, 0, 1, pxy_recv_upstream, pr) < 0) {
        pr->refs -= 1;                 /* the client arm holds the other ref */
        pxy_pair_close(pr);            /* close cancels the armed client recv */
        pxy_pair_unref(pr);
        return;
    }
    pxy_pair_unref(pr);                /* the connect's own reference */
}

static void pxy_on_accept(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *udata)
{
    dyn_pxy_t *p = (dyn_pxy_t *)udata;
    dyn_pxy_pair_t *pr;
    const char *host;
    uint16_t port;
    (void)buf; (void)len;

    if (res < 0)
        return;
    if (p->max_conns && p->npairs >= p->max_conns) {
        p->n_refused++;
        dyn_aio_close(aio, res);
        return;
    }
    pr = (dyn_pxy_pair_t *)calloc(1, sizeof(*pr));
    if (!pr) { dyn_aio_close(aio, res); return; }
    pr->owner = p;
    pr->cfd = res;
    pr->ufd = -1;
    pr->refs = 1;                      /* the live list */
    pr->last_ms = dyn_timer_now_ms();
    if (p->connect_ms)
        pr->deadline_ms = pr->last_ms + p->connect_ms;

    pr->lnext = p->pairs;
    if (p->pairs)
        p->pairs->lprev = pr;
    p->pairs = pr;
    p->npairs++;
    p->n_accepted++;

    host = p->up_host[p->next_up];
    port = p->up_port[p->next_up];
    p->next_up = (p->next_up + 1) % p->n_up;

    pr->refs++;                        /* the pending connect */
    pr->ufd = dyn_aio_connect(p->aio, host, port, pxy_on_connect, pr);
    if (pr->ufd < 0) {
        p->n_connect_failed++;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
    }
}

/* ---- sweep ------------------------------------------------------------- */

/* Runs on the shared reactor's drain hook. Both deadlines are checked here so
 * a quiet proxy -- the one an idle timeout exists for -- still runs them. */
static void pxy_sweep(void *udata)
{
    dyn_pxy_t *p = (dyn_pxy_t *)udata;
    uint64_t now = dyn_timer_now_ms();
    dyn_pxy_pair_t *pr = p->pairs, *next;

    while (pr) {
        next = pr->lnext;
        if (pr->deadline_ms && now >= pr->deadline_ms) {
            p->n_connect_failed++;
            pxy_pair_close(pr);
        } else if (p->idle_ms && pr->connected &&
                   now - pr->last_ms >= p->idle_ms) {
            p->n_idle_closed++;
            pxy_pair_close(pr);
        }
        pr = next;
    }
}

static int pxy_arm_sweep(dyn_pxy_t *p)
{
    if (p->hooked || (!p->idle_ms && !p->connect_ms))
        return 0;
    if (dyn_net_on_drain(pxy_sweep, p) < 0)
        return -1;
    p->hooked = 1;
    return 0;
}

/* ---- JS surface -------------------------------------------------------- */

static void dyn_pxy_dispose(void *native)
{
    dyn_pxy_t *p = (dyn_pxy_t *)native;
    int i;

    while (p->pairs)
        pxy_pair_close(p->pairs);
    if (p->hooked)
        dyn_net_off_drain(p);
    if (p->aio) {
        if (p->listen_fd >= 0)
            dyn_aio_close(p->aio, p->listen_fd);
        dyn_net_reactor_release(p->ctx);
    }
    for (i = 0; i < p->n_up; i++)
        free(p->up_host[i]);
    free(p->up_host);
    free(p->up_port);
    free(p);
}

/* One upstream entry: {host, port}. Coerced to C locals before anything else
 * touches the proxy -- coercion runs arbitrary user code. */
static int pxy_read_upstream(JSContext *ctx, dyn_pxy_t *p, JSValueConst o,
                             int idx)
{
    JSValue jh = JS_GetPropertyStr(ctx, o, "host");
    JSValue jp = JS_GetPropertyStr(ctx, o, "port");
    const char *h;
    int32_t port = 0;

    if (JS_IsException(jh) || JS_IsException(jp)) {
        JS_FreeValue(ctx, jh); JS_FreeValue(ctx, jp);
        return -1;
    }
    h = JS_IsUndefined(jh) ? NULL : JS_ToCString(ctx, jh);
    JS_FreeValue(ctx, jh);
    if (JS_ToInt32(ctx, &port, jp) < 0) {
        JS_FreeValue(ctx, jp);
        if (h) JS_FreeCString(ctx, h);
        return -1;
    }
    JS_FreeValue(ctx, jp);
    if (port <= 0 || port > 65535) {
        if (h) JS_FreeCString(ctx, h);
        JS_ThrowRangeError(ctx, "upstream port out of range");
        return -1;
    }
    p->up_host[idx] = strdup(h ? h : "127.0.0.1");
    if (h) JS_FreeCString(ctx, h);
    if (!p->up_host[idx])
        return -1;
    p->up_port[idx] = (uint16_t)port;
    return 0;
}

static int pxy_read_opts(JSContext *ctx, dyn_pxy_t *p, JSValueConst o)
{
    JSValue v;
    int32_t n;
    uint32_t i, cnt;
    JSValue up;

    v = JS_GetPropertyStr(ctx, o, "port");
    if (JS_IsException(v)) return -1;
    if (JS_ToInt32(ctx, &n, v) < 0) { JS_FreeValue(ctx, v); return -1; }
    JS_FreeValue(ctx, v);
    if (n < 0 || n > 65535) {
        JS_ThrowRangeError(ctx, "listen port out of range");
        return -1;
    }
    p->port = (uint16_t)n;

    v = JS_GetPropertyStr(ctx, o, "maxConns");
    if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &n, v) == 0 && n >= 0)
        p->max_conns = n;
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, o, "idleTimeoutMs");
    if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &n, v) == 0 && n > 0)
        p->idle_ms = (uint64_t)n;
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, o, "connectTimeoutMs");
    if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &n, v) == 0 && n > 0)
        p->connect_ms = (uint64_t)n;
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
            JS_FreeValue(ctx, jl); JS_FreeValue(ctx, up); return -1;
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
    p->up_host = (char **)calloc(cnt, sizeof(char *));
    p->up_port = (uint16_t *)calloc(cnt, sizeof(uint16_t));
    if (!p->up_host || !p->up_port) {
        JS_FreeValue(ctx, up);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < cnt; i++) {
        JSValue e = JS_IsArray(ctx, up) ? JS_GetPropertyUint32(ctx, up, i)
                                        : JS_DupValue(ctx, up);
        int rc;
        if (JS_IsException(e)) { JS_FreeValue(ctx, up); return -1; }
        rc = pxy_read_upstream(ctx, p, e, (int)i);
        JS_FreeValue(ctx, e);
        if (rc < 0) { JS_FreeValue(ctx, up); return -1; }
        p->n_up = (int)i + 1;
    }
    JS_FreeValue(ctx, up);
    return 0;
}

static JSValue dyn_pxy_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    dyn_pxy_t *p;
    (void)new_target;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "TCPProxy(options) needs an object");
    p = (dyn_pxy_t *)calloc(1, sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->ctx = ctx;
    p->listen_fd = -1;
    p->aio = dyn_net_reactor_acquire(ctx);
    if (!p->aio) {
        free(p);
        return JS_ThrowInternalError(ctx, "no reactor");
    }
    if (pxy_read_opts(ctx, p, argv[0]) < 0) {
        dyn_pxy_dispose(p);
        return JS_EXCEPTION;
    }
    return dyn_res_wrap(ctx, dyn_pxy_class_id, p, dyn_pxy_dispose);
}

static JSValue dyn_pxy_start(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_pxy_t *p = (dyn_pxy_t *)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    if (p->started)
        return JS_ThrowInternalError(ctx, "already started");
    p->listen_fd = dyn_aio_listen(p->aio, "0.0.0.0", p->port, 512);
    if (p->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "listen failed");
    if (pxy_arm_sweep(p) < 0) {
        dyn_aio_close(p->aio, p->listen_fd);
        p->listen_fd = -1;
        return JS_ThrowInternalError(ctx, "cannot arm the timeout sweep");
    }
    /* Resolve an OS-assigned port so `port: 0` is usable, as TCPServer does. */
    {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        if (getsockname(p->listen_fd, (struct sockaddr *)&sa, &sl) == 0)
            p->port = ntohs(sa.sin_port);
    }
    if (dyn_aio_accept(p->aio, p->listen_fd, pxy_on_accept, p) < 0) {
        dyn_aio_close(p->aio, p->listen_fd);
        p->listen_fd = -1;
        return JS_ThrowInternalError(ctx, "accept failed");
    }
    p->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_pxy_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_pxy_t *p = (dyn_pxy_t *)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->port);
}

static JSValue dyn_pxy_stats(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_pxy_t *p = (dyn_pxy_t *)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    JSValue o;
    (void)argc; (void)argv;

    if (!p)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "live", JS_NewInt32(ctx, p->npairs));
    JS_SetPropertyStr(ctx, o, "accepted", JS_NewInt64(ctx, (int64_t)p->n_accepted));
    JS_SetPropertyStr(ctx, o, "refused", JS_NewInt64(ctx, (int64_t)p->n_refused));
    JS_SetPropertyStr(ctx, o, "idleClosed",
                      JS_NewInt64(ctx, (int64_t)p->n_idle_closed));
    JS_SetPropertyStr(ctx, o, "connectFailed",
                      JS_NewInt64(ctx, (int64_t)p->n_connect_failed));
    JS_SetPropertyStr(ctx, o, "bytesUp", JS_NewInt64(ctx, (int64_t)p->bytes_up));
    JS_SetPropertyStr(ctx, o, "bytesDown",
                      JS_NewInt64(ctx, (int64_t)p->bytes_down));
    return o;
}

static const JSCFunctionListEntry dyn_pxy_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_pxy_start),
    JS_CFUNC_DEF("stats", 0, dyn_pxy_stats),
    JS_CGETSET_DEF("port", dyn_pxy_get_port, NULL),
};

int dyn_proxy_register(JSContext *ctx, JSModuleDef *m)
{
    return dyn_register_class(ctx, m, &dyn_pxy_class_id, &dyn_pxy_class,
                              dyn_pxy_proto, countof(dyn_pxy_proto),
                              dyn_pxy_ctor, "TCPProxy");
}

void dyn_proxy_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "TCPProxy");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
