#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_bimap_free(void* native)
{
    ds2_bimap_w* w = (ds2_bimap_w*)native;
    if (!w)
        return;
    dyn_bimap_free(w->c);
    dyn_ds_free(w);
}

JSClassID ds2_bimap_class_id;

DS2_FINALIZER(ds2_bimap)

const JSClassDef ds2_bimap_class = {
    "BiMap",
    .finalizer = ds2_bimap_finalizer,
};

JSValue ds2_bimap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_bimap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_bimap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_bimap_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_bimap_class_id, w, ds2_bimap_free);
}

static JSValue ds2_bimap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int force)
{
    ds2_bimap_w* w;
    ds2_str k, v;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &v, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        ds2_str_release(ctx, &v);
        return JS_EXCEPTION;
    }
    r = dyn_bimap_put(w->c, k.p, k.n, v.p, v.n, force);
    ds2_str_release(ctx, &k);
    ds2_str_release(ctx, &v);
    if (r == DYN_BIMAP_VALUE_TAKEN)
        return JS_ThrowTypeError(ctx,
            "value is already bound to another key; use forceSet to rebind");
    if (r < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_bimap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int inverse)
{
    ds2_bimap_w* w;
    ds2_str k;
    const char* r;
    size_t rn = 0;
    JSValue out;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    r = inverse ? dyn_bimap_key_for(w->c, k.p, k.n, &rn)
                : dyn_bimap_get(w->c, k.p, k.n, &rn);
    out = r ? ds2_new_str(ctx, r, rn) : JS_UNDEFINED;
    ds2_str_release(ctx, &k);
    return out;
}

static JSValue ds2_bimap_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_bimap_w* w;
    ds2_str k;
    size_t rn = 0;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        r = dyn_bimap_get(w->c, k.p, k.n, &rn) != NULL;
        break;
    case 1:
        r = dyn_bimap_key_for(w->c, k.p, k.n, &rn) != NULL;
        break;
    case 2:
        r = dyn_bimap_remove(w->c, k.p, k.n);
        break;
    default:
        r = dyn_bimap_remove_value(w->c, k.p, k.n);
        break;
    }
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, r);
}

static JSValue ds2_bimap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int inverse)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_bimap_size(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char *k, *v;
        size_t kl, vl;
        JSValue pair;
        dyn_bimap_at(w->c, i, &k, &kl, &v, &vl);
        pair = inverse ? ds2_tuple2(ctx, ds2_new_str(ctx, v, vl), ds2_new_str(ctx, k, kl))
                       : ds2_tuple2(ctx, ds2_new_str(ctx, k, kl), ds2_new_str(ctx, v, vl));
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

static JSValue ds2_bimap_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_bimap_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_bimap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_bimap_size(w->c));
}

DS2_ITERATOR(ds2_bimap, "entries")

const JSCFunctionListEntry ds2_bimap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_bimap_iterator),
    JS_CFUNC_MAGIC_DEF("set", 2, ds2_bimap_set, 0),
    JS_CFUNC_MAGIC_DEF("forceSet", 2, ds2_bimap_set, 1),
    JS_CFUNC_MAGIC_DEF("get", 1, ds2_bimap_get, 0),
    JS_CFUNC_MAGIC_DEF("keyOf", 1, ds2_bimap_get, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, ds2_bimap_op, 0),
    JS_CFUNC_MAGIC_DEF("hasValue", 1, ds2_bimap_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, ds2_bimap_op, 2),
    JS_CFUNC_MAGIC_DEF("deleteValue", 1, ds2_bimap_op, 3),
    JS_CFUNC_MAGIC_DEF("entries", 0, ds2_bimap_entries, 0),
    JS_CFUNC_MAGIC_DEF("inverseEntries", 0, ds2_bimap_entries, 1),
    JS_CFUNC_DEF("clear", 0, ds2_bimap_clear),
    JS_CGETSET_DEF("size", ds2_bimap_size, NULL),
};

int ds2_bimap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_bimap_w* o = (ds2_bimap_w*)JS_GetOpaque(obj, ds2_bimap_class_id);
    uint32_t i, n = dyn_bimap_size(o->c);
    (void)ctx;
    if (dyn_ser_u32(w, n) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char *k, *v;
        size_t kl, vl;
        dyn_bimap_at(o->c, i, &k, &kl, &v, &vl);
        if (dyn_ser_blob(w, k, kl) < 0 || dyn_ser_blob(w, v, vl) < 0)
            return -1;
    }
    return 0;
}

JSValue ds2_bimap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_bimap_t* b;
    uint32_t n, i;
    (void)opts;
    if (dyn_de_count(r, &n, 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    b = dyn_bimap_new();
    if (!b)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kl, vl;
        const char* k = dyn_de_blob(r, &kl);
        const char* v = dyn_de_blob(r, &vl);
        if (!dyn_de_ok(r)) {
            dyn_bimap_free(b);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (dyn_bimap_put(b, k, kl, v, vl, 1) < 0) {
            dyn_bimap_free(b);
            return ds2_oom(ctx);
        }
    }
    DS2_WRAP(ds2_bimap, dyn_bimap_free(b), b);
}

#endif
