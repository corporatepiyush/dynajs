// Copyright 2025 Google LLC
// Fuzz target for the DynaJS module + script parser on raw input.
//
// The previous version wrapped the input in snprintf("export ... %s", input)
// templates. That truncated the input at the first NUL byte and only ever fed
// a handful of fixed syntactic shells to the parser. This feeds the RAW fuzzer
// buffer (length-delimited, so embedded NULs survive) straight to the parser,
// in both module and plain-script mode, with JS_EVAL_FLAG_COMPILE_ONLY so the
// parser + codegen is fuzzed on arbitrary bytes without executing anything
// (no interrupt handler / event loop needed).

#include "dynajs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Compile the raw buffer under one eval mode and free the result / exception.
static void compile_and_drop(JSContext *ctx, const char *buf, size_t len, int mode) {
    JSValue v = JS_Eval(ctx, buf, len, "<module-fuzz>", mode | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt)
        return 0;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    // Cap pathological allocations / recursion (64 MiB, 256 KiB stack).
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);

    // JS_Eval requires buf[buf_len] == '\0' but honours buf_len, so embedded
    // NULs inside [0, size) are preserved (the '%s' harness lost them).
    char *buf = malloc(size + 1);
    if (!buf) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    if (size)
        memcpy(buf, data, size);
    buf[size] = '\0';

    // Module grammar (import/export) then plain-script grammar; parse only.
    compile_and_drop(ctx, buf, size, JS_EVAL_TYPE_MODULE);
    compile_and_drop(ctx, buf, size, JS_EVAL_TYPE_GLOBAL);

    free(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
