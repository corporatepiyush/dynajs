#include "bytes-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#define DYN_BH_UNKNOWN (-1)

static JSValue dyn_bytes_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    return dyn_utf8_lossy_string(ctx, buf, n);
}

JSValue dyn_bh_backing(JSValueConst v)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(v, dyn_bh_class_id);
    return b ? b->u8 : JS_UNDEFINED;
}

void dyn_bh_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(val, dyn_bh_class_id);
    if (!b)
        return;
    JS_FreeValueRT(rt, b->u8);
    dyn_nat_free(b);
}

void dyn_bh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_bh_t* b = (dyn_bh_t*)JS_GetOpaque(val, dyn_bh_class_id);
    if (b)
        JS_MarkValue(rt, b->u8, mark_func);
}

static void dyn_bh_scan(const uint8_t* p, size_t n, int* ascii, int* utf8)
{
    size_t i = 0;

    while (i + 8 <= n) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        if (word & 0x8080808080808080ULL)
            break;
        i += 8;
    }
    while (i < n && p[i] < 0x80)
        i++;
    if (i == n) {
        *ascii = 1;
        *utf8 = 1;
        return;
    }

    *ascii = 0;
    *utf8 = (simd.validate_utf8(p + i, n - i) == n - i);
}

static JSValue dyn_bh_wrap_flags(JSContext* ctx, JSValue u8, int inherit_ascii,
    JSValueConst new_target);

JSValue dyn_bh_wrap(JSContext* ctx, JSValue u8)
{
    return dyn_bh_wrap_flags(ctx, u8, 0, JS_UNDEFINED);
}

static JSValue dyn_bh_wrap_flags(JSContext* ctx, JSValue u8, int inherit_ascii,
    JSValueConst new_target)
{
    dyn_bh_t* b;
    uint8_t* p = NULL;
    size_t n = 0;
    JSValue obj, proto;

    if (JS_IsException(u8))
        return u8;
    proto = dyn_ctor_proto(ctx, new_target, dyn_bh_class_id);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, u8);
        return proto;
    }
    if (dyn_bytes_view(ctx, u8, &p, &n) < 0) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    b = (dyn_bh_t*)dyn_nat_calloc(1, sizeof(*b));
    if (!b) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, u8);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (inherit_ascii) {
        b->is_ascii = 1;
        b->is_valid_utf8 = 1;
    } else {
        dyn_bh_scan(p, n, &b->is_ascii, &b->is_valid_utf8);
    }
    b->u8 = u8;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_bh_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, u8);
        dyn_nat_free(b);
        return obj;
    }
    JS_SetOpaque(obj, b);
    return obj;
}

dyn_bh_t* dyn_bh_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_bh_t*)JS_GetOpaque2(ctx, v, dyn_bh_class_id);
}

static int dyn_bh_resolve(JSContext* ctx, dyn_bh_t* b)
{
    uint8_t* p;
    size_t n;
    if (!b->escaped && b->is_ascii != DYN_BH_UNKNOWN && b->is_valid_utf8 != DYN_BH_UNKNOWN)
        return 0;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return -1;
    dyn_bh_scan(p, n, &b->is_ascii, &b->is_valid_utf8);
    return 0;
}

JSValue dyn_bh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    uint8_t* p = NULL;
    size_t n = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new Bytes(data) requires an argument");

    if (JS_IsString(argv[0])) {
        size_t sl;
        const char* cs = JS_ToCStringLen(ctx, &sl, argv[0]);
        JSValue u8;
        if (!cs)
            return JS_EXCEPTION;
        u8 = dyn_nat_u8_copy(ctx, (const uint8_t*)cs, sl);
        JS_FreeCString(ctx, cs);
        return dyn_bh_wrap_flags(ctx, u8, 0, new_target);
    }
    if (dyn_bytes_view(ctx, argv[0], &p, &n) < 0)
        return JS_EXCEPTION;
    return dyn_bh_wrap_flags(ctx, dyn_nat_u8_copy(ctx, p, n), 0,
        new_target);
}

JSValue dyn_bh_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    (void)this_val;
    if (magic == 0) {
        uint64_t n = 0;
        if (argc < 1 || JS_ToIndex(ctx, &n, argv[0]))
            return JS_ThrowTypeError(ctx, "Bytes.alloc(n) requires a length");
        if (n > (uint64_t)INT32_MAX)
            return JS_ThrowRangeError(ctx, "Bytes.alloc: length too large");
        return dyn_bh_wrap(ctx, dyn_nat_u8_copy(ctx, NULL, (size_t)n));
    }
    if (magic == 1) {
        return JS_NewBool(ctx, argc >= 1 && JS_GetOpaque(argv[0], dyn_bh_class_id) != NULL);
    }
    {
        int64_t alen = 0, i, total;
        size_t total_bytes = 0, o = 0;
        uint8_t* out;
        JSValue res;
        JSValueConst list = argv[0];
        int is_list;

        if (argc < 1)
            return JS_ThrowTypeError(ctx, "Bytes.concat(list) requires an array");
        is_list = JS_IsArray(ctx, argv[0]);
        if (is_list) {
            JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
            if (JS_IsException(lv) || JS_ToInt64(ctx, &alen, lv)) {
                JS_FreeValue(ctx, lv);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
            if (alen < 0 || alen > (1LL << 20))
                return JS_ThrowRangeError(ctx,
                    "Bytes.concat: the list length must be in [0, 1048576]");
        }
        total = is_list ? alen + (argc - 1) : argc;

#define DYN_CONCAT_ELEM(idx)                                         \
    ((idx) < alen ? JS_GetPropertyUint32(ctx, list, (uint32_t)(idx)) \
                  : JS_DupValue(ctx, argv[(idx) - alen + first_extra]))
        {
            int first_extra = is_list ? 1 : 0;

            for (i = 0; i < total; i++) {
                JSValue e = DYN_CONCAT_ELEM(i);
                uint8_t* p;
                size_t n;
                if (JS_IsException(e))
                    return JS_EXCEPTION;
                if (dyn_bytes_view(ctx, e, &p, &n) < 0) {
                    JS_FreeValue(ctx, e);
                    return JS_EXCEPTION;
                }
                JS_FreeValue(ctx, e);
                total_bytes += n;
            }
            out = (uint8_t*)dyn_nat_malloc(total_bytes ? total_bytes : 1);
            if (!out)
                return JS_ThrowOutOfMemory(ctx);
            for (i = 0; i < total; i++) {
                JSValue e = DYN_CONCAT_ELEM(i);
                uint8_t* p;
                size_t n;
                if (JS_IsException(e) || dyn_bytes_view(ctx, e, &p, &n) < 0) {
                    JS_FreeValue(ctx, e);
                    dyn_nat_free(out);
                    return JS_EXCEPTION;
                }
                if (o + n > total_bytes) {
                    JS_FreeValue(ctx, e);
                    dyn_nat_free(out);
                    return JS_ThrowTypeError(ctx, "Bytes.concat: the list changed during concatenation");
                }
                memcpy(out + o, p, n);
                o += n;
                JS_FreeValue(ctx, e);
            }
#undef DYN_CONCAT_ELEM
        }
        res = dyn_bh_wrap(ctx, dyn_nat_u8_copy(ctx, out, o));
        dyn_nat_free(out);
        return res;
    }
}

JSValue dyn_bh_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint8_t* p;
    size_t n;
    if (!b)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
            return JS_EXCEPTION;
        return JS_NewInt64(ctx, (int64_t)n);
    case 1:
        if (dyn_bh_resolve(ctx, b) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(ctx, b->is_ascii);
    case 2:
        if (dyn_bh_resolve(ctx, b) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(ctx, b->is_valid_utf8);
    default:
        b->escaped = 1;
        return JS_DupValue(ctx, b->u8);
    }
}

JSValue dyn_bh_slice(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_bh_t* b = dyn_bh_of(ctx, this_val);
    uint8_t* p;
    size_t n;
    int64_t start = 0, end;
    JSValue sub, ret;

    if (!b)
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, b->u8, &p, &n) < 0)
        return JS_EXCEPTION;
    end = (int64_t)n;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToInt64(ctx, &start, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    if (start < 0)
        start += (int64_t)n;
    if (end < 0)
        end += (int64_t)n;
    if (start < 0)
        start = 0;
    if (start > (int64_t)n)
        start = (int64_t)n;
    if (end > (int64_t)n)
        end = (int64_t)n;
    if (end < start)
        end = start;

    {
        JSValue ab;
        size_t off = 0, blen = 0, bpe = 0;
        JSValueConst a[3];
        JSValue av[3];

        ab = JS_GetTypedArrayBuffer(ctx, b->u8, &off, &blen, &bpe);
        if (JS_IsException(ab))
            return ab;
        av[0] = ab;
        av[1] = JS_NewInt64(ctx, (int64_t)off + start);
        av[2] = JS_NewInt64(ctx, end - start);
        a[0] = av[0];
        a[1] = av[1];
        a[2] = av[2];
        sub = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, av[1]);
        JS_FreeValue(ctx, av[2]);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(sub))
            return sub;
    }
    if (dyn_bh_resolve(ctx, b) < 0) {
        JS_FreeValue(ctx, sub);
        return JS_EXCEPTION;
    }
    ret = dyn_bh_wrap_flags(ctx, sub, b->is_ascii, JS_UNDEFINED);
    return ret;
}

DYN_BH_FORWARD(dyn_bh_to_utf8, dyn_bytes_to_utf8, 0)

JSValue dyn_bh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_bh_to_utf8(ctx, this_val, 0, NULL);
}

#endif
