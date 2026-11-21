#include "protobuf-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROTOBUF)

#define PB_MAX_BYTES 0x7fffffffu
#define PB_MAX_UNKNOWN 65536u

typedef struct {
    JSContext* ctx;
    const uint8_t* p;
    size_t n, i;
    int depth;
} pb_rd_t;

static JSValue pb_decode_value(pb_rd_t* r, int type, const pb_schema_t* sub);

static JSValue pb_decode_message(pb_rd_t* r, const pb_schema_t* s);

static int pb_rd_uvarint(pb_rd_t* r, uint64_t* out)
{
    int nb = dyn_codec_uvarint(r->p + r->i, r->n - r->i, out);
    if (nb > 0) {
        r->i += (size_t)nb;
        return 0;
    }
    if (nb == 0)
        JS_ThrowSyntaxError(r->ctx, "decode: truncated varint at byte %u",
            (unsigned)r->i);
    else
        JS_ThrowRangeError(r->ctx, "decode: varint exceeds 64 bits at byte %u",
            (unsigned)r->i);
    return -1;
}

static int pb_rd_need(pb_rd_t* r, size_t k)
{
    if (k > r->n - r->i) {
        JS_ThrowSyntaxError(r->ctx, "decode: truncated at byte %u (need %u)",
            (unsigned)r->i, (unsigned)k);
        return -1;
    }
    return 0;
}

static int pb_rd_len(pb_rd_t* r, size_t* out)
{
    uint64_t v;
    if (pb_rd_uvarint(r, &v) < 0)
        return -1;
    if (v > PB_MAX_BYTES) {
        JS_ThrowRangeError(r->ctx, "decode: length %llu exceeds the 2 GiB "
                                   "wire limit at byte %u",
            (unsigned long long)v, (unsigned)r->i);
        return -1;
    }
    if (v > r->n - r->i) {
        JS_ThrowRangeError(r->ctx, "decode: length %llu exceeds the remaining "
                                   "%u bytes at byte %u",
            (unsigned long long)v, (unsigned)(r->n - r->i),
            (unsigned)r->i);
        return -1;
    }
    *out = (size_t)v;
    return 0;
}

static uint32_t pb_rd_le32(pb_rd_t* r)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v |= (uint32_t)r->p[r->i + (size_t)i] << (8 * i);
    r->i += 4;
    return v;
}

static uint64_t pb_rd_le64(pb_rd_t* r)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)r->p[r->i + (size_t)i] << (8 * i);
    r->i += 8;
    return v;
}

static JSValue pb_new_int64(JSContext* ctx, int64_t v)
{
    double dv = (double)v;
    if ((int64_t)dv != v)
        return JS_ThrowRangeError(ctx, "decode: int64 value %lld is not "
                                       "exactly representable as a JS number",
            (long long)v);
    return JS_NewFloat64(ctx, dv);
}

static JSValue pb_new_uint64(JSContext* ctx, uint64_t v)
{
    double dv = (double)v;
    if (dv >= 18446744073709551616.0 || (uint64_t)dv != v)
        return JS_ThrowRangeError(ctx, "decode: uint64 value %llu is not "
                                       "exactly representable as a JS number",
            (unsigned long long)v);
    return JS_NewFloat64(ctx, dv);
}

static JSValue pb_decode_value(pb_rd_t* r, int type, const pb_schema_t* sub)
{
    uint64_t u;

    switch (type) {
    case PB_T_INT32:
    case PB_T_ENUM:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        return JS_NewInt32(r->ctx, (int32_t)u);
    case PB_T_INT64:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        return pb_new_int64(r->ctx, (int64_t)u);
    case PB_T_UINT32:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(r->ctx, (int64_t)(uint32_t)u);
    case PB_T_UINT64:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        return pb_new_uint64(r->ctx, u);
    case PB_T_SINT32:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        {
            uint32_t t = (uint32_t)u;
            int32_t v = (int32_t)(t >> 1) ^ -(int32_t)(t & 1);
            return JS_NewInt32(r->ctx, v);
        }
    case PB_T_SINT64:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        {
            int64_t v = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
            return pb_new_int64(r->ctx, v);
        }
    case PB_T_FIXED32:
        if (pb_rd_need(r, 4) < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(r->ctx, (int64_t)pb_rd_le32(r));
    case PB_T_SFIXED32:
        if (pb_rd_need(r, 4) < 0)
            return JS_EXCEPTION;
        return JS_NewInt32(r->ctx, (int32_t)pb_rd_le32(r));
    case PB_T_FIXED64:
        if (pb_rd_need(r, 8) < 0)
            return JS_EXCEPTION;
        return pb_new_uint64(r->ctx, pb_rd_le64(r));
    case PB_T_SFIXED64:
        if (pb_rd_need(r, 8) < 0)
            return JS_EXCEPTION;
        return pb_new_int64(r->ctx, (int64_t)pb_rd_le64(r));
    case PB_T_FLOAT: {
        float f;
        uint32_t bits;
        if (pb_rd_need(r, 4) < 0)
            return JS_EXCEPTION;
        bits = pb_rd_le32(r);
        memcpy(&f, &bits, 4);
        return JS_NewFloat64(r->ctx, (double)f);
    }
    case PB_T_DOUBLE: {
        double d;
        uint64_t bits;
        if (pb_rd_need(r, 8) < 0)
            return JS_EXCEPTION;
        bits = pb_rd_le64(r);
        memcpy(&d, &bits, 8);
        return JS_NewFloat64(r->ctx, d);
    }
    case PB_T_BOOL:
        if (pb_rd_uvarint(r, &u) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(r->ctx, u != 0);
    case PB_T_STRING: {
        size_t len;
        JSValue out;
        if (pb_rd_len(r, &len) < 0)
            return JS_EXCEPTION;
        if (simd.validate_utf8(r->p + r->i, len) != len)
            return JS_ThrowSyntaxError(r->ctx,
                "decode: a string field at byte %u is not valid UTF-8",
                (unsigned)r->i);
        out = JS_NewStringLen(r->ctx, (const char*)r->p + r->i, len);
        r->i += len;
        return out;
    }
    case PB_T_BYTES: {
        size_t len;
        JSValueConst ta[3];
        JSValue ab, out;
        static const uint8_t zero = 0;
        if (pb_rd_len(r, &len) < 0)
            return JS_EXCEPTION;
        ab = JS_NewArrayBufferCopy(r->ctx, len ? r->p + r->i : &zero, len);
        if (JS_IsException(ab))
            return ab;
        r->i += len;
        ta[0] = ab;
        ta[1] = JS_UNDEFINED;
        ta[2] = JS_UNDEFINED;
        out = JS_NewTypedArray(r->ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(r->ctx, ab);
        return out;
    }
    case PB_T_MESSAGE: {
        size_t len;
        pb_rd_t s;
        JSValue inner;
        if (pb_rd_len(r, &len) < 0)
            return JS_EXCEPTION;
        s = *r;
        s.n = r->i + len;
        s.depth = r->depth + 1;
        inner = pb_decode_message(&s, sub);
        if (JS_IsException(inner))
            return inner;
        if (s.i != s.n) {
            JS_FreeValue(r->ctx, inner);
            return JS_ThrowSyntaxError(r->ctx, "decode: trailing bytes in a "
                                               "nested message");
        }
        r->i += len;
        return inner;
    }
    default:
        return JS_ThrowSyntaxError(r->ctx, "decode: unsupported type %d", type);
    }
}

static int pb_repeated_push(JSContext* ctx, JSValue out, JSAtom name,
    JSValue val)
{
    JSValue arr = JS_GetProperty(ctx, out, name);
    JSValue lv;
    int64_t len = 0;
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    if (JS_IsArray(ctx, arr) != 1) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) {
            JS_FreeValue(ctx, val);
            return -1;
        }
        if (JS_DefinePropertyValue(ctx, out, name, JS_DupValue(ctx, arr),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, val);
            return -1;
        }
    }
    lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, val);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)len, val,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    JS_FreeValue(ctx, arr);
    return 0;
}

static int pb_unknown_save(pb_rd_t* r, JSValue* unknown, size_t rec,
    unsigned wire)
{
    size_t end;
    JSValueConst ta[3];
    JSValue ab, u8, lv;
    int64_t n;

    switch (wire) {
    case 0: {
        uint64_t v;
        if (pb_rd_uvarint(r, &v) < 0)
            return -1;
        break;
    }
    case 1:
        if (pb_rd_need(r, 8) < 0)
            return -1;
        r->i += 8;
        break;
    case 5:
        if (pb_rd_need(r, 4) < 0)
            return -1;
        r->i += 4;
        break;
    default: {
        size_t len;
        if (pb_rd_len(r, &len) < 0)
            return -1;
        r->i += len;
        break;
    }
    }
    end = r->i;
    if (JS_IsUndefined(*unknown)) {
        *unknown = JS_NewArray(r->ctx);
        if (JS_IsException(*unknown))
            return -1;
    }
    ab = JS_NewArrayBufferCopy(r->ctx, r->p + rec, end - rec);
    if (JS_IsException(ab))
        return -1;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(r->ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(r->ctx, ab);
    if (JS_IsException(u8))
        return -1;
    lv = JS_GetPropertyStr(r->ctx, *unknown, "length");
    if (JS_IsException(lv) || JS_ToInt64(r->ctx, &n, lv) < 0) {
        JS_FreeValue(r->ctx, lv);
        JS_FreeValue(r->ctx, u8);
        return -1;
    }
    JS_FreeValue(r->ctx, lv);
    if (JS_DefinePropertyValueUint32(r->ctx, *unknown, (uint32_t)n, u8,
            JS_PROP_C_W_E)
        < 0)
        return -1;
    return 0;
}

static int pb_decode_one(pb_rd_t* r, JSValue out, const pb_field_t* f,
    int repeated)
{
    JSValue val = pb_decode_value(r, f->type, f->sub);
    if (JS_IsException(val))
        return -1;
    if (repeated)
        return pb_repeated_push(r->ctx, out, f->name, val);
    if (JS_DefinePropertyValue(r->ctx, out, f->name, val, JS_PROP_C_W_E) < 0)
        return -1;
    return 0;
}

static int pb_decode_packed(pb_rd_t* r, JSValue out, const pb_field_t* f)
{
    size_t len, start;
    pb_rd_t sub;

    if (pb_rd_len(r, &len) < 0)
        return -1;
    start = r->i;
    r->i += len;
    sub = *r;
    sub.i = start;
    sub.n = start + len;
    while (sub.i < sub.n) {
        JSValue el = pb_decode_value(&sub, f->type, f->sub);
        if (JS_IsException(el))
            return -1;
        if (pb_repeated_push(r->ctx, out, f->name, el) < 0)
            return -1;
    }
    return 0;
}

static int pb_decode_map(pb_rd_t* r, JSValue out, const pb_field_t* f)
{
    size_t len, start;
    pb_rd_t sub;
    JSValue map;
    int rc = -1;

    if (pb_rd_len(r, &len) < 0)
        return -1;
    start = r->i;
    r->i += len;
    sub = *r;
    sub.i = start;
    sub.n = start + len;
    map = JS_GetProperty(r->ctx, out, f->name);
    if (JS_IsException(map))
        return -1;
    if (!JS_IsObject(map) || JS_IsArray(r->ctx, map) == 1) {
        JS_FreeValue(r->ctx, map);
        map = JS_NewObject(r->ctx);
        if (JS_IsException(map))
            return -1;
        if (JS_DefinePropertyValue(r->ctx, out, f->name,
                JS_DupValue(r->ctx, map), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(r->ctx, map);
            return -1;
        }
    }
    JSValue key = JS_UNDEFINED, val = JS_UNDEFINED;
    int have_key, have_val;
    while (sub.i < sub.n) {
        key = val = JS_UNDEFINED;
        have_key = have_val = 0;
        while (sub.i < sub.n) {
            uint64_t tag;
            uint32_t num;
            unsigned w;
            if (pb_rd_uvarint(&sub, &tag) < 0)
                goto out;
            if (tag > (((uint64_t)PB_MAX_FIELD << 3) | 7)) {
                JS_ThrowSyntaxError(r->ctx, "decode: field number %llu exceeds the "
                                            "2^29-1 limit",
                    (unsigned long long)(tag >> 3));
                goto out;
            }
            num = (uint32_t)(tag >> 3);
            w = (unsigned)(tag & 7);
            if (num == 0) {
                JS_ThrowSyntaxError(r->ctx, "decode: field number 0 in a map "
                                            "entry");
                goto out;
            }
            if (num == 1) {
                if (w != (unsigned)pb_wire_of_type(f->ktype) || have_key) {
                    JS_ThrowSyntaxError(r->ctx, "decode: malformed map key");
                    goto out;
                }
                key = pb_decode_value(&sub, f->ktype, NULL);
                if (JS_IsException(key))
                    goto out;
                have_key = 1;
            } else if (num == 2) {
                if (w != (unsigned)pb_wire_of_type(f->vtype) || have_val) {
                    JS_ThrowSyntaxError(r->ctx, "decode: malformed map value");
                    goto out;
                }
                val = pb_decode_value(&sub, f->vtype, f->sub);
                if (JS_IsException(val))
                    goto out;
                have_val = 1;
            } else {
                JS_ThrowSyntaxError(r->ctx, "decode: unexpected field %u in a "
                                            "map entry",
                    num);
                goto out;
            }
        }
        if (!have_key || !have_val) {
            JS_ThrowSyntaxError(r->ctx, "decode: a map entry needs both a key "
                                        "and a value");
            goto out;
        }
        {
            JSAtom a = JS_ValueToAtom(r->ctx, key);
            JS_FreeValue(r->ctx, key);
            key = JS_UNDEFINED;
            if (a == JS_ATOM_NULL)
                goto out;
            if (JS_DefinePropertyValue(r->ctx, map, a, val, JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(r->ctx, a);
                goto out;
            }
            JS_FreeAtom(r->ctx, a);
            val = JS_UNDEFINED;
        }
    }
    rc = 0;
out:
    JS_FreeValue(r->ctx, key);
    JS_FreeValue(r->ctx, val);
    JS_FreeValue(r->ctx, map);
    return rc;
}

static int pb_decode_field(pb_rd_t* r, JSValue out, const pb_field_t* f,
    int wire)
{
    if (f->flags & PB_F_MAP)
        return pb_decode_map(r, out, f);
    if (f->flags & PB_F_REPEATED) {
        if (wire == 2 && pb_is_packable(f->type))
            return pb_decode_packed(r, out, f);
        return pb_decode_one(r, out, f, 1);
    }
    return pb_decode_one(r, out, f, 0);
}

static int pb_finish_message(JSContext* ctx, JSValue out, const pb_schema_t* s,
    JSValue* unknown)
{
    size_t k;
    for (k = 0; k < s->n; k++) {
        const pb_field_t* f = &s->f[k];
        JSValue p;
        if (!(f->flags & (PB_F_REPEATED | PB_F_MAP)))
            continue;
        p = JS_GetProperty(ctx, out, f->name);
        if (JS_IsException(p))
            return -1;
        if (!JS_IsUndefined(p)) {
            JS_FreeValue(ctx, p);
            continue;
        }
        JS_FreeValue(ctx, p);
        p = (f->flags & PB_F_MAP) ? JS_NewObject(ctx) : JS_NewArray(ctx);
        if (JS_IsException(p))
            return -1;
        if (JS_DefinePropertyValue(ctx, out, f->name, p, JS_PROP_C_W_E) < 0)
            return -1;
    }
    if (!JS_IsUndefined(*unknown)) {
        if (JS_DefinePropertyValueStr(ctx, out, PB_UNKNOWN_KEY, *unknown,
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)
            < 0)
            return -1;
        *unknown = JS_UNDEFINED;
    }
    return 0;
}

static JSValue pb_decode_message(pb_rd_t* r, const pb_schema_t* s)
{
    JSValue out, unknown = JS_UNDEFINED;
    size_t k, nunk = 0;
    int rc = 0;

    if (r->depth >= PB_MAX_DEPTH)
        return JS_ThrowRangeError(r->ctx, "decode: message nesting exceeds %d",
            PB_MAX_DEPTH);
    out = JS_NewObject(r->ctx);
    if (JS_IsException(out))
        return out;
    while (r->i < r->n) {
        uint64_t tag;
        uint32_t number;
        unsigned wire;
        size_t rec = r->i;
        const pb_field_t* f = NULL;

        if (pb_rd_uvarint(r, &tag) < 0) {
            rc = -1;
            break;
        }
        if (tag > (((uint64_t)PB_MAX_FIELD << 3) | 7)) {
            JS_ThrowSyntaxError(r->ctx, "decode: field number %llu exceeds the "
                                        "2^29-1 limit",
                (unsigned long long)(tag >> 3));
            rc = -1;
            break;
        }
        number = (uint32_t)(tag >> 3);
        wire = (unsigned)(tag & 7);
        if (number == 0) {
            JS_ThrowSyntaxError(r->ctx, "decode: field number 0 is invalid");
            rc = -1;
            break;
        }
        if (wire == 3 || wire == 4) {
            JS_ThrowSyntaxError(r->ctx, "decode: group wire types (3/4) are "
                                        "not supported");
            rc = -1;
            break;
        }
        if (wire > 5) {
            JS_ThrowSyntaxError(r->ctx, "decode: unknown wire type %u", wire);
            rc = -1;
            break;
        }
        for (k = 0; k < s->n; k++)
            if (s->f[k].number == number) {
                f = &s->f[k];
                break;
            }
        if (f && pb_field_wire_ok(f, (int)wire))
            rc = pb_decode_field(r, out, f, (int)wire);
        else {
            if (nunk >= PB_MAX_UNKNOWN) {
                JS_ThrowRangeError(r->ctx, "decode: too many unknown fields "
                                           "(cap %u)",
                    PB_MAX_UNKNOWN);
                rc = -1;
                break;
            }
            nunk++;
            rc = pb_unknown_save(r, &unknown, rec, wire);
        }
        if (rc < 0)
            break;
    }
    if (rc == 0)
        rc = pb_finish_message(r->ctx, out, s, &unknown);
    if (rc < 0) {
        JS_FreeValue(r->ctx, unknown);
        JS_FreeValue(r->ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

static int pb_rd_arg_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *hold = JS_UNDEFINED;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "Proto.decode(bytes): expected a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base) {
        JS_FreeValue(ctx, buf);
        return -1;
    }
    if (off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 0;
}

JSValue pb_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pb_schema_t s;
    pb_rd_t r;
    JSValue hold = JS_UNDEFINED, out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Proto.decode(bytes, schema): a schema "
                                      "is required");
    memset(&r, 0, sizeof r);
    r.ctx = ctx;
    if (pb_rd_arg_bytes(ctx, argv[0], &r.p, &r.n, &hold) < 0)
        return JS_EXCEPTION;
    if (pb_schema_parse(ctx, argv[1], &s, 0) < 0) {
        JS_FreeValue(ctx, hold);
        return JS_EXCEPTION;
    }
    out = pb_decode_message(&r, &s);
    JS_FreeValue(ctx, hold);
    pb_schema_free(ctx, &s);
    return out;
}

#endif
