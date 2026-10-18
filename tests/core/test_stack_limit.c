#include "dynajs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int eval_ok(JSRuntime* rt, const char* src)
{
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    int ok;

    if (!ctx)
        return 0;
    v = JS_Eval(ctx, src, strlen(src), "<stack-limit-probe>", JS_EVAL_TYPE_GLOBAL);
    ok = !JS_IsException(v);
    if (!ok) {
        JSValue e = JS_GetException(ctx);
        const char* m = JS_ToCString(ctx, e);
        fprintf(stderr, "  exception: %s\n", m ? m : "(?)");
        if (m)
            JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    return ok;
}

int main(void)
{
    JSRuntime* rt;
    int ok = 1;

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "test_stack_limit: no runtime\n");
        return 1;
    }

    JS_SetMaxStackSize(rt, SIZE_MAX);
    if (!eval_ok(rt, "1 + 1")) {
        fprintf(stderr, "test_stack_limit: oversized stack request rejected every call\n");
        ok = 0;
    }

    JS_SetMaxStackSize(rt, (size_t)1 << 20);
    if (!eval_ok(rt, "var s = 0; for (var i = 0; i < 1000; i++) s += i; s")) {
        fprintf(stderr, "test_stack_limit: sane limit broke evaluation\n");
        ok = 0;
    }
    if (!eval_ok(rt, "try { (function f() { return 1 + f(); })(); 0 } catch (e) { 1 }")) {
        fprintf(stderr, "test_stack_limit: deep recursion escaped as an uncatchable failure\n");
        ok = 0;
    }

    JS_FreeRuntime(rt);
    if (!ok) {
        fprintf(stderr, "test_stack_limit: FAIL\n");
        return 1;
    }
    printf("test_stack_limit: ok (oversized JS_SetMaxStackSize does not underflow the limit)\n");
    return 0;
}
