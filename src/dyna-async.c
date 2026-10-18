#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASYNC)

#include <string.h>

#include "dyna-async.js.inc.c"

static const char* const dyn_async_export_names[] = {
    "sleep",
    "withTimeout",
    "retry",
    "Semaphore",
    "Queue",
    "Channel",
    "pmap",
    "Pool",
    "debounce",
    "throttle",
};

#define DYN_ASYNC_NNAMES ((int)(sizeof(dyn_async_export_names) / sizeof(dyn_async_export_names[0])))

static JSValue dyn_async_stub(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowInternalError(ctx,
        "dyna:async is unavailable (module initialization failed)");
}

static int dyn_async_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue exports = JS_Eval(ctx, dyn_async_js_src, strlen(dyn_async_js_src),
        "<dyna:async>", JS_EVAL_TYPE_GLOBAL);
    int i;

    if (JS_IsException(exports)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        for (i = 0; i < DYN_ASYNC_NNAMES; i++) {
            JSValue stub = JS_NewCFunction(ctx, dyn_async_stub,
                dyn_async_export_names[i], 0);
            if (JS_IsException(stub))
                return -1;
            if (JS_SetModuleExport(ctx, m, dyn_async_export_names[i],
                    stub)
                < 0)
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

int js_nat_init_async(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:async", dyn_async_init_module);
    int i;

    if (!m)
        return -1;
    for (i = 0; i < DYN_ASYNC_NNAMES; i++)
        if (JS_AddModuleExport(ctx, m, dyn_async_export_names[i]) < 0)
            return -1;
    return 0;
}

#endif
