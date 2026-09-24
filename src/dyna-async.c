/*
 * dyna:async -- concurrency utilities as an embedded-JS module.
 *
 *   import { pmap, Pool, Channel, Queue, Semaphore, sleep,
 *            withTimeout, retry, debounce, throttle } from "dyna:async";
 *
 * SHAPE: the implementation lives in dyna-async.js.inc.c (one IIFE that
 * returns an exports object, the dyna-fetch.inc.c embed pattern). This file
 * is the small C shell: it registers the "dyna:async" module, evaluates the
 * embedded source ONCE at module init, and bridges the returned object's
 * own properties into the module's exports. The C side adds nothing and
 * wraps nothing -- every export is the JS object itself.
 *
 * THE NEVER-REJECTING BRIDGE (the dyna-scrape lesson): the embedded IIFE
 * cannot throw at the top level -- it builds a stub table first and swaps
 * in the real implementations inside try/catch, returning the stubs (which
 * throw a clear error at USE time) on any init failure. The bridge here is
 * correspondingly total: even if the eval ITSELF fails (out of memory, a
 * hostile embedder), every declared export is still defined -- as a
 * throwing stub -- so `import "dyna:async"` resolves in every build and a
 * failure is a clean exception at the call site, never a broken namespace.
 *
 * WHAT IS REAL, stated honestly (the full argument lives in the embedded
 * source's header): the engine's shared offload pool executes C functions,
 * not JS, and os.Worker takes a module filename whose messages cannot
 * carry functions -- so pmap/Pool deliver CONCURRENCY-LIMITED INTERLEAVING
 * on the event-loop thread, not worker parallelism. Channel/Queue/Semaphore
 * are real bounded handoff structures; withTimeout/retry/debounce/throttle
 * are real clock-driven combinators. Nothing pretends to be parallel.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASYNC)

#include <string.h>

#include "dyna-async.js.inc.c"

/* The declared export list: JS_AddModuleExport at registration (so the
 * namespace shape is fixed even if init later fails) and JS_SetModuleExport
 * from the evaluated object at module init. Keep in sync with the IIFE's
 * returned object. */
static const char *const dyn_async_export_names[] = {
    "sleep", "withTimeout", "retry",
    "Semaphore", "Queue", "Channel",
    "pmap", "Pool", "debounce", "throttle",
};

#define DYN_ASYNC_NNAMES ((int)(sizeof(dyn_async_export_names) / \
                                sizeof(dyn_async_export_names[0])))

/* A stub whose only job is to throw a clear error; used when the embedded
 * eval itself failed, so the namespace is still complete. */
static JSValue dyn_async_stub(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowInternalError(ctx,
        "dyna:async is unavailable (module initialization failed)");
}

static int dyn_async_init_module(JSContext *ctx, JSModuleDef *m)
{
    JSValue exports = JS_Eval(ctx, dyn_async_js_src, strlen(dyn_async_js_src),
                              "<dyna:async>", JS_EVAL_TYPE_GLOBAL);
    int i;

    if (JS_IsException(exports)) {
        /* The eval failed even though the source cannot throw at top level:
         * only OOM (or an engine fault) lands here. Swallow the exception,
         * define every export as a stub, and let the module load. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        for (i = 0; i < DYN_ASYNC_NNAMES; i++) {
            JSValue stub = JS_NewCFunction(ctx, dyn_async_stub,
                                           dyn_async_export_names[i], 0);
            if (JS_IsException(stub))
                return -1;
            if (JS_SetModuleExport(ctx, m, dyn_async_export_names[i],
                                   stub) < 0)
                return -1;
        }
        return 0;
    }
    if (!JS_IsObject(exports)) {
        JS_FreeValue(ctx, exports);
        JS_ThrowInternalError(ctx,
            "dyna:async: embedded source did not return an object");
        return -1;
    }
    for (i = 0; i < DYN_ASYNC_NNAMES; i++) {
        JSValue v = JS_GetPropertyStr(ctx, exports,
                                      dyn_async_export_names[i]);
        if (JS_IsException(v) || JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, exports);
            JS_ThrowInternalError(ctx,
                "dyna:async: embedded source is missing export \"%s\"",
                dyn_async_export_names[i]);
            return -1;
        }
        if (JS_SetModuleExport(ctx, m, dyn_async_export_names[i], v) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, exports);
            return -1;
        }
    }
    JS_FreeValue(ctx, exports);
    return 0;
}

int js_nat_init_async(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:async", dyn_async_init_module);
    int i;

    if (!m)
        return -1;
    for (i = 0; i < DYN_ASYNC_NNAMES; i++)
        if (JS_AddModuleExport(ctx, m, dyn_async_export_names[i]) < 0)
            return -1;
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_ASYNC */
