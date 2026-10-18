static JSValue js_using_new_capability(JSContext* ctx)
{
    JSValue obj;
    JSDisposableStackData* s;

    obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_ASYNC_DISPOSABLE_STACK],
        JS_CLASS_ASYNC_DISPOSABLE_STACK);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, s);
    return obj;
}

static int js_using_add(JSContext* ctx, JSValueConst recs_val,
    JSValueConst value, int hint)
{
    JSDisposableStackData* s = JS_GetOpaque2(ctx, recs_val,
        JS_CLASS_ASYNC_DISPOSABLE_STACK);
    JSValue method = JS_UNDEFINED;
    BOOL async = hint != 0;

    if (!s)
        return -1;
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        if (!async)
            return 0;
        return js_disposable_add_with_method(ctx, s, value, JS_UNDEFINED,
            TRUE);
    }
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "using initializers must be objects or null/undefined");
        return -1;
    }
    if (hint == 0) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_dispose);
        if (JS_IsException(method))
            return -1;
    } else if (hint == 2) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_asyncDispose);
        if (JS_IsException(method))
            return -1;
    } else {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_asyncDispose);
        if (JS_IsException(method))
            return -1;
    }
    if (JS_IsUndefined(method)) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_dispose);
        if (JS_IsException(method))
            return -1;
        if (JS_IsUndefined(method)) {
            JS_ThrowTypeError(ctx, "value is not disposable");
            return -1;
        }
        if (async) {
            method = js_make_sync_dispose_wrapper(ctx, method);
            if (JS_IsException(method))
                return -1;
        }
    }
    return js_disposable_add_with_method(ctx, s, value, method, async);
}

static JSValue js_using_dispose_sync(JSContext* ctx,
    JSDisposableStackData* s, JSValue pending)
{
    JSDisposableResource* res = s->resources;
    int count = s->count;
    BOOL has_error = !JS_IsUninitialized(pending);
    int i;

    s->resources = NULL;
    s->count = 0;
    s->size = 0;

    for (i = count - 1; i >= 0; i--) {
        JSValue ret;
        if (JS_IsUndefined(res[i].method)) {
            JS_FreeValue(ctx, res[i].value);
            JS_FreeValue(ctx, res[i].method);
            continue;
        }
        ret = JS_Call(ctx, res[i].method, res[i].value, 0, NULL);
        JS_FreeValue(ctx, res[i].value);
        JS_FreeValue(ctx, res[i].method);
        if (JS_IsException(ret)) {
            JSValue thrown = JS_GetException(ctx);
            if (has_error) {
                pending = js_new_suppressed_error(ctx, thrown, pending);
                if (JS_IsException(pending))
                    pending = JS_GetException(ctx);
            } else {
                pending = thrown;
                has_error = TRUE;
            }
        } else {
            JS_FreeValue(ctx, ret);
        }
    }
    js_free(ctx, res);

    if (has_error)
        return JS_Throw(ctx, pending);
    JS_FreeValue(ctx, pending);
    return JS_UNDEFINED;
}

static JSValue js_using_dispose_value(JSContext* ctx, JSValueConst recs_val,
    JSValue pending, int async)
{
    JSDisposableStackData* s;
    JSValue promise;
    JSValue resolving_funcs[2];

    if (JS_IsUninitialized(recs_val) || JS_IsNull(recs_val) || JS_IsUndefined(recs_val) || !JS_IsObject(recs_val)) {
        if (!JS_IsUninitialized(pending))
            JS_FreeValue(ctx, pending);
        return JS_UNDEFINED;
    }
    s = JS_GetOpaque(recs_val, JS_CLASS_ASYNC_DISPOSABLE_STACK);

    if (!s) {
        if (!JS_IsUninitialized(pending))
            JS_FreeValue(ctx, pending);
        JS_ThrowTypeError(ctx, "invalid using dispose capability");
        return JS_EXCEPTION;
    }
    if (!async)
        return js_using_dispose_sync(ctx, s, pending);
    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        if (!JS_IsUninitialized(pending))
            JS_FreeValue(ctx, pending);
        return JS_EXCEPTION;
    }
    s->disposed = TRUE;
    js_async_dispose_loop(ctx, resolving_funcs[0], resolving_funcs[1],
        recs_val, s->count - 1, pending);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}
