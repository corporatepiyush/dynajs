// covers: c:src/builtins/builtin-promise.c c:src/mm/js_malloc.inc.c c:src/serialize/bc_read.inc.c c:src/parser/parser.inc.c c:src/vm/interpreter.inc.c c:src/vm/tailcall.inc.c -- the uncatchable interrupt through the C API
// The interrupt handler fires exactly once, on its third call (the first calls
// can land in the setup code; every case ends in an endless loop), and no CLI
// deadline backs it up, so a path that turns the uncatchable "interrupted"
// error into a rejection, or consumes it, is visible here even when the CLI
// would still stop the script.
// Each case: arm the handler, run the source (and the C call), drain every job,
// then require (a) the interrupt surfaced as an uncatchable exception from
// JS_Eval, the C call, or JS_ExecutePendingJob, and (b) no user code observed
// it (globalThis.leak stays undefined even though the remaining jobs ran).
// Cases: JS_PromiseResolveValue with a spinning then getter, a native reaction
// whose callback spins, a top-level-await module that spins before its first
// await, Array.fromAsync resolving through a spinning then getter, and a class
// heritage or computed key in sloppy code (the catch that restores the frame's
// sloppy mode must not catch the interrupt) (the job
// runner reports a job that returns normally with the interrupt still pending).
#include "dynajs.h"

#include <stdio.h>
#include <string.h>

static int fire_armed, fired, calls;
static int fails, checks;

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (fire_armed && !fired && ++calls >= 3) {
        fired = 1;
        return 1;
    }
    return 0;
}

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: [%s] %s\n", name, what);
    }
}

static int take_interrupt(JSContext* ctx)
{
    JSValue e = JS_GetException(ctx);
    const char* m = JS_ToCString(ctx, e);
    if (!m)
        JS_FreeValue(ctx, JS_GetException(ctx));
    int ok = m && strstr(m, "interrupted") != NULL;
    if (m)
        JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
    return ok;
}

static JSValue spinning_reaction(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    (void)is_reject;
    return JS_Call(ctx, data, JS_UNDEFINED, 1, &arg);
}

enum { C_NONE, C_RESOLVE_VALUE, C_NATIVE_REACTION };

static void run_case(const char* name, const char* src, int module, int capi)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSContext* jctx;
    JSValue v, g, leak;
    int surfaced = 0, jobs = 0, r;

    JS_SetInterruptHandler(rt, handler, NULL);
    fire_armed = 1;
    fired = 0;
    calls = 0;
    v = JS_Eval(ctx, src, strlen(src), "<interrupt-api>", module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        surfaced |= take_interrupt(ctx);
    } else if (capi != C_NONE) {
        g = JS_GetGlobalObject(ctx);
        if (capi == C_RESOLVE_VALUE) {
            JSValue obj = JS_GetPropertyStr(ctx, g, "obj");
            JSValue p = JS_PromiseResolveValue(ctx, obj);
            if (JS_IsException(p))
                surfaced |= take_interrupt(ctx);
            JS_FreeValue(ctx, p);
            JS_FreeValue(ctx, obj);
        } else {
            JSValue fn = JS_GetPropertyStr(ctx, g, "spinFn");
            JSValue rx = JS_NewPromiseReaction(ctx, spinning_reaction, fn, 0);
            if (JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0) < 0)
                surfaced |= take_interrupt(ctx);
            JS_FreeValue(ctx, rx);
            JS_FreeValue(ctx, fn);
        }
        JS_FreeValue(ctx, g);
    }
    JS_FreeValue(ctx, v);
    while (jobs < 10000 && (r = JS_ExecutePendingJob(rt, &jctx)) != 0) {
        jobs++;
        if (r < 0)
            surfaced |= take_interrupt(jctx);
    }
    g = JS_GetGlobalObject(ctx);
    leak = JS_GetPropertyStr(ctx, g, "leak");
    printf("  %s: fired=%d surfaced=%d leak=%s jobs=%d\n", name, fired, surfaced, JS_IsUndefined(leak) ? "no" : "yes", jobs);
    check(fired, name, "the interrupt handler fired");
    check(surfaced, name, "the interrupt surfaced as an exception (JS_Eval, the C call or a job)");
    check(JS_IsUndefined(leak), name, "no user code observed the interrupt");
    JS_FreeValue(ctx, leak);
    JS_FreeValue(ctx, g);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    run_case("JS_PromiseResolveValue then getter",
        "globalThis.obj = { get then() { for (;;) {} } };", 0, C_RESOLVE_VALUE);
    run_case("native reaction callback",
        "globalThis.spinFn = () => { for (;;) {} };", 0, C_NATIVE_REACTION);
    run_case("top-level await module, spin before await",
        "try { for (;;) {} } catch (e) { globalThis.leak = 1; }\nawait 0;", 1, C_NONE);
    run_case("Array.fromAsync iterable resolve then getter",
        "Object.defineProperty(Array.prototype, 'then', { get() { for (;;) {} }, configurable: true });\n"
        "Array.fromAsync([1]).catch(() => { globalThis.leak = 1; });",
        0, C_NONE);
    run_case("Array.fromAsync array-like resolve then getter",
        "Object.defineProperty(Array.prototype, 'then', { get() { for (;;) {} }, configurable: true });\n"
        "Array.fromAsync({ length: 1, 0: 1 }).catch(() => { globalThis.leak = 1; });",
        0, C_NONE);
    run_case("Array.fromAsync return() result then getter",
        "Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; },"
        " return() { return { get then() { for (;;) {} } }; } }; } }, () => { throw 1; })"
        ".catch(() => { globalThis.leak = 1; });",
        0, C_NONE);
    run_case("async function then getter on return value",
        "(async () => ({ get then() { for (;;) {} } }))().catch(() => { globalThis.leak = 1; });", 0, C_NONE);
    run_case("class computed key spin in sloppy global code",
        "try { (class { [(() => { for (;;) {} })()]() {} }); } catch (e) { globalThis.leak = 1; }\nleak2 = 1;", 0, C_NONE);
    run_case("class heritage spin in a sloppy function",
        "(function () { try { (class extends (() => { for (;;) {} })() {}); } catch (e) { globalThis.leak = 1; } })();", 0, C_NONE);
    run_case("class computed key spin in a resumed generator",
        "const it = (function* () { try { (class { [(yield, (() => { for (;;) {} })())]() {} }); } catch (e) { globalThis.leak = 1; } })();\nit.next(); it.next();", 0, C_NONE);
    printf("test_interrupt_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
