/* test_net_final_sweep.c -- forced-path pin for the finalizer-mode
 * "release, never call" guards (redis/pg/dns).
 *
 * THE ENTRY UNDER TEST. A resource has two dispose entries: the explicit
 * close() a live program calls, and the class finalizer the collector (or
 * the runtime's final sweep) calls. close() runs with a live context and must
 * SETTLE every pending command -- a promise that never settles strands its
 * awaiter -- while the finalizer entry runs on a teardown path where the
 * context may already be going away, and the guards (redis_in_final /
 * pg_in_final / dns_in_final) make it RELEASE the pending work rt-form
 * instead of calling JS. Settling from that entry would call into a context
 * whose destruction has begun (during JS_FreeRuntime, after JS_FreeContext)
 * and enqueue reaction jobs the dying loop never drains.
 *
 * WHAT THIS TEST FORCES -- AND WHAT IT DOES NOT. It forces the finalizer
 * ENTRY (the code path each guard sits on), not the runtime's exact shutdown
 * moment: the script's bindings are block-scoped and the host clears the
 * globals, so the host holds the only references, and dropping the last
 * reference runs the class finalizer -- with the command still in flight.
 * The collector is then run explicitly (JS_RunGC) so the entry is reached
 * deterministically, with the context still ALIVE. It cannot be reached by
 * literally freeing the context first: JS_FreeContext returns early while the
 * context still has realm references (a fresh context carries ~985, measured
 * in this tree), and a host-held reference roots the object graph so the
 * runtime's final sweep cannot free it either. A live context is also what
 * makes the guard OBSERVABLE: guarded, the in-flight promise must still be
 * PENDING (released, never settled); unguarded, the JS_Call succeeds and the
 * promise settles -- a named row failure either way. lldb-verified hit counts
 * on the finalizer/teardown entries confirm the rows really reach the guarded
 * branch (no vacuous pass).
 *
 * Rows (named; reverting a guard must fail at least one):
 *   redis-final-sweep   Redis, one PING in flight against a dead port.
 *   pg-final-sweep      PostgreSQL, one SELECT in flight against a dead port.
 *   dns-final-sweep     DNSResolver, one promise lookup in flight.
 *
 * The event loop is deliberately NEVER TURNED: the connect stays in flight
 * and every issued command stays queued, so "work in flight at the finalizer
 * entry" holds by construction. Each row is its own runtime, and exit code is
 * 0 only if every row printed ok AND no sanitizer reported. The row also
 * refuses to pass vacuously: if the finalizer never ran, the client's native
 * bytes would still be on the module ledger, and the row fails naming that.
 *
 * Build/run: make test-net-final-sweep (needs a CONFIG_NATIVE_MODULES=y tree;
 * under CONFIG_ASAN=y the same rows are the sanitizer gate for the rt-form
 * teardown frees -- run it with ASAN_OPTIONS=detect_leaks=1 on Darwin, where
 * LSan is off by default).
 */
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

/* One row: arm the resource, keep the host's references as the only ones,
 * then drop the client's -- which runs the finalizer with work in flight. */
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
    /* Clear what the script published: from here the host holds the only
     * references, so the drop below really reaches the finalizer. */
    JS_SetPropertyStr(ctx, g, "__held", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, g, "__promise", JS_UNDEFINED);
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(held) || !JS_IsObject(promise)) {
        row_fail(row, "scenario did not produce a client and a pending promise");
        goto done;
    }

    /* Precondition: the work is genuinely IN FLIGHT when the finalizer entry
     * runs (a promise that never started pending would make the check
     * vacuous). */
    st_before = JS_PromiseState(ctx, promise);
    if (st_before != JS_PROMISE_PENDING) {
        row_fail(row, "precondition: the command settled before the forced dispose");
        goto done;
    }

    /* THE FORCED ENTRY: drop the host's last reference to the client, then
     * run the collector -- the class finalizer runs with the command still
     * queued, which is the entry the guards exist for. */
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

    /* The guard's contract, read without a context: the promise was RELEASED,
     * never called. */
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
    /* EVERY exit path frees the per-runtime std handler state: it is heap
     * allocated by js_std_init_handlers (160 B per runtime) and freed only
     * here. Omitting it on the success path leaked one state block per row
     * under LSan -- a leak in the row, not in the code under test. */
    JS_FreeValueRT(rt, held);
    JS_FreeValueRT(rt, promise);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    /* The connect goes to a port nothing listens on, and the loop is never
     * turned -- so nothing is refused while the row runs and every command
     * stays queued in the client. The block keeps R and p out of the module
     * scope: the globals below are the script's only durable references, and
     * the host clears them. */
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
