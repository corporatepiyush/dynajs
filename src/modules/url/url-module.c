#include "url-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_URL)

static const JSCFunctionListEntry dyn_url_funcs[] = {
    JS_CFUNC_DEF("formEncode", 1, dyn_form_encode),
    JS_CFUNC_DEF("formDecode", 1, dyn_form_decode),
    JS_CFUNC_DEF("encodeURIComponentStrict", 1, dyn_enc_strict),
    JS_CFUNC_DEF("domainToASCII", 1, dyn_domain_to_ascii),
    JS_CFUNC_DEF("domainToUnicode", 1, dyn_domain_to_unicode),
    JS_CFUNC_DEF("punycodeEncode", 1, dyn_punycode_encode),
    JS_CFUNC_DEF("punycodeDecode", 1, dyn_punycode_decode),
};

static int dyn_url_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue ctor = dyn_url_constructor(ctx);
    if (JS_IsException(ctor) || JS_SetModuleExport(ctx, m, "URL", ctor) < 0)
        return -1;
    ctor = dyn_sp_constructor(ctx);
    if (JS_IsException(ctor) || JS_SetModuleExport(ctx, m, "URLSearchParams", ctor) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

int js_nat_init_url(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:url", dyn_url_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "URL");
    JS_AddModuleExport(ctx, m, "URLSearchParams");
    return JS_AddModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

#endif
