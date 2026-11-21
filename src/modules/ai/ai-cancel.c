#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)
#include <sys/socket.h>

static JSClassID ai_cancel_class_id;

static ai_cancel_t* ai_cancel_new(void)
{
    ai_cancel_t* c = (ai_cancel_t*)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    if (pthread_mutex_init(&c->mu, NULL) != 0) {
        free(c);
        return NULL;
    }
    c->refs = 1;
    c->fd = -1;
    return c;
}

static ai_cancel_t* ai_cancel_ref(ai_cancel_t* c)
{
    pthread_mutex_lock(&c->mu);
    c->refs++;
    pthread_mutex_unlock(&c->mu);
    return c;
}

void ai_cancel_unref(ai_cancel_t* c)
{
    int left;
    if (!c)
        return;
    pthread_mutex_lock(&c->mu);
    left = --c->refs;
    pthread_mutex_unlock(&c->mu);
    if (left > 0)
        return;
    pthread_mutex_destroy(&c->mu);
    free(c);
}

int ai_cancel_on_fd(void* ud, int fd)
{
    ai_cancel_t* c = (ai_cancel_t*)ud;
    int rc = 0;
    pthread_mutex_lock(&c->mu);
    if (fd >= 0 && c->cancelled)
        rc = 1;
    else
        c->fd = fd;
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static void ai_cancel_fire(ai_cancel_t* c)
{
    pthread_mutex_lock(&c->mu);
    c->cancelled = 1;
    if (c->fd >= 0)
        shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_unlock(&c->mu);
}

int ai_req_cancelled(ai_req_t* r)
{
    int v;
    if (!r || !r->cancel)
        return 0;
    pthread_mutex_lock(&r->cancel->mu);
    v = r->cancel->cancelled;
    pthread_mutex_unlock(&r->cancel->mu);
    return v;
}

void ai_req_drop_stream(ai_req_t* r)
{
    if (!r->st)
        return;
    if (r->cancel)
        ai_cancel_on_fd(r->cancel, -1);
    hc_stream_dispose(r->st);
    r->st = NULL;
}

JSValue ai_abort_throw(JSContext* ctx)
{
    return ai_error_throw(ctx, "the request was aborted by its signal", 0, "aborted");
}

static void ai_cancel_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    ai_cancel_unref((ai_cancel_t*)JS_GetOpaque(val, ai_cancel_class_id));
}

static JSClassDef ai_cancel_class = { "AICancel", ai_cancel_finalizer, NULL, NULL, NULL };

int ai_cancel_register(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!ai_cancel_class_id)
        JS_NewClassID(&ai_cancel_class_id);
    if (!JS_IsRegisteredClass(rt, ai_cancel_class_id)
        && JS_NewClass(rt, ai_cancel_class_id, &ai_cancel_class) < 0)
        return -1;
    return 0;
}

static JSValue ai_cancel_listener(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic, JSValue* data)
{
    ai_cancel_t* c = (ai_cancel_t*)JS_GetOpaque(data[0], ai_cancel_class_id);
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (c)
        ai_cancel_fire(c);
    return JS_UNDEFINED;
}

static int ai_signal_call(JSContext* ctx, JSValueConst signal, const char* method,
    JSValueConst fn)
{
    JSValue m = JS_GetPropertyStr(ctx, signal, method), args[2], rv;
    if (JS_IsException(m))
        return -1;
    if (!JS_IsFunction(ctx, m)) {
        JS_FreeValue(ctx, m);
        JS_ThrowTypeError(ctx, "dyna:ai request: signal must be an AbortSignal");
        return -1;
    }
    args[0] = JS_NewString(ctx, "abort");
    args[1] = JS_DupValue(ctx, fn);
    rv = JS_Call(ctx, m, signal, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, m);
    if (JS_IsException(rv))
        return -1;
    JS_FreeValue(ctx, rv);
    return 0;
}

int ai_req_signal(JSContext* ctx, ai_req_t* r, JSValueConst signal)
{
    JSValue ab, holder, fn;
    int aborted;
    if (JS_IsUndefined(signal) || JS_IsNull(signal))
        return 0;
    if (!JS_IsObject(signal)) {
        JS_ThrowTypeError(ctx, "dyna:ai request: signal must be an AbortSignal");
        return -1;
    }
    ab = JS_GetPropertyStr(ctx, signal, "aborted");
    if (JS_IsException(ab))
        return -1;
    if (!JS_IsBool(ab)) {
        JS_FreeValue(ctx, ab);
        JS_ThrowTypeError(ctx, "dyna:ai request: signal must be an AbortSignal");
        return -1;
    }
    aborted = JS_ToBool(ctx, ab);
    JS_FreeValue(ctx, ab);
    if (aborted) {
        ai_abort_throw(ctx);
        return -1;
    }
    r->cancel = ai_cancel_new();
    if (!r->cancel) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    holder = JS_NewObjectClass(ctx, (int)ai_cancel_class_id);
    if (JS_IsException(holder))
        return -1;
    JS_SetOpaque(holder, ai_cancel_ref(r->cancel));
    fn = JS_NewCFunctionData(ctx, ai_cancel_listener, 1, 0, 1, &holder);
    JS_FreeValue(ctx, holder);
    if (JS_IsException(fn))
        return -1;
    if (ai_signal_call(ctx, signal, "addEventListener", fn) < 0) {
        JS_FreeValue(ctx, fn);
        return -1;
    }
    r->rt = JS_GetRuntime(ctx);
    r->signal = JS_DupValue(ctx, signal);
    r->listener = fn;
    return 0;
}

void ai_req_unlisten(JSContext* ctx, ai_req_t* r)
{
    int had;
    JSValue exc;
    if (!r || !JS_IsObject(r->signal))
        return;
    had = JS_HasException(ctx);
    exc = had ? JS_GetException(ctx) : JS_UNDEFINED;
    if (ai_signal_call(ctx, r->signal, "removeEventListener", r->listener) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    if (had)
        JS_Throw(ctx, exc);
    JS_FreeValue(ctx, r->signal);
    JS_FreeValue(ctx, r->listener);
    r->signal = JS_UNDEFINED;
    r->listener = JS_UNDEFINED;
}
#endif
