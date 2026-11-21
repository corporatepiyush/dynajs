#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

static const char* const cpr_lz4_keys[] = { "level", "dict" };

static const char* const cpr_lz4_dec_keys[] = { "level", "dict", "maxOutputBytes" };

static const char* const cpr_frame_keys[] = { "checksum", "level", "dict" };

int dyn_lz4_opts(JSContext* ctx, JSValueConst opts, int level_hi,
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

JSValue dyn_lz4_compress_js(JSContext* ctx, JSValueConst this_val,
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
    if (DYN_OFFLOAD_CPU(dyn_lz4_compress(src, src_len, dict, dict_len, level, NULL,
            &out, &out_len))
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

JSValue dyn_lz4_decompress_js(JSContext* ctx, JSValueConst this_val,
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
    if (argc > 1 && dyn_opts_strict(ctx, argv[1], cpr_lz4_dec_keys, 3))
        return JS_EXCEPTION;
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 12, &level, &dict,
            &dict_len, 0)
        < 0)
        return JS_EXCEPTION;
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (DYN_OFFLOAD_CPU(dyn_lz4_decompress(src, src_len, dict, dict_len, &o)) < 0) {
        free(src);
        free(dict);
        free(o.buf);
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "dyna:compress lz4Decompress");
        return JS_ThrowTypeError(ctx, "dyna:compress lz4Decompress: invalid LZ4 block");
    }
    free(src);
    free(dict);
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

JSValue dyn_lz4_frame_js(JSContext* ctx, JSValueConst this_val,
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
    if (DYN_OFFLOAD_CPU(dyn_lz4_frame_build(src, src_len, level, checksum, NULL,
            &out, &out_len))
        < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

JSValue dyn_lz4_unframe_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;
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
    {
        int rc = DYN_OFFLOAD_CPU(dyn_lz4_frame_decode(src, src_len, &o));
        if (rc < 0) {
            free(src);
            free(o.buf);
            if (o.exceeded)
                return dyn_cpr_cap_error(ctx, &o, "dyna:compress lz4Unframe");
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

#endif
