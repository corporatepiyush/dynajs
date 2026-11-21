#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define JS_NewCFunctionMagic(...) (dyn_fiber_onjs(), JS_NewCFunctionMagic(__VA_ARGS__))
#define dyn_path_borrow(...) (dyn_fiber_onjs(), dyn_path_borrow(__VA_ARGS__))
#define DYN_ML_UNFITTED "model is not fitted; call fit() before serializing"

typedef struct {
    JSClassID* id;
    const dyn_mlk_codec_t* codec;
} dyn_ml_codec_t;

static const dyn_ml_codec_t dyn_ml_codecs[] = {
    { &dyn_linreg_class_id, &dyn_mlk_codecs[DYN_MLK_C_LINREG] },
    { &dyn_logreg_class_id, &dyn_mlk_codecs[DYN_MLK_C_LOGREG] },
    { &dyn_kmeans_class_id, &dyn_mlk_codecs[DYN_MLK_C_KMEANS] },
    { &dyn_pca_class_id, &dyn_mlk_codecs[DYN_MLK_C_PCA] },
    { &dyn_nb_class_id, &dyn_mlk_codecs[DYN_MLK_C_NB] },
    { &dyn_knn_clf_class_id, &dyn_mlk_codecs[DYN_MLK_C_KNN_CLF] },
    { &dyn_knn_reg_class_id, &dyn_mlk_codecs[DYN_MLK_C_KNN_REG] },
    { &dyn_dbscan_class_id, &dyn_mlk_codecs[DYN_MLK_C_DBSCAN] },
    { &dyn_stdscaler_class_id, &dyn_mlk_codecs[DYN_MLK_C_STDSCALER] },
    { &dyn_minmax_class_id, &dyn_mlk_codecs[DYN_MLK_C_MINMAX] },
    { &dyn_svm_class_id, &dyn_mlk_codecs[DYN_MLK_C_SVC] },
    { &dyn_gmm_class_id, &dyn_mlk_codecs[DYN_MLK_C_GMM] },
    { &dyn_dtc_class_id, &dyn_mlk_codecs[DYN_MLK_C_DTC] },
    { &dyn_dtr_class_id, &dyn_mlk_codecs[DYN_MLK_C_DTR] },
    { &dyn_rfc_class_id, &dyn_mlk_codecs[DYN_MLK_C_RFC] },
    { &dyn_rfr_class_id, &dyn_mlk_codecs[DYN_MLK_C_RFR] },
    { &dyn_gbr_class_id, &dyn_mlk_codecs[DYN_MLK_C_GBR] },
    { &dyn_gbc_class_id, &dyn_mlk_codecs[DYN_MLK_C_GBC] },
    { &dyn_xgbr_class_id, &dyn_mlk_codecs[DYN_MLK_C_XGBR] },
    { &dyn_xgbc_class_id, &dyn_mlk_codecs[DYN_MLK_C_XGBC] },
};

static JSValue dyn_ml_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const uint8_t stub = 0;
    JSValue ab, out;
    JSValueConst args[3];

    ab = JS_NewArrayBufferCopy(ctx, n ? p : &stub, n);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_ml_serialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const dyn_ml_codec_t* c = &dyn_ml_codecs[magic];
    void* native = dyn_res_native(ctx, this_val, *c->id);
    uint8_t* buf;
    size_t len;
    JSValue out;
    (void)argc;
    (void)argv;

    if (!native)
        return JS_EXCEPTION;
    if (!c->codec->is_fitted(native))
        return JS_ThrowTypeError(ctx, DYN_ML_UNFITTED);
    buf = dyn_mlk_encode(c->codec, native, &len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    out = dyn_ml_bytes(ctx, buf, len);
    free(buf);
    return out;
}

static JSValue dyn_ml_save(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const dyn_ml_codec_t* c = &dyn_ml_codecs[magic];
    const char* path;
    void* native;
    uint8_t* buf;
    size_t len;
    (void)argc;

    path = dyn_path_borrow(ctx, argv[0], "save(path)", NULL);
    if (!path)
        return JS_EXCEPTION;
    native = dyn_res_native(ctx, this_val, *c->id);
    if (!native) {
        return JS_EXCEPTION;
    }
    if (!c->codec->is_fitted(native)) {
        return JS_ThrowTypeError(ctx, DYN_ML_UNFITTED);
    }
    dyn_fiber_offload();
    buf = dyn_mlk_encode(c->codec, native, &len);
    if (!buf) {
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_io_write_whole_atomic(path, (const char*)buf, len, 1) < 0) {
        int e = errno;
        free(buf);
        return JS_ThrowTypeError(ctx, "save: cannot write '%s': %s", path, strerror(e));
    }
    free(buf);
    return JS_NewInt64(ctx, (int64_t)len);
}

static JSValue dyn_ml_from_record(JSContext* ctx, const dyn_ml_codec_t* c,
    const uint8_t* p, size_t len)
{
    void* native;
    const char* held;
    int de_rc;

    switch (dyn_mlk_decode(c->codec, p, len, &native, &de_rc, &held)) {
    case DYN_MLK_DEC_OK:
        return dyn_res_wrap(ctx, JS_UNDEFINED, *c->id, native, c->codec->dispose);
    case DYN_MLK_DEC_FORMAT:
        return JS_ThrowTypeError(ctx, "%s", dyn_de_strerror(de_rc));
    case DYN_MLK_DEC_WRONG_TYPE:
        if (held)
            return JS_ThrowTypeError(ctx,
                "record holds a %s, not a %s",
                held, c->codec->name);
        return JS_ThrowTypeError(ctx, "record is not a %s", c->codec->name);
    default:
        return JS_ThrowTypeError(ctx, "malformed %s record", c->codec->name);
    }
}

static JSValue dyn_ml_deserialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const dyn_ml_codec_t* c = &dyn_ml_codecs[magic];
    size_t off = 0, blen = 0, esz = 0, total = 0;
    JSValue ab;
    uint8_t* p;
    (void)this_val;
    (void)argc;

    ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &esz);
    if (!JS_IsException(ab)) {
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p)
            return JS_EXCEPTION;
        return dyn_ml_from_record(ctx, c, p + off, blen);
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    p = JS_GetArrayBuffer(ctx, &total, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx,
            "expected bytes (Uint8Array or ArrayBuffer)");
    }
    return dyn_ml_from_record(ctx, c, p, total);
}

static JSValue dyn_ml_load(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const dyn_ml_codec_t* c = &dyn_ml_codecs[magic];
    const char* path = dyn_path_borrow(ctx, argv[0], "load(path)", NULL);
    uint8_t* buf = NULL;
    size_t len = 0, got;
    long end;
    FILE* f;
    JSValue out;
    (void)this_val;
    (void)argc;

    if (!path)
        return JS_EXCEPTION;
    dyn_fiber_offload();
    f = fopen(path, "rb");
    if (!f) {
        out = JS_ThrowTypeError(ctx, "cannot open '%s'", path);
        return out;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        out = JS_ThrowTypeError(ctx, "cannot size '%s'", path);
        return out;
    }
    len = (size_t)end;
    if (len > DYN_SER_DEFAULT_MAX) {
        fclose(f);
        out = JS_ThrowRangeError(ctx, "'%s' is too large for a model record",
            path);
        return out;
    }
    buf = (uint8_t*)malloc(len ? len : 1);
    if (!buf) {
        fclose(f);
        return JS_ThrowOutOfMemory(ctx);
    }
    got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        out = JS_ThrowTypeError(ctx, "short read from '%s'", path);
        return out;
    }
    out = dyn_ml_from_record(ctx, c, buf, len);
    free(buf);
    return out;
}

DYN_ML_ASYNC_MAGIC(dyn_ml_save, DYN_ML_SHARED)

DYN_ML_ASYNC_MAGIC(dyn_ml_load, DYN_ML_SHARED)

int dyn_ml_install_persistence(JSContext* ctx)
{
    size_t i;
    for (i = 0; i < countof(dyn_ml_codecs); i++) {
        JSValue proto = JS_GetClassProto(ctx, *dyn_ml_codecs[i].id), ctor;
        if (JS_IsException(proto))
            return -1;
        JS_DefinePropertyValueStr(ctx, proto, "serialize",
            JS_NewCFunctionMagic(ctx, dyn_ml_serialize, "serialize", 0,
                JS_CFUNC_generic_magic, (int)i),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, proto, "save",
            JS_NewCFunctionMagic(ctx, dyn_ml_save_async, "save", 1,
                JS_CFUNC_generic_magic, (int)i),
            JS_PROP_C_W_E);
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsException(ctor))
            return -1;
        JS_DefinePropertyValueStr(ctx, ctor, "deserialize",
            JS_NewCFunctionMagic(ctx, dyn_ml_deserialize, "deserialize", 1,
                JS_CFUNC_generic_magic, (int)i),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctor, "load",
            JS_NewCFunctionMagic(ctx, dyn_ml_load_async, "load", 1,
                JS_CFUNC_generic_magic, (int)i),
            JS_PROP_C_W_E);
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}

#endif
