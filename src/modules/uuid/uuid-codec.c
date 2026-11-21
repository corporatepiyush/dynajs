#include "uuid-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

static int dyn_uuid_ci_ne(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 1;
    }
    return 0;
}

static int dyn_uuid_hexpair(const char* p, uint8_t* out)
{
    int hi = dyn_hexval((unsigned char)p[0]);
    int lo = dyn_hexval((unsigned char)p[1]);
    if (hi < 0 || lo < 0)
        return -1;
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

static int dyn_uuid_parse_canonical(const char* s, uint8_t b[16])
{
    static const int pos[16] = {
        0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34
    };
    int i;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return -1;
    for (i = 0; i < 16; i++)
        if (dyn_uuid_hexpair(s + pos[i], &b[i]))
            return -1;
    return 0;
}

int dyn_uuid_parse_bytes(const char* s, size_t len, uint8_t b[16])
{
    int i;
    switch (len) {
    case 36:
        return dyn_uuid_parse_canonical(s, b);
    case 45:
        if (dyn_uuid_ci_ne(s, "urn:uuid:", 9))
            return -1;
        return dyn_uuid_parse_canonical(s + 9, b);
    case 38:
        if (s[0] != '{' || s[37] != '}')
            return -1;
        return dyn_uuid_parse_canonical(s + 1, b);
    case 32:
        for (i = 0; i < 16; i++)
            if (dyn_uuid_hexpair(s + i * 2, &b[i]))
                return -1;
        return 0;
    default:
        return -1;
    }
}

static const char* dyn_uuid_variant_name(uint8_t b8)
{
    if ((b8 & 0xc0) == 0x80)
        return "RFC4122";
    if ((b8 & 0xe0) == 0xc0)
        return "Microsoft";
    if ((b8 & 0xe0) == 0xe0)
        return "Future";
    return "NCS";
}

int dyn_uuid_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    base = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &ab, v) : NULL);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));

    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Uint8Array or ArrayBuffer");
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

JSValue dyn_uuid_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *sa, *sb;
    size_t la, lb;
    uint8_t ba[16], bb[16];
    int r;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "compare(a, b): two UUID strings are required");
    sa = JS_ToCStringLen(ctx, &la, argv[0]);
    if (!sa)
        return JS_EXCEPTION;
    sb = JS_ToCStringLen(ctx, &lb, argv[1]);
    if (!sb) {
        JS_FreeCString(ctx, sa);
        return JS_EXCEPTION;
    }
    if (dyn_uuid_parse_bytes(sa, la, ba) || dyn_uuid_parse_bytes(sb, lb, bb)) {
        JS_FreeCString(ctx, sa);
        JS_FreeCString(ctx, sb);
        return JS_ThrowTypeError(ctx, "compare(a, b): invalid UUID string");
    }
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    r = memcmp(ba, bb, 16);
    return JS_NewInt32(ctx, (r < 0) ? -1 : (r > 0));
}

JSValue dyn_uuid_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    char out[DYN_UUID_STRLEN];
    int r;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "parse: invalid UUID string");
    dyn_uuid_format(b, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

JSValue dyn_uuid_variant(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "variant: invalid UUID string");
    return JS_NewString(ctx, dyn_uuid_variant_name(b[8]));
}

JSValue dyn_uuid_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "bytes: invalid UUID string");
    return dyn_uuid_new_u8array(ctx, b, 16);
}

JSValue dyn_uuid_from_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* p;
    size_t n;
    char out[DYN_UUID_STRLEN];
    (void)this_val;
    (void)argc;

    if (dyn_uuid_view(ctx, argv[0], &p, &n))
        return JS_EXCEPTION;
    if (n != 16)
        return JS_ThrowRangeError(ctx, "fromBytes: expected exactly 16 bytes");
    dyn_uuid_format(p, out);
    return JS_NewStringLen(ctx, out, DYN_UUID_STRLEN);
}

#endif
