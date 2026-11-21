#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_mmheap_free(void* native)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)native;
    if (!w)
        return;
    dyn_mmheap_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_mmheap_class_id;

DS2_FINALIZER(ds2_mmheap)

static void ds2_mmheap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_mmheap_w* w = JS_GetOpaque(val, ds2_mmheap_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_mmheap_size(w->c);
    for (i = 0; i < n; i++)
        JS_MarkValue(rt, ds_cell_val(dyn_mmheap_cell_at(w->c, i)), mark_func);
}

const JSClassDef ds2_mmheap_class = {
    "MinMaxHeap",
    .finalizer = ds2_mmheap_finalizer,
    .gc_mark = ds2_mmheap_gc_mark,
};

JSValue ds2_mmheap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mmheap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mmheap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mmheap_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mmheap_class_id, w, ds2_mmheap_free);
}

static JSValue ds2_mmheap_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmheap_w* w;
    double pri;
    dyn_cell_t cell;

    if (JS_ToFloat64(ctx, &pri, argv[0]))
        return JS_EXCEPTION;
    if (isnan(pri))
        return JS_ThrowRangeError(ctx, "priority must not be NaN");
    w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    if (!w)
        return JS_EXCEPTION;
    cell = ds_val_cell(JS_DupValue(ctx, argc >= 2 ? argv[1] : argv[0]));
    if (dyn_mmheap_push(w->c, pri, &cell) < 0) {
        JS_FreeValue(ctx, ds_cell_val(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mmheap_take(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    double pri;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (magic < 2) {
        dyn_cell_t out;
        int ok = magic ? dyn_mmheap_pop_max(w->c, &pri, &out)
                       : dyn_mmheap_pop_min(w->c, &pri, &out);
        return ok ? ds_cell_val(&out) : JS_UNDEFINED;
    } else {
        const dyn_cell_t* v;
        int ok = magic == 3 ? dyn_mmheap_peek_max(w->c, &pri, &v)
                            : dyn_mmheap_peek_min(w->c, &pri, &v);
        return ok ? JS_DupValue(ctx, ds_cell_val(v)) : JS_UNDEFINED;
    }
}

static JSValue ds2_mmheap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmheap_size(w->c));
}

const JSCFunctionListEntry ds2_mmheap_proto[] = {
    JS_CFUNC_DEF("push", 1, ds2_mmheap_push),
    JS_CFUNC_MAGIC_DEF("popMin", 0, ds2_mmheap_take, 0),
    JS_CFUNC_MAGIC_DEF("popMax", 0, ds2_mmheap_take, 1),
    JS_CFUNC_MAGIC_DEF("peekMin", 0, ds2_mmheap_take, 2),
    JS_CFUNC_MAGIC_DEF("peekMax", 0, ds2_mmheap_take, 3),
    JS_CGETSET_DEF("size", ds2_mmheap_size, NULL),
};

static const dyn_cell_t* ds2_mmheap_cell(void* native, uint32_t i)
{
    return dyn_mmheap_cell_at((dyn_mmheap_t*)native, i);
}

int ds2_mmheap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mmheap_w* o = (ds2_mmheap_w*)JS_GetOpaque(obj, ds2_mmheap_class_id);
    uint32_t i, n = dyn_mmheap_size(o->c);
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    {
        double* b = (double*)dyn_ds_malloc(sizeof(*b) * ((size_t)n + 1));
        int rc;
        if (!b)
            return -1;
        for (i = 0; i < n; i++)
            b[i] = dyn_mmheap_pri_at(o->c, i);
        rc = dyn_num_write(w, b, n);
        dyn_ds_free(b);
        if (rc < 0)
            return -1;
    }
    return ds2_write_cells(ctx, w, o->c, n, ds2_mmheap_cell);
}

JSValue ds2_mmheap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mmheap_t* h;
    uint32_t n, i;
    JSValue vals;
    double* pri;
    (void)opts;

    {
        uint32_t e, first = dyn_de_u32(r);
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        pri = dyn_num_read_pairs(r, first, 1, &n, &e);
        if (!pri)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(pri);
        return vals;
    }
    h = dyn_mmheap_new();
    if (!h) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(pri);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int vex = JS_IsException(v);
        dyn_cell_t cell = ds_val_cell(v);
        if (vex || dyn_mmheap_push(h, pri[i], &cell) < 0) {
            JS_FreeValue(ctx, v);
            dyn_mmheap_free(h, ds_cell_release, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_ds_free(pri);
            return vex ? JS_EXCEPTION : ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(pri);
    DS2_WRAP(ds2_mmheap, dyn_mmheap_free(h, ds_cell_release, JS_GetRuntime(ctx)),
        h);
}

#endif
