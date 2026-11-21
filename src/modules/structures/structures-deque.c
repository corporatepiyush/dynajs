#include "structures-internal.h"
#include "core/ds/ds-deque.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef dyn_deque_t dsw_deque_t;

static void dsw_deque_free(void* native)
{
    dsw_deque_t* d = (dsw_deque_t*)native;
    if (!d)
        return;
    dyn_deque_free(d);
    dyn_ds_free(d);
}

JSClassID dsw_deque_class_id;

DYN_FINALIZER(dsw_deque)

static void dsw_deque_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_deque_t* d = JS_GetOpaque(val, dsw_deque_class_id);
    uint32_t i;
    if (!d)
        return;
    for (i = 0; i < d->count; i++)
        JS_MarkValue(rt, ds_cell_val(dyn_deque_at(d, i)), mark_func);
}

const JSClassDef dsw_deque_class = {
    "Deque",
    .finalizer = dsw_deque_finalizer,
    .gc_mark = dsw_deque_gc_mark,
};

JSValue dsw_deque_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dsw_deque_t*)dyn_ds_malloc(sizeof(*d));
    if (!d)
        return JS_ThrowOutOfMemory(ctx);
    dyn_deque_init(d, ds_cell_release, JS_GetRuntime(ctx));
    return dyn_plain_wrap(ctx, new_target, dsw_deque_class_id, d, dsw_deque_free);
}

static JSValue dsw_deque_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_deque_t* d;
    dyn_cell_t c;
    (void)argc;
    d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    c = ds_val_cell(argv[0]);
    if ((front ? dyn_deque_push_front(d, &c) : dyn_deque_push_back(d, &c)) < 0)
        return JS_ThrowOutOfMemory(ctx);
    JS_DupValue(ctx, argv[0]);
    return JS_NewInt64(ctx, (int64_t)d->count);
}

static JSValue dsw_deque_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_deque_t* d;
    dyn_cell_t c;
    (void)argc;
    (void)argv;
    d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if ((front ? dyn_deque_pop_front(d, &c) : dyn_deque_pop_back(d, &c)) < 0)
        return JS_UNDEFINED;
    return ds_cell_val(&c);
}

static JSValue dsw_deque_peek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ds_cell_val(dyn_deque_at(d, front ? 0 : d->count - 1)));
}

static JSValue dsw_deque_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_deque_t* d;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (i >= d->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ds_cell_val(dyn_deque_at(d, i)));
}

static JSValue dsw_deque_length(JSContext* ctx, JSValueConst this_val)
{
    dsw_deque_t* d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)d->count);
}

static JSValue dsw_deque_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_deque_t* d;
    JSValue arr;
    uint32_t i;
    (void)argc;
    (void)argv;
    d = (dsw_deque_t*)dyn_plain_get(ctx, this_val, dsw_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < d->count; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, ds_cell_val(dyn_deque_at(d, i))),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

const JSCFunctionListEntry dsw_deque_proto[] = {
    JS_CFUNC_MAGIC_DEF("pushBack", 1, dsw_deque_push, 0),
    JS_CFUNC_MAGIC_DEF("pushFront", 1, dsw_deque_push, 1),
    JS_CFUNC_MAGIC_DEF("popFront", 0, dsw_deque_pop, 1),
    JS_CFUNC_MAGIC_DEF("popBack", 0, dsw_deque_pop, 0),
    JS_CFUNC_MAGIC_DEF("peekFront", 0, dsw_deque_peek, 1),
    JS_CFUNC_MAGIC_DEF("peekBack", 0, dsw_deque_peek, 0),
    JS_CFUNC_DEF("get", 1, dsw_deque_get),
    JS_CFUNC_DEF("toArray", 0, dsw_deque_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("length", dsw_deque_length, NULL),
};

static JSValue dsw_deque_val_at(void* native, uint32_t i)
{
    dsw_deque_t* d = (dsw_deque_t*)native;
    return ds_cell_val(dyn_deque_at(d, i));
}

int dsw_deque_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_deque_t* d = JS_GetOpaque(obj, dsw_deque_class_id);
    if (!d || dyn_ser_u32(w, d->count) < 0)
        return -1;
    return dyn_ds_write_values(ctx, w, d, d->count, dsw_deque_val_at);
}

JSValue dsw_deque_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dsw_deque_class_id, "pushBack", 0,
        JS_UNDEFINED);
}

#endif
