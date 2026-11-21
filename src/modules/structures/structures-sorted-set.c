#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

JSClassID dsw_sortedset_class_id;

static void dsw_sortedset_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    skip_free(JS_GetOpaque(val, dsw_sortedset_class_id));
}

const JSClassDef dsw_sortedset_class = {
    "SortedSet",
    .finalizer = dsw_sortedset_finalizer,
};

JSValue dsw_sortedset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = skip_new(JS_GetRuntime(ctx));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, new_target, dsw_sortedset_class_id, s, skip_free);
}

static dsw_skip_t* sortedset_get(JSContext* ctx, JSValueConst t)
{
    return (dsw_skip_t*)dyn_plain_get(ctx, t, dsw_sortedset_class_id);
}

static JSValue dsw_sortedset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_ThrowRangeError(ctx, "NaN is not a valid key");
    if (skip_insert(ctx, s, x, JS_UNDEFINED))
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_sortedset_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(x) && dyn_btree_get(s->b, x) != NULL);
}

static JSValue dsw_sortedset_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(x) && skip_delete(s, x));
}

static JSValue dsw_sortedset_first(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 0);
}

static JSValue dsw_sortedset_last(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 1);
}

static JSValue dsw_sortedset_floor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_floor(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dsw_sortedset_ceil(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_ceil(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dsw_sortedset_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double lo, hi;
    dyn_btree_iter it;
    JSValue arr;
    uint32_t out = 0;
    (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[1]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (isnan(lo) || isnan(hi) || lo > hi)
        return arr;
    if (dyn_btree_iter_seek(s->b, lo, &it)) {
        double k;
        const dyn_cell_t* v;
        do {
            if (!dyn_btree_iter_get(&it, &k, &v) || k > hi)
                break;
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    JS_NewFloat64(ctx, k), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        } while (dyn_btree_iter_next(&it));
    }
    return arr;
}

static JSValue dsw_sortedset_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_keys_array(ctx, s);
}

static JSValue dsw_sortedset_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_skip_t* s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(s->b));
}

const JSCFunctionListEntry dsw_sortedset_proto[] = {
    JS_CFUNC_DEF("add", 1, dsw_sortedset_add),
    JS_CFUNC_DEF("has", 1, dsw_sortedset_has),
    JS_CFUNC_DEF("delete", 1, dsw_sortedset_delete),
    JS_CFUNC_DEF("first", 0, dsw_sortedset_first),
    JS_CFUNC_DEF("last", 0, dsw_sortedset_last),
    JS_CFUNC_DEF("floor", 1, dsw_sortedset_floor),
    JS_CFUNC_DEF("ceil", 1, dsw_sortedset_ceil),
    JS_CFUNC_DEF("rangeQuery", 2, dsw_sortedset_range),
    JS_CFUNC_DEF("toArray", 0, dsw_sortedset_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("size", dsw_sortedset_size, NULL),
};

int dsw_sortedset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    return dsw_skip_write(ctx, w, obj, dsw_sortedset_class_id, 0);
}

JSValue dsw_sortedset_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts)
{
    (void)opts;
    return dsw_skip_read(ctx, r, dsw_sortedset_class_id, "add", 0);
}

#endif
