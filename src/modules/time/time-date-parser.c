#include "time-internal.h"
#include "core/dyn-ctype.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TIME)

static void dyn_dp_dispose(void* native)
{
    free(native);
}

void dyn_dp_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_dp_dispose(JS_GetOpaque(val, dyn_dp_class_id));
}

JSValue dyn_dp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    DynDateParser* p;
    const char* name = NULL;
    const DynDateLocale* loc = dyn_dp_locale_find(NULL);
    JSValue obj, proto;
    int64_t base = 0;
    int has_base = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        size_t nlen;
        name = JS_ToCStringLen(ctx, &nlen, argv[0]);
        if (!name)
            return JS_EXCEPTION;
        loc = dyn_dp_locale_find(name);
        if (!loc) {
            JSValue e = JS_ThrowRangeError(ctx,
                "dyna:time: unknown locale \"%s\"; known: "
                "en-US en-GB en fr de es",
                name);
            JS_FreeCString(ctx, name);
            return e;
        }
        JS_FreeCString(ctx, name);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const dp_keys[] = { "now" };
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "DateParser: options must be an object");
        if (dyn_opts_strict(ctx, argv[1], dp_keys, 1))
            return JS_EXCEPTION;
        JSValue nv = JS_GetPropertyStr(ctx, argv[1], "now");
        if (JS_IsException(nv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(nv)) {
            if (dyn_time_read_int(ctx, nv, "now", &base)) {
                JS_FreeValue(ctx, nv);
                return JS_EXCEPTION;
            }
            has_base = 1;
        }
        JS_FreeValue(ctx, nv);
    }

    p = (DynDateParser*)malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->loc = loc;
    p->base = base;
    p->has_base = has_base;

    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        free(p);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_dp_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        free(p);
        return obj;
    }
    JS_SetOpaque(obj, p);
    return obj;
}

JSValue dyn_dp_parse_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynDateParser* p;
    const char* s;
    size_t len;
    int64_t out = 0, now;
    int rc;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "parse(text) requires an argument");
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    p = (DynDateParser*)JS_GetOpaque2(ctx, this_val, dyn_dp_class_id);
    if (!p) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    now = p->has_base ? p->base : (int64_t)time(NULL);
    rc = dyn_dp_parse(p, s, len, now, &out);
    JS_FreeCString(ctx, s);
    if (rc)
        return JS_NULL;
    return JS_NewInt64(ctx, out);
}

JSValue dyn_dp_locale_get(JSContext* ctx, JSValueConst this_val)
{
    DynDateParser* p = (DynDateParser*)JS_GetOpaque2(ctx, this_val,
        dyn_dp_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewString(ctx, p->loc->name);
}

JSValue dyn_dp_dayfirst_get(JSContext* ctx, JSValueConst this_val)
{
    DynDateParser* p = (DynDateParser*)JS_GetOpaque2(ctx, this_val,
        dyn_dp_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->loc->day_first);
}

#endif
