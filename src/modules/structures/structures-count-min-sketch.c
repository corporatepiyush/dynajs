#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static void ds2_cms_free(void* native)
{
    ds2_cms_w* w = (ds2_cms_w*)native;
    if (!w)
        return;
    if (w->ps.map)
        dyn_cms_swap_counters(w->c, w->own);
    dyn_ds_pstate_free(&w->ps);
    dyn_cms_free(w->c);
    dyn_ds_free(w);
}

JSClassID ds2_cms_class_id;

DS2_FINALIZER(ds2_cms)

const JSClassDef ds2_cms_class = {
    "CountMinSketch",
    .finalizer = ds2_cms_finalizer,
};

JSValue ds2_cms_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    uint32_t width, depth = 5;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "width required");
    if (JS_ToUint32(ctx, &width, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &depth, argv[1]))
        return JS_EXCEPTION;
    if (width == 0 || depth == 0)
        return JS_ThrowRangeError(ctx, "width and depth must be > 0");
    if (depth > 64)
        return JS_ThrowRangeError(ctx, "depth must be <= 64");
    w = (ds2_cms_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    memset(w, 0, sizeof(*w));
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_cms_new(width, depth);
    if (!w->c) {
        dyn_ds_free(w);
        return JS_ThrowRangeError(ctx, "width * depth is too large");
    }
    return dyn_plain_wrap(ctx, new_target, ds2_cms_class_id, w, ds2_cms_free);
}

static JSValue ds2_cms_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    ds2_str k;
    int64_t n = 1;

    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx,
            "count must be >= 0: a sketch cannot be decremented");
    }
    dyn_cms_add(w->c, k.p, k.n, (uint64_t)n);
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_cms_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_cms_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, c);
}

static JSValue ds2_cms_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w *w, *o;
    (void)argc;
    o = (ds2_cms_w*)JS_GetOpaque2(ctx, argv[0], ds2_cms_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (w == o)
        return JS_ThrowTypeError(ctx, "cannot merge a sketch into itself");
    if (dyn_cms_merge(w->c, o->c) < 0)
        return JS_ThrowTypeError(ctx,
            "merge requires identical width and depth");
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_cms_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    ds2_cms_w* w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewInt64(ctx, (int64_t)dyn_cms_width(w->c));
    case 1:
        return JS_NewInt64(ctx, (int64_t)dyn_cms_depth(w->c));
    default:
        return ds2_u64_js(ctx, dyn_cms_total(w->c));
    }
}

const JSCFunctionListEntry ds2_cms_proto[] = {
    JS_CFUNC_DEF("add", 1, ds2_cms_add),
    JS_CFUNC_DEF("count", 1, ds2_cms_count),
    JS_CFUNC_DEF("merge", 1, ds2_cms_merge),
    JS_CGETSET_MAGIC_DEF("width", ds2_cms_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("depth", ds2_cms_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("totalCount", ds2_cms_getter, NULL, 2),
};

int ds2_cms_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_cms_w* o = (ds2_cms_w*)JS_GetOpaque(obj, ds2_cms_class_id);
    uint32_t width = dyn_cms_width(o->c), depth = dyn_cms_depth(o->c), i;
    const uint64_t* c = dyn_cms_counters(o->c);
    (void)ctx;
    (void)i;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, width) < 0 || dyn_ser_u32(w, depth) < 0 || dyn_ser_u64(w, dyn_cms_total(o->c)) < 0)
        return -1;
    return dyn_sketch_write_u64(w, c, (size_t)width * depth);
}

JSValue ds2_cms_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), width, depth, i, n;
    int ext = (first == DS_SKETCH_EXT);
    uint64_t total;
    dyn_cms_t* s;
    uint64_t* c;
    (void)opts;

    width = ext ? dyn_de_u32(r) : first;
    depth = dyn_de_u32(r);
    total = dyn_de_u64(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (width == 0 || depth == 0 || depth > 64 || (!ext && (uint64_t)width * depth * 8 > (uint64_t)dyn_de_left(r)))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if ((uint64_t)width * depth > DYN_DECODE_SKETCH_MAX_WORDS)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (ext && dyn_sketch_fits_u64(r, (size_t)width * depth) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, (uint64_t)width * depth * 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    s = dyn_cms_new(width, depth);
    if (!s)
        return ds2_oom(ctx);
    c = dyn_cms_counters_mut(s);
    n = width * depth;
    if (ext) {
        if (dyn_sketch_read_u64(r, c, n) < 0) {
            dyn_cms_free(s);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        for (i = 0; i < n; i++)
            c[i] = dyn_de_u64(r);
    }
    dyn_cms_set_total(s, total);
    DS2_WRAP(ds2_cms, dyn_cms_free(s), s);
}

static dyn_ds_pstate* ds2_cms_pstate(JSValueConst obj)
{
    ds2_cms_w* w = (ds2_cms_w*)JS_GetOpaque(obj, ds2_cms_class_id);
    return w ? &w->ps : NULL;
}

static int ds2_cms_sections(JSContext* ctx, JSValueConst obj, dyn_persist_w* pw)
{
    ds2_cms_w* w = (ds2_cms_w*)JS_GetOpaque(obj, ds2_cms_class_id);
    uint64_t len;
    (void)ctx;
    if (!w)
        return -1;
    len = (uint64_t)dyn_cms_width(w->c) * dyn_cms_depth(w->c) * 8u;
    if (dyn_ds_pstate_track(&w->ps, dyn_cms_counters(w->c), len) < 0)
        return -1;
    pw->count = dyn_cms_total(w->c);
    dyn_persist_w_param(pw, 0, dyn_cms_width(w->c));
    dyn_persist_w_param(pw, 1, dyn_cms_depth(w->c));
    dyn_persist_w_param(pw, 2, dyn_cms_total(w->c));
    return dyn_persist_w_ref_dirty(pw, DYN_PKIND_WORDS, 0, 8, DYN_PERSIST_S_PAGE,
        dyn_cms_counters(w->c), len, w->ps.dirty, 14);
}

static JSValue ds2_cms_load(JSContext* ctx, dyn_persist_r* r, int borrow)
{
    const dyn_persist_rsect* s = dyn_persist_find(r, DYN_PKIND_WORDS, 0);
    uint64_t width = r->params[0], depth = r->params[1];
    JSValue args[2], obj;
    ds2_cms_w* w;
    if (!s || width == 0 || depth == 0 || depth > 64 || width > ((uint64_t)1 << 24)
        || width * depth > ((uint64_t)1 << 24) || s->len != width * depth * 8 || s->elem != 8)
        return dyn_ds_persist_bad(ctx, "CountMinSketch layout");
    args[0] = JS_NewInt64(ctx, (int64_t)width);
    args[1] = JS_NewInt64(ctx, (int64_t)depth);
    obj = ds2_cms_ctor(ctx, JS_UNDEFINED, 2, (JSValueConst*)args);
    if (JS_IsException(obj))
        return obj;
    w = (ds2_cms_w*)JS_GetOpaque(obj, ds2_cms_class_id);
    if (borrow && (s->flags & DYN_PERSIST_S_PAGE)) {
        w->own = dyn_cms_swap_counters(w->c, (uint64_t*)DYN_UNCONST(s->p));
        dyn_persist_retain(r);
        w->ps.map = r;
    } else {
        memcpy(dyn_cms_counters_mut(w->c), s->p, (size_t)s->len);
    }
    dyn_cms_set_total(w->c, r->params[2]);
    return obj;
}

static void ds2_cms_committed(JSValueConst obj, int ok)
{
    dyn_ds_pstate_committed(ds2_cms_pstate(obj), ok);
}

const dyn_codec_persist_t ds2_cms_persist = {
    ds2_cms_sections,
    ds2_cms_pstate,
    ds2_cms_load,
    ds2_cms_committed,
};

#endif
