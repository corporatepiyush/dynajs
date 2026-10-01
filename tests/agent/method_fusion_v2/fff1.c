#include "dynajs.h"

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static double js_fff1(double a, double b)
{
    return a + b;
}

static const JSCFunctionListEntry js_fff1_funcs[] = {
    { "fff1", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0,
      .u = { .func = { 1, JS_CFUNC_f_f_f, { .f_f_f = js_fff1 } } } },
};

static int js_fff1_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_fff1_funcs,
                                  countof(js_fff1_funcs));
}

JSModuleDef *js_init_module(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, js_fff1_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_fff1_funcs, countof(js_fff1_funcs));
    return m;
}
