#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static int ai_body_check(JSContext* ctx, JSValueConst body, const char* who,
    int need_max_tokens)
{
    JSValue v;
    int ok;
    if (!JS_IsObject(body) || JS_IsArray(ctx, body) || JS_IsFunction(ctx, body)) {
        JS_ThrowTypeError(ctx, "%s: the request body must be an object", who);
        return -1;
    }
    if (dyn_get(ctx, body, "model", &v))
        return -1;
    ok = JS_IsString(v);
    if (ok) {
        const char* s = JS_ToCString(ctx, v);
        ok = s && *s;
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "%s: model is required (there is no default model)", who);
        return -1;
    }
    if (dyn_get(ctx, body, "messages", &v))
        return -1;
    ok = JS_IsArray(ctx, v);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "%s: messages must be an array", who);
        return -1;
    }
    if (need_max_tokens) {
        if (dyn_get(ctx, body, "max_tokens", &v))
            return -1;
        ok = JS_IsNumber(v);
        JS_FreeValue(ctx, v);
        if (!ok) {
            JS_ThrowTypeError(ctx, "%s: max_tokens is required (a number)", who);
            return -1;
        }
    }
    return 0;
}

static JSValue ai_reject(JSContext* ctx)
{
    return hc_promise_rejected(ctx, JS_EXCEPTION);
}

static JSValue ai_msg_create(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    ai_req_t* r;
    int streaming;
    if (!cl || ai_body_check(ctx, body, "messages.create", 1) < 0)
        return ai_reject(ctx);
    streaming = dyn_get_truthy(ctx, body, "stream");
    if (streaming < 0)
        return ai_reject(ctx);
    if (streaming) {
        JS_ThrowTypeError(ctx, "messages.create: stream: true is not accepted here; use messages.stream(body)");
        return ai_reject(ctx);
    }
    r = ai_req_new(ctx, cl, "POST", "/v1/messages", JS_UNDEFINED, body,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

JSValue ai_body_with_stream(JSContext* ctx, JSValueConst body, int usage)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int has_opts = 0;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, body,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int skip = k && strcmp(k, "stream") == 0;
        if (k && strcmp(k, "stream_options") == 0)
            has_opts = 1;
        JS_FreeCString(ctx, k);
        if (skip)
            continue;
        JSValue pv = JS_GetProperty(ctx, body, tab[i].atom);
        if (JS_IsException(pv) || JS_SetProperty(ctx, o, tab[i].atom, pv) < 0) {
            JS_FreePropertyEnum(ctx, tab, len);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    JS_SetPropertyStr(ctx, o, "stream", JS_TRUE);
    if (usage && !has_opts) {
        JSValue so = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, so, "include_usage", JS_TRUE);
        JS_SetPropertyStr(ctx, o, "stream_options", so);
    }
    return o;
}

static JSValue ai_msg_stream(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue copy;
    ai_req_t* r;
    if (!cl || ai_body_check(ctx, body, "messages.stream", 1) < 0)
        return JS_EXCEPTION;
    copy = ai_body_with_stream(ctx, body, 0);
    if (JS_IsException(copy))
        return copy;
    r = ai_req_new(ctx, cl, "POST", "/v1/messages", JS_UNDEFINED, copy,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_STREAM);
    JS_FreeValue(ctx, copy);
    if (!r)
        return JS_EXCEPTION;
    return ai_stream_new(ctx, r);
}

static JSValue ai_msg_count(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    ai_req_t* r;
    if (!cl || ai_body_check(ctx, body, "messages.countTokens", 0) < 0)
        return ai_reject(ctx);
    r = ai_req_new(ctx, cl, "POST", "/v1/messages/count_tokens", JS_UNDEFINED,
        body, argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

const JSCFunctionListEntry ai_messages_fns[] = {
    JS_CFUNC_DEF("create", 2, ai_msg_create),
    JS_CFUNC_DEF("stream", 2, ai_msg_stream),
    JS_CFUNC_DEF("countTokens", 2, ai_msg_count),
};
const int ai_messages_nfns = (int)countof(ai_messages_fns);

static JSValue ai_models_list(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    ai_req_t* r;
    if (!cl)
        return ai_reject(ctx);
    r = ai_req_new(ctx, cl, "GET", "/v1/models", argc > 0 ? argv[0] : JS_UNDEFINED,
        JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_models_retrieve(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    ai_req_t* r;
    dyn_sb_t path;
    const char* id;
    size_t il;
    if (!cl)
        return ai_reject(ctx);
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "models.retrieve(id: string)");
        return ai_reject(ctx);
    }
    id = JS_ToCStringLen(ctx, &il, argv[0]);
    if (!id)
        return ai_reject(ctx);
    if (!il) {
        JS_FreeCString(ctx, id);
        JS_ThrowTypeError(ctx, "models.retrieve: id must not be empty");
        return ai_reject(ctx);
    }
    dyn_sb_init(&path);
    dyn_sb_puts(&path, "/v1/models/");
    ai_path_seg(&path, id, il);
    dyn_sb_putc(&path, 0);
    JS_FreeCString(ctx, id);
    if (path.oom) {
        dyn_sb_free(&path);
        JS_ThrowOutOfMemory(ctx);
        return ai_reject(ctx);
    }
    r = ai_req_new(ctx, cl, "GET", path.p, JS_UNDEFINED, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    dyn_sb_free(&path);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

const JSCFunctionListEntry ai_models_fns[] = {
    JS_CFUNC_DEF("list", 2, ai_models_list),
    JS_CFUNC_DEF("retrieve", 2, ai_models_retrieve),
};
const int ai_models_nfns = (int)countof(ai_models_fns);
#endif
