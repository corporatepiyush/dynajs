#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>

static int rows, fails;

static void row_fail(const char *row, const char *why)
{
    fprintf(stderr, "FAIL row %s: %s\n", row, why);
    fails++;
}

static void row_ok(const char *row)
{
    printf("row %s: ok\n", row);
}

static void print_exc(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "  exception: %s\n", s ? s : "(?)");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

static void run_row(const char *row, const char *src)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret, g, held = JS_UNDEFINED, promise = JS_UNDEFINED;
    uint64_t before, after;
    int st_before, st_after;

    rows++;
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
    if (js_nat_init_net(ctx) < 0) {
        print_exc(ctx);
        row_fail(row, "js_nat_init_net failed");
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
    if (after >= before) {
        row_fail(row, "the finalizer never ran: the client's native bytes are "
                      "still on the ledger, so this row would prove nothing");
        goto done;
    }

    st_after = JS_PromiseState(NULL, promise);
    if (st_after == JS_PROMISE_PENDING) {
        row_ok(row);
    } else {
        row_fail(row, st_after == JS_PROMISE_REJECTED
                     ? "the finalizer settled the in-flight promise (rejected) -- "
                       "release, never call is gone"
                     : "the finalizer settled the in-flight promise (fulfilled) -- "
                       "release, never call is gone");
    }

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
            "import { Redis } from \"dyna:net\";\n"
            "{\n"
            "  const R = new Redis({ host: \"127.0.0.1\", port: 1 });\n"
            "  const p = R.command(\"PING\");\n"
            "  globalThis.__held = R;\n"
            "  globalThis.__promise = p;\n"
            "  p.catch(() => {});   /* a reaction exists: a settle would enqueue a job */\n"
            "}\n");
    run_row("pg-final-sweep",
            "import { PostgreSQL } from \"dyna:net\";\n"
            "{\n"
            "  const P = new PostgreSQL({ host: \"127.0.0.1\", port: 1, user: \"u\" });\n"
            "  const q = P.query(\"SELECT 1\");\n"
            "  globalThis.__held = P;\n"
            "  globalThis.__promise = q;\n"
            "  q.catch(() => {});\n"
            "}\n");
    run_row("dns-final-sweep",
            "import { DNSResolver } from \"dyna:net\";\n"
            "{\n"
            "  const D = new DNSResolver({ server: \"127.0.0.1\", port: 1, timeoutMs: 60000 });\n"
            "  const p = D.lookup(\"final-sweep.test\");\n"
            "  globalThis.__held = D;\n"
            "  globalThis.__promise = p;\n"
            "  p.catch(() => {});\n"
            "}\n");

    printf("test_net_final_sweep: rows=%d fails=%d\n", rows, fails);
    return fails ? 1 : 0;
}
