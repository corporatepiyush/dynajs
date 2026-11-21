#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_rset_free(void* native)
{
    ds2_rset_w* w = (ds2_rset_w*)native;
    if (!w)
        return;
    dyn_rset_free(w->c);
    dyn_ds_free(w);
}

JSClassID ds2_rset_class_id;

DS2_FINALIZER(ds2_rset)

const JSClassDef ds2_rset_class = {
    "RangeSet",
    .finalizer = ds2_rset_finalizer,
};

JSValue ds2_rset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_rset_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_rset_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_rset_class_id, w, ds2_rset_free);
}

static JSValue ds2_rset_edit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int remove)
{
    ds2_rset_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if ((remove ? dyn_rset_remove(w->c, lo, hi)
                : dyn_rset_add(w->c, lo, hi))
        < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rset_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_rset_contains(w->c, x));
}

static JSValue ds2_rset_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int intersects)
{
    ds2_rset_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, intersects ? dyn_rset_intersects(w->c, lo, hi) : dyn_rset_encloses(w->c, lo, hi));
}

static JSValue ds2_spans_array(JSContext* ctx, const dyn_rset_t* s)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i, n;
    if (JS_IsException(arr))
        return arr;
    n = dyn_rset_count(s);
    for (i = 0; i < n; i++) {
        double lo, hi;
        JSValue pair;
        dyn_rset_at(s, i, &lo, &hi);
        pair = ds2_tuple2(ctx, JS_NewFloat64(ctx, lo), JS_NewFloat64(ctx, hi));
        if (JS_IsException(pair))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, arr, i, pair,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_rset_ranges(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    return ds2_spans_array(ctx, w->c);
}

static JSValue ds2_rset_complement(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    dyn_rset_t* out;
    double lo, hi;
    JSValue r;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    out = dyn_rset_new();
    if (!out)
        return ds2_oom(ctx);
    if (dyn_rset_complement(w->c, lo, hi, out) < 0) {
        dyn_rset_free(out);
        return ds2_oom(ctx);
    }
    r = ds2_spans_array(ctx, out);
    dyn_rset_free(out);
    return r;
}

static JSValue ds2_rset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_rset_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_rset_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_rset_count(w->c));
}

static JSValue ds2_rset_measure(JSContext* ctx, JSValueConst this_val)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    double total = 0;
    uint32_t i, n;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_rset_count(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        dyn_rset_at(w->c, i, &lo, &hi);
        total += hi - lo;
    }
    return JS_NewFloat64(ctx, total);
}

DS2_ITERATOR(ds2_rset, "ranges")

const JSCFunctionListEntry ds2_rset_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_rset_iterator),
    JS_CFUNC_MAGIC_DEF("add", 2, ds2_rset_edit, 0),
    JS_CFUNC_MAGIC_DEF("remove", 2, ds2_rset_edit, 1),
    JS_CFUNC_DEF("contains", 1, ds2_rset_contains),
    JS_CFUNC_MAGIC_DEF("encloses", 2, ds2_rset_query, 0),
    JS_CFUNC_MAGIC_DEF("intersects", 2, ds2_rset_query, 1),
    JS_CFUNC_DEF("ranges", 0, ds2_rset_ranges),
    JS_CFUNC_DEF("complement", 2, ds2_rset_complement),
    JS_CFUNC_DEF("clear", 0, ds2_rset_clear),
    JS_CGETSET_DEF("size", ds2_rset_size, NULL),
    JS_CGETSET_DEF("measure", ds2_rset_measure, NULL),
};

int ds2_rset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_rset_w* o = (ds2_rset_w*)JS_GetOpaque(obj, ds2_rset_class_id);
    uint32_t i, n = dyn_rset_count(o->c);
    double* v;
    int rc;
    (void)ctx;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    v = (double*)dyn_ds_malloc(sizeof(*v) * ((size_t)n * 2 + 1));
    if (!v)
        return -1;
    for (i = 0; i < n; i++)
        dyn_rset_at(o->c, i, &v[i * 2], &v[i * 2 + 1]);
    rc = dyn_num_write(w, v, (size_t)n * 2);
    dyn_ds_free(v);
    return rc;
}

JSValue ds2_rset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_rset_t* s;
    uint32_t n, i, e, first;
    double* b;
    (void)opts;
    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    b = dyn_num_read_pairs(r, first, 2, &n, &e);
    if (!b)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    s = dyn_rset_new();
    if (!s) {
        dyn_ds_free(b);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        if (dyn_rset_add(s, b[i * 2], b[i * 2 + 1]) < 0) {
            dyn_ds_free(b);
            dyn_rset_free(s);
            return ds2_oom(ctx);
        }
    }
    dyn_ds_free(b);
    if (!dyn_de_ok(r)) {
        dyn_rset_free(s);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    DS2_WRAP(ds2_rset, dyn_rset_free(s), s);
}

#endif
