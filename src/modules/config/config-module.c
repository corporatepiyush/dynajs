#include "config-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

int dyn_def(JSContext* ctx, JSValueConst obj, const char* k, size_t klen,
    JSValue v)
{
    JSAtom a = JS_NewAtomLen(ctx, k, klen);
    int r;
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = JS_DefinePropertyValue(ctx, obj, a, v, JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
    return r;
}

static const JSCFunctionListEntry dyn_toml_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_tml_parse),
    JS_CFUNC_DEF("stringify", 1, dyn_tml_stringify),
};

static const char* const DYN_CFG_NAMES[] = { "INI", "Env", "FrontMatter", "TOML" };

static int dyn_config_init_module(JSContext* ctx, JSModuleDef* m)
{
    const JSCFunctionListEntry* tabs[4];
    int lens[4];
    size_t k;
    tabs[0] = dyn_ini_ns;
    lens[0] = countof(dyn_ini_ns);
    tabs[1] = dyn_env_ns;
    lens[1] = countof(dyn_env_ns);
    tabs[2] = dyn_fm_ns;
    lens[2] = countof(dyn_fm_ns);
    tabs[3] = dyn_toml_ns;
    lens[3] = countof(dyn_toml_ns);
    for (k = 0; k < countof(DYN_CFG_NAMES); k++) {
        JSValue ns = JS_NewObject(ctx);
        if (JS_IsException(ns))
            return -1;
        JS_SetPropertyFunctionList(ctx, ns, tabs[k], lens[k]);
        if (JS_SetModuleExport(ctx, m, DYN_CFG_NAMES[k], ns) < 0)
            return -1;
    }
    return 0;
}

int js_nat_init_config(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:config", dyn_config_init_module);
    size_t k;
    if (!m)
        return -1;
    for (k = 0; k < countof(DYN_CFG_NAMES); k++)
        JS_AddModuleExport(ctx, m, DYN_CFG_NAMES[k]);
    return 0;
}

#endif
