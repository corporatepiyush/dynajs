#include "dynajs.h"
#include "src/fuzz/fuzz_common.h"

#define FZ_COVER_ENGINE 1

#include "src/fuzz/jsgen.inc.c"

#include <stdint.h>

static fz_env_t env;

static const char fz_det_src[] = "{ let s = 7; Math.random = function random() { s = (s * 16807) % 2147483647; return s / 2147483647; };"
                                 " const D = Date; const F = function Date(...a) { return new.target ? (a.length ? new D(...a) : new D(0)) : new D(0).toString(); };"
                                 " F.prototype = D.prototype; F.now = () => 0; F.UTC = D.UTC; F.parse = D.parse; globalThis.Date = F; }";

static void det_init(JSContext* ctx)
{
    JSValue v = JS_Eval(ctx, fz_det_src, sizeof(fz_det_src) - 1, "<det>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v))
        fz_fail("deterministic prelude");
    JS_FreeValue(ctx, v);
}

typedef struct {
    int exc;
    int budget;
    char cls[64];
    char val[192];
} outcome_t;

static void describe(JSContext* ctx, JSValueConst v, outcome_t* o)
{
    const char* s;
    if (JS_IsObject(v)) {
        JSValue n = JS_GetPropertyStr(ctx, v, "name");
        s = JS_IsException(n) ? NULL : JS_ToCString(ctx, n);
        if (JS_IsException(n) || !s)
            JS_FreeValue(ctx, JS_GetException(ctx));
        snprintf(o->cls, sizeof(o->cls), "%s:%s", JS_IsFunction(ctx, v) ? "function" : "object", JS_IsError(ctx, v) && s ? s : "");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, n);
        return;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    snprintf(o->cls, sizeof(o->cls), "tag%d", JS_VALUE_GET_NORM_TAG(v));
    snprintf(o->val, sizeof(o->val), "%s", s ? s : "?");
    JS_FreeCString(ctx, s);
}

static void settle_value(JSContext* ctx, JSValue v, outcome_t* o)
{
    if (JS_IsException(v)) {
        JSValue x = JS_GetException(ctx);
        o->exc = 1;
        o->budget = fz_is_budget_error(ctx, x) || fz_fired();
        if (o->budget)
            env.dirty = 1;
        describe(ctx, x, o);
        JS_FreeValue(ctx, x);
        return;
    }
    fz_drain(ctx);
    if (fz_fired())
        o->budget = 1;
    if (JS_IsObject(v) && JS_PromiseState(ctx, v) >= 0) {
        int st = JS_PromiseState(ctx, v);
        JSValue r = JS_PromiseResult(ctx, v);
        o->exc = st == JS_PROMISE_REJECTED;
        if (st == JS_PROMISE_PENDING)
            snprintf(o->cls, sizeof(o->cls), "pending");
        else
            describe(ctx, r, o);
        if (o->exc && JS_IsError(ctx, r)) {
            o->budget |= fz_is_budget_error(ctx, r);
        }
        JS_FreeValue(ctx, r);
    } else
        describe(ctx, v, o);
    JS_FreeValue(ctx, v);
}

static void run_fn(JSContext* ctx, JSValue fn, outcome_t* o)
{
    memset(o, 0, sizeof(*o));
    fz_arm(200000000u, 400);
    if (JS_VALUE_GET_TAG(fn) == JS_TAG_MODULE) {
        if (JS_ResolveModule(ctx, fn) < 0) {
            JS_FreeValue(ctx, fn);
            settle_value(ctx, JS_EXCEPTION, o);
            return;
        }
    }
    settle_value(ctx, JS_EvalFunction(ctx, fn), o);
}

static void run(fz_env_t* e, const uint8_t* data, size_t size)
{
    fz_in_t in = { data, size };
    fz_sb_t s = { 0 };
    JSContext *a = e->ctx[0], *b = e->ctx[1];
    unsigned mode = fz_u8(&in);
    int flags = JS_EVAL_FLAG_COMPILE_ONLY | ((mode & 2) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    JSValue fa, fb;
    uint8_t *b1 = NULL, *b2 = NULL;
    size_t n1 = 0, n2 = 0;
    outcome_t oa, ob;
    if (mode & 1)
        fz_js_program(&in, &s);
    else
        fz_sb_put(&s, (const char*)in.p, in.n);
    if (fz_print())
        fprintf(stderr, "---- program (mode %u)\n%s\n----\n", mode, s.b ? s.b : "");
    fz_arm(200000000u, 0);
    fa = JS_Eval(a, s.b ? s.b : "", s.n, "<compile-fuzz>", flags);
    if (JS_IsException(fa)) {
        JSValue x = JS_GetException(a);
        if (fz_is_budget_error(a, x))
            e->dirty = 1;
        JS_FreeValue(a, x);
        goto out;
    }
    b1 = JS_WriteObject(a, &n1, fa, JS_WRITE_OBJ_BYTECODE);
    if (!b1) {
        JSValue x = JS_GetException(a);
        if (!fz_is_budget_error(a, x))
            fz_fail("JS_WriteObject failed on a freshly compiled function");
        JS_FreeValue(a, x);
        JS_FreeValue(a, fa);
        e->dirty = 1;
        goto out;
    }
    fb = JS_ReadObject(b, b1, n1, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fb)) {
        JSValue x = JS_GetException(b);
        if (!fz_is_budget_error(b, x))
            fz_fail("JS_ReadObject rejected bytecode that JS_WriteObject just produced");
        JS_FreeValue(b, x);
        JS_FreeValue(a, fa);
        e->dirty = 1;
        goto out;
    }
    b2 = JS_WriteObject(b, &n2, fb, JS_WRITE_OBJ_BYTECODE);
    if (b2 && (n1 != n2 || memcmp(b1, b2, n1)))
        fz_fail("write(read(write(f))) differs from write(f)");
    if (!b2) {
        JS_FreeValue(b, JS_GetException(b));
        e->dirty = 1;
    }
    run_fn(a, fa, &oa);
    run_fn(b, fb, &ob);
    if (!oa.budget && !ob.budget && !strstr(s.b ? s.b : "", "WeakRef") && !strstr(s.b ? s.b : "", "Finalization")) {
        if (oa.exc != ob.exc || strcmp(oa.cls, ob.cls) || strcmp(oa.val, ob.val)) {
            fprintf(stderr, "direct: exc=%d %s %s\nfrom bytecode: exc=%d %s %s\n", oa.exc, oa.cls, oa.val, ob.exc, ob.cls, ob.val);
            fz_fail("function run from serialized bytecode behaves differently");
        }
    }
out:
    if (b1)
        js_free(a, b1);
    if (b2)
        js_free(b, b2);
    free(s.b);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size && size <= 65536)
        fz_one(&env, data, size);
    return 0;
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    fz_args(*argc, *argv);
    fz_env_open(&env, 2, 32, det_init, run);
    return 0;
}
