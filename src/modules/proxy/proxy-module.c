#include "proxy-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROXY)

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

void pxy_pair_close(dyn_pxy_pair_t* pr)
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

void pxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
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
    free(p->bind_host);
    free(p->up_host);
    free(p->up_port);
    free(p->up_sa);
    free(p->up_salen);
    free(p->up_fam);
    free(p);
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
        return dyn_net_throw_no_reactor(ctx, "TCPProxy");
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
    p->listen_fd = dyn_aio_listen(p->aio, p->bind_host ? p->bind_host : "0.0.0.0", p->port, 512);
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
    if (dyn_res_pin(ctx, this_val, dyn_pxy_class_id) < 0)
        return JS_ThrowOutOfMemory(ctx);
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
    JS_DefinePropertyValueStr(ctx, o, "live", JS_NewInt32(ctx, p->npairs), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "accepted", JS_NewInt64(ctx, (int64_t)p->n_accepted), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "refused", JS_NewInt64(ctx, (int64_t)p->n_refused), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "idleClosed",
        JS_NewInt64(ctx, (int64_t)p->n_idle_closed), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "connectFailed",
        JS_NewInt64(ctx, (int64_t)p->n_connect_failed), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bytesUp", JS_NewInt64(ctx, (int64_t)p->bytes_up), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bytesDown",
        JS_NewInt64(ctx, (int64_t)p->bytes_down), JS_PROP_C_W_E);
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


int js_nat_init_proxy(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:proxy", dyn_proxy_register);
    if (!m)
        return -1;
    dyn_proxy_add_exports(ctx, m);
    return 0;
}

#endif
