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
    s->pending = JS_UNINITIALIZED;
    s->await_value = JS_UNDEFINED;
    s->cursor = -1;
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
            if (js_exception_uncatchable(ctx)) {
                while (--i >= 0) {
                    JS_FreeValue(ctx, res[i].value);
                    JS_FreeValue(ctx, res[i].method);
                }
                js_free(ctx, res);
                JS_FreeValue(ctx, pending);
                return JS_EXCEPTION;
            }
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

static void js_using_accumulate(JSContext* ctx, JSDisposableStackData* s,
    JSValue thrown)
{
    if (JS_IsUninitialized(s->pending)) {
        s->pending = thrown;
        return;
    }
    s->pending = js_new_suppressed_error(ctx, thrown, s->pending);
    if (JS_IsException(s->pending))
        s->pending = js_exception_uncatchable(ctx) ? JS_DupValue(ctx, ctx->rt->current_exception) : JS_GetException(ctx);
}

static JSValue js_using_set_await(JSContext* ctx, JSDisposableStackData* s,
    JSValue v)
{
    JS_FreeValue(ctx, s->await_value);
    s->await_value = v;
    return JS_TRUE;
}

static JSValue js_using_dispose_step(JSContext* ctx, JSDisposableStackData* s)
{
    while (s->cursor >= 0) {
        JSDisposableResource* r = &s->resources[s->cursor];
        JSValue method, value, ret;
        BOOL async = r->async;

        if (!async && s->needs_await && !s->has_awaited) {
            s->needs_await = FALSE;
            return js_using_set_await(ctx, s, JS_UNDEFINED);
        }
        s->cursor--;
        method = r->method;
        value = r->value;
        r->method = JS_UNDEFINED;
        r->value = JS_UNDEFINED;
        if (JS_IsUndefined(method)) {
            JS_FreeValue(ctx, value);
            s->needs_await = TRUE;
            continue;
        }
        ret = JS_Call(ctx, method, value, 0, NULL);
        JS_FreeValue(ctx, method);
        JS_FreeValue(ctx, value);
        if (JS_IsException(ret)) {
            if (js_exception_uncatchable(ctx))
                return JS_EXCEPTION;
            js_using_accumulate(ctx, s, JS_GetException(ctx));
            continue;
        }
        if (async) {
            s->has_awaited = TRUE;
            return js_using_set_await(ctx, s, ret);
        }
        JS_FreeValue(ctx, ret);
    }
    if (s->needs_await && !s->has_awaited) {
        s->needs_await = FALSE;
        return js_using_set_await(ctx, s, JS_UNDEFINED);
    }
    return JS_FALSE;
}

static JSValue js_using_dispose_finish(JSContext* ctx, JSDisposableStackData* s)
{
    JSValue pending = s->pending;
    int i;

    for (i = 0; i < s->count; i++) {
        JS_FreeValue(ctx, s->resources[i].value);
        JS_FreeValue(ctx, s->resources[i].method);
    }
    js_free(ctx, s->resources);
    s->resources = NULL;
    s->count = 0;
    s->size = 0;
    s->cursor = -1;
    s->pending = JS_UNINITIALIZED;
    if (JS_IsUninitialized(pending))
        return JS_UNDEFINED;
    return JS_Throw(ctx, pending);
}

static JSValue js_using_dispose_value(JSContext* ctx, JSValueConst recs_val,
    JSValue pending, int flags)
{
    JSDisposableStackData* s;
    int mode = flags >> 2;
    JSValue v;

    s = JS_IsObject(recs_val) ? JS_GetOpaque(recs_val, JS_CLASS_ASYNC_DISPOSABLE_STACK) : NULL;
    if (!s) {
        if (JS_IsUninitialized(pending) || mode == USING_DISPOSE_REJECT)
            JS_FreeValue(ctx, pending);
        else
            return JS_Throw(ctx, pending);
        if (mode == USING_DISPOSE_STEP)
            return JS_FALSE;
        return JS_UNDEFINED;
    }
    switch (mode) {
    case USING_DISPOSE_BEGIN:
        JS_FreeValue(ctx, s->pending);
        s->pending = pending;
        s->cursor = s->count - 1;
        s->needs_await = FALSE;
        s->has_awaited = FALSE;
        s->disposed = TRUE;
        return JS_UNDEFINED;
    case USING_DISPOSE_STEP:
        return js_using_dispose_step(ctx, s);
    case USING_DISPOSE_TAKE:
        v = s->await_value;
        s->await_value = JS_UNDEFINED;
        return v;
    case USING_DISPOSE_REJECT:
        js_using_accumulate(ctx, s, pending);
        return JS_UNDEFINED;
    case USING_DISPOSE_FINISH:
        return js_using_dispose_finish(ctx, s);
    default:
        return js_using_dispose_sync(ctx, s, pending);
    }
}
