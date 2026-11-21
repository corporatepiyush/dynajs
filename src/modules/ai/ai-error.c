#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

JSClassID ai_apierror_class_id;

static const struct {
    const char* type;
    int status;
} ai_error_types[] = {
    { "invalid_request_error", 400 },
    { "authentication_error", 401 },
    { "billing_error", 402 },
    { "permission_error", 403 },
    { "not_found_error", 404 },
    { "request_too_large", 413 },
    { "rate_limit_error", 429 },
    { "api_error", 500 },
    { "overloaded_error", 529 },
};

int ai_error_status_of(const char* type)
{
    size_t i;
    if (!type)
        return 0;
    for (i = 0; i < countof(ai_error_types); i++)
        if (strcmp(ai_error_types[i].type, type) == 0)
            return ai_error_types[i].status;
    return 0;
}

static const char* ai_error_type_of(int status)
{
    size_t i;
    for (i = 0; i < countof(ai_error_types); i++)
        if (ai_error_types[i].status == status)
            return ai_error_types[i].type;
    if (status >= 500)
        return "api_error";
    return status ? "http_error" : "connection_error";
}

static int ai_error_retryable(int status, const char* type)
{
    if (status)
        return httpk_retry_status(status, HTTPK_RETRY_API);
    return type && strcmp(type, "connection_error") == 0;
}

static int ai_def(JSContext* ctx, JSValueConst o, const char* k, JSValue v, int flags)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, o, k, v, flags) < 0 ? -1 : 0;
}

JSValue ai_error_new(JSContext* ctx, const char* message, int status,
    const char* type, const char* request_id, double retry_after_ms,
    JSValueConst headers, JSValueConst body)
{
    JSValue e = JS_NewError(ctx), proto;
    int rc = 0;
    if (JS_IsException(e))
        return e;
    proto = JS_GetClassProto(ctx, ai_apierror_class_id);
    if (JS_IsObject(proto))
        rc = JS_SetPrototype(ctx, e, proto) < 0 ? -1 : 0;
    JS_FreeValue(ctx, proto);
    if (!type)
        type = ai_error_type_of(status);
    if (rc
        || ai_def(ctx, e, "message", JS_NewString(ctx, message ? message : type), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)
        || ai_def(ctx, e, "status", JS_NewInt32(ctx, status), JS_PROP_C_W_E)
        || ai_def(ctx, e, "type", JS_NewString(ctx, type), JS_PROP_C_W_E)
        || ai_def(ctx, e, "requestId", request_id ? JS_NewString(ctx, request_id) : JS_NULL, JS_PROP_C_W_E)
        || ai_def(ctx, e, "retryAfterMs", retry_after_ms >= 0 ? JS_NewFloat64(ctx, retry_after_ms) : JS_NULL, JS_PROP_C_W_E)
        || ai_def(ctx, e, "retryable", JS_NewBool(ctx, ai_error_retryable(status, type)), JS_PROP_C_W_E)
        || ai_def(ctx, e, "headers", JS_IsObject(headers) ? JS_DupValue(ctx, headers) : JS_NULL, JS_PROP_C_W_E)
        || ai_def(ctx, e, "body", JS_IsUndefined(body) ? JS_NULL : JS_DupValue(ctx, body), JS_PROP_C_W_E)) {
        JS_FreeValue(ctx, e);
        return JS_EXCEPTION;
    }
    return e;
}

JSValue ai_error_throw(JSContext* ctx, const char* message, int status,
    const char* type)
{
    JSValue e = ai_error_new(ctx, message, status, type, NULL, -1, JS_NULL,
        JS_NULL);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

static int ai_get_cstr(JSContext* ctx, JSValueConst obj, const char* key, char** out)
{
    JSValue v;
    const char* s;
    *out = NULL;
    if (dyn_get(ctx, obj, key, &v))
        return -1;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue ai_error_build(JSContext* ctx, int status, JSValueConst parsed,
    const char* fallback, JSValueConst headers, double retry_after_ms,
    const char* request_id_hdr)
{
    char *type = NULL, *msg = NULL, *rid = NULL, buf[512];
    JSValue err = JS_UNDEFINED, e = JS_EXCEPTION;
    if (JS_IsObject(parsed)) {
        if (dyn_get(ctx, parsed, "error", &err))
            goto out;
        if (JS_IsObject(err) && (ai_get_cstr(ctx, err, "type", &type) || ai_get_cstr(ctx, err, "message", &msg)))
            goto out;
        if (ai_get_cstr(ctx, parsed, "request_id", &rid))
            goto out;
    }
    if (!status && type)
        status = ai_error_status_of(type);
    snprintf(buf, sizeof buf, "%d %s: %.400s", status,
        type ? type : ai_error_type_of(status),
        msg ? msg : (fallback && *fallback ? fallback : "request failed"));
    e = ai_error_new(ctx, buf, status, type, rid ? rid : request_id_hdr,
        retry_after_ms, headers, parsed);
out:
    JS_FreeValue(ctx, err);
    free(type);
    free(msg);
    free(rid);
    return e;
}

JSValue ai_error_from_body(JSContext* ctx, int status, const char* body,
    size_t n, JSValueConst headers, double retry_after_ms)
{
    JSValue parsed = JS_UNDEFINED, e;
    char fallback[160], *rid = NULL;
    size_t k = n < 120 ? n : 120, i;
    for (i = 0; i < k; i++)
        fallback[i] = (body[i] >= 0x20 && body[i] < 0x7f) ? body[i] : ' ';
    fallback[k] = 0;
    if (n) {
        char* z = (char*)malloc(n + 1);
        if (z) {
            memcpy(z, body, n);
            z[n] = 0;
            parsed = JS_ParseJSON(ctx, z, n, "<api error>");
            free(z);
            if (JS_IsException(parsed)) {
                if (!dyn_take_syntax_error(ctx))
                    return JS_EXCEPTION;
                parsed = JS_UNDEFINED;
            }
        }
    }
    if (JS_IsObject(headers) && (ai_get_cstr(ctx, headers, "request-id", &rid) || (!rid && ai_get_cstr(ctx, headers, "x-request-id", &rid))))
        e = JS_EXCEPTION;
    else
        e = ai_error_build(ctx, status, parsed, fallback, headers, retry_after_ms,
            rid);
    free(rid);
    JS_FreeValue(ctx, parsed);
    return e;
}

JSValue ai_error_from_event(JSContext* ctx, JSValueConst data)
{
    return ai_error_build(ctx, 0, data, "error event", JS_NULL, -1, NULL);
}

JSValue ai_error_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    const char *msg = NULL, *type = NULL, *rid = NULL;
    int32_t status = 0;
    double ra = -1;
    JSValue init = argc > 1 ? argv[1] : JS_UNDEFINED, v, headers = JS_NULL,
            body = JS_NULL, e = JS_EXCEPTION, tv = JS_UNDEFINED, rv = JS_UNDEFINED;
    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        msg = JS_ToCString(ctx, argv[0]);
        if (!msg)
            return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(init) && !JS_IsObject(init)) {
        JS_ThrowTypeError(ctx, "APIError: init must be an object");
        goto out;
    }
    if (JS_IsObject(init)) {
        v = JS_GetPropertyStr(ctx, init, "status");
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &status, v)) {
            JS_FreeValue(ctx, v);
            goto out;
        }
        JS_FreeValue(ctx, v);
        tv = JS_GetPropertyStr(ctx, init, "type");
        if (JS_IsException(tv))
            goto out;
        if (JS_IsString(tv)) {
            type = JS_ToCString(ctx, tv);
            if (!type)
                goto out;
        }
        rv = JS_GetPropertyStr(ctx, init, "requestId");
        if (JS_IsException(rv))
            goto out;
        if (JS_IsString(rv)) {
            rid = JS_ToCString(ctx, rv);
            if (!rid)
                goto out;
        }
        if (dyn_get(ctx, init, "retryAfterMs", &v))
            goto out;
        if (JS_IsNumber(v) && JS_ToFloat64(ctx, &ra, v)) {
            JS_FreeValue(ctx, v);
            goto out;
        }
        JS_FreeValue(ctx, v);
        if (dyn_get(ctx, init, "headers", &headers) || dyn_get(ctx, init, "body", &body))
            goto out;
    }
    e = ai_error_new(ctx, msg, status, type, rid, ra, headers, body);
out:
    JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, rid);
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, headers);
    JS_FreeValue(ctx, body);
    return e;
}

static JSClassDef ai_apierror_class = { "APIError", NULL, NULL, NULL, NULL };

int ai_error_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue global, errctor, errproto, proto, ctor;
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!ai_apierror_class_id)
        JS_NewClassID(&ai_apierror_class_id);
    if (!JS_IsRegisteredClass(rt, ai_apierror_class_id)
        && JS_NewClass(rt, ai_apierror_class_id, &ai_apierror_class) < 0)
        return -1;
    global = JS_GetGlobalObject(ctx);
    errctor = JS_GetPropertyStr(ctx, global, "Error");
    JS_FreeValue(ctx, global);
    errproto = JS_GetPropertyStr(ctx, errctor, "prototype");
    if (JS_IsException(errproto)) {
        JS_FreeValue(ctx, errctor);
        return -1;
    }
    proto = JS_NewObjectProto(ctx, errproto);
    JS_FreeValue(ctx, errproto);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, errctor);
        return -1;
    }
    JS_DefinePropertyValueStr(ctx, proto, "name", JS_NewString(ctx, "APIError"),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    ctor = JS_NewCFunction2(ctx, ai_error_ctor, "APIError", 2,
        JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPrototype(ctx, ctor, errctor);
    JS_FreeValue(ctx, errctor);
    JS_SetClassProto(ctx, ai_apierror_class_id, proto);
    return JS_SetModuleExport(ctx, m, "APIError", ctor);
}
#endif
