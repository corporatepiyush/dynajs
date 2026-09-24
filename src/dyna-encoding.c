/*
 * dyna:encoding -- hex, base32/base32hex, base64/base64url, base85, and
 * varint. Self-contained.
 *
 * Encoders take Uint8Array and return string; decoders reverse that and throw
 * on malformed input rather than substituting. Hex and base64 route through
 * the SIMD kernel table; base32/base85 are scalar.
 * Full API: see the dyna:* module in dyna-libc.h.
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
        /* JS_ToBigInt64 silently reduces mod 2^64, so 2n**64n encoded as
         * [0x00] and -1n as ten 0xFF bytes -- both silently wrong for a
         * "non-negative value, at most 10 bytes" LEB128 uvarint. Range-check
         * the decimal form (BigInt ToString is plain digits, no 'n') before
         * reducing. */
        size_t len, start = 0, digits;
        const char *s = JS_ToCStringLen(ctx, &len, v);
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
        } else if (digits == 20 &&
                   memcmp(s + start, "18446744073709551615", 20) > 0) {
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
    if (nb < 0)                     /* codec's overflow sentinel: the value
                                       needs more than 64 bits (an 11th byte
                                       or a >1 final byte). The old [0, -11]
                                       return leaked the internal sentinel
                                       through a documented [value, bytesRead]
                                       API; overflow is a RangeError like
                                       PutUvarint's argument overflow. */
        return JS_ThrowRangeError(ctx, "Uvarint: value overflows 64 bits");
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
    if (nb < 0)                     /* same overflow contract as Uvarint */
        return JS_ThrowRangeError(ctx, "Varint: value overflows 64 bits");
    return dyn_enc_pair(ctx, nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0), nb);
}

/* ══════════════════: in-place varints ══════════════════
 *
 * appendUvarint(buf, value, offset=0) -> new end offset, uvarintAt(buf,
 * offset=0) / varintAt(buf, offset=0) -> value. The stream-walking pair the
 * [value, bytesRead] one-shots could not be: uvarintAt returns the VALUE
 * ALONE (no tuple allocation per field of a varint stream) and appendUvarint
 * writes into a caller-owned buffer (no Uint8Array per field). The PutUvarint,
 * Uvarint, PutVarint and Varint spellings above are legacy and unchanged.
 *
 * Contracts:
 *  - appendUvarint encodes at `offset` and returns offset + bytesWritten.
 *    NO reallocation, ever: the caller owns capacity, and a buffer too small
 *    at `offset` throws RangeError BEFORE anything is written (the value is
 *    encoded into stack scratch first, so the write is atomic). Same value
 *    coercion as PutUvarint -- non-negative safe integer or BigInt in
 *    [0, 2^64-1], at most DYN_CODEC_VARINT_MAX (10) bytes.
 *  - uvarintAt/varintAt read at `offset` and return the value: a Number when
 *    it fits exactly (<= 2^53-1, the same threshold as Uvarint's tuple) and
 *    a BigInt otherwise. TRUNCATION (no terminating byte before the buffer
 *    end) returns 0, matching the value half of Uvarint's [0, 0]; a 64-bit
 *    overflow throws the same RangeError Uvarint throws. An offset past the
 *    end is a RangeError, not a truncation: it is an argument bug, not a
 *    short stream.
 */

static JSValue dyn_enc_append_uvarint(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    uint64_t v;
    uint8_t scratch[DYN_CODEC_VARINT_MAX];
    uint64_t off = 0;
    size_t nb;
    uint8_t *buf;
    size_t n;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "appendUvarint(buf, value, offset?)");
    /* value first (as PutUvarint does), then the offset, then the buffer:
     * the same coerce-before-resolve order as the rest of the module */
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

static JSValue dyn_enc_uvarint_at(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t *buf;
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
    if (nb < 0)                     /* same overflow contract as Uvarint */
        return JS_ThrowRangeError(ctx, "uvarintAt: value overflows 64 bits");
    /* nb == 0: truncated stream -> value 0, matching Uvarint's [0, 0] */
    return nb > 0 ? dyn_enc_u64_to_js(ctx, v) : JS_NewInt32(ctx, 0);
}

static JSValue dyn_enc_varint_at(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t *buf;
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
    if (nb < 0)                     /* same overflow contract as Varint */
        return JS_ThrowRangeError(ctx, "varintAt: value overflows 64 bits");
    return nb > 0 ? dyn_enc_i64_to_js(ctx, v) : JS_NewInt32(ctx, 0);
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

/* ══════════════════: the *DecodeInto family ══════════════════
 *
 * Every text decoder gains a form that decodes into a CALLER-OWNED byte view
 * and returns the number of bytes written -- the output-buffer contract,
 * applied to the hot direction. Deliberately asymmetric: there is no
 * *EncodeInto for the text-output codecs, because an encoder's output is a
 * JS string and a string cannot be written into a buffer in JS -- the
 * string-producing encoders keep allocating. ('s "output buffers" makes
 * sense exactly where the output is BYTES, i.e. the decoders.)
 *
 * Shared contract:
 *  - `out` is any byte view (Uint8Array, Int8Array, Uint8ClampedArray,
 *    DataView) or ArrayBuffer; bytes are written at its offset 0.
 *  - BEFORE decoding, the required capacity is checked and a too-small `out`
 *    throws RangeError -- nothing is partially written on that path.
 *  - On a DECODE error the same SyntaxError as the one-shot is thrown and
 *    `out`'s contents are unspecified (the codec may have written a prefix).
 *  - The one-shots and the Into forms call the SAME core codecs
 *    (dyn_codec_*): byte-identity between the two spellings is a property of
 *    the implementation, not a test aspiration.
 *
 * Required capacities (the same bounds the one-shots allocate):
 *   hexDecodeInto          n/2                       (odd length rejected)
 *   base64DecodeInto       3*(n/4)
 *   base64UrlDecodeInto    3*((n+3)/4)
 *   base32DecodeInto       (n/8)*5                   (n % 8 must be 0)
 *   base32HexDecodeInto    (n/8)*5
 *   base85DecodeInto       4*n                       (whitespace-skipping safe)
 *   base58DecodeInto       n                         (see dyna-basex.inc.c)
 *   base58CheckDecodeInto  n
 * BaseXDecodeInto is deliberately NOT provided: the division codec
 * allocates internally anyway (bx_decode mallocs its own big-integer
 * workspace), so an Into form would only relocate the final memcpy while
 * adding a second capacity contract to document. Revisit only if bx_decode
 * ever grows a caller-buffer form. */

/* Shared: resolve `out` and enforce the required capacity. Returns 0 with
 * the pointer/length pair set, or -1 with a pending exception (a RangeError
 * naming the shortfall, so a caller can size the buffer from the message).
 * `what` labels the direction ("decodeInto"/"encodeInto"). */
static int dyn_enc_into_out_as(JSContext *ctx, JSValueConst out, size_t need,
                               const char *what, uint8_t **pp, size_t *pn)
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

static int dyn_enc_into_out(JSContext *ctx, JSValueConst out, size_t need,
                            uint8_t **pp, size_t *pn)
{
    return dyn_enc_into_out_as(ctx, out, need, "decodeInto", pp, pn);
}

/* Encode-side extra: the input is BYTES, so it can overlap `out` (the decode
 * side's input is text and cannot). In-place encoding would read bytes the
 * core has already overwritten -- silent corruption with a plausible shape.
 * REFUSE overlapping ranges outright. */
static int dyn_enc_no_alias(JSContext *ctx, const uint8_t *data, size_t n,
                            const uint8_t *out, size_t on, const char *what)
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

static JSValue dyn_enc_hex_decode_into(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, dec;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

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

static JSValue dyn_enc_base64_decode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const char *str;
    size_t n, dec;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

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

static JSValue dyn_enc_base64url_decode_into(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, declen;
    char *scratch;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    str = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    /* Same pre-check as the one-shot: 4k+1 is a truncation, not a corrupt
     * character, and deserves its own message. */
    if (slen % 4 == 1) {
        JS_FreeCString(ctx, str);
        return JS_ThrowSyntaxError(ctx, "base64UrlDecodeInto: invalid length");
    }
    if (dyn_enc_into_out(ctx, argv[1], dyn_codec_base64_decode_cap(slen + 3),
                         &out, &on)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    /* The core is allocation-free; the translate/re-pad scratch is ours, as
     * in the one-shot (at most 3 pad bytes are appended). */
    scratch = (char *)malloc(slen + 3 + 1);
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

/* magic: DYN_BASE32_STD or DYN_BASE32_HEX (shared with the one-shots). */
static JSValue dyn_enc_base32_decode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic)
{
    const char *str;
    size_t slen, declen;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

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
        return JS_ThrowSyntaxError(ctx, magic == DYN_BASE32_HEX ?
            "Base32HexDecodeInto: invalid base32hex string" :
            "Base32DecodeInto: invalid base32 string");
    }
    return JS_NewInt64(ctx, (int64_t)declen);
}

static JSValue dyn_enc_base85_decode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const char *str;
    size_t slen, declen;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

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

/* ══════════════════: the *EncodeInto family ══════════════════
 *
 * The mirror of the *DecodeInto contract above, for the ENCODE direction:
 * the text a codec produces is ASCII bytes, and a hot loop composing output
 * into a caller-owned buffer should not allocate a JS string per field to
 * throw most of it away. (The historical comment here argued there is no
 * *EncodeInto "because a string cannot be written into a buffer"; the
 * contract answers that the OUTPUT is what goes in the buffer -- the
 * caller's -- and the count of ASCII bytes written is returned.)
 *
 * Shared contract, identical to the decode side:
 *  - `out` is any byte view or ArrayBuffer; text is written at offset 0.
 *  - BEFORE encoding, the required capacity (the same bound the one-shot
 *    allocates) is checked and a too-small `out` throws RangeError naming
 *    the shortfall -- nothing is partially written on that path.
 *  - The one-shots and the Into forms call the SAME core codecs
 *    (dyn_codec_*), so their bytes are identical by construction.
 *
 * Required capacities (the one-shots' own sizing formulas):
 *   hexEncodeInto          2n                        (exact)
 *   base64EncodeInto       4*ceil(n/3)               (exact, padded)
 *   base64UrlEncodeInto    4*ceil(n/3)               (unpadded: <= bound)
 *   base32EncodeInto       ((n+4)/5)*8               (exact, padded)
 *   base32HexEncodeInto    ((n+4)/5)*8               (exact, padded)
 *   base85EncodeInto       ((n+3)/4)*5               (exact)
 *   base58EncodeInto       (n*8)/5+1                 (bound; n==0 -> 0)
 *   base58CheckEncodeInto  ((n+4)*8)/5+1             (bound; the payload is
 *                                                      the data + 4 checksum)
 * Scratch honesty: hex/base64/base32/base85 write EXACTLY the returned count
 * at offset 0; base64UrlEncodeInto shares the base64 core, which compacts in
 * place and leaves up to 3 pad bytes PAST the count as scratch -- always
 * inside the documented bound. Nothing is ever written past the bound.
 * BaseXEncodeInto is deliberately NOT provided, matching the decode side:
 * the division codec allocates its workspace internally. */

static JSValue dyn_enc_hex_encode_into(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, need;
    const char *owned;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_hex_encode_len(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "hexEncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    dyn_codec_hex_encode(data, n, (char *)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)need);
}

static JSValue dyn_enc_base64_encode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, need, written;
    const char *owned;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_base64_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base64EncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base64_encode(data, n, (char *)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

static JSValue dyn_enc_base64url_encode_into(JSContext *ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, written;
    const char *owned;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    /* the unpadded output is never longer than the padded bound */
    if (dyn_enc_into_out_as(ctx, argv[1], dyn_codec_base64_encode_cap(n),
                            "encodeInto", &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base64UrlEncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base64url_encode(data, n, (char *)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

/* magic: DYN_BASE32_STD or DYN_BASE32_HEX (shared with the one-shots). */
static JSValue dyn_enc_base32_encode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic)
{
    const uint8_t *data;
    size_t n, need, written;
    const char *owned;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_base32_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base32EncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base32_encode(data, n, (char *)out,
                                      (dyn_base32_alphabet)magic);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

static JSValue dyn_enc_base85_encode_into(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    const uint8_t *data;
    size_t n, need, written;
    const char *owned;
    uint8_t *out;
    size_t on;
    (void)this_val; (void)argc;

    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    need = dyn_codec_base85_encode_cap(n);
    if (dyn_enc_into_out_as(ctx, argv[1], need, "encodeInto", &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "base85EncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    written = dyn_codec_base85_encode(data, n, (char *)out);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)written);
}

/* =========================================================================
 * Charset detection (P1 brief 3.4) -- BOSCC pipeline:
 * 1. BOM probe (deterministic)
 * 2. Fast UTF-8 validity probe (simd.validate_utf8) -> "utf-8"
 * 3. Statistical CJK distribution probe (GBK, Big5, Shift-JIS, EUC-JP, EUC-KR)
 * 4. Deterministic fallback + allowList filtering
 * ========================================================================= */

#define DYN_DETECT_MIN_BYTES 16

static const char *dyn_detect_cjk(const uint8_t *p, size_t n)
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

            /* EUC-JP special lead bytes: 0x8E (half-width kana), 0x8F (3-byte) */
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

            /* Shift-JIS */
            if (((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC)) &&
                ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC))) {
                sjis_hits += 2;
                if (c1 == 0x82 || c1 == 0x83)
                    sjis_kana_hits++;
            }

            /* GBK: lead 0x81..0xFE, trail 0x40..0xFE except 0x7F */
            if (c1 >= 0x81 && c1 <= 0xFE && c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                gbk_hits += 2;
            }

            /* Big5: lead 0x81..0xFE, trail 0x40..0x7E or 0xA1..0xFE */
            if (c1 >= 0x81 && c1 <= 0xFE && ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0xA1 && c2 <= 0xFE))) {
                big5_hits += 2;
            }

            /* EUC-JP / EUC-KR standard 2-byte range */
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

static JSValue dyn_enc_detect_encoding(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    uint8_t *p;
    size_t n;
    const char *verdict = NULL;
    const char *fallback = "utf-8";
    const char *fallback_alloc = NULL;
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

    /* 1. BOM probe (deterministic) */
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

    /* 2. Fast UTF-8 validity probe via SIMD kernel */
    if (!verdict) {
        if (simd.validate_utf8 && simd.validate_utf8(p, n) == n) {
            verdict = "utf-8";
        }
    }

    /* 3. Statistical CJK detector for non-UTF8 multibyte payloads */
    if (!verdict && n >= DYN_DETECT_MIN_BYTES) {
        verdict = dyn_detect_cjk(p, n);
    }

    if (!verdict)
        verdict = fallback;

    /* 4. allowList filtering if specified */
    if (JS_IsArray(ctx, allow_list)) {
        uint32_t len = 0, i;
        int found = 0;
        JSValue lv = JS_GetPropertyStr(ctx, allow_list, "length");
        JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);

        for (i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, allow_list, i);
            const char *s = JS_ToCString(ctx, item);
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
                    const char *s = JS_ToCString(ctx, item);
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
                if (fallback_alloc) JS_FreeCString(ctx, fallback_alloc);
                JS_FreeValue(ctx, allow_list);
                return JS_ThrowTypeError(ctx, "detectEncoding: detected encoding not in allowList and no valid fallback");
            }
        }
    }

    if (fallback_alloc)
        JS_FreeCString(ctx, fallback_alloc);
    JS_FreeValue(ctx, allow_list);

    return JS_NewString(ctx, verdict);
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
    JS_CFUNC_MAGIC_DEF("QREncode", 1, dyn_qr_encode, 0),
    JS_CFUNC_MAGIC_DEF("QRToString", 1, dyn_qr_encode, 1),
    JS_CFUNC_DEF("JSON5Parse", 1, dyn_json5_parse),
    JS_CFUNC_MAGIC_DEF("JSON5Stringify", 1, dyn_stringify, 0),
    JS_CFUNC_MAGIC_DEF("StableStringify", 1, dyn_stringify, 1),
    JS_CFUNC_DEF("HexEncode", 1, dyn_enc_hex_encode),
    JS_CFUNC_DEF("HexDecode", 1, dyn_enc_hex_decode),
    /*decode into a caller-owned buffer, and its encode mirror
     * (ASCII text bytes into the buffer; both sides share the
     * contract -- see the blocks). */
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
    /*the in-place forms (value alone / caller-owned buffer) */
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
