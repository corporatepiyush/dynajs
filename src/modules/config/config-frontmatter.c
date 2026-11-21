#include "config-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

static JSValue dyn_fm_split(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *buf, *src;
    size_t n, dstart, dend, bstart;
    JSValue o;
    const char* lang = NULL;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "FrontMatter.split(text): argument must be a string");
    buf = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!buf)
        return JS_EXCEPTION;
    src = buf;
    dyn_cfg_bom(&src, &n);
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeCString(ctx, buf);
        return JS_EXCEPTION;
    }

    if (!dyn_frontmatter_find(src, n, &dstart, &dend, &bstart, &lang))
        goto no_fm;
    if (JS_DefinePropertyValueStr(ctx, o, "data",
            JS_NewStringLen(ctx, src + dstart, dend - dstart), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, o, "body",
               JS_NewStringLen(ctx, src + bstart, n - bstart), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, o, "lang",
               JS_NewString(ctx, lang), JS_PROP_C_W_E)
            < 0)
        goto fail;
    JS_FreeCString(ctx, buf);
    return o;
no_fm:
    if (JS_DefinePropertyValueStr(ctx, o, "data", JS_NULL, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "body",
               JS_NewStringLen(ctx, src, n), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, o, "lang", JS_NULL, JS_PROP_C_W_E) < 0)
        goto fail;
    JS_FreeCString(ctx, buf);
    return o;
fail:
    JS_FreeValue(ctx, o);
    JS_FreeCString(ctx, buf);
    return JS_EXCEPTION;
}

const JSCFunctionListEntry dyn_fm_ns[] = {
    JS_CFUNC_DEF("split", 1, dyn_fm_split),
};

#endif
