#include "bytes-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#define DYN_TXT_FORWARD(name, fn, maxargs)                               \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_txt_t* t = dyn_txt_of(ctx, this_val);                        \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!t)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = t->str;                                                   \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

JSClassID dyn_bh_class_id;

int dyn_bytes_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    {
        JSValue inner = dyn_bh_backing(v);
        if (!JS_IsUndefined(inner))
            v = inner;
    }

    buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (!JS_IsException(buf)) {
        if (bpe != 1) {
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx, "expected a byte view (Uint8Array, Int8Array, "
                                   "Uint8ClampedArray, DataView) or an ArrayBuffer; "
                                   "view a wider array as new Uint8Array(v.buffer, v.byteOffset, v.byteLength)");
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base) {
            JS_ThrowTypeError(ctx, "typed array buffer is detached");
            return -1;
        }
        if (off > ab || len > ab - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));

    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *pp = base;
        *pn = ab;
        return 0;
    }
    return -1;
}

static int dyn_bytes_view_w(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    JSValue inner = dyn_bh_backing(v);
    if (JS_CheckWritableBuffer(ctx, JS_IsUndefined(inner) ? v : inner) < 0)
        return -1;
    return dyn_bytes_view(ctx, v, pp, pn);
}

static JSValue dyn_bytes_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *a, *b;
    size_t an, bn, minlen;
    int cmp;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    minlen = an < bn ? an : bn;
    cmp = minlen ? memcmp(a, b, minlen) : 0;
    if (cmp != 0)
        cmp = cmp < 0 ? -1 : 1;
    else
        cmp = (an > bn) - (an < bn);
    return JS_NewInt32(ctx, cmp);
}

static JSValue dyn_bytes_equal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *a, *b;
    size_t an, bn;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_view(ctx, argv[0], &a, &an))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &b, &bn))
        return JS_EXCEPTION;

    return JS_NewBool(ctx, an == bn && (an == 0 || memcmp(a, b, an) == 0));
}

int dyn_bytes_needle(JSContext* ctx, JSValueConst v, uint8_t* byte_out,
    const uint8_t** pat, size_t* plen)
{
    if (JS_IsNumber(v)) {
        int32_t b;
        if (JS_ToInt32(ctx, &b, v))
            return -1;
        *byte_out = (uint8_t)b;
        *pat = byte_out;
        *plen = 1;
        return 0;
    }
    {
        uint8_t* p;
        size_t n;
        if (dyn_bytes_view(ctx, v, &p, &n))
            return -1;
        *pat = p;
        *plen = n;
        return 0;
    }
}

static JSValue dyn_bytes_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n;
    (void)this_val;
    (void)argc;

    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;

    if (plen == 0)
        return JS_NewBool(ctx, 1);
    if (plen > n)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, simd.strfind(buf, n, pat, plen) != SIZE_MAX);
}

static JSValue dyn_bytes_starts_with(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "startsWith(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen > n - start)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, plen == 0 || memcmp(buf + start, pat, plen) == 0);
}

static JSValue dyn_bytes_ends_with(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, end;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "endsWith(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &end))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (end == DYN_BYTES_IDX_UNSET)
        end = n;
    if (end > n)
        end = n;

    if (plen > end)
        return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, plen == 0 || memcmp(buf + end - plen, pat, plen) == 0);
}

static JSValue dyn_bytes_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue len_val, elem, result = JS_EXCEPTION;
    uint32_t n, i;
    uint8_t* out;
    size_t total, used;
    (void)this_val;
    (void)argc;

    if (!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "concat(list) requires an array");
    len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(len_val))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &n, len_val)) {
        JS_FreeValue(ctx, len_val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len_val);

    total = 0;
    for (i = 0; i < n; i++) {
        uint8_t* p;
        size_t plen;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            return JS_EXCEPTION;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, elem);
        total += plen;
    }

    out = (uint8_t*)dyn_nat_malloc(total ? total : 1);
    if (!out)
        return JS_ThrowOutOfMemory(ctx);

    used = 0;
    for (i = 0; i < n; i++) {
        uint8_t* p;
        size_t plen, remaining;
        elem = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(elem))
            goto done;
        if (dyn_bytes_view(ctx, elem, &p, &plen)) {
            JS_FreeValue(ctx, elem);
            goto done;
        }
        JS_FreeValue(ctx, elem);
        remaining = total - used;
        if (plen > remaining)
            plen = remaining;
        if (plen)
            memcpy(out + used, p, plen);
        used += plen;
    }
    result = dyn_nat_u8_copy(ctx, out, used);

done:
    dyn_nat_free(out);
    return result;
}

static JSValue dyn_bytes_copy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t dst_off = 0, src_off = 0, len = UINT64_MAX;
    uint8_t *dst, *src;
    size_t dn, sn;
    (void)this_val;
    (void)argc;

    if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &dst_off, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[3]) && JS_ToIndex(ctx, &src_off, argv[3]))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[4]) && JS_ToIndex(ctx, &len, argv[4]))
        return JS_EXCEPTION;

    if (dyn_bytes_view_w(ctx, argv[0], &dst, &dn))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[1], &src, &sn))
        return JS_EXCEPTION;

    if (dst_off > (uint64_t)dn)
        return JS_ThrowRangeError(ctx, "copy: dstOffset out of bounds");
    if (src_off > (uint64_t)sn)
        return JS_ThrowRangeError(ctx, "copy: srcOffset out of bounds");

    {
        uint64_t davail = (uint64_t)dn - dst_off, savail = (uint64_t)sn - src_off;
        uint64_t maxlen = davail < savail ? davail : savail;
        if (len == UINT64_MAX)
            len = maxlen;
        else if (len > maxlen)
            return JS_ThrowRangeError(ctx, "copy: length out of bounds");
    }

    if (len)
        memmove(dst + (size_t)dst_off, src + (size_t)src_off, (size_t)len);
    return JS_NewInt64(ctx, (int64_t)len);
}

static const JSClassDef dyn_bh_class = {
    "Bytes",
    .finalizer = dyn_bh_finalizer,
    .gc_mark = dyn_bh_mark,
};

DYN_BH_FORWARD(dyn_bh_compare, dyn_bytes_compare, 1)

DYN_BH_FORWARD(dyn_bh_equals, dyn_bytes_equal, 1)

DYN_BH_FORWARD(dyn_bh_index_of, dyn_bytes_index_of, 2)

DYN_BH_FORWARD(dyn_bh_last_index_of, dyn_bytes_last_index_of, 2)

DYN_BH_FORWARD(dyn_bh_includes, dyn_bytes_contains, 1)

DYN_BH_FORWARD(dyn_bh_count, dyn_bytes_count, 2)

DYN_BH_FORWARD(dyn_bh_starts_with, dyn_bytes_starts_with, 2)

DYN_BH_FORWARD(dyn_bh_ends_with, dyn_bytes_ends_with, 2)

DYN_BH_FORWARD(dyn_bh_index_of_any, dyn_text_index_of_any, 2)

JSClassID dyn_txt_class_id;

static const JSClassDef dyn_txt_class = {
    "Text",
    .finalizer = dyn_txt_finalizer,
    .gc_mark = dyn_txt_mark,
};

DYN_TXT_FORWARD(dyn_txt_m_count_utf8, dyn_text_count_utf8, 0)

DYN_TXT_FORWARD(dyn_txt_m_latin1_to_utf8, dyn_text_latin1_to_utf8, 0)

DYN_TXT_FORWARD(dyn_txt_m_utf8_to_latin1, dyn_text_utf8_to_latin1, 0)

DYN_TXT_FORWARD(dyn_txt_m_utf8_to_utf16, dyn_text_utf8_to_utf16, 0)

static const JSCFunctionListEntry dyn_txt_proto[] = {
    JS_CGETSET_MAGIC_DEF("isWide", dyn_txt_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("value", dyn_txt_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("isValidUtf8", dyn_txt_get, NULL, 4),
    JS_CFUNC_DEF("isValidUtf16", 0, dyn_txt_m_valid_utf16),
    JS_CFUNC_DEF("countUtf8", 0, dyn_txt_m_count_utf8),
    JS_CFUNC_DEF("countUtf16", 0, dyn_txt_m_count_utf16_str),
    JS_CFUNC_DEF("latin1ToUtf8", 0, dyn_txt_m_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 0, dyn_txt_m_utf8_to_latin1),
    JS_CFUNC_DEF("utf8ToUtf16", 0, dyn_txt_m_utf8_to_utf16),
    JS_CFUNC_DEF("utf16ToUtf8", 0, dyn_txt_m_utf16_to_utf8),
    JS_CFUNC_DEF("toBytes", 0, dyn_txt_to_bytes),
    JS_CFUNC_DEF("toString", 0, dyn_txt_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_txt_to_string),
};

static const JSCFunctionListEntry dyn_text_funcs[] = {
    JS_CFUNC_DEF("isValidUtf8", 1, dyn_text_is_valid_utf8),
    JS_CFUNC_DEF("latin1ToUtf8", 1, dyn_text_latin1_to_utf8),
    JS_CFUNC_DEF("utf8ToLatin1", 1, dyn_text_utf8_to_latin1),
    JS_CFUNC_DEF("countUtf8", 1, dyn_text_count_utf8),
    JS_CFUNC_DEF("utf8ToUtf16", 1, dyn_text_utf8_to_utf16),
    JS_CFUNC_DEF("utf16ToUtf8", 1, dyn_text_utf16_to_utf8),
    JS_CFUNC_DEF("isValidUtf16", 1, dyn_text_is_valid_utf16),
    JS_CFUNC_DEF("countUtf16", 1, dyn_text_count_utf16),
};

static const JSCFunctionListEntry dyn_bh_proto[] = {
    JS_CGETSET_MAGIC_DEF("length", dyn_bh_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("isAscii", dyn_bh_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("isValidUtf8", dyn_bh_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("array", dyn_bh_get, NULL, 3),
    JS_CFUNC_DEF("slice", 0, dyn_bh_slice),
    JS_CFUNC_DEF("compare", 1, dyn_bh_compare),
    JS_CFUNC_DEF("equals", 1, dyn_bh_equals),
    JS_CFUNC_DEF("indexOf", 2, dyn_bh_index_of),
    JS_CFUNC_DEF("lastIndexOf", 2, dyn_bh_last_index_of),
    JS_CFUNC_DEF("includes", 1, dyn_bh_includes),
    JS_CFUNC_DEF("count", 2, dyn_bh_count),
    JS_CFUNC_DEF("indexOfAny", 2, dyn_bh_index_of_any),
    JS_CFUNC_DEF("startsWith", 2, dyn_bh_starts_with),
    JS_CFUNC_DEF("endsWith", 2, dyn_bh_ends_with),
    JS_CFUNC_DEF("toString", 0, dyn_bh_to_string),
};

static const JSCFunctionListEntry dyn_bytes_funcs[] = {
    JS_CFUNC_DEF("decode", 2, dyn_iconv_decode),
    JS_CFUNC_DEF("encode", 2, dyn_iconv_encode),
    JS_CFUNC_DEF("encodingExists", 1, dyn_iconv_exists),
    JS_CFUNC_DEF("encodings", 0, dyn_iconv_list),
    JS_CFUNC_DEF("compare", 2, dyn_bytes_compare),
    JS_CFUNC_DEF("equal", 2, dyn_bytes_equal),
    JS_CFUNC_DEF("indexOf", 3, dyn_bytes_index_of),
    JS_CFUNC_DEF("lastIndexOf", 3, dyn_bytes_last_index_of),
    JS_CFUNC_DEF("contains", 2, dyn_bytes_contains),
    JS_CFUNC_DEF("count", 3, dyn_bytes_count),
    JS_CFUNC_DEF("concat", 1, dyn_bytes_concat),
    JS_CFUNC_DEF("copy", 5, dyn_bytes_copy),

};

static int dyn_bytes_register(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(&dyn_bh_class_id);
    if (JS_NewClass(rt, dyn_bh_class_id, &dyn_bh_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_bh_proto, (int)countof(dyn_bh_proto));
    JS_SetClassProto(ctx, dyn_bh_class_id, proto);
    ctor = JS_NewCFunction2(ctx, dyn_bh_ctor, "Bytes", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, ctor, "alloc",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "alloc", 1, JS_CFUNC_generic_magic, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "isBytes",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "isBytes", 1, JS_CFUNC_generic_magic, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "concat",
        JS_NewCFunctionMagic(ctx, dyn_bh_static, "concat", 1, JS_CFUNC_generic_magic, 2), JS_PROP_C_W_E);
    if (JS_SetModuleExport(ctx, m, "Bytes", ctor) < 0)
        return -1;

    JS_NewClassID(&dyn_txt_class_id);
    if (JS_NewClass(rt, dyn_txt_class_id, &dyn_txt_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_txt_proto, (int)countof(dyn_txt_proto));
    JS_SetClassProto(ctx, dyn_txt_class_id, proto);
    ctor = JS_NewCFunction2(ctx, dyn_txt_ctor, "Text", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, "Text", ctor);
}

static int dyn_bytes_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_bytes_register(ctx, m) < 0)
        return -1;
    if (JS_SetModuleExportList(ctx, m, dyn_text_funcs,
            countof(dyn_text_funcs))
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_bytes_funcs,
        countof(dyn_bytes_funcs));
}

int js_nat_init_bytes(JSContext* ctx)
{
    JSModuleDef* m;
    simd_init();
    m = JS_NewCModule(ctx, "dyna:bytes", dyn_bytes_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Bytes") < 0 || JS_AddModuleExport(ctx, m, "Text") < 0)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_text_funcs,
            countof(dyn_text_funcs))
        < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_bytes_funcs,
        countof(dyn_bytes_funcs));
}

#endif
