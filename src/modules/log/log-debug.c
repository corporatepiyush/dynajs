#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

static JSValue dyn_debug_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    const char* ns;
    if (!magic)
        return JS_UNDEFINED;
    ns = JS_ToCString(ctx, data[0]);
    if (!ns)
        return JS_EXCEPTION;
    {
        dyn_line_t b;
        int i;
        dyn_line_init(&b);
        dyn_line_esc_ctrl(&b, ns, strlen(ns));
        for (i = 0; i < argc; i++) {
            size_t n;
            const char* s = JS_ToCStringLen(ctx, &n, argv[i]);
            if (!s) {
                dyn_line_free(&b);
                return JS_EXCEPTION;
            }
            dyn_line_put(&b, " ", 1);
            dyn_line_esc_ctrl(&b, s, n);
            JS_FreeCString(ctx, s);
        }
        dyn_line_force(&b, "\n", 1);
        (void)dyn_write_all(STDERR_FILENO, b.p, b.n);
        dyn_line_free(&b);
    }
    JS_FreeCString(ctx, ns);
    return JS_UNDEFINED;
}

JSValue dyn_debug(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *ns, *env;
    JSValue data[1], fn;
    int on;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Debug(namespace): namespace must be a string");
    ns = JS_ToCString(ctx, argv[0]);
    if (!ns)
        return JS_EXCEPTION;
    env = getenv("DEBUG");
    on = (env && *env) ? dyn_debug_match(env, ns) : 0;
    JS_FreeCString(ctx, ns);
    data[0] = JS_DupValue(ctx, argv[0]);
    fn = JS_NewCFunctionData(ctx, dyn_debug_write, 1, on, 1, data);
    JS_FreeValue(ctx, data[0]);
    return fn;
}

#endif
