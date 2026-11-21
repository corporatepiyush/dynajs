#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DYN_DECODE_CHUNK_MAX_WORDS (1u << 22)
#define DS_BITSET_CHUNKED 0xFFFFFFFFu
#define DS_BITSET_CHUNKW 1024u
#define DS_BS_EMPTY 0
#define DS_BS_ARRAY 1
#define DS_BS_BITMAP 2
#define DS_BS_RUN 3

static void dsw_bitset_free(void* native)
{
    dsw_bitset_t* b = (dsw_bitset_t*)native;
    if (!b)
        return;
    dyn_bitset_free(b);
    dyn_ds_free(b);
}

JSClassID dsw_bitset_class_id;

DYN_FINALIZER(dsw_bitset)

const JSClassDef dsw_bitset_class = {
    "BitSet",
    .finalizer = dsw_bitset_finalizer,
};

#define DSW_BS_CC_ON 0x80000000u
#define DSW_BS_CC_DIRTY 0x7FFFFFFFu

static int dsw_bs_cache_opt(JSContext* ctx, JSValueConst opts, int* on)
{
    *on = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "BitSet: the options must be an object");
        return -1;
    }
    if (!dsx_opt_keys_ok(ctx, opts, "cache", "BitSet", "constructor"))
        return -1;
    return dsx_cache_opt(ctx, opts, "BitSet", "count", on);
}

static void dsw_bs_cc_add(dsw_bitset_t* b, int delta)
{
    if ((b->cc & DSW_BS_CC_ON) && (b->cc & DSW_BS_CC_DIRTY) != DSW_BS_CC_DIRTY)
        b->cc = (uint32_t)((int64_t)b->cc + delta);
}

JSValue dsw_bitset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t nbits = 0;
    int cache = 0;

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], 1u << 30, &nbits))
        return JS_EXCEPTION;
    if (dsw_bs_cache_opt(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED, &cache) < 0)
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_ds_malloc(sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    dyn_bitset_init(b);
    b->cc = cache ? DSW_BS_CC_ON : 0;
    if (nbits && dyn_bitset_reserve(b, (nbits - 1) >> 6)) {
        dsw_bitset_free(b);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_bitset_class_id, b, dsw_bitset_free);
}

static JSValue dsw_bitset_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if ((i >> 6) >= DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "bit index too large (max %u)",
            (unsigned)(DYN_MAX_CAPACITY << 6) - 1);
    {
        int was = dyn_bitset_get(b, i);
        if (dyn_bitset_set(b, i) < 0)
            return JS_ThrowOutOfMemory(ctx);
        if (!was)
            dsw_bs_cc_add(b, 1);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_bitset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if (dyn_bitset_get(b, i))
        dsw_bs_cc_add(b, -1);
    dyn_bitset_clear(b, i);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_bitset_flip(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if ((i >> 6) >= DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "bit index too large (max %u)",
            (unsigned)(DYN_MAX_CAPACITY << 6) - 1);
    {
        int was = dyn_bitset_get(b, i);
        if (dyn_bitset_flip(b, i) < 0)
            return JS_ThrowOutOfMemory(ctx);
        dsw_bs_cc_add(b, was ? -1 : 1);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_bitset_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t i;
    int bit;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    bit = dyn_bitset_get(b, i);
    return JS_NewBool(ctx, bit);
}

static JSValue dsw_bitset_count(JSContext* ctx, JSValueConst this_val)
{
    dsw_bitset_t* b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if (b->cc & DSW_BS_CC_ON) {
        if ((b->cc & DSW_BS_CC_DIRTY) == DSW_BS_CC_DIRTY)
            b->cc = DSW_BS_CC_ON | (uint32_t)dyn_bitset_count(b);
        return JS_NewInt64(ctx, (int64_t)(b->cc & DSW_BS_CC_DIRTY));
    }
    return JS_NewInt64(ctx, (int64_t)dyn_bitset_count(b));
}

static JSValue dsw_bitset_next_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    uint32_t from;
    (void)argc;
    if (JS_ToUint32(ctx, &from, argv[0]))
        return JS_EXCEPTION;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, dyn_bitset_next_set(b, from));
}

static JSValue dsw_bitset_bulk(JSContext* ctx, JSValueConst this_val,
    JSValueConst other_val, int op)
{
    dsw_bitset_t *a, *o;

    o = (dsw_bitset_t*)dyn_plain_get(ctx, other_val, dsw_bitset_class_id);
    if (!o)
        return JS_EXCEPTION;
    a = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!a)
        return JS_EXCEPTION;
    if (a->cc & DSW_BS_CC_ON)
        a->cc = DSW_BS_CC_ON | DSW_BS_CC_DIRTY;
    if (dyn_bitset_combine(a, o, op) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_bitset_and(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dsw_bitset_bulk(ctx, t, argv[0], 0);
}

static JSValue dsw_bitset_or(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dsw_bitset_bulk(ctx, t, argv[0], 1);
}

static JSValue dsw_bitset_xor(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dsw_bitset_bulk(ctx, t, argv[0], 2);
}

static JSValue dsw_bitset_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_bitset_t* b;
    JSValue arr;
    uint32_t wi, out = 0;
    (void)argc;
    (void)argv;
    b = (dsw_bitset_t*)dyn_plain_get(ctx, this_val, dsw_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (wi = 0; wi < b->nwords; wi++) {
        uint64_t w = b->words[wi];
        while (w) {
            int bit = __builtin_ctzll(w);
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    JS_NewInt64(ctx, (int64_t)wi * 64 + bit),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            w &= w - 1;
        }
    }
    return arr;
}

const JSCFunctionListEntry dsw_bitset_proto[] = {
    JS_CFUNC_DEF("set", 1, dsw_bitset_set),
    JS_CFUNC_DEF("clear", 1, dsw_bitset_clear),
    JS_CFUNC_DEF("flip", 1, dsw_bitset_flip),
    JS_CFUNC_DEF("get", 1, dsw_bitset_get),
    JS_CFUNC_DEF("nextSet", 1, dsw_bitset_next_set),
    JS_CFUNC_DEF("and", 1, dsw_bitset_and),
    JS_CFUNC_DEF("or", 1, dsw_bitset_or),
    JS_CFUNC_DEF("xor", 1, dsw_bitset_xor),
    JS_CFUNC_DEF("toArray", 0, dsw_bitset_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("count", dsw_bitset_count, NULL),
};

static void dyn_bs_chunk_stats(const uint64_t* w, uint32_t nw,
    uint32_t* bits, uint32_t* runs)
{
    uint32_t i, b = 0, r = 0;
    int prev = 0;
    for (i = 0; i < nw; i++) {
        uint64_t v = w[i];
        b += (uint32_t)__builtin_popcountll(v);
        r += (uint32_t)__builtin_popcountll(v & ~((v << 1) | (uint64_t)prev));
        prev = (int)(v >> 63);
    }
    *bits = b;
    *runs = r;
}

static int dyn_bs_write_chunk(dyn_ser_t* w, const uint64_t* words, uint32_t nw)
{
    uint32_t bits = 0, runs = 0, i;
    dyn_bs_chunk_stats(words, nw, &bits, &runs);

    if (bits == 0)
        return dyn_ser_u8(w, DS_BS_EMPTY);

    {
        size_t sz_arr = (size_t)bits * 2, sz_run = (size_t)runs * 4,
               sz_map = (size_t)nw * 8;
        if (sz_run <= sz_arr && sz_run < sz_map) {
            uint32_t start = 0, len = 0;
            if (dyn_ser_u8(w, DS_BS_RUN) < 0 || dyn_ser_u16(w, (uint16_t)runs) < 0)
                return -1;
            for (i = 0; i < nw; i++) {
                uint64_t v = words[i];
                uint32_t base = i * 64;
                if (v == 0) {
                    if (len) {
                        if (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0)
                            return -1;
                        len = 0;
                    }
                    continue;
                }
                if (v == ~(uint64_t)0) {
                    if (!len)
                        start = base;
                    len += 64;
                    continue;
                }
                {
                    uint32_t bit = 0;
                    while (bit < 64) {
                        uint64_t rest = v >> bit;
                        uint32_t skip;
                        if (!(rest & 1)) {
                            if (len) {
                                if (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0)
                                    return -1;
                                len = 0;
                            }
                            skip = rest ? (uint32_t)__builtin_ctzll(rest) : 64 - bit;
                        } else {
                            uint64_t inv = ~rest;
                            skip = inv ? (uint32_t)__builtin_ctzll(inv) : 64 - bit;
                            if (!len)
                                start = base + bit;
                            len += skip;
                        }
                        bit += skip;
                    }
                }
            }
            if (len && (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0))
                return -1;
            return 0;
        }
        if (sz_arr < sz_map) {
            if (dyn_ser_u8(w, DS_BS_ARRAY) < 0 || dyn_ser_u16(w, (uint16_t)bits) < 0)
                return -1;
            for (i = 0; i < nw; i++) {
                uint64_t v = words[i];
                while (v) {
                    uint32_t off = (uint32_t)(i * 64 + (uint32_t)__builtin_ctzll(v));
                    if (dyn_ser_u16(w, (uint16_t)off) < 0)
                        return -1;
                    v &= v - 1;
                }
            }
            return 0;
        }
    }
    if (dyn_ser_u8(w, DS_BS_BITMAP) < 0)
        return -1;
    for (i = 0; i < nw; i++)
        if (dyn_ser_u64(w, words[i]) < 0)
            return -1;
    return 0;
}

int dsw_bitset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_bitset_t* b = JS_GetOpaque(obj, dsw_bitset_class_id);
    uint32_t nchunks, c;
    (void)ctx;
    if (!b)
        return -1;

    nchunks = (b->nwords + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW;
    if (dyn_ser_u32(w, DS_BITSET_CHUNKED) < 0 || dyn_ser_u32(w, b->nwords) < 0 || dyn_ser_u32(w, nchunks) < 0)
        return -1;
    for (c = 0; c < nchunks; c++) {
        uint32_t off = c * DS_BITSET_CHUNKW;
        uint32_t nw = b->nwords - off;
        if (nw > DS_BITSET_CHUNKW)
            nw = DS_BITSET_CHUNKW;
        if (dyn_bs_write_chunk(w, b->words + off, nw) < 0)
            return -1;
    }
    return 0;
}

static int dyn_bs_read_chunk(dyn_de_t* r, uint64_t* words, uint32_t nw)
{
    uint32_t kind = dyn_de_u8(r), i, cnt;
    uint32_t maxbit = nw * 64;

    switch (kind) {
    case DS_BS_EMPTY:
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_ARRAY:
        cnt = dyn_de_u16(r);
        if (!dyn_de_ok(r) || (size_t)cnt * 2 > dyn_de_left(r))
            return -1;
        for (i = 0; i < cnt; i++) {
            uint32_t off = dyn_de_u16(r);
            if (off >= maxbit)
                return -1;
            words[off >> 6] |= (uint64_t)1 << (off & 63);
        }
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_RUN:
        cnt = dyn_de_u16(r);
        if (!dyn_de_ok(r) || (size_t)cnt * 4 > dyn_de_left(r))
            return -1;
        for (i = 0; i < cnt; i++) {
            uint32_t start = dyn_de_u16(r);
            uint32_t len = (uint32_t)dyn_de_u16(r) + 1;
            uint32_t j;
            if (start >= maxbit || len > maxbit - start)
                return -1;
            {
                uint32_t end = start + len;
                uint32_t wfirst = (start + 63) >> 6, wlast = end >> 6;
                for (j = start; j < end && (j & 63); j++)
                    words[j >> 6] |= (uint64_t)1 << (j & 63);
                for (j = wfirst; j < wlast; j++)
                    words[j] = ~(uint64_t)0;
                for (j = wlast * 64 > start ? wlast * 64 : start; j < end; j++)
                    words[j >> 6] |= (uint64_t)1 << (j & 63);
            }
        }
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_BITMAP:
        if ((size_t)nw * 8 > dyn_de_left(r))
            return -1;
        for (i = 0; i < nw; i++)
            words[i] = dyn_de_u64(r);
        return dyn_de_ok(r) ? 0 : -1;
    default:
        return -1;
    }
}

JSValue dsw_bitset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i, first;
    JSValue obj;
    dsw_bitset_t* b;
    int chunked;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    chunked = (first == DS_BITSET_CHUNKED);
    if (chunked) {
        uint32_t nchunks, want;
        n = dyn_de_u32(r);
        nchunks = dyn_de_u32(r);
        want = (n + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW;
        if (!dyn_de_ok(r) || n > DYN_MAX_CAPACITY || n > DYN_DECODE_CHUNK_MAX_WORDS || nchunks != want)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        if (dyn_de_charge(r, (uint64_t)n * 8) < 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = first;
        if ((size_t)n * 8 > dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }

    obj = dyn_ds_construct(ctx, dsw_bitset_class_id, 0, NULL);
    if (JS_IsException(obj))
        return obj;
    b = JS_GetOpaque(obj, dsw_bitset_class_id);
    if (n && dyn_bitset_reserve(b, n - 1) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (!chunked) {
        for (i = 0; i < n; i++)
            b->words[i] = dyn_de_u64(r);
        return obj;
    }
    {
        uint32_t nchunks = (n + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW, c;
        for (c = 0; c < nchunks; c++) {
            uint32_t off = c * DS_BITSET_CHUNKW;
            uint32_t nw = n - off;
            if (nw > DS_BITSET_CHUNKW)
                nw = DS_BITSET_CHUNKW;
            if (dyn_bs_read_chunk(r, b->words + off, nw) < 0) {
                JS_FreeValue(ctx, obj);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
        }
    }
    return obj;
}

#endif
