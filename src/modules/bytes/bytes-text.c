#include "bytes-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

static int dyn_text_bytes(JSContext* ctx, JSValueConst v, const uint8_t** data,
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
        size_t off = 0, blen = 0, bpe = 0;
        JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &blen, &bpe) : JS_EXCEPTION;
        if (!JS_IsException(buf)) {
            size_t absize = 0;
            uint8_t* ab;
            if (bpe != 1) {
                JS_FreeValue(ctx, buf);
                JS_ThrowTypeError(ctx, "expected a byte view (Uint8Array, Int8Array, "
                                       "Uint8ClampedArray, DataView) or an ArrayBuffer; "
                                       "view a wider array as new Uint8Array(v.buffer, v.byteOffset, v.byteLength)");
                return -1;
            }
            ab = JS_GetArrayBuffer(ctx, &absize, buf);
            JS_FreeValue(ctx, buf);
            if (!ab)
                return -1;
            *data = ab + off;
            *len = blen;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        uint8_t* ab = JS_GetArrayBuffer(ctx, len, v);
        if (ab) {
            *data = ab;
            return 0;
        }
        if (JS_IsObject(v))
            return -1;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        const char* s = JS_ToCStringLen(ctx, len, v);
        if (!s)
            return -1;
        *owned = s;
        *data = (const uint8_t*)s;
        return 0;
    }
}

JSValue dyn_text_index_of_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t *data, *set;
    size_t len, setlen, pos, start = 0;
    const char *owned_data, *owned_set;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOfAny(text, chars)");
    if (dyn_text_bytes(ctx, argv[1], &set, &setlen, &owned_set))
        return JS_EXCEPTION;
    if (dyn_bytes_from_index(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &start)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned_data)) {
        if (owned_set)
            JS_FreeCString(ctx, owned_set);
        return JS_EXCEPTION;
    }
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > len)
        start = len;
    pos = simd.find_first_of(data + start, len - start, set, setlen);
    if (owned_data)
        JS_FreeCString(ctx, owned_data);
    if (owned_set)
        JS_FreeCString(ctx, owned_set);
    if (pos == SIZE_MAX)
        return JS_NewInt32(ctx, -1);
    return JS_NewInt64(ctx, (int64_t)(pos + start));
}

JSValue dyn_text_is_valid_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    int ok;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    ok = (simd.validate_utf8(data, len) == len);
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewBool(ctx, ok);
}

JSValue dyn_text_latin1_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_len;
    const char* owned;
    uint8_t* out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "latin1ToUtf8(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t*)dyn_nat_malloc(len ? len * 2 : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    out_len = simd.latin1_to_utf8(data, len, out);
    if (owned)
        JS_FreeCString(ctx, owned);
    result = dyn_nat_u8_copy(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

JSValue dyn_text_utf8_to_latin1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_len = 0;
    const char* owned;
    uint8_t* out;
    JSValue result;
    int rc, bad_utf8 = 0;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToLatin1(bytes)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint8_t*)dyn_nat_malloc(len ? len : 1);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_latin1(data, len, out, &out_len);
    if (rc != 0)
        bad_utf8 = (simd.validate_utf8(data, len) != len);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        dyn_nat_free(out);
        if (bad_utf8)
            return JS_ThrowRangeError(ctx, "utf8ToLatin1: invalid UTF-8");
        return JS_ThrowRangeError(
            ctx, "utf8ToLatin1: code point > 0xFF is not representable in latin1");
    }
    result = dyn_nat_u8_copy(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

JSValue dyn_text_count_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf8(data)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    {
        size_t c = simd.count_utf8(data, len);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewInt64(ctx, (int64_t)c);
    }
}

static uint16_t* dyn_text_u16_copy(JSContext* ctx, JSValueConst v, size_t* units,
    int* odd)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint16_t* u16;

    if (dyn_text_bytes(ctx, v, &data, &len, &owned))
        return NULL;
    *odd = (int)(len & 1);
    *units = len >> 1;
    u16 = (uint16_t*)dyn_nat_malloc(*units ? *units * 2 : 2);
    if (!u16) {
        if (owned)
            JS_FreeCString(ctx, owned);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(u16, data, *units * 2);
    if (owned)
        JS_FreeCString(ctx, owned);
    return u16;
}

JSValue dyn_text_utf8_to_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const uint8_t* data;
    size_t len, out_units = 0;
    const char* owned;
    uint16_t* out;
    JSValue result;
    int rc;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf8ToUtf16(bytesOrString)");
    if (dyn_text_bytes(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    out = (uint16_t*)dyn_nat_malloc(len ? len * 2 : 2);
    if (!out) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf8_to_utf16le(data, len, out, &out_units);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (rc != 0) {
        dyn_nat_free(out);
        return JS_ThrowRangeError(ctx, "utf8ToUtf16: invalid UTF-8");
    }
    result = dyn_nat_u8_copy(ctx, (const uint8_t*)out, out_units * 2);
    dyn_nat_free(out);
    return result;
}

JSValue dyn_text_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0, out_len = 0;
    int odd, rc;
    uint16_t* u16;
    uint8_t* out;
    JSValue result;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "utf16ToUtf8(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        dyn_nat_free(u16);
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: byte length must be even");
    }
    out = (uint8_t*)dyn_nat_malloc(units ? units * 3 : 1);
    if (!out) {
        dyn_nat_free(u16);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = simd.utf16le_to_utf8(u16, units, out, &out_len);
    dyn_nat_free(u16);
    if (rc != 0) {
        dyn_nat_free(out);
        return JS_ThrowRangeError(ctx,
            "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    result = dyn_nat_u8_copy(ctx, out, out_len);
    dyn_nat_free(out);
    return result;
}

JSValue dyn_text_is_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0;
    int odd, ok;
    uint16_t* u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "isValidUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    ok = !odd && simd.validate_utf16le(u16, units);
    dyn_nat_free(u16);
    return JS_NewBool(ctx, ok);
}

JSValue dyn_text_count_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t units = 0, c;
    int odd;
    uint16_t* u16;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "countUtf16(u16bytes)");
    u16 = dyn_text_u16_copy(ctx, argv[0], &units, &odd);
    if (!u16)
        return JS_EXCEPTION;
    if (odd) {
        dyn_nat_free(u16);
        return JS_ThrowRangeError(ctx, "countUtf16: byte length must be even");
    }
    c = simd.count_utf16(u16, units);
    dyn_nat_free(u16);
    return JS_NewInt64(ctx, (int64_t)c);
}

void dyn_txt_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_txt_t* t = (dyn_txt_t*)JS_GetOpaque(val, dyn_txt_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->str);
    dyn_nat_free(t);
}

void dyn_txt_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_txt_t* t = (dyn_txt_t*)JS_GetOpaque(val, dyn_txt_class_id);
    if (t)
        JS_MarkValue(rt, t->str, mark_func);
}

dyn_txt_t* dyn_txt_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_txt_t*)JS_GetOpaque2(ctx, v, dyn_txt_class_id);
}

JSValue dyn_txt_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_txt_t* t;
    JSValue str, obj, proto;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Text(s) requires a string");
    proto = dyn_ctor_proto(ctx, new_target, dyn_txt_class_id);
    if (JS_IsException(proto))
        return proto;
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, proto);
        return str;
    }

    t = (dyn_txt_t*)dyn_nat_calloc(1, sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, str);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        size_t n = 0;
        const char* cs = JS_ToCStringLen(ctx, &n, str);
        if (!cs) {
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, str);
            dyn_nat_free(t);
            return JS_EXCEPTION;
        }
        t->is_wide = 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)cs[i] >= 0xC4) {
                t->is_wide = 1;
                break;
            }
        JS_FreeCString(ctx, cs);
    }
    t->str = str;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_txt_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, str);
        dyn_nat_free(t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    return obj;
}

static size_t dyn_txt_u16_scan(JSContext* ctx, dyn_txt_t* t, int* valid)
{
    const uint8_t* p;
    const char* cs;
    size_t n, i, units = 0;

    {
        size_t bn;
        p = JS_GetNarrowStringBytes(ctx, t->str, &bn);
        if (p) {
            if (valid)
                *valid = 1;
            return bn;
        }
    }
    cs = JS_ToCStringLen2(ctx, &n, t->str, 1);
    if (!cs) {
        if (valid)
            *valid = 0;
        return 0;
    }
    p = (const uint8_t*)cs;
    i = 0;
    while (i < n) {
        if (p[i] < 0x80) {
            units++;
            i++;
        } else if ((p[i] & 0xE0) == 0xC0) {
            units++;
            i += 2;
        } else if (p[i] == 0xED && i + 2 < n && p[i + 1] >= 0xA0 && p[i + 1] <= 0xAF) {
            if (i + 5 < n && p[i + 3] == 0xED && p[i + 4] >= 0xB0 && p[i + 4] <= 0xBF) {
                units++;
                i += 6;
            } else {
                units++;
                if (valid) {
                    *valid = 0;
                    JS_FreeCString(ctx, cs);
                    return 0;
                }
                i += 3;
            }
        } else if (p[i] == 0xED && i + 2 < n && p[i + 1] >= 0xB0 && p[i + 1] <= 0xBF) {
            units++;
            if (valid) {
                *valid = 0;
                JS_FreeCString(ctx, cs);
                return 0;
            }
            i += 3;
        } else {
            units++;
            i += ((p[i] & 0xF0) == 0xE0) ? 3 : 1;
        }
    }
    JS_FreeCString(ctx, cs);
    if (valid)
        *valid = 1;
    return units;
}

JSValue dyn_txt_m_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    int valid = 0;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    dyn_txt_u16_scan(ctx, t, &valid);
    return JS_NewBool(ctx, valid);
}

JSValue dyn_txt_m_count_utf16_str(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    size_t c;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    c = dyn_txt_u16_scan(ctx, t, NULL);
    return JS_NewInt64(ctx, (int64_t)c);
}

JSValue dyn_txt_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    JSValueConst a[1];
    if (!t)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, t->is_wide);
    if (magic == 4) {
        a[0] = t->str;
        return dyn_text_is_valid_utf8(ctx, JS_UNDEFINED, 1, a);
    }
    return JS_DupValue(ctx, t->str);
}

JSValue dyn_txt_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, t->str);
}

static JSValue dyn_txt_m_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    size_t sl;
    const char* cs;
    JSValue u8;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    cs = JS_ToCStringLen(ctx, &sl, t->str);
    if (!cs)
        return JS_EXCEPTION;
    u8 = dyn_nat_u8_copy(ctx, (const uint8_t*)cs, sl);
    JS_FreeCString(ctx, cs);
    return u8;
}

JSValue dyn_txt_to_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue u8 = dyn_txt_m_to_utf8(ctx, this_val, 0, NULL);
    if (JS_IsException(u8))
        return u8;
    return dyn_bh_wrap(ctx, u8);
}

JSValue dyn_txt_m_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_txt_t* t = dyn_txt_of(ctx, this_val);
    int valid = 0;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    dyn_txt_u16_scan(ctx, t, &valid);
    if (!valid) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_ThrowRangeError(ctx, "utf16ToUtf8: ill-formed UTF-16 surrogate");
    }
    return dyn_txt_m_to_utf8(ctx, this_val, 0, NULL);
}

#endif
