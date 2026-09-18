/* Iterator + Iterator Helper intrinsics and their prototype tables.
 *
 * Unity-build fragment: #included into src/dynajs.c, never compiled alone.
 * Split out of the former object_array_iterator.inc.c (byte-identical token
 * stream preserved). */
/* Iterator */

static JSValue js_iterator_constructor_getset(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic,
                                              JSValue *func_data)
{
    int ret;

    if (argc > 0) { // if setter
        if (!JS_IsObject(argv[0]))
            return JS_ThrowTypeErrorNotAnObject(ctx);
        ret = JS_DefinePropertyValue(ctx, this_val, JS_ATOM_constructor,
                                     JS_DupValue(ctx, argv[0]),
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        if (ret < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    } else {
        return JS_DupValue(ctx, func_data[0]);
    }
}

static JSValue js_iterator_constructor(JSContext *ctx, JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    JSObject *p;

    if (JS_TAG_OBJECT != JS_VALUE_GET_TAG(new_target))
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    p = JS_VALUE_GET_OBJ(new_target);
    if (p->class_id == JS_CLASS_C_FUNCTION &&
        p->u.cfunc.c_function.generic == js_iterator_constructor) {
        return JS_ThrowTypeError(ctx, "abstract class not constructable");
    }
    return js_create_from_ctor(ctx, new_target, JS_CLASS_ITERATOR);
}

// note: deliberately doesn't use space-saving bit fields for
// |index|, |count| and |running| because tcc miscompiles them
typedef struct JSIteratorConcatData {
    int index, count;             // elements (not pairs!) in values[] array
    BOOL running;
    JSValue iter, next, values[]; // array of (object, method) pairs
} JSIteratorConcatData;

static void js_iterator_concat_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData *it = p->u.iterator_concat_data;
    if (it) {
        JS_FreeValueRT(rt, it->iter);
        JS_FreeValueRT(rt, it->next);
        for (int i = it->index; i < it->count; i++)
            JS_FreeValueRT(rt, it->values[i]);
        js_free_rt(rt, it);
    }
}

static void js_iterator_concat_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData *it = p->u.iterator_concat_data;
    if (it) {
        JS_MarkValue(rt, it->iter, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        for (int i = it->index; i < it->count; i++)
            JS_MarkValue(rt, it->values[i], mark_func);
    }
}

static JSValue js_iterator_concat_next(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv,
                                       int *pdone, int magic)
{
    JSValue iter, item, next, val, *obj, *meth, ret;
    JSIteratorConcatData *it;
    int done;

    *pdone = FALSE;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_CONCAT);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");

    it->running = TRUE;
    for(;;) {
        if (it->index >= it->count) {
            *pdone = TRUE;
            ret = JS_UNDEFINED;
            break;
        }
        obj = &it->values[it->index + 0];
        meth = &it->values[it->index + 1];
        iter = it->iter;
        if (JS_IsUndefined(iter)) {
            iter = JS_GetIterator2(ctx, *obj, *meth);
            if (JS_IsException(iter))
                goto fail;
            it->iter = iter;
        }
        next = it->next;
        if (JS_IsUndefined(next)) {
            next = JS_GetProperty(ctx, iter, JS_ATOM_next);
            if (JS_IsException(next))
                goto fail;
            it->next = next;
        }
        item = JS_IteratorNext2(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done == 0) {
            ret = item;
            break;
        } else if (done == 2) {
            val = JS_GetProperty(ctx, item, JS_ATOM_done);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, item);
            fail:
                ret = JS_EXCEPTION;
                break;
            }
            done = JS_ToBoolFree(ctx, val);
            if (done)
                goto done_next;
            ret = JS_GetProperty(ctx, item, JS_ATOM_value);
            JS_FreeValue(ctx, item);
            break;
        } else {
        done_next:
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, iter);
            JS_FreeValue(ctx, next);
            it->iter = JS_UNDEFINED;
            it->next = JS_UNDEFINED;
            JS_FreeValue(ctx, *meth);
            JS_FreeValue(ctx, *obj);
            it->index += 2;
        }
    }
    it->running = FALSE;
    return ret;
}

static JSValue js_iterator_concat_return(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSIteratorConcatData *it;
    JSValue ret;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_CONCAT);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");
    ret = JS_UNDEFINED;
    if (!JS_IsUndefined(it->iter)) {
        it->running = TRUE;
        ret = JS_GetProperty(ctx, it->iter, JS_ATOM_return);
        if (JS_IsException(ret)) {
            it->running = FALSE;
            return JS_EXCEPTION;
        }
        ret = JS_CallFree(ctx, ret, it->iter, 0, NULL);
        it->running = FALSE;
    }
    while (it->index < it->count)
        JS_FreeValue(ctx, it->values[it->index++]);
    JS_FreeValue(ctx, it->iter);
    JS_FreeValue(ctx, it->next);
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    return ret;
}

static const JSCFunctionListEntry js_iterator_concat_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_concat_next, 0 ),
    JS_CFUNC_DEF("return", 0, js_iterator_concat_return ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Concat", JS_PROP_CONFIGURABLE ),
};

static JSValue js_iterator_concat(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSIteratorConcatData *it;
    JSValue obj, method;

    it = js_malloc(ctx, sizeof(*it) + 2*argc * sizeof(it->values[0]));
    if (!it)
        return JS_EXCEPTION;
    it->running = FALSE;
    it->index = 0;
    it->count = 0;
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        JSValueConst obj = argv[i];
        if (!JS_IsObject(obj)) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            goto fail;
        }
        method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
        if (JS_IsException(method))
            goto fail;
        if (!JS_IsFunction(ctx, method)) {
            JS_ThrowTypeError(ctx, "not a function");
            JS_FreeValue(ctx, method);
            goto fail;
        }
        it->values[it->count++] = JS_DupValue(ctx, obj);
        it->values[it->count++] = method;
    }
    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_CONCAT);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, it);
    return obj;
fail:
    for (int i = 0; i < it->count; i++)
        JS_FreeValue(ctx, it->values[i]);
    js_free(ctx, it);
    return JS_EXCEPTION;
}

/* ---- Iterator.zip / Iterator.zipKeyed (joint iteration) ----------------
 *
 * Steps N sub-iterators in lockstep, yielding one tuple per round. Three modes
 * differ only in what happens when they do not finish together:
 *
 *   "shortest" (default) -- stop at the first exhausted input, and CLOSE the
 *                           rest. Without that close, zipping an infinite
 *                           generator against a finite list would leak it.
 *   "longest"            -- run until all are exhausted, substituting a per-
 *                           input padding value for the ones already finished.
 *   "strict"             -- a TypeError if they do not all finish on the same
 *                           round, which is how you assert equal lengths.
 *
 * zipKeyed is the same machine over an object's own enumerable keys, yielding
 * an object per round instead of an array; `keys` non-NULL selects it.
 *
 * The `finished` flag per input is what makes "longest" work without asking a
 * spent iterator for another value -- calling next() again after done is
 * legal but not guaranteed, so it is not done here. */

typedef struct JSIteratorZipData {
    int count;          /* number of inputs */
    int live;           /* inputs not yet exhausted */
    int mode;           /* 0 shortest, 1 longest, 2 strict */
    BOOL running;
    BOOL done;
    JSAtom *keys;       /* zipKeyed: one own enumerable key per input, else NULL */
    JSValue pad;        /* array of padding values ("longest"), else UNDEFINED */
    /* 2 slots per input: iterator, next-method. Then `count` BOOL-ish flags
     * packed as JS_TRUE/JS_FALSE so they are traced uniformly. */
    JSValue slots[];
} JSIteratorZipData;

#define ZIP_ITER(it, i)     ((it)->slots[(i) * 2])
#define ZIP_NEXT(it, i)     ((it)->slots[(i) * 2 + 1])
#define ZIP_FIN(it, i)      ((it)->slots[(it)->count * 2 + (i)])

static void js_iterator_zip_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorZipData *it = p->u.iterator_zip_data;
    if (it) {
        int n = it->count * 3;
        for (int i = 0; i < n; i++)
            JS_FreeValueRT(rt, it->slots[i]);
        if (it->keys) {
            for (int i = 0; i < it->count; i++)
                JS_FreeAtomRT(rt, it->keys[i]);
            js_free_rt(rt, it->keys);
        }
        JS_FreeValueRT(rt, it->pad);
        js_free_rt(rt, it);
    }
}

static void js_iterator_zip_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorZipData *it = p->u.iterator_zip_data;
    if (it) {
        int n = it->count * 3;
        for (int i = 0; i < n; i++)
            JS_MarkValue(rt, it->slots[i], mark_func);
        JS_MarkValue(rt, it->pad, mark_func);
    }
}

/* Close every input that has not finished, ignoring secondary errors: we are
 * already unwinding, and the first error is the one worth reporting. */
static void js_iterator_zip_close_all(JSContext *ctx, JSIteratorZipData *it,
                                      int except)
{
    for (int i = 0; i < it->count; i++) {
        if (i == except || JS_ToBool(ctx, ZIP_FIN(it, i)))
            continue;
        JS_IteratorClose(ctx, ZIP_ITER(it, i), FALSE);
        JS_FreeValue(ctx, ZIP_FIN(it, i));
        ZIP_FIN(it, i) = JS_TRUE;
    }
    it->live = 0;
}

static JSValue js_iterator_zip_next(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    int *pdone, int magic)
{
    JSIteratorZipData *it;
    JSValue tuple, item;
    int i, done, nfinished = 0;

    *pdone = FALSE;
    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_ZIP);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");
    if (it->done || it->count == 0) {
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    it->running = TRUE;
    tuple = it->keys ? JS_NewObject(ctx) : JS_NewArray(ctx);
    if (JS_IsException(tuple)) {
        it->running = FALSE;
        return JS_EXCEPTION;
    }

    for (i = 0; i < it->count; i++) {
        JSValue v;
        if (JS_ToBool(ctx, ZIP_FIN(it, i))) {
            nfinished++;
            v = JS_UNDEFINED;
            if (!JS_IsUndefined(it->pad))
                v = JS_GetPropertyUint32(ctx, it->pad, i);
            if (JS_IsException(v))
                goto fail;
        } else {
            item = JS_IteratorNext(ctx, ZIP_ITER(it, i), ZIP_NEXT(it, i),
                                   0, NULL, &done);
            if (JS_IsException(item))
                goto fail;
            if (done) {
                JS_FreeValue(ctx, ZIP_FIN(it, i));
                ZIP_FIN(it, i) = JS_TRUE;
                it->live--;
                nfinished++;
                if (it->mode == 0) {          /* shortest: stop now */
                    JS_FreeValue(ctx, tuple);
                    js_iterator_zip_close_all(ctx, it, i);
                    it->done = TRUE;
                    it->running = FALSE;
                    *pdone = TRUE;
                    return JS_UNDEFINED;
                }
                v = JS_UNDEFINED;
                if (!JS_IsUndefined(it->pad))
                    v = JS_GetPropertyUint32(ctx, it->pad, i);
                if (JS_IsException(v))
                    goto fail;
            } else {
                v = item;
            }
        }
        if (it->keys) {
            if (JS_DefinePropertyValue(ctx, tuple, JS_DupAtom(ctx, it->keys[i]),
                                       v, JS_PROP_C_W_E) < 0)
                goto fail;
        } else {
            if (JS_DefinePropertyValueUint32(ctx, tuple, i, v,
                                             JS_PROP_C_W_E) < 0)
                goto fail;
        }
    }

    /* strict: they must all finish on the SAME round */
    if (it->mode == 2 && nfinished > 0 && nfinished < it->count) {
        JS_FreeValue(ctx, tuple);
        js_iterator_zip_close_all(ctx, it, -1);
        it->done = TRUE;
        it->running = FALSE;
        return JS_ThrowTypeError(ctx,
            "Iterator.zip: inputs of unequal length in strict mode");
    }
    if (nfinished == it->count) {           /* longest/strict: all spent */
        JS_FreeValue(ctx, tuple);
        it->done = TRUE;
        it->running = FALSE;
        *pdone = TRUE;
        return JS_UNDEFINED;
    }
    it->running = FALSE;
    return tuple;

fail:
    JS_FreeValue(ctx, tuple);
    js_iterator_zip_close_all(ctx, it, -1);
    it->done = TRUE;
    it->running = FALSE;
    return JS_EXCEPTION;
}

static JSValue js_iterator_zip_return(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSIteratorZipData *it;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_ZIP);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");
    it->running = TRUE;
    js_iterator_zip_close_all(ctx, it, -1);
    it->done = TRUE;
    it->running = FALSE;
    /* IteratorClose requires an OBJECT back; returning undefined here made
     * `break` out of a for..of throw "not an object" instead of closing. */
    return js_create_iterator_result(ctx, JS_UNDEFINED, TRUE);
}

static const JSCFunctionListEntry js_iterator_zip_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_zip_next, 0 ),
    JS_CFUNC_DEF("return", 0, js_iterator_zip_return ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Zip", JS_PROP_CONFIGURABLE ),
};

/* Drain an iterable into a fresh JS Array. zip needs its inputs materialised
 * up front -- the tuple width is fixed at construction -- and there is no
 * general iterable-to-array helper in the engine to borrow. */
static JSValue js_zip_drain(JSContext *ctx, JSValueConst iterable)
{
    JSValue iter, next, arr, item;
    uint32_t i = 0;
    int done;

    iter = JS_GetIterator(ctx, iterable, FALSE);
    if (JS_IsException(iter))
        return JS_EXCEPTION;
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, iter);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto fail;
    for (;;) {
        item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        if (JS_DefinePropertyValueUint32(ctx, arr, i++, item,
                                         JS_PROP_C_W_E) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iter);
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, next);
    JS_IteratorClose(ctx, iter, TRUE);
    JS_FreeValue(ctx, iter);
    return JS_EXCEPTION;
}

/* Read {mode, padding} once, at construction, so a getter cannot change the
 * mode part-way through iteration. */
static int js_iterator_zip_options(JSContext *ctx, JSValueConst options,
                                   int *pmode, JSValue *ppad)
{
    JSValue v;
    const char *s;

    *pmode = 0;
    *ppad = JS_UNDEFINED;
    if (JS_IsUndefined(options) || JS_IsNull(options))
        return 0;
    if (!JS_IsObject(options)) {
        JS_ThrowTypeError(ctx, "Iterator.zip: options must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, options, "mode");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        if (!strcmp(s, "shortest")) *pmode = 0;
        else if (!strcmp(s, "longest")) *pmode = 1;
        else if (!strcmp(s, "strict")) *pmode = 2;
        else {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx,
                "Iterator.zip: mode must be \"shortest\", \"longest\" or \"strict\"");
            return -1;
        }
        JS_FreeCString(ctx, s);
    } else {
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, options, "padding");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (*pmode != 1) {
            /* padding is only consulted in "longest"; materialise it anyway so
             * a bad value is rejected eagerly rather than silently ignored */
            JS_FreeValue(ctx, v);
            return 0;
        }
        *ppad = js_zip_drain(ctx, v);
        JS_FreeValue(ctx, v);
        if (JS_IsException(*ppad))
            return -1;
    } else {
        JS_FreeValue(ctx, v);
    }
    return 0;
}

/* magic 0 = zip (array of iterables), 1 = zipKeyed (object of iterables) */
static JSValue js_iterator_zip(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    JSIteratorZipData *it = NULL;
    JSValue list = JS_UNDEFINED, pad = JS_UNDEFINED, obj;
    JSAtom *keys = NULL;
    int mode, count = 0, i;

    if (JS_IsString(argv[0])) {
        /* a string is iterable but zipping its characters is never intended;
         * the spec rejects it so Iterator.from(str) has to be explicit */
        return JS_ThrowTypeError(ctx, "Iterator.zip: iterables must not be a string");
    }
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    if (js_iterator_zip_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                                &mode, &pad) < 0)
        return JS_EXCEPTION;

    if (magic == 0) {
        list = js_zip_drain(ctx, argv[0]);
        if (JS_IsException(list))
            goto fail;
        if (js_get_length32(ctx, (uint32_t *)&count, list) < 0)
            goto fail;
    } else {
        /* zipKeyed: own enumerable string keys, in property order. Atoms are
         * captured here so a later key mutation cannot change the tuple shape. */
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0, k;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail;
        count = (int)len;
        list = JS_NewArray(ctx);
        if (JS_IsException(list)) {
            JS_FreePropertyEnum(ctx, tab, len);
            goto fail;
        }
        keys = js_malloc(ctx, sizeof(JSAtom) * (count ? count : 1));
        if (!keys) {
            JS_FreePropertyEnum(ctx, tab, len);
            goto fail;
        }
        for (k = 0; k < len; k++) {
            JSValue v = JS_GetProperty(ctx, argv[0], tab[k].atom);
            if (JS_IsException(v)) {
                while (k > 0) JS_FreeAtom(ctx, keys[--k]);
                js_free(ctx, keys); keys = NULL;
                JS_FreePropertyEnum(ctx, tab, len);
                goto fail;
            }
            keys[k] = JS_DupAtom(ctx, tab[k].atom);
            if (JS_DefinePropertyValueUint32(ctx, list, k, v,
                                             JS_PROP_C_W_E) < 0) {
                do { JS_FreeAtom(ctx, keys[k]); } while (k-- > 0);
                js_free(ctx, keys); keys = NULL;
                JS_FreePropertyEnum(ctx, tab, len);
                goto fail;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }

    it = js_malloc(ctx, sizeof(*it) + 3 * (size_t)count * sizeof(it->slots[0]));
    if (!it)
        goto fail;
    it->count = count;
    it->live = count;
    it->mode = mode;
    it->running = FALSE;
    it->done = (count == 0);
    it->keys = keys;
    it->pad = pad;
    keys = NULL;    /* owned by `it` from here */
    pad = JS_UNDEFINED;
    for (i = 0; i < count * 3; i++)
        it->slots[i] = JS_UNDEFINED;
    for (i = 0; i < count; i++)
        ZIP_FIN(it, i) = JS_FALSE;

    for (i = 0; i < count; i++) {
        JSValue src = JS_GetPropertyUint32(ctx, list, i);
        JSValue iter, next;
        if (JS_IsException(src))
            goto fail_it;
        if (JS_IsString(src)) {
            JS_FreeValue(ctx, src);
            JS_ThrowTypeError(ctx, "Iterator.zip: an input must not be a string");
            goto fail_it;
        }
        iter = JS_GetIterator(ctx, src, /*is_async*/FALSE);
        JS_FreeValue(ctx, src);
        if (JS_IsException(iter))
            goto fail_it;
        next = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, iter);
            goto fail_it;
        }
        ZIP_ITER(it, i) = iter;
        ZIP_NEXT(it, i) = next;
    }
    JS_FreeValue(ctx, list);

    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_ZIP);
    if (JS_IsException(obj))
        goto fail_it;
    JS_SetOpaque(obj, it);
    return obj;

fail_it:
    /* `it` owns keys/pad by now, so free it as a unit -- closing whatever
     * inputs were already opened before giving up. */
    for (i = 0; i < count; i++) {
        if (!JS_IsUndefined(ZIP_ITER(it, i)))
            JS_IteratorClose(ctx, ZIP_ITER(it, i), TRUE);
        JS_FreeValue(ctx, ZIP_ITER(it, i));
        JS_FreeValue(ctx, ZIP_NEXT(it, i));
    }
    if (it->keys) {
        for (i = 0; i < it->count; i++)
            JS_FreeAtom(ctx, it->keys[i]);
        js_free(ctx, it->keys);
    }
    JS_FreeValue(ctx, it->pad);
    js_free(ctx, it);
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
fail:
    if (keys) {
        for (i = 0; i < count; i++)
            JS_FreeAtom(ctx, keys[i]);
        js_free(ctx, keys);
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, pad);
    return JS_EXCEPTION;
}

static JSValue js_iterator_from(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValueConst obj = argv[0];
    JSValue method, iter, wrapper;
    JSIteratorWrapData *it;
    int ret;

    if (!JS_IsObject(obj)) {
        if (!JS_IsString(obj))
            return JS_ThrowTypeError(ctx, "Iterator.from called on non-object");
    }
    method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (JS_IsNull(method) || JS_IsUndefined(method)) {
        iter = JS_DupValue(ctx, obj);
    } else {
        iter = JS_GetIterator2(ctx, obj, method);
        JS_FreeValue(ctx, method);
        if (JS_IsException(iter))
            return JS_EXCEPTION;
    }

    wrapper = JS_UNDEFINED;
    method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;

    ret = JS_OrdinaryIsInstanceOf(ctx, iter, ctx->iterator_ctor);
    if (ret < 0)
        goto fail;
    if (ret) {
        JS_FreeValue(ctx, method);
        return iter;
    }
    
    wrapper = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_WRAP);
    if (JS_IsException(wrapper))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it)
        goto fail;
    it->wrapped_iter = iter;
    it->wrapped_next = method;
    JS_SetOpaque(wrapper, it);
    return wrapper;

 fail:
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, wrapper);
    return JS_EXCEPTION;
}

typedef enum JSIteratorHelperKindEnum {
    JS_ITERATOR_HELPER_KIND_DROP,
    JS_ITERATOR_HELPER_KIND_EVERY,
    JS_ITERATOR_HELPER_KIND_FILTER,
    JS_ITERATOR_HELPER_KIND_FIND,
    JS_ITERATOR_HELPER_KIND_FLAT_MAP,
    JS_ITERATOR_HELPER_KIND_FOR_EACH,
    JS_ITERATOR_HELPER_KIND_MAP,
    JS_ITERATOR_HELPER_KIND_SOME,
    JS_ITERATOR_HELPER_KIND_TAKE,
    /* the lazy ext tier — bodies live in array.inc.c with the eager
     * ones they must agree with. Everything from here on dispatches through
     * js_iterator_lazy_next(); keep TAKE_WHILE first. */
    JS_ITERATOR_HELPER_KIND_TAKE_WHILE,
    JS_ITERATOR_HELPER_KIND_DROP_WHILE,
    JS_ITERATOR_HELPER_KIND_SCAN,
    JS_ITERATOR_HELPER_KIND_INTERSPERSE,
    JS_ITERATOR_HELPER_KIND_COMPACT,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY,
    JS_ITERATOR_HELPER_KIND_APERTURE,
    JS_ITERATOR_HELPER_KIND_SPLIT_EVERY,
    JS_ITERATOR_HELPER_KIND_ZIP_WITH,
    JS_ITERATOR_HELPER_KIND_PLUCK,
    JS_ITERATOR_HELPER_KIND_INIT,
    JS_ITERATOR_HELPER_KIND_TAIL,
    JS_ITERATOR_HELPER_KIND_UNIQUE,
    JS_ITERATOR_HELPER_KIND_TEE,
} JSIteratorHelperKindEnum;

#define JS_ITERATOR_HELPER_KIND_LAZY_FIRST JS_ITERATOR_HELPER_KIND_TAKE_WHILE

typedef struct JSIteratorHelperData {
    JSValue obj;
    JSValue next;
    JSValue func; // predicate (filter) or mapper (flatMap, map)
    JSValue inner; // innerValue (flatMap)
    JSValue acc;  // lazy tier: accumulator (scan), previous element (dropRepeats),
                  // buffered item (init), separator (intersperse), paired iterator (zipWith)
    JSValue aux;  // lazy tier: window buffer, seen-set, previous key, paired next method,
                  // shared fan-out state (tee)
    int64_t count; // limit (drop, take) or counter (filter, map, flatMap)
    int64_t count2; // lazy tier: window/chunk size, branch index
    JSIteratorHelperKindEnum kind : 8;
    uint8_t executing : 1;
    uint8_t done : 1;
    uint8_t started : 1; // lazy tier: the first element has been consumed
    uint8_t pending : 1; // lazy tier: `acc` holds a buffered item
} JSIteratorHelperData;

/* array.inc.c — the lazy tier's step function. Sets *pdone and returns an owned
 * value, or JS_EXCEPTION with *pdone set so the helper latches done. */
static JSValue js_iterator_lazy_next(JSContext *ctx, JSIteratorHelperData *it,
                                     int magic, int *pdone);

static JSValue js_create_iterator_helper(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    JSValueConst func;
    JSValue obj, method;
    int64_t count;
    JSIteratorHelperData *it;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    func = JS_UNDEFINED;
    count = 0;

    switch(magic) {
    case JS_ITERATOR_HELPER_KIND_DROP:
    case JS_ITERATOR_HELPER_KIND_TAKE:
        {
            JSValue v;
            double dlimit;
            v = JS_ToNumber(ctx, argv[0]);
            if (JS_IsException(v))
                goto fail;
            // Check for Infinity.
            if (JS_ToFloat64(ctx, &dlimit, v)) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            if (isnan(dlimit)) {
                JS_FreeValue(ctx, v);
                goto range_error;
            }
            if (!isfinite(dlimit)) {
                JS_FreeValue(ctx, v);
                if (dlimit < 0)
                    goto range_error;
                else
                    count = MAX_SAFE_INTEGER;
            } else {
                v = JS_ToIntegerFree(ctx, v);
                if (JS_IsException(v))
                    goto fail;
                if (JS_ToInt64Free(ctx, &count, v))
                    goto fail;
            }
            if (count < 0)
                goto range_error;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FILTER:
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP:
    case JS_ITERATOR_HELPER_KIND_MAP:
        {
            func = argv[0];
            if (check_function(ctx, func))
                goto fail;
        }
        break;
    default:
        abort();
        break;
    }

    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;
    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_HELPER);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it->kind = magic;
    it->obj = JS_DupValue(ctx, this_val);
    it->func = JS_DupValue(ctx, func);
    it->next = method;
    it->inner = JS_UNDEFINED;
    it->acc = JS_UNDEFINED;
    it->aux = JS_UNDEFINED;
    it->count = count;
    it->count2 = 0;
    it->executing = 0;
    it->done = 0;
    it->started = 0;
    it->pending = 0;
    JS_SetOpaque(obj, it);
    return obj;
range_error:
    JS_ThrowRangeError(ctx, "must be positive");
fail:
    JS_IteratorClose(ctx, this_val, TRUE);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_func(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSValue item, method, ret, func, index_val, r;
    JSValueConst args[2];
    int64_t idx;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    func = JS_UNDEFINED;
    method = JS_UNDEFINED;
    
    if (check_function(ctx, argv[0]))
        goto fail;
    func = JS_DupValue(ctx, argv[0]);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail_no_close;

    r = JS_UNDEFINED;

    switch(magic) {
    case JS_ITERATOR_HELPER_KIND_EVERY:
        {
            r = JS_TRUE;
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (!JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                        r = JS_EXCEPTION;
                    else
                        r = JS_FALSE;
                    break;
                }
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FIND:
        {
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret)) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                if (JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0) {
                        JS_FreeValue(ctx, item);
                        r = JS_EXCEPTION;
                    } else {
                        r = item;
                    }
                    break;
                }
                JS_FreeValue(ctx, item);
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FOR_EACH:
        {
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                JS_FreeValue(ctx, ret);
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    case JS_ITERATOR_HELPER_KIND_SOME:
        {
            r = JS_FALSE;
            for (idx = 0; /*empty*/; idx++) {
                item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    break;
                index_val = JS_NewInt64(ctx, idx);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (JS_ToBoolFree(ctx, ret)) {
                    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                        r = JS_EXCEPTION;
                    else
                        r = JS_TRUE;
                    break;
                }
                index_val = JS_UNDEFINED;
                ret = JS_UNDEFINED;
                item = JS_UNDEFINED;
            }
        }
        break;
    default:
        abort();
        break;
    }

    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return r;
 fail:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail_no_close:
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_reduce(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue item, method, ret, func, index_val, acc;
    JSValueConst args[3];
    int64_t idx;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    acc = JS_UNDEFINED;
    func = JS_UNDEFINED;
    method = JS_UNDEFINED;
    if (check_function(ctx, argv[0]))
        goto exception;
    func = JS_DupValue(ctx, argv[0]);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto exception;
    if (argc > 1) {
        acc = JS_DupValue(ctx, argv[1]);
        idx = 0;
    } else {
        acc = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(acc))
            goto exception_no_close;
        if (done) {
            JS_ThrowTypeError(ctx, "empty iterator");
            goto exception;
        }
        idx = 1;
    }
    for (/* empty */; /*empty*/; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception_no_close;
        if (done)
            break;
        index_val = JS_NewInt64(ctx, idx);
        args[0] = acc;
        args[1] = item;
        args[2] = index_val;
        ret = JS_Call(ctx, func, JS_UNDEFINED, countof(args), args);
        JS_FreeValue(ctx, item);
        JS_FreeValue(ctx, index_val);
        if (JS_IsException(ret))
            goto exception;
        JS_FreeValue(ctx, acc);
        acc = ret;
        index_val = JS_UNDEFINED;
        ret = JS_UNDEFINED;
        item = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return acc;
 exception:
    JS_IteratorClose(ctx, this_val, TRUE);
 exception_no_close:
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

/* Iterator.prototype.includes(searchElement)
 *
 * Consumes the iterator until a value is SameValueZero-equal to the argument.
 * SameValueZero (not ===) is what Array.prototype.includes uses, so NaN finds
 * NaN and +0 matches -0 -- the whole reason includes() exists next to
 * indexOf(). On a hit the iterator is CLOSED before returning, because it is
 * being abandoned mid-stream and may hold a resource. */
static JSValue js_iterator_proto_includes(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JSValue item, method;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    for (;;) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, method);
            return JS_EXCEPTION;
        }
        if (done) {
            JS_FreeValue(ctx, method);
            return JS_FALSE;
        }
        if (js_same_value_zero(ctx, item, argv[0])) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, method);
            if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                return JS_EXCEPTION;
            return JS_TRUE;
        }
        JS_FreeValue(ctx, item);
    }
}

/* Iterator.prototype[Symbol.dispose]()
 *
 * Lets an iterator be the subject of `using`, so abandoning a stream releases
 * whatever it holds at the end of the block instead of at the next GC. Defined
 * as IteratorClose, which is a no-op for an iterator with no return method --
 * so disposing a plain generator is safe and cheap. */
static JSValue js_iterator_proto_dispose(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_iterator_proto_toArray(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue item, method, result;
    int64_t idx;
    int done;

    result = JS_UNDEFINED;
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        goto exception;
    for (idx = 0; /*empty*/; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done)
            break;
        if (JS_DefinePropertyValueInt64(ctx, result, idx, item,
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            goto exception;
    }
    if (JS_SetProperty(ctx, result, JS_ATOM_length, JS_NewUint32(ctx, idx)) < 0)
        goto exception;
    JS_FreeValue(ctx, method);
    return result;
exception:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static JSValue js_iterator_proto_iterator(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    return JS_DupValue(ctx, this_val);
}

static JSValue js_iterator_proto_get_toStringTag(JSContext *ctx, JSValueConst this_val)
{
    return JS_AtomToString(ctx, JS_ATOM_Iterator);
}

static JSValue js_iterator_proto_set_toStringTag(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    int res;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_ITERATOR]))
        return JS_ThrowTypeError(ctx, "Cannot assign to read only property");
    res = JS_GetOwnProperty(ctx, NULL, this_val, JS_ATOM_Symbol_toStringTag);
    if (res < 0)
        return JS_EXCEPTION;
    if (res) {
        if (JS_SetProperty(ctx, this_val, JS_ATOM_Symbol_toStringTag, JS_DupValue(ctx, val)) < 0)
            return JS_EXCEPTION;
    } else {
        if (JS_DefinePropertyValue(ctx, this_val, JS_ATOM_Symbol_toStringTag, JS_DupValue(ctx, val), JS_PROP_C_W_E) < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* Iterator Helper */

static void js_iterator_helper_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData *it = p->u.iterator_helper_data;
    if (it) {
        JS_FreeValueRT(rt, it->obj);
        JS_FreeValueRT(rt, it->func);
        JS_FreeValueRT(rt, it->next);
        JS_FreeValueRT(rt, it->inner);
        JS_FreeValueRT(rt, it->acc);
        JS_FreeValueRT(rt, it->aux);
        js_free_rt(rt, it);
    }
}

static void js_iterator_helper_mark(JSRuntime *rt, JSValueConst val,
                                   JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData *it = p->u.iterator_helper_data;
    if (it) {
        JS_MarkValue(rt, it->obj, mark_func);
        JS_MarkValue(rt, it->func, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        JS_MarkValue(rt, it->inner, mark_func);
        JS_MarkValue(rt, it->acc, mark_func);
        JS_MarkValue(rt, it->aux, mark_func);
    }
}

static JSValue js_iterator_helper_next(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv,
                                      int *pdone, int magic)
{
    JSIteratorHelperData *it;
    JSValue ret;

    *pdone = FALSE;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_HELPER);
    if (!it)
        return JS_EXCEPTION;
    if (it->executing)
        return JS_ThrowTypeError(ctx, "cannot invoke a running iterator");
    if (it->done) {
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    it->executing = 1;

    switch (it->kind) {
    case JS_ITERATOR_HELPER_KIND_DROP:
        {
            JSValue item, method;
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
            while (it->count > 0) {
                it->count--;
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                if (JS_IsException(item)) {
                    JS_FreeValue(ctx, method);
                    goto fail_no_close;
                }
                JS_FreeValue(ctx, item);
                if (magic == GEN_MAGIC_RETURN)
                    *pdone = TRUE;
                if (*pdone) {
                    JS_FreeValue(ctx, method);
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }

            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            JS_FreeValue(ctx, method);
            if (JS_IsException(item))
                goto fail_no_close;
            ret = item;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FILTER:
        {
            JSValue item, method, selected, index_val;
            JSValueConst args[2];
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
        filter_again:
            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, method);
                goto fail_no_close;
            }
            if (*pdone || magic == GEN_MAGIC_RETURN) {
                JS_FreeValue(ctx, method);
                ret = item;
                goto done;
            }
            index_val = JS_NewInt64(ctx, it->count++);
            args[0] = item;
            args[1] = index_val;
            selected = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
            JS_FreeValue(ctx, index_val);
            if (JS_IsException(selected)) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, method);
                goto fail;
            }
            if (JS_ToBoolFree(ctx, selected)) {
                JS_FreeValue(ctx, method);
                ret = item;
                goto done;
            }
            JS_FreeValue(ctx, item);
            goto filter_again;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP:
        {
            JSValue item, method, index_val, iter;
            JSValueConst args[2];
        flat_map_again:
            if (JS_IsUndefined(it->inner)) {
                if (magic == GEN_MAGIC_NEXT) {
                    method = JS_DupValue(ctx, it->next);
                } else {
                    method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                    if (JS_IsException(method))
                        goto fail;
                    if (JS_IsUndefined(method) || JS_IsNull(method)) {
                        /* IteratorClose: underlying iterator has no 'return' */
                        *pdone = TRUE;
                        ret = JS_UNDEFINED;
                        goto done;
                    }
                }
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (*pdone || magic == GEN_MAGIC_RETURN) {
                    ret = item;
                    goto done;
                }
                index_val = JS_NewInt64(ctx, it->count++);
                args[0] = item;
                args[1] = index_val;
                ret = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, index_val);
                if (JS_IsException(ret))
                    goto fail;
                if (!JS_IsObject(ret)) {
                    JS_FreeValue(ctx, ret);
                    JS_ThrowTypeError(ctx, "not an object");
                    goto fail;
                }
                method = JS_GetProperty(ctx, ret, JS_ATOM_Symbol_iterator);
                if (JS_IsException(method)) {
                    JS_FreeValue(ctx, ret);
                    goto fail;
                }
                if (JS_IsNull(method) || JS_IsUndefined(method)) {
                    JS_FreeValue(ctx, method);
                    iter = ret;
                } else {
                    iter = JS_GetIterator2(ctx, ret, method);
                    JS_FreeValue(ctx, method);
                    JS_FreeValue(ctx, ret);
                    if (JS_IsException(iter))
                        goto fail;
                }

                it->inner = iter;
            }

            if (magic == GEN_MAGIC_NEXT)
                method = JS_GetProperty(ctx, it->inner, JS_ATOM_next);
            else
                method = JS_GetProperty(ctx, it->inner, JS_ATOM_return);
            if (JS_IsException(method)) {
            inner_fail:
                JS_IteratorClose(ctx, it->inner, FALSE);
                JS_FreeValue(ctx, it->inner);
                it->inner = JS_UNDEFINED;
                goto fail;
            }
            if (magic == GEN_MAGIC_RETURN && (JS_IsUndefined(method) || JS_IsNull(method))) {
                goto inner_end;
            } else {
                item = JS_IteratorNext(ctx, it->inner, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto inner_fail;
            }
            if (*pdone) {
            inner_end:
                *pdone = FALSE; // The outer iterator must continue.
                JS_IteratorClose(ctx, it->inner, FALSE);
                JS_FreeValue(ctx, it->inner);
                it->inner = JS_UNDEFINED;
                goto flat_map_again;
            }
            ret = item;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_MAP:
        {
            JSValue item, method, index_val;
            JSValueConst args[2];
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
                    /* IteratorClose: underlying iterator has no 'return' */
                    *pdone = TRUE;
                    ret = JS_UNDEFINED;
                    goto done;
                }
            }
            item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
            JS_FreeValue(ctx, method);
            if (JS_IsException(item))
                goto fail_no_close;
            if (*pdone || magic == GEN_MAGIC_RETURN) {
                ret = item;
                goto done;
            }
            index_val = JS_NewInt64(ctx, it->count++);
            args[0] = item;
            args[1] = index_val;
            ret = JS_Call(ctx, it->func, JS_UNDEFINED, countof(args), args);
            JS_FreeValue(ctx, index_val);
            JS_FreeValue(ctx, item);
            if (JS_IsException(ret))
                goto fail;
            goto done;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_TAKE:
        {
            JSValue item, method;
            if (it->count > 0) {
                if (magic == GEN_MAGIC_NEXT) {
                    method = JS_DupValue(ctx, it->next);
                } else {
                    method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                    if (JS_IsException(method))
                        goto fail;
                    if (JS_IsUndefined(method) || JS_IsNull(method)) {
                        /* IteratorClose: underlying iterator has no 'return' */
                        *pdone = TRUE;
                        ret = JS_UNDEFINED;
                        goto done;
                    }
                }
                it->count--;
                item = JS_IteratorNext(ctx, it->obj, method, 0, NULL, pdone);
                JS_FreeValue(ctx, method);
                if (JS_IsException(item))
                    goto fail_no_close;
                ret = item;
                goto done;
            }

            *pdone = TRUE;
            if (JS_IteratorClose(ctx, it->obj, FALSE))
                ret = JS_EXCEPTION;
            else
                ret = JS_UNDEFINED;
            goto done;
        }
        break;
    default:
        if (it->kind >= JS_ITERATOR_HELPER_KIND_LAZY_FIRST) {
            ret = js_iterator_lazy_next(ctx, it, magic, pdone);
            goto done;
        }
        abort();
    }

 done:
    it->done = magic == GEN_MAGIC_NEXT ? *pdone : 1;
    it->executing = 0;
    return ret;
 fail:
    /* close the iterator object, preserving pending exception */
    JS_IteratorClose(ctx, it->obj, TRUE);
 fail_no_close:
    ret = JS_EXCEPTION;
    goto done;
}

static const JSCFunctionListEntry js_iterator_funcs[] = {
    JS_CFUNC_DEF("concat", 0, js_iterator_concat ),
    JS_CFUNC_DEF("from", 1, js_iterator_from ),
    JS_CFUNC_MAGIC_DEF("zip", 1, js_iterator_zip, 0 ),
    JS_CFUNC_MAGIC_DEF("zipKeyed", 1, js_iterator_zip, 1 ),
};

static const JSCFunctionListEntry js_iterator_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("drop", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_DROP ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FILTER ),
    JS_CFUNC_MAGIC_DEF("flatMap", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FLAT_MAP ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_MAP ),
    JS_CFUNC_MAGIC_DEF("take", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_TAKE ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_EVERY ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FIND),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FOR_EACH ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_SOME ),
    JS_CFUNC_DEF("includes", 1, js_iterator_proto_includes ),
    JS_CFUNC_DEF("reduce", 1, js_iterator_proto_reduce ),
    JS_CFUNC_DEF("toArray", 0, js_iterator_proto_toArray ),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, js_iterator_proto_dispose ),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_iterator_proto_iterator ),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_iterator_proto_get_toStringTag, js_iterator_proto_set_toStringTag),
};

static const JSCFunctionListEntry js_iterator_helper_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_helper_next, GEN_MAGIC_NEXT ),
    JS_ITERATOR_NEXT_DEF("return", 0, js_iterator_helper_next, GEN_MAGIC_RETURN ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Helper", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_array_unscopables_funcs[] = {
    JS_PROP_BOOL_DEF("at", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("copyWithin", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("entries", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("fill", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("find", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findIndex", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findLast", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("findLastIndex", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("flat", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("flatMap", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("includes", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("keys", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toReversed", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toSorted", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("toSpliced", TRUE, JS_PROP_C_W_E),
    JS_PROP_BOOL_DEF("values", TRUE, JS_PROP_C_W_E),
};

/* ============================================================================
 * non-ECMAScript utilities, installed as `_`-prefixed
 * NON-ENUMERABLE methods on Array.prototype. They
 * operate on any array-like `this` via JS_ToObject + js_get_length64, exactly
 * like the standard methods; nothing native escapes. Phase 1, batch 1.
 * ========================================================================== */

static uint64_t xorshift64star(uint64_t *pstate); /* the Math.random PRNG, below */
/* the Map/Set value hasher + -0 normaliser (symbol.inc.c, later in the unity build) */
static uint32_t map_hash_key(JSValueConst key, int hash_bits);
static JSValueConst map_normalize_key_const(JSContext *ctx, JSValueConst key);

/* read element i of a (possibly array-like) object; *pval is owned, set to
 * JS_UNDEFINED for a missing index. returns -1 (exception) or 0. */
static int js_array_ext_getel(JSContext *ctx, JSValueConst obj, int64_t i,
                              JSValue *pval)
{
    int present = JS_TryGetPropertyInt64(ctx, obj, i, pval);
    if (present < 0)
        return -1;
    if (!present)
        *pval = JS_UNDEFINED;
    return 0;
}

