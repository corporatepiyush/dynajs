#include "cutils.h"
#include "dyna-libc.h"
#include "repl_src.inc"

int dyna_repl_eval(JSContext* ctx)
{
    JSValue obj = JS_Eval(ctx, dyna_repl_src, sizeof(dyna_repl_src) - 1, "<repl>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    JSValue val;
    if (JS_IsException(obj))
        return -1;
    if (js_module_set_import_meta(ctx, obj, FALSE, TRUE) < 0) {
        JS_FreeValue(ctx, obj);
        return -1;
    }
    val = js_std_await(ctx, JS_EvalFunction(ctx, obj));
    if (JS_IsException(val))
        return -1;
    JS_FreeValue(ctx, val);
    return 0;
}
