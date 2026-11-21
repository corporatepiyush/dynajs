#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

JSValue dyn_enc_base64url_encode(JSContext* ctx, JSValueConst this_val,
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
    cap = (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0);
    result = JS_NewString8Buf(ctx, cap, &out);
    if (!JS_IsException(result) && n) {
        dyn_codec_base64url_encode(data, n, out);
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    return result;
}

JSValue dyn_enc_base64url_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, cap, declen;
    char* scratch;
    uint8_t* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    if (slen % 4 == 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid length");
    }

    scratch = (char*)malloc(slen + 3 + 1);
    cap = dyn_codec_base64_decode_cap(slen + 3);
    out = (uint8_t*)malloc(cap ? cap : 1);
    if (!scratch || !out) {
        free(scratch);
        free(out);
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base64url_decode(str, slen, out, scratch);
    JS_FreeCString(ctx, str);
    free(scratch);
    if (declen == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid base64url string");
    }
    result = dyn_nat_u8_copy(ctx, out, declen);
    free(out);
    return result;
}

JSValue dyn_enc_base64url_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t slen, declen;
    char* scratch;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (slen % 4 == 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecodeInto: invalid length");
    }
    if (dyn_enc_into_out(ctx, argv[1], dyn_codec_base64_decode_cap(slen + 3),
            &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    scratch = (char*)malloc(slen + 3 + 1);
    if (!scratch) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base64url_decode(str, slen, out, scratch);
    JS_FreeCString(ctx, str);
    free(scratch);
    if (declen == DYN_CODEC_BAD)
        return JS_ThrowSyntaxError(ctx,
            "base64UrlDecodeInto: invalid base64url string");
    return JS_NewInt64(ctx, (int64_t)declen);
}

JSValue dyn_enc_base64url_encode_into(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, written;
    const char* owned;
    uint8_t* out;
    size_t on;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    if (dyn_enc_into_out_as(ctx, argv[1], dyn_codec_base64_encode_cap(n),
            "encodeInto", &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base64UrlEncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base64url_encode(data, n, (char*)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

#endif
