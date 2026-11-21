#include "vserialize-internal.h"
#include "vserialize-bson.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_DOCDB))

#include <pthread.h>
#include <stdio.h>

typedef struct {
    uint8_t b[16];
    uint8_t sub;
    size_t len;
    uint8_t* data;
    char* s1;
    char* s2;
} DynBsonObj;

static DynBsonJsKinds bsonjs_kinds;
static pthread_once_t bsonjs_once = PTHREAD_ONCE_INIT;

static const char* const bsonjs_names[DYN_BSONJS_NCLASSES] = {
    "ObjectId",
    "Timestamp",
    "Decimal128",
    "Binary",
    "BSONRegExp",
    "MinKey",
    "MaxKey",
    "Code",
};

static void bsonjs_finalizer(JSRuntime* rt, JSValue val)
{
    int i;
    (void)rt;
    for (i = 0; i < DYN_BSONJS_NCLASSES; i++) {
        DynBsonObj* o = JS_GetOpaque(val, bsonjs_kinds.cls[i]);
        if (o) {
            free(o->data);
            free(o->s1);
            free(o->s2);
            free(o);
            return;
        }
    }
}

static JSClassDef bsonjs_defs[DYN_BSONJS_NCLASSES];

static void bsonjs_alloc_ids(void)
{
    int i;
    for (i = 0; i < DYN_BSONJS_NCLASSES; i++) {
        JS_NewClassID(&bsonjs_kinds.cls[i]);
        bsonjs_defs[i].class_name = bsonjs_names[i];
        bsonjs_defs[i].finalizer = bsonjs_finalizer;
    }
}

static DynBsonObj* bsonjs_get(JSContext* ctx, JSValueConst v, int which)
{
    return JS_GetOpaque2(ctx, v, bsonjs_kinds.cls[which]);
}

static JSValue bsonjs_wrap(JSContext* ctx, JSValueConst new_target, int which, DynBsonObj* o)
{
    JSValue proto, obj;
    proto = dyn_ctor_proto(ctx, new_target, bsonjs_kinds.cls[which]);
    if (JS_IsException(proto))
        goto fail;
    obj = JS_NewObjectProtoClass(ctx, proto, bsonjs_kinds.cls[which]);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, o);
    return obj;
fail:
    free(o->data);
    free(o->s1);
    free(o->s2);
    free(o);
    return JS_EXCEPTION;
}

static DynBsonObj* bsonjs_new_payload(JSContext* ctx)
{
    DynBsonObj* o = calloc(1, sizeof *o);
    if (!o)
        JS_ThrowOutOfMemory(ctx);
    return o;
}

static int bsonjs_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int bsonjs_parse_hex24(const char* s, size_t n, uint8_t out[12])
{
    int i;
    if (n != 24)
        return -1;
    for (i = 0; i < 12; i++) {
        int hi = bsonjs_hexval((unsigned char)s[2 * i]), lo = bsonjs_hexval((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static void bsonjs_hex(const uint8_t* p, size_t n, char* out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int bsonjs_bytes_view(JSContext* ctx, JSValueConst v, const uint8_t** pp, size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    int kind = JS_GetBufferKind(v);
    *hold = JS_UNDEFINED;
    if (kind == JS_BUFFER_KIND_BUFFER) {
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (kind != JS_BUFFER_KIND_VIEW)
        return 1;
    *hold = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(*hold))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, *hold);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, *hold);
        *hold = JS_UNDEFINED;
        if (base)
            JS_ThrowRangeError(ctx, "byte view out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

int dyn_bsonjs_oid_of(JSContext* ctx, JSValueConst v, uint8_t out[12])
{
    DynBsonObj* o;
    (void)ctx;
    if (!bsonjs_kinds.cls[DYN_BSONJS_OBJECTID])
        return 0;
    o = JS_GetOpaque(v, bsonjs_kinds.cls[DYN_BSONJS_OBJECTID]);
    if (!o)
        return 0;
    memcpy(out, o->b, 12);
    return 1;
}

JSValue dyn_bsonjs_new_oid(JSContext* ctx, const uint8_t oid[12])
{
    DynBsonObj* o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    memcpy(o->b, oid, 12);
    return bsonjs_wrap(ctx, JS_UNDEFINED, DYN_BSONJS_OBJECTID, o);
}

static int bsonjs_oid_from_arg(JSContext* ctx, JSValueConst v, uint8_t out[12])
{
    const uint8_t* p;
    size_t n;
    JSValue hold;
    int rc;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        dyn_bson_oid_generate(out);
        return 0;
    }
    if (dyn_bsonjs_oid_of(ctx, v, out))
        return 0;
    if (JS_IsString(v)) {
        size_t sl;
        const char* s = JS_ToCStringLen(ctx, &sl, v);
        if (!s)
            return -1;
        rc = bsonjs_parse_hex24(s, sl, out);
        JS_FreeCString(ctx, s);
        if (rc < 0) {
            JS_ThrowTypeError(ctx, "ObjectId: expected 24 hexadecimal characters");
            return -1;
        }
        return 0;
    }
    rc = bsonjs_bytes_view(ctx, v, &p, &n, &hold);
    if (rc < 0)
        return -1;
    if (rc == 0) {
        if (n != 12) {
            JS_FreeValue(ctx, hold);
            JS_ThrowTypeError(ctx, "ObjectId: expected 12 bytes, got %zu", n);
            return -1;
        }
        memcpy(out, p, 12);
        JS_FreeValue(ctx, hold);
        return 0;
    }
    JS_ThrowTypeError(ctx, "ObjectId: expected a 24-character hex string, 12 bytes or an ObjectId");
    return -1;
}

static JSValue bsonjs_oid_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    DynBsonObj* o;
    uint8_t b[12];
    if (bsonjs_oid_from_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, b) < 0)
        return JS_EXCEPTION;
    o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    memcpy(o->b, b, 12);
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_OBJECTID, o);
}

static JSValue bsonjs_oid_hex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char buf[25];
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_OBJECTID);
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    bsonjs_hex(o->b, 12, buf);
    return JS_NewStringLen(ctx, buf, 24);
}

static JSValue bsonjs_oid_equals(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint8_t other[12];
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_OBJECTID);
    if (!o)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_FALSE;
    if (dyn_bsonjs_oid_of(ctx, argv[0], other))
        return JS_NewBool(ctx, memcmp(other, o->b, 12) == 0);
    if (JS_IsString(argv[0])) {
        size_t sl;
        const char* s = JS_ToCStringLen(ctx, &sl, argv[0]);
        int ok;
        if (!s)
            return JS_EXCEPTION;
        ok = bsonjs_parse_hex24(s, sl, other) == 0 && memcmp(other, o->b, 12) == 0;
        JS_FreeCString(ctx, s);
        return JS_NewBool(ctx, ok);
    }
    return JS_FALSE;
}

static JSValue bsonjs_oid_ts(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_OBJECTID);
    uint32_t secs;
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    secs = (uint32_t)o->b[0] << 24 | (uint32_t)o->b[1] << 16 | (uint32_t)o->b[2] << 8 | o->b[3];
    return JS_NewDate(ctx, (double)secs * 1000.0);
}

static JSValue bsonjs_oid_id(JSContext* ctx, JSValueConst this_val)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_OBJECTID);
    if (!o)
        return JS_EXCEPTION;
    return dyn_nat_u8_copy(ctx, o->b, 12);
}

static JSValue bsonjs_oid_is_valid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint8_t b[12];
    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_FALSE;
    if (bsonjs_oid_from_arg(ctx, argv[0], b) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_FALSE;
    }
    return JS_TRUE;
}

static JSValue bsonjs_oid_from_time(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double secs;
    uint8_t b[12] = { 0 };
    uint32_t s;
    (void)this_val;
    if (argc < 1 || JS_ToFloat64(ctx, &secs, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "ObjectId.createFromTime(seconds): seconds are required") : JS_EXCEPTION;
    if (!(secs >= 0 && secs <= 4294967295.0))
        return JS_ThrowRangeError(ctx, "ObjectId.createFromTime: seconds must be within 0..2^32-1");
    s = (uint32_t)secs;
    b[0] = (uint8_t)(s >> 24);
    b[1] = (uint8_t)(s >> 16);
    b[2] = (uint8_t)(s >> 8);
    b[3] = (uint8_t)s;
    return dyn_bsonjs_new_oid(ctx, b);
}

static int bsonjs_u32_arg(JSContext* ctx, JSValueConst v, const char* what, uint32_t* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0 && d <= 4294967295.0) || d != (double)(uint32_t)d) {
        JS_ThrowRangeError(ctx, "Timestamp: %s must be an integer within 0..2^32-1", what);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue bsonjs_ts_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    uint32_t t = 0, i = 0;
    DynBsonObj* o;
    JSValueConst a = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsBigInt(ctx, a)) {
        int64_t u;
        if (JS_ToBigInt64(ctx, &u, a))
            return JS_EXCEPTION;
        t = (uint32_t)((uint64_t)u >> 32);
        i = (uint32_t)(uint64_t)u;
    } else if (JS_IsObject(a)) {
        JSValue tv = JS_GetPropertyStr(ctx, a, "t"), iv;
        if (JS_IsException(tv))
            return JS_EXCEPTION;
        if (bsonjs_u32_arg(ctx, tv, "t", &t) < 0) {
            JS_FreeValue(ctx, tv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, tv);
        iv = JS_GetPropertyStr(ctx, a, "i");
        if (JS_IsException(iv))
            return JS_EXCEPTION;
        if (bsonjs_u32_arg(ctx, iv, "i", &i) < 0) {
            JS_FreeValue(ctx, iv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, iv);
    } else {
        return JS_ThrowTypeError(ctx, "Timestamp: expected { t, i } or a BigInt");
    }
    o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    dyn_bson_wr32(o->b, i);
    dyn_bson_wr32(o->b + 4, t);
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_TIMESTAMP, o);
}

static JSValue bsonjs_ts_field(JSContext* ctx, JSValueConst this_val, int magic)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_TIMESTAMP);
    if (!o)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_bson_rd32(o->b + (magic ? 0 : 4)));
}

static JSValue bsonjs_ts_tostring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char buf[64];
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_TIMESTAMP);
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    snprintf(buf, sizeof buf, "Timestamp(%u, %u)", dyn_bson_rd32(o->b + 4), dyn_bson_rd32(o->b));
    return JS_NewString(ctx, buf);
}

static JSValue bsonjs_ts_tojson(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_TIMESTAMP);
    JSValue outer, inner;
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    inner = JS_NewObject(ctx);
    if (JS_IsException(inner))
        return inner;
    JS_DefinePropertyValueStr(ctx, inner, "t", JS_NewUint32(ctx, dyn_bson_rd32(o->b + 4)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, inner, "i", JS_NewUint32(ctx, dyn_bson_rd32(o->b)), JS_PROP_C_W_E);
    outer = JS_NewObject(ctx);
    if (JS_IsException(outer)) {
        JS_FreeValue(ctx, inner);
        return outer;
    }
    JS_DefinePropertyValueStr(ctx, outer, "$timestamp", inner, JS_PROP_C_W_E);
    return outer;
}

static JSValue bsonjs_dec_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    DynBsonObj* o;
    uint8_t b[16];
    size_t n;
    const char* s;
    int rc;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Decimal128: expected a numeric string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    rc = dyn_bson_dec128_from_string(s, n, b);
    JS_FreeCString(ctx, s);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "Decimal128: more than 34 significant digits would be rounded");
    if (rc == -3)
        return JS_ThrowRangeError(ctx, "Decimal128: exponent out of range");
    if (rc < 0)
        return JS_ThrowTypeError(ctx, "Decimal128: not a decimal number string");
    o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    memcpy(o->b, b, 16);
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_DECIMAL128, o);
}

static JSValue bsonjs_dec_tostring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char buf[64];
    int n;
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_DECIMAL128);
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    n = dyn_bson_dec128_to_string(o->b, buf, sizeof buf);
    return JS_NewStringLen(ctx, buf, (size_t)n);
}

static JSValue bsonjs_dec_tojson(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue s = bsonjs_dec_tostring(ctx, this_val, argc, argv), o;
    if (JS_IsException(s))
        return s;
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, s);
        return o;
    }
    JS_DefinePropertyValueStr(ctx, o, "$numberDecimal", s, JS_PROP_C_W_E);
    return o;
}

static JSValue bsonjs_dec_tonumber(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double d;
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_DECIMAL128);
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    dyn_bson_dec128_to_f64(o->b, &d);
    return JS_NewFloat64(ctx, d);
}

static JSValue bsonjs_bin_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    const uint8_t* p;
    size_t n;
    JSValue hold;
    int32_t sub = 0;
    DynBsonObj* o;
    int rc;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Binary(bytes, subType): bytes are required");
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32(ctx, &sub, argv[1]))
            return JS_EXCEPTION;
        if (sub < 0 || sub > 255)
            return JS_ThrowRangeError(ctx, "Binary: subType must be within 0..255");
    }
    rc = bsonjs_bytes_view(ctx, argv[0], &p, &n, &hold);
    if (rc < 0)
        return JS_EXCEPTION;
    if (rc > 0)
        return JS_ThrowTypeError(ctx, "Binary: bytes must be an ArrayBuffer or a view");
    if (n > DYN_BSON_MAX_SIZE - 16) {
        JS_FreeValue(ctx, hold);
        return JS_ThrowRangeError(ctx, "Binary: payload too large");
    }
    o = bsonjs_new_payload(ctx);
    if (!o) {
        JS_FreeValue(ctx, hold);
        return JS_EXCEPTION;
    }
    o->data = malloc(n ? n : 1);
    if (!o->data) {
        JS_FreeValue(ctx, hold);
        free(o);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (n)
        memcpy(o->data, p, n);
    JS_FreeValue(ctx, hold);
    o->len = n;
    o->sub = (uint8_t)sub;
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_BINARY, o);
}

static JSValue bsonjs_bin_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_BINARY);
    if (!o)
        return JS_EXCEPTION;
    if (magic == 0)
        return dyn_nat_u8_copy(ctx, o->data, o->len);
    if (magic == 1)
        return JS_NewInt32(ctx, o->sub);
    return JS_NewFloat64(ctx, (double)o->len);
}

static char* bsonjs_cstr_dup(JSContext* ctx, JSValueConst v, const char* what)
{
    size_t n;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    char* r;
    if (!s)
        return NULL;
    if (memchr(s, 0, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "%s must not contain a NUL character", what);
        return NULL;
    }
    r = malloc(n + 1);
    if (!r) {
        JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(r, s, n + 1);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue bsonjs_re_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    DynBsonObj* o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    o->s1 = bsonjs_cstr_dup(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "BSONRegExp pattern");
    if (o->s1)
        o->s2 = argc > 1 && !JS_IsUndefined(argv[1]) ? bsonjs_cstr_dup(ctx, argv[1], "BSONRegExp options")
                                                     : calloc(1, 1);
    if (!o->s1 || !o->s2) {
        if (o->s1 && !o->s2 && !JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        free(o->s1);
        free(o->s2);
        free(o);
        return JS_EXCEPTION;
    }
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_REGEXP, o);
}

static JSValue bsonjs_re_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_REGEXP);
    if (!o)
        return JS_EXCEPTION;
    return JS_NewString(ctx, magic ? o->s2 : o->s1);
}

static JSValue bsonjs_code_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    DynBsonObj* o;
    size_t n;
    const char* s;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Code(source): source must be a string");
    o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) {
        free(o);
        return JS_EXCEPTION;
    }
    o->data = malloc(n + 1);
    if (!o->data) {
        JS_FreeCString(ctx, s);
        free(o);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(o->data, s, n + 1);
    o->len = n;
    JS_FreeCString(ctx, s);
    return bsonjs_wrap(ctx, new_target, DYN_BSONJS_CODE, o);
}

static JSValue bsonjs_code_get(JSContext* ctx, JSValueConst this_val)
{
    DynBsonObj* o = bsonjs_get(ctx, this_val, DYN_BSONJS_CODE);
    if (!o)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, (const char*)o->data, o->len);
}

static JSValue bsonjs_minmax_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv,
    int magic)
{
    DynBsonObj* o = bsonjs_new_payload(ctx);
    (void)argc;
    (void)argv;
    if (!o)
        return JS_EXCEPTION;
    return bsonjs_wrap(ctx, new_target, magic, o);
}

static JSValue bsonjs_min_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    return bsonjs_minmax_ctor(ctx, new_target, argc, argv, DYN_BSONJS_MINKEY);
}

static JSValue bsonjs_max_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    return bsonjs_minmax_ctor(ctx, new_target, argc, argv, DYN_BSONJS_MAXKEY);
}

static JSValue bsonjs_minmax_tojson(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
    int magic)
{
    JSValue o;
    (void)argc;
    (void)argv;
    if (!bsonjs_get(ctx, this_val, magic))
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, magic == DYN_BSONJS_MINKEY ? "$minKey" : "$maxKey", JS_NewInt32(ctx, 1),
        JS_PROP_C_W_E);
    return o;
}

static const JSCFunctionListEntry bsonjs_oid_proto[] = {
    JS_CFUNC_DEF("toHexString", 0, bsonjs_oid_hex),
    JS_CFUNC_DEF("toString", 0, bsonjs_oid_hex),
    JS_CFUNC_DEF("toJSON", 0, bsonjs_oid_hex),
    JS_CFUNC_DEF("equals", 1, bsonjs_oid_equals),
    JS_CFUNC_DEF("getTimestamp", 0, bsonjs_oid_ts),
    JS_CGETSET_DEF("id", bsonjs_oid_id, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "ObjectId", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry bsonjs_oid_static[] = {
    JS_CFUNC_DEF("isValid", 1, bsonjs_oid_is_valid),
    JS_CFUNC_DEF("createFromTime", 1, bsonjs_oid_from_time),
};

static const JSCFunctionListEntry bsonjs_ts_proto[] = {
    JS_CGETSET_MAGIC_DEF("t", bsonjs_ts_field, NULL, 0),
    JS_CGETSET_MAGIC_DEF("i", bsonjs_ts_field, NULL, 1),
    JS_CFUNC_DEF("toString", 0, bsonjs_ts_tostring),
    JS_CFUNC_DEF("toJSON", 0, bsonjs_ts_tojson),
};

static const JSCFunctionListEntry bsonjs_dec_proto[] = {
    JS_CFUNC_DEF("toString", 0, bsonjs_dec_tostring),
    JS_CFUNC_DEF("toJSON", 0, bsonjs_dec_tojson),
    JS_CFUNC_DEF("toNumber", 0, bsonjs_dec_tonumber),
};

static const JSCFunctionListEntry bsonjs_bin_proto[] = {
    JS_CGETSET_MAGIC_DEF("buffer", bsonjs_bin_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("subType", bsonjs_bin_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("length", bsonjs_bin_get, NULL, 2),
};

static const JSCFunctionListEntry bsonjs_re_proto[] = {
    JS_CGETSET_MAGIC_DEF("pattern", bsonjs_re_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("options", bsonjs_re_get, NULL, 1),
};

static const JSCFunctionListEntry bsonjs_code_proto[] = {
    JS_CGETSET_DEF("code", bsonjs_code_get, NULL),
};

static const JSCFunctionListEntry bsonjs_min_proto[] = {
    JS_CFUNC_MAGIC_DEF("toJSON", 0, bsonjs_minmax_tojson, DYN_BSONJS_MINKEY),
};

static const JSCFunctionListEntry bsonjs_max_proto[] = {
    JS_CFUNC_MAGIC_DEF("toJSON", 0, bsonjs_minmax_tojson, DYN_BSONJS_MAXKEY),
};

typedef struct {
    const JSCFunctionListEntry* proto;
    int nproto;
    const JSCFunctionListEntry* stat;
    int nstat;
    JSCFunction* ctor;
} bsonjs_spec_t;

static const bsonjs_spec_t bsonjs_specs[DYN_BSONJS_NCLASSES] = {
    { bsonjs_oid_proto, countof(bsonjs_oid_proto), bsonjs_oid_static, countof(bsonjs_oid_static),
        bsonjs_oid_ctor },
    { bsonjs_ts_proto, countof(bsonjs_ts_proto), NULL, 0, bsonjs_ts_ctor },
    { bsonjs_dec_proto, countof(bsonjs_dec_proto), NULL, 0, bsonjs_dec_ctor },
    { bsonjs_bin_proto, countof(bsonjs_bin_proto), NULL, 0, bsonjs_bin_ctor },
    { bsonjs_re_proto, countof(bsonjs_re_proto), NULL, 0, bsonjs_re_ctor },
    { bsonjs_min_proto, countof(bsonjs_min_proto), NULL, 0, bsonjs_min_ctor },
    { bsonjs_max_proto, countof(bsonjs_max_proto), NULL, 0, bsonjs_max_ctor },
    { bsonjs_code_proto, countof(bsonjs_code_proto), NULL, 0, bsonjs_code_ctor },
};

static int bsonjs_builtin_ids(JSContext* ctx)
{
    JSValue d, g, re_ctor, re, arg;
    if (bsonjs_kinds.date_cls && bsonjs_kinds.regexp_cls)
        return 0;
    d = JS_NewDate(ctx, 0);
    if (JS_IsException(d))
        return -1;
    bsonjs_kinds.date_cls = JS_GetClassID(d);
    JS_FreeValue(ctx, d);
    g = JS_GetGlobalObject(ctx);
    re_ctor = JS_GetPropertyStr(ctx, g, "RegExp");
    JS_FreeValue(ctx, g);
    if (JS_IsException(re_ctor))
        return -1;
    arg = JS_NewString(ctx, "a");
    re = JS_CallConstructor(ctx, re_ctor, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, re_ctor);
    if (JS_IsException(re))
        return -1;
    bsonjs_kinds.regexp_cls = JS_GetClassID(re);
    JS_FreeValue(ctx, re);
    return 0;
}

int dyn_bsonjs_ensure(JSContext* ctx, const DynBsonJsKinds** kinds)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto;
    int i;
    pthread_once(&bsonjs_once, bsonjs_alloc_ids);
    proto = JS_IsRegisteredClass(rt, bsonjs_kinds.cls[DYN_BSONJS_NCLASSES - 1]) ? JS_GetClassProto(ctx, bsonjs_kinds.cls[DYN_BSONJS_NCLASSES - 1]) : JS_UNDEFINED;
    if (JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        if (kinds)
            *kinds = &bsonjs_kinds;
        return 0;
    }
    JS_FreeValue(ctx, proto);
    if (bsonjs_builtin_ids(ctx) < 0)
        return -1;
    for (i = 0; i < DYN_BSONJS_NCLASSES; i++) {
        JSValue ctor;
        if (!JS_IsRegisteredClass(rt, bsonjs_kinds.cls[i]) && JS_NewClass(rt, bsonjs_kinds.cls[i], &bsonjs_defs[i]) < 0)
            return -1;
        proto = JS_GetClassProto(ctx, bsonjs_kinds.cls[i]);
        if (JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            continue;
        }
        JS_FreeValue(ctx, proto);
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, bsonjs_specs[i].proto, bsonjs_specs[i].nproto);
        ctor = JS_NewCFunction2(ctx, bsonjs_specs[i].ctor, bsonjs_names[i], 1, JS_CFUNC_constructor, 0);
        if (JS_IsException(ctor)) {
            JS_FreeValue(ctx, proto);
            return -1;
        }
        if (bsonjs_specs[i].stat)
            JS_SetPropertyFunctionList(ctx, ctor, bsonjs_specs[i].stat, bsonjs_specs[i].nstat);
        JS_SetConstructor(ctx, ctor, proto);
        JS_SetClassProto(ctx, bsonjs_kinds.cls[i], proto);
        JS_FreeValue(ctx, ctor);
    }
    if (kinds)
        *kinds = &bsonjs_kinds;
    return 0;
}

JSValue dyn_bsonjs_ctor(JSContext* ctx, int which)
{
    JSValue proto, ctor;
    if (dyn_bsonjs_ensure(ctx, NULL) < 0)
        return JS_EXCEPTION;
    proto = JS_GetClassProto(ctx, bsonjs_kinds.cls[which]);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    return ctor;
}

JSValue dyn_bsonjs_make(JSContext* ctx, int which, const uint8_t* p, size_t n, uint8_t sub, const char* s2)
{
    DynBsonObj* o = bsonjs_new_payload(ctx);
    if (!o)
        return JS_EXCEPTION;
    switch (which) {
    case DYN_BSONJS_OBJECTID:
        memcpy(o->b, p, 12);
        break;
    case DYN_BSONJS_TIMESTAMP:
        memcpy(o->b, p, 8);
        break;
    case DYN_BSONJS_DECIMAL128:
        memcpy(o->b, p, 16);
        break;
    case DYN_BSONJS_BINARY:
    case DYN_BSONJS_CODE:
        o->data = malloc(n + 1);
        if (!o->data) {
            free(o);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (n)
            memcpy(o->data, p, n);
        o->data[n] = 0;
        o->len = n;
        o->sub = sub;
        break;
    case DYN_BSONJS_REGEXP:
        o->s1 = strdup((const char*)p);
        o->s2 = strdup(s2);
        if (!o->s1 || !o->s2) {
            free(o->s1);
            free(o->s2);
            free(o);
            return JS_ThrowOutOfMemory(ctx);
        }
        break;
    default:
        break;
    }
    return bsonjs_wrap(ctx, JS_UNDEFINED, which, o);
}

int dyn_bsonjs_payload(JSValueConst v, int which, const uint8_t** p, size_t* n, uint8_t* sub, const char** s2)
{
    DynBsonObj* o = JS_GetOpaque(v, bsonjs_kinds.cls[which]);
    if (!o)
        return 0;
    switch (which) {
    case DYN_BSONJS_BINARY:
    case DYN_BSONJS_CODE:
        *p = o->data;
        *n = o->len;
        *sub = o->sub;
        break;
    case DYN_BSONJS_REGEXP:
        *p = (const uint8_t*)o->s1;
        *n = strlen(o->s1);
        *s2 = o->s2;
        break;
    default:
        *p = o->b;
        *n = which == DYN_BSONJS_OBJECTID ? 12 : which == DYN_BSONJS_TIMESTAMP ? 8
                                                                               : 16;
        break;
    }
    return 1;
}

int dyn_bsonjs_add_exports(JSContext* ctx, JSModuleDef* m)
{
    int i;
    for (i = 0; i < DYN_BSONJS_NCLASSES; i++)
        if (JS_AddModuleExport(ctx, m, bsonjs_names[i]) < 0)
            return -1;
    return 0;
}

int dyn_bsonjs_register(JSContext* ctx, JSModuleDef* m)
{
    int i;
    if (dyn_bsonjs_ensure(ctx, NULL) < 0)
        return -1;
    for (i = 0; i < DYN_BSONJS_NCLASSES; i++) {
        JSValue ctor = dyn_bsonjs_ctor(ctx, i);
        if (JS_IsException(ctor) || JS_SetModuleExport(ctx, m, bsonjs_names[i], ctor) < 0)
            return -1;
    }
    return 0;
}

#endif
