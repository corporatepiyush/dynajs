#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

JSClassID dyn_resolver_class_id;

_Thread_local int dns_in_final;

static void dyn_dns_finalizer(JSRuntime* rt, JSValue val)
{
    dns_in_final++;
    dyn_res_finalizer(rt, val);
    dns_in_final--;
}

static const JSClassDef dyn_resolver_class = {
    "DNSResolver",
    .finalizer = dyn_dns_finalizer,
    .gc_mark = dyn_resolver_gc_mark,
};

void dns_promise_settle(JSContext* ctx, dns_pending_t* p, JSValue err,
    JSValue val)
{
    JSValue r;

    if (!JS_IsNull(err)) {
        JSValue e = JS_NewError(ctx);
        JSValueConst a[1];
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message", JS_DupValue(ctx, err), JS_PROP_C_W_E);
            a[0] = e;
        } else {
            dyn_exc_drop(ctx);
            a[0] = err;
        }
        r = JS_Call(ctx, p->reject, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, e);
    } else {
        JSValue shaped = dns_shape_records(ctx, val, p->shape);
        JSValueConst a[1];
        if (JS_IsException(shaped) && JS_HasUncatchableException(ctx)) {
            dyn_exc_forward(ctx);
            shaped = JS_UNDEFINED;
            r = JS_UNDEFINED;
        } else if (JS_IsException(shaped)) {
            shaped = JS_GetException(ctx);
            r = JS_Call(ctx, p->reject, JS_UNDEFINED, 1,
                (JSValueConst*)&shaped);
        } else {
            a[0] = shaped;
            r = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, shaped);
    }
    if (JS_IsException(r))
        dyn_exc_drop(ctx);
    else
        JS_FreeValue(ctx, r);
}

static const JSCFunctionListEntry dyn_resolver_proto[] = {
    JS_CFUNC_DEF("query", 2, dyn_resolver_query),
    JS_CFUNC_DEF("lookup", 1, dyn_resolver_lookup),
    JS_CFUNC_DEF("resolveCname", 1, dyn_resolver_cname),
    JS_CFUNC_DEF("resolveMx", 1, dyn_resolver_mx),
    JS_CFUNC_DEF("resolveTxt", 1, dyn_resolver_txt),
    JS_CFUNC_DEF("resolveNs", 1, dyn_resolver_ns),
};

JSClassID dyn_dnssrv_class_id;

static const JSClassDef dyn_dnssrv_class = {
    "DNSServer",
    .finalizer = dyn_dns_finalizer,
    .gc_mark = dyn_dnssrv_gc_mark,
};

static const JSCFunctionListEntry dyn_dnssrv_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_dnssrv_start),
    JS_CGETSET_DEF("port", dyn_dnssrv_get_port, NULL),
};

int dyn_dns_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_resolver_class_id,
            &dyn_resolver_class, dyn_resolver_proto,
            countof(dyn_resolver_proto),
            dyn_resolver_ctor, "DNSResolver")
        < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_dnssrv_class_id, &dyn_dnssrv_class,
        dyn_dnssrv_proto, countof(dyn_dnssrv_proto),
        dyn_dnssrv_ctor, "DNSServer");
}

void dyn_dns_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "DNSResolver");
    JS_AddModuleExport(ctx, m, "DNSServer");
}


int js_nat_init_dns(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:dns", dyn_dns_register);
    if (!m)
        return -1;
    dyn_dns_add_exports(ctx, m);
    return 0;
}

#endif
