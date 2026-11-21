#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_btree_free(void* native)
{
    ds2_btree_w* w = (ds2_btree_w*)native;
    if (!w)
        return;
    dyn_btree_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_btree_class_id;

DS2_FINALIZER(ds2_btree)

static void ds2_btree_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_btree_w* w = JS_GetOpaque(val, ds2_btree_class_id);
    dyn_btree_iter it;
    if (!w)
        return;
    if (dyn_btree_iter_begin(w->c, &it))
        do {
            double k;
            const dyn_cell_t* v;
            if (dyn_btree_iter_get(&it, &k, &v))
                JS_MarkValue(rt, ds_cell_val(v), mark_func);
        } while (dyn_btree_iter_next(&it));
}

const JSClassDef ds2_btree_class = {
    "BTree",
    .finalizer = ds2_btree_finalizer,
    .gc_mark = ds2_btree_gc_mark,
};

JSValue ds2_btree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_btree_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_btree_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_btree_class_id, w, ds2_btree_free);
}

static int ds2_btree_key(JSContext* ctx, JSValueConst v, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (*out != *out) {
        JS_ThrowTypeError(ctx, "BTree keys must be ordered numbers, not NaN");
        return -1;
    }
    return 0;
}

static JSValue ds2_btree_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    JSValue dup;
    dyn_cell_t cell, old;
    int replaced = 0;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    dup = JS_DupValue(ctx, argv[1]);
    cell = ds_val_cell(dup);
    if (dyn_btree_set(w->c, k, &cell, &old, &replaced) < 0) {
        JS_FreeValue(ctx, dup);
        return ds2_oom(ctx);
    }
    if (replaced)
        ds_cell_release(w->rt, &old);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_btree_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    const dyn_cell_t* v;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    v = dyn_btree_get(w->c, k);
    return v ? JS_DupValue(ctx, ds_cell_val(v)) : JS_UNDEFINED;
}

static JSValue ds2_btree_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_btree_get(w->c, k) != NULL);
}

static JSValue ds2_btree_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    dyn_cell_t out;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!dyn_btree_del(w->c, k, &out))
        return JS_NewBool(ctx, 0);
    ds_cell_release(w->rt, &out);
    return JS_NewBool(ctx, 1);
}

static JSValue ds2_btree_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double k;
    (void)argc;
    (void)argv;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(magic ? dyn_btree_last(w->c, &k) : dyn_btree_first(w->c, &k)))
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, k);
}

static JSValue ds2_btree_near(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double k, out;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(magic ? dyn_btree_ceil(w->c, k, &out) : dyn_btree_floor(w->c, k, &out)))
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, out);
}

static JSValue ds2_btree_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double lo = 0, hi = 0;
    JSValue arr;
    dyn_btree_iter it;
    uint32_t out = 0;
    int bounded = !magic && argc >= 2;

    if (bounded && (ds2_btree_key(ctx, argv[0], &lo) || ds2_btree_key(ctx, argv[1], &hi)))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (!(bounded ? dyn_btree_iter_seek(w->c, lo, &it)
                  : dyn_btree_iter_begin(w->c, &it)))
        return arr;
    do {
        double k;
        const dyn_cell_t* v;
        JSValue item;
        if (!dyn_btree_iter_get(&it, &k, &v))
            break;
        if (bounded && k > hi)
            break;
        if (magic) {
            item = JS_NewFloat64(ctx, k);
        } else {
            JSValue pair[2];
            pair[0] = JS_NewFloat64(ctx, k);
            pair[1] = JS_DupValue(ctx, ds_cell_val(v));
            item = JS_NewArrayFrom(ctx, 2, pair);
            if (JS_IsException(item))
                goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, out++, item,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    } while (dyn_btree_iter_next(&it));
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_btree_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_btree_w* w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(w->c));
}

DS2_ITERATOR(ds2_btree, "rangeQuery")

const JSCFunctionListEntry ds2_btree_proto[] = {
    JS_CFUNC_DEF("set", 2, ds2_btree_set),
    JS_CFUNC_DEF("get", 1, ds2_btree_get),
    JS_CFUNC_DEF("has", 1, ds2_btree_has),
    JS_CFUNC_DEF("delete", 1, ds2_btree_delete),
    JS_CFUNC_MAGIC_DEF("firstKey", 0, ds2_btree_end, 0),
    JS_CFUNC_MAGIC_DEF("lastKey", 0, ds2_btree_end, 1),
    JS_CFUNC_MAGIC_DEF("floorKey", 1, ds2_btree_near, 0),
    JS_CFUNC_MAGIC_DEF("ceilKey", 1, ds2_btree_near, 1),
    JS_CFUNC_MAGIC_DEF("rangeQuery", 2, ds2_btree_range, 0),
    JS_CFUNC_MAGIC_DEF("keys", 0, ds2_btree_range, 1),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_btree_iterator),
    JS_CGETSET_DEF("size", ds2_btree_size, NULL),
};

int ds2_btree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_btree_w* o = (ds2_btree_w*)JS_GetOpaque(obj, ds2_btree_class_id);
    uint32_t n = dyn_btree_size(o->c), i = 0;
    double* k;
    dyn_btree_iter it;
    JSValue vals;
    int rc;

    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    k = (double*)dyn_ds_malloc(sizeof(*k) * ((size_t)n + 1));
    if (!k)
        return -1;
    vals = JS_NewArray(ctx);
    if (JS_IsException(vals)) {
        dyn_ds_free(k);
        return -1;
    }
    if (n && dyn_btree_iter_begin(o->c, &it))
        do {
            const dyn_cell_t* v;
            if (!dyn_btree_iter_get(&it, &k[i], &v) || i >= n)
                break;
            if (JS_DefinePropertyValueUint32(ctx, vals, i,
                    JS_DupValue(ctx, ds_cell_val(v)),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, vals);
                dyn_ds_free(k);
                return -1;
            }
            i++;
        } while (dyn_btree_iter_next(&it));
    rc = dyn_num_write(w, k, i);
    dyn_ds_free(k);
    if (rc == 0)
        rc = dyn_codec_write_values(ctx, w, vals);
    JS_FreeValue(ctx, vals);
    return rc;
}

JSValue ds2_btree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_btree_t* b;
    uint32_t n, e, i, first;
    double* k;
    JSValue vals;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    k = dyn_num_read_pairs(r, first, 1, &n, &e);
    if (!k)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(k);
        return vals;
    }
    b = dyn_btree_new();
    if (!b) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(k);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int vex = JS_IsException(v);
        dyn_cell_t cell = ds_val_cell(v), old;
        int replaced = 0;
        if (vex || k[i] != k[i] || dyn_btree_set(b, k[i], &cell, &old, &replaced) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, vals);
            dyn_btree_free(b, ds_cell_release, JS_GetRuntime(ctx));
            dyn_ds_free(k);
            return vex ? JS_EXCEPTION : dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (replaced)
            ds_cell_release(JS_GetRuntime(ctx), &old);
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(k);
    DS2_WRAP(ds2_btree, dyn_btree_free(b, ds_cell_release, JS_GetRuntime(ctx)), b);
}

#endif
