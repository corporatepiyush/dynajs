#include "structures-internal.h"
#include "core/ds/ds-interner.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_interner_t t;
} sdb_in_w;

JSClassID sdb_in_class_id;

static void sdb_in_free(void* native)
{
    sdb_in_w* w = (sdb_in_w*)native;
    if (!w)
        return;
    dyn_interner_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_in_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_in_free(JS_GetOpaque(val, sdb_in_class_id));
}

const JSClassDef sdb_in_class = {
    "Interner",
    .finalizer = sdb_in_finalizer,
};

JSValue sdb_in_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_in_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_in_class_id, w, sdb_in_free);
}

static JSValue sdb_in_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_in_w* w;
    ds2_str k;
    int64_t id;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    id = dyn_interner_intern(&w->t, k.p, k.n, magic == 0);
    ds2_str_release(ctx, &k);
    if (id == -2)
        return ds2_oom(ctx);
    if (magic == 2)
        return JS_NewBool(ctx, id >= 0);
    return id >= 0 ? JS_NewInt64(ctx, id) : JS_UNDEFINED;
}

static JSValue sdb_in_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    double d;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= (double)w->t.n)
        return JS_UNDEFINED;
    return JS_NewStringLen(ctx, w->t.s[(uint32_t)d].p, w->t.s[(uint32_t)d].len);
}

static JSValue sdb_in_intern_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    JSValue lv;
    uint32_t n, i;
    uint32_t* ids;
    JSValue out;
    (void)argc;
    if (JS_IsArray(ctx, argv[0]) <= 0)
        return JS_ThrowTypeError(ctx, "internMany: expected an array of strings");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n > DYN_DS_MAX_ITEMS)
        return JS_ThrowRangeError(ctx, "internMany: more than %u strings", DYN_DS_MAX_ITEMS);
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    ids = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)n + 1));
    if (!ids)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        JSValue* fast;
        uint32_t cnt;
        JSValue e;
        ds2_str k;
        int64_t id;
        if (JS_GetFastArray(ctx, argv[0], &fast, &cnt) && i < cnt)
            e = JS_DupValue(ctx, fast[i]);
        else
            e = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(e)) {
            dyn_ds_free(ids);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            dyn_ds_free(ids);
            return JS_ThrowTypeError(ctx, "internMany: element %u is not a string", (unsigned)i);
        }
        if (ds2_str_get(ctx, &k, e)) {
            JS_FreeValue(ctx, e);
            dyn_ds_free(ids);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
        id = w ? dyn_interner_intern(&w->t, k.p, k.n, 1) : -3;
        ds2_str_release(ctx, &k);
        if (id < 0) {
            dyn_ds_free(ids);
            return id == -3 ? JS_EXCEPTION : ds2_oom(ctx);
        }
        ids[i] = (uint32_t)id;
    }
    out = sdb_typed_out(ctx, ids, n, JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(ids);
    return out;
}

static JSValue sdb_in_get_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    uint32_t *ids, n, i, out = 0;
    JSValue arr;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "getMany", &ids, &n))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        dyn_ds_free(ids);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        JSValue v = ids[i] < w->t.n ? JS_NewStringLen(ctx, w->t.s[ids[i]].p, w->t.s[ids[i]].len) : JS_UNDEFINED;
        if (sdb_push(ctx, arr, &out, v) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ids);
    return arr;
}

static JSValue sdb_in_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_in_w* w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    return magic ? JS_NewInt64(ctx, (int64_t)w->t.bytes) : JS_NewInt64(ctx, w->t.n);
}

const JSCFunctionListEntry sdb_in_proto[] = {
    JS_CFUNC_MAGIC_DEF("intern", 1, sdb_in_op, 0),
    JS_CFUNC_MAGIC_DEF("lookup", 1, sdb_in_op, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_in_op, 2),
    JS_CFUNC_DEF("get", 1, sdb_in_get),
    JS_CFUNC_DEF("internMany", 1, sdb_in_intern_many),
    JS_CFUNC_DEF("getMany", 1, sdb_in_get_many),
    JS_CGETSET_MAGIC_DEF("size", sdb_in_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_in_getter, NULL, 1),
};

int sdb_in_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_in_w* t = JS_GetOpaque(obj, sdb_in_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.n) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++)
        if (dyn_ser_blob(w, t->t.s[i].p, t->t.s[i].len) < 0)
            return -1;
    return 0;
}

JSValue sdb_in_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_in_w* t;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0 || dyn_de_charge(r, (uint64_t)n * 48) < 0)
        return sdb_bad(ctx);
    t = (sdb_in_w*)dyn_ds_calloc(1, sizeof(*t));
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        int64_t id;
        if (!dyn_de_ok(r) || dyn_interner_intern(&t->t, k, kn, 0) >= 0) {
            sdb_in_free(t);
            return sdb_bad(ctx);
        }
        id = dyn_interner_intern(&t->t, k, kn, 1);
        if (id < 0) {
            sdb_in_free(t);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_in_class_id, t, sdb_in_free);
}

#endif
