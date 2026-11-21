#include "structures-internal.h"
#include "core/ds/ds-list.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef dyn_list_t dsw_list_t;

static void dsw_list_free(void* native)
{
    dsw_list_t* l = (dsw_list_t*)native;
    if (!l)
        return;
    dyn_list_free(l);
    dyn_ds_free(l);
}

JSClassID dsw_list_class_id;

static void dsw_list_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dsw_list_free(JS_GetOpaque(val, dsw_list_class_id));
}

static void dsw_list_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_list_t* l = JS_GetOpaque(val, dsw_list_class_id);
    dyn_list_node_t* n;
    if (!l)
        return;
    for (n = l->head; n; n = n->next)
        JS_MarkValue(rt, ds_cell_val(&n->value), mark_func);
}

const JSClassDef dsw_list_class = {
    "List",
    .finalizer = dsw_list_finalizer,
    .gc_mark = dsw_list_gc_mark,
};

JSValue dsw_list_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_list_t* l;
    (void)argc;
    (void)argv;
    l = (dsw_list_t*)dyn_ds_malloc(sizeof(*l));
    if (!l)
        return JS_ThrowOutOfMemory(ctx);
    dyn_list_init(l, ds_cell_release, JS_GetRuntime(ctx));
    return dyn_plain_wrap(ctx, new_target, dsw_list_class_id, l, dsw_list_free);
}

static JSValue dsw_list_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_list_t* l;
    dyn_cell_t c;
    (void)argc;
    l = (dsw_list_t*)dyn_plain_get(ctx, this_val, dsw_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    c = ds_val_cell(argv[0]);
    if ((front ? dyn_list_push_front(l, &c) : dyn_list_push_back(l, &c)) < 0)
        return JS_ThrowOutOfMemory(ctx);
    JS_DupValue(ctx, argv[0]);
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dsw_list_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_list_t* l;
    dyn_cell_t c;
    (void)argc;
    (void)argv;
    l = (dsw_list_t*)dyn_plain_get(ctx, this_val, dsw_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if ((front ? dyn_list_pop_front(l, &c) : dyn_list_pop_back(l, &c)) < 0)
        return JS_UNDEFINED;
    return ds_cell_val(&c);
}

static JSValue dsw_list_peek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int front)
{
    dsw_list_t* l;
    dyn_list_node_t* n;
    (void)argc;
    (void)argv;
    l = (dsw_list_t*)dyn_plain_get(ctx, this_val, dsw_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    n = front ? l->head : l->tail;
    if (!n)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ds_cell_val(&n->value));
}

static JSValue dsw_list_length(JSContext* ctx, JSValueConst this_val)
{
    dsw_list_t* l = (dsw_list_t*)dyn_plain_get(ctx, this_val, dsw_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dsw_list_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_list_t* l;
    dyn_list_node_t* n;
    JSValue arr;
    uint32_t i;

    (void)argc;
    (void)argv;
    l = (dsw_list_t*)dyn_plain_get(ctx, this_val, dsw_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    i = 0;
    for (n = l->head; n; n = n->next) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i++, JS_DupValue(ctx, ds_cell_val(&n->value)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dsw_list_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue arr, fn, iter;

    arr = dsw_list_to_array(ctx, this_val, argc, argv);
    if (JS_IsException(arr))
        return arr;
    fn = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, arr);
        return fn;
    }
    iter = JS_Call(ctx, fn, arr, 0, NULL);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arr);
    return iter;
}

const JSCFunctionListEntry dsw_list_proto[] = {
    JS_CFUNC_MAGIC_DEF("pushFront", 1, dsw_list_push, 1),
    JS_CFUNC_MAGIC_DEF("pushBack", 1, dsw_list_push, 0),
    JS_CFUNC_MAGIC_DEF("popFront", 0, dsw_list_pop, 1),
    JS_CFUNC_MAGIC_DEF("popBack", 0, dsw_list_pop, 0),
    JS_CFUNC_MAGIC_DEF("front", 0, dsw_list_peek, 1),
    JS_CFUNC_MAGIC_DEF("back", 0, dsw_list_peek, 0),
    JS_CFUNC_DEF("toArray", 0, dsw_list_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dsw_list_iterator),
    JS_CGETSET_DEF("length", dsw_list_length, NULL),
};

int dsw_list_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    JSValue arr, len, fn;
    uint32_t n = 0;
    int r;

    fn = JS_GetPropertyStr(ctx, obj, "toArray");
    if (JS_IsException(fn))
        return -1;
    arr = JS_Call(ctx, fn, obj, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return -1;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &n, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return -1;
    }
    JS_FreeValue(ctx, len);
    if (dyn_ser_u32(w, n) < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue dsw_list_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dsw_list_class_id, "pushBack", 0,
        JS_UNDEFINED);
}

#endif
