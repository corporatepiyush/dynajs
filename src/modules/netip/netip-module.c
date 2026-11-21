#include "netip-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NETIP)

static JSValue dyn_netip_masked(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, blen;
    NetAddr a;
    int bits;
    char buf[48];

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_prefix(s, n, &a, &bits) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid CIDR prefix");
    }
    netip_cfree(ctx, argv[0], s);
    netipk_mask(&a, bits);
    blen = netipk_format_addr(&a, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

static JSValue dyn_netip_canonical(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, blen;
    NetAddr a;
    char buf[48];

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_addr(s, n, &a) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid IP address");
    }
    netip_cfree(ctx, argv[0], s);
    blen = netipk_format_addr(&a, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

static JSValue dyn_netip_is_valid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    NetAddr a;
    int ok;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewBool(ctx, 0);
    s = netip_cstr(ctx, &n, argv[0]);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NewBool(ctx, 0);
    }
    ok = netipk_parse_addr(s, n, &a) == 0;
    netip_cfree(ctx, argv[0], s);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_netip_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *as, *bs;
    size_t an, bn;
    NetAddr x, y;
    int ok;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "dyna:netip: compareAddr(a, b) expects two strings");
    as = netip_cstr(ctx, &an, argv[0]);
    if (!as)
        return JS_EXCEPTION;
    bs = netip_cstr(ctx, &bn, argv[1]);
    if (!bs) {
        netip_cfree(ctx, argv[0], as);
        return JS_EXCEPTION;
    }
    ok = netipk_parse_addr(as, an, &x) == 0 && netipk_parse_addr(bs, bn, &y) == 0;
    netip_cfree(ctx, argv[0], as);
    netip_cfree(ctx, argv[1], bs);
    if (!ok)
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid IP address");
    return JS_NewInt32(ctx, netipk_compare(&x, &y));
}

JSClassID dyn_prefix_class_id;

static void dyn_prefix_dispose(void* native) { free(native); }

static void dyn_prefix_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_prefix_dispose(JS_GetOpaque(val, dyn_prefix_class_id));
}

static const JSClassDef dyn_prefix_class = {
    "Prefix",
    .finalizer = dyn_prefix_finalizer,
};

static JSValue dyn_prefix_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    NetAddr a;
    int bits;
    dyn_prefix_t* p;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Prefix(cidr) requires a CIDR string");
    s = netip_cstr(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (netipk_parse_prefix(s, n, &a, &bits) != 0) {
        netip_cfree(ctx, argv[0], s);
        return JS_ThrowTypeError(ctx, "dyna:netip: invalid CIDR prefix");
    }
    netip_cfree(ctx, argv[0], s);

    p = (dyn_prefix_t*)malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    netipk_mask(&a, bits);
    p->net = a;
    p->bits = bits;
    return dyn_plain_wrap(ctx, new_target, dyn_prefix_class_id, p, dyn_prefix_dispose);
}

static JSValue dyn_prefix_overlaps(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prefix_t *a, *b;
    int nbits;

    (void)argc;
    b = (dyn_prefix_t*)JS_GetOpaque2(ctx, argv[0], dyn_prefix_class_id);
    if (!b)
        return JS_EXCEPTION;
    a = (dyn_prefix_t*)dyn_plain_get(ctx, this_val, dyn_prefix_class_id);
    if (!a)
        return JS_EXCEPTION;
    if (a->net.is4 != b->net.is4)
        return JS_NewBool(ctx, 0);
    nbits = a->bits < b->bits ? a->bits : b->bits;
    return JS_NewBool(ctx, netipk_prefix_contains(&a->net, nbits, &b->net));
}

static JSValue dyn_prefix_get_masked(JSContext* ctx, JSValueConst this_val)
{
    dyn_prefix_t* p = (dyn_prefix_t*)dyn_plain_get(ctx, this_val,
        dyn_prefix_class_id);
    char buf[48];
    size_t blen;
    if (!p)
        return JS_EXCEPTION;
    blen = netipk_format_addr(&p->net, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

static JSValue dyn_prefix_get_bits(JSContext* ctx, JSValueConst this_val)
{
    dyn_prefix_t* p = (dyn_prefix_t*)dyn_plain_get(ctx, this_val,
        dyn_prefix_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->bits);
}

static JSValue dyn_prefix_get_is4(JSContext* ctx, JSValueConst this_val)
{
    dyn_prefix_t* p = (dyn_prefix_t*)dyn_plain_get(ctx, this_val,
        dyn_prefix_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->net.is4);
}

static const JSCFunctionListEntry dyn_prefix_proto[] = {
    JS_CFUNC_DEF("contains", 1, dyn_prefix_contains),
    JS_CFUNC_DEF("overlaps", 1, dyn_prefix_overlaps),
    JS_CGETSET_DEF("masked", dyn_prefix_get_masked, NULL),
    JS_CGETSET_DEF("bits", dyn_prefix_get_bits, NULL),
    JS_CGETSET_DEF("isIPv4", dyn_prefix_get_is4, NULL),
};

static const JSCFunctionListEntry dyn_netip_funcs[] = {
    JS_CFUNC_DEF("parseAddr", 1, dyn_netip_parse_addr),
    JS_CFUNC_DEF("parsePrefix", 1, dyn_netip_parse_prefix),
    JS_CFUNC_DEF("contains", 2, dyn_netip_contains),
    JS_CFUNC_DEF("masked", 1, dyn_netip_masked),
    JS_CFUNC_DEF("canonical", 1, dyn_netip_canonical),
    JS_CFUNC_DEF("isValid", 1, dyn_netip_is_valid),
    JS_CFUNC_DEF("compareAddr", 2, dyn_netip_compare),
    JS_CFUNC_MAGIC_DEF("isLoopback", 1, dyn_netip_classify, CLS_LOOPBACK),
    JS_CFUNC_MAGIC_DEF("isPrivate", 1, dyn_netip_classify, CLS_PRIVATE),
    JS_CFUNC_MAGIC_DEF("isMulticast", 1, dyn_netip_classify, CLS_MULTICAST),
    JS_CFUNC_MAGIC_DEF("isUnspecified", 1, dyn_netip_classify, CLS_UNSPECIFIED),
    JS_CFUNC_MAGIC_DEF("isLinkLocalUnicast", 1, dyn_netip_classify,
        CLS_LL_UNICAST),
    JS_CFUNC_MAGIC_DEF("isGlobalUnicast", 1, dyn_netip_classify,
        CLS_GLOBAL_UNICAST),
    JS_CFUNC_MAGIC_DEF("isLinkLocalMulticast", 1, dyn_netip_classify,
        CLS_LL_MULTICAST),
};

int dyn_netip_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_prefix_class_id, &dyn_prefix_class,
            dyn_prefix_proto, countof(dyn_prefix_proto),
            dyn_prefix_ctor, "Prefix")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_netip_funcs,
        countof(dyn_netip_funcs));
}

int dyn_netip_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "Prefix");
    return JS_AddModuleExportList(ctx, m, dyn_netip_funcs,
        countof(dyn_netip_funcs));
}


int js_nat_init_netip(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:netip", dyn_netip_register);
    if (!m)
        return -1;
    if (dyn_netip_add_exports(ctx, m) < 0)
        return -1;
    return 0;
}

#endif
