#include "asn1-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASN1)

int ab_write(ab_t* b, const uint8_t* p, size_t n)
{
    if (b->oom || !n)
        return 0;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return -1;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

int asn1_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return 0;
        }
        *hold = JS_UNDEFINED;
        *pp = base;
        *pn = ab;
        return 1;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        return 0;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "ASN.1: invalid byte view");
        return -1;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 1;
}

JSValue asn1_new_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, r;

    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return r;
}

int utf8_ok(const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i++];
        if (b < 0x80)
            continue;
        if (b >= 0xC2 && b <= 0xDF) {
            if (i >= n || (p[i] & 0xC0) != 0x80)
                return 0;
            i++;
        } else if (b == 0xE0) {
            if (i + 1 >= n || p[i] < 0xA0 || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
        } else if ((b >= 0xE1 && b <= 0xEC) || (b >= 0xEE && b <= 0xEF)) {
            if (i + 1 >= n || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
        } else if (b == 0xED) {
            if (i + 1 >= n || p[i] >= 0xA0 || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
        } else if (b == 0xF0) {
            if (i + 2 >= n || p[i] < 0x90 || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else if (b >= 0xF1 && b <= 0xF3) {
            if (i + 2 >= n || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else if (b == 0xF4) {
            if (i + 2 >= n || p[i] > 0x8F || (p[i] & 0xC0) != 0x80 || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return 0;
            i += 3;
        } else {
            return 0;
        }
    }
    return 1;
}

static int time_field(const uint8_t* p, int a, int b)
{
    int v = 0, i;
    for (i = a; i <= b; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

int time_ok(const uint8_t* p, size_t n, int tag)
{
    int mo, dd, hh, mi, ss;
    if (tag == 23) {
        if (n != 13 || p[12] != 'Z' || time_field(p, 0, 1) < 0)
            return 0;
        mo = 2;
        dd = 4;
        hh = 6;
        mi = 8;
        ss = 10;
    } else {
        if (n != 15 || p[14] != 'Z' || time_field(p, 0, 3) < 0)
            return 0;
        mo = 4;
        dd = 6;
        hh = 8;
        mi = 10;
        ss = 12;
    }
    if (time_field(p, mo, mo + 1) < 1 || time_field(p, mo, mo + 1) > 12 || time_field(p, dd, dd + 1) < 1 || time_field(p, dd, dd + 1) > 31 || time_field(p, hh, hh + 1) > 23 || time_field(p, mi, mi + 1) > 59 || time_field(p, ss, ss + 1) > 60)
        return 0;
    return 1;
}

int utf8_put(uint8_t* q, uint32_t cp)
{
    if (cp < 0x80) {
        q[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        q[0] = (uint8_t)(0xC0 | (cp >> 6));
        q[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        q[0] = (uint8_t)(0xE0 | (cp >> 12));
        q[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        q[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    q[0] = (uint8_t)(0xF0 | (cp >> 18));
    q[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    q[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    q[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

int node_int(JSContext* ctx, JSValueConst v, const char* name,
    int64_t lo, int64_t hi, int64_t* out)
{
    int64_t x;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "ASN.1: node '%s' must be a number", name);
        return -1;
    }
    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v))) {
        double dv;
        JS_ToFloat64(ctx, &dv, v);
        if (dv != floor(dv)) {
            JS_ThrowTypeError(ctx, "ASN.1: node '%s' must be an integer", name);
            return -1;
        }
    }
    if (JS_ToInt64(ctx, &x, v) < 0)
        return -1;
    if (x < lo || x > hi) {
        JS_ThrowRangeError(ctx, "ASN.1: node '%s' out of range", name);
        return -1;
    }
    *out = x;
    return 0;
}

JSValue asn1_node(JSContext* ctx, int cls, int tag, int constructed,
    JSValue value)
{
    JSValue o = JS_NewObject(ctx);
    JSValue vc, vt, vk;

    if (JS_IsException(o)) {
        JS_FreeValue(ctx, value);
        return o;
    }
    vc = JS_NewInt32(ctx, cls);
    vt = JS_NewInt32(ctx, tag);
    vk = JS_NewBool(ctx, constructed);
    if (JS_DefinePropertyValueStr(ctx, o, "cls", vc, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, o, "tag", vt, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, o, "constructed", vk, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "value", value,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue asn1_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ab_t b;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.encode(node): a node is required");
    ab_init(&b);
    if (asn1_enc_node(ctx, argv[0], &b, 0) < 0 || b.oom) {
        if (b.oom)
            JS_ThrowOutOfMemory(ctx);
        ab_free(&b);
        return JS_EXCEPTION;
    }
    out = asn1_new_bytes(ctx, b.p, b.n);
    ab_free(&b);
    return out;
}

static int asn1_arg_tag(JSContext* ctx, JSValueConst v, uint32_t* out)
{
    int64_t t;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "ASN.1: tag number must be a number");
        return -1;
    }
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0 || t > ASN1_MAX_TAG) {
        JS_ThrowRangeError(ctx, "ASN.1: tag number out of 0..2^31-1");
        return -1;
    }
    *out = (uint32_t)t;
    return 0;
}

static JSValue asn1_seq(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int isa;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.seq(children): children are "
                                      "required");
    isa = JS_IsArray(ctx, argv[0]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.seq: children must be an array");
    return asn1_node(ctx, 0, 16, 1, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int isa;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.set(children): children are "
                                      "required");
    isa = JS_IsArray(ctx, argv[0]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.set: children must be an array");
    return asn1_node(ctx, 0, 17, 1, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_int(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.int(value): a value is required");
    return asn1_node(ctx, 0, 2, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_bool(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.bool(value): a value is required");
    return asn1_node(ctx, 0, 1, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_null(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return asn1_node(ctx, 0, 5, 0, JS_NULL);
}

static JSValue asn1_octets(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.octets(bytes): bytes are required");
    return asn1_node(ctx, 0, 4, 0, JS_DupValue(ctx, argv[0]));
}

static JSValue asn1_bit_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue obj, vu, vb;
    int unused = 0;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.bitString(bytes, unused): two "
                                      "arguments are required");
    if (!JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "ASN1.bitString: unused must be a "
                                      "number");
    if (JS_ToInt32(ctx, &unused, argv[1]) < 0)
        return JS_EXCEPTION;
    if (unused < 0 || unused > 7)
        return JS_ThrowRangeError(ctx, "ASN1.bitString: unused-bits count "
                                       "must be 0..7");
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    vu = JS_NewInt32(ctx, unused);
    vb = JS_DupValue(ctx, argv[0]);
    if (JS_DefinePropertyValueStr(ctx, obj, "unused", vu,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, vb);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "bytes", vb,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return asn1_node(ctx, 0, 3, 0, obj);
}

static JSValue asn1_str_node(JSContext* ctx, int tag, JSValueConst v)
{
    return asn1_node(ctx, 0, tag, 0, JS_DupValue(ctx, v));
}

static JSValue asn1_oid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.oid(oid): an OID string is "
                                      "required");
    return asn1_str_node(ctx, 6, argv[0]);
}

static JSValue asn1_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.utf8(str): a string is required");
    return asn1_str_node(ctx, 12, argv[0]);
}

static JSValue asn1_ia5(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.ia5String(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 22, argv[0]);
}

static JSValue asn1_bmp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.bmpString(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 30, argv[0]);
}

static JSValue asn1_universal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.universalString(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 28, argv[0]);
}

static JSValue asn1_printable(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.printable(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 19, argv[0]);
}

static JSValue asn1_utc_time(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.utcTime(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 23, argv[0]);
}

static JSValue asn1_gen_time(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ASN1.generalizedTime(str): a string is "
                                      "required");
    return asn1_str_node(ctx, 24, argv[0]);
}

static JSValue asn1_context(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t tag;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.context(tag, content): two "
                                      "arguments are required");
    if (asn1_arg_tag(ctx, argv[0], &tag) < 0)
        return JS_EXCEPTION;
    return asn1_node(ctx, 2, (int)tag, 0, JS_DupValue(ctx, argv[1]));
}

static JSValue asn1_context_c(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t tag;
    int isa;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ASN1.contextC(tag, children): two "
                                      "arguments are required");
    if (asn1_arg_tag(ctx, argv[0], &tag) < 0)
        return JS_EXCEPTION;
    isa = JS_IsArray(ctx, argv[1]);
    if (isa < 0)
        return JS_EXCEPTION;
    if (isa != 1)
        return JS_ThrowTypeError(ctx, "ASN1.contextC: children must be an "
                                      "array");
    return asn1_node(ctx, 2, (int)tag, 1, JS_DupValue(ctx, argv[1]));
}

static const JSCFunctionListEntry asn1_methods[] = {
    JS_CFUNC_DEF("decode", 1, asn1_decode),
    JS_CFUNC_DEF("encode", 1, asn1_encode),
    JS_CFUNC_DEF("seq", 1, asn1_seq),
    JS_CFUNC_DEF("set", 1, asn1_set),
    JS_CFUNC_DEF("int", 1, asn1_int),
    JS_CFUNC_DEF("bool", 1, asn1_bool),
    JS_CFUNC_DEF("null", 0, asn1_null),
    JS_CFUNC_DEF("octets", 1, asn1_octets),
    JS_CFUNC_DEF("bitString", 2, asn1_bit_string),
    JS_CFUNC_DEF("oid", 1, asn1_oid),
    JS_CFUNC_DEF("utf8", 1, asn1_utf8),
    JS_CFUNC_DEF("ia5String", 1, asn1_ia5),
    JS_CFUNC_DEF("bmpString", 1, asn1_bmp),
    JS_CFUNC_DEF("universalString", 1, asn1_universal),
    JS_CFUNC_DEF("printable", 1, asn1_printable),
    JS_CFUNC_DEF("utcTime", 1, asn1_utc_time),
    JS_CFUNC_DEF("generalizedTime", 1, asn1_gen_time),
    JS_CFUNC_DEF("context", 2, asn1_context),
    JS_CFUNC_DEF("contextC", 2, asn1_context_c),
};

static const JSCFunctionListEntry asn1_exports[] = {
    JS_OBJECT_DEF("ASN1", asn1_methods, countof(asn1_methods),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
};

static int dyn_asn1_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, asn1_exports, countof(asn1_exports));
}

int js_nat_init_asn1(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:asn1", dyn_asn1_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, asn1_exports, countof(asn1_exports));
}

#endif
