#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_zsl_t sl;
} sdb_omap_w;

JSClassID sdb_omap_class_id;

static void sdb_omap_free(void* native)
{
    sdb_omap_w* w = (sdb_omap_w*)native;
    if (!w)
        return;
    dyn_zsl_destroy(&w->sl);
    dyn_ds_free(w);
}

static void sdb_omap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_omap_free(JS_GetOpaque(val, sdb_omap_class_id));
}

static void sdb_omap_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    sdb_omap_w* w = JS_GetOpaque(val, sdb_omap_class_id);
    if (w)
        sdb_zsl_mark(rt, &w->sl, mark_func);
}

const JSClassDef sdb_omap_class = {
    "OrderedMap",
    .finalizer = sdb_omap_finalizer,
    .gc_mark = sdb_omap_gc_mark,
};

JSValue sdb_omap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_omap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    if (sdb_zsl_init(&w->sl, ctx) < 0) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_omap_class_id, w, sdb_omap_free);
}

static int sdb_omap_put(JSContext* ctx, sdb_omap_w* w, const char* k, size_t n, JSValueConst v)
{
    dyn_zsl_node* x = dyn_zsl_find(&w->sl, k, n);
    if (x) {
        JSValue old = ds_cell_val(&x->val);
        x->val = ds_val_cell(JS_DupValue(ctx, v));
        JS_FreeValue(ctx, old);
        return 0;
    }
    {
        JSValue d = JS_DupValue(ctx, v);
        if (sdb_zsl_insert(&w->sl, 0, k, n, d, NULL) < 0) {
            JS_FreeValue(ctx, d);
            return -1;
        }
    }
    return 0;
}

static JSValue sdb_omap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str k;
    int rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_omap_put(ctx, w, k.p, k.n, argv[1]);
    ds2_str_release(ctx, &k);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_omap_entry(JSContext* ctx, const dyn_zsl_node* x)
{
    return sdb_pair(ctx, dyn_zsl_keystr(ctx, x), JS_DupValue(ctx, ds_cell_val(&x->val)));
}

static JSValue sdb_omap_key_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str k;
    dyn_zsl_node* x = NULL;
    uint32_t r = 0;
    JSValue res = JS_UNDEFINED;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        x = dyn_zsl_find(&w->sl, k.p, k.n);
        res = x ? JS_DupValue(ctx, ds_cell_val(&x->val)) : JS_UNDEFINED;
        break;
    case 1:
        res = JS_NewBool(ctx, dyn_zsl_find(&w->sl, k.p, k.n) != NULL);
        break;
    case 2:
        x = dyn_zsl_find(&w->sl, k.p, k.n);
        if (x)
            dyn_zsl_drop(&w->sl, x);
        res = JS_NewBool(ctx, x != NULL);
        break;
    case 3:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 4:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 5:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 6:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    default:
        dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, &r);
        res = JS_NewInt64(ctx, r);
        break;
    }
    ds2_str_release(ctx, &k);
    return res;
}

static JSValue sdb_omap_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    dyn_zsl_node* x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    return x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str lo = { 0 }, hi = { 0 };
    uint32_t limit = UINT32_MAX, out = 0;
    int reverse = 0, keys_only = magic == 3;
    dyn_zsl_node* x;
    JSValue arr;
    JSValueConst opts = JS_UNDEFINED;

    if (magic == 0 || magic == 1) {
        if (ds2_str_get(ctx, &lo, argv[0]))
            return JS_EXCEPTION;
    }
    if (magic == 0) {
        if (ds2_str_get(ctx, &hi, argv[1])) {
            ds2_str_release(ctx, &lo);
            return JS_EXCEPTION;
        }
        if (argc > 2)
            opts = argv[2];
    } else if (magic == 1 && argc > 1)
        opts = argv[1];
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || (magic == 0 && sdb_opt_bool(ctx, opts, "reverse", &reverse))) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    arr = w ? JS_NewArray(ctx) : JS_EXCEPTION;
    if (JS_IsException(arr)) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return arr;
    }
    if (magic >= 2) {
        x = w->sl.head->lv[0].fwd;
    } else if (reverse) {
        x = dyn_zsl_lt(&w->sl, 0, hi.p, (uint32_t)hi.n, 1, NULL);
    } else {
        x = dyn_zsl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
    }
    while (x && out < limit) {
        JSValue item;
        if (magic == 0) {
            if (reverse ? dyn_zsl_cmp(x, 0, lo.p, (uint32_t)lo.n) < 0 : dyn_zsl_cmp(x, 0, hi.p, (uint32_t)hi.n) > 0)
                break;
        } else if (magic == 1) {
            if (x->klen < lo.n || (lo.n && memcmp(x->key, lo.p, lo.n) != 0))
                break;
        }
        item = keys_only ? dyn_zsl_keystr(ctx, x) : sdb_omap_entry(ctx, x);
        if (sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return arr;
}

static JSValue sdb_omap_delete_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str lo, hi;
    dyn_zsl_node* x;
    uint32_t k = 0;
    (void)argc;
    if (ds2_str_get(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &hi, argv[1])) {
        ds2_str_release(ctx, &lo);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (w) {
        x = dyn_zsl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        while (x && dyn_zsl_cmp(x, 0, hi.p, (uint32_t)hi.n) <= 0) {
            dyn_zsl_node* nx = x->lv[0].fwd;
            dyn_zsl_drop(&w->sl, x);
            x = nx;
            k++;
        }
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return w ? JS_NewInt64(ctx, k) : JS_EXCEPTION;
}

static JSValue sdb_omap_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    int64_t i;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = dyn_zsl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_omap_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_omap_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_omap, "entries")

const JSCFunctionListEntry sdb_omap_proto[] = {
    JS_CFUNC_DEF("set", 2, sdb_omap_set),
    JS_CFUNC_MAGIC_DEF("get", 1, sdb_omap_key_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_omap_key_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_omap_key_op, 2),
    JS_CFUNC_MAGIC_DEF("floorKey", 1, sdb_omap_key_op, 3),
    JS_CFUNC_MAGIC_DEF("ceilKey", 1, sdb_omap_key_op, 4),
    JS_CFUNC_MAGIC_DEF("lowerKey", 1, sdb_omap_key_op, 5),
    JS_CFUNC_MAGIC_DEF("higherKey", 1, sdb_omap_key_op, 6),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_omap_key_op, 7),
    JS_CFUNC_MAGIC_DEF("firstKey", 0, sdb_omap_end, 0),
    JS_CFUNC_MAGIC_DEF("lastKey", 0, sdb_omap_end, 1),
    JS_CFUNC_MAGIC_DEF("rangeQuery", 2, sdb_omap_range, 0),
    JS_CFUNC_MAGIC_DEF("prefix", 1, sdb_omap_range, 1),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_omap_range, 2),
    JS_CFUNC_MAGIC_DEF("keys", 0, sdb_omap_range, 3),
    JS_CFUNC_DEF("deleteRange", 2, sdb_omap_delete_range),
    JS_CFUNC_DEF("at", 1, sdb_omap_at),
    JS_CFUNC_DEF("clear", 0, sdb_omap_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_omap_iterator),
    JS_CGETSET_DEF("size", sdb_omap_size, NULL),
};

int sdb_omap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_omap_w* o = JS_GetOpaque(obj, sdb_omap_class_id);
    dyn_zsl_node* x;
    JSValue vals;
    uint32_t i = 0;
    int rc;
    if (dyn_ser_u32(w, o->sl.len) < 0)
        return -1;
    vals = JS_NewArray(ctx);
    if (JS_IsException(vals))
        return -1;
    for (x = o->sl.head->lv[0].fwd; x; x = x->lv[0].fwd) {
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || sdb_push(ctx, vals, &i, JS_DupValue(ctx, ds_cell_val(&x->val))) < 0) {
            JS_FreeValue(ctx, vals);
            return -1;
        }
    }
    rc = dyn_codec_write_values(ctx, w, vals);
    JS_FreeValue(ctx, vals);
    return rc;
}

JSValue sdb_omap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_omap_w* o;
    JSValue vals, obj;
    const char** ks;
    size_t* kl;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 112) < 0)
        return sdb_bad(ctx);
    ks = (const char**)dyn_ds_malloc(sizeof(char*) * ((size_t)n + 1));
    kl = (size_t*)dyn_ds_malloc(sizeof(size_t) * ((size_t)n + 1));
    if (!ks || !kl) {
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        ks[i] = dyn_de_blob(r, &kl[i]);
        if (!dyn_de_ok(r)) {
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return sdb_bad(ctx);
        }
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return vals;
    }
    o = (sdb_omap_w*)dyn_ds_malloc(sizeof(*o));
    if (!o || sdb_zsl_init(&o->sl, ctx) < 0) {
        dyn_ds_free(o);
        JS_FreeValue(ctx, vals);
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v;
        if (dyn_zsl_find(&o->sl, ks[i], kl[i])) {
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return sdb_bad(ctx);
        }
        v = JS_GetPropertyUint32(ctx, vals, i);
        if (JS_IsException(v) || sdb_zsl_insert(&o->sl, 0, ks[i], kl[i], v, NULL) < 0) {
            JS_FreeValue(ctx, v);
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(ks);
    dyn_ds_free(kl);
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_omap_class_id, o, sdb_omap_free);
    return obj;
}

#endif
