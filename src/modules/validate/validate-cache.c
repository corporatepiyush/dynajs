#include "validate-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

typedef struct dyn_v_cache {
    struct dyn_v_cache* next;
    JSContext* ctx;
    const char* module;
    const char* fnname;
    JSValue fn;
} dyn_v_cache_t;

static dyn_v_cache_t* dyn_v_cache_list;

static pthread_mutex_t dyn_v_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static void dyn_v_cache_unlink(dyn_v_cache_t* e)
{
    dyn_v_cache_t** pp = &dyn_v_cache_list;
    while (*pp) {
        if (*pp == e) {
            *pp = e->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void dyn_v_cache_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_v_cache_t* e = (dyn_v_cache_t*)opaque;
    JSValue fn;
    pthread_mutex_lock(&dyn_v_cache_mu);
    dyn_v_cache_unlink(e);
    fn = e->fn;
    e->fn = JS_UNDEFINED;
    pthread_mutex_unlock(&dyn_v_cache_mu);
    if (!JS_IsUndefined(fn)) {
        if (ctx)
            JS_FreeValue(ctx, fn);
        else
            JS_FreeValueRT(rt, fn);
    }
    free(e);
}

JSValue dyn_v_cache_find(JSContext* ctx, const char* module,
    const char* fnname)
{
    dyn_v_cache_t* e;
    JSValue v = JS_UNDEFINED;
    pthread_mutex_lock(&dyn_v_cache_mu);
    for (e = dyn_v_cache_list; e; e = e->next) {
        if (e->ctx == ctx && !JS_IsUndefined(e->fn)
            && !strcmp(e->module, module) && !strcmp(e->fnname, fnname)) {
            v = JS_DupValue(ctx, e->fn);
            break;
        }
    }
    pthread_mutex_unlock(&dyn_v_cache_mu);
    return v;
}

void dyn_v_cache_store(JSContext* ctx, const char* module,
    const char* fnname, JSValue fn)
{
    dyn_v_cache_t* e = (dyn_v_cache_t*)malloc(sizeof(*e));
    if (!e)
        return;
    e->ctx = ctx;
    e->module = module;
    e->fnname = fnname;
    e->fn = JS_DupValue(ctx, fn);
    if (JS_AddShutdownSweep(JS_GetRuntime(ctx), dyn_v_cache_sweep, e) < 0) {
        JS_FreeValue(ctx, e->fn);
        free(e);
        return;
    }
    pthread_mutex_lock(&dyn_v_cache_mu);
    e->next = dyn_v_cache_list;
    dyn_v_cache_list = e;
    pthread_mutex_unlock(&dyn_v_cache_mu);
}

#endif
