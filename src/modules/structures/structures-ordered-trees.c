#include "structures-internal.h"
#include "core/ds/ds-cbtree.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

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

typedef union {
    double num;
    dsx_skey str;
} dsx_keybuf;

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int strings, mode, failed;
} dsx_collect;

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

JSClassID dsx_rbtree_class_id, dsx_wbtree_class_id, dsx_skiplist_class_id;

static void dsx_tree_free(void* native)
{
    dsx_tree_t* t = (dsx_tree_t*)native;
    if (!t)
        return;
    dyn_cbt_free(t->t);
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

static int dsx_tree_mark_visit(const void* key, void* val, void* ud)
{
    dsx_mark_ctx* m = (dsx_mark_ctx*)ud;
    (void)key;
    dsx_mark_value(m->rt, val, m->mark_func);
    return 0;
}

static dsx_tree_t* dsx_tree_opaque(JSValueConst val)
{
    JSClassID c;
    void* p = JS_GetAnyOpaque(val, &c);
    if (!c || (c != dsx_rbtree_class_id && c != dsx_wbtree_class_id && c != dsx_skiplist_class_id))
        return NULL;
    return (dsx_tree_t*)p;
}

static void dsx_tree_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_tree_t* t = dsx_tree_opaque(val);
    if (!t)
        return;
    {
        dsx_mark_ctx m = { rt, mark_func };
        dyn_cbt_walk(t->t, dsx_tree_mark_visit, &m);
    }
}

const JSClassDef dsx_rbtree_class = { "RBTree", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };

const JSClassDef dsx_wbtree_class = { "WBTree", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };

const JSClassDef dsx_skiplist_class = { "SkipList", .finalizer = dsx_tree_finalizer, .gc_mark = dsx_tree_gc_mark };

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
        ko.cmp = dyn_ds_cmp_f64;
    }
    vo = dsx_val_ops(t->rt);
    t->t = dyn_cbt_new(&ko, &vo);
    if (!t->t) {
        dyn_ds_free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, cid, t, dsx_tree_free);
}

JSValue dsx_rbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_RB);
}

JSValue dsx_wbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_WB);
}

JSValue dsx_skiplist_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return dsx_tree_ctor_kind(ctx, nt, argc, argv, DSX_TREE_SL);
}

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
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        kb->num = JS_VALUE_GET_INT(v);
    else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)))
        kb->num = JS_VALUE_GET_FLOAT64(v);
    else if (JS_ToFloat64(ctx, &kb->num, v))
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
    return dyn_cbt_size(t->t);
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
    if (JS_IsObject(argv[0]))
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
        r = dyn_cbt_put(t->t, &kb, &v);
        if (r < 0 || dsx_tree_count(t) == before)
            dyn_ds_free(adopt);
    } else {
        r = dyn_cbt_put(t->t, &kb, &v);
    }
    if (r < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static void* dsx_tree_lookup(dsx_tree_t* t, const dsx_keybuf* kb)
{
    return dyn_cbt_get(t->t, kb);
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
    if (JS_IsObject(argv[0]))
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
    if (JS_IsObject(argv[0]))
        t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    r = dyn_cbt_remove(t->t, &kb);
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

static int dsx_tree_collect_visit(const void* key, void* val, void* ud)
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
    dyn_cbt_walk(t->t, dsx_tree_collect_visit, &c);
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
    {
        size_t n = dyn_cbt_size(t->t);
        if (n)
            dyn_cbt_at(t->t, last ? n - 1 : 0, &k, NULL);
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
    if (JS_IsObject(argv[0]))
        t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    res = dyn_cbt_bound(t->t, &kb, is_floor);
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
    if (JS_IsObject(argv[0]))
        t = dsx_tree_get(ctx, this_val);
    if (!t) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    r = dyn_cbt_rank(t->t, &kb);
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
    if (dyn_cbt_at(t->t, (size_t)d, &k, &v) < 0)
        return JS_UNDEFINED;
    {
        JSValue pair[2];
        pair[0] = dsx_tree_key_value(ctx, t->strings, k);
        pair[1] = JS_DupValue(ctx, dsx_val_get(v));
        return JS_NewArrayFrom(ctx, 2, pair);
    }
}

const JSCFunctionListEntry dsx_rbtree_proto[] = {
    DSX_TREE_COMMON,
};

const JSCFunctionListEntry dsx_ranked_proto[] = {
    DSX_TREE_COMMON,
    JS_CFUNC_DEF("rank", 1, dsx_tree_rank),
    JS_CFUNC_DEF("at", 1, dsx_tree_at),
};

typedef struct {
    dsx_flat f;
    int strings;
} dsx_tree_flat;

static int dsx_tree_flat_visit(const void* key, void* val, void* ud)
{
    dsx_tree_flat* tf = (dsx_tree_flat*)ud;
    dsx_flat_push(&tf->f, dsx_tree_key_value(tf->f.ctx, tf->strings, key));
    dsx_flat_push(&tf->f, JS_DupValue(tf->f.ctx, dsx_val_get(val)));
    return tf->f.failed;
}

int dsx_tree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_tree_t* t = dsx_tree_opaque(obj);
    int r;
    if (!t)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)t->strings) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    {
        dsx_tree_flat tf;
        if (dsx_flat_open(ctx, &tf.f) < 0)
            return -1;
        tf.strings = t->strings;
        dyn_cbt_walk(t->t, dsx_tree_flat_visit, &tf);
        r = dsx_flat_close(&tf.f, w);
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
        int ok = JS_IsException(k) ? -1 : strings ? JS_IsString(k) : JS_IsNumber(k);
        JS_FreeValue(ctx, k);
        if (ok <= 0) {
            JS_FreeValue(ctx, arr);
            if (ok < 0)
                return JS_EXCEPTION;
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

JSValue dsx_rbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_RB);
}

JSValue dsx_wbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_WB);
}

JSValue dsx_skiplist_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dsx_tree_read_kind(ctx, r, DSX_TREE_SL);
}

#endif
