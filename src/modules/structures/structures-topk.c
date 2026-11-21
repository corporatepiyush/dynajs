#include "structures-internal.h"
#include "core/ds/ds-topk.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_topk_t t;
} sdb_tk_w;

JSClassID sdb_tk_class_id;

static void sdb_tk_free(void* native)
{
    sdb_tk_w* w = (sdb_tk_w*)native;
    if (!w)
        return;
    dyn_topk_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_tk_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_tk_free(JS_GetOpaque(val, sdb_tk_class_id));
}

const JSClassDef sdb_tk_class = {
    "TopK",
    .finalizer = sdb_tk_finalizer,
};

static sdb_tk_w* sdb_tk_new(uint32_t k)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    if (dyn_topk_init(&w->t, k) < 0) {
        sdb_tk_free(w);
        return NULL;
    }
    return w;
}

JSValue sdb_tk_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    uint32_t k;
    (void)argc;
    if (dyn_size_arg(ctx, argv[0], DYN_TOPK_MAX_K, &k))
        return JS_EXCEPTION;
    if (k == 0)
        return JS_ThrowRangeError(ctx, "k must be at least 1");
    w = sdb_tk_new(k);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_tk_class_id, w, sdb_tk_free);
}

static JSValue sdb_tk_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    ds2_str k;
    double c = 1;
    char* ev;
    size_t evn = 0;
    JSValue r;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &c, argv[1]))
            return JS_EXCEPTION;
        if (!(c > 0) || !isfinite(c))
            return JS_ThrowRangeError(ctx, "count must be a positive finite number");
    }
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (dyn_topk_add(&w->t, k.p, k.n, c, &ev, &evn) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    if (!ev)
        return JS_UNDEFINED;
    r = JS_NewStringLen(ctx, ev, evn);
    dyn_ds_free(ev);
    return r;
}

static JSValue sdb_tk_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_tk_w* w;
    ds2_str k;
    dyn_ds_sent* e;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    e = dyn_ds_smap_find(&w->t.idx, k.p, k.n, dyn_ds_strhash(k.p, k.n));
    ds2_str_release(ctx, &k);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    if (magic == 2)
        return e ? JS_NewFloat64(ctx, w->t.s[e->v].err) : JS_UNDEFINED;
    return JS_NewFloat64(ctx, e ? w->t.s[e->v].count : 0);
}

static JSValue sdb_tk_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    uint32_t *ord, i, out = 0;
    JSValue arr;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    ord = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)w->t.n + 1));
    if (!ord)
        return ds2_oom(ctx);
    dyn_topk_sorted(&w->t, ord);
    arr = JS_NewArray(ctx);
    for (i = 0; i < w->t.n && !JS_IsException(arr); i++) {
        const dyn_topk_slot* s = &w->t.s[ord[i]];
        JSValue item = JS_NewArray(ctx);
        uint32_t j = 0;
        if (sdb_push(ctx, item, &j, JS_NewStringLen(ctx, s->key, s->klen)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->count)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->err)) < 0
            || sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ord);
    return arr;
}

static JSValue sdb_tk_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->t.k);
    if (magic == 1)
        return JS_NewInt64(ctx, w->t.n);
    return JS_NewFloat64(ctx, w->t.total);
}

const JSCFunctionListEntry sdb_tk_proto[] = {
    JS_CFUNC_DEF("add", 1, sdb_tk_add),
    JS_CFUNC_MAGIC_DEF("count", 1, sdb_tk_query, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_tk_query, 1),
    JS_CFUNC_MAGIC_DEF("error", 1, sdb_tk_query, 2),
    JS_CFUNC_DEF("list", 0, sdb_tk_list),
    JS_CGETSET_MAGIC_DEF("k", sdb_tk_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("size", sdb_tk_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("total", sdb_tk_getter, NULL, 2),
};

int sdb_tk_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_tk_w* t = JS_GetOpaque(obj, sdb_tk_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.k) < 0 || dyn_ser_u32(w, t->t.n) < 0 || dyn_ser_f64(w, t->t.total) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++) {
        const dyn_topk_slot* s = &t->t.s[i];
        if (dyn_ser_blob(w, s->key, s->klen) < 0 || dyn_ser_f64(w, s->count) < 0 || dyn_ser_f64(w, s->err) < 0)
            return -1;
    }
    return 0;
}

JSValue sdb_tk_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t k = dyn_de_u32(r), n, i;
    double total;
    sdb_tk_w* t;
    (void)opts;
    if (!dyn_de_ok(r) || k == 0 || k > DYN_TOPK_MAX_K || dyn_de_count(r, &n, 20) < 0 || n > k)
        return sdb_bad(ctx);
    total = dyn_de_f64(r);
    if (!dyn_de_ok(r) || !(total >= 0) || !isfinite(total) || dyn_de_charge(r, (uint64_t)k * 48) < 0)
        return sdb_bad(ctx);
    t = sdb_tk_new(k);
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* key = dyn_de_blob(r, &kn);
        double c = dyn_de_f64(r), e = dyn_de_f64(r);
        dyn_topk_slot* s = &t->t.s[i];
        uint64_t h;
        if (!dyn_de_ok(r) || !(c > 0) || !isfinite(c) || !(e >= 0) || e > c || kn > UINT32_MAX - 1) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        h = dyn_ds_strhash(key, kn);
        if (dyn_ds_smap_find(&t->t.idx, key, kn, h)) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        s->key = (char*)dyn_ds_bytes_dup(key, kn);
        if (!s->key || !dyn_ds_smap_insert(&t->t.idx, s->key, kn, h, i)) {
            dyn_ds_free(s->key);
            s->key = NULL;
            sdb_tk_free(t);
            return ds2_oom(ctx);
        }
        s->klen = (uint32_t)kn;
        s->count = c;
        s->err = e;
        t->t.heap[i] = i;
        s->hpos = i;
        t->t.n = i + 1;
        dyn_topk_up(&t->t, i);
    }
    t->t.total = total;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_tk_class_id, t, sdb_tk_free);
}

#endif
