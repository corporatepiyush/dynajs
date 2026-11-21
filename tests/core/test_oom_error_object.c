// covers: c:src/object/shapes_objects_gc.inc.c c:src/mm/js_malloc.inc.c c:src/vm/interpreter.inc.c c:src/serialize/bc_write.inc.c -- out-of-memory paths on a capped allocator
// Port of the deleted tests/core/test_oom_error_object.js (its --memory-limit children went with the flag). The runtime
// is built on tests/lib/dyn-oomrt.h (public JS_NewRuntime2), capped at the live heap plus a swept headroom:
//  null:      an out-of-memory throw is never null (JS_ThrowError2 falls back to the context's preallocated InternalError);
//  await:     a failed await setup throws into the async function at the await (its catch runs; the result promise is
//             never rejected with the frame still suspended);
//  instanceof: `e instanceof InternalError` allocates nothing (Function.prototype[Symbol.hasInstance] is built eagerly);
//  module:    a module whose body fills the heap before its first await settles (no promise reaction is lost when a
//             settle cannot allocate its job)
//  (run with UBSAN_OPTIONS=halt_on_error=1: the sweep reaches the tail-call value stack before its first chunk, where
//  js_tcv_push must not form NULL + n)
//  module-nth: the Nth allocation of a parent+child TLA module evaluation fails, for every N (the child has a
//             constant object, a 40-variable function, a nested global reference and a sloppy indirect eval with a
//             `with` closure, so the parser's optional tables and closure-variable paths are swept too; a handler attach,
//             an async-module start or a settle handler that cannot allocate rejects or reports, never pends silently);
//  bcstring:  JS_ReadObject checks a string's declared length against the bytes left before allocating it.
//  longmsg:   a JS_Throw*Error message of any length (0..4096 bytes, embedded NUL) is kept whole; when the long-format
//             buffer cannot be allocated the first 255 bytes are kept and nothing else is thrown.
// Usage: test_oom_error_object [CASE-SUBSTRING]
#include "dynajs.h"
#include "../lib/dyn-oomrt.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s: %s\n", name, what);
    }
}

static size_t used(JSRuntime* rt)
{
    JSMemoryUsage mu;
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &mu);
    return (size_t)mu.malloc_size;
}

static void drain(JSRuntime* rt)
{
    JSContext* c;
    for (;;) {
        int r = JS_ExecutePendingJob(rt, &c);
        if (r == 0)
            break;
        if (r < 0)
            JS_FreeValue(c, JS_GetException(c));
    }
}

static JSValue run(JSContext* ctx, const char* src, int flags)
{
    return JS_Eval(ctx, src, strlen(src), "<oom>", flags);
}

static JSValue get(JSContext* ctx, const char* name)
{
    JSValue g = JS_GetGlobalObject(ctx), v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return v;
}

static void case_null(void)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx = JS_NewContext(rt);
    JSValue f;
    size_t head;
    long nulls = 0, errs = 0, oks = 0;
    JS_FreeValue(ctx, run(ctx, "globalThis.f = () => { const a = []; for (let i = 0; i < 96; i++) a.push({ i, s: 'k' + i }); return a.length; };", JS_EVAL_TYPE_GLOBAL));
    f = get(ctx, "f");
    for (head = 0; head < 64 * 1024; head += 16) {
        JSValue v;
        oomrt_cap = used(rt) + head;
        v = JS_Call(ctx, f, JS_UNDEFINED, 0, NULL);
        oomrt_cap = (size_t)-1;
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            if (JS_IsNull(e))
                nulls++;
            else if (JS_IsError(ctx, e))
                errs++;
            JS_FreeValue(ctx, e);
        } else
            oks++;
        JS_FreeValue(ctx, v);
    }
    printf("null: %ld errors, %ld null throws, %ld completed\n", errs, nulls, oks);
    check(nulls == 0, "null", "no out-of-memory throw is null");
    check(errs > 0 && oks > 0, "null", "the sweep crosses the failure boundary");
    JS_FreeValue(ctx, f);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_await(void)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx = JS_NewContext(rt);
    JSValue g, gl;
    size_t head;
    long bad = 0, caught = 0, resumed = 0;
    JS_FreeValue(ctx, run(ctx, "globalThis.st = 0; globalThis.g = async () => { let s = 0; try { await null; s = 1; } catch (e) { s = 2; } st = s; return s; };", JS_EVAL_TYPE_GLOBAL));
    g = get(ctx, "g");
    gl = JS_GetGlobalObject(ctx);
    for (head = 0; head < 32 * 1024; head += 8) {
        JSValue p, st;
        int32_t s = -1;
        JS_SetPropertyStr(ctx, gl, "st", JS_NewInt32(ctx, 0));
        oomrt_cap = used(rt) + head;
        p = JS_Call(ctx, g, JS_UNDEFINED, 0, NULL);
        oomrt_cap = (size_t)-1;
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        drain(rt);
        st = get(ctx, "st");
        if (JS_ToInt32(ctx, &s, st) < 0)
            s = -1;
        JS_FreeValue(ctx, st);
        if (JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED && s == 0)
            bad++;
        if (s == 2)
            caught++;
        if (s == 1)
            resumed++;
        JS_FreeValue(ctx, p);
    }
    printf("await: %ld caught at the await, %ld resumed, %ld rejected with the frame suspended\n", caught, resumed, bad);
    check(bad == 0, "await", "no result promise is rejected while its frame is still suspended");
    check(caught > 0 && resumed > 0, "await", "a failed await setup is caught inside the function");
    JS_FreeValue(ctx, gl);
    JS_FreeValue(ctx, g);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void case_instanceof(void)
{
    size_t head;
    long bad = 0, runs = 0;
    for (head = 0; head < 4096; head += 64) {
        JSRuntime* rt = oomrt_new();
        JSContext* ctx = JS_NewContext(rt);
        JSValue h, v;
        JS_FreeValue(ctx, run(ctx, "globalThis.ex = new InternalError('x'); globalThis.h = () => ex instanceof InternalError;", JS_EVAL_TYPE_GLOBAL));
        h = get(ctx, "h");
        oomrt_cap = used(rt) + head;
        v = JS_Call(ctx, h, JS_UNDEFINED, 0, NULL);
        oomrt_cap = (size_t)-1;
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            bad++;
        } else if (!JS_ToBool(ctx, v))
            bad++;
        runs++;
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, h);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    printf("instanceof: %ld runs, %ld failed at a full heap\n", runs, bad);
    check(bad == 0, "instanceof", "the first instanceof at a full heap allocates nothing");
}

static void case_module(void)
{
    static const char src[] = "let hog = []; try { for (;;) hog.push(new Array(256).fill(1.5)); } catch (e) {} try { for (;;) hog.push([1]); } catch (e) {} await null; hog = null;";
    size_t mb;
    long pending = 0, runs = 0;
    for (mb = 1; mb <= 16; mb *= 2) {
        JSRuntime* rt = oomrt_new();
        JSContext* ctx = JS_NewContext(rt);
        JSValue v;
        oomrt_cap = used(rt) + mb * 1024 * 1024;
        v = JS_Eval(ctx, src, sizeof src - 1, "<oom-module>", JS_EVAL_TYPE_MODULE);
        oomrt_cap = (size_t)-1;
        if (JS_IsException(v))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else {
            drain(rt);
            if (JS_PromiseState(ctx, v) == JS_PROMISE_PENDING)
                pending++;
        }
        runs++;
        JS_FreeValue(ctx, v);
        drain(rt);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    printf("module: %ld runs, %ld left pending with no work\n", runs, pending);
    check(pending == 0, "module", "a module that fills the heap before its first await settles");
}

static const char nth_child_src[] = "const K = { a: 1, b: 's', c: 2.5 }; "
    "function big(x) { let v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11, v12 = 12, v13 = 13, v14 = 14, v15 = 15, v16 = 16, v17 = 17, v18 = 18, v19 = 19, v20 = 20, v21 = 21, v22 = 22, v23 = 23, v24 = 24, v25 = 25, v26 = 26, v27 = 27, v28 = 28, v29 = 29, v30 = 30, v31 = 31, v32 = 32, v33 = 33, v34 = 34, v35 = 35, v36 = 36, v37 = 37, v38 = 38, v39 = 39; { let inner = x; v0 += inner; } return v0 + v39 + K.a + K.c; } "
    "const g = () => globalThis.Math; "
    "(0, eval)('var wo = { v: 1 }; with (wo) { (function () { return v; })(); }'); "
    "await null; globalThis.c = big(1) + g().abs(-1);";

static JSModuleDef* nth_loader(JSContext* ctx, const char* name, void* opaque)
{
    JSValue v;
    (void)opaque;
    if (strcmp(name, "child") != 0) {
        JS_ThrowReferenceError(ctx, "no module %s", name);
        return NULL;
    }
    v = JS_Eval(ctx, nth_child_src, sizeof nth_child_src - 1, "child", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v))
        return NULL;
    {
        JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(v);
        JS_FreeValue(ctx, v);
        return m;
    }
}

static void case_module_nth(void)
{
    static const char src[] = "import \"child\"; await null; globalThis.p = 1;";
    long n, silent = 0, runs = 0, rejected = 0, settled = 0, stale = 0;
    for (n = 1; n < 20000; n++) {
        JSRuntime* rt = oomrt_new();
        JSContext* ctx = JS_NewContext(rt);
        JSContext* c;
        JSValue v;
        size_t refused0 = oomrt_refused;
        int job_errors = 0, guard, r;
        JS_SetModuleLoaderFunc(rt, NULL, nth_loader, NULL);
        oomrt_fail_at = n;
        v = JS_Eval(ctx, src, sizeof src - 1, "<nth>", JS_EVAL_TYPE_MODULE);
        if (!JS_IsException(v) && JS_HasException(ctx))
            stale++;
        for (guard = 0; guard < 1000; guard++) {
            r = JS_ExecutePendingJob(rt, &c);
            if (r == 0)
                break;
            if (r < 0) {
                job_errors++;
                JS_FreeValue(c, JS_GetException(c));
            }
        }
        oomrt_fail_at = 0;
        runs++;
        if (JS_IsException(v)) {
            rejected++;
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (JS_PromiseState(ctx, v) == JS_PROMISE_PENDING) {
            if (!job_errors)
                silent++;
        } else {
            settled++;
        }
        JS_FreeValue(ctx, v);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        if (oomrt_refused == refused0)
            break;
    }
    printf("module-nth: %ld runs (%ld threw, %ld settled), %ld left pending with no error reported, %ld left an exception pending\n", runs, rejected, settled, silent, stale);
    check(n < 20000, "module-nth", "the sweep reached an allocation count that needs no refusal");
    check(silent == 0, "module-nth", "no failed allocation during module evaluation leaves the promise pending silently");
    check(stale == 0, "module-nth", "a successful evaluation leaves no exception pending");
}

static void case_bcstring(void)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx = JS_NewContext(rt);
    JSValue s = JS_NewString(ctx, "x"), v;
    size_t n = 0, refused;
    uint8_t* buf = JS_WriteObject(ctx, &n, s, 0);
    uint8_t* big = NULL;
    static const uint8_t leb[] = { 0x80, 0x80, 0x80, 0x80, 0x02 };
    JS_FreeValue(ctx, s);
    check(buf && n >= 2 && buf[n - 2] == 0x02 && buf[n - 1] == 'x', "bcstring", "a one-char string ends in its length 2 and the char");
    if (buf && n >= 2) {
        big = malloc(n + sizeof leb);
        if (!big) {
            js_free(ctx, buf);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return;
        }
        memcpy(big, buf, n - 2);
        memcpy(big + n - 2, leb, sizeof leb);
        big[n - 2 + sizeof leb] = 'x';
        oomrt_cap = used(rt) + 256 * 1024;
        refused = oomrt_refused;
        v = JS_ReadObject(ctx, big, n - 1 + sizeof leb, 0);
        refused = oomrt_refused - refused;
        oomrt_cap = (size_t)-1;
        check(JS_IsException(v), "bcstring", "a string longer than its input is refused");
        if (JS_IsException(v))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, v);
        printf("bcstring: %zu allocations refused while reading a 2^28-char string from %zu bytes\n", refused, n - 1 + sizeof leb);
        check(refused == 0, "bcstring", "the declared length is checked before the string is allocated");
        free(big);
    }
    js_free(ctx, buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static long longmsg_expect(JSContext* ctx, const char* tag, const char* name, const char* exp, size_t L)
{
    JSValue e = JS_GetException(ctx), m, nm;
    const char *s, *ns;
    size_t n = (size_t)-1;
    long ok;
    check(!JS_HasException(ctx), tag, "no exception stays pending after the throw is taken");
    m = JS_GetPropertyStr(ctx, e, "message");
    nm = JS_GetPropertyStr(ctx, e, "name");
    s = JS_ToCStringLen(ctx, &n, m);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    ns = JS_ToCString(ctx, nm);
    if (!ns)
        JS_FreeValue(ctx, JS_GetException(ctx));
    ok = s && ns && n == L && !memcmp(s, exp, L) && !strcmp(ns, name);
    if (!ok)
        printf("longmsg %s: got name=%s len=%zu want %s len=%zu\n", tag, ns ? ns : "?", n, name, L);
    check((int)ok, tag, "the message keeps every formatted byte");
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, ns);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, nm);
    JS_FreeValue(ctx, e);
    return ok;
}

static void case_longmsg(void)
{
    static char big[4200], exp[4200];
    static const size_t lens[] = { 0, 1, 254, 255, 256, 257, 511, 512, 4096 };
    JSRuntime* rt = oomrt_new();
    JSContext* ctx = JS_NewContext(rt);
    size_t i, refused;
    long k, full = 0, cut = 0, oom = 0, other = 0;
    for (i = 0; i < sizeof big - 1; i++)
        big[i] = (char)(0x21 + (int)(i % 90));
    for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        char tag[48];
        char save = big[lens[i]];
        big[lens[i]] = 0;
        snprintf(tag, sizeof tag, "longmsg-%zu", lens[i]);
        JS_ThrowTypeError(ctx, "%s", big);
        longmsg_expect(ctx, tag, "TypeError", big, lens[i]);
        big[lens[i]] = save;
    }
    JS_ThrowTypeError(ctx, "a%cb", 0);
    longmsg_expect(ctx, "longmsg-nul", "TypeError", "a\0b", 3);
    memcpy(exp, big, 300);
    exp[300] = 0;
    memcpy(exp + 301, "tail", 4);
    JS_ThrowRangeError(ctx, "%.300s%c%s", big, 0, "tail");
    longmsg_expect(ctx, "longmsg-nul-long", "RangeError", exp, 305);
    memcpy(exp, big, 4096);
    big[4096] = 0;
    refused = oomrt_refused;
    oomrt_fail_at = 1;
    JS_ThrowTypeError(ctx, "%s", big);
    oomrt_fail_at = 0;
    check(oomrt_refused == refused + 1, "longmsg-oom", "the long-format buffer allocation was the one refused");
    longmsg_expect(ctx, "longmsg-oom", "TypeError", big, 255);
    for (k = 1; k <= 64; k++) {
        JSValue e, m;
        const char* s;
        size_t n = 0;
        oomrt_fail_at = k;
        JS_ThrowTypeError(ctx, "%s", big);
        oomrt_fail_at = 0;
        e = JS_GetException(ctx);
        check(!JS_HasException(ctx), "longmsg-nth", "no exception stays pending after the throw is taken");
        check(JS_IsObject(e), "longmsg-nth", "a throw under a failing allocation still throws an object");
        m = JS_GetPropertyStr(ctx, e, "message");
        s = JS_ToCStringLen(ctx, &n, m);
        if (!s)
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (s && n == 4096 && !memcmp(s, big, n))
            full++;
        else if (s && n == 255 && !memcmp(s, big, n))
            cut++;
        else if (s && (n == 0 || !strcmp(s, "out of memory")))
            oom++;
        else
            other++;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, m);
        JS_FreeValue(ctx, e);
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    printf("longmsg-nth: full=%ld cut=%ld oom=%ld other=%ld\n", full, cut, oom, other);
    check(other == 0 && cut >= 1 && full >= 1, "longmsg-nth", "each failing allocation yields the full, the 255-byte or the out-of-memory message");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(int argc, char** argv)
{
    static const struct { const char* name; void (*fn)(void); } cases[] = {
        { "null", case_null }, { "await", case_await }, { "instanceof", case_instanceof }, { "module", case_module },
        { "module-nth", case_module_nth }, { "bcstring", case_bcstring }, { "longmsg", case_longmsg },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (argc < 2 || strstr(cases[i].name, argv[1]))
            cases[i].fn();
    printf("test_oom_error_object: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
