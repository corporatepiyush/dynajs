#include "dyna-nat.h"

#include <stdlib.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include "dyna-aio.h"
#include "dyna-fetch.inc.c"

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_rt(JSRuntime* rt, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_wait(JSContext* ctx,
    int (*wait)(void* udata, int timeout_ms));

static _Thread_local dyn_aio_t* net_aio;
static _Thread_local int net_aio_refs;

typedef struct {
    void (*fn)(void*);
    void* udata;
} net_hook_t;
static _Thread_local net_hook_t* net_hooks;
static _Thread_local int net_n_hooks, net_cap_hooks;

static _Thread_local int net_draining;
static _Thread_local int net_release_deferred;
static _Thread_local JSRuntime* net_rt;

static void net_reactor_free(void)
{
    if (!net_aio)
        return;
    if (net_rt)
        js_std_set_io_reactor_rt(net_rt, -1, NULL, NULL);
    dyn_aio_free(net_aio);
    net_aio = NULL;
    net_aio_refs = 0;
    net_n_hooks = 0;
    net_release_deferred = 0;
}

static void dyn_net_drain(void* udata)
{
    int i;
    net_draining++;
    dyn_aio_drain(udata);
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn)
            net_hooks[i].fn(net_hooks[i].udata);
    if (--net_draining == 0 && net_release_deferred)
        net_reactor_free();
}

static int dyn_net_wait(void* udata, int timeout_ms)
{
    int i;
    net_draining++;
    (void)dyn_aio_run(udata, timeout_ms);
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn)
            net_hooks[i].fn(net_hooks[i].udata);
    if (--net_draining == 0 && net_release_deferred)
        net_reactor_free();
    return 0;
}

#define NET_TICK_MS 250

int dyn_net_on_drain(void (*fn)(void*), void* udata)
{
    int i;
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn == fn && net_hooks[i].udata == udata)
            return 0;
    if (net_n_hooks == net_cap_hooks) {
        int cap = net_cap_hooks ? net_cap_hooks * 2 : 8;
        net_hook_t* n = (net_hook_t*)realloc(net_hooks, (size_t)cap * sizeof(*n));
        if (!n)
            return -1;
        net_hooks = n;
        net_cap_hooks = cap;
    }
    if (net_n_hooks == 0 && net_aio && dyn_aio_set_timer(net_aio, NET_TICK_MS) < 0)
        return -2;
    net_hooks[net_n_hooks].fn = fn;
    net_hooks[net_n_hooks].udata = udata;
    net_n_hooks++;
    return 0;
}

int dyn_net_on_drain_notick(void (*fn)(void*), void* udata)
{
    int i;
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn == fn && net_hooks[i].udata == udata)
            return 0;
    if (net_n_hooks == net_cap_hooks) {
        int cap = net_cap_hooks ? net_cap_hooks * 2 : 8;
        net_hook_t* n = (net_hook_t*)realloc(net_hooks, (size_t)cap * sizeof(*n));
        if (!n)
            return -1;
        net_hooks = n;
        net_cap_hooks = cap;
    }
    net_hooks[net_n_hooks].fn = fn;
    net_hooks[net_n_hooks].udata = udata;
    net_n_hooks++;
    return 0;
}

void dyn_net_off_drain(void* udata)
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

dyn_aio_t* dyn_net_reactor_acquire(JSContext* ctx)
{
    net_rt = JS_GetRuntime(ctx);
    if (!net_aio) {
        net_aio = dyn_aio_new(4096, 0);
        if (!net_aio)
            return NULL;
        js_std_set_io_reactor(ctx, dyn_aio_backend_fd(net_aio), dyn_net_drain,
            net_aio);
        js_std_set_io_reactor_wait(ctx, dyn_net_wait);
    }
    net_aio_refs++;
    return net_aio;
}

void dyn_net_reactor_release_rt(JSRuntime* rt)
{
    if (net_aio && --net_aio_refs <= 0) {
        net_rt = rt;
        if (net_draining) {
            net_release_deferred = 1;
            return;
        }
        net_reactor_free();
    }
}

void dyn_net_reactor_release(JSContext* ctx)
{
    dyn_net_reactor_release_rt(JS_GetRuntime(ctx));
}

static int dyn_net_init_module(JSContext* ctx, JSModuleDef* m)
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

static int dyn_http_init_standalone_module(JSContext* ctx, JSModuleDef* m)
{
    return dyn_http_register(ctx, m);
}

static int js_nat_init_http(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:http", dyn_http_init_standalone_module);
    if (!m)
        return -1;
    dyn_http_add_exports(ctx, m);
    return 0;
}

int js_nat_init_net(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    js_fetch_install(ctx, g);
    JS_FreeValue(ctx, g);

    JSModuleDef* m = JS_NewCModule(ctx, "dyna:net", dyn_net_init_module);
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
    if (js_nat_init_http(ctx) < 0)
        return -1;
    return 0;
}

#endif
