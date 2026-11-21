#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>

static int rows, fails;

static void row_fail(const char* row, const char* why)
{
    fprintf(stderr, "FAIL row %s: %s\n", row, why);
    fails++;
}

static void row_ok(const char* row)
{
    printf("row %s: ok\n", row);
}

static void print_exc(JSContext* ctx)
{
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    fprintf(stderr, "  exception: %s\n", s ? s : "(?)");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

static void run_row(const char* row, const char* src)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue ret, g, held = JS_UNDEFINED, promise = JS_UNDEFINED;
    uint64_t base, before, after;
    int st_before, st_after;

    rows++;
    base = dyn_nat_bytes();
    rt = JS_NewRuntime();
    ctx = rt ? JS_NewContext(rt) : NULL;
    if (!rt || !ctx) {
        row_fail(row, "cannot create runtime/context");
        if (rt)
            JS_FreeRuntime(rt);
        return;
    }
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
        js_module_check_attributes, NULL);
#ifdef CONFIG_NATIVE_MODULES
    if (js_nat_init_redis(ctx) < 0) {
        print_exc(ctx);
        row_fail(row, "module init failed");
        goto done;
    }
#else
    row_fail(row, "built without CONFIG_NATIVE_MODULES");
    goto done;
#endif

    ret = JS_Eval(ctx, src, strlen(src), "<final-sweep>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        print_exc(ctx);
        row_fail(row, "scenario eval threw");
        goto done;
    }
    JS_FreeValue(ctx, ret);

    g = JS_GetGlobalObject(ctx);
    held = JS_GetPropertyStr(ctx, g, "__held");
    promise = JS_GetPropertyStr(ctx, g, "__promise");
    if (JS_IsException(held) || JS_IsException(promise)) {
        JS_FreeValue(ctx, g);
        print_exc(ctx);
        row_fail(row, "reading the scenario globals threw");
        goto done;
    }
    JS_SetPropertyStr(ctx, g, "__held", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, g, "__promise", JS_UNDEFINED);
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(held) || !JS_IsObject(promise)) {
        row_fail(row, "scenario did not produce a client and a pending promise");
        goto done;
    }

    st_before = JS_PromiseState(ctx, promise);
    if (st_before != JS_PROMISE_PENDING) {
        row_fail(row, "precondition: the command settled before the forced dispose");
        goto done;
    }

    before = dyn_nat_bytes();
    JS_FreeValueRT(rt, held);
    held = JS_UNDEFINED;
    JS_RunGC(rt);
    after = dyn_nat_bytes();
    if (after != before) {
        row_fail(row, "a dropped client with a query in flight was finalized: the in-flight pin is gone");
        goto done;
    }

    st_after = JS_PromiseState(NULL, promise);
    if (st_after != JS_PROMISE_PENDING) {
        row_fail(row, st_after == JS_PROMISE_REJECTED ? "the in-flight promise rejected after a gc of its dropped client"
                                                      : "the in-flight promise fulfilled after a gc of its dropped client");
        goto done;
    }
    JS_FreeValueRT(rt, promise);
    promise = JS_UNDEFINED;
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    after = dyn_nat_bytes();
    if (after > base)
        row_fail(row, "the shutdown sweep never released the pinned client: its native bytes outlive the runtime");
    else
        row_ok(row);
    return;

done:
    JS_FreeValueRT(rt, held);
    JS_FreeValueRT(rt, promise);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    run_row("redis-final-sweep",
        "import { Redis } from \"dyna:redis\";\n"
        "{\n"
        "  const R = new Redis({ host: \"127.0.0.1\", port: 1 });\n"
        "  const p = R.command(\"PING\");\n"
        "  globalThis.__held = R;\n"
        "  globalThis.__promise = p;\n"
        "  p.catch(() => {});   /* a reaction exists: a settle would enqueue a job */\n"
        "}\n");

    printf("test_redis_final_sweep: rows=%d fails=%d\n", rows, fails);
    return fails ? 1 : 0;
}
