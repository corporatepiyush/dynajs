#include <arpa/inet.h>

#define DSX_MAX_NODES (1u << 26)

static int dsx_to_f64_free(JSContext* ctx, double* out, JSValue v)
{
    int r = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue dsx_typed_copy(JSContext* ctx, const void* data, size_t count, size_t esz, JSTypedArrayEnum type)
{
    JSValue buf, args[3], out;
    buf = JS_NewArrayBufferCopy(ctx, (const uint8_t*)data, count * esz);
    if (JS_IsException(buf))
        return buf;
    args[0] = buf;
    args[1] = JS_NewInt32(ctx, 0);
    args[2] = JS_NewInt64(ctx, (int64_t)count);
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, buf);
    return out;
}

static void dsx_val_copy(void* dst, const void* src, void* ud)
{
    JSValue v;
    memcpy(&v, src, sizeof(v));
    v = JS_DupValueRT((JSRuntime*)ud, v);
    memcpy(dst, &v, sizeof(v));
}

static void dsx_val_release(void* e, void* ud)
{
    JSValue v;
    memcpy(&v, e, sizeof(v));
    JS_FreeValueRT((JSRuntime*)ud, v);
}

static JSValue dsx_val_get(const void* e)
{
    JSValue v;
    memcpy(&v, e, sizeof(v));
    return v;
}

static dyn_ds_ops dsx_val_ops(JSRuntime* rt)
{
    dyn_ds_ops o;
    memset(&o, 0, sizeof(o));
    o.size = sizeof(JSValue);
    o.copy = dsx_val_copy;
    o.release = dsx_val_release;
    o.ud = rt;
    return o;
}

static void dsx_mark_value(JSRuntime* rt, const void* e, JS_MarkFunc* mark_func)
{
    JS_MarkValue(rt, dsx_val_get(e), mark_func);
}

static int dsx_index_arg(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    if (JS_IsBigInt(ctx, v)) {
        int64_t x;
        if (JS_ToBigInt64(ctx, &x, v))
            return -1;
        {
            JSValue back = JS_NewBigUint64(ctx, (uint64_t)x);
            int same;
            if (JS_IsException(back))
                return -1;
            same = JS_StrictEq(ctx, back, v);
            JS_FreeValue(ctx, back);
            if (!same) {
                JS_ThrowRangeError(ctx, "index must be a non-negative integer below 2^64");
                return -1;
            }
        }
        *out = (uint64_t)x;
        return 0;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 0) || d > 9007199254740991.0 || d != (double)(uint64_t)d) {
            JS_ThrowRangeError(ctx, "index must be a non-negative safe integer (or a BigInt below 2^64)");
            return -1;
        }
        *out = (uint64_t)d;
        return 0;
    }
}

static JSValue dsx_index_value(JSContext* ctx, uint64_t i)
{
    if (i <= 9007199254740991ULL)
        return JS_NewInt64(ctx, (int64_t)i);
    return JS_NewBigUint64(ctx, i);
}

typedef struct {
    JSRuntime* rt;
    JSContext* ctx;
    dyn_swiss_t* m;
} dsx_swiss_t;

static uint64_t dsx_key_hash(const void* e, void* ud)
{
    return JS_HashValueRT(((dsx_swiss_t*)ud)->rt, dsx_val_get(e));
}

static int dsx_key_eq(const void* a, const void* b, void* ud)
{
    return JS_SameValueZero(((dsx_swiss_t*)ud)->ctx, dsx_val_get(a), dsx_val_get(b));
}

static void dsx_key_copy(void* dst, const void* src, void* ud)
{
    dsx_val_copy(dst, src, ((dsx_swiss_t*)ud)->rt);
}

static void dsx_key_release(void* e, void* ud)
{
    dsx_val_release(e, ((dsx_swiss_t*)ud)->rt);
}

static void dsx_swiss_free(void* native)
{
    dsx_swiss_t* s = (dsx_swiss_t*)native;
    if (!s)
        return;
    dyn_swiss_free(s->m);
    dyn_ds_free(s);
}

static JSClassID dsx_swiss_class_id;
DYN_FINALIZER(dsx_swiss)

static void dsx_swiss_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_swiss_t* s = JS_GetOpaque(val, dsx_swiss_class_id);
    size_t it = 0;
    const void* k;
    void* v;
    if (!s)
        return;
    while ((it = dyn_swiss_next(s->m, it, &k, &v)) != 0) {
        dsx_mark_value(rt, k, mark_func);
        dsx_mark_value(rt, v, mark_func);
    }
}

static const JSClassDef dsx_swiss_class = {
    "SwissMap",
    .finalizer = dsx_swiss_finalizer,
    .gc_mark = dsx_swiss_gc_mark,
};

static dsx_swiss_t* dsx_swiss_get(JSContext* ctx, JSValueConst this_val)
{
    dsx_swiss_t* s = dyn_plain_get(ctx, this_val, dsx_swiss_class_id);
    if (s)
        s->ctx = ctx;
    return s;
}

static JSValueConst dsx_norm_key(JSValueConst k)
{
    if (JS_VALUE_GET_TAG(k) == JS_TAG_FLOAT64 && JS_VALUE_GET_FLOAT64(k) == 0.0)
        return JS_MKVAL(JS_TAG_INT, 0);
    return k;
}

static JSValue dsx_swiss_set_impl(JSContext* ctx, dsx_swiss_t* s, JSValueConst key, JSValueConst val)
{
    JSValueConst k = dsx_norm_key(key);
    if (dyn_swiss_put(s->m, &k, &val) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue dsx_swiss_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = (dsx_swiss_t*)dyn_ds_malloc(sizeof(*s));
    dyn_ds_ops ko, vo;
    JSValue obj;
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->rt = JS_GetRuntime(ctx);
    s->ctx = ctx;
    memset(&ko, 0, sizeof(ko));
    ko.size = sizeof(JSValue);
    ko.hash = dsx_key_hash;
    ko.eq = dsx_key_eq;
    ko.copy = dsx_key_copy;
    ko.release = dsx_key_release;
    ko.ud = s;
    vo = dsx_val_ops(s->rt);
    s->m = dyn_swiss_new(&ko, &vo);
    if (!s->m) {
        dyn_ds_free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    obj = dyn_plain_wrap(ctx, new_target, dsx_swiss_class_id, s, dsx_swiss_free);
    if (JS_IsException(obj))
        return obj;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        uint32_t n, i;
        JSValue len;
        if (!JS_IsArray(ctx, argv[0])) {
            JS_ThrowTypeError(ctx, "SwissMap: entries must be an array of [key, value] pairs");
            goto fail;
        }
        len = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToUint32(ctx, &n, len)) {
            JS_FreeValue(ctx, len);
            goto fail;
        }
        JS_FreeValue(ctx, len);
        for (i = 0; i < n; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], i), k, v, r;
            if (JS_IsException(item))
                goto fail;
            if (!JS_IsObject(item)) {
                JS_FreeValue(ctx, item);
                JS_ThrowTypeError(ctx, "SwissMap: entry %u is not a [key, value] pair", i);
                goto fail;
            }
            k = JS_GetPropertyUint32(ctx, item, 0);
            v = JS_GetPropertyUint32(ctx, item, 1);
            JS_FreeValue(ctx, item);
            if (JS_IsException(k) || JS_IsException(v)) {
                JS_FreeValue(ctx, k);
                JS_FreeValue(ctx, v);
                goto fail;
            }
            s = dsx_swiss_get(ctx, obj);
            r = s ? dsx_swiss_set_impl(ctx, s, k, v) : JS_EXCEPTION;
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            if (JS_IsException(r))
                goto fail;
        }
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue dsx_swiss_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    JSValue r;
    if (!s)
        return JS_EXCEPTION;
    r = dsx_swiss_set_impl(ctx, s, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(r))
        return r;
    return JS_DupValue(ctx, this_val);
}

static JSValue dsx_swiss_get_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    JSValueConst k;
    void* v;
    (void)argc;
    if (!s)
        return JS_EXCEPTION;
    k = dsx_norm_key(argv[0]);
    v = dyn_swiss_get(s->m, &k);
    return v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
}

static JSValue dsx_swiss_has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    JSValueConst k;
    (void)argc;
    if (!s)
        return JS_EXCEPTION;
    k = dsx_norm_key(argv[0]);
    return JS_NewBool(ctx, dyn_swiss_get(s->m, &k) != NULL);
}

static JSValue dsx_swiss_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    JSValueConst k;
    (void)argc;
    if (!s)
        return JS_EXCEPTION;
    k = dsx_norm_key(argv[0]);
    return JS_NewBool(ctx, dyn_swiss_remove(s->m, &k));
}

static JSValue dsx_swiss_clear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_swiss_clear(s->m);
    return JS_UNDEFINED;
}

static JSValue dsx_swiss_size(JSContext* ctx, JSValueConst this_val)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_swiss_size(s->m));
}

static JSValue dsx_swiss_reserve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double d;
    dsx_swiss_t* s;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    if (!(d >= 0) || d > (double)DSX_MAX_NODES || d != (double)(uint64_t)d)
        return JS_ThrowRangeError(ctx, "SwissMap.reserve: count must be an integer in 0..%u", DSX_MAX_NODES);
    s = dsx_swiss_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (dyn_swiss_reserve(s->m, (size_t)d) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsx_swiss_snapshot(JSContext* ctx, JSValueConst this_val, int magic)
{
    dsx_swiss_t* s = dsx_swiss_get(ctx, this_val);
    JSValue arr;
    size_t it = 0;
    uint32_t i = 0;
    const void* k;
    void* v;
    if (!s)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    while ((it = dyn_swiss_next(s->m, it, &k, &v)) != 0) {
        JSValue e;
        if (magic == 0) {
            e = JS_DupValue(ctx, dsx_val_get(k));
        } else if (magic == 1) {
            e = JS_DupValue(ctx, dsx_val_get(v));
        } else {
            JSValue pair[2];
            pair[0] = JS_DupValue(ctx, dsx_val_get(k));
            pair[1] = JS_DupValue(ctx, dsx_val_get(v));
            e = JS_NewArrayFrom(ctx, 2, pair);
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, arr);
                return e;
            }
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i++, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dsx_swiss_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dsx_swiss_snapshot(ctx, this_val, 0);
}

static JSValue dsx_swiss_values(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dsx_swiss_snapshot(ctx, this_val, 1);
}

static JSValue dsx_swiss_entries(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dsx_swiss_snapshot(ctx, this_val, 2);
}

static JSValue dsx_swiss_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_ds_iter_via(ctx, this_val, "entries", argc, argv);
}

static JSValue dsx_swiss_foreach(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue snap;
    uint32_t i, n;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "SwissMap.forEach: the callback must be a function");
    snap = dsx_swiss_snapshot(ctx, this_val, 2);
    if (JS_IsException(snap))
        return snap;
    {
        JSValue len = JS_GetPropertyStr(ctx, snap, "length");
        if (JS_ToUint32(ctx, &n, len)) {
            JS_FreeValue(ctx, len);
            JS_FreeValue(ctx, snap);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, len);
    }
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, snap, i), args[3], r;
        args[1] = JS_GetPropertyUint32(ctx, pair, 0);
        args[0] = JS_GetPropertyUint32(ctx, pair, 1);
        args[2] = (JSValue)this_val;
        JS_FreeValue(ctx, pair);
        r = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, (JSValueConst*)args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, snap);
            return r;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, snap);
    return JS_UNDEFINED;
}

static JSValue dsx_swiss_group_impl(JSContext* ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, dyn_swiss_group_impl());
}

static const JSCFunctionListEntry dsx_swiss_proto[] = {
    JS_CFUNC_DEF("set", 2, dsx_swiss_set),
    JS_CFUNC_DEF("get", 1, dsx_swiss_get_m),
    JS_CFUNC_DEF("has", 1, dsx_swiss_has),
    JS_CFUNC_DEF("delete", 1, dsx_swiss_delete),
    JS_CFUNC_DEF("clear", 0, dsx_swiss_clear),
    JS_CFUNC_DEF("reserve", 1, dsx_swiss_reserve),
    JS_CFUNC_DEF("keys", 0, dsx_swiss_keys),
    JS_CFUNC_DEF("values", 0, dsx_swiss_values),
    JS_CFUNC_DEF("entries", 0, dsx_swiss_entries),
    JS_CFUNC_DEF("forEach", 1, dsx_swiss_foreach),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dsx_swiss_iterator),
    JS_CGETSET_DEF("size", dsx_swiss_size, NULL),
    JS_CGETSET_DEF("groupImpl", dsx_swiss_group_impl, NULL),
};

enum { DSX_TREE_RB,
    DSX_TREE_WB,
    DSX_TREE_SL };

typedef struct {
    char* p;
    size_t n;
} dsx_skey;

typedef struct {
    int kind;
    int strings;
    JSRuntime* rt;
    void* t;
} dsx_tree_t;

static int dsx_num_cmp(const void* a, const void* b, void* ud)
{
    double x, y;
    (void)ud;
    memcpy(&x, a, 8);
    memcpy(&y, b, 8);
    return x < y ? -1 : x > y;
}

static int dsx_str_cmp(const void* a, const void* b, void* ud)
{
    const dsx_skey *x = (const dsx_skey*)a, *y = (const dsx_skey*)b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = n ? memcmp(x->p, y->p, n) : 0;
    (void)ud;
    if (c)
        return c;
    return x->n < y->n ? -1 : x->n > y->n;
}

static void dsx_str_release(void* e, void* ud)
{
    dsx_skey k;
    (void)ud;
    memcpy(&k, e, sizeof(k));
    dyn_ds_free(k.p);
}

static JSClassID dsx_rbtree_class_id, dsx_wbtree_class_id, dsx_skiplist_class_id;

static void dsx_tree_free(void* native)
{
    dsx_tree_t* t = (dsx_tree_t*)native;
    if (!t)
        return;
    if (t->kind == DSX_TREE_RB)
        dyn_rbtree_free(t->t);
    else if (t->kind == DSX_TREE_WB)
        dyn_wbtree_free(t->t);
    else
        dyn_skiplist_free(t->t);
    dyn_ds_free(t);
}

static void dsx_tree_finalizer(JSRuntime* rt, JSValue val)
{
    JSClassID ids[3] = { dsx_rbtree_class_id, dsx_wbtree_class_id, dsx_skiplist_class_id };
    int i;
    (void)rt;
    for (i = 0; i < 3; i++) {
        void* p = JS_GetOpaque(val, ids[i]);
        if (p) {
            dsx_tree_free(p);
            return;
        }
    }
}

typedef struct {
    JSRuntime* rt;
    JS_MarkFunc* mark_func;
} dsx_mark_ctx;

static int dsx_wb_mark_visit(const void* key, void* val, void* ud)
{
    dsx_mark_ctx* m = (dsx_mark_ctx*)ud;
    (void)key;
    dsx_mark_value(m->rt, val, m->mark_func);
    return 0;
}

static dsx_tree_t* dsx_tree_opaque(JSValueConst val)
{
    JSClassID ids[3] = { dsx_rbtree_class_id, dsx_wbtree_class_id, dsx_skiplist_class_id };
    int i;
    for (i = 0; i < 3; i++) {
        void* p = JS_GetOpaque(val, ids[i]);
        if (p)
            return (dsx_tree_t*)p;
    }
    return NULL;
}

static void dsx_tree_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_tree_t* t = dsx_tree_opaque(val);
    if (!t)
        return;
    if (t->kind == DSX_TREE_RB) {
        dyn_rbnode* n;
        for (n = dyn_rbtree_first(t->t); n; n = dyn_rbtree_next(n))
            dsx_mark_value(rt, dyn_rbtree_val(t->t, n), mark_func);
    } else if (t->kind == DSX_TREE_WB) {
        dsx_mark_ctx m = { rt, mark_func };
        dyn_wbtree_walk(t->t, dsx_wb_mark_visit, &m);
    } else {
        dyn_slnode* n;
        for (n = dyn_skiplist_first(t->t); n; n = dyn_skiplist_next(n))
            dsx_mark_value(rt, dyn_skiplist_val(t->t, n), mark_func);
    }
}

static const JSClassDef dsx_rbtree_class = { "RBTree", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };
static const JSClassDef dsx_wbtree_class = { "WBTree", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };
static const JSClassDef dsx_skiplist_class = { "SkipList", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };

static JSValue dsx_tree_ctor_kind(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int kind)
{
    dsx_tree_t* t;
    dyn_ds_ops ko, vo;
    int strings = 0;
    JSClassID cid = kind == DSX_TREE_RB ? dsx_rbtree_class_id : kind == DSX_TREE_WB ? dsx_wbtree_class_id
                                                                                    : dsx_skiplist_class_id;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue k = JS_GetPropertyStr(ctx, argv[0], "keys");
        if (JS_IsException(k))
            return k;
        if (!JS_IsUndefined(k)) {
            const char* s = JS_ToCString(ctx, k);
            JS_FreeValue(ctx, k);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "string"))
                strings = 1;
            else if (strcmp(s, "number")) {
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx, "keys must be \"number\" or \"string\"");
            }
            JS_FreeCString(ctx, s);
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "the options must be an object");
    }
    t = (dsx_tree_t*)dyn_ds_malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    t->kind = kind;
    t->strings = strings;
    t->rt = JS_GetRuntime(ctx);
    memset(&ko, 0, sizeof(ko));
    if (strings) {
        ko.size = sizeof(dsx_skey);
        ko.cmp = dsx_str_cmp;
        ko.release = dsx_str_release;
    } else {
        ko.size = sizeof(double);
        ko.cmp = dsx_num_cmp;
    }
    vo = dsx_val_ops(t->rt);
    if (kind == DSX_TREE_RB)
        t->t = dyn_rbtree_new(&ko, &vo);
    else if (kind == DSX_TREE_WB)
        t->t = dyn_wbtree_new(&ko, &vo);
    else
        t->t = dyn_skiplist_new(&ko, &vo, (uint64_t)(uintptr_t)t ^ 0x9E3779B97F4A7C15ULL);
    if (!t->t) {
        dyn_ds_free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, cid, t, dsx_tree_free);
}

static JSValue dsx_rbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_RB);
}

static JSValue dsx_wbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_WB);
}

static JSValue dsx_skiplist_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_SL);
}

typedef union {
    double num;
    dsx_skey str;
} dsx_keybuf;

static int dsx_tree_key(JSContext* ctx, int strings, JSValueConst v, dsx_keybuf* kb, const char** owned, int nan_absent)
{
    *owned = NULL;
    if (strings) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        kb->str.p = (char*)(uintptr_t)s;
        kb->str.n = n;
        *owned = s;
        return 0;
    }
    if (JS_ToFloat64(ctx, &kb->num, v))
        return -1;
    if (kb->num != kb->num) {
        if (nan_absent)
            return 1;
        JS_ThrowRangeError(ctx, "a NaN key is not allowed");
        return -1;
    }
    if (kb->num == 0)
        kb->num = 0;
    return 0;
}

static JSValue dsx_tree_key_value(JSContext* ctx, int strings, const void* k)
{
    if (strings) {
        const dsx_skey* s = (const dsx_skey*)k;
        return JS_NewStringLen(ctx, s->p, s->n);
    } else {
        double d;
        memcpy(&d, k, 8);
        return JS_NewFloat64(ctx, d);
    }
}

static dsx_tree_t* dsx_tree_get(JSContext* ctx, JSValueConst this_val)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    if (!t)
        JS_ThrowTypeError(ctx, "not an RBTree, WBTree or SkipList");
    return t;
}

static size_t dsx_tree_count(dsx_tree_t* t)
{
    return t->kind == DSX_TREE_RB ? dyn_rbtree_size(t->t) : t->kind == DSX_TREE_WB ? dyn_wbtree_size(t->t)
                                                                                   : dyn_skiplist_size(t->t);
}

static JSValue dsx_tree_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    dsx_keybuf kb;
    const char* owned;
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
    int r;
    if (!t)
        return dsx_tree_get(ctx, this_val), JS_EXCEPTION;
    if (dsx_tree_key(ctx, t->strings, argv[0], &kb, &owned, 0))
        return JS_EXCEPTION;
    t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (t->strings) {
        char* adopt = (char*)dyn_ds_malloc(kb.str.n ? kb.str.n : 1);
        size_t before = dsx_tree_count(t);
        if (!adopt) {
            JS_FreeCString(ctx, owned);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(adopt, kb.str.p, kb.str.n);
        JS_FreeCString(ctx, owned);
        kb.str.p = adopt;
        r = t->kind == DSX_TREE_RB ? dyn_rbtree_put(t->t, &kb, &v) : t->kind == DSX_TREE_WB ? dyn_wbtree_put(t->t, &kb, &v)
                                                                                            : dyn_skiplist_put(t->t, &kb, &v);
        if (r < 0 || dsx_tree_count(t) == before)
            dyn_ds_free(adopt);
    } else {
        r = t->kind == DSX_TREE_RB ? dyn_rbtree_put(t->t, &kb, &v) : t->kind == DSX_TREE_WB ? dyn_wbtree_put(t->t, &kb, &v)
                                                                                            : dyn_skiplist_put(t->t, &kb, &v);
    }
    if (r < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static void* dsx_tree_lookup(dsx_tree_t* t, const dsx_keybuf* kb)
{
    return t->kind == DSX_TREE_RB ? dyn_rbtree_get(t->t, kb) : t->kind == DSX_TREE_WB ? dyn_wbtree_get(t->t, kb)
                                                                                      : dyn_skiplist_get(t->t, kb);
}

static JSValue dsx_tree_get_has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    dsx_keybuf kb;
    const char* owned;
    void* v;
    (void)argc;
    if (!t)
        return dsx_tree_get(ctx, this_val), JS_EXCEPTION;
    {
        int kr = dsx_tree_key(ctx, t->strings, argv[0], &kb, &owned, 1);
        if (kr < 0)
            return JS_EXCEPTION;
        if (kr > 0)
            return magic ? JS_FALSE : JS_UNDEFINED;
    }
    t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    v = dsx_tree_lookup(t, &kb);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (magic)
        return JS_NewBool(ctx, v != NULL);
    return v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
}

static JSValue dsx_tree_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    dsx_keybuf kb;
    const char* owned;
    int r;
    (void)argc;
    if (!t)
        return dsx_tree_get(ctx, this_val), JS_EXCEPTION;
    {
        int kr = dsx_tree_key(ctx, t->strings, argv[0], &kb, &owned, 1);
        if (kr < 0)
            return JS_EXCEPTION;
        if (kr > 0)
            return JS_FALSE;
    }
    t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    r = t->kind == DSX_TREE_RB ? dyn_rbtree_remove(t->t, &kb) : t->kind == DSX_TREE_WB ? dyn_wbtree_remove(t->t, &kb)
                                                                                       : dyn_skiplist_remove(t->t, &kb);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewBool(ctx, r);
}

static JSValue dsx_tree_size(JSContext* ctx, JSValueConst this_val)
{
    dsx_tree_t* t = dsx_tree_get(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dsx_tree_count(t));
}

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int strings, mode, failed;
} dsx_collect;

static int dsx_collect_push(dsx_collect* c, const void* key, const void* val)
{
    JSValue e;
    if (c->mode == 0) {
        e = dsx_tree_key_value(c->ctx, c->strings, key);
    } else if (c->mode == 1) {
        e = JS_DupValue(c->ctx, dsx_val_get(val));
    } else {
        JSValue pair[2];
        pair[0] = dsx_tree_key_value(c->ctx, c->strings, key);
        pair[1] = JS_DupValue(c->ctx, dsx_val_get(val));
        e = JS_NewArrayFrom(c->ctx, 2, pair);
    }
    if (JS_IsException(e) || JS_DefinePropertyValueUint32(c->ctx, c->arr, c->i++, e, JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    return 0;
}

static int dsx_wb_collect_visit(const void* key, void* val, void* ud)
{
    return dsx_collect_push((dsx_collect*)ud, key, val);
}

static JSValue dsx_tree_collect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int mode)
{
    dsx_tree_t* t = dsx_tree_get(ctx, this_val);
    dsx_collect c;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    c.ctx = ctx;
    c.arr = JS_NewArray(ctx);
    c.i = 0;
    c.strings = t->strings;
    c.mode = mode;
    c.failed = 0;
    if (JS_IsException(c.arr))
        return c.arr;
    if (t->kind == DSX_TREE_RB) {
        dyn_rbnode* n;
        for (n = dyn_rbtree_first(t->t); n && !c.failed; n = dyn_rbtree_next(n))
            dsx_collect_push(&c, dyn_rbtree_key(t->t, n), dyn_rbtree_val(t->t, n));
    } else if (t->kind == DSX_TREE_WB) {
        dyn_wbtree_walk(t->t, dsx_wb_collect_visit, &c);
    } else {
        dyn_slnode* n;
        for (n = dyn_skiplist_first(t->t); n && !c.failed; n = dyn_skiplist_next(n))
            dsx_collect_push(&c, dyn_skiplist_key(t->t, n), dyn_skiplist_val(t->t, n));
    }
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

static JSValue dsx_tree_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dsx_tree_collect(ctx, this_val, argc, argv, 0);
}

static JSValue dsx_tree_values(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dsx_tree_collect(ctx, this_val, argc, argv, 1);
}

static JSValue dsx_tree_entries(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dsx_tree_collect(ctx, this_val, argc, argv, 2);
}

static JSValue dsx_tree_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_ds_iter_via(ctx, this_val, "entries", argc, argv);
}

static JSValue dsx_tree_edge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int last)
{
    dsx_tree_t* t = dsx_tree_get(ctx, this_val);
    const void* k = NULL;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    if (t->kind == DSX_TREE_RB) {
        dyn_rbnode* n = last ? dyn_rbtree_last(t->t) : dyn_rbtree_first(t->t);
        if (n)
            k = dyn_rbtree_key(t->t, n);
    } else if (t->kind == DSX_TREE_WB) {
        size_t n = dyn_wbtree_size(t->t);
        if (n)
            dyn_wbtree_select(t->t, last ? n - 1 : 0, &k, NULL);
    } else {
        size_t n = dyn_skiplist_size(t->t);
        dyn_slnode* s = n ? dyn_skiplist_at(t->t, last ? n - 1 : 0) : NULL;
        if (s)
            k = dyn_skiplist_key(t->t, s);
    }
    return k ? dsx_tree_key_value(ctx, t->strings, k) : JS_UNDEFINED;
}

static JSValue dsx_tree_bound(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int is_floor)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    dsx_keybuf kb;
    const char* owned;
    const void* res = NULL;
    (void)argc;
    if (!t)
        return dsx_tree_get(ctx, this_val), JS_EXCEPTION;
    {
        int kr = dsx_tree_key(ctx, t->strings, argv[0], &kb, &owned, 1);
        if (kr < 0)
            return JS_EXCEPTION;
        if (kr > 0)
            return JS_UNDEFINED;
    }
    t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (t->kind == DSX_TREE_RB) {
        dyn_rbnode* n;
        if (is_floor) {
            n = dyn_rbtree_upper_bound(t->t, &kb);
            n = n ? dyn_rbtree_prev(n) : dyn_rbtree_last(t->t);
        } else {
            n = dyn_rbtree_lower_bound(t->t, &kb);
        }
        if (n)
            res = dyn_rbtree_key(t->t, n);
    } else if (t->kind == DSX_TREE_WB) {
        size_t r = dyn_wbtree_rank(t->t, &kb), n = dyn_wbtree_size(t->t);
        int exact = dyn_wbtree_get(t->t, &kb) != NULL;
        if (is_floor) {
            if (exact)
                dyn_wbtree_select(t->t, r, &res, NULL);
            else if (r > 0)
                dyn_wbtree_select(t->t, r - 1, &res, NULL);
        } else if (r < n) {
            dyn_wbtree_select(t->t, r, &res, NULL);
        }
    } else {
        size_t r = dyn_skiplist_rank(t->t, &kb), n = dyn_skiplist_size(t->t);
        int exact = dyn_skiplist_get(t->t, &kb) != NULL;
        dyn_slnode* s = NULL;
        if (is_floor) {
            if (exact)
                s = dyn_skiplist_at(t->t, r);
            else if (r > 0)
                s = dyn_skiplist_at(t->t, r - 1);
        } else if (r < n) {
            s = dyn_skiplist_at(t->t, r);
        }
        if (s)
            res = dyn_skiplist_key(t->t, s);
    }
    {
        JSValue out = res ? dsx_tree_key_value(ctx, t->strings, res) : JS_UNDEFINED;
        if (owned)
            JS_FreeCString(ctx, owned);
        return out;
    }
}

static JSValue dsx_tree_rank(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_tree_t* t = dsx_tree_opaque(this_val);
    dsx_keybuf kb;
    const char* owned;
    size_t r;
    (void)argc;
    if (!t || t->kind == DSX_TREE_RB)
        return JS_ThrowTypeError(ctx, "rank needs a WBTree or SkipList");
    if (dsx_tree_key(ctx, t->strings, argv[0], &kb, &owned, 0))
        return JS_EXCEPTION;
    t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    r = t->kind == DSX_TREE_WB ? dyn_wbtree_rank(t->t, &kb) : dyn_skiplist_rank(t->t, &kb);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)r);
}

static JSValue dsx_tree_at(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double d;
    dsx_tree_t* t;
    const void* k = NULL;
    void* v = NULL;
    size_t n;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    t = dsx_tree_opaque(this_val);
    if (!t || t->kind == DSX_TREE_RB)
        return JS_ThrowTypeError(ctx, "at needs a WBTree or SkipList");
    n = dsx_tree_count(t);
    if (d < 0)
        d += (double)n;
    if (!(d >= 0) || d >= (double)n || d != (double)(size_t)d)
        return JS_UNDEFINED;
    if (t->kind == DSX_TREE_WB) {
        if (dyn_wbtree_select(t->t, (size_t)d, &k, &v) < 0)
            return JS_UNDEFINED;
    } else {
        dyn_slnode* s = dyn_skiplist_at(t->t, (size_t)d);
        if (!s)
            return JS_UNDEFINED;
        k = dyn_skiplist_key(t->t, s);
        v = dyn_skiplist_val(t->t, s);
    }
    {
        JSValue pair[2];
        pair[0] = dsx_tree_key_value(ctx, t->strings, k);
        pair[1] = JS_DupValue(ctx, dsx_val_get(v));
        return JS_NewArrayFrom(ctx, 2, pair);
    }
}

#define DSX_TREE_COMMON                                          \
    JS_CFUNC_DEF("set", 2, dsx_tree_set),                        \
        JS_CFUNC_MAGIC_DEF("get", 1, dsx_tree_get_has, 0),       \
        JS_CFUNC_MAGIC_DEF("has", 1, dsx_tree_get_has, 1),       \
        JS_CFUNC_DEF("delete", 1, dsx_tree_delete),              \
        JS_CFUNC_DEF("keys", 0, dsx_tree_keys),                  \
        JS_CFUNC_DEF("values", 0, dsx_tree_values),              \
        JS_CFUNC_DEF("entries", 0, dsx_tree_entries),            \
        JS_CFUNC_DEF("[Symbol.iterator]", 0, dsx_tree_iterator), \
        JS_CFUNC_MAGIC_DEF("firstKey", 0, dsx_tree_edge, 0),     \
        JS_CFUNC_MAGIC_DEF("lastKey", 0, dsx_tree_edge, 1),      \
        JS_CFUNC_MAGIC_DEF("floorKey", 1, dsx_tree_bound, 1),    \
        JS_CFUNC_MAGIC_DEF("ceilKey", 1, dsx_tree_bound, 0),     \
        JS_CGETSET_DEF("size", dsx_tree_size, NULL)

static const JSCFunctionListEntry dsx_rbtree_proto[] = {
    DSX_TREE_COMMON,
};

static const JSCFunctionListEntry dsx_ranked_proto[] = {
    DSX_TREE_COMMON,
    JS_CFUNC_DEF("rank", 1, dsx_tree_rank),
    JS_CFUNC_DEF("at", 1, dsx_tree_at),
};

typedef struct {
    JSRuntime* rt;
    dyn_xarray_t* x;
} dsx_xarray_t;

static void dsx_xarray_free(void* native)
{
    dsx_xarray_t* x = (dsx_xarray_t*)native;
    if (!x)
        return;
    dyn_xarray_free(x->x);
    dyn_ds_free(x);
}

static JSClassID dsx_xarray_class_id;
DYN_FINALIZER(dsx_xarray)

static void dsx_xarray_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_xarray_t* x = JS_GetOpaque(val, dsx_xarray_class_id);
    uint64_t i = 0;
    void* v;
    if (!x)
        return;
    while (dyn_xarray_find(x->x, &i, UINT64_MAX, &v)) {
        dsx_mark_value(rt, v, mark_func);
        if (i == UINT64_MAX)
            break;
        i++;
    }
}

static const JSClassDef dsx_xarray_class = { "XArray", .finalizer = dsx_xarray_finalizer, .gc_mark = dsx_xarray_gc_mark };

static JSValue dsx_xarray_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_xarray_t* x = (dsx_xarray_t*)dyn_ds_malloc(sizeof(*x));
    dyn_ds_ops vo;
    (void)argc;
    (void)argv;
    if (!x)
        return JS_ThrowOutOfMemory(ctx);
    x->rt = JS_GetRuntime(ctx);
    vo = dsx_val_ops(x->rt);
    x->x = dyn_xarray_new(&vo);
    if (!x->x) {
        dyn_ds_free(x);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_xarray_class_id, x, dsx_xarray_free);
}

static JSValue dsx_xarray_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint64_t i;
    dsx_xarray_t* x;
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (dsx_index_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    if (dyn_xarray_store(x->x, i, &v) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsx_xarray_get_has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint64_t i;
    dsx_xarray_t* x;
    void* v;
    (void)argc;
    if (dsx_index_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    v = dyn_xarray_load(x->x, i);
    if (magic)
        return JS_NewBool(ctx, v != NULL);
    return v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
}

static JSValue dsx_xarray_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint64_t i;
    dsx_xarray_t* x;
    (void)argc;
    if (dsx_index_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_xarray_erase(x->x, i));
}

static JSValue dsx_xarray_size(JSContext* ctx, JSValueConst this_val)
{
    dsx_xarray_t* x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_xarray_count(x->x));
}

static int dsx_mark_arg(JSContext* ctx, JSValueConst v, unsigned* out)
{
    int32_t m;
    if (JS_ToInt32(ctx, &m, v))
        return -1;
    if (m < 0 || m > 2) {
        JS_ThrowRangeError(ctx, "mark must be 0, 1 or 2");
        return -1;
    }
    *out = (unsigned)m;
    return 0;
}

static JSValue dsx_xarray_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint64_t from = 0, max = UINT64_MAX;
    unsigned mark = 0;
    dsx_xarray_t* x;
    void* v;
    int found;
    int ai = 0;
    if (magic) {
        if (dsx_mark_arg(ctx, argv[0], &mark))
            return JS_EXCEPTION;
        ai = 1;
    }
    if (argc > ai && !JS_IsUndefined(argv[ai]) && dsx_index_arg(ctx, argv[ai], &from))
        return JS_EXCEPTION;
    if (argc > ai + 1 && !JS_IsUndefined(argv[ai + 1]) && dsx_index_arg(ctx, argv[ai + 1], &max))
        return JS_EXCEPTION;
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    found = magic ? dyn_xarray_find_marked(x->x, &from, max, mark, &v) : dyn_xarray_find(x->x, &from, max, &v);
    if (!found)
        return JS_UNDEFINED;
    if (magic)
        v = dyn_xarray_load(x->x, from);
    {
        JSValue pair[2];
        pair[0] = dsx_index_value(ctx, from);
        pair[1] = JS_DupValue(ctx, dsx_val_get(v));
        return JS_NewArrayFrom(ctx, 2, pair);
    }
}

static JSValue dsx_xarray_mark(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint64_t i;
    unsigned m;
    int on = 1;
    dsx_xarray_t* x;
    if (dsx_index_arg(ctx, argv[0], &i) || dsx_mark_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &m))
        return JS_EXCEPTION;
    if (magic == 0 && argc > 2 && !JS_IsUndefined(argv[2]))
        on = JS_ToBool(ctx, argv[2]);
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    if (magic)
        return JS_NewBool(ctx, dyn_xarray_get_mark(x->x, i, m));
    return JS_NewBool(ctx, dyn_xarray_set_mark(x->x, i, m, on) == 0);
}

static JSValue dsx_xarray_entries(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint64_t from = 0, max = UINT64_MAX;
    dsx_xarray_t* x;
    JSValue arr;
    uint32_t n = 0;
    void* v;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && dsx_index_arg(ctx, argv[0], &from))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && dsx_index_arg(ctx, argv[1], &max))
        return JS_EXCEPTION;
    x = dyn_plain_get(ctx, this_val, dsx_xarray_class_id);
    if (!x)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    while (from <= max && dyn_xarray_find(x->x, &from, max, &v)) {
        JSValue pair[2], e;
        pair[0] = dsx_index_value(ctx, from);
        pair[1] = JS_DupValue(ctx, dsx_val_get(v));
        e = JS_NewArrayFrom(ctx, 2, pair);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, arr, n++, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        if (from == UINT64_MAX)
            break;
        from++;
    }
    return arr;
}

static JSValue dsx_xarray_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_ds_iter_via(ctx, this_val, "entries", 0, NULL);
}

static const JSCFunctionListEntry dsx_xarray_proto[] = {
    JS_CFUNC_DEF("set", 2, dsx_xarray_set),
    JS_CFUNC_MAGIC_DEF("get", 1, dsx_xarray_get_has, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, dsx_xarray_get_has, 1),
    JS_CFUNC_DEF("delete", 1, dsx_xarray_delete),
    JS_CFUNC_MAGIC_DEF("next", 2, dsx_xarray_next, 0),
    JS_CFUNC_MAGIC_DEF("nextMarked", 3, dsx_xarray_next, 1),
    JS_CFUNC_MAGIC_DEF("mark", 3, dsx_xarray_mark, 0),
    JS_CFUNC_MAGIC_DEF("isMarked", 2, dsx_xarray_mark, 1),
    JS_CFUNC_DEF("entries", 2, dsx_xarray_entries),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dsx_xarray_iterator),
    JS_CGETSET_DEF("size", dsx_xarray_size, NULL),
};

enum { DSX_PT_EMPTY,
    DSX_PT_KEYS,
    DSX_PT_CIDR };

typedef struct {
    JSRuntime* rt;
    int bytes;
    int space;
    dyn_patricia_t* t;
} dsx_patricia_t;

static int dsx_patricia_space(JSContext* ctx, dsx_patricia_t* p, int want, int inserting)
{
    if (dyn_patricia_size(p->t) == 0) {
        if (inserting)
            p->space = want;
        return 0;
    }
    if (p->space == want)
        return 0;
    JS_ThrowTypeError(ctx, want == DSX_PT_CIDR ? "PatriciaTrie: this trie holds string/byte keys, not CIDR blocks" : "PatriciaTrie: this trie holds CIDR blocks; use addCIDR/matchIP/deleteCIDR");
    return -1;
}

static void dsx_patricia_free(void* native)
{
    dsx_patricia_t* p = (dsx_patricia_t*)native;
    if (!p)
        return;
    dyn_patricia_free(p->t);
    dyn_ds_free(p);
}

static JSClassID dsx_patricia_class_id;
DYN_FINALIZER(dsx_patricia)

static int dsx_pt_mark_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_mark_ctx* m = (dsx_mark_ctx*)ud;
    (void)key;
    (void)bits;
    dsx_mark_value(m->rt, val, m->mark_func);
    return 0;
}

static void dsx_patricia_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_patricia_t* p = JS_GetOpaque(val, dsx_patricia_class_id);
    dsx_mark_ctx m = { rt, mark_func };
    if (p)
        dyn_patricia_walk(p->t, (const uint8_t*)"", 0, dsx_pt_mark_visit, &m);
}

static const JSClassDef dsx_patricia_class = { "PatriciaTrie", .finalizer = dsx_patricia_finalizer, .gc_mark = dsx_patricia_gc_mark };

static JSValue dsx_patricia_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p;
    dyn_ds_ops vo;
    int bytes = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue k = JS_GetPropertyStr(ctx, argv[0], "keys");
        if (JS_IsException(k))
            return k;
        if (!JS_IsUndefined(k)) {
            const char* s = JS_ToCString(ctx, k);
            JS_FreeValue(ctx, k);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "bytes"))
                bytes = 1;
            else if (strcmp(s, "string")) {
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx, "PatriciaTrie: keys must be \"string\" or \"bytes\"");
            }
            JS_FreeCString(ctx, s);
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "PatriciaTrie: the options must be an object");
    }
    p = (dsx_patricia_t*)dyn_ds_malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->rt = JS_GetRuntime(ctx);
    p->bytes = bytes;
    p->space = DSX_PT_EMPTY;
    vo = dsx_val_ops(p->rt);
    p->t = dyn_patricia_new(&vo);
    if (!p->t) {
        dyn_ds_free(p);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_patricia_class_id, p, dsx_patricia_free);
}

typedef struct {
    const uint8_t* p;
    size_t n;
    const char* cstr;
    JSValue buf;
} dsx_bytes;

static int dsx_key_bytes(JSContext* ctx, JSValueConst v, int bytes_mode, dsx_bytes* out)
{
    out->cstr = NULL;
    out->buf = JS_UNDEFINED;
    if (bytes_mode) {
        size_t off, len, bpe;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        size_t ablen;
        if (JS_IsException(ab))
            return -1;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx, "expected a Uint8Array");
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ablen, ab);
        if (!base && len) {
            JS_FreeValue(ctx, ab);
            return -1;
        }
        out->p = base ? base + off : (const uint8_t*)"";
        out->n = len;
        out->buf = ab;
        return 0;
    }
    out->cstr = JS_ToCStringLen(ctx, &out->n, v);
    if (!out->cstr)
        return -1;
    out->p = (const uint8_t*)out->cstr;
    return 0;
}

static void dsx_key_bytes_free(JSContext* ctx, dsx_bytes* b)
{
    if (b->cstr)
        JS_FreeCString(ctx, b->cstr);
    JS_FreeValue(ctx, b->buf);
}

static JSValue dsx_patricia_key_value(JSContext* ctx, dsx_patricia_t* p, const uint8_t* key, uint32_t bits)
{
    if (p->bytes)
        return dyn_nat_u8_copy(ctx, key, bits / 8);
    return JS_NewStringLen(ctx, (const char*)key, bits / 8);
}

static JSValue dsx_patricia_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    dsx_bytes kb;
    JSValue out = JS_UNDEFINED;
    if (!p)
        return JS_EXCEPTION;
    if (dsx_key_bytes(ctx, argv[0], p->bytes, &kb))
        return JS_EXCEPTION;
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    if (kb.n > 0x1FFFFFFF) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_ThrowRangeError(ctx, "PatriciaTrie: key too long");
    }
    if (dsx_patricia_space(ctx, p, DSX_PT_KEYS, magic == 0) < 0) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0: {
        JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (dyn_patricia_put(p->t, kb.p, (uint32_t)kb.n * 8, &v) < 0)
            out = JS_ThrowOutOfMemory(ctx);
        else
            out = JS_DupValue(ctx, this_val);
    } break;
    case 1: {
        void* v = dyn_patricia_get(p->t, kb.p, (uint32_t)kb.n * 8);
        out = v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
    } break;
    case 2:
        out = JS_NewBool(ctx, dyn_patricia_get(p->t, kb.p, (uint32_t)kb.n * 8) != NULL);
        break;
    case 3:
        out = JS_NewBool(ctx, dyn_patricia_remove(p->t, kb.p, (uint32_t)kb.n * 8));
        break;
    default: {
        uint32_t matched = 0;
        void* v = dyn_patricia_longest_prefix(p->t, kb.p, (uint32_t)kb.n * 8, &matched);
        if (v) {
            JSValue pair[2];
            pair[0] = dsx_patricia_key_value(ctx, p, kb.p, matched);
            pair[1] = JS_DupValue(ctx, dsx_val_get(v));
            out = JS_NewArrayFrom(ctx, 2, pair);
        }
    } break;
    }
    dsx_key_bytes_free(ctx, &kb);
    return out;
}

typedef struct {
    JSContext* ctx;
    dsx_patricia_t* p;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_pt_collect;

static int dsx_pt_collect_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_pt_collect* c = (dsx_pt_collect*)ud;
    JSValue pair[2], e;
    pair[0] = dsx_patricia_key_value(c->ctx, c->p, key, bits);
    pair[1] = JS_DupValue(c->ctx, dsx_val_get(val));
    e = JS_NewArrayFrom(c->ctx, 2, pair);
    if (JS_IsException(e) || JS_DefinePropertyValueUint32(c->ctx, c->arr, c->n++, e, JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    return 0;
}

static JSValue dsx_patricia_with_prefix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    dsx_bytes kb;
    dsx_pt_collect c;
    if (!p)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (dsx_key_bytes(ctx, argv[0], p->bytes, &kb))
            return JS_EXCEPTION;
    } else {
        kb.p = (const uint8_t*)"";
        kb.n = 0;
        kb.cstr = NULL;
        kb.buf = JS_UNDEFINED;
    }
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p || kb.n > 0x1FFFFFFF) {
        dsx_key_bytes_free(ctx, &kb);
        return p ? JS_ThrowRangeError(ctx, "PatriciaTrie: prefix too long") : JS_EXCEPTION;
    }
    if (dsx_patricia_space(ctx, p, DSX_PT_KEYS, 0) < 0) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    c.ctx = ctx;
    c.p = p;
    c.n = 0;
    c.failed = 0;
    c.arr = JS_NewArray(ctx);
    if (JS_IsException(c.arr)) {
        dsx_key_bytes_free(ctx, &kb);
        return c.arr;
    }
    dyn_patricia_walk(p->t, kb.p, (uint32_t)kb.n * 8, dsx_pt_collect_visit, &c);
    dsx_key_bytes_free(ctx, &kb);
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

static int dsx_parse_cidr(JSContext* ctx, JSValueConst v, int need_len, uint8_t* key, uint32_t* bits)
{
    const char* s = JS_ToCString(ctx, v);
    char buf[64];
    const char* slash;
    int family, plen, maxlen;
    unsigned char addr[16];
    size_t alen;
    if (!s)
        return -1;
    slash = strchr(s, '/');
    alen = slash ? (size_t)(slash - s) : strlen(s);
    if (alen >= sizeof(buf)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "not an IP address or CIDR block");
        return -1;
    }
    memcpy(buf, s, alen);
    buf[alen] = 0;
    if (inet_pton(AF_INET, buf, addr) == 1) {
        family = 4;
        maxlen = 32;
    } else if (inet_pton(AF_INET6, buf, addr) == 1) {
        family = 6;
        maxlen = 128;
    } else {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "not an IP address or CIDR block");
        return -1;
    }
    plen = maxlen;
    if (slash) {
        char* end;
        long l = strtol(slash + 1, &end, 10);
        if (!slash[1] || *end || l < 0 || l > maxlen) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx, "the prefix length must be 0..%d", maxlen);
            return -1;
        }
        plen = (int)l;
    } else if (need_len) {
        plen = maxlen;
    }
    JS_FreeCString(ctx, s);
    key[0] = (uint8_t)family;
    memcpy(key + 1, addr, (size_t)maxlen / 8);
    *bits = 8 + (uint32_t)plen;
    return 0;
}

static JSValue dsx_patricia_cidr(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint8_t key[17];
    uint32_t bits;
    dsx_patricia_t* p;
    if (dsx_parse_cidr(ctx, argv[0], magic == 0, key, &bits))
        return JS_EXCEPTION;
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p || dsx_patricia_space(ctx, p, DSX_PT_CIDR, magic == 0) < 0)
        return JS_EXCEPTION;
    if (magic == 0) {
        JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (dyn_patricia_put(p->t, key, bits, &v) < 0)
            return JS_ThrowOutOfMemory(ctx);
        return JS_DupValue(ctx, this_val);
    }
    if (magic == 1) {
        void* v = dyn_patricia_longest_prefix(p->t, key, bits, NULL);
        return v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
    }
    return JS_NewBool(ctx, dyn_patricia_remove(p->t, key, bits));
}

static JSValue dsx_patricia_size(JSContext* ctx, JSValueConst this_val)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_patricia_size(p->t));
}

static const JSCFunctionListEntry dsx_patricia_proto[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, dsx_patricia_op, 0),
    JS_CFUNC_MAGIC_DEF("get", 1, dsx_patricia_op, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, dsx_patricia_op, 2),
    JS_CFUNC_MAGIC_DEF("delete", 1, dsx_patricia_op, 3),
    JS_CFUNC_MAGIC_DEF("longestPrefix", 1, dsx_patricia_op, 4),
    JS_CFUNC_DEF("withPrefix", 1, dsx_patricia_with_prefix),
    JS_CFUNC_MAGIC_DEF("addCIDR", 2, dsx_patricia_cidr, 0),
    JS_CFUNC_MAGIC_DEF("matchIP", 1, dsx_patricia_cidr, 1),
    JS_CFUNC_MAGIC_DEF("deleteCIDR", 1, dsx_patricia_cidr, 2),
    JS_CGETSET_DEF("size", dsx_patricia_size, NULL),
};

typedef struct {
    dyn_csr_t* g;
} dsx_csr_t;

static void dsx_csr_free(void* native)
{
    dsx_csr_t* c = (dsx_csr_t*)native;
    if (!c)
        return;
    dyn_csr_free(c->g);
    dyn_ds_free(c);
}

static JSClassID dsx_csr_class_id;
DYN_FINALIZER(dsx_csr)
static const JSClassDef dsx_csr_class = { "CSRGraph", .finalizer = dsx_csr_finalizer };

static JSValue dsx_csr_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    double dn;
    uint32_t n, i, ne;
    int directed = 1;
    dyn_csr_edge* edges = NULL;
    dsx_csr_t* c;
    JSValue len;
    if (JS_ToFloat64(ctx, &dn, argv[0]))
        return JS_EXCEPTION;
    if (!(dn >= 0) || dn > (double)DSX_MAX_NODES || dn != (double)(uint32_t)dn)
        return JS_ThrowRangeError(ctx, "CSRGraph: nodes must be an integer in 0..%u", DSX_MAX_NODES);
    n = (uint32_t)dn;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue d = JS_GetPropertyStr(ctx, argv[2], "directed");
        if (JS_IsException(d))
            return d;
        if (!JS_IsUndefined(d))
            directed = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
    }
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "CSRGraph: edges must be an array of [src, dst, weight?]");
    len = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToUint32(ctx, &ne, len)) {
        JS_FreeValue(ctx, len);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (ne > 50000000)
        return JS_ThrowRangeError(ctx, "CSRGraph: too many edges");
    edges = (dyn_csr_edge*)dyn_ds_malloc(sizeof(dyn_csr_edge) * (ne ? ne : 1));
    if (!edges)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < ne; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i), a, b, w;
        double da, db, dw = 1;
        if (JS_IsException(e))
            goto fail;
        if (!JS_IsObject(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "CSRGraph: edge %u is not an array", i);
            goto fail;
        }
        a = JS_GetPropertyUint32(ctx, e, 0);
        b = JS_GetPropertyUint32(ctx, e, 1);
        w = JS_GetPropertyUint32(ctx, e, 2);
        JS_FreeValue(ctx, e);
        if (dsx_to_f64_free(ctx, &da, a) || dsx_to_f64_free(ctx, &db, b)) {
            JS_FreeValue(ctx, w);
            goto fail;
        }
        if (!JS_IsUndefined(w) && dsx_to_f64_free(ctx, &dw, w))
            goto fail;
        if (!(da >= 0 && da < dn && da == (double)(uint32_t)da && db >= 0 && db < dn && db == (double)(uint32_t)db)) {
            JS_ThrowRangeError(ctx, "CSRGraph: edge %u names a node outside 0..%u", i, n ? n - 1 : 0);
            goto fail;
        }
        if (dw != dw) {
            JS_ThrowRangeError(ctx, "CSRGraph: edge %u has a NaN weight", i);
            goto fail;
        }
        edges[i].src = (uint32_t)da;
        edges[i].dst = (uint32_t)db;
        edges[i].w = dw;
    }
    c = (dsx_csr_t*)dyn_ds_malloc(sizeof(*c));
    if (!c)
        goto oom;
    c->g = dyn_csr_build(n, edges, ne, directed);
    dyn_ds_free(edges);
    if (!c->g) {
        dyn_ds_free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_csr_class_id, c, dsx_csr_free);
oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    dyn_ds_free(edges);
    return JS_EXCEPTION;
}

static int dsx_node_arg(JSContext* ctx, JSValueConst v, dsx_csr_t** pc, JSValueConst this_val, uint32_t* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    *pc = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!*pc)
        return -1;
    if (!(d >= 0) || d >= (double)dyn_csr_nodes((*pc)->g) || d != (double)(uint32_t)d) {
        JS_ThrowRangeError(ctx, "CSRGraph: node out of range");
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue dsx_csr_nodes(JSContext* ctx, JSValueConst this_val, int magic)
{
    dsx_csr_t* c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!c)
        return JS_EXCEPTION;
    return magic ? JS_NewInt64(ctx, (int64_t)dyn_csr_edges(c->g)) : JS_NewInt64(ctx, dyn_csr_nodes(c->g));
}

static JSValue dsx_csr_neighbors(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_csr_t* c;
    uint32_t v, cnt;
    const double* w;
    const uint32_t* nb;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    nb = dyn_csr_neighbors(c->g, v, &w, &cnt);
    if (magic == 2)
        return JS_NewInt64(ctx, cnt);
    if (magic == 1)
        return dsx_typed_copy(ctx, w, cnt, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    return dsx_typed_copy(ctx, nb, cnt, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
}

static JSValue dsx_csr_bfs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c;
    uint32_t v, n;
    int32_t* d;
    JSValue out;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    d = (int32_t*)dyn_ds_malloc(sizeof(int32_t) * (n ? n : 1));
    if (!d || dyn_csr_bfs(c->g, v, d) < 0) {
        dyn_ds_free(d);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = dsx_typed_copy(ctx, d, n, sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    dyn_ds_free(d);
    return out;
}

static JSValue dsx_csr_dijkstra(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c;
    uint32_t v, n;
    double* dist;
    int32_t* pred;
    JSValue obj, a, b;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    dist = (double*)dyn_ds_malloc(sizeof(double) * (n ? n : 1));
    pred = (int32_t*)dyn_ds_malloc(sizeof(int32_t) * (n ? n : 1));
    if (!dist || !pred) {
        dyn_ds_free(dist);
        dyn_ds_free(pred);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_csr_dijkstra(c->g, v, dist, pred) < 0) {
        dyn_ds_free(dist);
        dyn_ds_free(pred);
        return JS_ThrowRangeError(ctx, "CSRGraph.dijkstra: negative edge weights are not allowed");
    }
    a = dsx_typed_copy(ctx, dist, n, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    b = dsx_typed_copy(ctx, pred, n, sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    dyn_ds_free(dist);
    dyn_ds_free(pred);
    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "dist", a, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "prev", b, JS_PROP_C_W_E);
    return obj;
}

static JSValue dsx_csr_pagerank(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double damping = 0.85, tol = 1e-10, di = 100;
    dsx_csr_t* c;
    double* r;
    uint32_t n;
    JSValue out;
    if (argc > 0 && JS_IsObject(argv[0])) {
        static const char* const names[3] = { "damping", "iterations", "tolerance" };
        double* dst[3] = { &damping, &di, &tol };
        int k;
        for (k = 0; k < 3; k++) {
            JSValue x = JS_GetPropertyStr(ctx, argv[0], names[k]);
            if (JS_IsException(x))
                return x;
            if (!JS_IsUndefined(x) && dsx_to_f64_free(ctx, dst[k], x))
                return JS_EXCEPTION;
            if (JS_IsUndefined(x))
                JS_FreeValue(ctx, x);
        }
    }
    if (!(damping >= 0 && damping <= 1))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: damping must be in [0, 1]");
    if (!(di >= 1 && di <= 100000))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: iterations must be in 1..100000");
    if (!(tol >= 0))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: tolerance must be >= 0");
    c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!c)
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    r = (double*)dyn_ds_malloc(sizeof(double) * (n ? n : 1));
    if (!r || dyn_csr_pagerank(c->g, damping, (uint32_t)di, tol, r) < 0) {
        dyn_ds_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = dsx_typed_copy(ctx, r, n, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    dyn_ds_free(r);
    return out;
}

static JSValue dsx_csr_components(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    uint32_t *lab, n, k;
    JSValue obj, labels;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    lab = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!lab)
        return JS_ThrowOutOfMemory(ctx);
    k = dyn_csr_components(c->g, lab);
    if (k == UINT32_MAX) {
        dyn_ds_free(lab);
        return JS_ThrowOutOfMemory(ctx);
    }
    labels = dsx_typed_copy(ctx, lab, n, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(lab);
    if (JS_IsException(labels))
        return labels;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, labels);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "count", JS_NewInt64(ctx, k), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "labels", labels, JS_PROP_C_W_E);
    return obj;
}

static const JSCFunctionListEntry dsx_csr_proto[] = {
    JS_CGETSET_MAGIC_DEF("nodes", dsx_csr_nodes, NULL, 0),
    JS_CGETSET_MAGIC_DEF("edgeCount", dsx_csr_nodes, NULL, 1),
    JS_CFUNC_MAGIC_DEF("neighbors", 1, dsx_csr_neighbors, 0),
    JS_CFUNC_MAGIC_DEF("weights", 1, dsx_csr_neighbors, 1),
    JS_CFUNC_MAGIC_DEF("degree", 1, dsx_csr_neighbors, 2),
    JS_CFUNC_DEF("bfs", 1, dsx_csr_bfs),
    JS_CFUNC_DEF("dijkstra", 1, dsx_csr_dijkstra),
    JS_CFUNC_DEF("pagerank", 1, dsx_csr_pagerank),
    JS_CFUNC_DEF("components", 0, dsx_csr_components),
};

typedef struct {
    dyn_rstar_t* t;
    unsigned dims;
} dsx_rstar_t;

static void dsx_rstar_free(void* native)
{
    dsx_rstar_t* r = (dsx_rstar_t*)native;
    if (!r)
        return;
    dyn_rstar_free(r->t);
    dyn_ds_free(r);
}

static JSClassID dsx_rstar_class_id;
DYN_FINALIZER(dsx_rstar)
static const JSClassDef dsx_rstar_class = { "RStarTree", .finalizer = dsx_rstar_finalizer };

static JSValue dsx_rstar_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int32_t dims = 2;
    dsx_rstar_t* r;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &dims, argv[0]))
        return JS_EXCEPTION;
    if (dims < 1 || dims > DYN_RSTAR_MAX_DIMS)
        return JS_ThrowRangeError(ctx, "RStarTree: dims must be 1..%d", DYN_RSTAR_MAX_DIMS);
    r = (dsx_rstar_t*)dyn_ds_malloc(sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->dims = (unsigned)dims;
    r->t = dyn_rstar_new(r->dims);
    if (!r->t) {
        dyn_ds_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_rstar_class_id, r, dsx_rstar_free);
}

static int dsx_coords(JSContext* ctx, JSValueConst v, unsigned dims, double* out, const char* what)
{
    uint32_t i;
    for (i = 0; i < dims; i++) {
        JSValue x = JS_GetPropertyUint32(ctx, v, i);
        if (JS_IsException(x))
            return -1;
        if (dsx_to_f64_free(ctx, &out[i], x))
            return -1;
        if (out[i] != out[i]) {
            JS_ThrowRangeError(ctx, "RStarTree: %s[%u] is NaN", what, i);
            return -1;
        }
    }
    return 0;
}

static int dsx_rstar_args(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, dsx_rstar_t** pr, double* lo, double* hi, uint64_t* id, int want_id)
{
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    unsigned dims;
    if (!r)
        return -1;
    dims = r->dims;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1])) {
        JS_ThrowTypeError(ctx, "RStarTree: expected (lo: number[], hi: number[])");
        return -1;
    }
    if (dsx_coords(ctx, argv[0], dims, lo, "lo") || dsx_coords(ctx, argv[1], dims, hi, "hi"))
        return -1;
    if (want_id) {
        double d;
        if (JS_ToFloat64(ctx, &d, argc > 2 ? argv[2] : JS_UNDEFINED))
            return -1;
        if (!(d >= 0) || d > 9007199254740991.0 || d != (double)(uint64_t)d) {
            JS_ThrowRangeError(ctx, "RStarTree: id must be a non-negative safe integer");
            return -1;
        }
        *id = (uint64_t)d;
    }
    *pr = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!*pr)
        return -1;
    if ((*pr)->dims != dims) {
        JS_ThrowTypeError(ctx, "RStarTree changed during argument conversion");
        return -1;
    }
    return 0;
}

static JSValue dsx_rstar_insert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    uint64_t id;
    dsx_rstar_t* r;
    unsigned d;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, &id, 1))
        return JS_EXCEPTION;
    for (d = 0; d < r->dims; d++)
        if (lo[d] > hi[d])
            return JS_ThrowRangeError(ctx, "RStarTree.insert: lo[%u] > hi[%u]", d, d);
    if (dyn_rstar_insert(r->t, lo, hi, id) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsx_rstar_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    uint64_t id;
    dsx_rstar_t* r;
    int rc;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, &id, 1))
        return JS_EXCEPTION;
    rc = dyn_rstar_remove(r->t, lo, hi, id);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewBool(ctx, rc);
}

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_rs_collect;

static int dsx_rs_visit(const double* lo, const double* hi, uint64_t id, void* ud)
{
    dsx_rs_collect* c = (dsx_rs_collect*)ud;
    (void)lo;
    (void)hi;
    if (JS_DefinePropertyValueUint32(c->ctx, c->arr, c->n++, JS_NewInt64(c->ctx, (int64_t)id), JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    return 0;
}

static JSValue dsx_rstar_search(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    dsx_rstar_t* r;
    dsx_rs_collect c;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, NULL, 0))
        return JS_EXCEPTION;
    c.ctx = ctx;
    c.n = 0;
    c.failed = 0;
    c.arr = JS_NewArray(ctx);
    if (JS_IsException(c.arr))
        return c.arr;
    dyn_rstar_search(r->t, lo, hi, dsx_rs_visit, &c);
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

static JSValue dsx_rstar_nearest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double p[DYN_RSTAR_MAX_DIMS], dk = 1, *dist;
    uint64_t* ids;
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    unsigned dims;
    size_t got, i;
    JSValue arr;
    if (!r)
        return JS_EXCEPTION;
    dims = r->dims;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "RStarTree.nearest: expected (point: number[], k?)");
    if (dsx_coords(ctx, argv[0], dims, p, "point"))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &dk, argv[1]))
        return JS_EXCEPTION;
    if (!(dk >= 1) || dk > 100000 || dk != (double)(size_t)dk)
        return JS_ThrowRangeError(ctx, "RStarTree.nearest: k must be an integer in 1..100000");
    r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!r || r->dims != dims)
        return r ? JS_ThrowTypeError(ctx, "RStarTree changed during argument conversion") : JS_EXCEPTION;
    ids = (uint64_t*)dyn_ds_malloc(sizeof(uint64_t) * (size_t)dk);
    dist = (double*)dyn_ds_malloc(sizeof(double) * (size_t)dk);
    if (!ids || !dist) {
        dyn_ds_free(ids);
        dyn_ds_free(dist);
        return JS_ThrowOutOfMemory(ctx);
    }
    got = dyn_rstar_nearest(r->t, p, (size_t)dk, ids, dist);
    arr = JS_NewArray(ctx);
    for (i = 0; !JS_IsException(arr) && i < got; i++) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        JS_DefinePropertyValueStr(ctx, o, "id", JS_NewInt64(ctx, (int64_t)ids[i]), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "distance", JS_NewFloat64(ctx, sqrt(dist[i])), JS_PROP_C_W_E);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ids);
    dyn_ds_free(dist);
    return arr;
}

static JSValue dsx_rstar_info(JSContext* ctx, JSValueConst this_val, int magic)
{
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)dyn_rstar_size(r->t));
    if (magic == 1)
        return JS_NewInt64(ctx, dyn_rstar_height(r->t));
    return JS_NewInt64(ctx, r->dims);
}

static const JSCFunctionListEntry dsx_rstar_proto[] = {
    JS_CFUNC_DEF("insert", 3, dsx_rstar_insert),
    JS_CFUNC_DEF("remove", 3, dsx_rstar_remove),
    JS_CFUNC_DEF("search", 2, dsx_rstar_search),
    JS_CFUNC_DEF("nearest", 2, dsx_rstar_nearest),
    JS_CGETSET_MAGIC_DEF("size", dsx_rstar_info, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", dsx_rstar_info, NULL, 1),
    JS_CGETSET_MAGIC_DEF("dims", dsx_rstar_info, NULL, 2),
};

typedef struct {
    dyn_sa_t* sa;
    int is_string;
    uint32_t* u16_at;
    uint8_t* starts;
    size_t text_units;
} dsx_sa_t;

static void dsx_sa_free(void* native)
{
    dsx_sa_t* s = (dsx_sa_t*)native;
    if (!s)
        return;
    dyn_sa_free(s->sa);
    dyn_ds_free(s->u16_at);
    dyn_ds_free(s->starts);
    dyn_ds_free(s);
}

static JSClassID dsx_sa_class_id;
DYN_FINALIZER(dsx_sa)
static const JSClassDef dsx_sa_class = { "SuffixArray", .finalizer = dsx_sa_finalizer };

static JSValue dsx_sa_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_sa_t* s;
    dsx_bytes b;
    int is_string;
    (void)argc;
    is_string = JS_IsString(argv[0]);
    if (!is_string) {
        size_t off, len, bpe;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
        if (JS_IsException(ab))
            return JS_ThrowTypeError(ctx, "SuffixArray: expected a string or a Uint8Array");
        JS_FreeValue(ctx, ab);
        if (bpe != 1)
            return JS_ThrowTypeError(ctx, "SuffixArray: expected a string or a Uint8Array");
    }
    if (dsx_key_bytes(ctx, argv[0], !is_string, &b))
        return JS_EXCEPTION;
    if (b.n >= (size_t)UINT32_MAX - 1) {
        dsx_key_bytes_free(ctx, &b);
        return JS_ThrowRangeError(ctx, "SuffixArray: text too long");
    }
    s = (dsx_sa_t*)dyn_ds_malloc(sizeof(*s));
    if (!s) {
        dsx_key_bytes_free(ctx, &b);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(s, 0, sizeof(*s));
    s->is_string = is_string;
    s->sa = dyn_sa_build(b.p, b.n);
    if (s->sa && is_string) {
        size_t i, u = 0;
        s->u16_at = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (b.n + 1));
        s->starts = (uint8_t*)dyn_ds_malloc(b.n + 1);
        if (s->u16_at && s->starts) {
            for (i = 0; i < b.n;) {
                uint8_t c = b.p[i];
                size_t l = c < 0x80 ? 1 : c < 0xE0 ? 2
                    : c < 0xF0                     ? 3
                                                   : 4,
                       j;
                for (j = 0; j < l && i + j < b.n; j++) {
                    s->u16_at[i + j] = (uint32_t)u;
                    s->starts[i + j] = j == 0;
                }
                i += l;
                u += l == 4 ? 2 : 1;
            }
            s->u16_at[b.n] = (uint32_t)u;
            s->starts[b.n] = 1;
            s->text_units = u;
        }
    } else if (s->sa) {
        s->text_units = b.n;
    }
    dsx_key_bytes_free(ctx, &b);
    if (!s->sa || (is_string && (!s->u16_at || !s->starts))) {
        dsx_sa_free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_sa_class_id, s, dsx_sa_free);
}

static uint32_t dsx_sa_pos(const dsx_sa_t* s, uint32_t bytepos)
{
    return s->is_string ? s->u16_at[bytepos] : bytepos;
}

static JSValue dsx_sa_array(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    const uint32_t *sa, *lcp = NULL;
    uint32_t *out, n, i, k = 0, run_min = UINT32_MAX;
    JSValue r;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    n = (uint32_t)dyn_sa_len(s->sa);
    sa = dyn_sa_array(s->sa);
    if (magic) {
        lcp = dyn_sa_lcp(s->sa);
        if (!lcp)
            return JS_ThrowOutOfMemory(ctx);
    }
    out = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        if (magic && lcp[i] < run_min)
            run_min = lcp[i];
        if (s->is_string && !s->starts[sa[i]])
            continue;
        if (magic) {
            uint32_t l = run_min;
            if (s->is_string)
                l = s->u16_at[sa[i] + l] - s->u16_at[sa[i]];
            out[k++] = l;
            run_min = UINT32_MAX;
        } else {
            out[k++] = dsx_sa_pos(s, sa[i]);
        }
    }
    r = dsx_typed_copy(ctx, out, k, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(out);
    return r;
}

static int dsx_u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static JSValue dsx_sa_search(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    dsx_bytes b;
    size_t first = 0, cnt, i;
    (void)argc;
    if (!s)
        return JS_EXCEPTION;
    if (dsx_key_bytes(ctx, argv[0], !s->is_string && !JS_IsString(argv[0]), &b))
        return JS_EXCEPTION;
    s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    if (!s) {
        dsx_key_bytes_free(ctx, &b);
        return JS_EXCEPTION;
    }
    if (b.n == 0) {
        dsx_key_bytes_free(ctx, &b);
        return magic ? JS_NewArray(ctx) : JS_NewInt64(ctx, 0);
    }
    cnt = dyn_sa_find(s->sa, b.p, b.n, &first);
    dsx_key_bytes_free(ctx, &b);
    if (!magic)
        return JS_NewInt64(ctx, (int64_t)cnt);
    {
        uint32_t* pos = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (cnt ? cnt : 1));
        const uint32_t* sa = dyn_sa_array(s->sa);
        JSValue arr;
        if (!pos)
            return JS_ThrowOutOfMemory(ctx);
        for (i = 0; i < cnt; i++)
            pos[i] = dsx_sa_pos(s, sa[first + i]);
        qsort(pos, cnt, sizeof(uint32_t), dsx_u32_cmp);
        arr = JS_NewArray(ctx);
        for (i = 0; !JS_IsException(arr) && i < cnt; i++)
            if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewInt64(ctx, pos[i]), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                arr = JS_EXCEPTION;
            }
        dyn_ds_free(pos);
        return arr;
    }
}

static JSValue dsx_sa_longest_repeat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    size_t pos = 0, len = 0;
    uint32_t start, units = 0;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (!dyn_sa_lcp(s->sa))
        return JS_ThrowOutOfMemory(ctx);
    if (!s->is_string) {
        len = dyn_sa_longest_repeat(s->sa, &pos);
        units = (uint32_t)len;
    } else {
        const uint32_t* sa = dyn_sa_array(s->sa);
        const uint32_t* lcp = dyn_sa_lcp(s->sa);
        uint32_t n = (uint32_t)dyn_sa_len(s->sa), i, run = UINT32_MAX;
        for (i = 0; i < n; i++) {
            if (lcp[i] < run)
                run = lcp[i];
            if (!s->starts[sa[i]])
                continue;
            if (run) {
                uint32_t u = s->u16_at[sa[i] + run] - s->u16_at[sa[i]];
                if (u > units) {
                    units = u;
                    pos = sa[i];
                }
            }
            run = UINT32_MAX;
        }
    }
    if (!units)
        return JS_UNDEFINED;
    start = dsx_sa_pos(s, (uint32_t)pos);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "start", JS_NewInt64(ctx, start), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "length", JS_NewInt64(ctx, units), JS_PROP_C_W_E);
    return o;
}

static JSValue dsx_sa_length(JSContext* ctx, JSValueConst this_val)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)s->text_units);
}

static const JSCFunctionListEntry dsx_sa_proto[] = {
    JS_CFUNC_MAGIC_DEF("array", 0, dsx_sa_array, 0),
    JS_CFUNC_MAGIC_DEF("lcp", 0, dsx_sa_array, 1),
    JS_CFUNC_MAGIC_DEF("count", 1, dsx_sa_search, 0),
    JS_CFUNC_MAGIC_DEF("find", 1, dsx_sa_search, 1),
    JS_CFUNC_DEF("longestRepeat", 0, dsx_sa_longest_repeat),
    JS_CGETSET_DEF("length", dsx_sa_length, NULL),
};

static int dsx_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dsx_swiss_class_id, &dsx_swiss_class, dsx_swiss_proto, countof(dsx_swiss_proto), dsx_swiss_ctor, "SwissMap") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_rbtree_class_id, &dsx_rbtree_class, dsx_rbtree_proto, countof(dsx_rbtree_proto), dsx_rbtree_ctor, "RBTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_wbtree_class_id, &dsx_wbtree_class, dsx_ranked_proto, countof(dsx_ranked_proto), dsx_wbtree_ctor, "WBTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_skiplist_class_id, &dsx_skiplist_class, dsx_ranked_proto, countof(dsx_ranked_proto), dsx_skiplist_ctor, "SkipList") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_xarray_class_id, &dsx_xarray_class, dsx_xarray_proto, countof(dsx_xarray_proto), dsx_xarray_ctor, "XArray") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_patricia_class_id, &dsx_patricia_class, dsx_patricia_proto, countof(dsx_patricia_proto), dsx_patricia_ctor, "PatriciaTrie") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_csr_class_id, &dsx_csr_class, dsx_csr_proto, countof(dsx_csr_proto), dsx_csr_ctor, "CSRGraph") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_rstar_class_id, &dsx_rstar_class, dsx_rstar_proto, countof(dsx_rstar_proto), dsx_rstar_ctor, "RStarTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_sa_class_id, &dsx_sa_class, dsx_sa_proto, countof(dsx_sa_proto), dsx_sa_ctor, "SuffixArray") < 0)
        return -1;
    return 0;
}

static void dsx_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = { "SwissMap", "RBTree", "WBTree", "SkipList", "XArray", "PatriciaTrie", "CSRGraph", "RStarTree", "SuffixArray" };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}

#define DYN_TID_SWISSMAP 42
#define DYN_TID_RBTREE 43
#define DYN_TID_WBTREE 44
#define DYN_TID_SKIPLIST 45
#define DYN_TID_XARRAY 46
#define DYN_TID_PATRICIA 47
#define DYN_TID_CSRGRAPH 48
#define DYN_TID_RSTAR 49
#define DYN_TID_SUFFIXARRAY 50

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_flat;

static int dsx_flat_push(dsx_flat* f, JSValue v)
{
    if (f->failed || JS_IsException(v) || JS_DefinePropertyValueUint32(f->ctx, f->arr, f->n++, v, JS_PROP_C_W_E) < 0) {
        f->failed = 1;
        return -1;
    }
    return 0;
}

static int dsx_flat_open(JSContext* ctx, dsx_flat* f)
{
    f->ctx = ctx;
    f->n = 0;
    f->failed = 0;
    f->arr = JS_NewArray(ctx);
    return JS_IsException(f->arr) ? -1 : 0;
}

static int dsx_flat_close(dsx_flat* f, dyn_ser_t* w)
{
    int r = f->failed ? -1 : dyn_codec_write_values(f->ctx, w, f->arr);
    JS_FreeValue(f->ctx, f->arr);
    return r;
}

static JSValue dsx_flat_read(JSContext* ctx, dyn_de_t* r, uint32_t group, uint32_t* n)
{
    JSValue arr = dyn_codec_read_values(ctx, r), len;
    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, n, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (*n % group) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: %u values do not form groups of %u", *n, group);
    }
    return arr;
}

typedef JSValue (*dsx_method_fn)(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static JSValue dsx_replay(JSContext* ctx, JSValue obj, JSValue arr, uint32_t n, uint32_t group, dsx_method_fn fn)
{
    uint32_t i, j;
    for (i = 0; i < n; i += group) {
        JSValue args[3], r;
        for (j = 0; j < group; j++)
            args[j] = JS_GetPropertyUint32(ctx, arr, i + j);
        r = fn(ctx, obj, (int)group, (JSValueConst*)args);
        for (j = 0; j < group; j++)
            JS_FreeValue(ctx, args[j]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arr);
    return obj;
}

static int dsx_swiss_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_swiss_t* s = JS_GetOpaque(obj, dsx_swiss_class_id);
    dsx_flat f;
    size_t it = 0;
    const void* k;
    void* v;
    if (!s || dsx_flat_open(ctx, &f) < 0)
        return -1;
    while ((it = dyn_swiss_next(s->m, it, &k, &v)) != 0) {
        dsx_flat_push(&f, JS_DupValue(ctx, dsx_val_get(k)));
        dsx_flat_push(&f, JS_DupValue(ctx, dsx_val_get(v)));
    }
    return dsx_flat_close(&f, w);
}

static JSValue dsx_swiss_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n;
    JSValue arr, obj;
    (void)opts;
    arr = dsx_flat_read(ctx, r, 2, &n);
    if (JS_IsException(arr))
        return arr;
    obj = dsx_swiss_ctor(ctx, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, arr);
        return obj;
    }
    return dsx_replay(ctx, obj, arr, n, 2, dsx_swiss_set);
}

static int dsx_tree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_tree_t* t = dsx_tree_opaque(obj);
    JSValue arr;
    int r;
    if (!t)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)t->strings) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    arr = dsx_tree_collect(ctx, obj, 0, NULL, 2);
    if (JS_IsException(arr))
        return -1;
    {
        dsx_flat f;
        uint32_t i, n;
        JSValue len = JS_GetPropertyStr(ctx, arr, "length");
        if (dsx_flat_open(ctx, &f) < 0 || JS_ToUint32(ctx, &n, len)) {
            JS_FreeValue(ctx, len);
            JS_FreeValue(ctx, arr);
            return -1;
        }
        JS_FreeValue(ctx, len);
        for (i = 0; i < n; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
            dsx_flat_push(&f, JS_GetPropertyUint32(ctx, pair, 0));
            dsx_flat_push(&f, JS_GetPropertyUint32(ctx, pair, 1));
            JS_FreeValue(ctx, pair);
        }
        JS_FreeValue(ctx, arr);
        r = dsx_flat_close(&f, w);
    }
    return r;
}

static JSValue dsx_tree_read_kind(JSContext* ctx, dyn_de_t* r, int kind)
{
    uint8_t strings = dyn_de_u8(r);
    uint32_t n, i;
    JSValue arr, obj, opt, ctor_arg;
    if (!dyn_de_ok(r) || strings > 1)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    arr = dsx_flat_read(ctx, r, 2, &n);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i += 2) {
        JSValue k = JS_GetPropertyUint32(ctx, arr, i);
        int ok = strings ? JS_IsString(k) : JS_IsNumber(k);
        JS_FreeValue(ctx, k);
        if (!ok) {
            JS_FreeValue(ctx, arr);
            return JS_ThrowTypeError(ctx, "malformed DYNS record: key %u is not a %s", i / 2, strings ? "string" : "number");
        }
    }
    opt = JS_NewObject(ctx);
    if (JS_IsException(opt)) {
        JS_FreeValue(ctx, arr);
        return opt;
    }
    JS_DefinePropertyValueStr(ctx, opt, "keys", JS_NewString(ctx, strings ? "string" : "number"), JS_PROP_C_W_E);
    ctor_arg = opt;
    obj = dsx_tree_ctor_kind(ctx, JS_UNDEFINED, 1, (JSValueConst*)&ctor_arg, kind);
    JS_FreeValue(ctx, opt);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, arr);
        return obj;
    }
    return dsx_replay(ctx, obj, arr, n, 2, dsx_tree_set);
}

static JSValue dsx_rbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_RB);
}

static JSValue dsx_wbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_WB);
}

static JSValue dsx_skiplist_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_SL);
}

static int dsx_xarray_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_xarray_t* x = JS_GetOpaque(obj, dsx_xarray_class_id);
    dsx_flat f;
    uint64_t i = 0;
    void* v;
    if (!x || dsx_flat_open(ctx, &f) < 0)
        return -1;
    while (dyn_xarray_find(x->x, &i, UINT64_MAX, &v)) {
        unsigned m, bits = 0;
        for (m = 0; m < 3; m++)
            bits |= (unsigned)(dyn_xarray_get_mark(x->x, i, m) != 0) << m;
        dsx_flat_push(&f, i <= 9007199254740991ULL ? JS_NewInt64(ctx, (int64_t)i) : JS_NewBigUint64(ctx, i));
        dsx_flat_push(&f, JS_DupValue(ctx, dsx_val_get(v)));
        dsx_flat_push(&f, JS_NewInt32(ctx, (int32_t)bits));
        if (i == UINT64_MAX)
            break;
        i++;
    }
    return dsx_flat_close(&f, w);
}

static JSValue dsx_xarray_restore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int32_t bits;
    JSValue r;
    unsigned m;
    (void)argc;
    if (!JS_IsNumber(argv[2]) || JS_ToInt32(ctx, &bits, argv[2]) || bits < 0 || bits > 7)
        return JS_ThrowTypeError(ctx, "malformed DYNS record: XArray mark bits must be 0..7");
    r = dsx_xarray_set(ctx, this_val, 2, argv);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    for (m = 0; m < 3; m++) {
        if (bits & (1 << m)) {
            JSValue a[3];
            a[0] = argv[0];
            a[1] = JS_NewInt32(ctx, (int32_t)m);
            a[2] = JS_TRUE;
            r = dsx_xarray_mark(ctx, this_val, 3, (JSValueConst*)a, 0);
            if (JS_IsException(r))
                return r;
            JS_FreeValue(ctx, r);
        }
    }
    return JS_UNDEFINED;
}

static JSValue dsx_xarray_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n;
    JSValue arr, obj;
    (void)opts;
    arr = dsx_flat_read(ctx, r, 3, &n);
    if (JS_IsException(arr))
        return arr;
    obj = dsx_xarray_ctor(ctx, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, arr);
        return obj;
    }
    return dsx_replay(ctx, obj, arr, n, 3, dsx_xarray_restore);
}

static int dsx_pt_ser_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_flat* f = (dsx_flat*)ud;
    dsx_flat_push(f, dyn_nat_u8_copy(f->ctx, key, (bits + 7) / 8));
    dsx_flat_push(f, JS_NewInt64(f->ctx, bits));
    dsx_flat_push(f, JS_DupValue(f->ctx, dsx_val_get(val)));
    return f->failed;
}

static int dsx_patricia_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_patricia_t* p = JS_GetOpaque(obj, dsx_patricia_class_id);
    dsx_flat f;
    if (!p)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)p->bytes) < 0 || dyn_ser_u8(w, (uint8_t)(dyn_patricia_size(p->t) ? p->space : DSX_PT_EMPTY)) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (dsx_flat_open(ctx, &f) < 0)
        return -1;
    dyn_patricia_walk(p->t, (const uint8_t*)"", 0, dsx_pt_ser_visit, &f);
    return dsx_flat_close(&f, w);
}

static JSValue dsx_patricia_restore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p = JS_GetOpaque(this_val, dsx_patricia_class_id);
    size_t off, len, bpe;
    JSValue ab;
    uint8_t* base;
    size_t ablen;
    double dbits;
    uint32_t bits;
    int ok;
    (void)argc;
    if (!p)
        return JS_EXCEPTION;
    if (!JS_IsNumber(argv[1]) || JS_ToFloat64(ctx, &dbits, argv[1]))
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key length is not a number");
    ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab))
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key is not bytes");
    base = JS_GetArrayBuffer(ctx, &ablen, ab);
    if (p->space == DSX_PT_CIDR) {
        ok = len >= 1 && base && (base[off] == 4 || base[off] == 6);
        if (ok) {
            uint32_t maxbits = 8 + (base[off] == 4 ? 32u : 128u);
            ok = dbits >= 8 && dbits <= maxbits && dbits == (double)(uint32_t)dbits && len == ((uint32_t)dbits + 7) / 8;
        }
    } else {
        ok = dbits >= 0 && dbits == (double)len * 8;
    }
    if (!ok || len > 0x1FFFFFFF) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key does not fit its key space");
    }
    bits = (uint32_t)dbits;
    {
        uint8_t key[17];
        const uint8_t* kp = base ? base + off : (const uint8_t*)"";
        int rc;
        if (p->space == DSX_PT_CIDR) {
            memset(key, 0, sizeof(key));
            memcpy(key, kp, len);
            kp = key;
        }
        rc = dyn_patricia_put(p->t, kp, bits, &argv[2]);
        JS_FreeValue(ctx, ab);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

static JSValue dsx_patricia_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t bytes = dyn_de_u8(r), space = dyn_de_u8(r);
    uint32_t n;
    JSValue arr, obj;
    dsx_patricia_t* p;
    (void)opts;
    if (!dyn_de_ok(r) || bytes > 1 || space > DSX_PT_CIDR)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    arr = dsx_flat_read(ctx, r, 3, &n);
    if (JS_IsException(arr))
        return arr;
    if ((n == 0) != (space == DSX_PT_EMPTY)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key space does not match its entries");
    }
    obj = dsx_patricia_ctor(ctx, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, arr);
        return obj;
    }
    p = JS_GetOpaque(obj, dsx_patricia_class_id);
    p->bytes = bytes;
    p->space = space;
    return dsx_replay(ctx, obj, arr, n, 3, dsx_patricia_restore);
}

static int dsx_csr_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_csr_t* c = JS_GetOpaque(obj, dsx_csr_class_id);
    uint32_t n, v, k, cnt;
    if (!c)
        return -1;
    n = dyn_csr_nodes(c->g);
    if (dyn_ser_u32(w, n) < 0 || dyn_ser_u32(w, (uint32_t)dyn_csr_edges(c->g)) < 0)
        goto oom;
    for (v = 0; v < n; v++) {
        const double* wt;
        const uint32_t* nb = dyn_csr_neighbors(c->g, v, &wt, &cnt);
        for (k = 0; k < cnt; k++)
            if (dyn_ser_u32(w, v) < 0 || dyn_ser_u32(w, nb[k]) < 0 || dyn_ser_f64(w, wt[k]) < 0)
                goto oom;
    }
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
    return -1;
}

static JSValue dsx_csr_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n = dyn_de_u32(r), m, i;
    dyn_csr_edge* edges;
    dsx_csr_t* c;
    (void)opts;
    if (!dyn_de_ok(r) || n > DSX_MAX_NODES || dyn_de_count(r, &m, 16) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    edges = (dyn_csr_edge*)dyn_ds_malloc(sizeof(dyn_csr_edge) * (m ? m : 1));
    if (!edges)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < m; i++) {
        edges[i].src = dyn_de_u32(r);
        edges[i].dst = dyn_de_u32(r);
        edges[i].w = dyn_de_f64(r);
        if (!dyn_de_ok(r) || edges[i].src >= n || edges[i].dst >= n || edges[i].w != edges[i].w) {
            dyn_ds_free(edges);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    c = (dsx_csr_t*)dyn_ds_malloc(sizeof(*c));
    if (!c) {
        dyn_ds_free(edges);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->g = dyn_csr_build(n, edges, m, 1);
    dyn_ds_free(edges);
    if (!c->g) {
        dyn_ds_free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dsx_csr_class_id, c, dsx_csr_free);
}

typedef struct {
    dyn_ser_t* w;
    unsigned dims;
    int failed;
} dsx_rs_ser;

static int dsx_rs_ser_visit(const double* lo, const double* hi, uint64_t id, void* ud)
{
    dsx_rs_ser* s = (dsx_rs_ser*)ud;
    unsigned d;
    for (d = 0; d < s->dims; d++)
        if (dyn_ser_f64(s->w, lo[d]) < 0 || dyn_ser_f64(s->w, hi[d]) < 0)
            s->failed = 1;
    if (dyn_ser_u64(s->w, id) < 0)
        s->failed = 1;
    return s->failed;
}

static int dsx_rstar_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_rstar_t* r = JS_GetOpaque(obj, dsx_rstar_class_id);
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    dsx_rs_ser s;
    unsigned d;
    if (!r)
        return -1;
    for (d = 0; d < r->dims; d++) {
        lo[d] = -(double)INFINITY;
        hi[d] = (double)INFINITY;
    }
    s.w = w;
    s.dims = r->dims;
    s.failed = dyn_ser_u8(w, (uint8_t)r->dims) < 0 || dyn_ser_u32(w, (uint32_t)dyn_rstar_size(r->t)) < 0;
    if (!s.failed)
        dyn_rstar_search(r->t, lo, hi, dsx_rs_ser_visit, &s);
    if (s.failed) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue dsx_rstar_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t dims = dyn_de_u8(r);
    uint32_t n, i;
    unsigned d;
    JSValue obj, dv;
    dsx_rstar_t* t;
    (void)opts;
    if (!dyn_de_ok(r) || dims < 1 || dims > DYN_RSTAR_MAX_DIMS || dyn_de_count(r, &n, (size_t)dims * 16 + 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    dv = JS_NewInt32(ctx, dims);
    obj = dsx_rstar_ctor(ctx, JS_UNDEFINED, 1, (JSValueConst*)&dv);
    if (JS_IsException(obj))
        return obj;
    t = JS_GetOpaque(obj, dsx_rstar_class_id);
    for (i = 0; i < n; i++) {
        double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
        uint64_t id;
        int bad = 0;
        for (d = 0; d < dims; d++) {
            lo[d] = dyn_de_f64(r);
            hi[d] = dyn_de_f64(r);
            if (!(lo[d] <= hi[d]))
                bad = 1;
        }
        id = dyn_de_u64(r);
        if (!dyn_de_ok(r) || bad || id > 9007199254740991ULL) {
            JS_FreeValue(ctx, obj);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (dyn_rstar_insert(t->t, lo, hi, id) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    return obj;
}

static int dsx_sa_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_sa_t* s = JS_GetOpaque(obj, dsx_sa_class_id);
    if (!s)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)s->is_string) < 0 || dyn_ser_blob(w, dyn_sa_text(s->sa), dyn_sa_len(s->sa)) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue dsx_sa_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t is_string = dyn_de_u8(r);
    size_t n = 0;
    const char* p;
    JSValue text, obj;
    (void)opts;
    p = dyn_de_blob(r, &n);
    if (!dyn_de_ok(r) || !p || is_string > 1)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    text = is_string ? JS_NewStringLen(ctx, p, n) : dyn_nat_u8_copy(ctx, (const uint8_t*)p, n);
    if (JS_IsException(text))
        return text;
    obj = dsx_sa_ctor(ctx, JS_UNDEFINED, 1, (JSValueConst*)&text);
    JS_FreeValue(ctx, text);
    return obj;
}

static int dsx_register_codecs(void)
{
    const dyn_codec_t codecs[] = {
        { dsx_swiss_class_id, DYN_TID_SWISSMAP, "SwissMap", dsx_swiss_write, dsx_swiss_read },
        { dsx_rbtree_class_id, DYN_TID_RBTREE, "RBTree", dsx_tree_write, dsx_rbtree_read },
        { dsx_wbtree_class_id, DYN_TID_WBTREE, "WBTree", dsx_tree_write, dsx_wbtree_read },
        { dsx_skiplist_class_id, DYN_TID_SKIPLIST, "SkipList", dsx_tree_write, dsx_skiplist_read },
        { dsx_xarray_class_id, DYN_TID_XARRAY, "XArray", dsx_xarray_write, dsx_xarray_read },
        { dsx_patricia_class_id, DYN_TID_PATRICIA, "PatriciaTrie", dsx_patricia_write, dsx_patricia_read },
        { dsx_csr_class_id, DYN_TID_CSRGRAPH, "CSRGraph", dsx_csr_write, dsx_csr_read },
        { dsx_rstar_class_id, DYN_TID_RSTAR, "RStarTree", dsx_rstar_write, dsx_rstar_read },
        { dsx_sa_class_id, DYN_TID_SUFFIXARRAY, "SuffixArray", dsx_sa_write, dsx_sa_read },
    };
    size_t i;
    for (i = 0; i < countof(codecs); i++)
        if (dyn_codec_register(&codecs[i]) < 0)
            return -1;
    return 0;
}
