#include "libc-wst.h"


static const char* const wst_names[WSTC_N] = {
    "ReadableStream",
    "ReadableStreamDefaultReader",
    "ReadableStreamBYOBReader",
    "ReadableStreamDefaultController",
    "ReadableByteStreamController",
    "ReadableStreamBYOBRequest",
    "WritableStream",
    "WritableStreamDefaultWriter",
    "WritableStreamDefaultController",
    "TransformStream",
    "TransformStreamDefaultController",
    "ByteLengthQueuingStrategy",
    "CountQueuingStrategy",
    "TextEncoderStream",
    "TextDecoderStream",
    "CompressionStream",
    "DecompressionStream",
    NULL,
};

int wst_ns_install(JSContext* ctx, JSValueConst ns, int all)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return -1;
    JSValue g;
    int i;
    (void)all;

    if (!w)
        return -1;
    g = JS_GetGlobalObject(ctx);
    if (wst_slot(ctx, &w->abort_ctor, JS_GetPropertyStr(ctx, g, "AbortController")) < 0
        || wst_slot(ctx, &w->ab_ctor, JS_GetPropertyStr(ctx, g, "ArrayBuffer")) < 0
        || wst_slot(ctx, &w->sig_ctor, JS_GetPropertyStr(ctx, g, "AbortSignal")) < 0) {
        JS_FreeValue(ctx, g);
        return -1;
    }
    JS_FreeValue(ctx, g);
    {
        JSValue p = JS_GetPropertyStr(ctx, w->ab_ctor, "prototype");
        int r = JS_IsException(p) ? -1 : wst_slot(ctx, &w->ab_transfer, JS_GetPropertyStr(ctx, p, "transfer"));
        JS_FreeValue(ctx, p);
        if (r < 0)
            return -1;
        if (JS_IsFunction(ctx, w->sig_ctor)) {
            p = JS_GetPropertyStr(ctx, w->sig_ctor, "prototype");
            r = JS_IsException(p) ? -1 : wst_slot(ctx, &w->sig_add, JS_GetPropertyStr(ctx, p, "addEventListener"));
            if (r == 0)
                r = wst_slot(ctx, &w->sig_remove, JS_GetPropertyStr(ctx, p, "removeEventListener"));
            JS_FreeValue(ctx, p);
            if (r < 0)
                return -1;
        }
    }
    if (wst_init_ws(ctx, w->ctors) < 0 || wst_init_rs(ctx, w->ctors) < 0 || wst_init_bytes(ctx, w->ctors) < 0 || wst_init_pipe(ctx) < 0 || wst_init_ts(ctx, w->ctors) < 0 || wst_init_misc(ctx, w->ctors) < 0)
        return -1;
    {
        JSValue internal = JS_GetPropertyStr(ctx, ns, "internal");
        int r = JS_IsException(internal) ? -1 : wst_init_adapt(ctx, internal);
        JS_FreeValue(ctx, internal);
        if (r < 0)
            return -1;
    }
    for (i = 0; i < WSTC_N; i++) {
        if (!wst_names[i] || JS_IsUndefined(w->ctors[i]))
            continue;
        if (JS_SetPropertyStr(ctx, ns, wst_names[i], JS_DupValue(ctx, w->ctors[i])) < 0)
            return -1;
    }
    return 0;
}
