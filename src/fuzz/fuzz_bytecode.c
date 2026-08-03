// Copyright 2025 Google LLC
// Fuzz target for the DynaJS bytecode / object deserializer (JS_ReadObject).
//
// The previous version round-tripped TRUSTED bytecode through
// JS_WriteObject/JS_ReadObject and only fed a lightly-mutated copy to the
// reader, so it barely reached the real deserializer on attacker-controlled
// bytes. This drives JS_ReadObject on the RAW fuzzer buffer, so the reader
// itself -- JS_ReadObjectRec, JS_ReadFunctionTag, JS_ReadString,
// JS_ReadBigInt, the typed-array / module readers and the atom fixups -- is
// fuzzed on fully attacker-controlled input.
//
// Flag sets exercised:
//   * JS_READ_OBJ_BYTECODE               function/module bytecode reader; this
//     path is reachable in embedders that deserialize precompiled code and is
//     therefore an untrusted-input surface.
//   * JS_READ_OBJ_BYTECODE|..._REFERENCE additionally reaches the object
//     reference-table decoder.
//   * 0                                  the untrusted object-graph reader
//     (arrays, typed arrays, dates, maps, bigints, ...); bytecode tags are
//     rejected here, exercising the "plain object" path.
//
// JS_ReadObject does not require NUL termination and is fully bounds-checked
// against buf_len (bc_get_* -> bc_read_error_end), so the raw const buffer is
// passed directly with no copy. JS_ReadObjectRec is guarded by
// js_check_stack_overflow, so nesting bombs fail cleanly rather than crashing.
// NOTE: the deserialized value is intentionally NOT evaluated -- executing
// attacker-controlled bytecode is a different (trusted-input) threat model and
// would only add interpreter noise; the goal here is the reader.

#include "dynajs.h"

#include <stdint.h>
#include <stdlib.h>

// Read the raw buffer under one flag set and free the result / any exception.
static void read_and_drop(JSContext *ctx, const uint8_t *buf, size_t len, int flags) {
    JSValue v = JS_ReadObject(ctx, buf, len, flags);
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

    read_and_drop(ctx, data, size, JS_READ_OBJ_BYTECODE);
    read_and_drop(ctx, data, size, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE);
    read_and_drop(ctx, data, size, 0);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
