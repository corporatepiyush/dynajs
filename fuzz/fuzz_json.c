// Copyright 2025 Google LLC
// Fuzz target for the DynaJS JSON reader (JS_ParseJSON).
//
// The previous version wrapped the input in snprintf("JSON.parse(%s)")
// templates and fed the result to JS_Eval. That fuzzed the JS *parser*, not
// the JSON reader, and the "%s" formatting truncated the input at the first
// NUL byte, so binary inputs died early. This drives the C JSON reader
// (JS_ParseJSON / json_parse_value / json_next_token and the string & number
// lexers) directly on the RAW fuzzer buffer, preserving embedded NULs.
//
// NOTE: json_parse_value() is recursive and, unlike the bytecode reader
// (JS_ReadObjectRec), has NO js_check_stack_overflow guard -- deeply nested
// input recurses on the native C stack. Run this target with a bounded input
// length (e.g. -dict=fuzz/fuzz.dict -max_len=4096, which is also libFuzzer's
// default) so a nesting bomb cannot exhaust the 8 MiB native stack. The memory
// limit below caps pathological allocations so they fail cleanly instead of
// being reported as OOM.

#include "dynajs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Free a JS_ParseJSON result on every path, clearing any pending exception.
static void drop(JSContext *ctx, JSValue v) {
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

    // JS_ParseJSON requires buf[buf_len] == '\0' but honours buf_len, so
    // embedded NULs inside [0, size) are preserved (the '%s' harness lost them).
    char *buf = malloc(size + 1);
    if (!buf) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    if (size)
        memcpy(buf, data, size);
    buf[size] = '\0';

    // Strict JSON, then extended JSON (comments / trailing commas / ...).
    drop(ctx, JS_ParseJSON(ctx, buf, size, "<json-fuzz>"));
    drop(ctx, JS_ParseJSON2(ctx, buf, size, "<json-fuzz>", JS_PARSE_JSON_EXT));

    free(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
