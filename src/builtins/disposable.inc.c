typedef struct JSDisposableResource {
    JSValue value;
    JSValue method;
    BOOL async;
} JSDisposableResource;

typedef struct JSDisposableStackData {
    BOOL disposed;
    int count;
    int size;
    JSDisposableResource* resources;
} JSDisposableStackData;

static void js_disposable_stack_finalizer(JSRuntime* rt, JSValue val)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSDisposableStackData* s = p->u.opaque;
    int i;

    if (s) {
        for (i = 0; i < s->count; i++) {
            JS_FreeValueRT(rt, s->resources[i].value);
            JS_FreeValueRT(rt, s->resources[i].method);
        }
        js_free_rt(rt, s->resources);
        js_free_rt(rt, s);
    }
}

static void js_disposable_stack_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSDisposableStackData* s = p->u.opaque;
    int i;

    if (s) {
        for (i = 0; i < s->count; i++) {
            JS_MarkValue(rt, s->resources[i].value, mark_func);
            JS_MarkValue(rt, s->resources[i].method, mark_func);
        }
    }
}

static JSValue js_new_suppressed_error(JSContext* ctx, JSValue error,
    JSValue suppressed)
{
    JSValue obj;

    obj = JS_NewObjectProtoClass(ctx, ctx->native_error_proto[JS_SUPPRESSED_ERROR],
        JS_CLASS_ERROR);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, suppressed);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_error, error,
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_suppressed, suppressed,
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return obj;
}

static JSValue js_get_dispose_method(JSContext* ctx, JSValueConst obj,
    JSAtom atom)
{
    JSValue method = JS_GetProperty(ctx, obj, atom);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JS_FreeValue(ctx, method);
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        return JS_ThrowTypeError(ctx, "dispose method is not a function");
    }
    return method;
}

static JSValue js_dispose_sync_method_closure(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic, JSValue* func_data)
{
    JSValueConst method = func_data[0];
    JSValue promise, resolving_funcs[2], result, r;
    JSValueConst undef = JS_UNDEFINED;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    result = JS_Call(ctx, method, this_val, 0, NULL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        r = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1,
            (JSValueConst*)&error);
        JS_FreeValue(ctx, error);
    } else {
        JS_FreeValue(ctx, result);
        r = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &undef);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_make_sync_dispose_wrapper(JSContext* ctx, JSValue method)
{
    JSValue f = JS_NewCFunctionData(ctx, js_dispose_sync_method_closure, 0,
        0, 1, (JSValueConst*)&method);
    JS_FreeValue(ctx, method);
    return f;
}

static int js_disposable_stack_push(JSContext* ctx, JSDisposableStackData* s,
    JSValue value, JSValue method, BOOL async)
{
    if (s->count >= s->size) {
        int new_size = s->size ? s->size * 2 : 4;
        JSDisposableResource* nr = js_realloc(ctx, s->resources,
            sizeof(*nr) * new_size);
        if (!nr) {
            JS_FreeValue(ctx, value);
            JS_FreeValue(ctx, method);
            return -1;
        }
        s->resources = nr;
        s->size = new_size;
    }
    s->resources[s->count].value = value;
    s->resources[s->count].method = method;
    s->resources[s->count].async = async;
    s->count++;
    return 0;
}

static JSDisposableStackData* js_disposable_stack_get(JSContext* ctx,
    JSValueConst this_val,
    BOOL async)
{
    JSClassID class_id = async ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;
    return JS_GetOpaque2(ctx, this_val, class_id);
}

static JSValue js_disposable_stack_constructor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv,
    int magic)
{
    JSValue obj;
    JSDisposableStackData* s;
    JSClassID class_id = magic ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    obj = js_create_from_ctor(ctx, new_target, class_id);
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

static int js_disposable_add_with_method(JSContext* ctx,
    JSDisposableStackData* s,
    JSValueConst value, JSValue method,
    BOOL async)
{
    return js_disposable_stack_push(ctx, s, JS_DupValue(ctx, value), method,
        async);
}

static JSValue js_disposable_stack_use(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst value = argv[0];
    JSValue method;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");

    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        if (async) {
            if (js_disposable_stack_push(ctx, s, JS_DupValue(ctx, value),
                    JS_UNDEFINED, TRUE))
                return JS_EXCEPTION;
        }
        return JS_DupValue(ctx, value);
    }
    if (!JS_IsObject(value))
        return JS_ThrowTypeError(ctx, "value is not an object");

    method = JS_UNDEFINED;
    if (async) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_asyncDispose);
        if (JS_IsException(method))
            return JS_EXCEPTION;
    }
    if (JS_IsUndefined(method)) {
        method = js_get_dispose_method(ctx, value, JS_ATOM_Symbol_dispose);
        if (JS_IsException(method))
            return JS_EXCEPTION;
        if (JS_IsUndefined(method))
            return JS_ThrowTypeError(ctx, "value is not disposable");
        if (async) {
            method = js_make_sync_dispose_wrapper(ctx, method);
            if (JS_IsException(method))
                return JS_EXCEPTION;
        }
    }
    if (JS_IsUndefined(method))
        return JS_ThrowTypeError(ctx, "value is not disposable");
    if (js_disposable_add_with_method(ctx, s, value, method, async))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, value);
}

static JSValue js_disposable_adopt_closure(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic, JSValue* func_data)
{
    return JS_Call(ctx, func_data[0], JS_UNDEFINED, 1,
        (JSValueConst*)&func_data[1]);
}

static JSValue js_disposable_stack_adopt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst value = argv[0];
    JSValueConst on_dispose = argv[1];
    JSValueConst data[2];
    JSValue f;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "onDispose is not a function");
    data[0] = on_dispose;
    data[1] = value;
    f = JS_NewCFunctionData(ctx, js_disposable_adopt_closure, 0, 0, 2, data);
    if (JS_IsException(f))
        return JS_EXCEPTION;
    if (js_disposable_stack_push(ctx, s, JS_UNDEFINED, f, async))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, value);
}

static JSValue js_disposable_stack_defer(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, async);
    JSValueConst on_dispose = argv[0];

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    if (!JS_IsFunction(ctx, on_dispose))
        return JS_ThrowTypeError(ctx, "onDispose is not a function");
    if (js_disposable_stack_push(ctx, s, JS_UNDEFINED,
            JS_DupValue(ctx, on_dispose), async))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_disposable_stack_move(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    BOOL async = magic;
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, async);
    JSClassID class_id = async ? JS_CLASS_ASYNC_DISPOSABLE_STACK
                               : JS_CLASS_DISPOSABLE_STACK;
    JSDisposableStackData* ns;
    JSValue obj;

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_ThrowReferenceError(ctx, "disposable stack already disposed");
    obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    ns = js_mallocz(ctx, sizeof(*ns));
    if (!ns) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    ns->resources = s->resources;
    ns->count = s->count;
    ns->size = s->size;
    JS_SetOpaque(obj, ns);
    s->resources = NULL;
    s->count = 0;
    s->size = 0;
    s->disposed = TRUE;
    return obj;
}

static JSValue js_dispose_resources_sync(JSContext* ctx,
    JSDisposableStackData* s)
{
    JSDisposableResource* res = s->resources;
    int count = s->count;
    JSValue pending = JS_UNDEFINED;
    BOOL has_error = FALSE;
    int i;

    s->resources = NULL;
    s->count = 0;
    s->size = 0;

    for (i = count - 1; i >= 0; i--) {
        JSValue ret = JS_Call(ctx, res[i].method, res[i].value, 0, NULL);
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
    return JS_UNDEFINED;
}

static JSValue js_disposable_stack_dispose(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, FALSE);

    if (!s)
        return JS_EXCEPTION;
    if (s->disposed)
        return JS_UNDEFINED;
    s->disposed = TRUE;
    return js_dispose_resources_sync(ctx, s);
}

static JSValue js_disposable_stack_get_disposed(JSContext* ctx,
    JSValueConst this_val,
    int magic)
{
    JSDisposableStackData* s = js_disposable_stack_get(ctx, this_val, magic);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, s->disposed);
}

enum {
    ASYNC_DISPOSE_RESOLVE = 0,
    ASYNC_DISPOSE_REJECT,
    ASYNC_DISPOSE_STACK,
    ASYNC_DISPOSE_INDEX,
    ASYNC_DISPOSE_ERROR,
    ASYNC_DISPOSE_DATA_COUNT,
};

static JSValue js_async_dispose_accumulate(JSContext* ctx, JSValue error,
    JSValue thrown)
{
    if (JS_IsUninitialized(error))
        return thrown;
    return js_new_suppressed_error(ctx, thrown, error);
}

static void js_async_dispose_finish(JSContext* ctx, JSDisposableStackData* s,
    JSValueConst resolve, JSValueConst reject,
    JSValue error)
{
    JSValueConst undef = JS_UNDEFINED;
    JSValue r;
    int i;

    for (i = 0; i < s->count; i++) {
        JS_FreeValue(ctx, s->resources[i].value);
        JS_FreeValue(ctx, s->resources[i].method);
    }
    js_free(ctx, s->resources);
    s->resources = NULL;
    s->count = 0;
    s->size = 0;

    if (JS_IsUninitialized(error)) {
        r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &undef);
    } else {
        r = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst*)&error);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, error);
}

static void js_async_dispose_loop(JSContext* ctx, JSValueConst resolve,
    JSValueConst reject, JSValueConst stack_obj,
    int start_index, JSValue error);

static JSValue js_async_dispose_resume(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* func_data)
{
    JSValueConst resolve = func_data[ASYNC_DISPOSE_RESOLVE];
    JSValueConst reject = func_data[ASYNC_DISPOSE_REJECT];
    JSValueConst stack_obj = func_data[ASYNC_DISPOSE_STACK];
    int index = JS_VALUE_GET_INT(func_data[ASYNC_DISPOSE_INDEX]);
    JSValue error = JS_DupValue(ctx, func_data[ASYNC_DISPOSE_ERROR]);

    if (magic)
        error = js_async_dispose_accumulate(ctx, error,
            JS_DupValue(ctx, argv[0]));
    js_async_dispose_loop(ctx, resolve, reject, stack_obj, index, error);
    return JS_UNDEFINED;
}

static void js_async_dispose_await(JSContext* ctx, JSValueConst resolve,
    JSValueConst reject, JSValueConst stack_obj,
    int next_index, JSValue error,
    JSValue result)
{
    JSValue promise, on_settled[2], r, exc;
    JSValueConst throwaway[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValueConst data[ASYNC_DISPOSE_DATA_COUNT];
    int i, res;

    promise = js_promise_resolve(ctx, ctx->promise_ctor, 1,
        (JSValueConst*)&result, 0);
    JS_FreeValue(ctx, result);
    if (JS_IsException(promise))
        goto fail;
    data[ASYNC_DISPOSE_RESOLVE] = resolve;
    data[ASYNC_DISPOSE_REJECT] = reject;
    data[ASYNC_DISPOSE_STACK] = stack_obj;
    data[ASYNC_DISPOSE_INDEX] = JS_NewInt32(ctx, next_index);
    data[ASYNC_DISPOSE_ERROR] = error;
    for (i = 0; i < 2; i++) {
        on_settled[i] = JS_NewCFunctionData(ctx, js_async_dispose_resume, 1,
            i, ASYNC_DISPOSE_DATA_COUNT,
            (JSValueConst*)data);
        if (JS_IsException(on_settled[i])) {
            if (i)
                JS_FreeValue(ctx, on_settled[0]);
            JS_FreeValue(ctx, promise);
            goto fail;
        }
    }
    JS_FreeValue(ctx, error);
    res = perform_promise_then(ctx, promise, (JSValueConst*)on_settled,
        throwaway);
    JS_FreeValue(ctx, on_settled[0]);
    JS_FreeValue(ctx, on_settled[1]);
    JS_FreeValue(ctx, promise);
    if (res < 0) {
        exc = JS_GetException(ctx);
        r = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    }
    return;
fail:
    exc = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
    r = JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, exc);
}

static void js_async_dispose_loop(JSContext* ctx, JSValueConst resolve,
    JSValueConst reject, JSValueConst stack_obj,
    int start_index, JSValue error)
{
    JSObject* p = JS_VALUE_GET_OBJ(stack_obj);
    JSDisposableStackData* s = p->u.opaque;
    int i = start_index;

    while (i >= 0) {
        JSValueConst method = s->resources[i].method;
        JSValueConst value = s->resources[i].value;
        JSValue result;

        if (JS_IsUndefined(method)) {
            js_async_dispose_await(ctx, resolve, reject, stack_obj, i - 1,
                error, JS_UNDEFINED);
            return;
        }
        result = JS_Call(ctx, method, value, 0, NULL);
        if (JS_IsException(result)) {
            error = js_async_dispose_accumulate(ctx, error,
                JS_GetException(ctx));
            i--;
            continue;
        }
        js_async_dispose_await(ctx, resolve, reject, stack_obj, i - 1,
            error, result);
        return;
    }
    js_async_dispose_finish(ctx, s, resolve, reject, error);
}

static JSValue js_async_disposable_stack_dispose_async(JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv)
{
    JSValue promise, resolving_funcs[2], r, exc;
    JSValueConst undef = JS_UNDEFINED;
    JSDisposableStackData* s;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;

    s = JS_GetOpaque(this_val, JS_CLASS_ASYNC_DISPOSABLE_STACK);
    if (!s) {
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_ASYNC_DISPOSABLE_STACK);
        exc = JS_GetException(ctx);
        r = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1,
            (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    } else if (s->disposed) {
        r = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &undef);
        JS_FreeValue(ctx, r);
    } else {
        s->disposed = TRUE;
        js_async_dispose_loop(ctx, resolving_funcs[0], resolving_funcs[1],
            this_val, s->count - 1, JS_UNINITIALIZED);
    }
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static const JSCFunctionListEntry js_disposable_stack_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("use", 1, js_disposable_stack_use, 0),
    JS_CFUNC_MAGIC_DEF("adopt", 2, js_disposable_stack_adopt, 0),
    JS_CFUNC_MAGIC_DEF("defer", 1, js_disposable_stack_defer, 0),
    JS_CFUNC_MAGIC_DEF("move", 0, js_disposable_stack_move, 0),
    JS_CFUNC_DEF("dispose", 0, js_disposable_stack_dispose),
    JS_CGETSET_MAGIC_DEF("disposed", js_disposable_stack_get_disposed, NULL, 0),
    JS_ALIAS_DEF("[Symbol.dispose]", "dispose"),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DisposableStack", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry js_async_disposable_stack_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("use", 1, js_disposable_stack_use, 1),
    JS_CFUNC_MAGIC_DEF("adopt", 2, js_disposable_stack_adopt, 1),
    JS_CFUNC_MAGIC_DEF("defer", 1, js_disposable_stack_defer, 1),
    JS_CFUNC_MAGIC_DEF("move", 0, js_disposable_stack_move, 1),
    JS_CFUNC_DEF("disposeAsync", 0, js_async_disposable_stack_dispose_async),
    JS_CGETSET_MAGIC_DEF("disposed", js_disposable_stack_get_disposed, NULL, 1),
    JS_ALIAS_DEF("[Symbol.asyncDispose]", "disposeAsync"),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncDisposableStack", JS_PROP_CONFIGURABLE),
};

static const JSClassShortDef js_disposable_stack_class_def[] = {
    { JS_ATOM_DisposableStack, js_disposable_stack_finalizer, js_disposable_stack_mark },
    { JS_ATOM_AsyncDisposableStack, js_disposable_stack_finalizer, js_disposable_stack_mark },
};

int JS_AddIntrinsicDisposableStack(JSContext* ctx)
{
    JSRuntime* rt = ctx->rt;
    JSValue obj;
    JSCFunctionType ft;

    if (!JS_IsRegisteredClass(rt, JS_CLASS_DISPOSABLE_STACK)) {
        if (init_class_range(rt, js_disposable_stack_class_def,
                JS_CLASS_DISPOSABLE_STACK,
                countof(js_disposable_stack_class_def)))
            return -1;
    }
    ft.constructor_magic = js_disposable_stack_constructor;
    obj = JS_NewCConstructor(ctx, JS_CLASS_DISPOSABLE_STACK, "DisposableStack",
        ft.generic, 0,
        JS_CFUNC_constructor_magic, 0,
        JS_UNDEFINED,
        NULL, 0,
        js_disposable_stack_proto_funcs,
        countof(js_disposable_stack_proto_funcs),
        0);
    if (JS_IsException(obj))
        return -1;
    JS_FreeValue(ctx, obj);

    ft.constructor_magic = js_disposable_stack_constructor;
    obj = JS_NewCConstructor(ctx, JS_CLASS_ASYNC_DISPOSABLE_STACK,
        "AsyncDisposableStack",
        ft.generic, 0,
        JS_CFUNC_constructor_magic, 1,
        JS_UNDEFINED,
        NULL, 0,
        js_async_disposable_stack_proto_funcs,
        countof(js_async_disposable_stack_proto_funcs),
        0);
    if (JS_IsException(obj))
        return -1;
    JS_FreeValue(ctx, obj);
    return 0;
}
