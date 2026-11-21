#include "structures-internal.h"
#include "core/ds/ds-fenwick.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef dyn_fenwick_t dsw_fenwick_t;

static void dsw_fenwick_free(void* native)
{
    dsw_fenwick_t* f = (dsw_fenwick_t*)native;
    if (!f)
        return;
    dyn_fenwick_free(f);
    dyn_ds_free(f);
}

JSClassID dsw_fenwick_class_id;

DYN_FINALIZER(dsw_fenwick)

const JSClassDef dsw_fenwick_class = {
    "Fenwick",
    .finalizer = dsw_fenwick_finalizer,
};

JSValue dsw_fenwick_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_fenwick_t* f;
    uint32_t n = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], DYN_UF_MAX_N, &n))
        return JS_EXCEPTION;
    f = (dsw_fenwick_t*)dyn_ds_malloc(sizeof(*f));
    if (!f)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_fenwick_init(f, n) < 0) {
        dyn_ds_free(f);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_fenwick_class_id, f, dsw_fenwick_free);
}

static JSValue dsw_fenwick_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_fenwick_t* f;
    uint32_t i;
    double delta;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &delta, argv[1]))
        return JS_EXCEPTION;
    f = (dsw_fenwick_t*)dyn_plain_get(ctx, this_val, dsw_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (i >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    dyn_fenwick_add(f, i, delta);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_fenwick_prefix_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_fenwick_t* f;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    f = (dsw_fenwick_t*)dyn_plain_get(ctx, this_val, dsw_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (i >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    return JS_NewFloat64(ctx, dyn_fenwick_prefix(f, i));
}

static JSValue dsw_fenwick_range_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_fenwick_t* f;
    uint32_t lo, hi;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &lo))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &hi))
        return JS_EXCEPTION;
    f = (dsw_fenwick_t*)dyn_plain_get(ctx, this_val, dsw_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (lo >= f->n || hi >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    return JS_NewFloat64(ctx, dyn_fenwick_range(f, lo, hi));
}

static JSValue dsw_fenwick_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_fenwick_t* f = (dsw_fenwick_t*)dyn_plain_get(ctx, this_val, dsw_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)f->n);
}

const JSCFunctionListEntry dsw_fenwick_proto[] = {
    JS_CFUNC_DEF("update", 2, dsw_fenwick_update),
    JS_CFUNC_DEF("prefixSum", 1, dsw_fenwick_prefix_sum),
    JS_CFUNC_DEF("rangeQuery", 2, dsw_fenwick_range_query),
    JS_CGETSET_DEF("size", dsw_fenwick_size, NULL),
};

int dsw_fenwick_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_fenwick_t* f = JS_GetOpaque(obj, dsw_fenwick_class_id);
    (void)ctx;
    if (!f)
        return -1;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    return dyn_num_write(w, f->tree + 1, f->n);
}

JSValue dsw_fenwick_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i = 0, first;
    uint8_t kind;
    JSValue obj, arg;
    dsw_fenwick_t* f;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &n, &kind) < 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = first;
        kind = DS_NUM_RAW;
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    arg = JS_NewInt64(ctx, (int64_t)n);
    obj = dyn_ds_construct(ctx, dsw_fenwick_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj))
        return obj;
    f = JS_GetOpaque(obj, dsw_fenwick_class_id);
    if (dyn_num_fill(r, kind, f->tree + 1, n) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    (void)i;
    return obj;
}

#endif
