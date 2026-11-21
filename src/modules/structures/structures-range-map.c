#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_dup_cell(void* ud, const dyn_cell_t* in, dyn_cell_t* out)
{
    *out = ds_val_cell(JS_DupValueRT((JSRuntime*)ud, ds_cell_val(in)));
}

static void ds2_rmap_free(void* native)
{
    ds2_rmap_w* w = (ds2_rmap_w*)native;
    if (!w)
        return;
    dyn_rmap_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_rmap_class_id;

DS2_FINALIZER(ds2_rmap)

static void ds2_rmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_rmap_w* w = JS_GetOpaque(val, ds2_rmap_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_rmap_count(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        dyn_rmap_at(w->c, i, &lo, &hi, &v);
        JS_MarkValue(rt, ds_cell_val(v), mark_func);
    }
}

const JSClassDef ds2_rmap_class = {
    "RangeMap",
    .finalizer = ds2_rmap_finalizer,
    .gc_mark = ds2_rmap_gc_mark,
};

JSValue ds2_rmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_rmap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_rmap_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_rmap_class_id, w, ds2_rmap_free);
}

static JSValue ds2_rmap_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    double lo, hi;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(hi > lo))
        return JS_DupValue(ctx, this_val);
    cell = ds_val_cell(JS_DupValue(ctx, argv[2]));
    if (dyn_rmap_put(w->c, lo, hi, &cell, ds2_dup_cell, ds_cell_release,
            w->rt)
        < 0) {
        JS_FreeValue(ctx, ds_cell_val(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    const dyn_cell_t* v;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    v = dyn_rmap_get(w->c, x);
    return v ? JS_DupValue(ctx, ds_cell_val(v)) : JS_UNDEFINED;
}

static JSValue ds2_rmap_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (dyn_rmap_remove(w->c, lo, hi, ds2_dup_cell, ds_cell_release, w->rt) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rmap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_rmap_count(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        JSValue t;
        dyn_rmap_at(w->c, i, &lo, &hi, &v);
        t = ds2_tuple3(ctx, JS_NewFloat64(ctx, lo), JS_NewFloat64(ctx, hi),
            JS_DupValue(ctx, ds_cell_val(v)));
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_rmap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_rmap_w* w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_rmap_count(w->c));
}

DS2_ITERATOR(ds2_rmap, "entries")

const JSCFunctionListEntry ds2_rmap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_rmap_iterator),
    JS_CFUNC_DEF("put", 3, ds2_rmap_put),
    JS_CFUNC_DEF("get", 1, ds2_rmap_get),
    JS_CFUNC_DEF("remove", 2, ds2_rmap_remove),
    JS_CFUNC_DEF("entries", 0, ds2_rmap_entries),
    JS_CGETSET_DEF("size", ds2_rmap_size, NULL),
};

static const dyn_cell_t* ds2_rmap_cell(void* native, uint32_t i)
{
    double lo, hi;
    const dyn_cell_t* v = NULL;
    dyn_rmap_at((dyn_rmap_t*)native, i, &lo, &hi, &v);
    return v;
}

int ds2_rmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_rmap_w* o = (ds2_rmap_w*)JS_GetOpaque(obj, ds2_rmap_class_id);
    uint32_t i, n = dyn_rmap_count(o->c);
    double* b;
    int rc;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    b = (double*)dyn_ds_malloc(sizeof(*b) * ((size_t)n * 2 + 1));
    if (!b)
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* v;
        dyn_rmap_at(o->c, i, &b[i * 2], &b[i * 2 + 1], &v);
    }
    rc = dyn_num_write(w, b, (size_t)n * 2);
    dyn_ds_free(b);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_rmap_cell);
}

JSValue ds2_rmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_rmap_t* m;
    uint32_t n, i;
    JSValue vals;
    double *lo, *hi;
    (void)opts;

    {
        uint32_t e, first = dyn_de_u32(r);
        double* b;
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        b = dyn_num_read_pairs(r, first, 2, &n, &e);
        if (!b)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        lo = (double*)dyn_ds_calloc(n ? n : 1, sizeof(double));
        hi = (double*)dyn_ds_calloc(n ? n : 1, sizeof(double));
        if (!lo || !hi) {
            dyn_ds_free(lo);
            dyn_ds_free(hi);
            dyn_ds_free(b);
            return ds2_oom(ctx);
        }
        for (i = 0; i < n; i++) {
            lo[i] = b[i * 2];
            hi[i] = b[i * 2 + 1];
        }
        dyn_ds_free(b);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(lo);
        dyn_ds_free(hi);
        return vals;
    }
    m = dyn_rmap_new();
    if (!m) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(lo);
        dyn_ds_free(hi);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int vex = JS_IsException(v);
        dyn_cell_t cell = ds_val_cell(v);
        if (!vex && !(hi[i] > lo[i])) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (vex || dyn_rmap_put(m, lo[i], hi[i], &cell, ds2_dup_cell, ds_cell_release,
                JS_GetRuntime(ctx))
            < 0) {
            JS_FreeValue(ctx, v);
            dyn_rmap_free(m, ds_cell_release, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_ds_free(lo);
            dyn_ds_free(hi);
            return vex ? JS_EXCEPTION : ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(lo);
    dyn_ds_free(hi);
    DS2_WRAP(ds2_rmap, dyn_rmap_free(m, ds_cell_release, JS_GetRuntime(ctx)), m);
}

#endif
