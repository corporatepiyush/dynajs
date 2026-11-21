#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DS_MSET_EXT 0xFFFFFFFFu

typedef struct {
    const char* k;
    size_t kl;
    uint64_t c;
} ds2_mset_ent;

static void ds2_mset_free(void* native)
{
    ds2_mset_w* w = (ds2_mset_w*)native;
    if (!w)
        return;
    dyn_mset_free(w->c);
    dyn_ds_free(w);
}

JSClassID ds2_mset_class_id;

DS2_FINALIZER(ds2_mset)

const JSClassDef ds2_mset_class = {
    "Multiset",
    .finalizer = ds2_mset_finalizer,
};

JSValue ds2_mset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mset_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mset_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mset_class_id, w, ds2_mset_free);
}

static JSValue ds2_mset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_mset_w* w;
    ds2_str k;
    int64_t n = 1;
    uint64_t out = 0;

    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx, "count must be >= 0");
    }
    if (magic)
        n = -n;
    if (dyn_mset_add(w->c, k.p, k.n, n, &out) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, out);
}

static JSValue ds2_mset_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_mset_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, c);
}

static JSValue ds2_mset_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_mset_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, c > 0);
}

static JSValue ds2_mset_set_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    int64_t n;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setCount(key, count): count is required");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx, "count must be >= 0");
    }
    if (dyn_mset_set_count(w->c, k.p, k.n, (uint64_t)n) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mset_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    int had;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    had = dyn_mset_count(w->c, k.p, k.n) > 0;
    if (had)
        dyn_mset_set_count(w->c, k.p, k.n, 0);
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, had);
}

static JSValue ds2_mset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_mset_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_mset_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int entries)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_mset_distinct(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char* k;
        size_t kl;
        uint64_t c;
        JSValue item;
        dyn_mset_at(w->c, i, &k, &kl, &c);
        if (entries) {
            item = JS_NewArray(ctx);
            if (JS_IsException(item))
                goto fail;
            if (JS_DefinePropertyValueUint32(ctx, item, 0,
                    ds2_new_str(ctx, k, kl),
                    JS_PROP_C_W_E)
                    < 0
                || JS_DefinePropertyValueUint32(ctx, item, 1,
                       ds2_u64_js(ctx, c),
                       JS_PROP_C_W_E)
                    < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
        } else {
            item = ds2_new_str(ctx, k, kl);
            if (JS_IsException(item))
                goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, item,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_mset_total(JSContext* ctx, JSValueConst this_val)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return ds2_u64_js(ctx, dyn_mset_total(w->c));
}

static JSValue ds2_mset_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mset_distinct(w->c));
}

DS2_ITERATOR(ds2_mset, "entrySet")

const JSCFunctionListEntry ds2_mset_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_mset_iterator),
    JS_CFUNC_MAGIC_DEF("add", 1, ds2_mset_add, 0),
    JS_CFUNC_MAGIC_DEF("remove", 1, ds2_mset_add, 1),
    JS_CFUNC_DEF("count", 1, ds2_mset_count),
    JS_CFUNC_DEF("has", 1, ds2_mset_has),
    JS_CFUNC_DEF("setCount", 2, ds2_mset_set_count),
    JS_CFUNC_DEF("delete", 1, ds2_mset_delete),
    JS_CFUNC_DEF("clear", 0, ds2_mset_clear),
    JS_CFUNC_MAGIC_DEF("elementSet", 0, ds2_mset_set, 0),
    JS_CFUNC_MAGIC_DEF("entrySet", 0, ds2_mset_set, 1),
    JS_CGETSET_DEF("size", ds2_mset_size, NULL),
    JS_CGETSET_DEF("totalSize", ds2_mset_total, NULL),
};

static int ds2_mset_cmp(const ds2_mset_ent* x, const ds2_mset_ent* y)
{
    size_t n = x->kl < y->kl ? x->kl : y->kl;
    int c = n ? memcmp(x->k, y->k, n) : 0;
    if (c)
        return c;
    return x->kl < y->kl ? -1 : (x->kl > y->kl ? 1 : 0);
}

static void ds2_mset_sort(ds2_mset_ent* v, ds2_mset_ent* tmp, size_t n)
{
    size_t mid, i, j, k;
    if (n < 2)
        return;
    mid = n / 2;
    ds2_mset_sort(v, tmp, mid);
    ds2_mset_sort(v + mid, tmp, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n)
        tmp[k++] = ds2_mset_cmp(&v[i], &v[j]) <= 0 ? v[i++] : v[j++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(*v));
}

int ds2_mset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mset_w* o = (ds2_mset_w*)JS_GetOpaque(obj, ds2_mset_class_id);
    uint32_t i, n = dyn_mset_distinct(o->c);
    ds2_mset_ent *ents = NULL, *tmp = NULL;
    const char* prev = NULL;
    size_t prev_len = 0;
    int rc = -1;
    (void)ctx;

    if (dyn_ser_u32(w, DS_MSET_EXT) < 0 || dyn_ser_u32(w, n) < 0)
        return -1;
    if (!n)
        return 0;
    ents = (ds2_mset_ent*)dyn_ds_malloc(sizeof(*ents) * n);
    tmp = (ds2_mset_ent*)dyn_ds_malloc(sizeof(*tmp) * n);
    if (!ents || !tmp)
        goto done;
    for (i = 0; i < n; i++)
        dyn_mset_at(o->c, i, &ents[i].k, &ents[i].kl, &ents[i].c);
    ds2_mset_sort(ents, tmp, n);
    for (i = 0; i < n; i++) {
        size_t sh = 0, lim = ents[i].kl < prev_len ? ents[i].kl : prev_len;
        while (sh < lim && ents[i].k[sh] == prev[sh])
            sh++;
        if (dyn_sk_uvarint(w, sh) < 0 || dyn_sk_uvarint(w, ents[i].kl - sh) < 0 || dyn_ser_raw(w, ents[i].k + sh, ents[i].kl - sh) < 0 || dyn_sk_uvarint(w, ents[i].c) < 0)
            goto done;
        prev = ents[i].k;
        prev_len = ents[i].kl;
    }
    rc = 0;
done:
    dyn_ds_free(ents);
    dyn_ds_free(tmp);
    return rc;
}

JSValue ds2_mset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mset_t* m;
    uint32_t n, i, first;
    uint8_t* cur = NULL;
    size_t curcap = 0, cur_len = 0;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first != DS_MSET_EXT) {
        n = first;
        if ((uint64_t)n * 12 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        m = dyn_mset_new();
        if (!m)
            return ds2_oom(ctx);
        for (i = 0; i < n; i++) {
            size_t kl;
            const char* k = dyn_de_blob(r, &kl);
            uint64_t c = dyn_de_u64(r);
            if (!dyn_de_ok(r) || dyn_mset_set_count(m, k, kl, c) < 0) {
                dyn_mset_free(m);
                return dyn_de_ok(r) ? ds2_oom(ctx)
                                    : dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
        }
        DS2_WRAP(ds2_mset, dyn_mset_free(m), m);
    }
    if (dyn_de_count(r, &n, 3) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    m = dyn_mset_new();
    if (!m)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        uint64_t sh, sl, c;
        const uint8_t* p;
        if (dyn_sk_read_uvarint(r, &sh) < 0 || dyn_sk_read_uvarint(r, &sl) < 0)
            goto bad;
        if (sh > cur_len || sl > (uint64_t)dyn_de_left(r))
            goto bad;
        if (dsw_trie_grow((void**)&cur, &curcap, (size_t)(sh + sl), 1) < 0)
            goto oom;
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto bad;
        memcpy(cur + sh, p, (size_t)sl);
        cur_len = (size_t)(sh + sl);
        if (dyn_sk_read_uvarint(r, &c) < 0)
            goto bad;
        if (dyn_mset_set_count(m, (const char*)cur, cur_len, c) < 0)
            goto oom;
    }
    dyn_ds_free(cur);
    DS2_WRAP(ds2_mset, dyn_mset_free(m), m);
bad:
    dyn_ds_free(cur);
    dyn_mset_free(m);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
oom:
    dyn_ds_free(cur);
    dyn_mset_free(m);
    return ds2_oom(ctx);
}

#endif
