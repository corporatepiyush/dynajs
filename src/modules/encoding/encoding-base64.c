#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

JSValue dyn_enc_base64_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, cap;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = 4 * ((n + 2) / 3);
    result = JS_NewString8Buf(ctx, cap, &out);
    if (!JS_IsException(result) && n) {
        dyn_codec_base64_encode(data, n, out);
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    return result;
}

JSValue dyn_enc_base64_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t n, cap, declen;
    uint8_t* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = 3 * (n / 4);
    out = (uint8_t*)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base64_decode(str, n, out);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base64Decode: invalid base64 string");
    }
    result = dyn_nat_u8_copy(ctx, out, declen);
    free(out);
    return result;
}

JSValue dyn_enc_base64_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t n, dec;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (dyn_enc_into_out(ctx, argv[1], 3 * (n / 4), &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    dec = dyn_codec_base64_decode(str, n, out);
    JS_FreeCString(ctx, str);
    if (dec == DYN_CODEC_BAD)
        return JS_ThrowSyntaxError(ctx, "base64DecodeInto: invalid base64 string");
    return JS_NewInt64(ctx, (int64_t)dec);
}

JSValue dyn_enc_base64_encode_into(JSContext* ctx, JSValueConst this_val,
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
    need = dyn_codec_base64_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base64EncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base64_encode(data, n, (char*)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

#endif
