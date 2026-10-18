#include "dynajs.h"
#include "dyna-libc.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long fz_reads_rej, fz_reads_ok, fz_evals, fz_evals_ok;

__attribute__((destructor)) static void fz_print_stats(void)
{
    const char* e = getenv("FUZZ_BCEVAL_STATS");
    if (!e || e[0] == '0')
        return;
    fprintf(stderr, "fuzz_bceval: reads_rej=%llu reads_ok=%llu evals=%llu "
                    "evals_ok=%llu\n",
        fz_reads_rej, fz_reads_ok, fz_evals, fz_evals_ok);
}

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

    uint8_t* buf = malloc(size);
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
    JSValue val = JS_EvalFunction(ctx, obj);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
        fz_evals_ok++;
        fuzz_loop(ctx);
    }
    JS_FreeValue(ctx, val);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
