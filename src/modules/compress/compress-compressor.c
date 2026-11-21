#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

typedef struct {
    dyn_comp_ctx_t cx;
    uint8_t* dict;
    size_t dict_len;
    uint32_t dict_id;
    int level;
    int algo;
    int checksum;
} dyn_compressor_t;

static const char* const cpr_ctor_keys[] = { "algo", "checksum", "level", "dict" };

static int dyn_codec_level(JSContext* ctx, JSValueConst opts, int algo,
    int* plevel)
{
    int lo, hi, def;
    if (algo == 0) {
        lo = 1;
        hi = 9;
        def = 6;
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

static void dyn_compressor_dispose(void* native)
{
    dyn_compressor_t* c = (dyn_compressor_t*)native;
    if (!c)
        return;
    dyn_comp_ctx_free(&c->cx);
    free(c->dict);
    free(c);
}

JSValue dyn_compressor_ctor(JSContext* ctx, JSValueConst new_target,
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
        if (algo == 0 || algo >= 3) {
            if (dyn_codec_level(ctx, argv[0], algo, &level) < 0)
                return JS_EXCEPTION;
        } else if (dyn_lz4_opts(ctx, argv[0], 12,
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
    if ((algo == 0 || algo >= 3) && argc > 0 && JS_IsObject(argv[0])) {
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

JSValue dyn_compressor_compress(JSContext* ctx, JSValueConst this_val,
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
        rc = dyn_zstd_encode(src, src_len, c->level, 1, &out, &out_len);
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

JSValue dyn_compressor_decompress(JSContext* ctx, JSValueConst this_val,
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
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;
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
        rc = dyn_zstd_decode(src + off, src_len - off, dyn_cpr_cap(&o), &o, &why);
        if (rc < 0)
            codec = "zstd";
    } else if (c->algo == 4)
        rc = dyn_brotli_decode(src + off, src_len - off, dyn_cpr_cap(&o), &o, &why);
    else if (c->algo == 5)
        rc = dyn_snappy_decompress(src + off, src_len - off, dyn_cpr_cap(&o), &o);
    else
        rc = dyn_lz4_decompress(src + off, src_len - off, c->dict, c->dict_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "Compressor.decompress");
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

JSValue dyn_compressor_get_algo(JSContext* ctx, JSValueConst this_val)
{
    dyn_compressor_t* c = (dyn_compressor_t*)dyn_res_native(ctx, this_val,
        dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->algo == 0 ? "gzip" : (c->algo == 1 ? "lz4" : (c->algo == 2 ? "lz4frame" : (c->algo == 3 ? "zstd" : (c->algo == 4 ? "brotli" : "snappy")))));
}

JSValue dyn_compressor_get_dictid(JSContext* ctx, JSValueConst this_val)
{
    dyn_compressor_t* c = (dyn_compressor_t*)dyn_res_native(ctx, this_val,
        dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return c->dict_len ? JS_NewUint32(ctx, c->dict_id) : JS_NULL;
}

#endif
