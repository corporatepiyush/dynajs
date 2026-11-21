#include "libc-wst.h"
#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)
#include "modules/stream/stream-internal.h"
#endif

typedef struct SStrat {
    JSValue obj;
    double hwm;
} SStrat;

typedef struct SWrap {
    JSValue obj;
    JSValue ts;
    STS* t;
    JSValue aux;
    JSValue opt;
    uint8_t pending[3];
    int npending;
    const char* what;
} SWrap;

static void strat_finalizer(JSRuntime* rt, JSValue val)
{
    SStrat* s = wst_opaque(val, WSTC_BLQS);
    if (!s)
        s = wst_opaque(val, WSTC_CQS);
    js_free_rt(rt, s);
}

static SWrap* wrap_opaque(JSValueConst v)
{
    int k;
    for (k = WSTC_TES; k <= WSTC_DS; k++) {
        SWrap* w = wst_opaque(v, k);
        if (w)
            return w;
    }
    return NULL;
}

static void wrap_finalizer(JSRuntime* rt, JSValue val)
{
    SWrap* w = wrap_opaque(val);
    if (!w)
        return;
    JS_FreeValueRT(rt, w->ts);
    JS_FreeValueRT(rt, w->aux);
    JS_FreeValueRT(rt, w->opt);
    js_free_rt(rt, w);
}

static void wrap_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SWrap* w = wrap_opaque(val);
    if (!w)
        return;
    JS_MarkValue(rt, w->ts, mf);
    JS_MarkValue(rt, w->aux, mf);
    JS_MarkValue(rt, w->opt, mf);
}

static JSValue byte_length_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return JS_GetPropertyStr(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "byteLength");
}

static JSValue count_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, 1);
}

static JSValue strat_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv, int k, const char* what)
{
    JSValue d, v, proto, obj;
    SStrat* s;
    double hwm;
    WST_NEED_NEW(ctx, nt);
    proto = wst_proto_from_target(ctx, nt);
    if (JS_IsException(proto))
        return proto;
    if (wst_to_dict(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, what, &d) < 0)
        goto fail;
    v = wst_dict_get(ctx, d, WSA_highWaterMark);
    JS_FreeValue(ctx, d);
    if (JS_IsException(v))
        goto fail;
    if (JS_IsUndefined(v)) {
        wst_throw(ctx, 0, "%s: highWaterMark is required", what);
        goto fail;
    }
    if (JS_ToFloat64(ctx, &hwm, v) < 0) {
        JS_FreeValue(ctx, v);
        goto fail;
    }
    JS_FreeValue(ctx, v);
    obj = wst_new_obj(ctx, k, proto, sizeof(SStrat), (void**)&s);
    JS_FreeValue(ctx, proto);
    if (!JS_IsException(obj))
        s->hwm = hwm;
    return obj;
fail:
    JS_FreeValue(ctx, proto);
    return JS_EXCEPTION;
}

static JSValue blqs_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return strat_ctor(ctx, nt, argc, argv, WSTC_BLQS, "ByteLengthQueuingStrategy");
}

static JSValue cqs_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return strat_ctor(ctx, nt, argc, argv, WSTC_CQS, "CountQueuingStrategy");
}

static const char* const strat_names[2] = { "ByteLengthQueuingStrategy", "CountQueuingStrategy" };

static JSValue strat_get_hwm(JSContext* ctx, JSValueConst this_val, int magic)
{
    SStrat* s = wst_opaque(this_val, magic ? WSTC_CQS : WSTC_BLQS);
    if (!s)
        return wst_throw(ctx, 0, "this is not a %s", strat_names[magic]);
    return JS_NewFloat64(ctx, s->hwm);
}

static JSValue strat_get_size(JSContext* ctx, JSValueConst this_val, int magic)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    if (!wst_opaque(this_val, magic ? WSTC_CQS : WSTC_BLQS))
        return wst_throw(ctx, 0, "this is not a %s", strat_names[magic]);
    return JS_DupValue(ctx, magic ? w->count_size : w->blen_size);
}

int wst_buffer_source_bytes(JSContext* ctx, JSValueConst v, const char* what, JSValue* out)
{
    WVInfo vi;
    int r;
    if (wst_is_ab(v)) {
        if (wst_is_detached(v)) {
            wst_throw(ctx, 0, "%s is detached", what);
            return -1;
        }
        *out = wst_new_u8(ctx, v, 0, wst_ab_len(ctx, v));
        return JS_IsException(*out) ? -1 : 0;
    }
    r = wst_view_info(ctx, v, &vi);
    if (r < 0)
        return -1;
    if (r == 0) {
        wst_throw(ctx, 0, "%s must be an ArrayBuffer or ArrayBufferView", what);
        return -1;
    }
    if (wst_is_ab(vi.buffer) && wst_is_detached(vi.buffer)) {
        JS_FreeValue(ctx, vi.buffer);
        wst_throw(ctx, 0, "%s is detached", what);
        return -1;
    }
    *out = wst_new_u8(ctx, vi.buffer, vi.off, vi.len);
    JS_FreeValue(ctx, vi.buffer);
    return JS_IsException(*out) ? -1 : 0;
}

static int enqueue_bytes(JSContext* ctx, SWrap* w, const uint8_t* p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), u8;
    int r;
    if (JS_IsException(ab))
        return -1;
    u8 = wst_new_u8(ctx, ab, 0, (double)n);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8))
        return -1;
    r = wst_tsdc_enqueue(ctx, w->t->c, u8);
    JS_FreeValue(ctx, u8);
    return r;
}

static int is_sur(const uint8_t* p, size_t i, size_t n, int hi)
{
    return i + 2 < n && p[i] == 0xED && (hi ? (p[i + 1] >= 0xA0 && p[i + 1] <= 0xAF) : (p[i + 1] >= 0xB0 && p[i + 1] <= 0xBF));
}

static JSValue tes_transform(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SWrap* w = wst_opaque(data, WSTC_TES);
    const char* cs;
    size_t len, n, i, o = 0;
    uint8_t *buf, *out;
    int r = 0;

    cs = JS_ToCStringLen2(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED, 0);
    if (!cs)
        return wst_reject_exc(ctx);
    n = len + (size_t)w->npending;
    buf = js_malloc(ctx, n + 1);
    out = js_malloc(ctx, n + 4);
    if (!buf || !out) {
        js_free(ctx, buf);
        js_free(ctx, out);
        JS_FreeCString(ctx, cs);
        return wst_reject_exc(ctx);
    }
    memcpy(buf, w->pending, (size_t)w->npending);
    memcpy(buf + w->npending, cs, len);
    JS_FreeCString(ctx, cs);
    w->npending = 0;
    for (i = 0; i < n;) {
        if (is_sur(buf, i, n, 1)) {
            if (is_sur(buf, i + 3, n, 0)) {
                uint32_t hi = ((uint32_t)(buf[i] & 0x0F) << 12) | ((uint32_t)(buf[i + 1] & 0x3F) << 6) | (buf[i + 2] & 0x3F);
                uint32_t lo = ((uint32_t)(buf[i + 3] & 0x0F) << 12) | ((uint32_t)(buf[i + 4] & 0x3F) << 6) | (buf[i + 5] & 0x3F);
                uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                out[o++] = (uint8_t)(0xF0 | (cp >> 18));
                out[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
                i += 6;
            } else if (i + 3 == n) {
                memcpy(w->pending, buf + i, 3);
                w->npending = 3;
                i += 3;
            } else {
                out[o++] = 0xEF;
                out[o++] = 0xBF;
                out[o++] = 0xBD;
                i += 3;
            }
        } else if (is_sur(buf, i, n, 0)) {
            out[o++] = 0xEF;
            out[o++] = 0xBF;
            out[o++] = 0xBD;
            i += 3;
        } else {
            out[o++] = buf[i++];
        }
    }
    if (o)
        r = enqueue_bytes(ctx, w, out, o);
    js_free(ctx, buf);
    js_free(ctx, out);
    if (r < 0)
        return wst_reject_exc(ctx);
    return wst_resolved_undef(ctx);
}

static JSValue tes_flush(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    static const uint8_t fffd[3] = { 0xEF, 0xBF, 0xBD };
    SWrap* w = wst_opaque(data, WSTC_TES);
    (void)argc;
    (void)argv;
    if (w->npending) {
        w->npending = 0;
        if (enqueue_bytes(ctx, w, fffd, 3) < 0)
            return wst_reject_exc(ctx);
    }
    return wst_resolved_undef(ctx);
}

static JSValue wrap_new(JSContext* ctx, int k, JSValueConst nt, SWrap** out)
{
    JSValue proto = wst_proto_from_target(ctx, nt), obj;
    SWrap* w;
    if (JS_IsException(proto))
        return proto;
    obj = wst_new_obj(ctx, k, proto, sizeof(SWrap), (void**)&w);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    w->ts = JS_UNDEFINED;
    w->aux = JS_UNDEFINED;
    w->opt = JS_UNDEFINED;
    *out = w;
    return obj;
}

static int wrap_ts(JSContext* ctx, SWrap* w, SAlgoC* transform, SAlgoC* flush, SAlgoC* cancel)
{
    SAlgo a, b, c;
    sa_init(&a);
    sa_init(&b);
    sa_init(&c);
    sa_set_c(ctx, &a, transform, w->obj);
    sa_set_c(ctx, &b, flush, w->obj);
    if (cancel)
        sa_set_c(ctx, &c, cancel, w->obj);
    w->ts = wst_ts_create(ctx, &a, &b, &c, &w->t);
    sa_clear(ctx, &a);
    sa_clear(ctx, &b);
    sa_clear(ctx, &c);
    if (JS_IsException(w->ts)) {
        w->ts = JS_UNDEFINED;
        return -1;
    }
    return 0;
}

static JSValue tes_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    SWrap* w;
    JSValue obj;
    (void)argc;
    (void)argv;
    WST_NEED_NEW(ctx, nt);
    obj = wrap_new(ctx, WSTC_TES, nt, &w);
    if (JS_IsException(obj))
        return obj;
    if (wrap_ts(ctx, w, tes_transform, tes_flush, NULL) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue type_error_of(JSContext* ctx, JSValue e)
{
    WstRt* rt = wst_rt(ctx);
    JSValue m, s, r;
    const char* cs;
    if (JS_IsInstanceOf(ctx, e, rt->type_error_ctor) > 0)
        return e;
    m = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "message") : JS_UNDEFINED;
    if (JS_IsException(m))
        m = JS_UNDEFINED;
    s = JS_ToString(ctx, (JS_IsUndefined(m) || !JS_ToBool(ctx, m)) ? e : m);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, e);
    if (JS_IsException(s))
        return JS_GetException(ctx);
    cs = JS_ToCString(ctx, s);
    JS_FreeValue(ctx, s);
    if (!cs)
        return JS_EXCEPTION;
    r = wst_err(ctx, 0, "%s", cs);
    JS_FreeCString(ctx, cs);
    return r;
}

static JSValue reject_type_error(JSContext* ctx)
{
    JSValue e, p;
    if (JS_HasUncatchableException(ctx))
        return JS_EXCEPTION;
    e = type_error_of(ctx, JS_GetException(ctx));
    if (JS_IsException(e))
        return wst_reject_exc(ctx);
    p = wst_rejected(ctx, e);
    JS_FreeValue(ctx, e);
    return p;
}

static int enqueue_if_nonempty(JSContext* ctx, SWrap* w, JSValueConst out)
{
    JSValue l;
    int64_t n = 0;
    if (JS_IsUndefined(out))
        return 0;
    l = JS_GetPropertyStr(ctx, out, "length");
    if (JS_IsException(l))
        return -1;
    n = JS_ToBool(ctx, l);
    JS_FreeValue(ctx, l);
    if (!n)
        return 0;
    return wst_tsdc_enqueue(ctx, w->t->c, out);
}

static JSValue tds_transform(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SWrap* w = wst_opaque(data, WSTC_TDS);
    WstRt* rt = wst_rt(ctx);
    if (!rt)
        return JS_EXCEPTION;
    JSValue bytes, out, args[2];
    int r;
    if (wst_buffer_source_bytes(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "TextDecoderStream chunk", &bytes) < 0)
        return reject_type_error(ctx);
    args[0] = bytes;
    args[1] = w->opt;
    out = JS_Call(ctx, rt->dec_decode, w->aux, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, bytes);
    if (JS_IsException(out))
        return reject_type_error(ctx);
    r = enqueue_if_nonempty(ctx, w, out);
    JS_FreeValue(ctx, out);
    if (r < 0)
        return reject_type_error(ctx);
    return wst_resolved_undef(ctx);
}

static JSValue tds_flush(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SWrap* w = wst_opaque(data, WSTC_TDS);
    WstRt* rt = wst_rt(ctx);
    if (!rt)
        return JS_EXCEPTION;
    JSValue out;
    int r;
    (void)argc;
    (void)argv;
    out = JS_Call(ctx, rt->dec_decode, w->aux, 0, NULL);
    if (JS_IsException(out))
        return reject_type_error(ctx);
    r = enqueue_if_nonempty(ctx, w, out);
    JS_FreeValue(ctx, out);
    if (r < 0)
        return reject_type_error(ctx);
    return wst_resolved_undef(ctx);
}

static JSValue tds_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    WstRt* rt = wst_rt(ctx);
    if (!rt)
        return JS_EXCEPTION;
    JSValue args[2], obj;
    SWrap* w;
    WST_NEED_NEW(ctx, nt);
    obj = wrap_new(ctx, WSTC_TDS, nt, &w);
    if (JS_IsException(obj))
        return obj;
    args[0] = (argc > 0 && !JS_IsUndefined(argv[0])) ? JS_DupValue(ctx, argv[0]) : JS_NewString(ctx, "utf-8");
    args[1] = argc > 1 ? argv[1] : JS_UNDEFINED;
    w->aux = JS_CallConstructor(ctx, rt->dec_ctor, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    if (JS_IsException(w->aux)) {
        w->aux = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    w->opt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, w->opt, "stream", JS_TRUE);
    if (wrap_ts(ctx, w, tds_transform, tds_flush, NULL) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)
typedef struct {
    JSContext* ctx;
    SWrap* w;
} CodecEmit;

static int codec_emit(void* ud, uint8_t* buf, size_t len)
{
    CodecEmit* ce = (CodecEmit*)ud;
    JSValue ab = dyn_nat_ab_take(ce->ctx, buf, len), u8;
    int r;
    if (JS_IsException(ab))
        return -1;
    u8 = wst_new_u8(ce->ctx, ab, 0, len);
    JS_FreeValue(ce->ctx, ab);
    if (JS_IsException(u8))
        return -1;
    r = wst_tsdc_enqueue(ce->ctx, ce->w->t->c, u8);
    JS_FreeValue(ce->ctx, u8);
    return r;
}

static JSValue codec_step(JSContext* ctx, SWrap* w, const uint8_t* p, size_t n, int end)
{
    CodecEmit ce;
    const char* err;
    JSValue e, pr;
    ce.ctx = ctx;
    ce.w = w;
    err = dyn_push_codec_step(w->aux, p, n, end, codec_emit, &ce);
    if (!err)
        return wst_resolved_undef(ctx);
    if (err == dyn_push_emit_failed)
        return wst_reject_exc(ctx);
    e = wst_err(ctx, 0, "%s: %s", w->what, err);
    pr = wst_rejected(ctx, e);
    JS_FreeValue(ctx, e);
    return pr;
}

static JSValue codec_transform(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SWrap* w = wrap_opaque(data);
    char what[64];
    JSValue bytes, r;
    uint8_t* p;
    size_t n = 0;
    snprintf(what, sizeof(what), "%s chunk", w->what);
    if (wst_buffer_source_bytes(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, what, &bytes) < 0)
        return wst_reject_exc(ctx);
    p = stream_view_bytes(ctx, bytes, &n, 1);
    if (!p && JS_HasException(ctx)) {
        JS_FreeValue(ctx, bytes);
        return wst_reject_exc(ctx);
    }
    r = codec_step(ctx, w, p, n, 0);
    JS_FreeValue(ctx, bytes);
    return r;
}

static JSValue codec_flush(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return codec_step(ctx, wrap_opaque(data), NULL, 0, 1);
}
#else
static JSValue codec_transform(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)data;
    (void)argc;
    (void)argv;
    return wst_resolved_undef(ctx);
}

static JSValue codec_flush(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)data;
    (void)argc;
    (void)argv;
    return wst_resolved_undef(ctx);
}
#endif

static JSValue codec_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SWrap* w = wrap_opaque(data);
    WstRt* rt = wst_rt(ctx);
    JSValue r = rt ? JS_Call(ctx, rt->pc_close, w->aux, 0, NULL) : JS_EXCEPTION;
    (void)argc;
    (void)argv;
    if (JS_IsException(r))
        wst_drop_exc(ctx);
    JS_FreeValue(ctx, r);
    return wst_resolved_undef(ctx);
}

static JSValue codec_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv, int k, int encode, const char* what)
{
    WstRt* rt = wst_rt(ctx);
    if (!rt)
        return JS_EXCEPTION;
    JSValue obj, args[2];
    const char *f, *codec = NULL;
    SWrap* w;
    WST_NEED_NEW(ctx, nt);
    f = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!f)
        return JS_EXCEPTION;
    if (!strcmp(f, "deflate"))
        codec = "zlib";
    else if (!strcmp(f, "deflate-raw"))
        codec = "deflate";
    else if (!strcmp(f, "gzip"))
        codec = "gzip";
    else if (!strcmp(f, "brotli"))
        codec = "brotli";
#if defined(CONFIG_ZSTD)
    else if (!strcmp(f, "zstd"))
        codec = "zstd";
#endif
    else if (!strcmp(f, "lz4"))
        codec = "lz4";
    if (!codec) {
        obj = wst_throw(ctx, 0, "%s: unsupported format \"%s\" (deflate, deflate-raw, gzip, brotli, zstd, lz4)", what, f);
        JS_FreeCString(ctx, f);
        return obj;
    }
    JS_FreeCString(ctx, f);
    if (!JS_IsFunction(ctx, rt->pc_ctor))
        return wst_throw(ctx, 0, "%s is unavailable in this build (no native codec)", what);
    obj = wrap_new(ctx, k, nt, &w);
    if (JS_IsException(obj))
        return obj;
    w->what = what;
    args[0] = JS_NewString(ctx, codec);
    args[1] = JS_NewBool(ctx, encode);
    w->aux = JS_CallConstructor(ctx, rt->pc_ctor, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    if (JS_IsException(w->aux)) {
        w->aux = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (wrap_ts(ctx, w, codec_transform, codec_flush, codec_cancel) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue cs_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return codec_ctor(ctx, nt, argc, argv, WSTC_CS, 1, "CompressionStream");
}

static JSValue ds_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return codec_ctor(ctx, nt, argc, argv, WSTC_DS, 0, "DecompressionStream");
}

static const char* const wrap_names[4] = { "TextEncoderStream", "TextDecoderStream", "CompressionStream", "DecompressionStream" };

static JSValue wrap_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    int k = magic >> 4, f = magic & 15;
    SWrap* w = wst_opaque(this_val, k);
    if (!w)
        return wst_throw(ctx, 0, "this is not a %s", wrap_names[k - WSTC_TES]);
    switch (f) {
    case 0:
        return JS_DupValue(ctx, w->t->readable);
    case 1:
        return JS_DupValue(ctx, w->t->writable);
    case 2:
        return k == WSTC_TES ? JS_NewString(ctx, "utf-8") : JS_GetPropertyStr(ctx, w->aux, "encoding");
    case 3:
        return JS_GetPropertyStr(ctx, w->aux, "fatal");
    default:
        return JS_GetPropertyStr(ctx, w->aux, "ignoreBOM");
    }
}

#define WRAP_GET(name, k, f) { name, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, ((k) << 4) | (f), .u = { .getset = { .get = { .getter_magic = wrap_get }, .set = { .setter_magic = NULL } } } }
#define STRAT_GET(name, fn, m) { name, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, m, .u = { .getset = { .get = { .getter_magic = fn }, .set = { .setter_magic = NULL } } } }

static const JSCFunctionListEntry blqs_funcs[] = {
    STRAT_GET("highWaterMark", strat_get_hwm, 0),
    STRAT_GET("size", strat_get_size, 0),
    WST_TAG("ByteLengthQueuingStrategy"),
};

static const JSCFunctionListEntry cqs_funcs[] = {
    STRAT_GET("highWaterMark", strat_get_hwm, 1),
    STRAT_GET("size", strat_get_size, 1),
    WST_TAG("CountQueuingStrategy"),
};

static const JSCFunctionListEntry tes_funcs[] = {
    WRAP_GET("encoding", WSTC_TES, 2),
    WRAP_GET("readable", WSTC_TES, 0),
    WRAP_GET("writable", WSTC_TES, 1),
    WST_TAG("TextEncoderStream"),
};

static const JSCFunctionListEntry tds_funcs[] = {
    WRAP_GET("encoding", WSTC_TDS, 2),
    WRAP_GET("fatal", WSTC_TDS, 3),
    WRAP_GET("ignoreBOM", WSTC_TDS, 4),
    WRAP_GET("readable", WSTC_TDS, 0),
    WRAP_GET("writable", WSTC_TDS, 1),
    WST_TAG("TextDecoderStream"),
};

static const JSCFunctionListEntry cs_funcs[] = {
    WRAP_GET("readable", WSTC_CS, 0),
    WRAP_GET("writable", WSTC_CS, 1),
    WST_TAG("CompressionStream"),
};

static const JSCFunctionListEntry ds_funcs[] = {
    WRAP_GET("readable", WSTC_DS, 0),
    WRAP_GET("writable", WSTC_DS, 1),
    WST_TAG("DecompressionStream"),
};

int wst_init_misc(JSContext* ctx, JSValue* ctors)
{
    static const WstClassSpec specs[] = {
        { WSTC_BLQS, "ByteLengthQueuingStrategy", blqs_ctor, 1, blqs_funcs, countof(blqs_funcs), NULL, 0, strat_finalizer, NULL },
        { WSTC_CQS, "CountQueuingStrategy", cqs_ctor, 1, cqs_funcs, countof(cqs_funcs), NULL, 0, strat_finalizer, NULL },
        { WSTC_TES, "TextEncoderStream", tes_ctor, 0, tes_funcs, countof(tes_funcs), NULL, 0, wrap_finalizer, wrap_mark },
        { WSTC_TDS, "TextDecoderStream", tds_ctor, 0, tds_funcs, countof(tds_funcs), NULL, 0, wrap_finalizer, wrap_mark },
        { WSTC_CS, "CompressionStream", cs_ctor, 1, cs_funcs, countof(cs_funcs), NULL, 0, wrap_finalizer, wrap_mark },
        { WSTC_DS, "DecompressionStream", ds_ctor, 1, ds_funcs, countof(ds_funcs), NULL, 0, wrap_finalizer, wrap_mark },
    };
    WstRt* w = wst_rt(ctx);
    if (!w)
        return -1;
    JSValue g, p, nat;
    size_t i;
    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &ctors[specs[i].k]) < 0)
            return -1;
    if (wst_slot(ctx, &w->blen_size, JS_NewCFunction(ctx, byte_length_size, "size", 1)) < 0
        || wst_slot(ctx, &w->count_size, JS_NewCFunction(ctx, count_size, "size", 0)) < 0)
        return -1;
    g = JS_GetGlobalObject(ctx);
    if (wst_slot(ctx, &w->dec_ctor, JS_GetPropertyStr(ctx, g, "TextDecoder")) < 0
        || wst_slot(ctx, &w->type_error_ctor, JS_GetPropertyStr(ctx, g, "TypeError")) < 0) {
        JS_FreeValue(ctx, g);
        return -1;
    }
    JS_FreeValue(ctx, g);
    if (JS_IsFunction(ctx, w->dec_ctor)) {
        int r;
        p = JS_GetPropertyStr(ctx, w->dec_ctor, "prototype");
        r = JS_IsException(p) ? -1 : wst_slot(ctx, &w->dec_decode, JS_GetPropertyStr(ctx, p, "decode"));
        JS_FreeValue(ctx, p);
        if (r < 0)
            return -1;
    }
#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)
    nat = JS_NewObject(ctx);
    p = JS_UNDEFINED;
    if (JS_IsException(nat) || dyn_stream_web_natives(ctx, nat) < 0
        || wst_slot(ctx, &w->pc_ctor, JS_GetPropertyStr(ctx, nat, "PushCodec")) < 0
        || JS_IsException(p = JS_GetPropertyStr(ctx, w->pc_ctor, "prototype"))
        || wst_slot(ctx, &w->pc_close, JS_GetPropertyStr(ctx, p, "close")) < 0) {
        wst_slot(ctx, &w->pc_ctor, JS_UNDEFINED);
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, p);
            JS_FreeValue(ctx, nat);
            return -1;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, nat);
#else
    (void)nat;
#endif
    return 0;
}
