/*
 * fff1.c — test-only native module registering the hazardous arity shape:
 * a C function with cproto JS_CFUNC_f_f_f (the dispatcher reads argv[1]
 * unconditionally) and declared length 1 (so a 1-argument call skips
 * js_call_c_function's arity-pad alloca and hands the caller's argv straight
 * through). Used by rv04_native_fff1.js to pin the fused-call argv pad
 * (mf_argv1[2], slot [1] = JS_UNDEFINED) under ASan.
 */
#include "dynajs.h"

#define countof(x) (sizeof(x) / sizeof((x)[0]))

/* plain double(double,double) — the js_call_c_function DISPATCHER does the
 * JS_ToFloat64 conversions itself (reading arg_buf[0] AND arg_buf[1]
 * unconditionally for this cproto), so the C function never touches argv and
 * the probe isolates the dispatcher's argv-arity contract. */
static double js_fff1(double a, double b)
{
    return a + b;
}

static const JSCFunctionListEntry js_fff1_funcs[] = {
    /* raw entry: cproto JS_CFUNC_f_f_f with declared length 1 — the shape
       that skips js_call_c_function's arity-pad alloca at argc == 1 */
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
