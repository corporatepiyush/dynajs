/* A native that returns JS_EXCEPTION must leave an exception pending; when one does not, or when a value that is
   JS_EXCEPTION reaches a property store, the engine must not let the "no exception" sentinel escape as a value.
   Found by dyna:ai's heap-cap child (bugs.tsv 2026-11-27): an APIError built under out-of-memory stored the failed
   JS_NewString result (JS_EXCEPTION) as its message, String(e.message) then returned JS_EXCEPTION with nothing pending,
   the module's async body rejected with JS_UNINITIALIZED and the CLI printed "uninitialized". The same sweep found that a
   lazily created native method (JS_DEF_CFUNC, an autoinit property) whose first access hit out-of-memory became
   undefined for the rest of the run: every later a.feed() was "not a function".
   Cases:
     lost      a native returning JS_EXCEPTION without throwing leaves an InternalError pending once it returns into the
               engine: called from bytecode (caught in JS, inside a loop, rejecting an async function, uncaught from
               JS_Eval), through JS_Call, as a promise reaction job, and as a JS_NewCFunctionData function. JS_GetException
               itself stays a plain take: natives clear speculatively with it on hot paths (dyn_enc_view's
               view-then-ArrayBuffer probe), and building an error there cost 35x (encoding audit_w4, 77 -> 2700 ns).
     store     JS_SetProperty, JS_SetPropertyStr, JS_SetPropertyUint32, JS_SetPropertyInt64, JS_DefinePropertyValue,
               JS_DefinePropertyValueStr and JS_DefinePropertyValueUint32 refuse a JS_EXCEPTION value: -1, nothing stored.
     autoinit  the memory limit is swept byte range by byte range over the first access of a function-list method: a
               refused access throws (null or an error object, never JS_UNINITIALIZED) and the next access without a
               limit returns the function.
     syntax    JS_IsSyntaxError: true for JSON.parse's error and new SyntaxError, false for other errors, for an
               ordinary object inheriting SyntaxError.prototype and for non-objects.
   Built with ASan+UBSan by the ctest stage. Mutants (all red): drop the uninitialized check in js_call_c_function,
   js_call_cfunc_generic, js_c_function_data_call or at the interpreter's exception entry (lost); drop the exception-value check in any store wrapper (store); convert the
   autoinit slot before calling its initializer (autoinit); compare against the TypeError prototype (syntax). */
#include "dynajs.h"
#include "../lib/dyn-oomrt.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char* what)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "  FAIL %s\n", what);
    }
}

static JSValue js_lose(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_EXCEPTION;
}

static JSValue js_one(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, 1);
}

static JSValue js_lose_data(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)data;
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry FL[] = {
    JS_CFUNC_DEF("feed", 1, js_one),
};

static int eval_bool(JSContext* ctx, const char* src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<exception-lost>", JS_EVAL_TYPE_GLOBAL);
    int b;
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static int is_lost_error(JSContext* ctx, JSValueConst e)
{
    const char* s;
    int ok;
    if (!JS_IsError(ctx, e))
        return 0;
    s = JS_ToCString(ctx, e);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    ok = strstr(s, "InternalError: exception lost") == s;
    JS_FreeCString(ctx, s);
    return ok;
}

static void case_lost(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx), e, v, p, fn;
    check(!JS_HasException(ctx), "lost: nothing pending at start");
    fn = JS_NewCFunction(ctx, js_lose, "lose", 0);
    JS_SetPropertyStr(ctx, g, "lose", JS_DupValue(ctx, fn));
    JS_SetPropertyStr(ctx, g, "loseData", JS_NewCFunctionData(ctx, js_lose_data, 0, 0, 0, NULL));
    JS_SetPropertyStr(ctx, g, "lose2", JS_NewCFunction(ctx, js_lose, "lose2", 2));
    v = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    check(JS_IsException(v) && JS_HasException(ctx), "lost: JS_Call of the native leaves an exception pending");
    e = JS_GetException(ctx);
    check(is_lost_error(ctx, e), "lost: JS_Call's pending exception is an InternalError");
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, fn);
    {
        static const char* const names[] = { "lose2", "loseData" };
        int k;
        for (k = 0; k < 2; k++) {
            JSValue f2 = JS_GetPropertyStr(ctx, g, names[k]);
            v = JS_Call(ctx, f2, JS_UNDEFINED, 0, NULL);
            e = JS_HasException(ctx) ? JS_GetException(ctx) : JS_UNINITIALIZED;
            check(JS_IsException(v) && is_lost_error(ctx, e), k ? "lost: JS_Call of a JS_NewCFunctionData native leaves an InternalError" : "lost: JS_Call with fewer arguments than the native's length leaves an InternalError");
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, f2);
        }
    }
    check(eval_bool(ctx, "(() => { const o = { lose, loseData, lose2 }; let r = 0; for (let i = 0; i < 200; i++) for (const k of ['lose', 'loseData', 'lose2']) try { o[k](); } catch (e) { if (e instanceof InternalError) r++; } try { o.lose(); } catch (e) { if (e instanceof InternalError) r++; } return r === 601; })()") == 1,
        "lost: warm method calls of every native kind are each an InternalError");
    check(eval_bool(ctx, "(() => { try { loseData(); return false; } catch (e) { return e instanceof InternalError && /^exception lost/.test(e.message); } })()") == 1,
        "lost: a JS_NewCFunctionData native's loss is caught as an InternalError");
    check(eval_bool(ctx, "(() => { try { lose(); return false; } catch (e) { return e instanceof InternalError && /^exception lost/.test(e.message); } })()") == 1,
        "lost: JS catches the lost exception as an InternalError");
    check(eval_bool(ctx, "(() => { let r = 0; for (let i = 0; i < 3; i++) try { lose(); } catch (e) { if (e instanceof InternalError) r++; } return r === 3; })()") == 1,
        "lost: repeated losses inside a loop are each an InternalError");
    v = JS_Eval(ctx, "lose()", 6, "<exception-lost>", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(v), "lost: an uncaught loss fails the eval");
    e = JS_GetException(ctx);
    check(is_lost_error(ctx, e), "lost: the uncaught loss reaches the caller as an InternalError");
    JS_FreeValue(ctx, e);
    p = JS_Eval(ctx, "(async () => { await 0; lose(); })()", 36, "<exception-lost>", JS_EVAL_TYPE_GLOBAL);
    while (JS_IsJobPending(JS_GetRuntime(ctx))) {
        JSContext* c1;
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), &c1) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    check(JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED, "lost: the async function rejects");
    v = JS_PromiseResult(ctx, p);
    check(!JS_IsUninitialized(v) && is_lost_error(ctx, v), "lost: the rejection reason is an InternalError");
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    p = JS_Eval(ctx, "Promise.resolve().then(lose)", 28, "<exception-lost>", JS_EVAL_TYPE_GLOBAL);
    while (JS_IsJobPending(JS_GetRuntime(ctx))) {
        JSContext* c1;
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), &c1) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    check(JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED, "lost: a reaction job calling the native rejects");
    v = JS_PromiseResult(ctx, p);
    check(!JS_IsUninitialized(v) && is_lost_error(ctx, v), "lost: the reaction job's rejection reason is an InternalError");
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, g);
}

static void case_store(JSContext* ctx)
{
    JSValue o = JS_NewObject(ctx), a = JS_NewArray(ctx), g = JS_GetGlobalObject(ctx);
    JSAtom at = JS_NewAtom(ctx, "viaAtom");
    check(JS_SetProperty(ctx, o, at, JS_EXCEPTION) < 0, "store: JS_SetProperty refuses");
    check(JS_SetPropertyStr(ctx, o, "s", JS_EXCEPTION) < 0, "store: JS_SetPropertyStr refuses");
    check(JS_SetPropertyUint32(ctx, o, 3, JS_EXCEPTION) < 0, "store: JS_SetPropertyUint32 refuses");
    check(JS_SetPropertyInt64(ctx, o, 5, JS_EXCEPTION) < 0, "store: JS_SetPropertyInt64 refuses (small index)");
    check(JS_SetPropertyInt64(ctx, o, (int64_t)1 << 40, JS_EXCEPTION) < 0, "store: JS_SetPropertyInt64 refuses (large index)");
    check(JS_DefinePropertyValue(ctx, o, at, JS_EXCEPTION, JS_PROP_C_W_E) < 0, "store: JS_DefinePropertyValue refuses");
    check(JS_DefinePropertyValueStr(ctx, o, "d", JS_EXCEPTION, JS_PROP_C_W_E) < 0, "store: JS_DefinePropertyValueStr refuses");
    check(JS_DefinePropertyValueUint32(ctx, a, 0, JS_EXCEPTION, JS_PROP_C_W_E) < 0, "store: JS_DefinePropertyValueUint32 refuses on a fast array");
    check(JS_SetPropertyUint32(ctx, a, 0, JS_EXCEPTION) < 0, "store: JS_SetPropertyUint32 refuses on an array");
    JS_SetPropertyStr(ctx, g, "storeObj", JS_DupValue(ctx, o));
    JS_SetPropertyStr(ctx, g, "storeArr", JS_DupValue(ctx, a));
    check(eval_bool(ctx, "Reflect.ownKeys(storeObj).length === 0 && storeArr.length === 0") == 1, "store: nothing was stored");
    check(!JS_HasException(ctx), "store: a refused store leaves nothing pending of its own");
    check(JS_SetPropertyStr(ctx, o, "ok", JS_NewInt32(ctx, 7)) >= 0 && eval_bool(ctx, "storeObj.ok === 7") == 1, "store: CONTROL an ordinary value is stored");
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, g);
}

static void case_autoinit(JSRuntime* rt, JSContext* ctx)
{
    JSAtom feed = JS_NewAtom(ctx, "feed");
    JSMemoryUsage mu;
    size_t head;
    long refused = 0, ran = 0, bad_exc = 0, sticky = 0;
    for (head = 0; head < 8 * 1024; head += 8) {
        JSValue o = JS_NewObject(ctx), v;
        if (JS_IsException(o) || JS_SetPropertyFunctionList(ctx, o, FL, 1) < 0) {
            check(0, "autoinit: setup");
            break;
        }
        JS_RunGC(rt);
        JS_ComputeMemoryUsage(rt, &mu);
        oomrt_cap = (size_t)mu.malloc_size + head;
        v = JS_GetProperty(ctx, o, feed);
        oomrt_cap = (size_t)-1;
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            refused++;
            if (JS_IsUninitialized(e) || !(JS_IsNull(e) || JS_IsObject(e)))
                bad_exc++;
            JS_FreeValue(ctx, e);
            v = JS_GetProperty(ctx, o, feed);
            if (!JS_IsFunction(ctx, v))
                sticky++;
        } else {
            ran++;
            if (!JS_IsFunction(ctx, v))
                sticky++;
        }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, o);
    }
    JS_FreeAtom(ctx, feed);
    check(refused > 0 && ran > 0, "autoinit: the sweep crosses the failure boundary");
    check(bad_exc == 0, "autoinit: a refused first access throws null or an error object");
    check(sticky == 0, "autoinit: the method is a function on the next access after a refused one");
    printf("  autoinit: %ld headrooms refused, %ld ran, %ld left the method undefined\n", refused, ran, sticky);
}

static void case_syntax(JSContext* ctx)
{
    JSValue v = JS_ParseJSON(ctx, "{x", 2, "<syntax>"), e;
    check(JS_IsException(v), "syntax: bad JSON fails");
    e = JS_GetException(ctx);
    check(JS_IsSyntaxError(ctx, e), "syntax: JSON.parse's error is a SyntaxError");
    JS_FreeValue(ctx, e);
    JS_ThrowInternalError(ctx, "x");
    e = JS_GetException(ctx);
    check(!JS_IsSyntaxError(ctx, e), "syntax: an InternalError is not");
    JS_FreeValue(ctx, e);
    JS_ThrowTypeError(ctx, "x");
    e = JS_GetException(ctx);
    check(!JS_IsSyntaxError(ctx, e), "syntax: a TypeError is not");
    JS_FreeValue(ctx, e);
    v = JS_Eval(ctx, "new SyntaxError('s')", 20, "<syntax>", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsSyntaxError(ctx, v), "syntax: new SyntaxError is");
    JS_FreeValue(ctx, v);
    v = JS_Eval(ctx, "Object.create(SyntaxError.prototype)", 36, "<syntax>", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(v) && !JS_IsSyntaxError(ctx, v), "syntax: an ordinary object inheriting SyntaxError.prototype is not");
    JS_FreeValue(ctx, v);
    check(!JS_IsSyntaxError(ctx, JS_NULL) && !JS_IsSyntaxError(ctx, JS_UNDEFINED) && !JS_IsSyntaxError(ctx, JS_NewInt32(ctx, 1)), "syntax: non-objects are not");
}

int main(void)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx;
    if (!rt || !(ctx = JS_NewContext(rt))) {
        fprintf(stderr, "test_exception_lost: no runtime\n");
        return 1;
    }
    case_lost(ctx);
    case_store(ctx);
    case_autoinit(rt, ctx);
    case_syntax(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (failures) {
        fprintf(stderr, "test_exception_lost: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_exception_lost: %d checks passed\n", checks);
    return 0;
}
