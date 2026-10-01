#include "dynajs.h"
#include "dyna-libc.h"
#include "cutils.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    test_one_input_init(rt, ctx);

    uint8_t* null_terminated_data = malloc(size + 1);
    memcpy(null_terminated_data, data, size);
    null_terminated_data[size] = 0;

    JSValue obj = JS_Eval(ctx, (const char*)null_terminated_data, size, "<none>", JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
    free(null_terminated_data);
    if (JS_IsException(obj)) {
        js_std_free_handlers(rt);
        JS_FreeValue(ctx, obj);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    obj = js_std_await(ctx, obj);
    size_t bytecode_size;
    uint8_t* bytecode = JS_WriteObject(ctx, &bytecode_size, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);
    if (!bytecode) {
        js_std_free_handlers(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    obj = JS_ReadObject(ctx, bytecode, bytecode_size, JS_READ_OBJ_BYTECODE);
    js_free(ctx, bytecode);
    if (JS_IsException(obj)) {
        js_std_free_handlers(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    reset_nbinterrupts();
    JSValue val;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {
        if (JS_ResolveModule(ctx, obj) < 0) {
            JS_FreeValue(ctx, obj);
            js_std_free_handlers(rt);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 0;
        }
        js_module_set_import_meta(ctx, obj, FALSE, TRUE);
    }
    val = JS_EvalFunction(ctx, obj);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
    } else {
        fuzz_loop(ctx);
    }
    JS_FreeValue(ctx, val);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    return 0;
}
