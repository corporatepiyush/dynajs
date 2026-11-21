#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#define DYN_ENC_MAX_SAFE_INT (((int64_t)1 << 53) - 1)
#define DYN_CODEC_VARINT_MAX 10

int dyn_enc_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
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

int dyn_enc_bytes(JSContext* ctx, JSValueConst v, const uint8_t** data,
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
    return dyn_nat_u8_copy(ctx, buf, dyn_codec_put_uvarint(v, buf));
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
    return dyn_nat_u8_copy(ctx, buf, dyn_codec_put_varint(v, buf));
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

int dyn_enc_into_out_as(JSContext* ctx, JSValueConst out, size_t need,
    const char* what, uint8_t** pp, size_t* pn)
{
    if (dyn_enc_view_w(ctx, out, pp, pn))
        return -1;
    if (*pn < need) {
        JS_ThrowRangeError(ctx,
            "%s: output needs at least %lu bytes, got %lu", what,
            (unsigned long)need, (unsigned long)*pn);
        return -1;
    }
    return 0;
}

int dyn_enc_no_alias(JSContext* ctx, const uint8_t* data, size_t n,
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
