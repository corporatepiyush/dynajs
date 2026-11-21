#include "protobuf-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROTOBUF)

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
    if (d >= 9223372036854775808.0) {
        uint64_t u = (uint64_t)d;
        memcpy(out, &u, sizeof u);
    } else {
        *out = (int64_t)d;
    }
    return 0;
}

static int pb_value_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
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
        errno = 0;
        v = strtoll(buf, &end, 10);
        if (!end || end == buf || *end != 0 || errno == ERANGE) {
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
    JSValue buf;
    if (JS_GetBufferKind(v) != JS_BUFFER_KIND_VIEW)
        return 0;
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_FreeValue(ctx, buf);
    JS_ThrowTypeError(ctx, "encode: a byte view is a bytes value, not a message");
    return -1;
}

int pb_enc_message(pb_enc_t* e, JSValueConst v, const pb_schema_t* s)
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

#endif
