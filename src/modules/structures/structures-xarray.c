#include "structures-internal.h"
#include "core/ds/ds-xarray.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    JSRuntime* rt;
    dyn_xarray_t* x;
} dsx_xarray_t;

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

static void dsx_xarray_free(void* native)
{
    dsx_xarray_t* x = (dsx_xarray_t*)native;
    if (!x)
        return;
    dyn_xarray_free(x->x);
    dyn_ds_free(x);
}

JSClassID dsx_xarray_class_id;

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

const JSClassDef dsx_xarray_class = { "XArray", .finalizer = dsx_xarray_finalizer, .gc_mark = dsx_xarray_gc_mark };

JSValue dsx_xarray_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
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

const JSCFunctionListEntry dsx_xarray_proto[] = {
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

int dsx_xarray_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
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

JSValue dsx_xarray_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
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

#endif
