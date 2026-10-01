#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

#include "core/dyn-hash.h"
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_UUID_STRLEN 36

static const char dyn_uuid_hex[] = "0123456789abcdef";

static int dyn_uuid_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void dyn_uuid_format(const uint8_t b[16], char out[DYN_UUID_STRLEN])
{
    int i, j = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[j++] = '-';
        out[j++] = dyn_uuid_hex[b[i] >> 4];
        out[j++] = dyn_uuid_hex[b[i] & 0x0f];
    }
}

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
    int hi = dyn_uuid_hexval((unsigned char)p[0]);
    int lo = dyn_uuid_hexval((unsigned char)p[1]);
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

static int dyn_uuid_parse_bytes(const char* s, size_t len, uint8_t b[16])
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

static int dyn_uuid_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    base = JS_GetArrayBuffer(ctx, &ab, v);
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

static JSValue dyn_uuid_new_u8array(JSContext* ctx, const uint8_t* data, size_t len)
{
    JSValue ab, out;
    JSValueConst ta_args[3];

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

static int dyn_uuid_opts_check(JSContext* ctx, JSValueConst opts,
    const char* const* allowed, size_t n_allowed,
    const char* who)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    size_t j;

    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: opts must be an object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len; i++) {
        size_t name_len = 0;
        const char* name = JS_AtomBorrowASCII(ctx, &name_len, tab[i].atom);
        int ok = 0;

        if (name) {
            for (j = 0; j < n_allowed; j++) {
                if (strcmp(name, allowed[j]) == 0) {
                    ok = 1;
                    break;
                }
            }
        }
        if (!ok) {
            const char* shown = JS_AtomToCString(ctx, tab[i].atom);
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\"", who,
                shown ? shown : "?");
            if (shown)
                JS_FreeCString(ctx, shown);
            JS_FreePropertyEnum(ctx, tab, len);
            return -1;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return 0;
}

static int dyn_uuid_opt_as(JSContext* ctx, JSValueConst opts, const char* who)
{
    JSValue as_val;
    int out = 0;

    if (!(JS_IsObject(opts)))
        return 0;
    as_val = JS_GetPropertyStr(ctx, opts, "as");
    if (JS_IsException(as_val))
        return -1;
    if (JS_IsString(as_val)) {
        size_t n;
        const char* a = JS_ToCStringLen(ctx, &n, as_val);
        if (!a) {
            JS_FreeValue(ctx, as_val);
            return -1;
        }
        if (strcmp(a, "bytes") == 0)
            out = 1;
        else if (strcmp(a, "string") != 0) {
            JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
            out = -1;
        }
        JS_FreeCString(ctx, a);
    } else if (!JS_IsUndefined(as_val)) {
        JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
        out = -1;
    }
    JS_FreeValue(ctx, as_val);
    return out;
}

static JSValue dyn_uuid_v4(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            allowed, 1, "v4(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        "v4(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_v7(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    struct timespec ts;
    uint64_t ms, ctr;
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            allowed, 1, "v7(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        "v7(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: clock_gettime failed");
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");

    {
        static _Atomic uint64_t v7_mono;
        uint64_t cur = atomic_load_explicit(&v7_mono, memory_order_relaxed);
        uint64_t fresh = ((((uint64_t)b[0] & 0xF0) << 4) | b[1]) & 0xFFF;
        uint64_t next;
        for (;;) {
            uint64_t cur_ms = cur >> 12;
            if (ms < cur_ms)
                ms = cur_ms;
            if (cur_ms == ms) {
                if ((cur & 0xFFF) == 0xFFF) {
                    ms += 1;
                    next = (ms << 12) | fresh;
                } else {
                    next = cur + 1;
                }
            } else {
                next = (ms << 12) | fresh;
            }
            if (atomic_compare_exchange_weak_explicit(
                    &v7_mono, &cur, next,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        }
        ms = next >> 12;
        ctr = next & 0xFFF;
    }

    b[0] = (uint8_t)(ms >> 40);
    b[1] = (uint8_t)(ms >> 32);
    b[2] = (uint8_t)(ms >> 24);
    b[3] = (uint8_t)(ms >> 16);
    b[4] = (uint8_t)(ms >> 8);
    b[5] = (uint8_t)ms;
    b[6] = (uint8_t)((ctr >> 8) | 0x70);
    b[7] = (uint8_t)ctr;
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_compare(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_uuid_v8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "fill" };
    uint8_t b[16];
    uint8_t* p;
    size_t n;
    JSValue fv;
    char s[DYN_UUID_STRLEN];

    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx,
            "v8(opts): opts with {fill} is required");
    if (dyn_uuid_opts_check(ctx, argv[0], allowed, 1, "v8(opts)"))
        return JS_EXCEPTION;
    fv = JS_GetPropertyStr(ctx, argv[0], "fill");
    if (JS_IsException(fv))
        return JS_EXCEPTION;
    if (JS_IsUndefined(fv)) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowTypeError(ctx, "v8(opts): fill is required");
    }
    if (dyn_uuid_view(ctx, fv, &p, &n)) {
        JS_FreeValue(ctx, fv);
        return JS_EXCEPTION;
    }
    if (n != 16) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowRangeError(ctx,
            "v8(opts): fill must be exactly 16 bytes");
    }
    memcpy(b, p, 16);
    JS_FreeValue(ctx, fv);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x80);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static int dyn_uuid_namespace(JSContext* ctx, JSValueConst v, uint8_t ns[16])
{
    if (JS_IsString(v)) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        int r;
        if (!s)
            return -1;
        r = dyn_uuid_parse_bytes(s, len, ns);
        JS_FreeCString(ctx, s);
        if (r) {
            JS_ThrowSyntaxError(ctx, "v3/v5: invalid namespace UUID");
            return -1;
        }
        return 0;
    }
    {
        uint8_t* p;
        size_t n;
        if (dyn_uuid_view(ctx, v, &p, &n))
            return -1;
        if (n != 16) {
            JS_ThrowTypeError(ctx, "v3/v5: namespace must be a UUID string or 16-byte view");
            return -1;
        }
        memcpy(ns, p, 16);
        return 0;
    }
}

static JSValue dyn_uuid_named(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    uint8_t ns[16], b[16], digest[20];
    const uint8_t* name = NULL;
    const char* owned = NULL;
    size_t namelen = 0;
    uint8_t* msg;
    char s[DYN_UUID_STRLEN];
    (void)this_val;
    (void)argc;

    if (dyn_uuid_namespace(ctx, argv[0], ns))
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        owned = JS_ToCStringLen(ctx, &namelen, argv[1]);
        if (!owned)
            return JS_EXCEPTION;
        name = (const uint8_t*)owned;
    } else {
        uint8_t* p;
        size_t n;
        if (dyn_uuid_view(ctx, argv[1], &p, &n))
            return JS_EXCEPTION;
        name = p;
        namelen = n;
    }

    msg = (uint8_t*)malloc(16 + namelen);
    if (!msg) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(msg, ns, 16);
    if (namelen)
        memcpy(msg + 16, name, namelen);
    if (magic == 5) {
        dyn_sha1(msg, 16 + namelen, digest);
        memcpy(b, digest, 16);
    } else {
        dyn_md5(msg, 16 + namelen, b);
    }
    free(msg);
    if (owned)
        JS_FreeCString(ctx, owned);

    b[6] = (uint8_t)((b[6] & 0x0f) | (magic << 4));
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static JSValue dyn_uuid_parse(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_uuid_validate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    int ok;
    (void)this_val;
    (void)argc;

    if (!JS_IsString(argv[0]))
        return JS_FALSE;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    ok = (dyn_uuid_parse_bytes(str, len, b) == 0);
    JS_FreeCString(ctx, str);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_uuid_version(JSContext* ctx, JSValueConst this_val,
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
        return JS_ThrowSyntaxError(ctx, "version: invalid UUID string");
    return JS_NewInt32(ctx, b[6] >> 4);
}

static JSValue dyn_uuid_variant(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_uuid_bytes(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_uuid_from_bytes(JSContext* ctx, JSValueConst this_val,
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

#include "dyna-nanoid.inc.c"

static const JSCFunctionListEntry dyn_uuid_funcs[] = {
    JS_CFUNC_DEF("NanoID", 0, dyn_nanoid),
    JS_CFUNC_DEF("NanoIDAlphabet", 1, dyn_nanoid_alphabet),
    JS_CFUNC_DEF("ULID", 2, dyn_ulid),
    JS_CFUNC_DEF("ULIDTime", 1, dyn_ulid_time),
    JS_CFUNC_DEF("v4", 1, dyn_uuid_v4),
    JS_CFUNC_DEF("v7", 1, dyn_uuid_v7),
    JS_CFUNC_DEF("v8", 1, dyn_uuid_v8),
    JS_CFUNC_DEF("compare", 2, dyn_uuid_compare),
    JS_CFUNC_MAGIC_DEF("v3", 2, dyn_uuid_named, 3),
    JS_CFUNC_MAGIC_DEF("v5", 2, dyn_uuid_named, 5),
    JS_CFUNC_DEF("parse", 1, dyn_uuid_parse),
    JS_CFUNC_DEF("validate", 1, dyn_uuid_validate),
    JS_CFUNC_DEF("version", 1, dyn_uuid_version),
    JS_CFUNC_DEF("variant", 1, dyn_uuid_variant),
    JS_CFUNC_DEF("bytes", 1, dyn_uuid_bytes),
    JS_CFUNC_DEF("fromBytes", 1, dyn_uuid_from_bytes),
    JS_PROP_STRING_DEF("NIL", "00000000-0000-0000-0000-000000000000", 0),
    JS_PROP_STRING_DEF("MAX", "ffffffff-ffff-ffff-ffff-ffffffffffff", 0),
    JS_PROP_STRING_DEF("NAMESPACE_DNS", "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_URL", "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_OID", "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_X500", "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 0),
};

static int dyn_uuid_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

int js_nat_init_uuid(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:uuid", dyn_uuid_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

#endif
