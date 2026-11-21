#include "netip-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NETIP)

JSValue dyn_netip_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *ps, *as;
    size_t pn, an;
    NetAddr prefix, ip;
    int bits, ok;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "dyna:netip: contains(prefix, addr) expects two strings");
    ps = netip_cstr(ctx, &pn, argv[0]);
    if (!ps)
        return JS_EXCEPTION;
    as = netip_cstr(ctx, &an, argv[1]);
    if (!as) {
        netip_cfree(ctx, argv[0], ps);
        return JS_EXCEPTION;
    }
    ok = netipk_parse_prefix(ps, pn, &prefix, &bits) == 0 && netipk_parse_addr(as, an, &ip) == 0;
    netip_cfree(ctx, argv[0], ps);
    netip_cfree(ctx, argv[1], as);
    if (!ok)
        return JS_ThrowTypeError(ctx,
            "dyna:netip: invalid CIDR prefix or address");
    return JS_NewBool(ctx, netipk_prefix_contains(&prefix, bits, &ip));
}

JSValue dyn_netip_classify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n;
    NetAddr a;
    int r;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_addr(s, n, &a) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid IP address");
    }
    netip_cfree(ctx, argv[0], s);

    r = netipk_class(&a, magic);
    return JS_NewBool(ctx, r);
}

JSValue dyn_prefix_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    NetAddr ip;
    dyn_prefix_t* p;
    int ok;

    (void)argc;
    s = netip_cstr(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    p = (dyn_prefix_t*)dyn_plain_get(ctx, this_val, dyn_prefix_class_id);
    if (!p) {
        netip_cfree(ctx, argv[0], s);
        return JS_EXCEPTION;
    }
    ok = netipk_parse_addr(s, n, &ip) == 0 && netipk_prefix_contains(&p->net, p->bits, &ip);
    netip_cfree(ctx, argv[0], s);
    return JS_NewBool(ctx, ok);
}

#endif
