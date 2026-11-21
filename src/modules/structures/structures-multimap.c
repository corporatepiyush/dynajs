#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_mmap_free(void* native)
{
    ds2_mmap_w* w = (ds2_mmap_w*)native;
    if (!w)
        return;
    dyn_mmap_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_mmap_class_id;

DS2_FINALIZER(ds2_mmap)

static void ds2_mmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_mmap_w* w = JS_GetOpaque(val, ds2_mmap_class_id);
    uint32_t i, j, nk, nv;
    if (!w)
        return;
    nk = dyn_mmap_keys(w->c);
    for (i = 0; i < nk; i++) {
        const dyn_cell_t* cells = dyn_mmap_cells_at(w->c, i, &nv);
        for (j = 0; j < nv; j++)
            JS_MarkValue(rt, ds_cell_val(&cells[j]), mark_func);
    }
}

const JSClassDef ds2_mmap_class = {
    "Multimap",
    .finalizer = ds2_mmap_finalizer,
    .gc_mark = ds2_mmap_gc_mark,
};

JSValue ds2_mmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mmap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mmap_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mmap_class_id, w, ds2_mmap_free);
}

static JSValue ds2_mmap_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    cell = ds_val_cell(JS_DupValue(ctx, argv[1]));
    if (dyn_mmap_put(w->c, k.p, k.n, &cell) < 0) {
        JS_FreeValue(ctx, ds_cell_val(&cell));
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    const dyn_cell_t* cells;
    uint32_t n, i;
    JSValue arr;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    cells = dyn_mmap_cells_for(w->c, k.p, k.n, &n);
    ds2_str_release(ctx, &k);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx,
                    ds_cell_val(&cells[i])),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static JSValue ds2_mmap_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t n;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    n = dyn_mmap_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_NewInt64(ctx, (int64_t)n);
}

static JSValue ds2_mmap_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t n;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    n = dyn_mmap_remove_key(w->c, k.p, k.n, ds_cell_release, w->rt);
    ds2_str_release(ctx, &k);
    return JS_NewInt64(ctx, (int64_t)n);
}

static JSValue ds2_mmap_remove_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t idx;
    dyn_cell_t out;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "removeAt(key, index): index is required");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &idx, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (!dyn_mmap_remove_at(w->c, k.p, k.n, idx, &out)) {
        ds2_str_release(ctx, &k);
        return JS_UNDEFINED;
    }
    ds2_str_release(ctx, &k);
    return ds_cell_val(&out);
}

static JSValue ds2_mmap_view(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int entries)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    JSValue arr;
    uint32_t i, nk, out = 0;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    nk = dyn_mmap_keys(w->c);
    for (i = 0; i < nk; i++) {
        const char* k;
        size_t kl;
        uint32_t nv, j;
        const dyn_cell_t* cells;
        dyn_mmap_key_at(w->c, i, &k, &kl, &nv);
        if (!entries) {
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    ds2_new_str(ctx, k, kl),
                    JS_PROP_C_W_E)
                < 0)
                goto fail;
            continue;
        }
        cells = dyn_mmap_cells_at(w->c, i, &nv);
        for (j = 0; j < nv; j++) {
            JSValue pair = ds2_tuple2(ctx, ds2_new_str(ctx, k, kl),
                JS_DupValue(ctx, ds_cell_val(&cells[j])));
            if (JS_IsException(pair))
                goto fail;
            if (JS_DefinePropertyValueUint32(ctx, arr, out++, pair,
                    JS_PROP_C_W_E)
                < 0)
                goto fail;
        }
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_mmap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmap_size(w->c));
}

static JSValue ds2_mmap_keycount(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmap_keys(w->c));
}

DS2_ITERATOR(ds2_mmap, "entries")

const JSCFunctionListEntry ds2_mmap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_mmap_iterator),
    JS_CFUNC_DEF("put", 2, ds2_mmap_put),
    JS_CFUNC_DEF("get", 1, ds2_mmap_get),
    JS_CFUNC_DEF("count", 1, ds2_mmap_count),
    JS_CFUNC_DEF("delete", 1, ds2_mmap_delete),
    JS_CFUNC_DEF("removeAt", 2, ds2_mmap_remove_at),
    JS_CFUNC_MAGIC_DEF("keys", 0, ds2_mmap_view, 0),
    JS_CFUNC_MAGIC_DEF("entries", 0, ds2_mmap_view, 1),
    JS_CGETSET_DEF("size", ds2_mmap_size, NULL),
    JS_CGETSET_DEF("keyCount", ds2_mmap_keycount, NULL),
};

static _Thread_local struct {
    const dyn_mmap_t* m;
    uint32_t k, base;
} ds2_mmap_cur;

static const dyn_cell_t* ds2_mmap_flat(void* native, uint32_t i)
{
    dyn_mmap_t* m = (dyn_mmap_t*)native;
    uint32_t k, base, nv;
    if (ds2_mmap_cur.m != m || i < ds2_mmap_cur.base) {
        ds2_mmap_cur.m = m;
        ds2_mmap_cur.k = 0;
        ds2_mmap_cur.base = 0;
    }
    k = ds2_mmap_cur.k;
    base = ds2_mmap_cur.base;
    for (; k < dyn_mmap_keys(m); k++) {
        const dyn_cell_t* cells = dyn_mmap_cells_at(m, k, &nv);
        if (i - base < nv) {
            ds2_mmap_cur.k = k;
            ds2_mmap_cur.base = base;
            return &cells[i - base];
        }
        base += nv;
    }
    ds2_mmap_cur.m = NULL;
    return NULL;
}

int ds2_mmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mmap_w* o = (ds2_mmap_w*)JS_GetOpaque(obj, ds2_mmap_class_id);
    uint32_t i, nk = dyn_mmap_keys(o->c);
    if (dyn_ser_u32(w, nk) < 0)
        return -1;
    for (i = 0; i < nk; i++) {
        const char* k;
        size_t kl;
        uint32_t nv;
        dyn_mmap_key_at(o->c, i, &k, &kl, &nv);
        if (dyn_ser_blob(w, k, kl) < 0 || dyn_ser_u32(w, nv) < 0)
            return -1;
    }
    return ds2_write_cells(ctx, w, o->c, (uint32_t)dyn_mmap_size(o->c),
        ds2_mmap_flat);
}

JSValue ds2_mmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mmap_t* m;
    uint32_t nk, i, j, at;
    uint64_t total = 0;
    JSValue vals;
    uint32_t* counts;
    const char** keys;
    size_t* klens;
    (void)opts;

    if (dyn_de_count(r, &nk, 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    keys = (const char**)dyn_ds_calloc(nk ? nk : 1, sizeof(*keys));
    klens = (size_t*)dyn_ds_calloc(nk ? nk : 1, sizeof(*klens));
    counts = (uint32_t*)dyn_ds_calloc(nk ? nk : 1, sizeof(*counts));
    if (!keys || !klens || !counts) {
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        dyn_ds_free(counts);
        return ds2_oom(ctx);
    }
    for (i = 0; i < nk; i++) {
        keys[i] = dyn_de_blob(r, &klens[i]);
        counts[i] = dyn_de_u32(r);
        if (!dyn_de_ok(r) || counts[i] > dyn_de_left(r)) {
            dyn_ds_free(keys);
            dyn_ds_free(klens);
            dyn_ds_free(counts);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        total += counts[i];
        if (total > DYN_MAX_CAPACITY) {
            dyn_ds_free(keys);
            dyn_ds_free(klens);
            dyn_ds_free(counts);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    vals = ds2_read_cells(ctx, r, (uint32_t)total);
    if (JS_IsException(vals)) {
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        dyn_ds_free(counts);
        return vals;
    }
    m = dyn_mmap_new();
    if (!m) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        dyn_ds_free(counts);
        return ds2_oom(ctx);
    }
    at = 0;
    for (i = 0; i < nk; i++)
        for (j = 0; j < counts[i]; j++) {
            JSValue v = JS_GetPropertyUint32(ctx, vals, at++);
            int vex = JS_IsException(v);
            dyn_cell_t cell = ds_val_cell(v);
            if (vex || dyn_mmap_put(m, keys[i], klens[i], &cell) < 0) {
                JS_FreeValue(ctx, v);
                dyn_mmap_free(m, ds_cell_release, JS_GetRuntime(ctx));
                JS_FreeValue(ctx, vals);
                dyn_ds_free(keys);
                dyn_ds_free(klens);
                dyn_ds_free(counts);
                return vex ? JS_EXCEPTION : ds2_oom(ctx);
            }
        }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(keys);
    dyn_ds_free(klens);
    dyn_ds_free(counts);
    DS2_WRAP(ds2_mmap, dyn_mmap_free(m, ds_cell_release, JS_GetRuntime(ctx)), m);
}

#endif
