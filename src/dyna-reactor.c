#include "dyna-nat.h"

#include <stdlib.h>

#if defined(CONFIG_NATIVE_MODULES)

#include "dyna-aio.h"
#include "dyna-libc.h"

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_weak_rt(JSRuntime* rt, int weak);
void js_std_set_io_reactor_rt(JSRuntime* rt, int fd,
    void (*drain)(void* udata), void* udata);
void js_std_set_io_reactor_wait(JSContext* ctx,
    int (*wait)(void* udata, int timeout_ms));
void js_std_set_io_reactor_wait_us(JSContext* ctx,
    int (*wait)(void* udata, int64_t timeout_us));
void js_std_set_io_reactor_watch(JSContext* ctx,
    int (*watch)(void* udata, int fd, void (*ready)(void* arg), void* arg),
    int (*unwatch)(void* udata, int fd));

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
static _Thread_local int net_weak_holds;
static _Thread_local JSRuntime* net_rt;
static _Thread_local void (*net_free_hooks[8])(void);

typedef struct {
    JSRuntime* rt;
    void (*ready)(void* arg);
    void* arg;
} net_reg_t;
#define NET_MAX_RTS 8
static _Thread_local net_reg_t net_regs[NET_MAX_RTS];

static net_reg_t* net_reg_find(const JSRuntime* rt)
{
    int i;
    for (i = 0; i < NET_MAX_RTS; i++)
        if (net_regs[i].rt && net_regs[i].rt == rt)
            return &net_regs[i];
    return NULL;
}

static void net_set_weak(int weak)
{
    int i;
    for (i = 0; i < NET_MAX_RTS; i++)
        if (net_regs[i].rt)
            js_std_set_io_reactor_weak_rt(net_regs[i].rt, weak);
}

static void net_reactor_free(void)
{
    int i;
    if (!net_aio)
        return;
    for (i = 0; i < NET_MAX_RTS; i++) {
        if (net_regs[i].rt)
            js_std_set_io_reactor_rt(net_regs[i].rt, -1, NULL, NULL);
        net_regs[i].rt = NULL;
        net_regs[i].ready = NULL;
        net_regs[i].arg = NULL;
    }
    dyn_aio_free(net_aio);
    net_aio = NULL;
    for (i = 0; i < 8 && net_free_hooks[i]; i++)
        net_free_hooks[i]();
    net_aio_refs = 0;
    free(net_hooks);
    net_hooks = NULL;
    net_n_hooks = 0;
    net_cap_hooks = 0;
    net_release_deferred = 0;
}

int dyn_net_on_reactor_free(void (*fn)(void))
{
    int i;
    for (i = 0; i < 8; i++) {
        if (net_free_hooks[i] == fn)
            return 0;
        if (!net_free_hooks[i]) {
            net_free_hooks[i] = fn;
            return 0;
        }
    }
    return -1;
}

static void dyn_net_drain(void* udata)
{
    int i;
    if (!net_aio)
        return;
    (void)udata;
    net_draining++;
    dyn_aio_drain(net_aio);
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn)
            net_hooks[i].fn(net_hooks[i].udata);
    if (--net_draining == 0 && net_release_deferred && net_aio_refs <= 0
        && net_weak_holds == 0)
        net_reactor_free();
}

static int dyn_net_wait(void* udata, int64_t timeout_us)
{
    int i;
    if (!net_aio)
        return 0;
    (void)udata;
    net_draining++;
    (void)dyn_aio_run_us(net_aio, timeout_us);
    for (i = 0; i < net_n_hooks; i++)
        if (net_hooks[i].fn)
            net_hooks[i].fn(net_hooks[i].udata);
    if (--net_draining == 0 && net_release_deferred && net_aio_refs <= 0
        && net_weak_holds == 0)
        net_reactor_free();
    return 0;
}

static void dyn_net_watch_fired(dyn_aio_t* aio, int fd, void* ud)
{
    net_reg_t* r = ud;
    (void)aio;
    (void)fd;
    if (r->rt && r->ready)
        r->ready(r->arg);
}

static int dyn_net_watch(void* udata, int fd, void (*ready)(void* arg), void* arg)
{
    net_reg_t* r = net_reg_find(udata);
    if (!r || !net_aio)
        return -1;
    r->ready = ready;
    r->arg = arg;
    return dyn_aio_watch_fd(net_aio, fd, dyn_net_watch_fired, r);
}

static int dyn_net_unwatch(void* udata, int fd)
{
    net_reg_t* r = net_reg_find(udata);
    if (r) {
        r->ready = NULL;
        r->arg = NULL;
    }
    return net_aio ? dyn_aio_unwatch_fd(net_aio, fd) : -1;
}

static void dyn_net_detach(JSRuntime* rt)
{
    net_reg_t* r = net_reg_find(rt);
    int i;
    if (r) {
        r->rt = NULL;
        r->ready = NULL;
        r->arg = NULL;
    }
    if (net_rt == rt) {
        net_rt = NULL;
        for (i = 0; i < NET_MAX_RTS && !net_rt; i++)
            net_rt = net_regs[i].rt;
    }
    js_std_set_io_reactor_rt(rt, -1, NULL, NULL);
}

static int net_register(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    int i;
    if (!JS_GetRuntimeOpaque(rt) || net_reg_find(rt))
        return 0;
    for (i = 0; i < NET_MAX_RTS && net_regs[i].rt; i++) {
    }
    if (i == NET_MAX_RTS)
        return -1;
    net_regs[i].rt = rt;
    net_regs[i].ready = NULL;
    net_regs[i].arg = NULL;
    js_std_set_io_reactor(ctx, dyn_aio_backend_fd(net_aio), dyn_net_drain, rt);
    js_std_set_io_reactor_wait_us(ctx, dyn_net_wait);
    js_std_set_io_reactor_watch(ctx, dyn_net_watch, dyn_net_unwatch);
    js_std_set_io_reactor_detach(ctx, dyn_net_detach);
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

static void dyn_net_after_dispatch(dyn_aio_t* aio)
{
    JSRuntime* rt = net_rt;
    JSContext* jc = NULL;
    int err;
    (void)aio;
    if (!rt)
        return;
    if (js_std_job_error_pending_rt(rt))
        return;
    while ((err = JS_ExecutePendingJob(rt, &jc)) > 0) {
    }
    if (err < 0)
        js_std_set_job_error_rt(rt, jc ? JS_GetException(jc) : JS_UNINITIALIZED);
}

dyn_aio_t* dyn_net_reactor_acquire(JSContext* ctx)
{
    if (!net_aio) {
        net_aio = dyn_aio_new(4096, 0);
        if (!net_aio)
            return NULL;
        dyn_aio_set_after_dispatch(net_aio, dyn_net_after_dispatch);
    }
    if (net_register(ctx) < 0)
        return NULL;
    net_rt = JS_GetRuntime(ctx);
    net_aio_refs++;
    net_release_deferred = 0;
    net_set_weak(0);
    return net_aio;
}

JSValue dyn_net_throw_no_reactor(JSContext* ctx, const char* who)
{
#ifdef CONFIG_IO_URING
    return JS_ThrowInternalError(ctx,
        "%s: the io reactor could not be created (io_uring is unavailable "
        "or denied here, for example by a seccomp profile)",
        who);
#else
    return JS_ThrowInternalError(ctx, "%s: the io reactor could not be created", who);
#endif
}

void dyn_net_reactor_weak_hold(void)
{
    net_weak_holds++;
}

void dyn_net_reactor_hold(void)
{
    if (!net_aio)
        return;
    net_aio_refs++;
    net_release_deferred = 0;
    net_set_weak(0);
}

void dyn_net_reactor_weak_drop(JSRuntime* rt)
{
    if (net_weak_holds <= 0 || --net_weak_holds > 0)
        return;
    (void)rt;
    if (!net_aio || net_aio_refs > 0)
        return;
    if (net_draining) {
        net_release_deferred = 1;
        return;
    }
    net_reactor_free();
}

void dyn_net_reactor_release_rt(JSRuntime* rt)
{
    (void)rt;
    if (net_aio && --net_aio_refs <= 0) {
        if (net_weak_holds > 0) {
            net_aio_refs = 0;
            net_set_weak(1);
            return;
        }
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

#endif
