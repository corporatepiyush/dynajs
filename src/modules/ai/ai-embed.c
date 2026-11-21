#include "ai-internal.h"
#include "core/dyn-codec.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static JSValue ai_emb_bad(JSContext* ctx, uint32_t i, const char* why, int status)
{
    char msg[160];
    snprintf(msg, sizeof msg, "malformed embedding at data[%u]: %s", i, why);
    return ai_error_throw(ctx, msg, status, "api_error");
}

static JSValue ai_emb_f32_new(JSContext* ctx, const float* p, size_t n)
{
    JSValue args[3], ta;
    args[0] = JS_NewArrayBufferCopy(ctx, (const uint8_t*)p, n * sizeof(float));
    if (JS_IsException(args[0]))
        return args[0];
    args[1] = JS_NewInt32(ctx, 0);
    args[2] = JS_NewInt64(ctx, (int64_t)n);
    ta = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, args[0]);
    return ta;
}

static JSValue ai_emb_from_b64(JSContext* ctx, JSValueConst s, uint32_t i, int status)
{
    size_t n, got, k;
    const char* p = JS_ToCStringLen(ctx, &n, s);
    uint8_t* raw;
    float* f;
    JSValue out;
    if (!p)
        return JS_EXCEPTION;
    raw = (uint8_t*)malloc(dyn_codec_base64_decode_cap(n) + 1);
    if (!raw) {
        JS_FreeCString(ctx, p);
        return JS_ThrowOutOfMemory(ctx);
    }
    got = dyn_codec_base64_decode(p, n, raw);
    JS_FreeCString(ctx, p);
    if (got == SIZE_MAX || got % 4) {
        free(raw);
        return ai_emb_bad(ctx, i, got == SIZE_MAX ? "not base64" : "byte length is not a multiple of 4", status);
    }
    f = (float*)malloc(got ? got : 1);
    if (!f) {
        free(raw);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (k = 0; k < got / 4; k++) {
        uint32_t u = (uint32_t)raw[4 * k] | (uint32_t)raw[4 * k + 1] << 8
            | (uint32_t)raw[4 * k + 2] << 16 | (uint32_t)raw[4 * k + 3] << 24;
        memcpy(&f[k], &u, 4);
    }
    free(raw);
    out = ai_emb_f32_new(ctx, f, got / 4);
    free(f);
    return out;
}

static JSValue ai_emb_from_arr(JSContext* ctx, JSValueConst a, uint32_t i, int status)
{
    int64_t n = 0, k;
    float* f;
    JSValue out;
    if (ai_arr_len(ctx, a, &n) < 0)
        return JS_EXCEPTION;
    if (n > (int64_t)(1 << 24))
        return ai_emb_bad(ctx, i, "more than 16777216 dimensions", status);
    f = (float*)malloc(n ? (size_t)n * sizeof(float) : 1);
    if (!f)
        return JS_ThrowOutOfMemory(ctx);
    for (k = 0; k < n; k++) {
        JSValue e = JS_GetPropertyUint32(ctx, a, (uint32_t)k);
        double d;
        if (JS_IsException(e)) {
            free(f);
            return JS_EXCEPTION;
        }
        if (!JS_IsNumber(e)) {
            JS_FreeValue(ctx, e);
            free(f);
            return ai_emb_bad(ctx, i, "an element is not a number", status);
        }
        JS_ToFloat64(ctx, &d, e);
        f[k] = (float)d;
    }
    out = ai_emb_f32_new(ctx, f, (size_t)n);
    free(f);
    return out;
}

JSValue ai_emb_f32(JSContext* ctx, JSValue v, int status)
{
    JSValue data = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "data") : JS_UNDEFINED;
    JSValue f = JS_UNDEFINED;
    int64_t n = 0, i;
    if (JS_IsException(data)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    if (!JS_IsArray(ctx, data) || ai_arr_len(ctx, data, &n) < 0) {
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, v);
        return ai_error_throw(ctx, "malformed embeddings response: no data array", status, "api_error");
    }
    for (i = 0; i < n; i++) {
        JSValue it = JS_GetPropertyUint32(ctx, data, (uint32_t)i), e;
        if (JS_IsException(it)) {
            f = JS_EXCEPTION;
            goto fail;
        }
        if (!JS_IsObject(it)) {
            JS_FreeValue(ctx, it);
            f = ai_emb_bad(ctx, (uint32_t)i, "not an object", status);
            goto fail;
        }
        e = JS_GetPropertyStr(ctx, it, "embedding");
        if (JS_IsException(e))
            f = JS_EXCEPTION;
        else if (JS_IsString(e))
            f = ai_emb_from_b64(ctx, e, (uint32_t)i, status);
        else if (JS_IsArray(ctx, e))
            f = ai_emb_from_arr(ctx, e, (uint32_t)i, status);
        else
            f = ai_emb_bad(ctx, (uint32_t)i, "embedding is neither base64 text nor an array", status);
        JS_FreeValue(ctx, e);
        if (JS_IsException(f) || JS_SetPropertyStr(ctx, it, "embedding", f) < 0) {
            JS_FreeValue(ctx, it);
            f = JS_EXCEPTION;
            goto fail;
        }
        JS_FreeValue(ctx, it);
    }
    JS_FreeValue(ctx, data);
    return v;
fail:
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, v);
    return f;
}

static JSValue ai_emb_body(JSContext* ctx, JSValueConst body)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, body, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        JSValue pv = JS_GetProperty(ctx, body, tab[i].atom);
        if (JS_IsException(pv) || JS_SetProperty(ctx, o, tab[i].atom, pv) < 0) {
            JS_FreePropertyEnum(ctx, tab, len);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (JS_SetPropertyStr(ctx, o, "encoding_format", JS_NewString(ctx, "base64")) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue ai_emb_create(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue v, send = JS_UNDEFINED;
    ai_req_t* r;
    int ok, own_format;
    if (!cl)
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    if (!JS_IsObject(body) || JS_IsArray(ctx, body) || JS_IsFunction(ctx, body)) {
        JS_ThrowTypeError(ctx, "embeddings.create: the request body must be an object");
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    }
    if (dyn_get(ctx, body, "model", &v))
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    ok = JS_IsString(v);
    if (ok) {
        size_t l = 0;
        const char* s = JS_ToCStringLen(ctx, &l, v);
        ok = s && l;
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "embeddings.create: model is required (there is no default model)");
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    }
    if (dyn_get(ctx, body, "input", &v))
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    ok = JS_IsString(v) || JS_IsArray(ctx, v);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "embeddings.create: input must be a string or an array");
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    }
    if (dyn_get(ctx, body, "encoding_format", &v))
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    own_format = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    if (!own_format) {
        send = ai_emb_body(ctx, body);
        if (JS_IsException(send))
            return hc_promise_rejected(ctx, JS_EXCEPTION);
    }
    r = ai_req_new(ctx, cl, "POST", "/embeddings", JS_UNDEFINED, own_format ? body : send,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    JS_FreeValue(ctx, send);
    if (!r)
        return hc_promise_rejected(ctx, JS_EXCEPTION);
    r->emb_f32 = !own_format;
    return ai_req_start(ctx, r);
}

const JSCFunctionListEntry ai_embeddings_fns[] = {
    JS_CFUNC_DEF("create", 2, ai_emb_create),
};
const int ai_embeddings_nfns = (int)countof(ai_embeddings_fns);
#endif
