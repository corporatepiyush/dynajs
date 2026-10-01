#include "dyna-nat.h"
#include "core/dyn-codec.h"
#include "core/dyn-sb.h"
#include "dyna-simd-kernels.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VSERIALIZE)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PB_MAX_DEPTH 64
#define PB_MAX_FIELD 536870911u
#define PB_MAX_BYTES 0x7fffffffu
#define PB_MAX_FIELDS 65536
#define PB_MAX_UNKNOWN 65536u
#define PB_UNKNOWN_KEY "__protoUnknown"

enum {
    PB_T_INT32 = 0,
    PB_T_INT64,
    PB_T_UINT32,
    PB_T_UINT64,
    PB_T_SINT32,
    PB_T_SINT64,
    PB_T_FIXED32,
    PB_T_SFIXED32,
    PB_T_FIXED64,
    PB_T_SFIXED64,
    PB_T_FLOAT,
    PB_T_DOUBLE,
    PB_T_BOOL,
    PB_T_STRING,
    PB_T_BYTES,
    PB_T_MESSAGE,
    PB_T_ENUM,
    PB_T_COUNT
};

#define PB_F_REPEATED 1
#define PB_F_PACKED 2
#define PB_F_MAP 4

static const char* const pb_type_names[PB_T_COUNT] = {
    "int32",
    "int64",
    "uint32",
    "uint64",
    "sint32",
    "sint64",
    "fixed32",
    "sfixed32",
    "fixed64",
    "sfixed64",
    "float",
    "double",
    "bool",
    "string",
    "bytes",
    "message",
    "enum",
};

typedef struct pb_schema pb_schema_t;

typedef struct {
    JSAtom name;
    uint32_t number;
    uint8_t type;
    uint8_t flags;
    uint8_t ktype, vtype;
    pb_schema_t* sub;
} pb_field_t;

struct pb_schema {
    pb_field_t* f;
    size_t n;
};

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} pb_w_t;

static void pb_w_reserve(pb_w_t* w, size_t extra)
{
    if (w->oom || !extra)
        return;
    if (w->n + extra <= w->cap)
        return;
    if (!dyn_sb_reserve((void**)&w->p, &w->cap, w->n + extra, 128))
        w->oom = 1;
}

static void pb_w_raw(pb_w_t* w, const void* p, size_t n)
{
    if (w->oom || !n)
        return;
    pb_w_reserve(w, n);
    if (w->oom)
        return;
    memcpy(w->p + w->n, p, n);
    w->n += n;
}

static void pb_w_varint(pb_w_t* w, uint64_t v)
{
    uint8_t tmp[DYN_CODEC_VARINT_MAX];
    pb_w_raw(w, tmp, dyn_codec_put_uvarint(v, tmp));
}

static void pb_w_le32(pb_w_t* w, uint32_t v)
{
    uint8_t t[4];
    t[0] = (uint8_t)v;
    t[1] = (uint8_t)(v >> 8);
    t[2] = (uint8_t)(v >> 16);
    t[3] = (uint8_t)(v >> 24);
    pb_w_raw(w, t, 4);
}

static void pb_w_le64(pb_w_t* w, uint64_t v)
{
    uint8_t t[8];
    int i;
    for (i = 0; i < 8; i++)
        t[i] = (uint8_t)(v >> (8 * i));
    pb_w_raw(w, t, 8);
}

static int pb_wire_of_type(int t)
{
    switch (t) {
    case PB_T_INT32:
    case PB_T_INT64:
    case PB_T_UINT32:
    case PB_T_UINT64:
    case PB_T_SINT32:
    case PB_T_SINT64:
    case PB_T_BOOL:
    case PB_T_ENUM:
        return 0;
    case PB_T_FIXED64:
    case PB_T_SFIXED64:
    case PB_T_DOUBLE:
        return 1;
    case PB_T_STRING:
    case PB_T_BYTES:
    case PB_T_MESSAGE:
        return 2;
    case PB_T_FIXED32:
    case PB_T_SFIXED32:
    case PB_T_FLOAT:
        return 5;
    default:
        return -1;
    }
}

static int pb_is_packable(int t)
{
    int w = pb_wire_of_type(t);
    return w == 0 || w == 1 || w == 5;
}

static int pb_type_of(JSContext* ctx, JSValueConst v, int* out,
    const char* what)
{
    size_t n;
    const char* s;
    int i;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "schema: %s must be a type string", what);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    for (i = 0; i < PB_T_COUNT; i++)
        if (strlen(pb_type_names[i]) == n && memcmp(pb_type_names[i], s, n) == 0)
            break;
    JS_FreeCString(ctx, s);
    if (i == PB_T_COUNT) {
        JS_ThrowTypeError(ctx, "schema: unknown type string for %s", what);
        return -1;
    }
    *out = i;
    return 0;
}

static int pb_key_type_ok(int t)
{
    switch (t) {
    case PB_T_STRING:
    case PB_T_BOOL:
    case PB_T_INT32:
    case PB_T_INT64:
    case PB_T_UINT32:
    case PB_T_UINT64:
    case PB_T_SINT32:
    case PB_T_SINT64:
        return 1;
    default:
        return 0;
    }
}

static int pb_num_prop(JSContext* ctx, JSValueConst o, const char* prop,
    double lo, double hi, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, prop);
    double d;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "schema: \"%s\" must be a number", prop);
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (d != floor(d) || d < lo || d >= hi) {
        JS_ThrowRangeError(ctx, "schema: \"%s\" must be an integer in [%g, %g)",
            prop, lo, hi);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static int pb_schema_parse(JSContext* ctx, JSValueConst schema,
    pb_schema_t* out, int depth);

static void pb_schema_free(JSContext* ctx, pb_schema_t* s)
{
    size_t k;
    if (!s->f)
        return;
    for (k = 0; k < s->n; k++) {
        if (s->f[k].name != JS_ATOM_NULL)
            JS_FreeAtom(ctx, s->f[k].name);
        if (s->f[k].sub) {
            pb_schema_free(ctx, s->f[k].sub);
            free(s->f[k].sub);
            s->f[k].sub = NULL;
        }
    }
    free(s->f);
    s->f = NULL;
    s->n = 0;
}

static int pb_field_parse(JSContext* ctx, JSValueConst fields, uint32_t idx,
    pb_field_t* f, int depth)
{
    JSValue fo, pv;
    int64_t num;
    size_t nlen;
    const char* ns;
    int rc = -1;

    memset(f, 0, sizeof *f);
    fo = JS_GetPropertyUint32(ctx, fields, idx);
    if (JS_IsException(fo))
        return -1;
    if (!JS_IsObject(fo)) {
        JS_ThrowTypeError(ctx, "schema: field %u is not an object", idx);
        JS_FreeValue(ctx, fo);
        return -1;
    }
    if (pb_num_prop(ctx, fo, "number", 1.0, (double)PB_MAX_FIELD + 1, &num) < 0)
        goto out;
    f->number = (uint32_t)num;

    pv = JS_GetPropertyStr(ctx, fo, "name");
    if (JS_IsException(pv))
        goto out;
    if (!JS_IsString(pv)) {
        JS_ThrowTypeError(ctx, "schema: field %u needs a string \"name\"", idx);
        JS_FreeValue(ctx, pv);
        goto out;
    }
    ns = JS_ToCStringLen(ctx, &nlen, pv);
    if (!ns) {
        JS_FreeValue(ctx, pv);
        goto out;
    }
    f->name = JS_NewAtomLen(ctx, ns, nlen);
    JS_FreeCString(ctx, ns);
    JS_FreeValue(ctx, pv);
    if (f->name == JS_ATOM_NULL)
        goto out;

    pv = JS_GetPropertyStr(ctx, fo, "type");
    if (JS_IsException(pv))
        goto out;
    {
        int ttype;
        if (pb_type_of(ctx, pv, &ttype, "a field") < 0) {
            JS_FreeValue(ctx, pv);
            goto out;
        }
        f->type = (uint8_t)ttype;
    }
    JS_FreeValue(ctx, pv);

    pv = JS_GetPropertyStr(ctx, fo, "repeated");
    if (JS_IsException(pv))
        goto out;
    if (!JS_IsUndefined(pv)) {
        int isrep = JS_ToBool(ctx, pv);
        if (isrep < 0) {
            JS_FreeValue(ctx, pv);
            goto out;
        }
        if (isrep)
            f->flags |= PB_F_REPEATED;
    }
    JS_FreeValue(ctx, pv);

    if ((f->flags & PB_F_REPEATED) && pb_is_packable(f->type))
        f->flags |= PB_F_PACKED;
    pv = JS_GetPropertyStr(ctx, fo, "packed");
    if (JS_IsException(pv))
        goto out;
    if (!JS_IsUndefined(pv)) {
        int ispacked = JS_ToBool(ctx, pv);
        if (ispacked < 0) {
            JS_FreeValue(ctx, pv);
            goto out;
        }
        if (ispacked)
            f->flags |= PB_F_PACKED;
        else
            f->flags &= ~PB_F_PACKED;
    }
    JS_FreeValue(ctx, pv);

    pv = JS_GetPropertyStr(ctx, fo, "map");
    if (JS_IsException(pv))
        goto out;
    {
        int ismap = JS_ToBool(ctx, pv);
        JS_FreeValue(ctx, pv);
        if (ismap < 0)
            goto out;
        if (!ismap)
            goto no_map;
    }
    {
        f->flags |= PB_F_MAP;
        pv = JS_GetPropertyStr(ctx, fo, "keyType");
        if (JS_IsException(pv))
            goto out;
        {
            int ttype;
            if (pb_type_of(ctx, pv, &ttype, "map keyType") < 0 || !pb_key_type_ok(ttype)) {
                JS_ThrowTypeError(ctx, "schema: map keyType must be string, bool "
                                       "or an integral type");
                JS_FreeValue(ctx, pv);
                goto out;
            }
            f->ktype = (uint8_t)ttype;
        }
        JS_FreeValue(ctx, pv);
        pv = JS_GetPropertyStr(ctx, fo, "valueType");
        if (JS_IsException(pv))
            goto out;
        {
            int ttype;
            if (pb_type_of(ctx, pv, &ttype, "map valueType") < 0) {
                JS_FreeValue(ctx, pv);
                goto out;
            }
            f->vtype = (uint8_t)ttype;
        }
        JS_FreeValue(ctx, pv);
        if (f->vtype == PB_T_MESSAGE) {
            pv = JS_GetPropertyStr(ctx, fo, "message");
            if (JS_IsException(pv))
                goto out;
            f->sub = (pb_schema_t*)calloc(1, sizeof *f->sub);
            if (!f->sub) {
                JS_FreeValue(ctx, pv);
                JS_ThrowOutOfMemory(ctx);
                goto out;
            }
            if (pb_schema_parse(ctx, pv, f->sub, depth + 1) < 0) {
                JS_FreeValue(ctx, pv);
                free(f->sub);
                f->sub = NULL;
                goto out;
            }
            JS_FreeValue(ctx, pv);
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
no_map:

    if (f->type == PB_T_MESSAGE && !(f->flags & PB_F_MAP)) {
        pv = JS_GetPropertyStr(ctx, fo, "message");
        if (JS_IsException(pv))
            goto out;
        if (JS_IsUndefined(pv)) {
            JS_ThrowTypeError(ctx, "schema: a message field needs a nested "
                                   "\"message\" schema");
            JS_FreeValue(ctx, pv);
            goto out;
        }
        f->sub = (pb_schema_t*)calloc(1, sizeof *f->sub);
        if (!f->sub) {
            JS_FreeValue(ctx, pv);
            JS_ThrowOutOfMemory(ctx);
            goto out;
        }
        if (pb_schema_parse(ctx, pv, f->sub, depth + 1) < 0) {
            JS_FreeValue(ctx, pv);
            free(f->sub);
            f->sub = NULL;
            goto out;
        }
        JS_FreeValue(ctx, pv);
    }

    pv = JS_GetPropertyStr(ctx, fo, "oneof");
    if (JS_IsException(pv))
        goto out;
    if (!JS_IsUndefined(pv) && !JS_IsString(pv)) {
        JS_ThrowTypeError(ctx, "schema: \"oneof\" must be a string");
        JS_FreeValue(ctx, pv);
        goto out;
    }
    JS_FreeValue(ctx, pv);

    if ((f->flags & PB_F_MAP) && (f->flags & PB_F_REPEATED)) {
        JS_ThrowTypeError(ctx, "schema: a map field cannot be repeated");
        goto out;
    }
    rc = 0;
out:
    JS_FreeValue(ctx, fo);
    return rc;
}

static int pb_schema_parse(JSContext* ctx, JSValueConst schema,
    pb_schema_t* out, int depth)
{
    JSValue fields, lv;
    int64_t n = 0, k, i, j;
    int rc = -1;

    memset(out, 0, sizeof *out);
    if (depth >= PB_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "schema: nesting exceeds %d", PB_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(schema) || JS_IsArray(ctx, schema) == 1) {
        JS_ThrowTypeError(ctx, "schema: a message schema must be an object "
                               "with a \"fields\" array");
        return -1;
    }
    fields = JS_GetPropertyStr(ctx, schema, "fields");
    if (JS_IsException(fields))
        return -1;
    if (JS_IsArray(ctx, fields) != 1) {
        JS_ThrowTypeError(ctx, "schema: \"fields\" must be an array");
        JS_FreeValue(ctx, fields);
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, fields, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, fields);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (n < 0 || n > PB_MAX_FIELDS) {
        JS_ThrowRangeError(ctx, "schema: field count %lld is out of range",
            (long long)n);
        JS_FreeValue(ctx, fields);
        return -1;
    }
    out->f = (pb_field_t*)calloc((size_t)(n ? n : 1), sizeof(pb_field_t));
    if (!out->f) {
        JS_ThrowOutOfMemory(ctx);
        JS_FreeValue(ctx, fields);
        return -1;
    }
    out->n = (size_t)n;
    for (k = 0; k < n; k++) {
        if (pb_field_parse(ctx, fields, (uint32_t)k, &out->f[k], depth) < 0)
            goto fail;
    }
    for (i = 0; i < n; i++)
        for (j = 0; j < i; j++)
            if (out->f[i].number == out->f[j].number || out->f[i].name == out->f[j].name) {
                JS_ThrowTypeError(ctx, "schema: duplicate field number or name");
                goto fail;
            }
    rc = 0;
fail:
    JS_FreeValue(ctx, fields);
    if (rc < 0) {
        pb_schema_free(ctx, out);
        return -1;
    }
    return 0;
}

typedef struct {
    JSContext* ctx;
    pb_w_t w;
    int depth;
} pb_enc_t;

static int pb_enc_message(pb_enc_t* e, JSValueConst v, const pb_schema_t* s);
static int pb_enc_payload(pb_enc_t* e, int type, const pb_schema_t* sub,
    JSValueConst v);

static int pb_checked_int(JSContext* ctx, JSValueConst v, double lo,
    double hi, const char* what, int64_t* out)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "encode: a %s field requires a number", what);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0)
        return -1;
    if (isnan(d) || isinf(d) || d != floor(d) || d < lo || d >= hi) {
        JS_ThrowRangeError(ctx, "encode: a %s field value %g is not an "
                                "integer in [%g, %g)",
            what, d, lo, hi);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static int pb_value_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "encode: a bytes field requires a byte view");
        return -1;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "encode: a bytes field requires a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "encode: byte view out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

static int pb_enc_key(pb_enc_t* e, uint32_t number, int wire)
{
    pb_w_varint(&e->w, ((uint64_t)number << 3) | (uint64_t)(unsigned)wire);
    return 0;
}

static int pb_enc_payload(pb_enc_t* e, int type, const pb_schema_t* sub,
    JSValueConst v)
{
    JSContext* ctx = e->ctx;
    int64_t val;
    const char* what = pb_type_names[type];
    const uint8_t* p;
    size_t n;

    switch (type) {
    case PB_T_INT32:
    case PB_T_INT64:
    case PB_T_ENUM:
        if (pb_checked_int(ctx, v,
                type == PB_T_INT32 ? -2147483648.0 : -9223372036854775808.0,
                type == PB_T_INT32 ? 2147483648.0 : 9223372036854775808.0,
                what, &val)
            < 0)
            return -1;
        if (val < 0) {
            uint8_t t[10];
            int k;
            for (k = 0; k < 9; k++)
                t[k] = (uint8_t)(((uint64_t)val >> (7 * k)) & 0x7F) | 0x80;
            t[9] = (uint8_t)(((uint64_t)val >> 63) & 0x7F);
            pb_w_raw(&e->w, t, 10);
        } else {
            pb_w_varint(&e->w, (uint64_t)val);
        }
        return 0;
    case PB_T_UINT32:
        if (pb_checked_int(ctx, v, 0.0, 4294967296.0, what, &val) < 0)
            return -1;
        pb_w_varint(&e->w, (uint64_t)(uint32_t)val);
        return 0;
    case PB_T_UINT64:
        if (pb_checked_int(ctx, v, 0.0, 18446744073709551616.0, what, &val) < 0)
            return -1;
        pb_w_varint(&e->w, (uint64_t)val);
        return 0;
    case PB_T_SINT32:
        if (pb_checked_int(ctx, v, -2147483648.0, 2147483648.0, what, &val) < 0)
            return -1;
        {
            uint32_t z = ((uint32_t)val << 1) ^ (uint32_t)(val >> 31);
            pb_w_varint(&e->w, z);
        }
        return 0;
    case PB_T_SINT64:
        if (pb_checked_int(ctx, v, -9223372036854775808.0,
                9223372036854775808.0, what, &val)
            < 0)
            return -1;
        {
            uint64_t z = (uint64_t)val << 1;
            if (val < 0)
                z = ~z;
            pb_w_varint(&e->w, z);
        }
        return 0;
    case PB_T_FIXED32:
        if (pb_checked_int(ctx, v, 0.0, 4294967296.0, what, &val) < 0)
            return -1;
        pb_w_le32(&e->w, (uint32_t)val);
        return 0;
    case PB_T_SFIXED32:
        if (pb_checked_int(ctx, v, -2147483648.0, 2147483648.0, what, &val) < 0)
            return -1;
        pb_w_le32(&e->w, (uint32_t)val);
        return 0;
    case PB_T_FIXED64:
        if (pb_checked_int(ctx, v, 0.0, 18446744073709551616.0, what, &val) < 0)
            return -1;
        pb_w_le64(&e->w, (uint64_t)val);
        return 0;
    case PB_T_SFIXED64:
        if (pb_checked_int(ctx, v, -9223372036854775808.0,
                9223372036854775808.0, what, &val)
            < 0)
            return -1;
        pb_w_le64(&e->w, (uint64_t)val);
        return 0;
    case PB_T_FLOAT:
    case PB_T_DOUBLE: {
        double d;
        if (!JS_IsNumber(v)) {
            JS_ThrowTypeError(ctx, "encode: a %s field requires a number", what);
            return -1;
        }
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (type == PB_T_FLOAT) {
            float f = (float)d;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            pb_w_le32(&e->w, bits);
        } else {
            uint64_t bits;
            memcpy(&bits, &d, 8);
            pb_w_le64(&e->w, bits);
        }
        return 0;
    }
    case PB_T_BOOL:
        if (!JS_IsBool(v) && !JS_IsNumber(v)) {
            JS_ThrowTypeError(ctx, "encode: a bool field requires a boolean");
            return -1;
        }
        pb_w_varint(&e->w, JS_ToBool(ctx, v) ? 1 : 0);
        return 0;
    case PB_T_STRING: {
        const char* s;
        if (!JS_IsString(v)) {
            JS_ThrowTypeError(ctx, "encode: a string field requires a string");
            return -1;
        }
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        pb_w_varint(&e->w, n);
        pb_w_raw(&e->w, s, n);
        JS_FreeCString(ctx, s);
        return 0;
    }
    case PB_T_BYTES:
        if (pb_value_bytes(ctx, v, &p, &n) < 0)
            return -1;
        pb_w_varint(&e->w, n);
        pb_w_raw(&e->w, p, n);
        return 0;
    case PB_T_MESSAGE: {
        pb_enc_t tmp = *e;
        tmp.w.p = NULL;
        tmp.w.n = tmp.w.cap = 0;
        tmp.w.oom = 0;
        tmp.depth = e->depth + 1;
        if (pb_enc_message(&tmp, v, sub) < 0) {
            free(tmp.w.p);
            return -1;
        }
        if (tmp.w.oom) {
            free(tmp.w.p);
            JS_ThrowOutOfMemory(e->ctx);
            return -1;
        }
        pb_w_varint(&e->w, tmp.w.n);
        pb_w_raw(&e->w, tmp.w.p, tmp.w.n);
        free(tmp.w.p);
        return e->w.oom ? -1 : 0;
    }
    default:
        return -1;
    }
}

static int pb_enc_map_key(pb_enc_t* e, int ktype, const char* s, size_t n)
{
    int64_t val;
    if (ktype == PB_T_STRING) {
        pb_w_varint(&e->w, n);
        pb_w_raw(&e->w, s, n);
        return 0;
    }
    if (ktype == PB_T_BOOL) {
        int b = (n == 4 && memcmp(s, "true", 4) == 0);
        if (!b && !(n == 5 && memcmp(s, "false", 5) == 0)) {
            JS_ThrowTypeError(e->ctx, "encode: a bool map key must be "
                                      "\"true\" or \"false\"");
            return -1;
        }
        pb_w_varint(&e->w, b);
        return 0;
    }
    {
        char buf[24];
        char* end = NULL;
        long long v;
        if (n == 0 || n >= sizeof buf) {
            JS_ThrowTypeError(e->ctx, "encode: map key is not an integer");
            return -1;
        }
        memcpy(buf, s, n);
        buf[n] = 0;
        v = strtoll(buf, &end, 10);
        if (!end || end == buf || *end != 0) {
            JS_ThrowTypeError(e->ctx, "encode: map key \"%s\" is not an integer",
                buf);
            return -1;
        }
        val = (int64_t)v;
    }
    switch (ktype) {
    case PB_T_INT32:
        if (val < -2147483648LL || val > 2147483647LL)
            goto badkey;
        pb_w_varint(&e->w, (uint64_t)val);
        break;
    case PB_T_INT64:
        pb_w_varint(&e->w, (uint64_t)val);
        break;
    case PB_T_UINT32:
        if (val < 0 || val > 4294967295LL)
            goto badkey;
        pb_w_varint(&e->w, (uint64_t)val);
        break;
    case PB_T_UINT64:
        if (val < 0)
            goto badkey;
        pb_w_varint(&e->w, (uint64_t)val);
        break;
    case PB_T_SINT32:
        if (val < -2147483648LL || val > 2147483647LL)
            goto badkey;
        {
            uint32_t z = ((uint32_t)val << 1) ^ (uint32_t)(val >> 31);
            pb_w_varint(&e->w, z);
        }
        break;
    case PB_T_SINT64: {
        uint64_t z = (uint64_t)val << 1;
        if (val < 0)
            z = ~z;
        pb_w_varint(&e->w, z);
    } break;
    default:
        return -1;
    }
    return 0;
badkey:
    JS_ThrowRangeError(e->ctx, "encode: map key \"%.*s\" is out of range for "
                               "its key type",
        (int)n, s);
    return -1;
}

static int pb_enc_unknowns(pb_enc_t* e, JSValueConst v)
{
    JSValue u = JS_GetPropertyStr(e->ctx, v, PB_UNKNOWN_KEY);
    JSValue lv;
    int64_t n = 0, i;
    if (JS_IsException(u))
        return -1;
    if (JS_IsArray(e->ctx, u) != 1) {
        JS_FreeValue(e->ctx, u);
        return 0;
    }
    lv = JS_GetPropertyStr(e->ctx, u, "length");
    if (JS_IsException(lv) || JS_ToInt64(e->ctx, &n, lv) < 0) {
        JS_FreeValue(e->ctx, lv);
        JS_FreeValue(e->ctx, u);
        return -1;
    }
    JS_FreeValue(e->ctx, lv);
    for (i = 0; i < n; i++) {
        const uint8_t* p;
        size_t plen;
        JSValue el = JS_GetPropertyUint32(e->ctx, u, (uint32_t)i);
        if (JS_IsException(el)) {
            JS_FreeValue(e->ctx, u);
            return -1;
        }
        if (pb_value_bytes(e->ctx, el, &p, &plen) < 0) {
            JS_FreeValue(e->ctx, el);
            JS_FreeValue(e->ctx, u);
            return -1;
        }
        pb_w_raw(&e->w, p, plen);
        JS_FreeValue(e->ctx, el);
    }
    JS_FreeValue(e->ctx, u);
    return e->w.oom ? -1 : 0;
}

static int pb_enc_map_entry(pb_enc_t* e, const pb_field_t* f, JSAtom katom,
    JSValueConst kv)
{
    pb_enc_t sub = *e;
    size_t klen;
    const char* ks;
    int rc = -1;

    sub.w.p = NULL;
    sub.w.n = sub.w.cap = 0;
    sub.w.oom = 0;
    ks = JS_AtomToCStringLen(e->ctx, &klen, katom);
    if (!ks)
        return -1;
    if (pb_enc_key(&sub, 1, pb_wire_of_type(f->ktype)) < 0)
        goto out;
    if (pb_enc_map_key(&sub, f->ktype, ks, klen) < 0)
        goto out;
    if (pb_enc_key(&sub, 2, pb_wire_of_type(f->vtype)) < 0)
        goto out;
    if (pb_enc_payload(&sub, f->vtype, f->sub, kv) < 0)
        goto out;
    if (sub.w.oom) {
        JS_ThrowOutOfMemory(e->ctx);
        goto out;
    }
    if (pb_enc_key(e, f->number, 2) < 0)
        goto out;
    pb_w_varint(&e->w, sub.w.n);
    pb_w_raw(&e->w, sub.w.p, sub.w.n);
    if (e->w.oom) {
        JS_ThrowOutOfMemory(e->ctx);
        goto out;
    }
    rc = 0;
out:
    JS_FreeCString(e->ctx, ks);
    free(sub.w.p);
    return rc;
}

static int pb_enc_map(pb_enc_t* e, JSValueConst v, const pb_field_t* f)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (!JS_IsObject(v) || JS_IsArray(e->ctx, v) == 1) {
        JS_ThrowTypeError(e->ctx, "encode: a map field requires an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(e->ctx, &tab, &len, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        JSValue kv = JS_GetProperty(e->ctx, v, tab[k].atom);
        if (JS_IsException(kv)) {
            rc = -1;
            break;
        }
        rc = pb_enc_map_entry(e, f, tab[k].atom, kv);
        JS_FreeValue(e->ctx, kv);
    }
    JS_FreePropertyEnum(e->ctx, tab, len);
    return rc;
}

static int pb_enc_repeated(pb_enc_t* e, JSValueConst v, const pb_field_t* f)
{
    JSValue lv;
    int64_t len, i;

    if (JS_IsArray(e->ctx, v) != 1) {
        JS_ThrowTypeError(e->ctx, "encode: a repeated field requires an array");
        return -1;
    }
    lv = JS_GetPropertyStr(e->ctx, v, "length");
    if (JS_IsException(lv) || JS_ToInt64(e->ctx, &len, lv) < 0) {
        JS_FreeValue(e->ctx, lv);
        return -1;
    }
    JS_FreeValue(e->ctx, lv);
    if (len <= 0)
        return 0;
    if ((f->flags & PB_F_PACKED) && pb_is_packable(f->type)) {
        pb_enc_t sub = *e;
        sub.w.p = NULL;
        sub.w.n = sub.w.cap = 0;
        sub.w.oom = 0;
        for (i = 0; i < len && !sub.w.oom; i++) {
            JSValue el = JS_GetPropertyUint32(e->ctx, v, (uint32_t)i);
            int rc;
            if (JS_IsException(el)) {
                free(sub.w.p);
                return -1;
            }
            rc = pb_enc_payload(&sub, f->type, f->sub, el);
            JS_FreeValue(e->ctx, el);
            if (rc < 0) {
                free(sub.w.p);
                return -1;
            }
        }
        if (sub.w.oom) {
            free(sub.w.p);
            JS_ThrowOutOfMemory(e->ctx);
            return -1;
        }
        if (pb_enc_key(e, f->number, 2) < 0) {
            free(sub.w.p);
            return -1;
        }
        pb_w_varint(&e->w, sub.w.n);
        pb_w_raw(&e->w, sub.w.p, sub.w.n);
        free(sub.w.p);
        return e->w.oom ? -1 : 0;
    }
    for (i = 0; i < len && !e->w.oom; i++) {
        JSValue el = JS_GetPropertyUint32(e->ctx, v, (uint32_t)i);
        int rc;
        if (JS_IsException(el))
            return -1;
        if (pb_enc_key(e, f->number, pb_wire_of_type(f->type)) < 0) {
            JS_FreeValue(e->ctx, el);
            return -1;
        }
        rc = pb_enc_payload(e, f->type, f->sub, el);
        JS_FreeValue(e->ctx, el);
        if (rc < 0)
            return -1;
    }
    return e->w.oom ? -1 : 0;
}

static int pb_enc_field(pb_enc_t* e, JSValueConst v, const pb_field_t* f)
{
    if (f->flags & PB_F_MAP)
        return pb_enc_map(e, v, f);
    if (f->flags & PB_F_REPEATED)
        return pb_enc_repeated(e, v, f);
    if (pb_enc_key(e, f->number, pb_wire_of_type(f->type)) < 0)
        return -1;
    return pb_enc_payload(e, f->type, f->sub, v);
}

static int pb_refuse_view(JSContext* ctx, JSValueConst v)
{
    size_t off, len, bpe;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_FreeValue(ctx, buf);
    JS_ThrowTypeError(ctx, "encode: a byte view is a bytes value, not a message");
    return -1;
}

static int pb_enc_message(pb_enc_t* e, JSValueConst v, const pb_schema_t* s)
{
    size_t k;

    if (e->depth >= PB_MAX_DEPTH) {
        JS_ThrowRangeError(e->ctx, "encode: message nesting exceeds %d",
            PB_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(e->ctx, "encode: a message value must be an object");
        return -1;
    }
    if (JS_IsArray(e->ctx, v) == 1) {
        JS_ThrowTypeError(e->ctx, "encode: a message field cannot be an array");
        return -1;
    }
    if (JS_IsFunction(e->ctx, v)) {
        JS_ThrowTypeError(e->ctx, "encode: a function is not a message");
        return -1;
    }
    if (pb_refuse_view(e->ctx, v) < 0)
        return -1;
    e->depth++;
    for (k = 0; k < s->n && !e->w.oom; k++) {
        const pb_field_t* f = &s->f[k];
        JSValue pv = JS_GetProperty(e->ctx, v, f->name);
        int rc;
        if (JS_IsException(pv)) {
            e->depth--;
            return -1;
        }
        if (JS_IsUndefined(pv) || JS_IsNull(pv)) {
            JS_FreeValue(e->ctx, pv);
            continue;
        }
        rc = pb_enc_field(e, pv, f);
        JS_FreeValue(e->ctx, pv);
        if (rc < 0) {
            e->depth--;
            return -1;
        }
    }
    e->depth--;
    if (e->w.oom) {
        JS_ThrowOutOfMemory(e->ctx);
        return -1;
    }
    return pb_enc_unknowns(e, v);
}

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

static int pb_field_wire_ok(const pb_field_t* f, int wire)
{
    if (f->flags & PB_F_MAP)
        return wire == 2;
    if (f->flags & PB_F_REPEATED) {
        if (wire == 2 && pb_is_packable(f->type))
            return 1;
        return wire == pb_wire_of_type(f->type);
    }
    return wire == pb_wire_of_type(f->type);
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
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
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

static JSValue pb_new_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, out;
    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue pb_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pb_schema_t s;
    pb_enc_t e;
    JSValue out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Proto.encode(value, schema): a schema "
                                      "is required");
    if (pb_schema_parse(ctx, argv[1], &s, 0) < 0)
        return JS_EXCEPTION;
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    if (pb_enc_message(&e, argv[0], &s) < 0 || e.w.oom) {
        if (e.w.oom)
            JS_ThrowOutOfMemory(ctx);
        free(e.w.p);
        pb_schema_free(ctx, &s);
        return JS_EXCEPTION;
    }
    out = pb_new_bytes(ctx, e.w.p, e.w.n);
    free(e.w.p);
    pb_schema_free(ctx, &s);
    return out;
}

static JSValue pb_decode(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_proto_namespace(JSContext* ctx)
{
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return proto;
    if (JS_DefinePropertyValueStr(ctx, proto, "encode",
            JS_NewCFunction(ctx, pb_encode, "encode", 2), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, proto, "decode",
               JS_NewCFunction(ctx, pb_decode, "decode", 2), JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, proto);
        return JS_EXCEPTION;
    }
    return proto;
}

int dyn_proto_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto = dyn_proto_namespace(ctx);
    if (JS_IsException(proto))
        return -1;
    return JS_SetModuleExport(ctx, m, "Proto", proto);
}

int dyn_proto_add_exports(JSContext* ctx, JSModuleDef* m)
{
    (void)ctx;
    return JS_AddModuleExport(ctx, m, "Proto");
}

#endif