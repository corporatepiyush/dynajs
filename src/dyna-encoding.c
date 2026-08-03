/*
 * dyna:encoding -- hex, base32/base32hex, base64/base64url, base85, and
 * varint. Self-contained.
 *
 * Encoders take Uint8Array and return string; decoders reverse that and throw
 * on malformed input rather than substituting. Hex and base64 route through
 * the SIMD kernel table; base32/base85 are scalar.
 * Full API: docs/dynajs-guide/API.md.
 */
#include "dyna-nat.h"
#include "core/dyn-hash.h"   /* dyn_sha256: the Base58Check checksum */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h"

/* the pure-C codec library (src/core/dyn-codec.c) */
#include "core/dyn-codec.h" /* base64_encode/base64_decode, shared with text/bytes */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Number.MAX_SAFE_INTEGER == 2^53-1: the largest magnitude a JS double
 * represents exactly. Values beyond this must travel as BigInt. */
#define DYN_ENC_MAX_SAFE_INT (((int64_t)1 << 53) - 1)
/* ceil(64/7): the longest a LEB128 varint of a 64-bit value can be. */
#define DYN_CODEC_VARINT_MAX 10

/* ---------- buffer boundary: Uint8Array/Int8Array/Uint8ClampedArray view or
 * a plain ArrayBuffer -> raw byte pointer + length (dyna:bytes' dyn_bytes_view,
 * duplicated per this module's own conventions -- see dyna-bytes.c). ---------- */
static int dyn_enc_view(JSContext *ctx, JSValueConst v, uint8_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    /* THE VIEW IS TRIED FIRST. JS_GetArrayBuffer THROWS for a typed array --
     * it allocates an Error this function then discards -- so probing the
     * ArrayBuffer case first charged a thrown-and-swallowed exception to the
     * argument type callers overwhelmingly pass. Measured across the module:
     * hexEncode of 1 KiB was 5.32x slower with a Uint8Array than with an
     * ArrayBuffer, for work that is 0.26 us. The rarer ArrayBuffer path now
     * pays what the common one used to. */
    /* Accepts any byte-addressed view, DataView included -- this module is the
     * SOLE owner of the binary-to-text codecs, so it must accept everything
     * the retired duplicates accepted. */
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));   /* not a view: try a buffer */
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (base) {
            *pp = base;
            *pn = ab;
            return 0;
        }
        return -1;   /* neither; JS_GetArrayBuffer left its TypeError pending */
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
        return -1; /* detached mid-resolve; JS_GetArrayBuffer already threw */
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* Resolve `v` to raw bytes for an *Encode function: a string (UTF-8 via
 * JS_ToCStringLen -> *owned, must be released via JS_FreeCString) or a byte
 * view (dyn_enc_view; *owned left NULL, zero-copy). */
static int dyn_enc_bytes(JSContext *ctx, JSValueConst v, const uint8_t **data,
                         size_t *len, const char **owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t *)s;
        return 0;
    }
    {
        uint8_t *p;
        size_t n;
        if (dyn_enc_view(ctx, v, &p, &n))
            return -1;
        *data = p;
        *len = n;
        return 0;
    }
}

/* Build a fresh Uint8Array copying `len` bytes from `data` (never aliases a
 * native pointer into JS). `data` may be NULL only when len==0. */
static JSValue dyn_enc_new_u8array(JSContext *ctx, const uint8_t *data, size_t len)
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

/* ══════════════════════════════ hex ══════════════════════════════ */

/* hex enc/dec run on the shared SIMD kernel (PSHUFB on x86, table-lookup on
 * NEON) — same one dyna:text uses; several GB/s on long inputs. */
static JSValue dyn_enc_hex_encode(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;

    out = (char *)malloc(n ? n * 2 : 1);
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

static JSValue dyn_enc_hex_decode(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, outlen, dec;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (slen & 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "hexDecode: odd-length hex string");
    }
    outlen = slen / 2;
    out = (uint8_t *)malloc(outlen ? outlen : 1);
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

/* ══════════════════════════════ base64 (standard, via SIMD kernel) ══════ */

static JSValue dyn_enc_base64_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = 4 * ((n + 2) / 3);
    out = (char *)malloc(cap ? cap : 1);
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

static JSValue dyn_enc_base64_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *str;
    size_t n, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = 3 * (n / 4);
    out = (uint8_t *)malloc(cap ? cap : 1);
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

/* base64url (RFC 4648 sec.5): '-'/'_' instead of '+'/'/', no padding. Derived
 * from the standard kernel: encode then translate+strip padding; decode
 * translates back and re-pads to a multiple of 4 before handing off (the
 * kernel itself requires n%4==0). A stray '+'/'/' is rejected -- those bytes
 * are simply not part of the url-safe alphabet, so the reverse table has no
 * entry for them. */
static JSValue dyn_enc_base64url_encode(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = dyn_codec_base64_encode_cap(n);
    out = (char *)malloc(cap ? cap : 1);
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

static JSValue dyn_enc_base64url_decode(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, cap, declen;
    char *scratch;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    /* Checked here as well as in the core: the core reports every failure with
     * one sentinel, and a length of 4k+1 deserves its own message -- no byte
     * count encodes to that many characters, so it is a truncation, not a
     * corrupt character. */
    if (slen % 4 == 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecode: invalid length");
    }

    /* The core is allocation-free, so the scratch for the translated and
     * re-padded string is ours: at most 3 pad bytes are ever appended. */
    scratch = (char *)malloc(slen + 3 + 1);
    cap = dyn_codec_base64_decode_cap(slen + 3);
    out = (uint8_t *)malloc(cap ? cap : 1);
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

/* ══════════════════════════════ base32 (RFC 4648) ══════════════════════ */

static JSValue dyn_enc_base32_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 4) / 5) * 8;
    out = (char *)malloc(cap ? cap : 1);
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

static JSValue dyn_enc_base32_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    const char *str;
    size_t slen, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = (slen / 8) * 5;
    out = (uint8_t *)malloc(cap ? cap : 1);
    if (!out) {
        JS_FreeCString(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    declen = dyn_codec_base32_decode(str, slen, out,
                                 (dyn_base32_alphabet)magic);
    JS_FreeCString(ctx, str);
    if (declen == DYN_CODEC_BAD) {
        free(out);
        return JS_ThrowSyntaxError(ctx, magic == DYN_BASE32_HEX ?
            "Base32HexDecode: invalid base32hex string" :
            "Base32Decode: invalid base32 string");
    }
    result = dyn_enc_new_u8array(ctx, out, declen);
    free(out);
    return result;
}

/* ══════════════════ varint: LEB128 + zigzag ════════════════════════════ */

static int dyn_enc_to_u64(JSContext *ctx, JSValueConst v, uint64_t *out)
{
    if (JS_IsBigInt(ctx, v)) {
        int64_t raw;
        if (JS_ToBigInt64(ctx, &raw, v))
            return -1;
        *out = (uint64_t)raw;
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

/* Same for putVarint: a BigInt (full int64 range, wraps mod 2^64) or a
 * Number that must be an exact safe integer of either sign. */
static int dyn_enc_to_i64(JSContext *ctx, JSValueConst v, int64_t *out)
{
    if (JS_IsBigInt(ctx, v))
        return JS_ToBigInt64(ctx, out, v);
    {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= (double)-DYN_ENC_MAX_SAFE_INT &&
              d <= (double)DYN_ENC_MAX_SAFE_INT && floor(d) == d)) {
            JS_ThrowRangeError(ctx, "putVarint: value must be a safe integer or a BigInt");
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    }
}

/* Inverse: a decoded magnitude picks Number when it fits exactly (matching
 * Number.MAX_SAFE_INTEGER), else BigInt -- so callers never see silent
 * precision loss on either decoder. */
static JSValue dyn_enc_u64_to_js(JSContext *ctx, uint64_t v)
{
    if (v <= (uint64_t)DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigUint64(ctx, v);
}

static JSValue dyn_enc_i64_to_js(JSContext *ctx, int64_t v)
{
    if (v >= -DYN_ENC_MAX_SAFE_INT && v <= DYN_ENC_MAX_SAFE_INT)
        return JS_NewFloat64(ctx, (double)v);
    return JS_NewBigInt64(ctx, v);
}

/* Build the [value, bytesRead] result pair. Takes ownership of `value`
 * (consumed by JS_DefinePropertyValueUint32 on both success and failure). */
static JSValue dyn_enc_pair(JSContext *ctx, JSValue value, int32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, value);
        return arr;
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, n), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue dyn_enc_put_uvarint_js(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    uint64_t v;
    uint8_t buf[DYN_CODEC_VARINT_MAX];
    (void)this_val; (void)argc;

    if (dyn_enc_to_u64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_codec_put_uvarint(v, buf));
}

static JSValue dyn_enc_put_varint_js(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    int64_t v;
    uint8_t buf[DYN_CODEC_VARINT_MAX];
    (void)this_val; (void)argc;

    if (dyn_enc_to_i64(ctx, argv[0], &v))
        return JS_EXCEPTION;
    return dyn_enc_new_u8array(ctx, buf, dyn_codec_put_varint(v, buf));
}

static JSValue dyn_enc_uvarint_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    uint64_t v = 0;
    int nb;
    (void)this_val; (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_codec_uvarint(buf, n, &v);
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_u64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

static JSValue dyn_enc_varint_js(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *buf;
    size_t n;
    int64_t v = 0;
    int nb;
    (void)this_val; (void)argc;

    if (dyn_enc_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    nb = dyn_codec_varint(buf, n, &v);
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

/* ══════════════════════════════ base85 (ascii85) ══════════════════════ */
/*
 * Adobe-less ascii85: '!'..'u' (33-117) is a base-85 big-
 * endian digit alphabet, 4 input bytes -> 5 output chars, with the 'z'
 * shorthand for an all-zero 4-byte group (encode only; only recognized at a
 * decode group boundary -- 'z' elsewhere is corrupt input). A trailing
 * partial group of 1-3 input bytes encodes as (that count + 1) characters
 * (the high-order digits of the 5; the low-order ones are discarded). Decode's
 * inverse: a trailing group of 2-4 characters (1 is impossible/corrupt)
 * decodes by treating the missing digit(s) as maximal ('u'=84) before
 * extracting the high-order (count-1) bytes. Whitespace (space/tab/CR/LF/
 * VT/FF) is skipped, matching ascii85's use inside line-wrapped text formats
 * (PostScript, PDF). Algorithm verified byte-for-byte against Python's
 * base64.a85encode/a85decode(adobe=False) -- see tests/test_encoding.js.
 */

static JSValue dyn_enc_base85_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, cap, written;
    const char *owned;
    char *out;
    JSValue result;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    cap = ((n + 3) / 4) * 5;
    out = (char *)malloc(cap ? cap : 1);
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

static JSValue dyn_enc_base85_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, cap, declen;
    uint8_t *out;
    JSValue result;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    cap = slen * 4;
    out = (uint8_t *)malloc(cap ? cap : 1);
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

/* ---------- module registration ---------- */

/* JSON5 and the RFC 8785 canonical form: text codecs, same owner as the rest. */
/* Base58 and the generic BaseX: division codecs, same owner. */
#include "dyna-basex.inc.c"

#include "dyna-json5.inc.c"

/* JSONPath: the query language over the same values, same owner. */
#include "dyna-jsonpath.inc.c"

/* QR Code: Reed-Solomon and bit placement, no codec. */
#include "dyna-qr.inc.c"

static const JSCFunctionListEntry dyn_enc_funcs[] = {
    JS_CFUNC_MAGIC_DEF("QREncode", 2, dyn_qr_encode, 0),
    JS_CFUNC_MAGIC_DEF("QRToString", 2, dyn_qr_encode, 1),
    JS_CFUNC_DEF("JSON5Parse", 1, dyn_json5_parse),
    JS_CFUNC_MAGIC_DEF("JSON5Stringify", 2, dyn_stringify, 0),
    JS_CFUNC_MAGIC_DEF("StableStringify", 2, dyn_stringify, 1),
    JS_CFUNC_DEF("HexEncode", 1, dyn_enc_hex_encode),
    JS_CFUNC_DEF("HexDecode", 1, dyn_enc_hex_decode),

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

    JS_CFUNC_MAGIC_DEF("Base58Encode", 1, dyn_b58, 0),
    JS_CFUNC_MAGIC_DEF("Base58Decode", 1, dyn_b58, 1),
    JS_CFUNC_MAGIC_DEF("Base58CheckEncode", 1, dyn_b58, 2),
    JS_CFUNC_MAGIC_DEF("Base58CheckDecode", 1, dyn_b58, 3),
    JS_CFUNC_MAGIC_DEF("BaseXEncode", 2, dyn_basex, 0),
    JS_CFUNC_MAGIC_DEF("BaseXDecode", 2, dyn_basex, 1),
    JS_CFUNC_DEF("Base85Encode", 1, dyn_enc_base85_encode),
    JS_CFUNC_DEF("Base85Decode", 1, dyn_enc_base85_decode),
};

static int dyn_enc_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_jp_class_id, &dyn_jp_class,
                                 dyn_jp_proto, countof(dyn_jp_proto),
                                 dyn_jp_ctor, "JSONPath") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

int js_nat_init_encoding(JSContext *ctx)
{
    JSModuleDef *m;
    simd_init(); /* idempotent (pthread_once): select the best base64 kernel */
    m = JS_NewCModule(ctx, "dyna:encoding", dyn_enc_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "JSONPath");
    return JS_AddModuleExportList(ctx, m, dyn_enc_funcs, countof(dyn_enc_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_ENCODING */
