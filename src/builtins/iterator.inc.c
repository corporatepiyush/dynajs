static JSValue js_iterator_constructor_getset(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic,
    JSValue* func_data)
{
    int ret;

    if (argc > 0) {
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

static JSValue js_iterator_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSObject* p;

    if (JS_TAG_OBJECT != JS_VALUE_GET_TAG(new_target))
        return JS_ThrowTypeError(ctx, "constructor requires 'new'");
    p = JS_VALUE_GET_OBJ(new_target);
    if (p->class_id == JS_CLASS_C_FUNCTION && p->u.cfunc.c_function.generic == js_iterator_constructor) {
        return JS_ThrowTypeError(ctx, "abstract class not constructable");
    }
    return js_create_from_ctor(ctx, new_target, JS_CLASS_ITERATOR);
}

typedef struct JSIteratorConcatData {
    int index, count;
    BOOL running;
    JSValue iter, next, values[];
} JSIteratorConcatData;

static void js_iterator_concat_finalizer(JSRuntime* rt, JSValue val)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData* it = p->u.iterator_concat_data;
    if (it) {
        JS_FreeValueRT(rt, it->iter);
        JS_FreeValueRT(rt, it->next);
        for (int i = it->index; i < it->count; i++)
            JS_FreeValueRT(rt, it->values[i]);
        js_free_rt(rt, it);
    }
}

static void js_iterator_concat_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorConcatData* it = p->u.iterator_concat_data;
    if (it) {
        JS_MarkValue(rt, it->iter, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        for (int i = it->index; i < it->count; i++)
            JS_MarkValue(rt, it->values[i], mark_func);
    }
}

static JSValue js_iterator_concat_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int* pdone, int magic)
{
    JSValue iter, item, next, val, *obj, *meth, ret;
    JSIteratorConcatData* it;
    int done;

    *pdone = FALSE;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_CONCAT);
    if (!it)
        return JS_EXCEPTION;
    if (it->running)
        return JS_ThrowTypeError(ctx, "already running");

    it->running = TRUE;
    for (;;) {
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

static JSValue js_iterator_concat_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSIteratorConcatData* it;
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
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_concat_next, 0),
    JS_CFUNC_DEF("return", 0, js_iterator_concat_return),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Concat", JS_PROP_CONFIGURABLE),
};

static JSValue js_iterator_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSIteratorConcatData* it;
    JSValue obj, method;

    it = js_malloc(ctx, sizeof(*it) + 2 * argc * sizeof(it->values[0]));
    if (!it)
        return JS_EXCEPTION;
    it->running = FALSE;
    it->index = 0;
    it->count = 0;
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        JSValueConst elt = argv[i];
        if (!JS_IsObject(elt)) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            goto fail;
        }
        method = JS_GetProperty(ctx, elt, JS_ATOM_Symbol_iterator);
        if (JS_IsException(method))
            goto fail;
        if (!JS_IsFunction(ctx, method)) {
            JS_ThrowTypeError(ctx, "not a function");
            JS_FreeValue(ctx, method);
            goto fail;
        }
        it->values[it->count++] = JS_DupValue(ctx, elt);
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

typedef enum {
    ZIP_SUSPENDED_START,
    ZIP_SUSPENDED_YIELD,
    ZIP_RUNNING,
    ZIP_COMPLETED,
} JSIteratorZipState;

typedef struct JSIteratorZipData {
    int count;
    int mode;
    int state;
    JSAtom* keys;
    uint8_t* open;
    JSValue* scratch;
    JSValue slots[];
} JSIteratorZipData;

#define ZIP_ITER(it, i) ((it)->slots[(i) * 3])
#define ZIP_NEXT(it, i) ((it)->slots[(i) * 3 + 1])
#define ZIP_PAD(it, i) ((it)->slots[(i) * 3 + 2])

static void js_iterator_zip_finalizer(JSRuntime* rt, JSValue val)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorZipData* it = p->u.iterator_zip_data;
    if (it) {
        int n = it->count * 3;
        for (int i = 0; i < n; i++)
            JS_FreeValueRT(rt, it->slots[i]);
        if (it->keys) {
            for (int i = 0; i < it->count; i++)
                JS_FreeAtomRT(rt, it->keys[i]);
            js_free_rt(rt, it->keys);
        }
        if (it->scratch) {
            for (int i = 0; i < it->count; i++)
                JS_FreeValueRT(rt, it->scratch[i]);
            js_free_rt(rt, it->scratch);
        }
        js_free_rt(rt, it->open);
        js_free_rt(rt, it);
    }
}

static void js_iterator_zip_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorZipData* it = p->u.iterator_zip_data;
    if (it) {
        int n = it->count * 3;
        for (int i = 0; i < n; i++)
            JS_MarkValue(rt, it->slots[i], mark_func);
        if (it->scratch) {
            for (int i = 0; i < it->count; i++)
                JS_MarkValue(rt, it->scratch[i], mark_func);
        }
    }
}

static void js_zip_data_free(JSContext* ctx, JSIteratorZipData* it)
{
    for (int i = 0; i < it->count * 3; i++)
        JS_FreeValue(ctx, it->slots[i]);
    if (it->keys) {
        for (int i = 0; i < it->count; i++)
            JS_FreeAtom(ctx, it->keys[i]);
        js_free(ctx, it->keys);
    }
    if (it->scratch) {
        for (int i = 0; i < it->count; i++)
            JS_FreeValue(ctx, it->scratch[i]);
        js_free(ctx, it->scratch);
    }
    js_free(ctx, it->open);
    js_free(ctx, it);
}

static int js_zip_close_list(JSContext* ctx, JSValue* iters, int n, BOOL is_throw)
{
    for (int i = n - 1; i >= 0; i--) {
        if (JS_IsUndefined(iters[i]) || JS_IsNull(iters[i]))
            continue;
        if (is_throw)
            JS_IteratorClose(ctx, iters[i], TRUE);
        else if (JS_IteratorClose(ctx, iters[i], FALSE) < 0)
            is_throw = TRUE;
    }
    return is_throw ? -1 : 0;
}

static int js_zip_close_open(JSContext* ctx, JSIteratorZipData* it, BOOL is_throw)
{
    for (int i = it->count - 1; i >= 0; i--) {
        if (!it->open[i])
            continue;
        it->open[i] = 0;
        if (is_throw)
            JS_IteratorClose(ctx, ZIP_ITER(it, i), TRUE);
        else if (JS_IteratorClose(ctx, ZIP_ITER(it, i), FALSE) < 0)
            is_throw = TRUE;
    }
    return is_throw ? -1 : 0;
}

static JSValue js_zip_finish(JSContext* ctx, JSIteratorZipData* it, JSValue* results)
{
    JSValue obj;
    int i;

    if (it->keys) {
        obj = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(obj))
            goto fail;
        for (i = 0; i < it->count; i++) {
            if (JS_DefinePropertyValue(ctx, obj, it->keys[i], results[i], JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
                results[i] = JS_UNDEFINED;
                JS_FreeValue(ctx, obj);
                goto fail;
            }
            results[i] = JS_UNDEFINED;
        }
        return obj;
    }
    obj = JS_NewArrayFrom(ctx, it->count, results);
    for (i = 0; i < it->count; i++)
        results[i] = JS_UNDEFINED;
    return obj;
fail:
    for (i = 0; i < it->count; i++) {
        JS_FreeValue(ctx, results[i]);
        results[i] = JS_UNDEFINED;
    }
    return JS_EXCEPTION;
}

static JSValue js_iterator_zip_step(JSContext* ctx, JSValueConst this_val,
    JSIteratorZipData* it, int* pdone, int magic)
{
    JSValue* results = NULL;
    JSValue ret;
    int i, k, done, open_left;

    *pdone = FALSE;
    if (it->state == ZIP_RUNNING)
        return JS_ThrowTypeError(ctx, "cannot invoke a running iterator");
    if (magic == GEN_MAGIC_RETURN) {
        int r = 0;
        if (it->state == ZIP_SUSPENDED_START) {
            it->state = ZIP_COMPLETED;
            r = js_zip_close_open(ctx, it, FALSE);
        } else if (it->state == ZIP_SUSPENDED_YIELD) {
            it->state = ZIP_RUNNING;
            r = js_zip_close_open(ctx, it, FALSE);
            it->state = ZIP_COMPLETED;
        }
        if (r < 0)
            return JS_EXCEPTION;
        *pdone = TRUE;
        return JS_UNDEFINED;
    }
    if (it->state == ZIP_COMPLETED || it->count == 0) {
        it->state = ZIP_COMPLETED;
        *pdone = TRUE;
        return JS_UNDEFINED;
    }
    it->state = ZIP_RUNNING;
    results = it->scratch;
    for (i = 0; i < it->count; i++) {
        JSValue v;
        if (JS_IsNull(ZIP_ITER(it, i))) {
            results[i] = JS_DupValue(ctx, ZIP_PAD(it, i));
            continue;
        }
        v = JS_IteratorNext(ctx, ZIP_ITER(it, i), ZIP_NEXT(it, i), 0, NULL, &done);
        if (JS_IsException(v)) {
            it->open[i] = 0;
            goto fail_throw;
        }
        if (!done) {
            results[i] = v;
            continue;
        }
        it->open[i] = 0;
        if (it->mode == 0) {
            it->state = ZIP_COMPLETED;
            if (js_zip_close_open(ctx, it, FALSE) < 0)
                goto fail_done;
            goto finished;
        } else if (it->mode == 2) {
            if (i != 0) {
                JS_ThrowTypeError(ctx, "Iterator.zip: inputs have different lengths");
                goto fail_throw;
            }
            for (k = 1; k < it->count; k++) {
                JSValue r = JS_IteratorNext2(ctx, ZIP_ITER(it, k), ZIP_NEXT(it, k), 0, NULL, &done);
                if (JS_IsException(r)) {
                    it->open[k] = 0;
                    goto fail_throw;
                }
                if (done == 2) {
                    JSValue dv = JS_GetProperty(ctx, r, JS_ATOM_done);
                    JS_FreeValue(ctx, r);
                    if (JS_IsException(dv)) {
                        it->open[k] = 0;
                        goto fail_throw;
                    }
                    done = JS_ToBoolFree(ctx, dv);
                } else {
                    JS_FreeValue(ctx, r);
                }
                if (done) {
                    it->open[k] = 0;
                } else {
                    JS_ThrowTypeError(ctx, "Iterator.zip: inputs have different lengths");
                    goto fail_throw;
                }
            }
            it->state = ZIP_COMPLETED;
            goto finished;
        } else {
            open_left = 0;
            for (k = 0; k < it->count; k++)
                open_left |= it->open[k];
            if (!open_left) {
                it->state = ZIP_COMPLETED;
                goto finished;
            }
            JS_FreeValue(ctx, ZIP_ITER(it, i));
            ZIP_ITER(it, i) = JS_NULL;
            results[i] = JS_DupValue(ctx, ZIP_PAD(it, i));
        }
    }
    ret = js_zip_finish(ctx, it, results);
    if (JS_IsException(ret)) {
        it->state = ZIP_COMPLETED;
        js_zip_close_open(ctx, it, TRUE);
        return JS_EXCEPTION;
    }
    it->state = ZIP_SUSPENDED_YIELD;
    return ret;
finished:
    for (i = 0; i < it->count; i++) {
        JS_FreeValue(ctx, results[i]);
        results[i] = JS_UNDEFINED;
    }
    *pdone = TRUE;
    return JS_UNDEFINED;
fail_throw:
    it->state = ZIP_COMPLETED;
    js_zip_close_open(ctx, it, TRUE);
fail_done:
    for (i = 0; i < it->count; i++) {
        JS_FreeValue(ctx, results[i]);
        results[i] = JS_UNDEFINED;
    }
    return JS_EXCEPTION;
}

static JSValue js_zip_flattenable(JSContext* ctx, JSValueConst obj, JSValue* pnext)
{
    JSValue method, iter, next;

    if (!JS_IsObject(obj))
        return JS_ThrowTypeError(ctx, "Iterator.zip: an input is not an object");
    method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        iter = JS_DupValue(ctx, obj);
    } else {
        if (!JS_IsFunction(ctx, method)) {
            JS_FreeValue(ctx, method);
            return JS_ThrowTypeError(ctx, "Iterator.zip: Symbol.iterator is not a function");
        }
        iter = JS_GetIterator2(ctx, obj, method);
        JS_FreeValue(ctx, method);
        if (JS_IsException(iter))
            return JS_EXCEPTION;
    }
    if (!JS_IsObject(iter)) {
        JS_FreeValue(ctx, iter);
        return JS_ThrowTypeErrorNotAnObject(ctx);
    }
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, iter);
        return JS_EXCEPTION;
    }
    *pnext = next;
    return iter;
}

typedef struct {
    JSValue* iters;
    JSValue* nexts;
    JSAtom* keys;
    int count, size;
} JSZipList;

static int js_zip_list_push(JSContext* ctx, JSZipList* l, JSValue iter, JSValue next, JSAtom key)
{
    if (l->count >= l->size) {
        int ns = l->size ? l->size * 2 : 8;
        JSValue* a = js_realloc(ctx, l->iters, sizeof(JSValue) * ns);
        JSValue* b;
        JSAtom* c;
        if (!a)
            return -1;
        l->iters = a;
        b = js_realloc(ctx, l->nexts, sizeof(JSValue) * ns);
        if (!b)
            return -1;
        l->nexts = b;
        c = js_realloc(ctx, l->keys, sizeof(JSAtom) * ns);
        if (!c)
            return -1;
        l->keys = c;
        l->size = ns;
    }
    l->iters[l->count] = iter;
    l->nexts[l->count] = next;
    l->keys[l->count] = key;
    l->count++;
    return 0;
}

static void js_zip_list_free(JSContext* ctx, JSZipList* l, BOOL with_keys)
{
    for (int i = 0; i < l->count; i++) {
        JS_FreeValue(ctx, l->iters[i]);
        JS_FreeValue(ctx, l->nexts[i]);
        if (with_keys)
            JS_FreeAtom(ctx, l->keys[i]);
    }
    js_free(ctx, l->iters);
    js_free(ctx, l->nexts);
    js_free(ctx, l->keys);
}

static JSValue js_iterator_zip(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValueConst iterables = argv[0];
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue mode_v = JS_UNDEFINED, pad_opt = JS_UNDEFINED, obj;
    JSZipList l = { 0 };
    JSIteratorZipData* it = NULL;
    JSValue* pad = NULL;
    int mode = 0, i;

    if (!JS_IsObject(iterables))
        return JS_ThrowTypeError(ctx, "Iterator.zip: iterables must be an object");
    if (!JS_IsUndefined(options)) {
        if (!JS_IsObject(options))
            return JS_ThrowTypeError(ctx, "Iterator.zip: options must be an object");
        mode_v = JS_GetPropertyStr(ctx, options, "mode");
        if (JS_IsException(mode_v))
            return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(mode_v)) {
        const char* m;
        if (!JS_IsString(mode_v)) {
            JS_FreeValue(ctx, mode_v);
            return JS_ThrowTypeError(ctx, "Iterator.zip: mode must be \"shortest\", \"longest\" or \"strict\"");
        }
        m = JS_ToCString(ctx, mode_v);
        JS_FreeValue(ctx, mode_v);
        if (!m)
            return JS_EXCEPTION;
        if (!strcmp(m, "shortest"))
            mode = 0;
        else if (!strcmp(m, "longest"))
            mode = 1;
        else if (!strcmp(m, "strict"))
            mode = 2;
        else
            mode = -1;
        JS_FreeCString(ctx, m);
        if (mode < 0)
            return JS_ThrowTypeError(ctx, "Iterator.zip: mode must be \"shortest\", \"longest\" or \"strict\"");
    }
    if (mode == 1 && !JS_IsUndefined(options)) {
        pad_opt = JS_GetPropertyStr(ctx, options, "padding");
        if (JS_IsException(pad_opt))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(pad_opt) && !JS_IsObject(pad_opt)) {
            JS_FreeValue(ctx, pad_opt);
            return JS_ThrowTypeError(ctx, "Iterator.zip: padding must be an object");
        }
    }

    if (magic == 0) {
        JSValue input, input_next;
        input = JS_GetIterator(ctx, iterables, FALSE);
        if (JS_IsException(input))
            goto fail;
        input_next = JS_GetProperty(ctx, input, JS_ATOM_next);
        if (JS_IsException(input_next)) {
            JS_FreeValue(ctx, input);
            goto fail;
        }
        for (;;) {
            JSValue v, iter, next;
            int done;
            v = JS_IteratorNext(ctx, input, input_next, 0, NULL, &done);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, input);
                JS_FreeValue(ctx, input_next);
                js_zip_close_list(ctx, l.iters, l.count, TRUE);
                goto fail;
            }
            if (done)
                break;
            iter = js_zip_flattenable(ctx, v, &next);
            JS_FreeValue(ctx, v);
            if (JS_IsException(iter) || js_zip_list_push(ctx, &l, iter, next, JS_ATOM_NULL) < 0) {
                if (!JS_IsException(iter)) {
                    JS_FreeValue(ctx, iter);
                    JS_FreeValue(ctx, next);
                }
                js_zip_close_list(ctx, l.iters, l.count, TRUE);
                JS_IteratorClose(ctx, input, TRUE);
                JS_FreeValue(ctx, input);
                JS_FreeValue(ctx, input_next);
                goto fail;
            }
        }
        JS_FreeValue(ctx, input);
        JS_FreeValue(ctx, input_next);
    } else {
        JSPropertyEnum* tab = NULL;
        uint32_t len = 0, k;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, iterables, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
            goto fail;
        for (k = 0; k < len; k++) {
            JSPropertyDescriptor desc;
            JSValue v, iter, next;
            int r = JS_GetOwnProperty(ctx, &desc, iterables, tab[k].atom);
            if (r < 0)
                goto fail_keys;
            if (!r)
                continue;
            js_free_desc(ctx, &desc);
            if (!(desc.flags & JS_PROP_ENUMERABLE))
                continue;
            v = JS_GetProperty(ctx, iterables, tab[k].atom);
            if (JS_IsException(v))
                goto fail_keys;
            if (JS_IsUndefined(v))
                continue;
            iter = js_zip_flattenable(ctx, v, &next);
            JS_FreeValue(ctx, v);
            if (JS_IsException(iter))
                goto fail_keys;
            if (js_zip_list_push(ctx, &l, iter, next, JS_DupAtom(ctx, tab[k].atom)) < 0) {
                JS_FreeValue(ctx, iter);
                JS_FreeValue(ctx, next);
                goto fail_keys;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
        goto keys_done;
    fail_keys:
        JS_FreePropertyEnum(ctx, tab, len);
        js_zip_close_list(ctx, l.iters, l.count, TRUE);
        goto fail;
    keys_done:;
    }

    pad = js_mallocz(ctx, sizeof(JSValue) * (l.count ? l.count : 1));
    if (!pad)
        goto fail_close;
    for (i = 0; i < l.count; i++)
        pad[i] = JS_UNDEFINED;
    if (mode == 1 && !JS_IsUndefined(pad_opt)) {
        if (magic == 0) {
            JSValue piter, pnext;
            BOOL using_iter = TRUE;
            piter = JS_GetIterator(ctx, pad_opt, FALSE);
            if (JS_IsException(piter))
                goto fail_close;
            pnext = JS_GetProperty(ctx, piter, JS_ATOM_next);
            if (JS_IsException(pnext)) {
                JS_FreeValue(ctx, piter);
                goto fail_close;
            }
            for (i = 0; i < l.count && using_iter; i++) {
                int done;
                JSValue v = JS_IteratorNext(ctx, piter, pnext, 0, NULL, &done);
                if (JS_IsException(v)) {
                    JS_FreeValue(ctx, piter);
                    JS_FreeValue(ctx, pnext);
                    goto fail_close;
                }
                if (done)
                    using_iter = FALSE;
                else
                    pad[i] = v;
            }
            if (using_iter && JS_IteratorClose(ctx, piter, FALSE) < 0) {
                JS_FreeValue(ctx, piter);
                JS_FreeValue(ctx, pnext);
                goto fail_close;
            }
            JS_FreeValue(ctx, piter);
            JS_FreeValue(ctx, pnext);
        } else {
            for (i = 0; i < l.count; i++) {
                JSValue v = JS_GetProperty(ctx, pad_opt, l.keys[i]);
                if (JS_IsException(v))
                    goto fail_close;
                pad[i] = v;
            }
        }
    }

    it = js_mallocz(ctx, sizeof(*it) + 3 * (size_t)l.count * sizeof(JSValue));
    if (!it)
        goto fail_close;
    it->open = js_mallocz(ctx, l.count ? l.count : 1);
    if (!it->open)
        goto fail_close;
    it->scratch = js_malloc(ctx, sizeof(JSValue) * (l.count ? l.count : 1));
    if (!it->scratch)
        goto fail_close;
    for (i = 0; i < l.count; i++)
        it->scratch[i] = JS_UNDEFINED;
    it->count = l.count;
    it->mode = mode;
    it->state = ZIP_SUSPENDED_START;
    for (i = 0; i < l.count; i++) {
        ZIP_ITER(it, i) = l.iters[i];
        ZIP_NEXT(it, i) = l.nexts[i];
        ZIP_PAD(it, i) = pad[i];
        it->open[i] = 1;
    }
    if (magic == 1) {
        it->keys = l.keys;
        l.keys = NULL;
    }
    js_free(ctx, l.iters);
    js_free(ctx, l.nexts);
    js_free(ctx, l.keys);
    js_free(ctx, pad);
    JS_FreeValue(ctx, pad_opt);

    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_ZIP);
    if (JS_IsException(obj)) {
        js_zip_close_open(ctx, it, TRUE);
        js_zip_data_free(ctx, it);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, it);
    return obj;

fail_close:
    js_zip_close_list(ctx, l.iters, l.count, TRUE);
fail:
    if (it) {
        js_free(ctx, it->scratch);
        js_free(ctx, it->open);
        js_free(ctx, it);
    }
    if (pad) {
        for (i = 0; i < l.count; i++)
            JS_FreeValue(ctx, pad[i]);
        js_free(ctx, pad);
    }
    js_zip_list_free(ctx, &l, magic == 1);
    JS_FreeValue(ctx, pad_opt);
    return JS_EXCEPTION;
}

static JSValue js_iterator_from(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValueConst obj = argv[0];
    JSValue method, iter, wrapper;
    JSIteratorWrapData* it;
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
    JSValue func;
    JSValue inner;
    JSValue acc;
    JSValue aux;
    int64_t count;
    int64_t count2;
    JSIteratorHelperKindEnum kind : 8;
    uint8_t executing : 1;
    uint8_t done : 1;
    uint8_t started : 1;
    uint8_t pending : 1;
} JSIteratorHelperData;

static JSValue js_iterator_lazy_next(JSContext* ctx, JSIteratorHelperData* it,
    int magic, int* pdone);

static JSValue js_create_iterator_helper(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValueConst func;
    JSValue obj, method;
    int64_t count;
    JSIteratorHelperData* it;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    func = JS_UNDEFINED;
    count = 0;

    switch (magic) {
    case JS_ITERATOR_HELPER_KIND_DROP:
    case JS_ITERATOR_HELPER_KIND_TAKE: {
        JSValue v;
        double dlimit;
        v = JS_ToNumber(ctx, argv[0]);
        if (JS_IsException(v))
            goto fail;
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
    } break;
    case JS_ITERATOR_HELPER_KIND_FILTER:
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP:
    case JS_ITERATOR_HELPER_KIND_MAP: {
        func = argv[0];
        if (check_function(ctx, func))
            goto fail;
    } break;
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

static JSValue js_iterator_proto_func(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
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

    switch (magic) {
    case JS_ITERATOR_HELPER_KIND_EVERY: {
        r = JS_TRUE;
        for (idx = 0;; idx++) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_FIND: {
        for (idx = 0;; idx++) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_FOR_EACH: {
        for (idx = 0;; idx++) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_SOME: {
        r = JS_FALSE;
        for (idx = 0;; idx++) {
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
    } break;
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

static JSValue js_iterator_proto_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
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
        goto exception_no_close;
    if (argc > 1) {
        acc = JS_DupValue(ctx, argv[1]);
        idx = 0;
    } else {
        acc = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(acc))
            goto exception_no_close;
        if (done) {
            JS_ThrowTypeError(ctx, "empty iterator");
            goto exception_no_close;
        }
        idx = 1;
    }
    for (;; idx++) {
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

static JSValue js_iterator_proto_includes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
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

static JSValue js_iterator_proto_dispose(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_iterator_proto_toArray(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
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
    for (idx = 0;; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done)
            break;
        if (JS_DefinePropertyValueInt64(ctx, result, idx, item,
                JS_PROP_C_W_E | JS_PROP_THROW)
            < 0)
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

static JSValue js_iterator_proto_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return JS_DupValue(ctx, this_val);
}

static JSValue js_iterator_proto_get_toStringTag(JSContext* ctx, JSValueConst this_val)
{
    return JS_AtomToString(ctx, JS_ATOM_Iterator);
}

static JSValue js_iterator_proto_set_toStringTag(JSContext* ctx, JSValueConst this_val, JSValueConst val)
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

static void js_iterator_helper_finalizer(JSRuntime* rt, JSValue val)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData* it = p->u.iterator_helper_data;
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

static void js_iterator_helper_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSIteratorHelperData* it = p->u.iterator_helper_data;
    if (it) {
        JS_MarkValue(rt, it->obj, mark_func);
        JS_MarkValue(rt, it->func, mark_func);
        JS_MarkValue(rt, it->next, mark_func);
        JS_MarkValue(rt, it->inner, mark_func);
        JS_MarkValue(rt, it->acc, mark_func);
        JS_MarkValue(rt, it->aux, mark_func);
    }
}

static JSValue js_iterator_helper_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int* pdone, int magic)
{
    JSIteratorHelperData* it;
    JSValue ret;

    *pdone = FALSE;

    it = JS_GetOpaque(this_val, JS_CLASS_ITERATOR_HELPER);
    if (unlikely(!it)) {
        JSIteratorZipData* zd = JS_GetOpaque(this_val, JS_CLASS_ITERATOR_ZIP);
        if (zd)
            return js_iterator_zip_step(ctx, this_val, zd, pdone, magic);
        it = JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_HELPER);
        if (!it)
            return JS_EXCEPTION;
    }
    if (it->executing)
        return JS_ThrowTypeError(ctx, "cannot invoke a running iterator");
    if (it->done) {
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    it->executing = 1;

    switch ((int)(it->kind)) {
    case JS_ITERATOR_HELPER_KIND_DROP: {
        JSValue item, method;
        if (magic == GEN_MAGIC_NEXT) {
            method = JS_DupValue(ctx, it->next);
        } else {
            method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
            if (JS_IsException(method))
                goto fail;
            if (JS_IsUndefined(method) || JS_IsNull(method)) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_FILTER: {
        JSValue item, method, selected, index_val;
        JSValueConst args[2];
        if (magic == GEN_MAGIC_NEXT) {
            method = JS_DupValue(ctx, it->next);
        } else {
            method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
            if (JS_IsException(method))
                goto fail;
            if (JS_IsUndefined(method) || JS_IsNull(method)) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_FLAT_MAP: {
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
            *pdone = FALSE;
            JS_IteratorClose(ctx, it->inner, FALSE);
            JS_FreeValue(ctx, it->inner);
            it->inner = JS_UNDEFINED;
            goto flat_map_again;
        }
        ret = item;
        goto done;
    } break;
    case JS_ITERATOR_HELPER_KIND_MAP: {
        JSValue item, method, index_val;
        JSValueConst args[2];
        if (magic == GEN_MAGIC_NEXT) {
            method = JS_DupValue(ctx, it->next);
        } else {
            method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
            if (JS_IsException(method))
                goto fail;
            if (JS_IsUndefined(method) || JS_IsNull(method)) {
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
    } break;
    case JS_ITERATOR_HELPER_KIND_TAKE: {
        JSValue item, method;
        if (it->count > 0) {
            if (magic == GEN_MAGIC_NEXT) {
                method = JS_DupValue(ctx, it->next);
            } else {
                method = JS_GetProperty(ctx, it->obj, JS_ATOM_return);
                if (JS_IsException(method))
                    goto fail;
                if (JS_IsUndefined(method) || JS_IsNull(method)) {
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
    } break;
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
    JS_IteratorClose(ctx, it->obj, TRUE);
fail_no_close:
    ret = JS_EXCEPTION;
    goto done;
}

static const JSCFunctionListEntry js_iterator_funcs[] = {
    JS_CFUNC_DEF("concat", 0, js_iterator_concat),
    JS_CFUNC_DEF("from", 1, js_iterator_from),
    JS_CFUNC_MAGIC_DEF("zip", 1, js_iterator_zip, 0),
    JS_CFUNC_MAGIC_DEF("zipKeyed", 1, js_iterator_zip, 1),
};

static const JSCFunctionListEntry js_iterator_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("drop", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_DROP),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FILTER),
    JS_CFUNC_MAGIC_DEF("flatMap", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_FLAT_MAP),
    JS_CFUNC_MAGIC_DEF("map", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_MAP),
    JS_CFUNC_MAGIC_DEF("take", 1, js_create_iterator_helper, JS_ITERATOR_HELPER_KIND_TAKE),
    JS_CFUNC_MAGIC_DEF("every", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_EVERY),
    JS_CFUNC_MAGIC_DEF("find", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FIND),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_FOR_EACH),
    JS_CFUNC_MAGIC_DEF("some", 1, js_iterator_proto_func, JS_ITERATOR_HELPER_KIND_SOME),
    JS_CFUNC_DEF("includes", 1, js_iterator_proto_includes),
    JS_CFUNC_DEF("reduce", 1, js_iterator_proto_reduce),
    JS_CFUNC_DEF("toArray", 0, js_iterator_proto_toArray),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, js_iterator_proto_dispose),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_iterator_proto_iterator),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_iterator_proto_get_toStringTag, js_iterator_proto_set_toStringTag),
};

static const JSCFunctionListEntry js_iterator_helper_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_iterator_helper_next, GEN_MAGIC_NEXT),
    JS_ITERATOR_NEXT_DEF("return", 0, js_iterator_helper_next, GEN_MAGIC_RETURN),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Iterator Helper", JS_PROP_CONFIGURABLE),
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

static uint64_t xorshift64star(uint64_t* pstate);
static uint32_t map_hash_key(JSRuntime* rt, JSValueConst key, int hash_bits, BOOL strong);
static JSValueConst map_normalize_key_const(JSContext* ctx, JSValueConst key);

static int js_array_ext_getel(JSContext* ctx, JSValueConst obj, int64_t i,
    JSValue* pval)
{
    int present = JS_TryGetPropertyInt64(ctx, obj, i, pval);
    if (present < 0)
        return -1;
    if (!present)
        *pval = JS_UNDEFINED;
    return 0;
}
