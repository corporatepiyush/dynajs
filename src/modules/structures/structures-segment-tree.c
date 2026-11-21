#include "structures-internal.h"
#include "core/ds/ds-segment-tree.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef dyn_segtree_t dsw_segtree_t;

#define DYN_SEG_MAX_N 67108864u

static void dsw_segtree_free(void* native)
{
    dsw_segtree_t* s = (dsw_segtree_t*)native;
    if (!s)
        return;
    dyn_segtree_free(s);
    dyn_ds_free(s);
}

JSClassID dsw_segtree_class_id;

DYN_FINALIZER(dsw_segtree)

const JSClassDef dsw_segtree_class = {
    "SegTree",
    .finalizer = dsw_segtree_finalizer,
};

JSValue dsw_segtree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_segtree_t* s;
    uint32_t n = 0;
    int op = DYN_SEG_SUM;

    {
        double nd;
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "size required");
        if (JS_ToFloat64(ctx, &nd, argv[0]))
            return JS_EXCEPTION;
        if (!(nd >= 1) || nd > (double)DYN_SEG_MAX_N || nd != (double)(uint32_t)nd)
            return JS_ThrowRangeError(ctx,
                "size must be an integer in 1..%u", (unsigned)DYN_SEG_MAX_N);
        n = (uint32_t)nd;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char* o = JS_ToCString(ctx, argv[1]);
        if (!o)
            return JS_EXCEPTION;
        if (!strcmp(o, "sum"))
            op = DYN_SEG_SUM;
        else if (!strcmp(o, "min"))
            op = DYN_SEG_MIN;
        else if (!strcmp(o, "max"))
            op = DYN_SEG_MAX;
        else {
            JS_FreeCString(ctx, o);
            return JS_ThrowRangeError(ctx, "op must be sum|min|max");
        }
        JS_FreeCString(ctx, o);
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "size must be > 0");
    s = (dsw_segtree_t*)dyn_ds_malloc(sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_segtree_init(s, n, op) < 0) {
        dyn_ds_free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_segtree_class_id, s, dsw_segtree_free);
}

static JSValue dsw_segtree_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_segtree_t* s;
    uint32_t i;
    double v;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v, argv[1]))
        return JS_EXCEPTION;
    s = (dsw_segtree_t*)dyn_plain_get(ctx, this_val, dsw_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (i >= s->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    dyn_segtree_set(s, i, v);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_segtree_range_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_segtree_t* s;
    uint32_t lo, hi;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &lo))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &hi))
        return JS_EXCEPTION;
    s = (dsw_segtree_t*)dyn_plain_get(ctx, this_val, dsw_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (lo >= s->n || hi >= s->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    return JS_NewFloat64(ctx, dyn_segtree_query(s, lo, hi));
}

static JSValue dsw_segtree_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_segtree_t* s = (dsw_segtree_t*)dyn_plain_get(ctx, this_val, dsw_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)s->n);
}

const JSCFunctionListEntry dsw_segtree_proto[] = {
    JS_CFUNC_DEF("update", 2, dsw_segtree_update),
    JS_CFUNC_DEF("rangeQuery", 2, dsw_segtree_range_query),
    JS_CGETSET_DEF("size", dsw_segtree_size, NULL),
};

int dsw_segtree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_segtree_t* s = JS_GetOpaque(obj, dsw_segtree_class_id);
    (void)ctx;
    if (!s)
        return -1;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0 || dyn_ser_u32(w, (uint32_t)s->op) < 0)
        return -1;
    return dyn_num_write(w, s->tree + 1, (size_t)2 * s->n - 1);
}

JSValue dsw_segtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), op = dyn_de_u32(r), n, elems;
    uint8_t kind;
    JSValue obj, args[2];
    dsw_segtree_t* s;
    static const char* const opnames[] = { "sum", "min", "max" };
    (void)opts;

    if (!dyn_de_ok(r) || op >= countof(opnames))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &elems, &kind) < 0 || !elems || !(elems & 1))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        n = (elems + 1) / 2;
    } else {
        n = first;
        kind = DS_NUM_RAW;
        if ((uint64_t)n * 16 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    args[0] = JS_NewInt64(ctx, (int64_t)n);
    args[1] = JS_NewString(ctx, opnames[op]);
    obj = dyn_ds_construct(ctx, dsw_segtree_class_id, 2,
        (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(obj))
        return obj;
    s = JS_GetOpaque(obj, dsw_segtree_class_id);
    if (dyn_num_fill(r, kind, s->tree + 1, 2 * n - 1) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    return obj;
}

#endif
