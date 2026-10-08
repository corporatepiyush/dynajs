#include "dynajs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void compile_and_drop(JSContext* ctx, const char* buf, size_t len, int mode)
{
    JSValue v = JS_Eval(ctx, buf, len, "<module-fuzz>", mode | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt = JS_NewRuntime();
    if (!rt)
        return 0;
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);

    char* buf = malloc(size + 1);
    if (!buf) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    if (size)
        memcpy(buf, data, size);
    buf[size] = '\0';

    compile_and_drop(ctx, buf, size, JS_EVAL_TYPE_MODULE);
    compile_and_drop(ctx, buf, size, JS_EVAL_TYPE_GLOBAL);

    free(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
