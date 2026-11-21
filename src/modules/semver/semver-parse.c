#include "semver-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

static JSValue sv_build_array(JSContext* ctx, const SemVer* v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < v->n_build; i++) {
        JSValue item = JS_NewStringLen(ctx, v->build[i].s, v->build[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

JSValue dyn_semver_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    SemVer v;
    JSValue obj, ver, pre, build;
    int err;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JSValue e = JS_ThrowTypeError(ctx, "dyna:semver: invalid version \"%.*s\"",
            (int)(n > 80 ? 80 : n), s);
        JS_FreeCString(ctx, s);
        return e;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, s);
        return obj;
    }
    ver = sv_build_version(ctx, &v);
    pre = JS_IsException(ver) ? JS_EXCEPTION : sv_pre_array(ctx, &v);
    build = JS_IsException(pre) ? JS_EXCEPTION : sv_build_array(ctx, &v);
    JS_FreeCString(ctx, s);
    if (JS_IsException(ver) || JS_IsException(pre) || JS_IsException(build)) {
        JS_FreeValue(ctx, ver);
        JS_FreeValue(ctx, pre);
        JS_FreeValue(ctx, build);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    err = JS_DefinePropertyValueStr(ctx, obj, "major",
              JS_NewInt64(ctx, (int64_t)v.major),
              JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "minor",
               JS_NewInt64(ctx, (int64_t)v.minor),
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "patch",
               JS_NewInt64(ctx, (int64_t)v.patch),
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "prerelease", pre,
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "build", build,
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "version", ver,
               JS_PROP_C_W_E)
        < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

#endif
