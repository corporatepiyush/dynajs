/* Legacy single-byte charsets for dyna:bytes (design 11). Bytes are storage and
   a charset is a reading of them, which is why this lives with Bytes/Text.
   Multi-byte CJK is NOT here; encodingExists() reports that honestly. */

#include "dyna-charsets.h"

/* Label matching is ASCII case-insensitive and ignores leading/trailing space,
   per the Encoding Standard; it is NOT a general Unicode fold. */
static int dyn_cs_label_eq(const char *a, const char *b, size_t bn)
{
    size_t i = 0;
    while (bn && (b[0] == ' ' || b[0] == '\t')) { b++; bn--; }
    while (bn && (b[bn - 1] == ' ' || b[bn - 1] == '\t')) bn--;
    for (i = 0; i < bn; i++) {
        char x = a[i], y = b[i];
        if (!x)
            return 0;
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return a[i] == 0;
}

/* Aliases callers actually write. Each maps to a label in DYN_CHARSETS. */
typedef struct { const char *alias, *canon; } dyn_cs_alias_t;
static const dyn_cs_alias_t DYN_CS_ALIASES[] = {
    { "latin1", "iso-8859-1" }, { "latin-1", "iso-8859-1" },
    { "l1", "iso-8859-1" }, { "iso88591", "iso-8859-1" },
    { "cp1252", "windows-1252" }, { "win1252", "windows-1252" },
    { "cp1251", "windows-1251" }, { "win1251", "windows-1251" },
    { "cp1250", "windows-1250" }, { "cp1253", "windows-1253" },
    { "cp1254", "windows-1254" }, { "cp1255", "windows-1255" },
    { "cp1256", "windows-1256" }, { "cp1257", "windows-1257" },
    { "cp1258", "windows-1258" },
    { "ascii", "us-ascii" }, { "usascii", "us-ascii" },
    { "latin2", "iso-8859-2" }, { "latin9", "iso-8859-15" },
    { "cyrillic", "iso-8859-5" }, { "greek", "iso-8859-7" },
    { "hebrew", "iso-8859-8" }, { "koi8r", "koi8-r" }, { "koi8u", "koi8-u" },
    { "mac", "macintosh" }, { "cp866", "ibm866" },
};

/* Resolve a caller's label to a table, or NULL. *is_latin1 marks the two
   charsets whose high half is the identity and therefore carry no table. */
static const dyn_charset_t *dyn_cs_find(const char *label, size_t n, int *ascii_only)
{
    size_t i;
    const char *canon = NULL;
    *ascii_only = 0;
    for (i = 0; i < countof(DYN_CS_ALIASES); i++)
        if (dyn_cs_label_eq(DYN_CS_ALIASES[i].alias, label, n)) {
            canon = DYN_CS_ALIASES[i].canon;
            break;
        }
    for (i = 0; i < countof(DYN_CHARSETS); i++) {
        int hit = canon ? (strcmp(DYN_CHARSETS[i].label, canon) == 0)
                        : dyn_cs_label_eq(DYN_CHARSETS[i].label, label, n);
        if (hit) {
            if (!DYN_CHARSETS[i].hi)
                *ascii_only = (strcmp(DYN_CHARSETS[i].label, "us-ascii") == 0) ? 2 : 1;
            return &DYN_CHARSETS[i];
        }
    }
    return NULL;
}

/* encodingExists(label) -> boolean. False for the CJK multi-byte families,
   which are not built: a caller must be able to find that out without a throw. */
static JSValue dyn_iconv_exists(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *label;
    size_t n;
    int ascii_only;
    const dyn_charset_t *cs;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "encodingExists(label): label must be a string");
    label = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!label)
        return JS_EXCEPTION;
    if (dyn_cs_label_eq("utf-8", label, n) || dyn_cs_label_eq("utf8", label, n)) {
        JS_FreeCString(ctx, label);
        return JS_TRUE;
    }
    cs = dyn_cs_find(label, n, &ascii_only);
    JS_FreeCString(ctx, label);
    return JS_NewBool(ctx, cs != NULL);
}

/* decode(bytes, label) -> string. Bytes below 0x80 are themselves in every
   charset here, so the ASCII run is copied and only the high bytes look up. */
static JSValue dyn_iconv_decode(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *label;
    size_t ln, n = 0, i, o = 0;
    uint8_t *p = NULL;
    const dyn_charset_t *cs;
    int ascii_only = 0;
    uint8_t *out;
    JSValue ret;

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "decode(bytes, label): label must be a string");
    label = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!label)
        return JS_EXCEPTION;
    /* utf-8 is not in DYN_CHARSETS -- it is the engine's own string encoding.
       encodingExists() and encodings() both advertise it, so refusing it here
       made the capability probe disagree with the operation: a caller that
       checked first still got a throw. JS_NewStringLen decodes UTF-8. */
    if (dyn_cs_label_eq("utf-8", label, ln) || dyn_cs_label_eq("utf8", label, ln)) {
        JS_FreeCString(ctx, label);
        if (dyn_bytes_view(ctx, argv[0], &p, &n) < 0)
            return JS_ThrowTypeError(ctx,
                "decode(bytes, label): bytes must be a byte-addressed view");
        return JS_NewStringLen(ctx, (const char *)p, n);
    }
    cs = dyn_cs_find(label, ln, &ascii_only);
    if (!cs) {
        JSValue e = JS_ThrowRangeError(ctx, "decode(bytes, label): unknown encoding \"%s\"", label);
        JS_FreeCString(ctx, label);
        return e;
    }
    JS_FreeCString(ctx, label);
    if (dyn_bytes_view(ctx, argv[0], &p, &n) < 0)
        return JS_ThrowTypeError(ctx, "decode(bytes, label): bytes must be a byte-addressed view");

    /* BOSCC: an input with no byte >= 0x80 decodes to itself in every charset
       here, so it needs no allocation and no per-byte loop at all. */
    {
        /* No kernel tests "any high bit", and find_u8 matches ONE exact byte --
           it cannot answer this. SWAR over words is the portable form. */
        size_t k = 0;
        uint64_t w;
        while (k + 8 <= n) {
            memcpy(&w, p + k, 8);
            if (w & 0x8080808080808080ull)
                break;
            k += 8;
        }
        while (k < n && p[k] < 0x80)
            k++;
        if (k == n)
            return JS_NewStringLen(ctx, (const char *)p, n);
    }
    /* Worst case is 3 UTF-8 bytes per input byte (a BMP code point). */
    out = (uint8_t *)js_malloc(ctx, n * 3 + 1);
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        uint32_t cp = p[i];
        if (cp < 0x80) { out[o++] = (uint8_t)cp; continue; }
        if (ascii_only == 2) {
            cp = 0xFFFD;                       /* us-ascii: high bytes are junk */
        } else if (ascii_only == 1) {
            /* iso-8859-1: the high half IS U+0080..U+00FF */
        } else {
            cp = cs->hi[cp - 0x80];
            if (cp == 0xFFFF)
                cp = 0xFFFD;                   /* undefined byte -> replacement */
        }
        if (cp < 0x800) {
            out[o++] = (uint8_t)(0xC0 | (cp >> 6));
            out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (uint8_t)(0xE0 | (cp >> 12));
            out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        }
    }
    ret = JS_NewStringLen(ctx, (const char *)out, o);
    js_free(ctx, out);
    return ret;
}

/* Next code point from UTF-8; advances *i. The engine only hands out
   well-formed UTF-8 here, so a malformed lead byte decodes as itself. */
static uint32_t dyn_cs_u8_next(const char *s, size_t n, size_t *i)
{
    unsigned char c = (unsigned char)s[(*i)++];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *i < n)
        return (uint32_t)((c & 0x1F) << 6) | ((unsigned char)s[(*i)++] & 0x3F);
    if ((c & 0xF0) == 0xE0 && *i + 1 < n) {
        uint32_t v = (uint32_t)(c & 0x0F) << 12;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 6;
        v |= (unsigned char)s[(*i)++] & 0x3F;
        return v;
    }
    if ((c & 0xF8) == 0xF0 && *i + 2 < n) {
        uint32_t v = (uint32_t)(c & 0x07) << 18;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 12;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 6;
        v |= (unsigned char)s[(*i)++] & 0x3F;
        return v;
    }
    return c;
}

/* Reverse lookup: code point -> byte, or -1. Bisects the generated reverse map;
   the forward scan it replaced was 128 compares per code point. */
static int dyn_cs_rev(const dyn_charset_t *cs, int ascii_only, uint32_t cp)
{
    unsigned lo = 0, hi;
    if (cp < 0x80)
        return (int)cp;
    if (ascii_only == 2)
        return -1;
    if (ascii_only == 1)
        return (cp <= 0xFF) ? (int)cp : -1;
    if (cp > 0xFFFF || !cs->rev)
        return -1;
    hi = cs->n_rev;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1;
        if (cp < cs->rev[mid][0])      hi = mid;
        else if (cp > cs->rev[mid][0]) lo = mid + 1;
        else                           return (int)cs->rev[mid][1];
    }
    return -1;
}

/* encode(string, label) -> Uint8Array. A code point the charset cannot express
   becomes '?' -- the substitution every legacy encoder uses. */
static JSValue dyn_iconv_encode(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *label, *src;
    size_t ln, n = 0, i = 0, o = 0;
    const dyn_charset_t *cs;
    int ascii_only = 0;
    uint8_t *out;
    JSValue ret;

    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "encode(text, label): both arguments must be strings");
    label = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!label)
        return JS_EXCEPTION;
    /* Same reason as decode: utf-8 is advertised but is not a table charset.
       JS_ToCStringLen already yields UTF-8, so this is a copy, not a convert. */
    if (dyn_cs_label_eq("utf-8", label, ln) || dyn_cs_label_eq("utf8", label, ln)) {
        JS_FreeCString(ctx, label);
        src = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!src)
            return JS_EXCEPTION;
        ret = dyn_bytes_new_u8array(ctx, (const uint8_t *)src, n);
        JS_FreeCString(ctx, src);
        return ret;
    }
    cs = dyn_cs_find(label, ln, &ascii_only);
    if (!cs) {
        JSValue e = JS_ThrowRangeError(ctx, "encode(text, label): unknown encoding \"%s\"", label);
        JS_FreeCString(ctx, label);
        return e;
    }
    JS_FreeCString(ctx, label);
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    out = (uint8_t *)js_malloc(ctx, n + 1);       /* never grows: 1 byte per cp */
    if (!out) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }
    while (i < n) {
        uint32_t cp = dyn_cs_u8_next(src, n, &i);
        int b = dyn_cs_rev(cs, ascii_only, cp);
        out[o++] = (b < 0) ? (uint8_t)'?' : (uint8_t)b;
    }
    ret = dyn_bytes_new_u8array(ctx, out, o);
    js_free(ctx, out);
    JS_FreeCString(ctx, src);
    return ret;
}

/* encodings() -> string[]: every label this build can decode. The CJK families
   are absent, and a caller enumerating this is how they find that out. */
static JSValue dyn_iconv_list(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;
    size_t i;
    if (JS_IsException(arr))
        return arr;
    if (JS_DefinePropertyValueUint32(ctx, arr, k++, JS_NewString(ctx, "utf-8"),
                                     JS_PROP_C_W_E) < 0)
        goto fail;
    for (i = 0; i < countof(DYN_CHARSETS); i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, k++,
                JS_NewString(ctx, DYN_CHARSETS[i].label), JS_PROP_C_W_E) < 0)
            goto fail;
    return arr;
 fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}
