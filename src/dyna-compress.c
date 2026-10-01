#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-compress.h"
#include "core/dyn-dict.h"
#include "core/dyn-hash.h"

#if defined(CONFIG_ZSTD)
#include <zstd.h>
#endif
#if defined(__APPLE__)
#include <compression.h>
#elif defined(CONFIG_BROTLI)
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif
#include "core/dyn-snappy.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_read_input(JSContext* ctx, JSValueConst val, uint8_t** pout,
    size_t* plen)
{
    uint8_t *base, *copy;
    size_t off = 0, tlen = 0, ab = 0;
    JSValue buf;

    if (JS_IsString(val)) {
        size_t slen;
        const char* str = JS_ToCStringLen(ctx, &slen, val);
        if (!str)
            return -1;
        copy = (uint8_t*)malloc(slen ? slen : 1);
        if (!copy) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (slen)
            memcpy(copy, str, slen);
        JS_FreeCString(ctx, str);
        *pout = copy;
        *plen = slen;
        return 0;
    }

    buf = JS_GetTypedArrayBuffer(ctx, val, &off, &tlen, NULL);
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        if (!base) {
            JS_FreeValue(ctx, buf);
            return -1;
        }
        if (off > ab)
            off = ab;
        if (tlen > ab - off)
            tlen = ab - off;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, val);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "dyna:compress: input must be a string, "
                                   "Uint8Array, or ArrayBuffer");
            return -1;
        }
        buf = JS_UNDEFINED;
        off = 0;
        tlen = ab;
    }

    copy = (uint8_t*)malloc(tlen ? tlen : 1);
    if (!copy) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (tlen)
        memcpy(copy, base + off, tlen);
    JS_FreeValue(ctx, buf);
    *pout = copy;
    *plen = tlen;
    return 0;
}

static JSValue dyn_bytes_to_uint8(JSContext* ctx, const uint8_t* data, size_t len)
{
    static const uint8_t empty = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &empty, len);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const cpr_level_keys[] = { "level" };
static const char* const cpr_asstring_keys[] = { "asString" };
static const char* const cpr_lz4_keys[] = { "level", "dict" };
static const char* const cpr_frame_keys[] = { "checksum", "level", "dict" };
static const char* const cpr_algo_keys[] = { "algo" };
static const char* const cpr_ctor_keys[] = { "algo", "checksum", "level", "dict" };

static JSValue dyn_gzip(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;
    int level = 1;

    (void)this_val;

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSValueConst lv_arg = argv[1];
        JSValue prop;
        int32_t lv;
        if (JS_IsObject(argv[1])) {
            if (dyn_opts_strict(ctx, argv[1], cpr_level_keys, 1))
                return JS_EXCEPTION;
            prop = JS_GetPropertyStr(ctx, argv[1], "level");
            if (JS_IsException(prop))
                return JS_EXCEPTION;
            lv_arg = prop;
        } else {
            prop = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(lv_arg)) {
            if (!JS_IsNumber(lv_arg)) {
                JS_FreeValue(ctx, prop);
                return JS_ThrowTypeError(ctx, "gzip: level must be a number");
            }
            if (JS_ToInt32(ctx, &lv, lv_arg)) {
                JS_FreeValue(ctx, prop);
                return JS_EXCEPTION;
            }
            level = lv;
        }
        JS_FreeValue(ctx, prop);
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gzip_build_ctx(src, src_len, level, NULL, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_gunzip(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gunzip_decode(src, src_len, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress gunzip: invalid gzip data");
    }
    free(src);

    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static int dyn_lz4_opts(JSContext* ctx, JSValueConst opts, int level_hi,
    int* plevel, uint8_t** pdict, size_t* pdict_len,
    int clamp_level)
{
    *plevel = 1;
    *pdict = NULL;
    *pdict_len = 0;
    if (!JS_IsObject(opts))
        return 0;
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "level");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (lv < 1 || lv > level_hi) {
                if (clamp_level)
                    lv = lv < 1 ? 1 : level_hi;
                else {
                    JS_FreeValue(ctx, v);
                    JS_ThrowRangeError(ctx, "level must be 1..%d", level_hi);
                    return -1;
                }
            }
            *plevel = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "dict");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (dyn_read_input(ctx, v, pdict, pdict_len) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static JSValue dyn_lz4_compress_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *src = NULL, *dict = NULL, *out = NULL;
    size_t src_len = 0, dict_len = 0, out_len = 0;
    int level;
    JSValue result;

    (void)this_val;
    if (argc > 1 && dyn_opts_strict(ctx, argv[1], cpr_lz4_keys, 2))
        return JS_EXCEPTION;
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 12, &level, &dict,
            &dict_len, 1)
        < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (dyn_lz4_compress(src, src_len, dict, dict_len, level, NULL,
            &out, &out_len)
        < 0) {
        free(src);
        free(dict);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    free(dict);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_lz4_decompress_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *src = NULL, *dict = NULL;
    size_t src_len = 0, dict_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int level;
    JSValue result;

    (void)this_val;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
        && !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "dyna:compress lz4Decompress: the second argument is an options "
            "object { level, dict }, not a number (audit C3)");
    if (argc > 1 && dyn_opts_strict(ctx, argv[1], cpr_lz4_keys, 2))
        return JS_EXCEPTION;
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 12, &level, &dict,
            &dict_len, 0)
        < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (dyn_lz4_decompress(src, src_len, dict, dict_len, &o) < 0) {
        free(src);
        free(dict);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress lz4Decompress: invalid LZ4 block");
    }
    free(src);
    free(dict);
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_lz4_frame_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *src = NULL, *dict = NULL, *out = NULL;
    size_t src_len = 0, dict_len = 0, out_len = 0;
    int level, checksum = 1;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_frame_keys, 3))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "checksum");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            checksum = JS_ToBool(ctx, v);
            if (checksum < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 12, &level, &dict,
            &dict_len, 0)
        < 0)
        return JS_EXCEPTION;
    free(dict);
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_lz4_frame_build(src, src_len, level, checksum, NULL,
            &out, &out_len)
        < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_lz4_unframe_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    {
        int rc = dyn_lz4_frame_decode(src, src_len, &o);
        if (rc < 0) {
            free(src);
            free(o.buf);
            return JS_ThrowTypeError(ctx, "dyna:compress lz4Unframe: %s",
                dyn_lz4_frame_reason(rc));
        }
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

#if defined(CONFIG_ZSTD)
#define DYN_HAVE_ZSTD 1
#else
#define DYN_HAVE_ZSTD 0
#endif
#if defined(__APPLE__) || defined(CONFIG_BROTLI)
#define DYN_HAVE_BROTLI 1
#else
#define DYN_HAVE_BROTLI 0
#endif

static int dyn_out_grow(dyn_outbuf_t* o, size_t need, size_t cap)
{
    size_t nc;
    uint8_t* nb;
    if (need > cap)
        return -1;
    if (need <= o->cap)
        return 0;
    nc = o->cap ? o->cap * 2 : 65536;
    if (nc > cap)
        nc = cap;
    if (nc < need)
        nc = need;
    nb = (uint8_t*)realloc(o->buf, nc);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = nc;
    return 0;
}

#if DYN_HAVE_ZSTD
static int dyn_zstd_frame_bounds(const uint8_t* src, size_t len, size_t cap,
    uint64_t* pwin, uint64_t* pfcs,
    const char** pwhy)
{
    uint64_t fcs;
    size_t p;
    int fsz, single, fcs_flag, did_flag;
    uint8_t desc;

    if (len < 6) {
        *pwhy = "truncated frame";
        return -1;
    }
    {
        uint32_t magic = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        if (magic != 0xFD2FB528u) {
            *pwhy = "bad magic";
            return -1;
        }
    }
    desc = src[4];
    fcs_flag = desc >> 6;
    single = (desc >> 5) & 1;
    did_flag = desc & 3;
    p = 5;
    if (single) {
        *pwin = 0;
    } else {
        uint64_t window;
        int expo, mant;
        if (p >= len) {
            *pwhy = "truncated frame";
            return -1;
        }
        {
            uint8_t wd = src[p++];
            expo = wd >> 3;
            mant = wd & 7;
        }
        window = (uint64_t)1 << (10 + expo);
        window += (window / 8) * (uint64_t)mant;
        if (window > cap) {
            *pwhy = "oversized window";
            return -1;
        }
        *pwin = window;
    }
    if (did_flag == 1) {
        if (p + 1 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 1;
    } else if (did_flag == 2) {
        if (p + 2 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 2;
    } else if (did_flag == 3) {
        if (p + 4 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 4;
    }
    if (fcs_flag == 0)
        fsz = single ? 1 : 0;
    else if (fcs_flag == 1)
        fsz = 2;
    else if (fcs_flag == 2)
        fsz = 4;
    else
        fsz = 8;
    fcs = 0;
    if (fsz) {
        int i;
        if (p + (size_t)fsz > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        for (i = 0; i < fsz; i++)
            fcs |= (uint64_t)src[p + i] << (8 * i);
        if (fsz == 2)
            fcs += 256;
        if (fcs > cap) {
            *pwhy = "declared output exceeds cap";
            return -1;
        }
    }
    *pfcs = fcs;
    return 0;
}

static int dyn_zstd_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    ZSTD_DStream* ds;
    ZSTD_inBuffer in;
    uint64_t win, fcs2;
    int err = 0;

    if (dyn_zstd_frame_bounds(src, len, cap, &win, &fcs2, pwhy) < 0)
        return -1;
    (void)win;
    (void)fcs2;
    ds = ZSTD_createDStream();
    if (!ds) {
        *pwhy = "out of memory";
        return -1;
    }
    ZSTD_initDStream(ds);
    in.src = src;
    in.size = len;
    in.pos = 0;
    for (;;) {
        ZSTD_outBuffer out;
        size_t r;
        if (o->len == o->cap) {
            if (dyn_out_grow(o, o->len + 65536, cap) < 0) {
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
        }
        out.dst = o->buf + o->len;
        out.size = o->cap - o->len;
        out.pos = 0;
        r = ZSTD_decompressStream(ds, &out, &in);
        o->len += out.pos;
        if (ZSTD_isError(r)) {
            *pwhy = ZSTD_getErrorName(r);
            err = 1;
            break;
        }
        if (r == 0)
            break;
        if (in.pos == in.size && out.pos == 0) {
            *pwhy = "truncated frame";
            err = 1;
            break;
        }
    }
    ZSTD_freeDStream(ds);
    return err ? -1 : 0;
}

static int dyn_zstd_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
    size_t bound = ZSTD_compressBound(len);
    uint8_t* out;
    size_t r;

    out = (uint8_t*)malloc(bound);
    if (!out)
        return -1;
    r = ZSTD_compress(out, bound, src, len, level);
    if (ZSTD_isError(r)) {
        free(out);
        return -1;
    }
    *pout = out;
    *pout_len = r;
    return 0;
}
#endif

#if DYN_HAVE_BROTLI
static int dyn_brotli_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
#if defined(__APPLE__)
    compression_stream s;
    uint8_t chunk[65536];
    int err = 0;

    if (compression_stream_init(&s, COMPRESSION_STREAM_DECODE,
            COMPRESSION_BROTLI)
        != COMPRESSION_STATUS_OK) {
        *pwhy = "brotli init failed";
        return -1;
    }
    s.src_ptr = src;
    s.src_size = len;
    for (;;) {
        size_t produced;
        compression_status st;
        s.dst_ptr = chunk;
        s.dst_size = sizeof chunk;
        st = compression_stream_process(&s, COMPRESSION_STREAM_FINALIZE);
        produced = sizeof chunk - s.dst_size;
        if (produced) {
            if (dyn_out_grow(o, o->len + produced, cap) < 0) {
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
            memcpy(o->buf + o->len, chunk, produced);
            o->len += produced;
        }
        if (st == COMPRESSION_STATUS_END)
            break;
        if (st == COMPRESSION_STATUS_ERROR) {
            *pwhy = "malformed brotli stream";
            err = 1;
            break;
        }
        if (st == COMPRESSION_STATUS_OK && s.src_size == 0 && s.dst_size != 0) {
            *pwhy = "truncated brotli stream";
            err = 1;
            break;
        }
    }
    compression_stream_destroy(&s);
    return err ? -1 : 0;
#else
    BrotliDecoderState* st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    const uint8_t* in = src;
    size_t avail_in = len;
    int err = 0;

    if (!st) {
        *pwhy = "out of memory";
        return -1;
    }
    for (;;) {
        uint8_t* dst;
        size_t avail_out;
        BrotliDecoderResult r;
        if (o->len == o->cap) {
            if (dyn_out_grow(o, o->len + 65536, cap) < 0) {
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
        }
        dst = o->buf + o->len;
        avail_out = o->cap - o->len;
        r = BrotliDecoderDecompressStream(st, &avail_in, &in, &avail_out,
            &dst, NULL);
        o->len = (size_t)(dst - o->buf);
        if (r == BROTLI_DECODER_RESULT_SUCCESS)
            break;
        if (r == BROTLI_DECODER_RESULT_ERROR) {
            *pwhy = "malformed brotli stream";
            err = 1;
            break;
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0) {
            *pwhy = "truncated brotli stream";
            err = 1;
            break;
        }
    }
    BrotliDecoderDestroyInstance(st);
    return err ? -1 : 0;
#endif
}

static int dyn_brotli_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
#if defined(__APPLE__)
    compression_stream s;
    uint8_t chunk[65536];
    dyn_outbuf_t o = { NULL, 0, 0 };
    int err = 0;

    (void)level;
    if (compression_stream_init(&s, COMPRESSION_STREAM_ENCODE,
            COMPRESSION_BROTLI)
        != COMPRESSION_STATUS_OK)
        return -1;
    s.src_ptr = src;
    s.src_size = len;
    for (;;) {
        size_t produced;
        compression_status st;
        s.dst_ptr = chunk;
        s.dst_size = sizeof chunk;
        st = compression_stream_process(&s, COMPRESSION_STREAM_FINALIZE);
        produced = sizeof chunk - s.dst_size;
        if (produced) {
            if (dyn_out_grow(&o, o.len + produced, (size_t)-1) < 0) {
                err = 1;
                break;
            }
            memcpy(o.buf + o.len, chunk, produced);
            o.len += produced;
        }
        if (st == COMPRESSION_STATUS_END)
            break;
        if (st == COMPRESSION_STATUS_ERROR) {
            err = 1;
            break;
        }
    }
    compression_stream_destroy(&s);
    if (err) {
        free(o.buf);
        return -1;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
#else
    size_t bound = BrotliEncoderMaxCompressedSize(len);
    uint8_t* out;
    size_t olen;

    if (bound == 0)
        bound = 1;
    out = (uint8_t*)malloc(bound);
    if (!out)
        return -1;
    olen = bound;
    if (!BrotliEncoderCompress(level, BROTLI_DEFAULT_WINDOW,
            BROTLI_MODE_GENERIC, len, src, &olen, out)) {
        free(out);
        return -1;
    }
    *pout = out;
    *pout_len = olen;
    return 0;
#endif
}
#endif

#if !DYN_HAVE_ZSTD
static int dyn_zstd_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
    (void)src;
    (void)len;
    (void)level;
    (void)pout;
    (void)pout_len;
    return -1;
}
static int dyn_zstd_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    (void)src;
    (void)len;
    (void)cap;
    (void)o;
    *pwhy = "zstd support not compiled in";
    return -1;
}
#endif

#if !DYN_HAVE_BROTLI
static int dyn_brotli_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
    (void)src;
    (void)len;
    (void)level;
    (void)pout;
    (void)pout_len;
    return -1;
}
static int dyn_brotli_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    (void)src;
    (void)len;
    (void)cap;
    (void)o;
    *pwhy = "brotli support not compiled in";
    return -1;
}
#endif

static int dyn_codec_level(JSContext* ctx, JSValueConst opts, int algo,
    int* plevel)
{
    int lo, hi, def;
    if (algo == 0) {
        lo = 1;
        hi = 9;
        def = 1;
    } else if (algo == 3) {
        lo = 1;
        hi = 22;
        def = 3;
    } else if (algo == 4) {
        lo = 0;
        hi = 11;
        def = 5;
    } else {
        *plevel = 1;
        return 0;
    }
    *plevel = def;
    if (!JS_IsObject(opts))
        return 0;
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "level");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (lv < lo || lv > hi) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "level must be %d..%d", lo, hi);
                return -1;
            }
            *plevel = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static JSValue dyn_zstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_ZSTD
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 3;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_level_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (lv < 1 || lv > 22) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "zstd level must be 1..22");
            }
            level = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_zstd_encode(src, src_len, level, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
#else
    return JS_ThrowTypeError(ctx,
        "dyna:compress zstd: support not compiled in (build with libzstd)");
#endif
}

static JSValue dyn_unzstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_ZSTD
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    const char* why = NULL;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_zstd_decode(src, src_len, DYN_MAX_OUTPUT, &o, &why) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress unzstd: zstd: %s",
            why ? why : "invalid zstd data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress zstd: support not compiled in (build with libzstd)");
#endif
}

static JSValue dyn_brotli_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_BROTLI
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 5;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_level_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (lv < 0 || lv > 11) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "brotli level must be 0..11");
            }
            level = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_brotli_encode(src, src_len, level, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress brotli: support not compiled in");
#endif
}

static JSValue dyn_unbrotli_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_BROTLI
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    const char* why = NULL;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_brotli_decode(src, src_len, DYN_MAX_OUTPUT, &o, &why) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress unbrotli: %s",
            why ? why : "invalid brotli data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress brotli: support not compiled in");
#endif
}

static JSValue dyn_snappy_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;

    (void)this_val;
    (void)argc;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_snappy_compress(src, src_len, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_unsnappy_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_snappy_decompress(src, src_len, DYN_MAX_OUTPUT, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx,
            "dyna:compress unsnappy: invalid snappy data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

typedef struct {
    dyn_comp_ctx_t cx;
    uint8_t* dict;
    size_t dict_len;
    uint32_t dict_id;
    int level;
    int algo;
    int checksum;
} dyn_compressor_t;

static JSClassID dyn_compressor_class_id;

static void dyn_compressor_dispose(void* native)
{
    dyn_compressor_t* c = (dyn_compressor_t*)native;
    if (!c)
        return;
    dyn_comp_ctx_free(&c->cx);
    free(c->dict);
    free(c);
}

static const JSClassDef dyn_compressor_class = {
    "Compressor", .finalizer = dyn_res_finalizer
};

static JSValue dyn_compressor_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_compressor_t* c;
    uint8_t* dict = NULL;
    size_t dict_len = 0;
    int level = 1, algo = 1, checksum = 1;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        if (dyn_opts_strict(ctx, argv[0], cpr_ctor_keys, 4))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "algo");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* name = JS_ToCString(ctx, v);
            if (!name) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!strcmp(name, "gzip"))
                algo = 0;
            else if (!strcmp(name, "lz4"))
                algo = 1;
            else if (!strcmp(name, "lz4frame"))
                algo = 2;
            else if (!strcmp(name, "zstd"))
                algo = 3;
            else if (!strcmp(name, "brotli"))
                algo = 4;
            else if (!strcmp(name, "snappy"))
                algo = 5;
            else {
                JS_FreeCString(ctx, name);
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "algo must be \"gzip\", \"lz4\", \"lz4frame\", \"zstd\", \"brotli\" or \"snappy\"");
            }
            JS_FreeCString(ctx, name);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "checksum");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            checksum = JS_ToBool(ctx, v);
            if (checksum < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        if (algo == 3 || algo == 4 || algo == 5) {
            if (dyn_codec_level(ctx, argv[0], algo, &level) < 0)
                return JS_EXCEPTION;
        } else if (dyn_lz4_opts(ctx, argv[0], algo == 0 ? 9 : 12,
                       &level, &dict, &dict_len, 0)
            < 0) {
            return JS_EXCEPTION;
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    if (dict_len && algo != 1) {
        free(dict);
        return JS_ThrowTypeError(ctx,
            "a dictionary applies to algo \"lz4\" only");
    }
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        if (algo == 3)
            level = 3;
        else if (algo == 4)
            level = 5;
    }
    if (algo >= 3 && argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "dict");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "a dictionary applies to algo \"lz4\" only");
        }
        JS_FreeValue(ctx, v);
    }

    c = (dyn_compressor_t*)calloc(1, sizeof(*c));
    if (!c) {
        free(dict);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_comp_ctx_init(&c->cx) < 0) {
        free(dict);
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->dict = dict;
    c->dict_len = dict_len;
    c->dict_id = dict_len ? dyn_crc32c(dict, dict_len) : 0;
    c->level = level;
    c->algo = algo;
    c->checksum = checksum;
    return dyn_res_wrap(ctx, new_target, dyn_compressor_class_id, c, dyn_compressor_dispose);
}

static JSValue dyn_compressor_compress(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_compressor_t* c;
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;
    int rc;

    (void)argc;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    c = (dyn_compressor_t*)dyn_res_native(ctx, this_val, dyn_compressor_class_id);
    if (!c) {
        free(src);
        return JS_EXCEPTION;
    }
    if (c->algo == 0)
        rc = dyn_gzip_build_ctx(src, src_len, c->level, &c->cx, &out, &out_len);
    else if (c->algo == 2)
        rc = dyn_lz4_frame_build(src, src_len, c->level, c->checksum, &c->cx,
            &out, &out_len);
    else if (c->algo == 3)
        rc = dyn_zstd_encode(src, src_len, c->level, &out, &out_len);
    else if (c->algo == 4)
        rc = dyn_brotli_encode(src, src_len, c->level, &out, &out_len);
    else if (c->algo == 5)
        rc = dyn_snappy_compress(src, src_len, &out, &out_len);
    else
        rc = dyn_lz4_compress(src, src_len, c->dict, c->dict_len, c->level,
            &c->cx, &out, &out_len);
    free(src);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (c->dict_len) {
        uint8_t* stamped = (uint8_t*)malloc(out_len + 4);
        if (!stamped) {
            free(out);
            return JS_ThrowOutOfMemory(ctx);
        }
        stamped[0] = (uint8_t)(c->dict_id & 0xff);
        stamped[1] = (uint8_t)((c->dict_id >> 8) & 0xff);
        stamped[2] = (uint8_t)((c->dict_id >> 16) & 0xff);
        stamped[3] = (uint8_t)((c->dict_id >> 24) & 0xff);
        memcpy(stamped + 4, out, out_len);
        free(out);
        out = stamped;
        out_len += 4;
    }
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_compressor_decompress(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_compressor_t* c;
    uint8_t* src = NULL;
    size_t src_len = 0, off = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;
    const char* why = NULL;
    const char* codec = NULL;
    int rc;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    c = (dyn_compressor_t*)dyn_res_native(ctx, this_val, dyn_compressor_class_id);
    if (!c) {
        free(src);
        return JS_EXCEPTION;
    }
    if (c->dict_len) {
        uint32_t id;
        if (src_len < 4) {
            free(src);
            return JS_ThrowTypeError(ctx, "record is too short to carry a dictionary id");
        }
        id = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        if (id != c->dict_id) {
            free(src);
            return JS_ThrowTypeError(ctx,
                "dictionary mismatch: record was written with a different dictionary");
        }
        off = 4;
    }
    if (c->algo == 0)
        rc = dyn_gunzip_decode(src + off, src_len - off, &o);
    else if (c->algo == 2)
        rc = dyn_lz4_frame_decode(src + off, src_len - off, &o);
    else if (c->algo == 3) {
        rc = dyn_zstd_decode(src + off, src_len - off, DYN_MAX_OUTPUT, &o, &why);
        if (rc < 0)
            codec = "zstd";
    } else if (c->algo == 4)
        rc = dyn_brotli_decode(src + off, src_len - off, DYN_MAX_OUTPUT, &o, &why);
    else if (c->algo == 5)
        rc = dyn_snappy_decompress(src + off, src_len - off, DYN_MAX_OUTPUT, &o);
    else
        rc = dyn_lz4_decompress(src + off, src_len - off, c->dict, c->dict_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        if (codec)
            return JS_ThrowTypeError(ctx, "Compressor.decompress: %s: %s",
                codec, why ? why : "invalid record");
        if (c->algo == 2)
            return JS_ThrowTypeError(ctx, "Compressor.decompress: %s",
                dyn_lz4_frame_reason(rc));
        return JS_ThrowTypeError(ctx, "Compressor.decompress: %s",
            why ? why : "invalid record");
    }
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_compressor_get_algo(JSContext* ctx, JSValueConst this_val)
{
    dyn_compressor_t* c = (dyn_compressor_t*)dyn_res_native(ctx, this_val,
        dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->algo == 0 ? "gzip" : (c->algo == 1 ? "lz4" : (c->algo == 2 ? "lz4frame" : (c->algo == 3 ? "zstd" : (c->algo == 4 ? "brotli" : "snappy")))));
}

static JSValue dyn_compressor_get_dictid(JSContext* ctx, JSValueConst this_val)
{
    dyn_compressor_t* c = (dyn_compressor_t*)dyn_res_native(ctx, this_val,
        dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return c->dict_len ? JS_NewUint32(ctx, c->dict_id) : JS_NULL;
}

static const JSCFunctionListEntry dyn_compressor_proto[] = {
    JS_CFUNC_DEF("compress", 1, dyn_compressor_compress),
    JS_CFUNC_DEF("decompress", 1, dyn_compressor_decompress),
    JS_CGETSET_DEF("algo", dyn_compressor_get_algo, NULL),
    JS_CGETSET_DEF("dictId", dyn_compressor_get_dictid, NULL),
};

typedef struct {
    dyn_dict_t* d;
} dyn_jsdict_t;

static JSClassID dyn_jsdict_class_id;

static void dyn_jsdict_dispose(void* native)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)native;
    if (!j)
        return;
    dyn_dict_free(j->d);
    free(j);
}

static const JSClassDef dyn_jsdict_class = {
    "Dictionary",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_jsdict_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_jsdict_t* j;
    const uint8_t** phrases = NULL;
    size_t* lens = NULL;
    const char** owned = NULL;
    int64_t n = 0;
    int64_t i;
    int n_owned = 0;
    JSValue ret = JS_EXCEPTION;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new Dictionary(phrases[])");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n < 1)
        return JS_ThrowRangeError(ctx, "at least one phrase is required");
    if (n > DYN_DICT_MAX_PHRASES)
        return JS_ThrowRangeError(ctx, "at most %d phrases", DYN_DICT_MAX_PHRASES);

    phrases = (const uint8_t**)calloc((size_t)n, sizeof(*phrases));
    lens = (size_t*)calloc((size_t)n, sizeof(*lens));
    owned = (const char**)calloc((size_t)n, sizeof(*owned));
    if (!phrases || !lens || !owned) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t sl;
        const char* cs;
        if (JS_IsException(e))
            goto done;
        cs = JS_ToCStringLen(ctx, &sl, e);
        JS_FreeValue(ctx, e);
        if (!cs)
            goto done;
        owned[n_owned++] = cs;
        if (sl == 0) {
            JS_ThrowRangeError(ctx, "an empty phrase matches everywhere and "
                                    "encodes nothing; it is not a phrase");
            goto done;
        }
        phrases[i] = (const uint8_t*)cs;
        lens[i] = sl;
    }

    j = (dyn_jsdict_t*)calloc(1, sizeof(*j));
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    j->d = dyn_dict_new(phrases, lens, (size_t)n);
    if (!j->d) {
        free(j);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = dyn_res_wrap(ctx, new_target, dyn_jsdict_class_id, j, dyn_jsdict_dispose);

done:
    for (i = 0; i < n_owned; i++)
        JS_FreeCString(ctx, owned[i]);
    free(owned);
    free(phrases);
    free(lens);
    return ret;
}

static JSValue dyn_jsdict_run(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_jsdict_t* j;
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    JSValue result;
    int rc;

    (void)argc;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val, dyn_jsdict_class_id);
    if (!j) {
        free(src);
        return JS_EXCEPTION;
    }
    rc = magic ? dyn_dict_decompress(j->d, src, src_len, &o)
               : dyn_dict_compress(j->d, src, src_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        return magic
            ? JS_ThrowTypeError(ctx, "not a record from this dictionary, or "
                                     "the record is malformed")
            : JS_ThrowOutOfMemory(ctx);
    }
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_jsdict_get_id(JSContext* ctx, JSValueConst this_val)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val,
        dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_dict_id(j->d));
}

static JSValue dyn_jsdict_get_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val,
        dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_dict_count(j->d));
}

static const JSCFunctionListEntry dyn_jsdict_proto[] = {
    JS_CFUNC_MAGIC_DEF("compress", 1, dyn_jsdict_run, 0),
    JS_CFUNC_MAGIC_DEF("decompress", 1, dyn_jsdict_run, 1),
    JS_CGETSET_DEF("id", dyn_jsdict_get_id, NULL),
    JS_CGETSET_DEF("size", dyn_jsdict_get_size, NULL),
};

#include "dyna-archive.inc.c"

static JSValue dyn_compress_bound_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* algo = NULL;
    int64_t n64;
    size_t n;
    JSValue av;
    (void)this_val;

    if (argc < 1 || JS_ToInt64(ctx, &n64, argv[0]))
        return JS_ThrowTypeError(ctx, "compressBound(n, { algo }): n required");
    if (n64 < 0)
        return JS_ThrowRangeError(ctx, "compressBound: n must be >= 0");
    if (n64 > (int64_t)(SIZE_MAX >> 9))
        return JS_ThrowRangeError(ctx, "compressBound: n out of range");
    n = (size_t)n64;

    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "compressBound(n, { algo }): algo must be \"gzip\", \"lz4\", "
            "\"lz4frame\", \"zstd\", \"brotli\" or \"snappy\"");
    if (dyn_opts_strict(ctx, argv[1], cpr_algo_keys, 1))
        return JS_EXCEPTION;
    av = JS_GetPropertyStr(ctx, argv[1], "algo");
    if (JS_IsException(av))
        return JS_EXCEPTION;
    algo = JS_ToCString(ctx, av);
    JS_FreeValue(ctx, av);
    if (!algo)
        return JS_EXCEPTION;

    if (!strcmp(algo, "gzip")) {
        JS_FreeCString(ctx, algo);
        {
            size_t nblocks = n ? (n + 65534) / 65535 : 1;
            size_t stored = n ? nblocks * 5 + n : 5;
            if (stored > SIZE_MAX - 18)
                return JS_ThrowRangeError(ctx, "compressBound: n out of range");
            return JS_NewInt64(ctx, (int64_t)(18 + stored));
        }
    }
    if (!strcmp(algo, "lz4")) {
        JS_FreeCString(ctx, algo);
        return JS_NewInt64(ctx, (int64_t)(n + n / 255 + 16));
    }
    if (!strcmp(algo, "lz4frame")) {
        JS_FreeCString(ctx, algo);
        {
            size_t nblk = n ? (n + 4194304 - 1) / 4194304 : 1;
            size_t total = 7 + nblk * 4 + (n + n / 255 + 16) + 8;
            if (total > SIZE_MAX - 4)
                return JS_ThrowRangeError(ctx, "compressBound: n out of range");
            return JS_NewInt64(ctx, (int64_t)(total + 4));
        }
    }
    if (!strcmp(algo, "zstd")) {
        JS_FreeCString(ctx, algo);
#if DYN_HAVE_ZSTD
        return JS_NewInt64(ctx, (int64_t)ZSTD_compressBound(n));
#else
        return JS_ThrowTypeError(ctx, "dyna:compress zstd: support not compiled in");
#endif
    }
    if (!strcmp(algo, "brotli")) {
        JS_FreeCString(ctx, algo);
#if defined(__APPLE__)
        return JS_NewInt64(ctx, (int64_t)(n ? n + 2 + 4 * (n >> 14) + 4 : 2));
#elif DYN_HAVE_BROTLI
        return JS_NewInt64(ctx, (int64_t)BrotliEncoderMaxCompressedSize(n));
#else
        return JS_ThrowTypeError(ctx, "dyna:compress brotli: support not compiled in");
#endif
    }
    if (!strcmp(algo, "snappy")) {
        JS_FreeCString(ctx, algo);
        return JS_NewInt64(ctx, (int64_t)(n + n / 50 + 16));
    }
    JS_FreeCString(ctx, algo);
    return JS_ThrowTypeError(ctx,
        "compressBound(n, { algo }): algo must be \"gzip\", \"lz4\", "
        "\"lz4frame\", \"zstd\", \"brotli\" or \"snappy\"");
}

static const JSCFunctionListEntry dyn_compress_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, dyn_gzip),
    JS_CFUNC_DEF("gunzip", 1, dyn_gunzip),
    JS_CFUNC_DEF("compressBound", 2, dyn_compress_bound_js),
    JS_CFUNC_DEF("lz4Compress", 1, dyn_lz4_compress_js),
    JS_CFUNC_DEF("lz4Decompress", 2, dyn_lz4_decompress_js),
    JS_CFUNC_DEF("lz4Frame", 1, dyn_lz4_frame_js),
    JS_CFUNC_DEF("lz4Unframe", 1, dyn_lz4_unframe_js),
    JS_CFUNC_DEF("zstd", 1, dyn_zstd_js),
    JS_CFUNC_DEF("unzstd", 1, dyn_unzstd_js),
    JS_CFUNC_DEF("brotli", 1, dyn_brotli_js),
    JS_CFUNC_DEF("unbrotli", 1, dyn_unbrotli_js),
    JS_CFUNC_DEF("snappy", 1, dyn_snappy_js),
    JS_CFUNC_DEF("unsnappy", 1, dyn_unsnappy_js),
    JS_CFUNC_MAGIC_DEF("TarList", 1, dyn_tar_read, 0),
    JS_CFUNC_MAGIC_DEF("TarExtract", 1, dyn_tar_read, 1),
    JS_CFUNC_DEF("TarPack", 1, dyn_tar_pack),
    JS_CFUNC_MAGIC_DEF("ZipList", 1, dyn_zip_read, 0),
    JS_CFUNC_MAGIC_DEF("ZipRead", 2, dyn_zip_read, 1),
    JS_CFUNC_DEF("ZipPack", 1, dyn_zip_pack),
    JS_CFUNC_DEF("ZipReadAt", 2, dyn_zip_read_at),
    JS_CFUNC_DEF("ZipExtractAll", 1, dyn_zip_extract_all),
    JS_CFUNC_DEF("deflate", 1, dyn_deflate_js),
    JS_CFUNC_DEF("inflate", 1, dyn_inflate_js),
};

static int dyn_compress_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_compress_funcs,
            countof(dyn_compress_funcs))
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_jsdict_class_id, &dyn_jsdict_class,
            dyn_jsdict_proto, (int)countof(dyn_jsdict_proto),
            dyn_jsdict_ctor, "Dictionary")
        < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_compressor_class_id,
        &dyn_compressor_class, dyn_compressor_proto,
        countof(dyn_compressor_proto),
        dyn_compressor_ctor, "Compressor");
}

int js_nat_init_compress(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:compress", dyn_compress_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_compress_funcs,
            countof(dyn_compress_funcs))
        < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Dictionary") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "Compressor");
}

#endif
