/* tests/park_sweep.c -- the JS_AddShutdownSweep-at-park template (the
 * file_job_sweep / pipe_op_sweep handover pattern) adopted by a NET-STYLE
 * park: a promise parked across the event loop with C-pinned resolve/reject.
 * The permanent fixture behind tests/test_shutdown_repark.js. Validates the
 * template's five elements:
 *   1. register the sweep at EVERY park (idempotent per (func, opaque))
 *   2. remove at EVERY normal settle
 *   3. sweep: fail the park (ctx) or release without settling (straggler),
 *      then defer-free the shell a stale settle may still name
 *   4. settle entry points walk away on the dead flag
 *   5. normal settle: undefer + free the shell exactly once
 *
 * Exports: park() / parkKeep() (the adapter pattern above) and parkRereg(),
 * the same park whose sweep re-registers the identical pair on every call
 * -- the bounded-walk row that proves the engine's sweep pass terminates
 * instead of spinning (JS_SHUTDOWN_SWEEP_PASS_MAX).
 */
#include "dynajs.h"
#include <stdlib.h>
#include <string.h>
#define countof(x) (sizeof(x) / sizeof((x)[0]))

typedef struct {
    JSValue resolve, reject;
    int dead;
    int settled;
} park_t;

static park_t *g_last = NULL;
static park_t *g_rereg_last = NULL;

static void park_fail(JSContext *ctx, JSRuntime *rt,
                      JSValue *resolve, JSValue *reject, const char *what)
{
    if (JS_IsUndefined(*resolve) && JS_IsUndefined(*reject))
        return;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc)) {
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, what), JS_PROP_WRITABLE |
                JS_PROP_CONFIGURABLE);
        } else {
            exc = JS_GetException(ctx);
        }
        r = JS_Call(ctx, *reject, JS_UNDEFINED, 1, (JSValueConst *)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, *resolve);
        JS_FreeValue(ctx, *reject);
    } else {
        JS_FreeValueRT(rt, *resolve);
        JS_FreeValueRT(rt, *reject);
    }
    *resolve = JS_UNDEFINED;
    *reject = JS_UNDEFINED;
}

static void park_sweep(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    park_t *p = (park_t *)opaque;
    park_fail(ctx, rt, &p->resolve, &p->reject,
              "probe.park: aborted at engine shutdown");
    p->dead = 1;
    JS_ShutdownDeferFree(rt, p);
}

/* The bounded-walk fixture: a sweep that re-registers the IDENTICAL
 * (func, opaque) pair on every call -- the shape the pass bound exists for
 * (JS_SHUTDOWN_SWEEP_PASS_MAX). The park is failed on the first call (so
 * the drain still runs and the failure is observable), the shell is
 * defer-owned, and then the sweep re-adds itself: the walk must terminate
 * at the cap and drop the entry WITHOUT calling it (nothing pinned is left
 * -- both pins are released above and the shell is engine-owned). Without
 * the bound this walk never sees an empty registry. */
static void park_sweep_rereg(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    park_t *p = (park_t *)opaque;
    park_fail(ctx, rt, &p->resolve, &p->reject,
              "probe.rereg: aborted at engine shutdown");
    p->dead = 1;
    JS_ShutdownDeferFree(rt, p);
    JS_AddShutdownSweep(rt, park_sweep_rereg, p);
}

/* The park shell: a promise, boxed resolve/reject, the sweep registered at
 * the park (the documented idiom), and the shell address kept in the slot
 * the settle entry point reads. */
static JSValue park_new(JSContext *ctx, JSShutdownSweepFunc sweep,
                        park_t **slot)
{
    JSValue funcs[2], promise;
    park_t *p;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    p = (park_t *)calloc(1, sizeof *p);
    if (!p) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];
    *slot = p;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), sweep, p);
    return promise;
}

/* the net-style park: never settles on its own */
static JSValue js_park(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return park_new(ctx, park_sweep, &g_last);
}

/* the same park whose sweep re-registers itself (the bounded-walk row) */
static JSValue js_park_rereg(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return park_new(ctx, park_sweep_rereg, &g_rereg_last);
}


/* the normal settle path: exactly-once, remove-then-free */
static JSValue js_settle(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    park_t *p = g_last;
    JSValue r;
    (void)this_val; (void)argc; (void)argv;
    if (!p || p->dead || p->settled)
        return JS_UNDEFINED;          /* dead-flag walk-away */
    p->settled = 1;
    JS_RemoveShutdownSweep(JS_GetRuntime(ctx), park_sweep, p);
    JS_ShutdownUndeferFree(JS_GetRuntime(ctx), p);
    {
        JSValue out = JS_NewInt32(ctx, 42);
        r = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, (JSValueConst *)&out);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, out);
    }
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    free(p);
    g_last = NULL;
    return JS_UNDEFINED;
}

/* keep the shell address observable for the stale-settle row */
static JSValue js_park_keep(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    return js_park(ctx, this_val, argc, argv);
}

static const JSCFunctionListEntry probe_funcs[] = {
    JS_CFUNC_DEF("park", 0, js_park),
    JS_CFUNC_DEF("parkKeep", 0, js_park_keep),
    JS_CFUNC_DEF("parkRereg", 0, js_park_rereg),
    JS_CFUNC_DEF("settle", 0, js_settle),
};

static int js_probe_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, probe_funcs, countof(probe_funcs));
}

JSModuleDef *js_init_module(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_probe_init);
    if (!m) return NULL;
    JS_AddModuleExportList(ctx, m, probe_funcs, countof(probe_funcs));
    return m;
}
