#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

JSValue dyn_enc_hex_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;

    result = JS_NewString8Buf(ctx, n * 2, &out);
    if (!JS_IsException(result) && n)
        dyn_codec_hex_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return result;
}

JSValue dyn_enc_hex_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, outlen, dec;
    uint8_t* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (slen & 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "hexDecode: odd-length hex string");
    }
    outlen = slen / 2;
    out = (uint8_t*)malloc(outlen ? outlen : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    dec = dyn_codec_hex_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (dec == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "hexDecode: invalid hex digit");
    }
    result = dyn_nat_u8_copy(ctx, out, dec);
    free(out);
    return result;
}

JSValue dyn_enc_hex_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, dec;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (slen & 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "hexDecodeInto: odd-length hex string");
    }
    if (dyn_enc_into_out(ctx, argv[1], slen / 2, &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    dec = dyn_codec_hex_decode(str, slen, out);
    JS_FreeCString(ctx, str);
    if (dec == DYN_CODEC_BAD)
        return JS_ThrowSyntaxError(ctx, "hexDecodeInto: invalid hex digit");
    return JS_NewInt64(ctx, (int64_t)dec);
}

JSValue dyn_enc_hex_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, need;
    const char* owned;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_hex_encode_len(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "hexEncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dyn_codec_hex_encode(data, n, (char*)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)need);
}

#endif
