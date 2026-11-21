#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void dsw_heap_free(void* native)
{
    dsw_heap_t* h = (dsw_heap_t*)native;
    if (!h)
        return;
    dyn_heap_free(&h->heap, ds_cell_release, h->rt);
    JS_FreeValueRT(h->rt, h->cmp);
    dyn_ds_free(h);
}

JSClassID dsw_heap_class_id;

static void dsw_heap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dsw_heap_free(JS_GetOpaque(val, dsw_heap_class_id));
}

static void dsw_heap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_heap_t* h = JS_GetOpaque(val, dsw_heap_class_id);
    uint32_t i;
    if (!h)
        return;
    JS_MarkValue(rt, h->cmp, mark_func);
    for (i = 0; i < h->heap.count; i++)
        JS_MarkValue(rt, ds_cell_val(&h->heap.items[i]), mark_func);
}

const JSClassDef dsw_heap_class = {
    "Heap",
    .finalizer = dsw_heap_finalizer,
    .gc_mark = dsw_heap_gc_mark,
};

JSValue dsw_heap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_heap_t* h;
    int natural = (argc < 1 || JS_IsUndefined(argv[0]));
    if (!natural && !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Heap(comparator) requires a comparator function");
    h = (dsw_heap_t*)dyn_ds_malloc(sizeof(*h));
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    h->rt = JS_GetRuntime(ctx);
    h->natural = natural;
    h->cmp = natural ? JS_UNDEFINED : JS_DupValue(ctx, argv[0]);
    dyn_heap_init(&h->heap);
    h->busy = 0;
    return dyn_plain_wrap(ctx, new_target, dsw_heap_class_id, h, dsw_heap_free);
}

static int dsw_heap_number(JSValueConst v, double* out)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) {
        *out = (double)JS_VALUE_GET_INT(v);
        return 1;
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        *out = JS_VALUE_GET_FLOAT64(v);
        return 1;
    }
    return 0;
}

static int dsw_heap_cmp_nat(void* ud, const dyn_cell_t* a, const dyn_cell_t* b)
{
    dsw_heap_cmpctx* c = (dsw_heap_cmpctx*)ud;
    double x, y;
    if (!dsw_heap_number(ds_cell_val(a), &x) || !dsw_heap_number(ds_cell_val(b), &y)) {
        JS_ThrowTypeError(c->ctx, "a Heap built without a comparator orders "
                                  "numbers only; pass a comparator for other values");
        return DYN_HEAP_ABORT;
    }
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int dsw_heap_sift(JSContext* ctx, JSValueConst this_val,
    dsw_heap_t* h, uint32_t idx, int down)
{
    dsw_heap_cmpctx c;
    int rc;
    c.ctx = ctx;
    c.this_val = this_val;
    c.h = h;
    if (h->natural)
        return down ? dyn_heap_sift_down(&h->heap, idx, dsw_heap_cmp_nat, &c) : dyn_heap_sift_up(&h->heap, idx, dsw_heap_cmp_nat, &c);
    h->busy = 1;
    rc = down ? dyn_heap_sift_down(&h->heap, idx, dsw_heap_cmp_js, &c) : dyn_heap_sift_up(&h->heap, idx, dsw_heap_cmp_js, &c);
    if (dyn_plain_get(ctx, this_val, dsw_heap_class_id))
        h->busy = 0;
    return rc;
}

static JSValue dsw_heap_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_heap_t* h;
    uint32_t idx;
    (void)argc;
    h = (dsw_heap_t*)dyn_plain_get(ctx, this_val, dsw_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (h->natural) {
        double d;
        if (!dsw_heap_number(argv[0], &d))
            return JS_ThrowTypeError(ctx,
                "a Heap built without a comparator orders numbers only; "
                "pass a comparator for other values");
    }
    if (dyn_heap_reserve(&h->heap, h->heap.count + 1))
        return JS_ThrowOutOfMemory(ctx);
    h->heap.items[h->heap.count] = ds_val_cell(JS_DupValue(ctx, argv[0]));
    idx = h->heap.count;
    h->heap.count++;
    if (dsw_heap_sift(ctx, this_val, h, idx, 0)) {
        h->heap.count--;
        JS_FreeValue(ctx, ds_cell_val(&h->heap.items[h->heap.count]));
        return JS_EXCEPTION;
    }
    return JS_NewInt64(ctx, (int64_t)h->heap.count);
}

static JSValue dsw_heap_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_heap_t* h;
    JSValue top;
    (void)argc;
    (void)argv;
    h = (dsw_heap_t*)dyn_plain_get(ctx, this_val, dsw_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (h->heap.count == 0)
        return JS_UNDEFINED;
    top = ds_cell_val(&h->heap.items[0]);
    h->heap.count--;
    if (h->heap.count > 0) {
        h->heap.items[0] = h->heap.items[h->heap.count];
        if (dsw_heap_sift(ctx, this_val, h, 0, 1)) {
            JS_FreeValue(ctx, top);
            return JS_EXCEPTION;
        }
    }
    return top;
}

static JSValue dsw_heap_peek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_heap_t* h;

    (void)argc;
    (void)argv;
    h = (dsw_heap_t*)dyn_plain_get(ctx, this_val, dsw_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->heap.count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, ds_cell_val(&h->heap.items[0]));
}

static JSValue dsw_heap_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_heap_t* h = (dsw_heap_t*)dyn_plain_get(ctx, this_val, dsw_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)h->heap.count);
}

const JSCFunctionListEntry dsw_heap_proto[] = {
    JS_CFUNC_DEF("push", 1, dsw_heap_push),
    JS_CFUNC_DEF("pop", 0, dsw_heap_pop),
    JS_CFUNC_DEF("peek", 0, dsw_heap_peek),
    JS_CGETSET_DEF("size", dsw_heap_size, NULL),
    JS_CGETSET_DEF("length", dsw_heap_size, NULL),
};

int dsw_heap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_heap_t* h = JS_GetOpaque(obj, dsw_heap_class_id);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (!h || JS_IsException(arr))
        return -1;
    if (dyn_ser_u32(w, h->heap.count) < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    for (i = 0; i < h->heap.count; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, ds_cell_val(&h->heap.items[i])),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue dsw_heap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    if (!JS_IsUndefined(opts) && !JS_IsFunction(ctx, opts))
        return JS_ThrowTypeError(ctx,
            "decoding a Heap takes its comparator or nothing: "
            "Heap.deserialize(bytes, (a, b) => a - b), or "
            "Heap.deserialize(bytes) for natural order");
    return dyn_ds_read_pushed(ctx, r, dsw_heap_class_id, "push", 0, opts);
}

#endif
