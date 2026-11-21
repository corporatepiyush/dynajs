#include "json-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_JSON)

int js_nat_init_json(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:json", dyn_jp_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_jp_funcs,
        countof(dyn_jp_funcs));
}

#endif
