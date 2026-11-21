#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

static int dyn_sketch_write_u8(dyn_ser_t* w, const uint8_t* v, size_t n)
{
    size_t i, rle = 0;

    for (i = 0; i < n;) {
        size_t j = i;
        while (j < n && v[j] == v[i])
            j++;
        rle += dyn_varint_len(j - i) + 1;
        i = j;
    }
    if (rle >= n) {
        if (dyn_ser_u8(w, DS_SKETCH_RAW) < 0)
            return -1;
        return dyn_ser_raw(w, v, n);
    }
    if (dyn_ser_u8(w, DS_SKETCH_ALT) < 0)
        return -1;
    for (i = 0; i < n;) {
        size_t j = i;
        while (j < n && v[j] == v[i])
            j++;
        if (dyn_ser_uvarint(w, j - i) < 0 || dyn_ser_u8(w, v[i]) < 0)
            return -1;
        i = j;
    }
    return 0;
}

static int dyn_sketch_read_u8(dyn_de_t* r, uint8_t* v, size_t n)
{
    uint8_t kind = dyn_de_u8(r);
    size_t at = 0;

    if (!dyn_de_ok(r) || kind > DS_SKETCH_ALT)
        return -1;
    if (kind == DS_SKETCH_RAW) {
        const uint8_t* p = dyn_de_raw(r, n);
        if (!p)
            return -1;
        memcpy(v, p, n);
        return 0;
    }
    while (at < n) {
        uint64_t run;
        uint8_t val;
        if (dyn_de_uvarint(r, &run) < 0)
            return -1;
        val = dyn_de_u8(r);
        if (!dyn_de_ok(r) || !run || run > (uint64_t)(n - at))
            return -1;
        memset(v + at, val, (size_t)run);
        at += (size_t)run;
    }
    return 0;
}

static void ds2_hll_free(void* native)
{
    ds2_hll_w* w = (ds2_hll_w*)native;
    if (!w)
        return;
    if (w->ps.map)
        dyn_hll_swap_regs(w->c, w->own);
    dyn_ds_pstate_free(&w->ps);
    dyn_hll_free(w->c);
    dyn_ds_free(w);
}

JSClassID ds2_hll_class_id;

DS2_FINALIZER(ds2_hll)

const JSClassDef ds2_hll_class = {
    "HyperLogLog",
    .finalizer = ds2_hll_finalizer,
};

JSValue ds2_hll_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w;
    uint32_t p = 14;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToUint32(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    if (p < DYN_HLL_MIN_PRECISION || p > DYN_HLL_MAX_PRECISION)
        return JS_ThrowRangeError(ctx, "precision must be %d..%d",
            DYN_HLL_MIN_PRECISION,
            DYN_HLL_MAX_PRECISION);
    w = (ds2_hll_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    memset(w, 0, sizeof(*w));
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_hll_new(p);
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_hll_class_id, w, ds2_hll_free);
}

static JSValue ds2_hll_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w;
    ds2_str k;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    dyn_hll_add(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_hll_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_hll_count(w->c));
}

static JSValue ds2_hll_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w *w, *o;
    (void)argc;
    o = (ds2_hll_w*)JS_GetOpaque2(ctx, argv[0], ds2_hll_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (w == o)
        return JS_DupValue(ctx, this_val);
    if (dyn_hll_merge(w->c, o->c) < 0)
        return JS_ThrowTypeError(ctx, "merge requires equal precision");
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_hll_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    ds2_hll_w* w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic ? (int64_t)dyn_hll_registers(w->c) : (int64_t)dyn_hll_precision(w->c));
}

const JSCFunctionListEntry ds2_hll_proto[] = {
    JS_CFUNC_DEF("add", 1, ds2_hll_add),
    JS_CFUNC_DEF("count", 0, ds2_hll_count),
    JS_CFUNC_DEF("merge", 1, ds2_hll_merge),
    JS_CGETSET_MAGIC_DEF("precision", ds2_hll_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("registers", ds2_hll_getter, NULL, 1),
};

int ds2_hll_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_hll_w* o = (ds2_hll_w*)JS_GetOpaque(obj, ds2_hll_class_id);
    (void)ctx;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, dyn_hll_precision(o->c)) < 0)
        return -1;
    return dyn_sketch_write_u8(w, dyn_hll_data(o->c),
        dyn_hll_registers(o->c));
}

JSValue ds2_hll_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), p;
    int ext;
    size_t n = 0;
    const char* data;
    dyn_hll_t* h;
    (void)opts;

    ext = (first == DS_SKETCH_EXT);
    p = ext ? dyn_de_u32(r) : first;
    if (!dyn_de_ok(r) || p < DYN_HLL_MIN_PRECISION || p > DYN_HLL_MAX_PRECISION)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, (uint64_t)1 << p) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    h = dyn_hll_new(p);
    if (!h)
        return ds2_oom(ctx);
    if (ext) {
        if (dyn_sketch_read_u8(r, dyn_hll_data_mut(h), (size_t)1 << p) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (dyn_hll_regs_valid(h) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        data = dyn_de_blob(r, &n);
        if (!dyn_de_ok(r) || n != (size_t)1 << p) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        memcpy(dyn_hll_data_mut(h), data, n);
        if (dyn_hll_regs_valid(h) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    DS2_WRAP(ds2_hll, dyn_hll_free(h), h);
}

static dyn_ds_pstate* ds2_hll_pstate(JSValueConst obj)
{
    ds2_hll_w* w = (ds2_hll_w*)JS_GetOpaque(obj, ds2_hll_class_id);
    return w ? &w->ps : NULL;
}

static int ds2_hll_sections(JSContext* ctx, JSValueConst obj, dyn_persist_w* pw)
{
    ds2_hll_w* w = (ds2_hll_w*)JS_GetOpaque(obj, ds2_hll_class_id);
    uint64_t len;
    (void)ctx;
    if (!w)
        return -1;
    len = dyn_hll_registers(w->c);
    if (dyn_ds_pstate_track(&w->ps, dyn_hll_data(w->c), len) < 0)
        return -1;
    pw->count = len;
    dyn_persist_w_param(pw, 0, dyn_hll_precision(w->c));
    return dyn_persist_w_ref_dirty(pw, DYN_PKIND_WORDS, 0, 1, DYN_PERSIST_S_PAGE,
        dyn_hll_data(w->c), len, w->ps.dirty, 14);
}

static JSValue ds2_hll_load(JSContext* ctx, dyn_persist_r* r, int borrow)
{
    const dyn_persist_rsect* s = dyn_persist_find(r, DYN_PKIND_WORDS, 0);
    uint64_t p = r->params[0];
    JSValue arg, obj;
    ds2_hll_w* w;
    if (!s || p < DYN_HLL_MIN_PRECISION || p > DYN_HLL_MAX_PRECISION || s->len != ((uint64_t)1 << p) || s->elem != 1)
        return dyn_ds_persist_bad(ctx, "HyperLogLog layout");
    arg = JS_NewInt64(ctx, (int64_t)p);
    obj = ds2_hll_ctor(ctx, JS_UNDEFINED, 1, (JSValueConst*)&arg);
    if (JS_IsException(obj))
        return obj;
    w = (ds2_hll_w*)JS_GetOpaque(obj, ds2_hll_class_id);
    if (borrow && (s->flags & DYN_PERSIST_S_PAGE)) {
        w->own = dyn_hll_swap_regs(w->c, (uint8_t*)DYN_UNCONST(s->p));
        dyn_persist_retain(r);
        w->ps.map = r;
    } else {
        memcpy(dyn_hll_data_mut(w->c), s->p, (size_t)s->len);
    }
    if (dyn_hll_regs_valid(w->c) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_ds_persist_bad(ctx, "HyperLogLog register out of range");
    }
    return obj;
}

static void ds2_hll_committed(JSValueConst obj, int ok)
{
    dyn_ds_pstate_committed(ds2_hll_pstate(obj), ok);
}

const dyn_codec_persist_t ds2_hll_persist = {
    ds2_hll_sections,
    ds2_hll_pstate,
    ds2_hll_load,
    ds2_hll_committed,
};

#endif
