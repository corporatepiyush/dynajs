#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

JSClassID dsw_sortedmap_class_id;

static void dsw_sortedmap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    skip_free(JS_GetOpaque(val, dsw_sortedmap_class_id));
}

static void dsw_sortedmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_skip_t* s = JS_GetOpaque(val, dsw_sortedmap_class_id);
    dyn_btree_iter it;
    if (!s)
        return;
    if (dyn_btree_iter_begin(s->b, &it)) {
        double k;
        const dyn_cell_t* v;
        do {
            if (dyn_btree_iter_get(&it, &k, &v))
                JS_MarkValue(rt, ds_cell_val(v), mark_func);
        } while (dyn_btree_iter_next(&it));
    }
}

const JSClassDef dsw_sortedmap_class = {
    "SortedMap",
    .finalizer = dsw_sortedmap_finalizer,
    .gc_mark = dsw_sortedmap_gc_mark,
};

JSValue dsw_sortedmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = skip_new(JS_GetRuntime(ctx));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, new_target, dsw_sortedmap_class_id, s, skip_free);
}

static dsw_skip_t* sortedmap_get(JSContext* ctx, JSValueConst t)
{
    return (dsw_skip_t*)dyn_plain_get(ctx, t, dsw_sortedmap_class_id);
}

static JSValue dsw_sortedmap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(k))
        return JS_ThrowRangeError(ctx, "NaN is not a valid key");
    if (skip_insert(ctx, s, k, argv[1]))
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_sortedmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double k;
    const dyn_cell_t* v;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(k))
        return JS_UNDEFINED;
    v = dyn_btree_get(s->b, k);
    return v ? JS_DupValue(ctx, ds_cell_val(v)) : JS_UNDEFINED;
}

static JSValue dsw_sortedmap_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(k) && dyn_btree_get(s->b, k) != NULL);
}

static JSValue dsw_sortedmap_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(k) && skip_delete(s, k));
}

static JSValue dsw_sortedmap_first_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 0);
}

static JSValue dsw_sortedmap_last_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 1);
}

static JSValue dsw_sortedmap_floor_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_floor(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dsw_sortedmap_ceil_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_ceil(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dsw_sortedmap_range(JSContext* ctx, JSValueConst this_val,
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
    s = sortedmap_get(ctx, this_val);
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
            JSValue pair = ds2_tuple2(ctx, JS_NewFloat64(ctx, k), JS_DupValue(ctx, ds_cell_val(v)));
            if (JS_IsException(pair) || JS_DefinePropertyValueUint32(ctx, arr, out++, pair, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        } while (dyn_btree_iter_next(&it));
    }
    return arr;
}

static JSValue dsw_sortedmap_keys(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_keys_array(ctx, s);
}

static JSValue dsw_sortedmap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_skip_t* s;
    skip_pairs_state st;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    st.ctx = ctx;
    st.i = 0;
    st.failed = 0;
    st.arr = JS_NewArray(ctx);
    if (JS_IsException(st.arr))
        return st.arr;
    dyn_btree_walk_leaves(s->b, skip_pairs_leaf, &st);
    if (st.failed == 2)
        return JS_EXCEPTION;
    if (st.failed) {
        JS_FreeValue(ctx, st.arr);
        return JS_EXCEPTION;
    }
    return st.arr;
}

static JSValue dsw_sortedmap_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_skip_t* s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(s->b));
}

const JSCFunctionListEntry dsw_sortedmap_proto[] = {
    JS_CFUNC_DEF("set", 2, dsw_sortedmap_set),
    JS_CFUNC_DEF("get", 1, dsw_sortedmap_get),
    JS_CFUNC_DEF("has", 1, dsw_sortedmap_has),
    JS_CFUNC_DEF("delete", 1, dsw_sortedmap_delete),
    JS_CFUNC_DEF("firstKey", 0, dsw_sortedmap_first_key),
    JS_CFUNC_DEF("lastKey", 0, dsw_sortedmap_last_key),
    JS_CFUNC_DEF("floorKey", 1, dsw_sortedmap_floor_key),
    JS_CFUNC_DEF("ceilKey", 1, dsw_sortedmap_ceil_key),
    JS_CFUNC_DEF("rangeQuery", 2, dsw_sortedmap_range),
    JS_CFUNC_DEF("keys", 0, dsw_sortedmap_keys),
    JS_CFUNC_DEF("entries", 0, dsw_sortedmap_entries),
    JS_CGETSET_DEF("size", dsw_sortedmap_size, NULL),
};

int dsw_sortedmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    return dsw_skip_write(ctx, w, obj, dsw_sortedmap_class_id, 1);
}

JSValue dsw_sortedmap_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts)
{
    (void)opts;
    return dsw_skip_read(ctx, r, dsw_sortedmap_class_id, "set", 1);
}

#endif
