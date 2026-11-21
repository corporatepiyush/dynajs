#include "structures-internal.h"
#include "core/ds/ds-bloom-filter.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_bloom_t b;
    dyn_ds_pstate ps;
    uint64_t* own;
} dsw_bloom_t;

static void dsw_bloom_free(void* native)
{
    dsw_bloom_t* b = (dsw_bloom_t*)native;
    if (!b)
        return;
    if (b->ps.map)
        b->b.words = b->own;
    dyn_ds_pstate_free(&b->ps);
    dyn_bloom_free(&b->b);
    dyn_ds_free(b);
}

JSClassID dsw_bloom_class_id;

DYN_FINALIZER(dsw_bloom)

const JSClassDef dsw_bloom_class = {
    "BloomFilter",
    .finalizer = dsw_bloom_finalizer,
};

JSValue dsw_bloom_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_bloom_t* b;
    uint32_t bits, k = 3;
    if (argc < 1 || dyn_size_arg(ctx, argv[0], 1u << 30, &bits))
        return argc < 1 ? JS_ThrowTypeError(ctx, "bits required") : JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (bits == 0)
        return JS_ThrowRangeError(ctx, "bits must be > 0");
    if (k == 0)
        k = 1;
    if (k > 32)
        k = 32;
    b = (dsw_bloom_t*)dyn_ds_malloc(sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    memset(b, 0, sizeof(*b));
    if (dyn_bloom_init(&b->b, bits, k) < 0) {
        dyn_ds_free(b);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_bloom_class_id, b, dsw_bloom_free);
}

static JSValue dsw_bloom_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bloom_t* b;
    const char* key;
    size_t klen;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    b = (dsw_bloom_t*)dyn_plain_get(ctx, this_val, dsw_bloom_class_id);
    if (!b) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    dyn_bloom_add(&b->b, key, klen);
    JS_FreeCString(ctx, key);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_bloom_may_contain(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bloom_t* b;
    const char* key;
    size_t klen;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    b = (dsw_bloom_t*)dyn_plain_get(ctx, this_val, dsw_bloom_class_id);
    if (!b) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    {
        int r = dyn_bloom_may_contain(&b->b, key, klen);
        JS_FreeCString(ctx, key);
        return JS_NewBool(ctx, r);
    }
}

static JSValue dsw_bloom_bits(JSContext* ctx, JSValueConst this_val)
{
    dsw_bloom_t* b = (dsw_bloom_t*)dyn_plain_get(ctx, this_val, dsw_bloom_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)b->b.nbits);
}

static JSValue dsw_bloom_hashes_getter(JSContext* ctx, JSValueConst this_val)
{
    dsw_bloom_t* b = (dsw_bloom_t*)dyn_plain_get(ctx, this_val, dsw_bloom_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)b->b.k);
}

const JSCFunctionListEntry dsw_bloom_proto[] = {
    JS_CFUNC_DEF("add", 1, dsw_bloom_add),
    JS_CFUNC_DEF("mayContain", 1, dsw_bloom_may_contain),
    JS_CGETSET_DEF("bits", dsw_bloom_bits, NULL),
    JS_CGETSET_DEF("hashes", dsw_bloom_hashes_getter, NULL),
};

int dsw_bloom_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_bloom_t* b = JS_GetOpaque(obj, dsw_bloom_class_id);
    uint32_t i;
    (void)ctx;
    (void)i;
    if (!b)
        return -1;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, b->b.nbits) < 0 || dyn_ser_u32(w, b->b.k) < 0)
        return -1;
    return dyn_sketch_write_u64(w, b->b.words, b->b.nwords);
}

JSValue dsw_bloom_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t nbits, k, i, first;
    uint64_t nwords;
    int ext;
    JSValue obj, args[2];
    dsw_bloom_t* b;
    (void)opts;

    first = dyn_de_u32(r);
    ext = (first == DS_SKETCH_EXT);
    nbits = ext ? dyn_de_u32(r) : first;
    k = dyn_de_u32(r);
    if (!dyn_de_ok(r) || nbits == 0 || k == 0 || k > 32)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    nwords = ((uint64_t)nbits + 63) >> 6;
    if (nwords > DYN_DECODE_SKETCH_MAX_WORDS)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (!ext && nwords * 8 > (uint64_t)dyn_de_left(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (ext && dyn_sketch_fits_u64(r, (size_t)nwords) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, nwords * 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    args[0] = JS_NewInt64(ctx, (int64_t)nbits);
    args[1] = JS_NewInt64(ctx, (int64_t)k);
    obj = dyn_ds_construct(ctx, dsw_bloom_class_id, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(obj))
        return obj;
    b = JS_GetOpaque(obj, dsw_bloom_class_id);
    if (ext) {
        if (dyn_sketch_read_u64(r, b->b.words, (size_t)nwords) < 0) {
            JS_FreeValue(ctx, obj);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        for (i = 0; i < nwords; i++)
            b->b.words[i] = dyn_de_u64(r);
    }
    return obj;
}

static dyn_ds_pstate* dsw_bloom_pstate(JSValueConst obj)
{
    dsw_bloom_t* b = (dsw_bloom_t*)JS_GetOpaque(obj, dsw_bloom_class_id);
    return b ? &b->ps : NULL;
}

static int dsw_bloom_sections(JSContext* ctx, JSValueConst obj, dyn_persist_w* pw)
{
    dsw_bloom_t* b = (dsw_bloom_t*)JS_GetOpaque(obj, dsw_bloom_class_id);
    uint64_t len;
    (void)ctx;
    if (!b)
        return -1;
    len = (uint64_t)b->b.nwords * 8u;
    if (dyn_ds_pstate_track(&b->ps, b->b.words, len) < 0)
        return -1;
    pw->count = b->b.nbits;
    dyn_persist_w_param(pw, 0, b->b.nbits);
    dyn_persist_w_param(pw, 1, b->b.k);
    return dyn_persist_w_ref_dirty(pw, DYN_PKIND_WORDS, 0, 8, DYN_PERSIST_S_PAGE,
        b->b.words, len, b->ps.dirty, 14);
}

static JSValue dsw_bloom_load(JSContext* ctx, dyn_persist_r* r, int borrow)
{
    const dyn_persist_rsect* s = dyn_persist_find(r, DYN_PKIND_WORDS, 0);
    uint64_t nbits = r->params[0], k = r->params[1];
    JSValue args[2], obj;
    dsw_bloom_t* b;
    if (!s || nbits == 0 || nbits > ((uint64_t)1 << 30) || k == 0 || k > 32
        || s->len != ((nbits + 63) >> 6) * 8 || s->elem != 8)
        return dyn_ds_persist_bad(ctx, "BloomFilter layout");
    args[0] = JS_NewInt64(ctx, (int64_t)nbits);
    args[1] = JS_NewInt64(ctx, (int64_t)k);
    obj = dyn_ds_construct(ctx, dsw_bloom_class_id, 2, (JSValueConst*)args);
    if (JS_IsException(obj))
        return obj;
    b = (dsw_bloom_t*)JS_GetOpaque(obj, dsw_bloom_class_id);
    if (borrow && (s->flags & DYN_PERSIST_S_PAGE)) {
        b->own = b->b.words;
        b->b.words = (uint64_t*)DYN_UNCONST(s->p);
        dyn_persist_retain(r);
        b->ps.map = r;
    } else {
        memcpy(b->b.words, s->p, (size_t)s->len);
    }
    return obj;
}

static void dsw_bloom_committed(JSValueConst obj, int ok)
{
    dyn_ds_pstate_committed(dsw_bloom_pstate(obj), ok);
}

const dyn_codec_persist_t dsw_bloom_persist = {
    dsw_bloom_sections,
    dsw_bloom_pstate,
    dsw_bloom_load,
    dsw_bloom_committed,
};

#endif
