// covers: c:src/modules/http/http-cookiejar.c -- rule 18 embedder rows for dyna:http.CookieJar: a one-shot interrupt raised in a getter, valueOf or Proxy trap the jar runs (forced allocation failure is covered by the C malloc hooks; the heap-cap sweep was removed with the flag)
// fz5n2. storeResponse swallowed the exception of a Set-Cookie getter, a Proxy get trap and a failed key conversion,
// so a one-shot interrupt raised there let the script run on (the CLI's repeating --timeout-ms handler hides it), and
// under memory pressure the call returned normally with an out-of-memory error still pending. Interrupt cases pass
// when the loop ends with an error and the script never set globalThis.leak. probe() fails a run when any jar call returned with an exception pending.
// Usage: test_http_cookiejar_api [CASE-SUBSTRING]; run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int armed, fired, stale;
static int fails, checks;

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (armed && !fired) {
        fired = 1;
        return 1;
    }
    return 0;
}

static JSValue h_arm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    armed = 1;
    return JS_UNDEFINED;
}

static JSValue h_probe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_HasException(ctx)) {
        stale++;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return JS_UNDEFINED;
}

static unsigned long cj_alloc_n, cj_fail_at;
static int cj_failed;

#undef malloc
#undef realloc
void* malloc(size_t n);
void* realloc(void* p, size_t n);

void* cj_t_malloc(size_t n)
{
    if (cj_fail_at && ++cj_alloc_n == cj_fail_at) {
        cj_failed = 1;
        return NULL;
    }
    return malloc(n);
}

void* cj_t_realloc(void* p, size_t n)
{
    if (cj_fail_at && ++cj_alloc_n == cj_fail_at) {
        cj_failed = 1;
        return NULL;
    }
    return realloc(p, n);
}

static JSValue h_fail_alloc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t at = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt64(ctx, &at, argv[0]))
        return JS_EXCEPTION;
    cj_alloc_n = 0;
    cj_failed = 0;
    cj_fail_at = at > 0 ? (unsigned long)at : 0;
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, cj_failed);
}

static JSContext* new_ctx(JSRuntime* rt)
{
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_nat_init_all(ctx);
    return ctx;
}

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: [%s] %s\n", name, what);
    }
}

#define PRE                                                                                     \
    "import * as os from 'os';\n"                                                               \
    "const spin = () => { arm(); for (;;) {} };\n"                                              \
    "const leak = (e) => { globalThis.leak = String(e); };\n"                                   \
    "os.setTimeout(() => { if (globalThis.leak === undefined) globalThis.leak = 'timer'; }, 50);\n" \
    "const { CookieJar } = await import('dyna:http');\n"                                        \
    "const U = 'http://a.test/';\n"

static const char* const cases[][2] = {
    { "storeResponse: Set-Cookie getter spins", "try { new CookieJar().storeResponse(U, { get 'set-cookie'() { spin(); } }); } catch (e) { leak(e); }\nleak('after');\n" },
    { "storeResponse: Proxy get spins", "try { new CookieJar().storeResponse(U, new Proxy({ 'set-cookie': 'a=1' }, { get() { spin(); } })); } catch (e) { leak(e); }\nleak('after');\n" },
    { "storeResponse: Proxy ownKeys spins", "try { new CookieJar().storeResponse(U, new Proxy({}, { ownKeys() { spin(); } })); } catch (e) { leak(e); }\nleak('after');\n" },
    { "storeResponse: Proxy descriptor spins", "try { new CookieJar().storeResponse(U, new Proxy({ 'set-cookie': 'a=1' }, { getOwnPropertyDescriptor() { spin(); } })); } catch (e) { leak(e); }\nleak('after');\n" },
    { "CookieJar: maxCookies valueOf spins", "try { new CookieJar({ maxCookies: { valueOf() { spin(); } } }); } catch (e) { leak(e); }\nleak('after');\n" },
    { "CookieJar: maxCookies getter spins", "try { new CookieJar({ get maxCookies() { spin(); } }); } catch (e) { leak(e); }\nleak('after');\n" },
};

static JSRuntime* new_rt(JSContext** pctx)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g;
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JS_SetInterruptHandler(rt, handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, h_arm, "arm", 0));
    JS_SetPropertyStr(ctx, g, "probe", JS_NewCFunction(ctx, h_probe, "probe", 0));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "allocFailed", JS_NewCFunction(ctx, h_alloc_failed, "allocFailed", 0));
    JS_FreeValue(ctx, g);
    *pctx = ctx;
    return rt;
}

static void free_rt(JSRuntime* rt, JSContext* ctx)
{
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
}

static void run_case(const char* name, const char* body)
{
    JSContext* ctx;
    JSRuntime* rt;
    JSValue v, g, l;
    char src[4096];
    int rc;
    armed = 0;
    fired = 0;
    rt = new_rt(&ctx);
    snprintf(src, sizeof(src), "%s%s", PRE, body);
    v = JS_Eval(ctx, src, strlen(src), "<case>", JS_EVAL_TYPE_MODULE);
    v = js_std_await(ctx, v);
    rc = JS_IsException(v) ? -1 : 0;
    if (rc)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    if (!rc)
        rc = js_std_loop(ctx);
    g = JS_GetGlobalObject(ctx);
    l = JS_GetPropertyStr(ctx, g, "leak");
    check(fired, name, "the handler fired");
    check(rc != 0, name, "the run ended with the interrupt");
    if (!JS_IsUndefined(l)) {
        const char* s = JS_ToCString(ctx, l);
        char what[256];
        snprintf(what, sizeof(what), "user code observed the interrupt (leak = %s)", s ? s : "?");
        check(0, name, what);
        if (s)
            JS_FreeCString(ctx, s);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, l);
    JS_FreeValue(ctx, g);
    free_rt(rt, ctx);
}


static const char hook_src[] = "const { CookieJar } = await import('dyna:http');\n"
                               "const U = 'http://a.test/x/y', bad = [], seen = { setCookie: 0, storeResponse: 0, cookieHeader: 0 };\n"
                               "const ops = { setCookie: (j) => j.setCookie(U, 'sid=1; Domain=a.test; Path=/x'),\n"
                               "  storeResponse: (j) => j.storeResponse(U, { 'set-cookie': 'sid=1; Domain=a.test; Path=/x' }),\n"
                               "  cookieHeader: (j) => j.cookieHeader(U) };\n"
                               "for (const [name, op] of Object.entries(ops)) {\n"
                               "  for (let at = 1; at < 64; at++) {\n"
                               "    const j = new CookieJar(); j.setCookie(U, 'old=1; Path=/');\n"
                               "    let r; failAlloc(at);\n"
                               "    try { r = op(j); } catch (e) { r = e; }\n"
                               "    const failed = allocFailed(); failAlloc(0); probe();\n"
                               "    if (!failed) { if (r instanceof Error) bad.push(name + ' threw ' + r + ' with no failed allocation'); break; }\n"
                               "    seen[name]++;\n"
                               "    if (!(r instanceof Error) || !/out of memory/.test(String(r.message))) bad.push(name + ' at ' + at + ' returned ' + String(r));\n"
                               "    if (name !== 'cookieHeader' && j.cookieHeader(U) !== 'old=1') bad.push(name + ' at ' + at + ' changed the jar: ' + j.cookieHeader(U));\n"
                               "  }\n"
                               "}\n"
                               "for (const k in seen) if (!seen[k]) bad.push(k + ': no allocation failed');\n"
                               "globalThis.res = bad.length ? bad.slice(0, 4).join('; ') : 'ok ' + JSON.stringify(seen);\n";

static void run_hook(void)
{
    JSContext* ctx;
    JSRuntime* rt = new_rt(&ctx);
    JSValue v, g, r;
    const char* s;
    stale = 0;
    v = js_std_await(ctx, JS_Eval(ctx, hook_src, sizeof hook_src - 1, "<hook>", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, v);
    g = JS_GetGlobalObject(ctx);
    r = JS_GetPropertyStr(ctx, g, "res");
    s = JS_ToCString(ctx, r);
    printf("alloc hook: %s\n", s ? s : "(no result)");
    check(s && !strncmp(s, "ok ", 3), "alloc hook", "every failing httpk allocation throws out of memory and leaves the jar as it was");
    check(stale == 0, "alloc hook", "no jar call returned with an exception pending");
    if (s)
        JS_FreeCString(ctx, s);
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, g);
    free_rt(rt, ctx);
}

int main(int argc, char** argv)
{
    size_t i;
    alarm(120);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && !strstr(cases[i][0], argv[1]))
            continue;
        printf("case: %s\n", cases[i][0]);
        fflush(stdout);
        run_case(cases[i][0], cases[i][1]);
    }
    if (argc < 2 || strstr("alloc hook", argv[1]))
        run_hook();
    printf("http_cookiejar_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
