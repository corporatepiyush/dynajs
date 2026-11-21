#include "protobuf-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROTOBUF)

#define PB_MAX_FIELDS 65536

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

void pb_schema_free(JSContext* ctx, pb_schema_t* s)
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

static int pb_cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

int pb_schema_parse(JSContext* ctx, JSValueConst schema,
    pb_schema_t* out, int depth)
{
    JSValue fields, lv;
    int64_t n = 0, k, i;
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
    {
        uint32_t* nums = (uint32_t*)js_malloc(ctx, (size_t)(n ? n : 1) * sizeof(*nums));
        JSAtom* names = nums
            ? (JSAtom*)js_malloc(ctx, (size_t)(n ? n : 1) * sizeof(*names))
            : NULL;
        if (!nums || !names) {
            js_free(ctx, nums);
            js_free(ctx, names);
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < n; i++) {
            nums[i] = out->f[i].number;
            names[i] = out->f[i].name;
        }
        qsort(nums, (size_t)n, sizeof(*nums), pb_cmp_u32);
        qsort(names, (size_t)n, sizeof(*names), pb_cmp_u32);
        for (i = 1; i < n; i++)
            if (nums[i] == nums[i - 1] || names[i] == names[i - 1]) {
                js_free(ctx, nums);
                js_free(ctx, names);
                JS_ThrowTypeError(ctx, "schema: duplicate field number or name");
                goto fail;
            }
        js_free(ctx, nums);
        js_free(ctx, names);
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

int pb_field_wire_ok(const pb_field_t* f, int wire)
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

#endif
