#include "structures-internal.h"
#include "core/ds/ds-circular-buffer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef dyn_circbuf_t dsw_ringbuf_t;

static void dsw_ringbuf_free(void* native)
{
    dsw_ringbuf_t* r = (dsw_ringbuf_t*)native;
    if (!r)
        return;
    dyn_circbuf_free(r);
    dyn_ds_free(r);
}

JSClassID dsw_ringbuf_class_id;

DYN_FINALIZER(dsw_ringbuf)

static void dsw_ringbuf_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_ringbuf_t* r = JS_GetOpaque(val, dsw_ringbuf_class_id);
    uint32_t i;
    if (!r)
        return;
    for (i = 0; i < r->count; i++)
        JS_MarkValue(rt, ds_cell_val(dyn_circbuf_at(r, i)), mark_func);
}

const JSClassDef dsw_ringbuf_class = {
    "RingBuffer",
    .finalizer = dsw_ringbuf_finalizer,
    .gc_mark = dsw_ringbuf_gc_mark,
};

JSValue dsw_ringbuf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_ringbuf_t* r;
    uint32_t cap;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "capacity required");
    if (dyn_size_arg(ctx, argv[0], DYN_MAX_CAPACITY, &cap))
        return JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be > 0");
    r = (dsw_ringbuf_t*)dyn_ds_malloc(sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_circbuf_init(r, cap, ds_cell_release, JS_GetRuntime(ctx)) < 0) {
        dyn_ds_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_ringbuf_class_id, r, dsw_ringbuf_free);
}

static JSValue dsw_ringbuf_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_ringbuf_t* r;
    (void)argc;
    r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    {
        dyn_cell_t c = ds_val_cell(JS_DupValue(ctx, argv[0])), old;
        if (dyn_circbuf_push(r, &c, &old))
            JS_FreeValue(ctx, ds_cell_val(&old));
    }
    return JS_NewInt64(ctx, (int64_t)r->count);
}

static JSValue dsw_ringbuf_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_ringbuf_t* r;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (i >= r->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ds_cell_val(dyn_circbuf_at(r, i)));
}

static JSValue dsw_ringbuf_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_ringbuf_t* r;
    JSValue arr;
    uint32_t i;
    (void)argc;
    (void)argv;
    r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < r->count; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, ds_cell_val(dyn_circbuf_at(r, i))),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dsw_ringbuf_length(JSContext* ctx, JSValueConst this_val)
{
    dsw_ringbuf_t* r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->count);
}

static JSValue dsw_ringbuf_capacity(JSContext* ctx, JSValueConst this_val)
{
    dsw_ringbuf_t* r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->cap);
}

static JSValue dsw_ringbuf_full(JSContext* ctx, JSValueConst this_val)
{
    dsw_ringbuf_t* r = (dsw_ringbuf_t*)dyn_plain_get(ctx, this_val, dsw_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r->count == r->cap);
}

const JSCFunctionListEntry dsw_ringbuf_proto[] = {
    JS_CFUNC_DEF("push", 1, dsw_ringbuf_push),
    JS_CFUNC_DEF("get", 1, dsw_ringbuf_get),
    JS_CFUNC_DEF("toArray", 0, dsw_ringbuf_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("length", dsw_ringbuf_length, NULL),
    JS_CGETSET_DEF("capacity", dsw_ringbuf_capacity, NULL),
    JS_CGETSET_DEF("full", dsw_ringbuf_full, NULL),
};

static JSValue dsw_ringbuf_val_at(void* native, uint32_t i)
{
    dsw_ringbuf_t* rb = (dsw_ringbuf_t*)native;
    return ds_cell_val(dyn_circbuf_at(rb, i));
}

int dsw_ringbuf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_ringbuf_t* rb = JS_GetOpaque(obj, dsw_ringbuf_class_id);
    if (!rb || dyn_ser_u32(w, rb->cap) < 0 || dyn_ser_u32(w, rb->count) < 0)
        return -1;
    return dyn_ds_write_values(ctx, w, rb, rb->count, dsw_ringbuf_val_at);
}

JSValue dsw_ringbuf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dsw_ringbuf_class_id, "push", 1,
        JS_UNDEFINED);
}

#endif
