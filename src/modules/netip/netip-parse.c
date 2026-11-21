#include "netip-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NETIP)

JSValue dyn_netip_parse_addr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    NetAddr a;
    JSValue obj, u8, str;
    char buf[48];
    size_t blen;
    int nbytes, err;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_addr(s, n, &a) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid IP address");
    }
    netip_cfree(ctx, argv[0], s);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    nbytes = a.is4 ? 4 : 16;
    u8 = dyn_nat_u8_copy(ctx, a.b, (size_t)nbytes);
    if (JS_IsException(u8)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    blen = netipk_format_addr(&a, buf);
    str = JS_NewStringLen(ctx, buf, blen);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, u8);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    err = JS_DefinePropertyValueStr(ctx, obj, "is4",
              JS_NewBool(ctx, a.is4), JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "is6",
               JS_NewBool(ctx, !a.is4), JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "bytes", u8,
               JS_PROP_C_W_E)
        < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "string", str,
               JS_PROP_C_W_E)
        < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

JSValue dyn_netip_parse_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    NetAddr a;
    int bits, err;
    JSValue obj, str;
    char buf[48];
    size_t blen;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_prefix(s, n, &a, &bits) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid CIDR prefix");
    }
    netip_cfree(ctx, argv[0], s);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    blen = netipk_format_addr(&a, buf);
    str = JS_NewStringLen(ctx, buf, blen);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    err = JS_DefinePropertyValueStr(ctx, obj, "addr", str, JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "bits",
               JS_NewInt32(ctx, bits), JS_PROP_C_W_E)
        < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

#endif
