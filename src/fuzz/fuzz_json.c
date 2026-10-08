#include "dynajs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drop(JSContext* ctx, JSValue v)
{
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

    drop(ctx, JS_ParseJSON(ctx, buf, size, "<json-fuzz>"));
    drop(ctx, JS_ParseJSON2(ctx, buf, size, "<json-fuzz>", JS_PARSE_JSON_EXT));

    free(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
