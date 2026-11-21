#include "dynajs.h"
#include "src/fuzz/fuzz_common.h"

#define FZ_COVER_ENGINE 1

#include "src/fuzz/jsgen.inc.c"

#include <stdint.h>

static fz_env_t env;

static void run(fz_env_t* e, const uint8_t* data, size_t size)
{
    fz_in_t in = { data, size };
    fz_sb_t s = { 0 };
    JSContext* ctx = e->ctx[0];
    unsigned mode = fz_u8(&in);
    int flags;
    if (mode & 1)
        fz_js_program(&in, &s);
    else
        fz_sb_put(&s, (const char*)in.p, in.n);
    if (fz_print())
        fprintf(stderr, "---- program (mode %u)\n%s\n----\n", mode, s.b ? s.b : "");
    flags = (mode & 2) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    if (mode & 4)
        flags |= JS_EVAL_FLAG_STRICT;
    fz_arm(50000000u, 0);
    fz_settle(ctx, JS_Eval(ctx, s.b ? s.b : "", s.n, "<eval-fuzz>", flags), "interrupt fired but JS_Eval completed normally");
    free(s.b);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size && size <= 65536)
        fz_one(&env, data, size);
    return 0;
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    fz_args(*argc, *argv);
    fz_env_open(&env, 1, 32, NULL, run);
    return 0;
}
