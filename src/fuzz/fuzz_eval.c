#include "dynajs.h"
#include "dyna-libc.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    JSRuntime* rt = JS_NewRuntime();
    if (!rt)
        return 0;
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    test_one_input_init(rt, ctx);

    uint8_t* null_terminated_data = malloc(size + 1);
    if (!null_terminated_data) {
        js_std_free_handlers(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    memcpy(null_terminated_data, data, size);
    null_terminated_data[size] = 0;

    reset_nbinterrupts();
    JSValue val = JS_Eval(ctx, (const char*)null_terminated_data, size, "<none>", JS_EVAL_TYPE_GLOBAL);
    free(null_terminated_data);
    if (!JS_IsException(val)) {
        fuzz_loop(ctx);
        JS_FreeValue(ctx, val);
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
