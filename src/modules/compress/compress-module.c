#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

int dyn_read_input(JSContext* ctx, JSValueConst val, uint8_t** pout,
    size_t* plen)
{
    uint8_t *base, *copy;
    size_t off = 0, tlen = 0, ab = 0;
    JSValue buf;

    if (JS_IsString(val)) {
        size_t slen;
        const char* str = JS_ToCStringLen(ctx, &slen, val);
        if (!str)
            return -1;
        copy = (uint8_t*)malloc(slen ? slen : 1);
        if (!copy) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (slen)
            memcpy(copy, str, slen);
        JS_FreeCString(ctx, str);
        *pout = copy;
        *plen = slen;
        return 0;
    }

    buf = JS_GetBufferKind(val) == JS_BUFFER_KIND_VIEW ? JS_GetTypedArrayBuffer(ctx, val, &off, &tlen, NULL) : JS_EXCEPTION;
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        if (!base) {
            JSValue ex = JS_GetException(ctx);
            JS_FreeValue(ctx, ex);
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx,
                "dyna:compress: input buffer is detached or invalid");
            return -1;
        }
        if (off > ab)
            off = ab;
        if (tlen > ab - off)
            tlen = ab - off;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, val);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "dyna:compress: input must be a string, "
                                   "Uint8Array, or ArrayBuffer");
            return -1;
        }
        buf = JS_UNDEFINED;
        off = 0;
        tlen = ab;
    }

    copy = (uint8_t*)malloc(tlen ? tlen : 1);
    if (!copy) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (tlen)
        memcpy(copy, base + off, tlen);
    JS_FreeValue(ctx, buf);
    *pout = copy;
    *plen = tlen;
    return 0;
}

JSValue dyn_bytes_to_uint8(JSContext* ctx, const uint8_t* data, size_t len)
{
    static const uint8_t empty = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &empty, len);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

const char* const cpr_level_keys[] = { "level" };

const char* const cpr_asstring_keys[] = { "asString", "maxOutputBytes" };

static const char* const cpr_algo_keys[] = { "algo" };

int dyn_cpr_maxout(JSContext* ctx, JSValueConst opts, dyn_outbuf_t* o)
{
    JSValue v;
    int64_t n;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "maxOutputBytes");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_ToInt64(ctx, &n, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (n < 1 || (uint64_t)n > (uint64_t)DYN_MAX_OUTPUT) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx,
                "maxOutputBytes must be an integer in [1, %llu]",
                (unsigned long long)DYN_MAX_OUTPUT);
            return -1;
        }
        o->limit = (size_t)n;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue dyn_gzip(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;
    int level = 6;
    int rc;

    (void)this_val;

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSValueConst lv_arg = argv[1];
        JSValue prop;
        int32_t lv;
        if (JS_IsObject(argv[1])) {
            if (dyn_opts_strict(ctx, argv[1], cpr_level_keys, 1))
                return JS_EXCEPTION;
            prop = JS_GetPropertyStr(ctx, argv[1], "level");
            if (JS_IsException(prop))
                return JS_EXCEPTION;
            lv_arg = prop;
        } else {
            prop = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(lv_arg)) {
            if (!JS_IsNumber(lv_arg)) {
                JS_FreeValue(ctx, prop);
                return JS_ThrowTypeError(ctx, "gzip: level must be a number");
            }
            if (JS_ToInt32(ctx, &lv, lv_arg)) {
                JS_FreeValue(ctx, prop);
                return JS_EXCEPTION;
            }
            level = lv;
        }
        JS_FreeValue(ctx, prop);
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    rc = DYN_OFFLOAD_CPU(dyn_gzip_build_ctx(src, src_len, level, NULL, &out, &out_len));
    if (rc < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_gunzip(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_gunzip_decode(src, src_len, &o)) < 0) {
        free(src);
        free(o.buf);
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "dyna:compress gunzip");
        return JS_ThrowTypeError(ctx, "dyna:compress gunzip: invalid gzip data");
    }
    free(src);

    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

int dyn_out_grow(dyn_outbuf_t* o, size_t need, size_t cap)
{
    size_t nc;
    uint8_t* nb;
    if (need > cap) {
        o->exceeded = 1;
        return -1;
    }
    if (need <= o->cap)
        return 0;
    nc = o->cap ? o->cap * 2 : 65536;
    if (nc > cap)
        nc = cap;
    if (nc < need)
        nc = need;
    nb = (uint8_t*)realloc(o->buf, nc);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = nc;
    return 0;
}

static JSValue dyn_unbrotli_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_BROTLI
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    const char* why = NULL;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_brotli_decode(src, src_len, dyn_cpr_cap(&o), &o, &why)) < 0) {
        free(src);
        free(o.buf);
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "dyna:compress unbrotli");
        return JS_ThrowTypeError(ctx, "dyna:compress unbrotli: %s",
            why ? why : "invalid brotli data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress brotli: support not compiled in");
#endif
}

static JSValue dyn_snappy_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;

    (void)this_val;
    (void)argc;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_snappy_compress(src, src_len, &out, &out_len)) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_unsnappy_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_snappy_decompress(src, src_len, dyn_cpr_cap(&o), &o)) < 0) {
        free(src);
        free(o.buf);
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "dyna:compress unsnappy");
        return JS_ThrowTypeError(ctx,
            "dyna:compress unsnappy: invalid snappy data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

JSClassID dyn_compressor_class_id;

static const JSClassDef dyn_compressor_class = {
    "Compressor", .finalizer = dyn_res_finalizer
};

static const JSCFunctionListEntry dyn_compressor_proto[] = {
    JS_CFUNC_DEF("compress", 1, dyn_compressor_compress),
    JS_CFUNC_DEF("decompress", 1, dyn_compressor_decompress),
    JS_CGETSET_DEF("algo", dyn_compressor_get_algo, NULL),
    JS_CGETSET_DEF("dictId", dyn_compressor_get_dictid, NULL),
};

JSClassID dyn_jsdict_class_id;

static const JSClassDef dyn_jsdict_class = {
    "Dictionary",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_jsdict_proto[] = {
    JS_CFUNC_MAGIC_DEF("compress", 1, dyn_jsdict_run, 0),
    JS_CFUNC_MAGIC_DEF("decompress", 1, dyn_jsdict_run, 1),
    JS_CGETSET_DEF("id", dyn_jsdict_get_id, NULL),
    JS_CGETSET_DEF("size", dyn_jsdict_get_size, NULL),
};

static JSValue dyn_compress_bound_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* algo = NULL;
    int64_t n64;
    size_t n;
    JSValue av;
    (void)this_val;

    if (argc < 1 || JS_ToInt64(ctx, &n64, argv[0]))
        return JS_ThrowTypeError(ctx, "compressBound(n, { algo }): n required");
    if (n64 < 0)
        return JS_ThrowRangeError(ctx, "compressBound: n must be >= 0");
    if (n64 > (int64_t)(SIZE_MAX >> 9))
        return JS_ThrowRangeError(ctx, "compressBound: n out of range");
    n = (size_t)n64;

    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "compressBound(n, { algo }): algo must be \"gzip\", \"lz4\", "
            "\"lz4frame\", \"zstd\", \"brotli\" or \"snappy\"");
    if (dyn_opts_strict(ctx, argv[1], cpr_algo_keys, 1))
        return JS_EXCEPTION;
    av = JS_GetPropertyStr(ctx, argv[1], "algo");
    if (JS_IsException(av))
        return JS_EXCEPTION;
    algo = JS_ToCString(ctx, av);
    JS_FreeValue(ctx, av);
    if (!algo)
        return JS_EXCEPTION;

    if (!strcmp(algo, "gzip")) {
        JS_FreeCString(ctx, algo);
        {
            size_t nblocks = n ? (n + 65534) / 65535 : 1;
            size_t stored = n ? nblocks * 5 + n : 5;
            if (stored > SIZE_MAX - 18)
                return JS_ThrowRangeError(ctx, "compressBound: n out of range");
            return JS_NewInt64(ctx, (int64_t)(18 + stored));
        }
    }
    if (!strcmp(algo, "lz4")) {
        JS_FreeCString(ctx, algo);
        return JS_NewInt64(ctx, (int64_t)(n + n / 255 + 16));
    }
    if (!strcmp(algo, "lz4frame")) {
        JS_FreeCString(ctx, algo);
        {
            size_t nblk = n ? (n + 4194304 - 1) / 4194304 : 1;
            size_t total = 7 + nblk * 4 + (n + n / 255 + 16) + 8;
            if (total > SIZE_MAX - 4)
                return JS_ThrowRangeError(ctx, "compressBound: n out of range");
            return JS_NewInt64(ctx, (int64_t)(total + 4));
        }
    }
    if (!strcmp(algo, "zstd")) {
        JS_FreeCString(ctx, algo);
#if DYN_HAVE_ZSTD
        return JS_NewInt64(ctx, (int64_t)ZSTD_compressBound(n));
#else
        return JS_ThrowTypeError(ctx, "dyna:compress zstd: support not compiled in");
#endif
    }
    if (!strcmp(algo, "brotli")) {
        JS_FreeCString(ctx, algo);
#if defined(__APPLE__)
        return JS_NewInt64(ctx, (int64_t)(n ? n + 2 + 4 * (n >> 14) + 4 : 2));
#elif DYN_HAVE_BROTLI
        return JS_NewInt64(ctx, (int64_t)BrotliEncoderMaxCompressedSize(n));
#else
        return JS_ThrowTypeError(ctx, "dyna:compress brotli: support not compiled in");
#endif
    }
    if (!strcmp(algo, "snappy")) {
        JS_FreeCString(ctx, algo);
        return JS_NewInt64(ctx, (int64_t)(n + n / 50 + 16));
    }
    JS_FreeCString(ctx, algo);
    return JS_ThrowTypeError(ctx,
        "compressBound(n, { algo }): algo must be \"gzip\", \"lz4\", "
        "\"lz4frame\", \"zstd\", \"brotli\" or \"snappy\"");
}

static const JSCFunctionListEntry dyn_compress_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, dyn_gzip),
    JS_CFUNC_DEF("gunzip", 1, dyn_gunzip),
    JS_CFUNC_DEF("compressBound", 2, dyn_compress_bound_js),
    JS_CFUNC_DEF("lz4Compress", 1, dyn_lz4_compress_js),
    JS_CFUNC_DEF("lz4Decompress", 2, dyn_lz4_decompress_js),
    JS_CFUNC_DEF("lz4Frame", 1, dyn_lz4_frame_js),
    JS_CFUNC_DEF("lz4Unframe", 1, dyn_lz4_unframe_js),
    JS_CFUNC_DEF("zstd", 1, dyn_zstd_js),
    JS_CFUNC_DEF("unzstd", 1, dyn_unzstd_js),
    JS_CFUNC_DEF("brotli", 1, dyn_brotli_js),
    JS_CFUNC_DEF("unbrotli", 1, dyn_unbrotli_js),
    JS_CFUNC_DEF("snappy", 1, dyn_snappy_js),
    JS_CFUNC_DEF("unsnappy", 1, dyn_unsnappy_js),
    JS_CFUNC_MAGIC_DEF("TarList", 1, dyn_tar_read, 0),
    JS_CFUNC_MAGIC_DEF("TarExtract", 1, dyn_tar_read, 1),
    JS_CFUNC_DEF("TarPack", 1, dyn_tar_pack),
    JS_CFUNC_MAGIC_DEF("ZipList", 1, dyn_zip_read, 0),
    JS_CFUNC_MAGIC_DEF("ZipRead", 2, dyn_zip_read, 1),
    JS_CFUNC_DEF("ZipPack", 1, dyn_zip_pack),
    JS_CFUNC_DEF("ZipReadAt", 2, dyn_zip_read_at),
    JS_CFUNC_DEF("ZipExtractAll", 1, dyn_zip_extract_all),
    JS_CFUNC_DEF("deflate", 1, dyn_deflate_js),
    JS_CFUNC_DEF("inflate", 1, dyn_inflate_js),
};

static int dyn_compress_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_compress_funcs,
            countof(dyn_compress_funcs))
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_jsdict_class_id, &dyn_jsdict_class,
            dyn_jsdict_proto, (int)countof(dyn_jsdict_proto),
            dyn_jsdict_ctor, "Dictionary")
        < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_compressor_class_id,
        &dyn_compressor_class, dyn_compressor_proto,
        countof(dyn_compressor_proto),
        dyn_compressor_ctor, "Compressor");
}

int js_nat_init_compress(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:compress", dyn_compress_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_compress_funcs,
            countof(dyn_compress_funcs))
        < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Dictionary") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "Compressor");
}

#endif
