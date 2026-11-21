#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

static void expr_free_v(void* p) { exprk_free((expr_t*)p); }

static int dyn_expr_num(void* ud, const char* s, size_t n, double* out)
{
    JSContext* ctx = (JSContext*)ud;
    JSValue sv = JS_NewStringLen(ctx, s, n);
    if (JS_IsException(sv))
        return EXPRK_NUM_NOMEM;
    if (JS_ToFloat64(ctx, out, sv) < 0) {
        JS_FreeValue(ctx, sv);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return EXPRK_NUM_BAD;
    }
    JS_FreeValue(ctx, sv);
    return EXPRK_NUM_OK;
}

JSClassID dyn_expr_class_id;

static void dyn_expr_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    exprk_free((expr_t*)JS_GetOpaque(val, dyn_expr_class_id));
}

const JSClassDef dyn_expr_class = {
    "Expression",
    .finalizer = dyn_expr_finalizer,
};

JSValue dyn_expr_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ex_parse_t p;
    const char* s;
    size_t n;
    expr_t* e;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Expression(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > EX_MAX_SRC) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new Expression: exceeds %u bytes",
            EX_MAX_SRC);
    }
    e = (expr_t*)calloc(1, sizeof *e);
    if (!e) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(&p, 0, sizeof p);
    p.ud = ctx;
    p.num = dyn_expr_num;
    p.s = s;
    p.n = n;
    p.e = e;
    if (exprk_compile(&p) < 0) {
        JS_FreeCString(ctx, s);
        exprk_free(e);
        return JS_ThrowSyntaxError(ctx, "new Expression: %s",
            p.err[0] ? p.err : "out of memory");
    }
    JS_FreeCString(ctx, s);
    return dyn_plain_wrap(ctx, new_target, dyn_expr_class_id, e, expr_free_v);
}

static JSValue dyn_expr_eval(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    expr_t* e = (expr_t*)dyn_plain_get(ctx, this_val, dyn_expr_class_id);
    double *vals = NULL, *stk = NULL;
    uint32_t i;
    JSValue r = JS_EXCEPTION;

    if (!e)
        return JS_EXCEPTION;
    if (e->nname && (argc < 1 || !JS_IsObject(argv[0])))
        return JS_ThrowTypeError(ctx,
            "Expression.eval(vars): this expression has variables, so it needs "
            "an object of values");
    vals = (double*)calloc(e->nname ? e->nname : 1, sizeof *vals);
    stk = (double*)calloc(e->max_depth ? e->max_depth : 1, sizeof *stk);
    if (!vals || !stk) {
        free(vals);
        free(stk);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < e->nname; i++) {
        JSAtom a = JS_NewAtom(ctx, e->names[i]);
        JSPropertyDescriptor d;
        int got;
        if (a == JS_ATOM_NULL)
            goto done;
        got = JS_GetOwnProperty(ctx, &d, argv[0], a);
        JS_FreeAtom(ctx, a);
        if (got < 0)
            goto done;
        if (got == 0 || (d.flags & JS_PROP_GETSET)) {
            if (got) {
                JS_FreeValue(ctx, d.getter);
                JS_FreeValue(ctx, d.setter);
                JS_FreeValue(ctx, d.value);
            }
            JS_ThrowRangeError(ctx, "Expression.eval: no value for %s",
                e->names[i]);
            goto done;
        }
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
        if (!JS_IsNumber(d.value)) {
            JS_FreeValue(ctx, d.value);
            JS_ThrowTypeError(ctx, "Expression.eval: %s must be a number",
                e->names[i]);
            goto done;
        }
        if (JS_ToFloat64(ctx, &vals[i], d.value) < 0) {
            JS_FreeValue(ctx, d.value);
            goto done;
        }
        JS_FreeValue(ctx, d.value);
    }
    {
        double res;
        if (exprk_run(e, vals, stk, &res) < 0) {
            JS_ThrowInternalError(ctx, "Expression: the compiled program is "
                                       "inconsistent with its stack");
            goto done;
        }
        r = JS_NewFloat64(ctx, res);
    }
done:
    free(vals);
    free(stk);
    return r;
}

static JSValue dyn_expr_vars(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    expr_t* e = (expr_t*)dyn_plain_get(ctx, this_val, dyn_expr_class_id);
    JSValue out;
    uint32_t i;

    (void)argc;
    (void)argv;
    if (!e)
        return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < e->nname; i++)
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                JS_NewString(ctx, e->names[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    return out;
}

const JSCFunctionListEntry dyn_expr_proto[] = {
    JS_CFUNC_DEF("eval", 0, dyn_expr_eval),
    JS_CFUNC_DEF("variables", 0, dyn_expr_vars),
};

#endif
