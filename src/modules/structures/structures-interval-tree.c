#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_itree_free(void* native)
{
    ds2_itree_w* w = (ds2_itree_w*)native;
    if (!w)
        return;
    dyn_itree_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_itree_class_id;

DS2_FINALIZER(ds2_itree)

static void ds2_itree_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_itree_w* w = JS_GetOpaque(val, ds2_itree_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_itree_size(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        dyn_itree_at(w->c, i, &lo, &hi, &v);
        JS_MarkValue(rt, ds_cell_val(v), mark_func);
    }
}

const JSClassDef ds2_itree_class = {
    "IntervalTree",
    .finalizer = ds2_itree_finalizer,
    .gc_mark = ds2_itree_gc_mark,
};

JSValue ds2_itree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_itree_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_itree_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_itree_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_itree_class_id, w, ds2_itree_free);
}

static JSValue ds2_itree_insert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_itree_w* w;
    double lo, hi;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    if (lo > hi)
        return JS_ThrowRangeError(ctx, "IntervalTree: lo must not exceed hi");
    w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    cell = ds_val_cell(JS_DupValue(ctx, argv[2]));
    if (dyn_itree_insert(w->c, lo, hi, &cell) < 0) {
        JS_FreeValue(ctx, ds_cell_val(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_itree_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int point)
{
    ds2_itree_w* w;
    double lo, hi;
    uint32_t *idx = NULL, n, i, cap;
    JSValue arr;

    if (point) {
        if (JS_ToFloat64(ctx, &lo, argv[0]))
            return JS_EXCEPTION;
        if (isnan(lo))
            return JS_ThrowRangeError(ctx, "interval bounds must not be NaN");
        hi = lo;
    } else if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1])) {
        return JS_EXCEPTION;
    }
    (void)argc;
    w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(hi >= lo)) {
        JSValue res = JS_NewArray(ctx);
        if (JS_IsException(res))
            return res;
        return res;
    }
    cap = dyn_itree_size(w->c);
    if (cap) {
        idx = (uint32_t*)dyn_ds_malloc((size_t)cap * sizeof(uint32_t));
        if (!idx)
            return ds2_oom(ctx);
    }
    n = dyn_itree_query(w->c, lo, hi, idx, cap);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_ds_free(idx);
        return arr;
    }
    for (i = 0; i < n; i++) {
        double a, b;
        const dyn_cell_t* v;
        JSValue t;
        dyn_itree_at(w->c, idx[i], &a, &b, &v);
        t = ds2_tuple3(ctx, JS_NewFloat64(ctx, a), JS_NewFloat64(ctx, b),
            JS_DupValue(ctx, ds_cell_val(v)));
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    dyn_ds_free(idx);
    return arr;
fail:
    dyn_ds_free(idx);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_itree_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_itree_w* w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_itree_size(w->c));
}

const JSCFunctionListEntry ds2_itree_proto[] = {
    JS_CFUNC_DEF("insert", 3, ds2_itree_insert),
    JS_CFUNC_MAGIC_DEF("overlapping", 2, ds2_itree_query, 0),
    JS_CFUNC_MAGIC_DEF("at", 1, ds2_itree_query, 1),
    JS_CGETSET_DEF("size", ds2_itree_size, NULL),
};

static const dyn_cell_t* ds2_itree_cell(void* native, uint32_t i)
{
    double lo, hi;
    const dyn_cell_t* v = NULL;
    dyn_itree_at((dyn_itree_t*)native, i, &lo, &hi, &v);
    return v;
}

int ds2_itree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_itree_w* o = (ds2_itree_w*)JS_GetOpaque(obj, ds2_itree_class_id);
    uint32_t i, n = dyn_itree_size(o->c);
    double* b;
    int rc;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    b = (double*)dyn_ds_malloc(sizeof(*b) * ((size_t)n * 2 + 1));
    if (!b)
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* v;
        dyn_itree_at(o->c, i, &b[i * 2], &b[i * 2 + 1], &v);
    }
    rc = dyn_num_write(w, b, (size_t)n * 2);
    dyn_ds_free(b);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_itree_cell);
}

JSValue ds2_itree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_itree_t* t;
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
    t = dyn_itree_new();
    if (!t) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(lo);
        dyn_ds_free(hi);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int vex = JS_IsException(v);
        dyn_cell_t cell = ds_val_cell(v);
        if (!vex && (!(lo[i] >= lo[i]) || !(hi[i] >= hi[i]) || lo[i] > hi[i])) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (vex || dyn_itree_insert(t, lo[i], hi[i], &cell) < 0) {
            JS_FreeValue(ctx, v);
            dyn_itree_free(t, ds_cell_release, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_ds_free(lo);
            dyn_ds_free(hi);
            return vex ? JS_EXCEPTION : ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(lo);
    dyn_ds_free(hi);
    DS2_WRAP(ds2_itree, dyn_itree_free(t, ds_cell_release, JS_GetRuntime(ctx)), t);
}

#endif
