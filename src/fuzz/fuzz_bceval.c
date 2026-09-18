// Copyright 2025 Google LLC
// Fuzz target for the FULL bytecode read -> link -> execute path
// (JS_ReadObject with JS_READ_OBJ_BYTECODE, then JS_EvalFunction).
//
// Deserializer-sweep finding F4 left a coverage gap: fuzz_bytecode.c reads
// attacker-controlled bytes but deliberately NEVER evaluates the result, and
// fuzz_compile.c only evaluates bytecode the fuzzer itself just wrote -- so no
// target exercises the reader's output under the interpreter. The bugs the
// sweep proved live in exactly that seam (closure-var var_idx/var_ref_idx and
// module export var_idx were accepted unchecked and crashed in js_closure2 /
// get_var_ref / module linking, i.e. during EVAL, not during READ). This
// target closes it: arbitrary bytes are deserialized as function/module
// bytecode and, on a successful read, evaluated on a full-intrinsics context,
// so closure/module index validation, cpool handling and opcode validation are
// all reachable from raw input.
//
// The input buffer handed to the reader is an EXACT-SIZE heap allocation with
// no slack: any byte read past buf_len lands in an ASan redzone. A fixed-size
// buffer would hide precisely the overread class fuzz_bytecode.c exists to
// catch. JS_ReadObject does not require NUL termination and the reader
// consumes nothing after the read returns, so the copy is freed before the
// eval and nothing can retain a pointer into it.
//
// test_one_input_init() supplies the eval-safety rails fuzz_eval/fuzz_compile
// run under: 64 MiB memory limit, capped stack, an interrupt handler that
// stops runaway loops, and a module loader. Exceptions are caught and freed
// -- a fuzz target produces no output.

#include "dynajs.h"
#include "dyna-libc.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reach counters. Printed ONLY when FUZZ_BCEVAL_STATS is set in the
   environment (a destructor; normal runs stay silent). This is the evidence
   that mutated garbage actually crosses the reader into JS_EvalFunction
   instead of dying on the version byte. */
static unsigned long long fz_reads_rej, fz_reads_ok, fz_evals, fz_evals_ok;

__attribute__((destructor)) static void fz_print_stats(void)
{
    const char *e = getenv("FUZZ_BCEVAL_STATS");
    if (!e || e[0] == '0')
        return;
    fprintf(stderr, "fuzz_bceval: reads_rej=%llu reads_ok=%llu evals=%llu "
                    "evals_ok=%llu\n",
            fz_reads_rej, fz_reads_ok, fz_evals, fz_evals_ok);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0)
        return 0;

    JSRuntime *rt = JS_NewRuntime();
    if (!rt)
        return 0;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    test_one_input_init(rt, ctx);

    /* Exact-size copy: the reader sees a heap block with no slack, so an
       out-of-bounds read past buf_len is an ASan heap-buffer-overflow. */
    uint8_t *buf = malloc(size);
    if (!buf) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    memcpy(buf, data, size);

    JSValue obj = JS_ReadObject(ctx, buf, size, JS_READ_OBJ_BYTECODE);
    free(buf);
    if (JS_IsException(obj)) {
        fz_reads_rej++;
        JS_FreeValue(ctx, JS_GetException(ctx));
        js_std_free_handlers(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    fz_reads_ok++;

    reset_nbinterrupts();
    /* Same link steps as js_std_eval_binary, minus the exit() on exception. */
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {
        if (JS_ResolveModule(ctx, obj) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, obj);
            js_std_free_handlers(rt);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 0;
        }
        js_module_set_import_meta(ctx, obj, 0, 1);
    }
    fz_evals++;
    JSValue val = JS_EvalFunction(ctx, obj); /* consumes obj */
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
        fz_evals_ok++;
        fuzz_loop(ctx); /* pump promise/module jobs; interrupt handler bounds it */
    }
    JS_FreeValue(ctx, val);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
