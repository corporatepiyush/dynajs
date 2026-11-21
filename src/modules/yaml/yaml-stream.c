#include "yaml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

static JSValue ystream_result(JSContext* ctx, JSValue value, int done)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, value);
        return result;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done",
        done ? JS_TRUE : JS_FALSE, JS_PROP_C_W_E);
    return result;
}

void ystream_free(JSRuntime* rt, ystream_t* st)
{
    if (!st)
        return;
    yml_shapes_free(rt, &st->p);
    free(st->p.ln);
    st->p.ln = NULL;
    if (st->buf)
        js_free_rt(rt, st->buf);
    st->buf = NULL;
    JS_FreeValueRT(rt, st->err_val);
    free(st);
}

void ystream_finalizer(JSRuntime* rt, JSValue val)
{
    ystream_free(rt, (ystream_t*)JS_GetOpaque(val, ystream_class_id));
}

static JSValue ystream_next_doc(ystream_t* st, int* out_dead)
{
    JSValue d;

    *out_dead = 0;
    for (;;) {
        yml_skip_blank(&st->p);
        if (st->p.li >= st->p.nln)
            return JS_UNDEFINED;
        if (yml_at_doc_end(&st->p)) {
            if (!yml_marker_tail_ok(&st->p, st->p.li)) {
                yml_fail(&st->p, "content after the document-end marker");
                *out_dead = 1;
                return JS_EXCEPTION;
            }
            st->p.li++;
            continue;
        }
        break;
    }
    d = yml_one(&st->p);
    if (JS_IsException(d))
        *out_dead = 1;
    return d;
}

JSValue ystream_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ystream_t* st = (ystream_t*)dyn_plain_get(ctx, this_val,
        ystream_class_id);
    JSValue d;
    int dead;

    (void)argc;
    (void)argv;
    if (!st)
        return JS_EXCEPTION;
    if (st->dead)
        return JS_Throw(ctx, JS_DupValue(ctx, st->err_val));
    if (st->done)
        return ystream_result(ctx, JS_UNDEFINED, 1);
    d = ystream_next_doc(st, &dead);
    if (dead) {
        st->dead = 1;
        snprintf(st->err, sizeof st->err, "document %u: %s",
            (unsigned)(st->ndocs + 1),
            st->p.err[0] ? st->p.err : "malformed document");
        JS_ThrowSyntaxError(ctx, "ParseStream: %s", st->err);
        st->err_val = JS_GetException(ctx);
        return JS_Throw(ctx, JS_DupValue(ctx, st->err_val));
    }
    if (JS_IsUndefined(d)) {
        st->done = 1;
        free(st->p.ln);
        st->p.ln = NULL;
        js_free_rt(JS_GetRuntime(ctx), st->buf);
        st->buf = NULL;
        return ystream_result(ctx, JS_UNDEFINED, 1);
    }
    st->ndocs++;
    return ystream_result(ctx, d, 0);
}

JSValue ystream_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ystream_t* st = (ystream_t*)dyn_plain_get(ctx, this_val,
        ystream_class_id);

    (void)argc;
    (void)argv;
    if (!st)
        return JS_EXCEPTION;
    if (!st->done && !st->dead) {
        st->done = 1;
        free(st->p.ln);
        st->p.ln = NULL;
        js_free_rt(JS_GetRuntime(ctx), st->buf);
        st->buf = NULL;
    }
    return ystream_result(ctx, JS_UNDEFINED, 1);
}

JSValue ystream_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

#endif
