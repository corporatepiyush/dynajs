/*
 * dyna:net -- the networking module: addresses, HTTP, and (later) TCP/UDP/IPC,
 * DNS and the protocol clients.
 *
 * Registration point only. Each capability keeps its own translation unit and
 * registers here through a dyn_<x>_register / dyn_<x>_add_exports pair -- the
 * same seam dyna-serialize.c and dyna-graph.c use for dyna:structures.
 */
#include "dyna-nat.h"

#include <stdlib.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include "dyna-aio.h"

/* from dyna-libc.h: fold a reactor into the JS event loop */
void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata);

/* ONE reactor per JS thread, shared by every dyna:net object.
 *
 * js_std_set_io_reactor holds a SINGLE fd, so a server and a client that each
 * owned a reactor silently overwrote each other and neither was ever drained.
 * Thread-local rather than global because every JS Worker has its own runtime
 * and its own loop -- which is exactly the "one dyn_aio per JS thread" the
 * adapter already documents. Refcounted so the last user tears it down. */
static _Thread_local dyn_aio_t *net_aio;
static _Thread_local int net_aio_refs;

/* Drain hooks. A registrant whose drain must do MORE than reap the reactor --
 * App runs its idle-timeout sweep there -- adds one here instead of installing
 * its own reactor, which would take the single slot back. */
/* GROWS. A fixed table silently refuses the ninth registrant, and the caller
 * that gets -1 has no way to carry on -- its timeouts simply never fire, which
 * looks like a bug in the timeout rather than a full table. */
typedef struct { void (*fn)(void *); void *udata; } net_hook_t;
static _Thread_local net_hook_t *net_hooks;
static _Thread_local int net_n_hooks, net_cap_hooks;

/* A release that lands INSIDE a drain is deferred to the end of it. Freeing
 * the shared reactor from a completion callback is a use-after-free of the
 * reactor itself: dyn_aio_drain reads a->chan on the line after the poll that
 * dispatched the callback. Reached by `close(){ server.close() }`, and found
 * by ASan, not by reasoning. */
static _Thread_local int net_draining;
static _Thread_local int net_release_deferred;
static _Thread_local JSContext *net_ctx;

static void net_reactor_free(void)
{
    if (!net_aio)
        return;
    if (net_ctx)
        js_std_set_io_reactor(net_ctx, -1, NULL, NULL);
    dyn_aio_free(net_aio);
    net_aio = NULL;
    net_aio_refs = 0;
    net_n_hooks = 0;
    net_release_deferred = 0;
}

static void dyn_net_drain(void *udata)
{
    int i;
    net_draining++;
    dyn_aio_drain(udata);
    /* Snapshot the count: a hook may release the reactor and unregister
     * itself, and the array must not shift under the loop. */
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn)
            net_hooks[i].fn(net_hooks[i].udata);
    if (--net_draining == 0 && net_release_deferred)
        net_reactor_free();
}

/* Every hook is time-driven work, so registering one arms the backend's
 * periodic wakeup. ONE tick owned here, rather than each registrant calling
 * dyn_aio_set_timer and the last writer silently winning. */
#define NET_TICK_MS 250

int dyn_net_on_drain(void (*fn)(void *), void *udata)
{
    if (net_n_hooks == net_cap_hooks) {
        int cap = net_cap_hooks ? net_cap_hooks * 2 : 8;
        net_hook_t *n = (net_hook_t *)realloc(net_hooks, (size_t)cap * sizeof(*n));
        if (!n)
            return -1;
        net_hooks = n;
        net_cap_hooks = cap;
    }
    /* Do NOT swallow this. A hook is time-driven work; a backend that cannot
       arm a clock gives you a hook that runs only when other traffic wakes the
       loop, which is precisely the quiet server the timeouts exist for. Every
       caller already checks this return -- it just always said 0. */
    if (net_n_hooks == 0 && net_aio && dyn_aio_set_timer(net_aio, NET_TICK_MS) < 0)
        return -2;              /* distinct from -1 so the caller can say why */
    net_hooks[net_n_hooks].fn = fn;
    net_hooks[net_n_hooks].udata = udata;
    net_n_hooks++;
    return 0;
}

void dyn_net_off_drain(void *udata)
{
    int i;
    for (i = 0; i < net_n_hooks; i++) {
        if (net_hooks[i].udata == udata) {
            net_hooks[i] = net_hooks[net_n_hooks - 1];
            net_n_hooks--;
            if (net_n_hooks == 0) {
                free(net_hooks);
                net_hooks = NULL;
                net_cap_hooks = 0;
            }
            return;
        }
    }
}

dyn_aio_t *dyn_net_reactor_acquire(JSContext *ctx)
{
    net_ctx = ctx;
    if (!net_aio) {
        net_aio = dyn_aio_new(4096, 0);
        if (!net_aio)
            return NULL;
        js_std_set_io_reactor(ctx, dyn_aio_backend_fd(net_aio), dyn_net_drain,
                              net_aio);
    }
    net_aio_refs++;
    return net_aio;
}

void dyn_net_reactor_release(JSContext *ctx)
{
    if (net_aio && --net_aio_refs <= 0) {
        net_ctx = ctx;
        if (net_draining) {     /* a completion callback is on the stack */
            net_release_deferred = 1;
            return;
        }
        net_reactor_free();
    }
}

static int dyn_net_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_netip_register(ctx, m) < 0)
        return -1;
    if (dyn_http_register(ctx, m) < 0)
        return -1;
    if (dyn_tcp_register(ctx, m) < 0)
        return -1;
    if (dyn_proxy_register(ctx, m) < 0)
        return -1;
    if (dyn_dns_register(ctx, m) < 0)
        return -1;
    if (dyn_ratelimit_register(ctx, m) < 0)
        return -1;
    if (dyn_metrics_register(ctx, m) < 0)
        return -1;
    if (dyn_redis_register(ctx, m) < 0)
        return -1;
    if (dyn_pg_register(ctx, m) < 0)
        return -1;
#ifdef CONFIG_SQLITE
    if (dyn_sqlite_register(ctx, m) < 0)
        return -1;
#endif
    return 0;
}

int js_nat_init_net(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:net", dyn_net_init_module);
    if (!m)
        return -1;
    if (dyn_netip_add_exports(ctx, m) < 0)
        return -1;
    dyn_http_add_exports(ctx, m);
    dyn_tcp_add_exports(ctx, m);
    dyn_proxy_add_exports(ctx, m);
    dyn_dns_add_exports(ctx, m);
    dyn_ratelimit_add_exports(ctx, m);
    dyn_metrics_add_exports(ctx, m);
    dyn_redis_add_exports(ctx, m);
    dyn_pg_add_exports(ctx, m);
#ifdef CONFIG_SQLITE
    dyn_sqlite_add_exports(ctx, m);
#endif
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
