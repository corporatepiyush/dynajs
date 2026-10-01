#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

static const char* const pxy_ctor_keys[] = { "port", "maxConns", "idleTimeoutMs", "connectTimeoutMs", "upstream" };
static const char* const pxy_upstream_keys[] = { "host", "port" };

dyn_aio_t* dyn_net_reactor_acquire(JSContext* ctx);
void dyn_net_reactor_release(JSContext* ctx);
void dyn_net_reactor_release_rt(JSRuntime* rt);
int dyn_net_on_drain(void (*fn)(void*), void* udata);
void dyn_net_off_drain(void* udata);

#define PXY_HIGH_WATER (256 * 1024)
#define PXY_LOW_WATER (64 * 1024)

typedef struct dyn_pxy dyn_pxy_t;

typedef struct dyn_pxy_pair {
    dyn_pxy_t* owner;
    int cfd, ufd;
    int refs;
    unsigned c_eof : 1;
    unsigned u_eof : 1;
    unsigned c_paused : 1;
    unsigned u_paused : 1;
    unsigned connected : 1;
    unsigned gone : 1;
    size_t to_c, to_u;
    uint64_t last_ms;
    uint64_t deadline_ms;
    struct dyn_pxy_pair *lnext, *lprev;
} dyn_pxy_pair_t;

struct dyn_pxy {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    int started;
    int hooked;

    char** up_host;
    uint16_t* up_port;
    struct sockaddr_storage* up_sa;
    socklen_t* up_salen;
    int* up_fam;
    int n_up, next_up;

    int max_conns;
    uint64_t idle_ms;
    uint64_t connect_ms;

    dyn_pxy_pair_t* pairs;
    int npairs;
    uint64_t n_accepted, n_refused, n_idle_closed, n_connect_failed;
    uint64_t bytes_up, bytes_down;
};

static JSClassID dyn_pxy_class_id;

static const JSClassDef dyn_pxy_class = {
    "TCPProxy",
    .finalizer = dyn_res_finalizer,
};

static void pxy_unlink(dyn_pxy_t* p, dyn_pxy_pair_t* pr)
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

static void pxy_pair_unref(dyn_pxy_pair_t* pr)
{
    if (--pr->refs == 0)
        free(pr);
}

static void pxy_recv_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);
static void pxy_recv_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);
static void pxy_sent_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);
static void pxy_sent_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);
static void pxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

static void pxy_pair_close(dyn_pxy_pair_t* pr)
{
    dyn_pxy_t* p = pr->owner;

    int cfd = pr->cfd, ufd = pr->ufd;

    if (pr->gone)
        return;
    pr->cfd = pr->ufd = -1;
    pxy_unlink(p, pr);
    if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0)
        pr->refs--;
    if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0)
        pr->refs--;
    if (dyn_aio_cancel(p->aio, pxy_on_connect, pr) > 0)
        pr->refs--;
    if (cfd >= 0)
        dyn_aio_close(p->aio, cfd);
    if (ufd >= 0)
        dyn_aio_close(p->aio, ufd);
    pxy_pair_unref(pr);
}

static void pxy_half_close(dyn_pxy_pair_t* pr, int from_client)
{
    dyn_pxy_t* p = pr->owner;

    if (from_client) {
        pr->c_eof = 1;
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0)
            pr->refs--;
        if (pr->ufd >= 0)
            shutdown(pr->ufd, SHUT_WR);
    } else {
        pr->u_eof = 1;
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0)
            pr->refs--;
        if (pr->cfd >= 0)
            shutdown(pr->cfd, SHUT_WR);
    }
    if (pr->c_eof && pr->u_eof)
        pxy_pair_close(pr);
}

static void pxy_forward(dyn_pxy_pair_t* pr, const uint8_t* buf, size_t len,
    int to_upstream)
{
    dyn_pxy_t* p = pr->owner;
    int dst = to_upstream ? pr->ufd : pr->cfd;

    if (dst < 0 || len == 0)
        return;
    if (to_upstream) {
        pr->to_u += len;
        p->bytes_up += len;
    } else {
        pr->to_c += len;
        p->bytes_down += len;
    }

    pr->refs++;
    if (dyn_aio_send(p->aio, dst, buf, len, 0,
            to_upstream ? pxy_sent_upstream : pxy_sent_client, pr)
        < 0) {
        pxy_pair_unref(pr);
        pxy_pair_close(pr);
        return;
    }
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
    pr->last_ms = dyn_timer_now_ms();
}

static void pxy_drained(dyn_pxy_pair_t* pr, int res, int to_upstream)
{
    dyn_pxy_t* p = pr->owner;
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

static void pxy_sent_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;
    pxy_drained(pr, res, 1);
    pxy_pair_unref(pr);
}

static void pxy_sent_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;
    pxy_drained(pr, res, 0);
    pxy_pair_unref(pr);
}

static void pxy_recv_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    dyn_pxy_t* p = pr->owner;
    (void)aio;
    if (pr->cfd < 0)
        return;
    if (res == 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) == 0)
            pr->refs--;
        pxy_half_close(pr, 1);
        return;
    }
    if (res < 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) == 0)
            pr->refs--;
        pxy_pair_close(pr);
        return;
    }
    pxy_forward(pr, buf, len, 1);
}

static void pxy_recv_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    dyn_pxy_t* p = pr->owner;
    (void)aio;
    if (pr->ufd < 0)
        return;
    if (res == 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) == 0)
            pr->refs--;
        pxy_half_close(pr, 0);
        return;
    }
    if (res < 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) == 0)
            pr->refs--;
        pxy_pair_close(pr);
        return;
    }
    pxy_forward(pr, buf, len, 0);
}

static void pxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    dyn_pxy_t* p = pr->owner;
    (void)aio;
    (void)buf;
    (void)len;

    if (res < 0 || pr->cfd < 0) {
        p->n_connect_failed++;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
        return;
    }
    pr->connected = 1;
    pr->deadline_ms = 0;
    pr->last_ms = dyn_timer_now_ms();
    pr->refs += 2;
    if (dyn_aio_recv(p->aio, pr->cfd, 0, 1, pxy_recv_client, pr) < 0) {
        pr->refs -= 2;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
        return;
    }
    if (dyn_aio_recv(p->aio, pr->ufd, 0, 1, pxy_recv_upstream, pr) < 0) {
        pr->refs -= 1;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
        return;
    }
    pxy_pair_unref(pr);
}

static void pxy_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_t* p = (dyn_pxy_t*)udata;
    dyn_pxy_pair_t* pr;
    int idx2;
    (void)buf;
    (void)len;

    if (res < 0)
        return;
    if (p->max_conns && p->npairs >= p->max_conns) {
        p->n_refused++;
        dyn_aio_close(aio, res);
        return;
    }
    pr = (dyn_pxy_pair_t*)calloc(1, sizeof(*pr));
    if (!pr) {
        dyn_aio_close(aio, res);
        return;
    }
    pr->owner = p;
    pr->cfd = res;
    pr->ufd = -1;
    pr->refs = 1;
    pr->last_ms = dyn_timer_now_ms();
    if (p->connect_ms)
        pr->deadline_ms = pr->last_ms + p->connect_ms;

    pr->lnext = p->pairs;
    if (p->pairs)
        p->pairs->lprev = pr;
    p->pairs = pr;
    p->npairs++;
    p->n_accepted++;

    idx2 = p->next_up;
    p->next_up = (p->next_up + 1) % p->n_up;

    pr->refs++;
    pr->ufd = dyn_aio_connect_addr(p->aio, &p->up_sa[idx2], p->up_salen[idx2],
        p->up_fam[idx2], pxy_on_connect, pr);
    if (pr->ufd < 0) {
        p->n_connect_failed++;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
    }
}

static void pxy_sweep(void* udata)
{
    dyn_pxy_t* p = (dyn_pxy_t*)udata;
    uint64_t now = dyn_timer_now_ms();
    dyn_pxy_pair_t *pr = p->pairs, *next;

    while (pr) {
        next = pr->lnext;
        if (pr->deadline_ms && now >= pr->deadline_ms) {
            p->n_connect_failed++;
            pxy_pair_close(pr);
        } else if (p->idle_ms && pr->connected && now - pr->last_ms >= p->idle_ms) {
            p->n_idle_closed++;
            pxy_pair_close(pr);
        }
        pr = next;
    }
}

static int pxy_arm_sweep(dyn_pxy_t* p)
{
    if (p->hooked || (!p->idle_ms && !p->connect_ms))
        return 0;
    if (dyn_net_on_drain(pxy_sweep, p) < 0)
        return -1;
    p->hooked = 1;
    return 0;
}

static void dyn_pxy_dispose(void* native)
{
    dyn_pxy_t* p = (dyn_pxy_t*)native;
    int i;

    while (p->pairs)
        pxy_pair_close(p->pairs);
    if (p->hooked)
        dyn_net_off_drain(p);
    if (p->aio) {
        if (p->listen_fd >= 0)
            dyn_aio_close(p->aio, p->listen_fd);
        dyn_net_reactor_release_rt(p->rt);
    }
    for (i = 0; i < p->n_up; i++)
        free(p->up_host[i]);
    free(p->up_host);
    free(p->up_port);
    free(p->up_sa);
    free(p->up_salen);
    free(p->up_fam);
    free(p);
}

static int pxy_read_upstream(JSContext* ctx, dyn_pxy_t* p, JSValueConst o,
    int idx)
{
    JSValue jh, jp;
    const char* h;
    int32_t port = 0;

    if (dyn_opts_strict(ctx, o, pxy_upstream_keys, 2))
        return -1;
    jh = JS_GetPropertyStr(ctx, o, "host");
    jp = JS_GetPropertyStr(ctx, o, "port");

    if (JS_IsException(jh) || JS_IsException(jp)) {
        JS_FreeValue(ctx, jh);
        JS_FreeValue(ctx, jp);
        return -1;
    }
    h = JS_IsUndefined(jh) ? NULL : JS_ToCString(ctx, jh);
    JS_FreeValue(ctx, jh);
    if (JS_ToInt32(ctx, &port, jp) < 0) {
        JS_FreeValue(ctx, jp);
        if (h)
            JS_FreeCString(ctx, h);
        return -1;
    }
    JS_FreeValue(ctx, jp);
    if (port <= 0 || port > 65535) {
        if (h)
            JS_FreeCString(ctx, h);
        JS_ThrowRangeError(ctx, "upstream port out of range");
        return -1;
    }
    p->up_host[idx] = strdup(h ? h : "127.0.0.1");
    if (h)
        JS_FreeCString(ctx, h);
    if (!p->up_host[idx])
        return -1;
    p->up_port[idx] = (uint16_t)port;
    {
        char errbuf[160];
        if (dyn_aio_resolve(p->up_host[idx], p->up_port[idx],
                &p->up_sa[idx], &p->up_salen[idx],
                &p->up_fam[idx])
            != 0) {
            snprintf(errbuf, sizeof errbuf, "TCPProxy: cannot resolve upstream '%s'",
                p->up_host[idx]);
            JS_ThrowTypeError(ctx, "%s", errbuf);
            free(p->up_host[idx]);
            p->up_host[idx] = NULL;
            return -1;
        }
    }
    return 0;
}

static int pxy_read_opts(JSContext* ctx, dyn_pxy_t* p, JSValueConst o)
{
    JSValue v;
    int32_t n;
    uint32_t i, cnt;
    JSValue up;

    if (dyn_opts_strict(ctx, o, pxy_ctor_keys, 5))
        return -1;

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
        if (n > 0)
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

static JSValue dyn_pxy_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_pxy_t* p;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "TCPProxy(options) needs an object");
    p = (dyn_pxy_t*)calloc(1, sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->ctx = ctx;
    p->rt = JS_GetRuntime(ctx);
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
    return dyn_res_wrap(ctx, new_target, dyn_pxy_class_id, p, dyn_pxy_dispose);
}

static JSValue dyn_pxy_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pxy_t* p = (dyn_pxy_t*)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    (void)argc;
    (void)argv;

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
    {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        if (getsockname(p->listen_fd, (struct sockaddr*)&sa, &sl) == 0)
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

static JSValue dyn_pxy_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_pxy_t* p = (dyn_pxy_t*)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->port);
}

static JSValue dyn_pxy_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pxy_t* p = (dyn_pxy_t*)dyn_res_native(ctx, this_val, dyn_pxy_class_id);
    JSValue o;
    (void)argc;
    (void)argv;

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

int dyn_proxy_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &dyn_pxy_class_id, &dyn_pxy_class,
        dyn_pxy_proto, countof(dyn_pxy_proto),
        dyn_pxy_ctor, "TCPProxy");
}

void dyn_proxy_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "TCPProxy");
}

#endif
