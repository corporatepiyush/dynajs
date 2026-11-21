#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

static int simd_window_arg(JSContext* ctx, int64_t* out, JSValueConst v)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (d != d)
        d = 0.0;
    if (d > 9007199254740991.0 || d < -9007199254740991.0) {
        JS_ThrowRangeError(ctx, "offset/length/limit out of range");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

int simd_window(JSContext* ctx, int argc, JSValueConst* argv, int base,
    size_t* poff, size_t* plen)
{
    int64_t off = 0, len = -1;
    if (argc == base)
        return 0;
    if (argc == base + 1 && JS_IsObject(argv[base])) {
        JSValue o = argv[base];
        JSValue v;
        int has_len = 0, has_off = 0;
        {
            JSPropertyEnum* props = NULL;
            uint32_t nprops = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
                return -1;
            for (i = 0; i < nprops; i++) {
                const char* nm = JS_AtomToCString(ctx, props[i].atom);
                int ok = 0;
                if (nm) {
                    ok = !strcmp(nm, "offset") || !strcmp(nm, "length") || !strcmp(nm, "limit");
                    if (!ok)
                        JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: "
                                               "offset, length, limit)",
                            nm);
                    JS_FreeCString(ctx, nm);
                }
                if (!ok) {
                    uint32_t j;
                    for (j = 0; j < nprops; j++)
                        JS_FreeAtom(ctx, props[j].atom);
                    js_free(ctx, props);
                    return -1;
                }
            }
            for (uint32_t j = 0; j < nprops; j++)
                JS_FreeAtom(ctx, props[j].atom);
            js_free(ctx, props);
        }
        v = JS_GetPropertyStr(ctx, o, "offset");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (simd_window_arg(ctx, &off, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            has_off = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "length");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (simd_window_arg(ctx, &len, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            has_len = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "limit");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t lim;
            if (simd_window_arg(ctx, &lim, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (has_len) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "length and limit are aliases (never both)");
                return -1;
            }
            len = lim;
            has_len = 1;
        }
        JS_FreeValue(ctx, v);
        if (!has_len && !has_off)
            return 0;
        if (!has_len) {
            JS_ThrowTypeError(ctx, "window requires length (or limit)");
            return -1;
        }
    } else if (argc == base + 2) {
        if (simd_window_arg(ctx, &off, argv[base]) < 0)
            return -1;
        if (simd_window_arg(ctx, &len, argv[base + 1]) < 0)
            return -1;
    } else {
        return (JS_ThrowTypeError(ctx, "expected %d args plus optional (offset, length)", base), -1);
    }
    if (off < 0 || len < 0) {
        JS_ThrowRangeError(ctx, "window offset/length must be >= 0");
        return -1;
    }
    *poff = (size_t)off;
    *plen = (size_t)len;
    return 1;
}

#endif
