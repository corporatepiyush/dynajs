#include "dyna-nat.h"
#include "cutils.h"
#include "core/dyn-hash.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h"

#include "core/dyn-codec.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_ENC_MAX_SAFE_INT (((int64_t)1 << 53) - 1)
#define DYN_CODEC_VARINT_MAX 10

static int dyn_enc_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (base) {
            *pp = base;
            *pn = ab;
            return 0;
        }
        return -1;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a byte view (Uint8Array, Int8Array, "
                               "Uint8ClampedArray, DataView), an ArrayBuffer or "
                               "a string");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

static int dyn_enc_bytes(JSContext* ctx, JSValueConst v, const uint8_t** data,
    size_t* len, const char** owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        const char* s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t*)s;
        return 0;
    }
    {
        uint8_t* p;
        size_t n;
        if (dyn_enc_view(ctx, v, &p, &n))
            return -1;
        *data = p;
        *len = n;
        return 0;
    }
}

static JSValue dyn_enc_new_u8array(JSContext* ctx, const uint8_t* data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_enc_hex_encode(JSContext* ctx, JSValueConst this_val,
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

    out = (char*)malloc(n ? n * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_codec_hex_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, n * 2);
    free(out);
    return result;
}

static JSValue dyn_enc_hex_decode(JSContext* ctx, JSValueConst this_val,
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
    result = dyn_enc_new_u8array(ctx, out, dec);
    free(out);
    return result;
}

static JSValue dyn_enc_base64_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, cap, written;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = 4 * ((n + 2) / 3);
    out = (char*)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_codec_base64_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base64_decode(JSContext* ctx, JSValueConst this_val,
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
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

static JSValue dyn_enc_base64url_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, cap, written;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = dyn_codec_base64_encode_cap(n);
    out = (char*)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_codec_base64url_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base64url_decode(JSContext* ctx, JSValueConst this_val,
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
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

static JSValue dyn_enc_base32_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const uint8_t* data;
    size_t n, cap, written;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 4) / 5) * 8;
    out = (char*)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_codec_base32_encode(data, n, out,
        (dyn_base32_alphabet)magic);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base32_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
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
    cap = (slen / 8) * 5;
    out = (uint8_t*)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base32_decode(str, slen, out,
        (dyn_base32_alphabet)magic);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, magic == DYN_BASE32_HEX ? "Base32HexDecode: invalid base32hex string" : "Base32Decode: invalid base32 string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

static int dyn_enc_to_u64(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    if (JS_IsBigInt(ctx, v)) {
        size_t len, start = 0, digits;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        int neg = 0, bad = 0;
        if (!s)
            return -1;
        if (len > 0 && s[0] == '-') {
            neg = 1;
            start = 1;
        }
        while (start < len && s[start] == '0')
            start++;
        digits = len - start;
        if (neg) {
            bad = 1;
        } else if (digits > 20) {
            bad = 1;
        } else if (digits == 20 && memcmp(s + start, "18446744073709551615", 20) > 0) {
            bad = 1;
        }
        JS_FreeCString(ctx, s);
        if (bad) {
            JS_ThrowRangeError(ctx, "putUvarint: BigInt value must be in [0, 2^64-1]");
            return -1;
        }
        {
            int64_t raw;
            if (JS_ToBigInt64(ctx, &raw, v))
                return -1;
            *out = (uint64_t)raw;
        }
        return 0;
    }
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 0 && d <= (double)DYN_ENC_MAX_SAFE_INT && floor(d) == d)) {
            JS_ThrowRangeError(ctx, "putUvarint: value must be a non-negative safe integer or a BigInt");
            return -1;
        }
        *out = (uint64_t)d;
        return 0;
    }
}

static int dyn_enc_to_i64(JSContext* ctx, JSValueConst v, int64_t* out)
{
    if (JS_IsBigInt(ctx, v))
        return JS_ToBigInt64(ctx, out, v);
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= (double)-DYN_ENC_MAX_SAFE_INT && d <= (double)DYN_ENC_MAX_SAFE_INT && floor(d) == d)) {
            JS_ThrowRangeError(ctx, "putVarint: value must be a safe integer or a BigInt");
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    }
}

static JSValue dyn_enc_u64_to_js(JSContext* ctx, uint64_t v)
{
    if (v <= (uint64_t)DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigUint64(ctx, v);
}

static JSValue dyn_enc_i64_to_js(JSContext* ctx, int64_t v)
{
    if (v >= -DYN_ENC_MAX_SAFE_INT && v <= DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigInt64(ctx, v);
}

static JSValue dyn_enc_pair(JSContext* ctx, JSValue value, int32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, value);
        return arr;
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, value, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, n), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_enc_put_uvarint_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t v;
    uint8_t buf[DYN_CODEC_VARINT_MAX];
    (void)this_val;
    (void)argc;

    if (dyn_enc_to_u64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_codec_put_uvarint(v, buf));
}

static JSValue dyn_enc_put_varint_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t v;
    uint8_t buf[DYN_CODEC_VARINT_MAX];
    (void)this_val;
    (void)argc;

    if (dyn_enc_to_i64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_codec_put_varint(v, buf));
}

static JSValue dyn_enc_uvarint_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    uint64_t v = 0;
    int nb;
    (void)this_val;
    (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_codec_uvarint(buf, n, &v);
    if (nb < 0)
        return JS_ThrowRangeError(ctx, "Uvarint: value overflows 64 bits");
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_u64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

static JSValue dyn_enc_varint_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    int64_t v = 0;
    int nb;
    (void)this_val;
    (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_codec_varint(buf, n, &v);
    if (nb < 0)
        return JS_ThrowRangeError(ctx, "Varint: value overflows 64 bits");
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

static JSValue dyn_enc_append_uvarint(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t v;
    uint8_t scratch[DYN_CODEC_VARINT_MAX];
    uint64_t off = 0;
    size_t nb;
    uint8_t* buf;
    size_t n;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "appendUvarint(buf, value, offset?)");
    if (dyn_enc_to_u64(ctx, argv[1], &v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &off, argv[2]))
        return JS_EXCEPTION;
    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    nb = dyn_codec_put_uvarint(v, scratch);
    if (off > (uint64_t)n || nb > n - off)
        return JS_ThrowRangeError(ctx,
            "appendUvarint: needs %lu bytes at offset %llu, buffer has %lu "
            "(no reallocation: size the buffer, or use PutUvarint)",
            (unsigned long)nb, (unsigned long long)off, (unsigned long)n);
    memcpy(buf + off, scratch, nb);
    return JS_NewInt64(ctx, (int64_t)(off + nb));
}

static JSValue dyn_enc_uvarint_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    uint64_t off = 0, v = 0;
    int nb;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "uvarintAt(buf, offset?)");
    if (!JS_IsUndefined(argv[1]) && JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;
    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off > n)
        return JS_ThrowRangeError(ctx, "uvarintAt: offset out of bounds");
    nb = dyn_codec_uvarint(buf + off, n - off, &v);
    if (nb < 0)
        return JS_ThrowRangeError(ctx, "uvarintAt: value overflows 64 bits");
    return nb > 0 ? dyn_enc_u64_to_js(ctx, v) : JS_NewInt32(ctx, 0);
}

static JSValue dyn_enc_varint_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    uint64_t off = 0;
    int64_t v = 0;
    int nb;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "varintAt(buf, offset?)");
    if (!JS_IsUndefined(argv[1]) && JS_ToIndex(ctx, &off, argv[1]))
        return JS_EXCEPTION;
    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (off > n)
        return JS_ThrowRangeError(ctx, "varintAt: offset out of bounds");
    nb = dyn_codec_varint(buf + off, n - off, &v);
    if (nb < 0)
        return JS_ThrowRangeError(ctx, "varintAt: value overflows 64 bits");
    return nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0);
}

static JSValue dyn_enc_base85_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t n, cap, written;
    const char* owned;
    char* out;
    JSValue result;
    (void)this_val;
    (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 3) / 4) * 5;
    out = (char*)malloc(cap ? cap : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    written = dyn_codec_base85_encode(data, n, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = JS_NewStringLen(ctx, out, written);
    free(out);
    return result;
}

static JSValue dyn_enc_base85_decode(JSContext* ctx, JSValueConst this_val,
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
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

static int dyn_enc_into_out_as(JSContext* ctx, JSValueConst out, size_t need,
    const char* what, uint8_t** pp, size_t* pn)
{
    if (dyn_enc_view(ctx, out, pp, pn))
        return -1;
    if (*pn < need) {
        JS_ThrowRangeError(ctx,
            "%s: output needs at least %lu bytes, got %lu", what,
            (unsigned long)need, (unsigned long)*pn);
        return -1;
    }
    return 0;
}

static int dyn_enc_into_out(JSContext* ctx, JSValueConst out, size_t need,
    uint8_t** pp, size_t* pn)
{
    return dyn_enc_into_out_as(ctx, out, need, "decodeInto", pp, pn);
}

static int dyn_enc_no_alias(JSContext* ctx, const uint8_t* data, size_t n,
    const uint8_t* out, size_t on, const char* what)
{
    uintptr_t a = (uintptr_t)data, ae = a + n;
    uintptr_t b = (uintptr_t)out, be = b + on;

    if (n && on && a < be && b < ae) {
        JS_ThrowTypeError(ctx, "%s: the input and output buffers overlap",
            what);
        return -1;
    }
    return 0;
}

static JSValue dyn_enc_hex_decode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_base64_decode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_base64url_decode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_base32_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic)
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
    if (dyn_enc_into_out(ctx, argv[1], (slen / 8) * 5, &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    declen = dyn_codec_base32_decode(str, slen, out, (dyn_base32_alphabet)magic);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD) {
        return JS_ThrowSyntaxError(ctx, magic == DYN_BASE32_HEX ? "Base32HexDecodeInto: invalid base32hex string" : "Base32DecodeInto: invalid base32 string");
    }
    return JS_NewInt64(ctx, (int64_t)declen);
}

static JSValue dyn_enc_base85_decode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_hex_encode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_base64_encode_into(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_enc_base64url_encode_into(JSContext* ctx,
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

static JSValue dyn_enc_base32_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic)
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
    need = dyn_codec_base32_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base32EncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base32_encode(data, n, (char*)out,
        (dyn_base32_alphabet)magic);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

static JSValue dyn_enc_base85_encode_into(JSContext* ctx, JSValueConst this_val,
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

#define DYN_DETECT_MIN_BYTES 16

static const char* dyn_detect_cjk(const uint8_t* p, size_t n)
{
    size_t i;
    int gbk_hits = 0, big5_hits = 0, sjis_hits = 0, eucjp_hits = 0, euckr_hits = 0;
    int sjis_kana_hits = 0;
    int eucjp_specials = 0;
    int total_multibyte = 0;

    for (i = 0; i < n; i++) {
        uint8_t c1 = p[i];
        if (c1 < 0x80)
            continue;
        total_multibyte++;

        if (i + 1 < n) {
            uint8_t c2 = p[i + 1];

            if (c1 == 0x8E && c2 >= 0xA1 && c2 <= 0xDF) {
                eucjp_hits += 2;
                eucjp_specials++;
                i++;
                continue;
            }
            if (c1 == 0x8F && i + 2 < n) {
                uint8_t c3 = p[i + 2];
                if (c2 >= 0xA1 && c2 <= 0xFE && c3 >= 0xA1 && c3 <= 0xFE) {
                    eucjp_hits += 3;
                    eucjp_specials += 2;
                    i += 2;
                    continue;
                }
            }

            if (((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC)) && ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC))) {
                sjis_hits += 2;
                if (c1 == 0x82 || c1 == 0x83)
                    sjis_kana_hits++;
            }

            if (c1 >= 0x81 && c1 <= 0xFE && c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                gbk_hits += 2;
            }

            if (c1 >= 0x81 && c1 <= 0xFE && ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0xA1 && c2 <= 0xFE))) {
                big5_hits += 2;
            }

            if (c1 >= 0xA1 && c1 <= 0xFE && c2 >= 0xA1 && c2 <= 0xFE) {
                eucjp_hits += 2;
                euckr_hits += 2;
            }
            i++;
        }
    }

    if (total_multibyte == 0)
        return "utf-8";

    if (eucjp_specials > 0 && eucjp_hits >= sjis_hits && eucjp_hits >= gbk_hits)
        return "euc-jp";
    if (sjis_kana_hits > 0 && sjis_hits >= gbk_hits)
        return "shift_jis";
    if (sjis_hits > gbk_hits && sjis_hits > eucjp_hits && sjis_hits > big5_hits)
        return "shift_jis";
    if (big5_hits > gbk_hits && big5_hits > sjis_hits)
        return "big5";
    if (gbk_hits > 0 && gbk_hits >= sjis_hits && gbk_hits >= eucjp_hits)
        return "gbk";
    if (euckr_hits > 0 && euckr_hits >= gbk_hits)
        return "euc-kr";
    if (eucjp_hits > 0)
        return "euc-jp";

    return NULL;
}

static JSValue dyn_enc_detect_encoding(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* p;
    size_t n;
    const char* verdict = NULL;
    const char* fallback = "utf-8";
    const char* fallback_alloc = NULL;
    JSValue allow_list = JS_UNDEFINED;

    if (dyn_enc_view(ctx, argv[0], &p, &n) < 0)
        return JS_ThrowTypeError(ctx, "detectEncoding requires a TypedArray or ArrayBuffer");

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue fb = JS_GetPropertyStr(ctx, argv[1], "fallback");
        if (!JS_IsUndefined(fb)) {
            fallback_alloc = JS_ToCString(ctx, fb);
            if (fallback_alloc)
                fallback = fallback_alloc;
        }
        JS_FreeValue(ctx, fb);
        allow_list = JS_GetPropertyStr(ctx, argv[1], "allowList");
    }

    if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
        verdict = "utf-32be";
    } else if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
        verdict = "utf-32le";
    } else if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        verdict = "utf-8";
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        verdict = "utf-16be";
    } else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        verdict = "utf-16le";
    }

    if (!verdict) {
        if (simd.validate_utf8 && simd.validate_utf8(p, n) == n) {
            verdict = "utf-8";
        }
    }

    if (!verdict && n >= DYN_DETECT_MIN_BYTES) {
        verdict = dyn_detect_cjk(p, n);
    }

    if (!verdict)
        verdict = fallback;

    if (JS_IsArray(ctx, allow_list)) {
        uint32_t len = 0, i;
        int found = 0;
        JSValue lv = JS_GetPropertyStr(ctx, allow_list, "length");
        JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);

        for (i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, allow_list, i);
            const char* s = JS_ToCString(ctx, item);
            if (s) {
                if (strcasecmp(s, verdict) == 0)
                    found = 1;
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, item);
            if (found)
                break;
        }
        if (!found) {
            if (fallback && strcasecmp(fallback, verdict) != 0) {
                for (i = 0; i < len; i++) {
                    JSValue item = JS_GetPropertyUint32(ctx, allow_list, i);
                    const char* s = JS_ToCString(ctx, item);
                    if (s) {
                        if (strcasecmp(s, fallback) == 0)
                            found = 1;
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, item);
                    if (found) {
                        verdict = fallback;
                        break;
                    }
                }
            }
            if (!found) {
                if (fallback_alloc)
                    JS_FreeCString(ctx, fallback_alloc);
                JS_FreeValue(ctx, allow_list);
                return JS_ThrowTypeError(ctx, "detectEncoding: detected encoding not in allowList and no valid fallback");
            }
        }
    }

    {
        JSValue ret = JS_NewString(ctx, verdict);
        if (fallback_alloc)
            JS_FreeCString(ctx, fallback_alloc);
        JS_FreeValue(ctx, allow_list);
        return ret;
    }
}
#include "dyna-basex.inc.c"

#include "dyna-json5.inc.c"

#include "dyna-jsonpath.inc.c"

#include "dyna-qr.inc.c"

static const JSCFunctionListEntry dyn_enc_funcs[] = {
    JS_CFUNC_MAGIC_DEF("QREncode", 1, dyn_qr_encode, 0),
    JS_CFUNC_MAGIC_DEF("QRToString", 1, dyn_qr_encode, 1),
    JS_CFUNC_DEF("JSON5Parse", 1, dyn_json5_parse),
    JS_CFUNC_MAGIC_DEF("JSON5Stringify", 1, dyn_stringify, 0),
    JS_CFUNC_MAGIC_DEF("StableStringify", 1, dyn_stringify, 1),
    JS_CFUNC_DEF("HexEncode", 1, dyn_enc_hex_encode),
    JS_CFUNC_DEF("HexDecode", 1, dyn_enc_hex_decode),
    JS_CFUNC_DEF("hexDecodeInto", 2, dyn_enc_hex_decode_into),
    JS_CFUNC_DEF("base64DecodeInto", 2, dyn_enc_base64_decode_into),
    JS_CFUNC_DEF("base64UrlDecodeInto", 2, dyn_enc_base64url_decode_into),
    JS_CFUNC_MAGIC_DEF("base32DecodeInto", 2, dyn_enc_base32_decode_into, DYN_BASE32_STD),
    JS_CFUNC_MAGIC_DEF("base32HexDecodeInto", 2, dyn_enc_base32_decode_into, DYN_BASE32_HEX),
    JS_CFUNC_DEF("base85DecodeInto", 2, dyn_enc_base85_decode_into),
    JS_CFUNC_MAGIC_DEF("base58DecodeInto", 2, dyn_b58_decode_into, 0),
    JS_CFUNC_MAGIC_DEF("base58CheckDecodeInto", 2, dyn_b58_decode_into, 1),
    JS_CFUNC_DEF("hexEncodeInto", 2, dyn_enc_hex_encode_into),
    JS_CFUNC_DEF("base64EncodeInto", 2, dyn_enc_base64_encode_into),
    JS_CFUNC_DEF("base64UrlEncodeInto", 2, dyn_enc_base64url_encode_into),
    JS_CFUNC_MAGIC_DEF("base32EncodeInto", 2, dyn_enc_base32_encode_into, DYN_BASE32_STD),
    JS_CFUNC_MAGIC_DEF("base32HexEncodeInto", 2, dyn_enc_base32_encode_into, DYN_BASE32_HEX),
    JS_CFUNC_DEF("base85EncodeInto", 2, dyn_enc_base85_encode_into),
    JS_CFUNC_MAGIC_DEF("base58EncodeInto", 2, dyn_b58_encode_into, 0),
    JS_CFUNC_MAGIC_DEF("base58CheckEncodeInto", 2, dyn_b58_encode_into, 1),

    JS_CFUNC_DEF("Base64Encode", 1, dyn_enc_base64_encode),
    JS_CFUNC_DEF("Base64Decode", 1, dyn_enc_base64_decode),
    JS_CFUNC_DEF("Base64URLEncode", 1, dyn_enc_base64url_encode),
    JS_CFUNC_DEF("Base64URLDecode", 1, dyn_enc_base64url_decode),

    JS_CFUNC_MAGIC_DEF("Base32Encode", 1, dyn_enc_base32_encode, DYN_BASE32_STD),
    JS_CFUNC_MAGIC_DEF("Base32Decode", 1, dyn_enc_base32_decode, DYN_BASE32_STD),
    JS_CFUNC_MAGIC_DEF("Base32HexEncode", 1, dyn_enc_base32_encode, DYN_BASE32_HEX),
    JS_CFUNC_MAGIC_DEF("Base32HexDecode", 1, dyn_enc_base32_decode, DYN_BASE32_HEX),

    JS_CFUNC_DEF("PutUvarint", 1, dyn_enc_put_uvarint_js),
    JS_CFUNC_DEF("Uvarint", 1, dyn_enc_uvarint_js),
    JS_CFUNC_DEF("PutVarint", 1, dyn_enc_put_varint_js),
    JS_CFUNC_DEF("Varint", 1, dyn_enc_varint_js),
    JS_CFUNC_DEF("appendUvarint", 3, dyn_enc_append_uvarint),
    JS_CFUNC_DEF("uvarintAt", 2, dyn_enc_uvarint_at),
    JS_CFUNC_DEF("varintAt", 2, dyn_enc_varint_at),

    JS_CFUNC_MAGIC_DEF("Base58Encode", 1, dyn_b58, 0),
    JS_CFUNC_MAGIC_DEF("Base58Decode", 1, dyn_b58, 1),
    JS_CFUNC_MAGIC_DEF("Base58CheckEncode", 1, dyn_b58, 2),
    JS_CFUNC_MAGIC_DEF("Base58CheckDecode", 1, dyn_b58, 3),
    JS_CFUNC_MAGIC_DEF("BaseXEncode", 2, dyn_basex, 0),
    JS_CFUNC_MAGIC_DEF("BaseXDecode", 2, dyn_basex, 1),
    JS_CFUNC_DEF("Base85Encode", 1, dyn_enc_base85_encode),
    JS_CFUNC_DEF("Base85Decode", 1, dyn_enc_base85_decode),
    JS_CFUNC_DEF("DetectEncoding", 1, dyn_enc_detect_encoding),
    JS_CFUNC_DEF("detectEncoding", 1, dyn_enc_detect_encoding),
};

static int dyn_enc_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_jp_class_id, &dyn_jp_class,
            dyn_jp_proto, countof(dyn_jp_proto),
            dyn_jp_ctor, "JSONPath")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

int js_nat_init_encoding(JSContext* ctx)
{
    JSModuleDef* m;
    simd_init();
    m = JS_NewCModule(ctx, "dyna:encoding", dyn_enc_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "JSONPath");
    return JS_AddModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

#endif
