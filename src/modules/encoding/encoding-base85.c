#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

JSValue dyn_enc_base85_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, len;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    len = dyn_codec_base85_encode_len(data, n);
    result = JS_NewString8Buf(ctx, len, &out);
    if (!JS_IsException(result) && n)
        dyn_codec_base85_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return result;
}

JSValue dyn_enc_base85_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, cap, declen;
    uint8_t* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = slen * 4;
    out = (uint8_t*)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base85_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base85Decode: invalid ascii85 string");
    }
    result = dyn_nat_u8_copy(ctx, out, declen);
    free(out);
    return result;
}

JSValue dyn_enc_base85_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, declen;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (dyn_enc_into_out(ctx, argv[1], slen * 4, &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    declen = dyn_codec_base85_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD)
        return JS_ThrowSyntaxError(ctx,
            "base85DecodeInto: invalid ascii85 string");
    return JS_NewInt64(ctx, (int64_t)declen);
}

JSValue dyn_enc_base85_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, need, written;
    const char* owned;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_base85_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base85EncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base85_encode(data, n, (char*)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

#endif
