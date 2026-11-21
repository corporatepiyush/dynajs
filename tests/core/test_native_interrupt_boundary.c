// covers: c:src/object/shapes_objects_gc.inc.c c:src/object/property_set_convert.inc.c c:src/vm/interpreter.inc.c c:src/vm/tailcall.inc.c -- the interrupt survives a native that swallows it
// A native that takes the uncatchable "interrupted" exception (JS_GetException) and then returns a value, a
// catchable error or a rejected promise is turned back into the uncatchable interrupt at its call boundary: the
// runtime counts every uncatchable exception JS_GetException hands out and each native call compares the count.
// The handler fires once, on its third poll, so only the boundary check can stop the script after the swallow.
// Swallow cases (each must end the eval with an uncatchable exception, no catch block run, no code after it run):
//  plain, method (any argument, no argument, local argument: the inline fast path), magic, getter, cfunc-data, bound, Reflect.apply, Array.prototype.map callback, nested natives,
//  convert (TypeError thrown instead), rethrow (same value rethrown catchable), reject (rejected promise returned),
//  iterator-next (a JS_ITERATOR_NEXT_DEF native reached by for-of), method0-hot and method1-hot (a hot loop: the
//  tail-call interpreter's method fast path; spinArm arms the one-shot handler only inside the swallowed call).
//  await-method0 and await-method1 (module only): a method call in a frame resumed after an await, which runs on the
//  tail-call interpreter (js_tc_op2_call_method0 under async_func_resume).
// test-native-interrupt-boundary runs the engine as built (tail-call interpreter when tools/tcprobe passes; every opcode
// then runs there); test-native-interrupt-boundary-switch builds it without CONFIG_TAILCALL_INTERP (switch interpreter). Every case also runs as module code (tail-call
//  interpreter: module bodies run there, script code mostly does not).
// regexp-ctor, regexp-ctor-v, regexp-literal: the handler fires inside lre_compile; the compile error is the uncatchable
// interrupt, not a catchable SyntaxError (fuzz_eval found the swallow; seed src/fuzz/corpus_eval).
// Negative cases: a native that swallows a catchable error is unchanged; an embedder that took the interrupt at
// top level does not trip later native calls; a native that propagates the interrupt keeps the original error.
#include "dynajs.h"

#include <stdio.h>
#include <string.h>

static int armed, fired, polls;
static int fails, checks;

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (armed && !fired && ++polls >= 3) {
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
        printf("FAIL: %s: %s\n", name, what);
    }
}

static JSValue call_arg(JSContext* ctx, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_UNDEFINED;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue swallow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = call_arg(ctx, argc, argv);
    (void)this_val;
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return r;
}

static JSValue swallow_magic(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)magic;
    return swallow(ctx, this_val, argc, argv);
}

static JSValue swallow_data(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)magic;
    (void)data;
    return swallow(ctx, this_val, argc, argv);
}

static JSValue swallow_getter(JSContext* ctx, JSValueConst this_val)
{
    JSValue g = JS_GetGlobalObject(ctx), spin = JS_GetPropertyStr(ctx, g, "spin"), r;
    (void)this_val;
    JS_FreeValue(ctx, g);
    if (JS_IsException(spin))
        return JS_EXCEPTION;
    r = JS_Call(ctx, spin, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, spin);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NewInt32(ctx, 1);
    }
    return r;
}

static JSValue swallow0(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return swallow_getter(ctx, this_val);
}

static JSValue swallow_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int* pdone, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    *pdone = 1;
    return swallow_getter(ctx, this_val);
}

static JSValue convert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = call_arg(ctx, argc, argv);
    (void)this_val;
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "converted");
    }
    return r;
}

static JSValue rethrow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = call_arg(ctx, argc, argv);
    (void)this_val;
    if (JS_IsException(r))
        return JS_Throw(ctx, JS_GetException(ctx));
    return r;
}

static JSValue reject(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = call_arg(ctx, argc, argv), fns[2], p, e, s;
    (void)this_val;
    if (!JS_IsException(r))
        return r;
    e = JS_GetException(ctx);
    p = JS_NewPromiseCapability(ctx, fns);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, e);
        return JS_EXCEPTION;
    }
    s = JS_Call(ctx, fns[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, fns[0]);
    JS_FreeValue(ctx, fns[1]);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, s);
    return p;
}

static JSValue arm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    armed = 1;
    polls = 0;
    return JS_UNDEFINED;
}

static JSValue propagate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return call_arg(ctx, argc, argv);
}

static const JSCFunctionListEntry obj_funcs[] = {
    JS_CFUNC_DEF("sw", 1, swallow),
    JS_CFUNC_DEF("sw0", 0, swallow0),
    JS_ITERATOR_NEXT_DEF("next", 0, swallow_next, 0),
    JS_CGETSET_DEF("g", swallow_getter, NULL),
};

static int setup(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx), o = JS_NewObject(ctx), v;
    int ok = !JS_IsException(o);
    if (ok)
        JS_SetPropertyFunctionList(ctx, o, obj_funcs, 4);
    ok = ok && JS_SetPropertyStr(ctx, g, "o", o) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "swallow", JS_NewCFunction(ctx, swallow, "swallow", 1)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "swallowMagic", JS_NewCFunctionMagic(ctx, swallow_magic, "swallowMagic", 1, JS_CFUNC_generic_magic, 3)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "swallowData", JS_NewCFunctionData(ctx, swallow_data, 1, 0, 0, NULL)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "convert", JS_NewCFunction(ctx, convert, "convert", 1)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "rethrow", JS_NewCFunction(ctx, rethrow, "rethrow", 1)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "reject", JS_NewCFunction(ctx, reject, "reject", 1)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "propagate", JS_NewCFunction(ctx, propagate, "propagate", 1)) >= 0;
    ok = ok && JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, arm, "arm", 0)) >= 0;
    JS_FreeValue(ctx, g);
    v = JS_Eval(ctx, "globalThis.spin = () => { for (;;); };", strlen("globalThis.spin = () => { for (;;); };"), "<setup>", 0);
    ok = ok && !JS_IsException(v);
    JS_FreeValue(ctx, v);
    v = JS_Eval(ctx, "globalThis.spinArm = () => { arm(); for (;;); };", strlen("globalThis.spinArm = () => { arm(); for (;;); };"), "<setup>", 0);
    ok = ok && !JS_IsException(v);
    JS_FreeValue(ctx, v);
    return ok;
}

static int global_is_undefined(JSContext* ctx, const char* name)
{
    JSValue g = JS_GetGlobalObject(ctx), v = JS_GetPropertyStr(ctx, g, name);
    int r = JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, g);
    return r;
}

static int message_is(JSContext* ctx, JSValueConst e, const char* want)
{
    JSValue m = JS_GetPropertyStr(ctx, e, "message");
    const char* s = JS_ToCString(ctx, m);
    int r = s && strcmp(s, want) == 0;
    if (s)
        JS_FreeCString(ctx, s);
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, m);
    return r;
}

static void drain(JSRuntime* rt)
{
    JSContext* c;
    int guard;
    for (guard = 0; guard < 1000; guard++) {
        int r = JS_ExecutePendingJob(rt, &c);
        if (r == 0)
            break;
        if (r < 0)
            JS_FreeValue(c, JS_GetException(c));
    }
}

static void swallow_case(const char* name, const char* expr, int module)
{
    char src[512];
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    int n;
    armed = fired = polls = 0;
    JS_SetInterruptHandler(rt, handler, NULL);
    check(setup(ctx), name, "setup");
    n = snprintf(src, sizeof src, "try { %s; } catch (e) { globalThis.caught = 1; } globalThis.leak = 1;", expr);
    armed = strstr(expr, "spinArm") == NULL;
    v = JS_Eval(ctx, src, (size_t)n, "<case>", module ? JS_EVAL_TYPE_MODULE : 0);
    if (module && !JS_IsException(v)) {
        JSContext* c;
        int guard;
        for (guard = 0; guard < 100; guard++) {
            int r = JS_ExecutePendingJob(rt, &c);
            if (r <= 0) {
                if (r < 0) {
                    JS_FreeValue(ctx, v);
                    v = JS_EXCEPTION;
                }
                break;
            }
        }
    }
    armed = 0;
    check(fired, name, "the interrupt handler fired");
    check(JS_IsException(v), name, "the eval (or a module job) ends with an exception");
    check(JS_HasUncatchableException(ctx), name, "the exception is the uncatchable interrupt");
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        check(message_is(ctx, e, "interrupted"), name, "the exception is the interrupt error");
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    check(global_is_undefined(ctx, "caught"), name, "no catch block saw the interrupt");
    check(global_is_undefined(ctx, "leak"), name, "no code after the swallowing native ran");
    drain(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_catchable(void)
{
    static const char src[] = "globalThis.r = swallow(() => { throw 1; }); globalThis.s = o.sw(() => 5); globalThis.done = 1;";
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    armed = fired = polls = 0;
    JS_SetInterruptHandler(rt, handler, NULL);
    check(setup(ctx), "catchable", "setup");
    v = JS_Eval(ctx, src, sizeof src - 1, "<catchable>", 0);
    check(!JS_IsException(v), "catchable", "a native that swallows a catchable error returns normally");
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    check(!global_is_undefined(ctx, "done"), "catchable", "the script ran to its end");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_embedder_taken(void)
{
    static const char src[] = "globalThis.r = o.sw(() => 7) + swallow(() => 1) + swallowData(() => 1) + o.g + [1].map(x => x)[0];";
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    int i;
    armed = fired = polls = 0;
    JS_SetInterruptHandler(rt, handler, NULL);
    check(setup(ctx), "embedder", "setup");
    v = JS_Eval(ctx, "globalThis.spin = () => 0;", strlen("globalThis.spin = () => 0;"), "<s>", 0);
    JS_FreeValue(ctx, v);
    for (i = 0; i < 2; i++) {
        armed = 1;
        fired = polls = 0;
        v = JS_Eval(ctx, "for (;;);", 9, "<spin>", 0);
        armed = 0;
        check(JS_IsException(v) && JS_HasUncatchableException(ctx), "embedder", "the interrupt reached the embedder");
        JS_FreeValue(ctx, JS_GetException(ctx));
        v = JS_Eval(ctx, src, sizeof src - 1, "<after>", 0);
        check(!JS_IsException(v), "embedder", "native calls after the embedder took the interrupt return normally");
        if (JS_IsException(v))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, v);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_propagate(void)
{
    static const char src[] = "try { propagate(spin); } catch (e) { globalThis.caught = 1; } globalThis.leak = 1;";
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    armed = fired = polls = 0;
    JS_SetInterruptHandler(rt, handler, NULL);
    check(setup(ctx), "propagate", "setup");
    armed = 1;
    v = JS_Eval(ctx, src, sizeof src - 1, "<propagate>", 0);
    armed = 0;
    check(JS_IsException(v) && JS_HasUncatchableException(ctx), "propagate", "a propagating native keeps the interrupt");
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    check(global_is_undefined(ctx, "leak") && global_is_undefined(ctx, "caught"), "propagate", "no code after it ran");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_regexp(const char* name, const char* src)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue v;
    armed = fired = polls = 0;
    JS_SetInterruptHandler(rt, handler, NULL);
    check(setup(ctx), name, "setup");
    v = JS_Eval(ctx, src, strlen(src), "<regexp>", 0);
    armed = 0;
    check(fired, name, "the handler fired inside the regexp compiler");
    check(JS_IsException(v) && JS_HasUncatchableException(ctx), name, "the interrupt stays uncatchable");
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
    check(global_is_undefined(ctx, "leak") && global_is_undefined(ctx, "caught"), name, "no catch block or later code ran");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(int argc, char** argv)
{
    static const struct {
        const char* name;
        const char* expr;
        int module_only;
    } cases[] = {
        { "plain", "swallow(spin)" },
        { "method", "o.sw(spin)" },
        { "method0", "o.sw0()" },
        { "method1-local", "(function () { const f = spin; o.sw(f); })()" },
        { "method1-loop", "(function () { const f = spin; let k = 0; for (let i = 0; i < 64; i++) k += i * 2; o.sw(f); })()" },
        { "method0-loop", "(function () { let k = 0; for (let i = 0; i < 64; i++) k += i * 2; o.sw0(); })()" },
        { "method1-arith", "(function (a) { const f = spin; a = (a + 1) | 0; o.sw(f); })(1)" },
        { "magic", "swallowMagic(spin)" },
        { "getter", "o.g" },
        { "cfunc-data", "swallowData(spin)" },
        { "bound", "swallow.bind(null, spin)()" },
        { "reflect", "Reflect.apply(swallow, null, [spin])" },
        { "map", "[spin].map(swallow)" },
        { "nested", "swallow(() => swallow(spin))" },
        { "nested-method", "o.sw(() => { o.sw(spin); })" },
        { "convert", "convert(spin)" },
        { "rethrow", "rethrow(spin)" },
        { "reject", "reject(spin)" },
        { "iterator-next", "for (const x of { [Symbol.iterator]() { return o; } }) {}" },
        { "method1-hot", "(function () { const g = () => 0; for (let i = 0; i < 300000; i++) { let h = g; if (i === 299999) h = spinArm; o.sw(h); } })()" },
        { "await-method0", "await null; o.sw0()", 1 },
        { "await-method1", "await (async () => { const f = spin; await null; o.sw(f); })()", 1 },
        { "method0-hot", "(function () { globalThis.spin = () => 0; for (let i = 0; i < 300000; i++) { if (i === 299999) globalThis.spin = spinArm; o.sw0(); } })()" },
    };
    const char* only = argc > 1 ? argv[1] : NULL;
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!only || strstr(cases[i].name, only))
        {
            char mname[64];
            if (!cases[i].module_only)
                swallow_case(cases[i].name, cases[i].expr, 0);
            snprintf(mname, sizeof mname, "module-%s", cases[i].name);
            swallow_case(mname, cases[i].expr, 1);
        }
    if (!only || strstr("catchable", only))
        case_catchable();
    if (!only || strstr("embedder", only))
        case_embedder_taken();
    if (!only || strstr("propagate", only))
        case_propagate();
    if (!only || strstr("regexp-ctor", only))
        case_regexp("regexp-ctor", "const p = 'a|'.repeat(4000) + 'b'; arm(); try { new RegExp(p); } catch (e) { globalThis.caught = 1; } globalThis.leak = 1;");
    if (!only || strstr("regexp-ctor-v", only))
        case_regexp("regexp-ctor-v", "const p = '[a]|'.repeat(4000) + 'c'; arm(); try { new RegExp(p, 'v'); } catch (e) { globalThis.caught = 1; } globalThis.leak = 1;");
    if (!only || strstr("regexp-literal", only))
        case_regexp("regexp-literal", "const p = '/' + 'd|'.repeat(4000) + 'e/'; arm(); try { (0, eval)(p); } catch (e) { globalThis.caught = 1; } globalThis.leak = 1;");
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
