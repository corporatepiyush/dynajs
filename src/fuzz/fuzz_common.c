#include <stdlib.h>
#include <string.h>

#include "src/fuzz/fuzz_common.h"

void fuzz_drain_or_abort(JSContext* ctx, JSValueConst v)
{
    fuzz_loop(ctx);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        abort();
    }
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_Throw(ctx, JS_DupValue(ctx, JS_PromiseResult(ctx, v)));
        (void)err;
        js_std_dump_error(ctx);
        abort();
    }
}

static int interrupt_handler(JSRuntime* rt, void* opaque)
{
    nbinterrupts++;
    return (nbinterrupts > 100);
}

void reset_nbinterrupts()
{
    nbinterrupts = 0;
}

void fuzz_loop(JSContext* ctx)
{
    int err;
    for (;;) {
        err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
        if (err <= 0) {
            if (err < 0)
                js_std_dump_error(ctx);
            break;
        }
    }
}

void test_one_input_init(JSRuntime* rt, JSContext* ctx)
{
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x10000);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);
    JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);

    js_std_init_handlers(rt);
    reset_nbinterrupts();
}
