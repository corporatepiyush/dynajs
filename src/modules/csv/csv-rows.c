#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

typedef struct {
    char* path;
    int64_t offset;
    int64_t batch;
    uint8_t delim, quote;
    int strict;
    int done;
    int loaded;
    CsvView t;
} csv_rows_t;

static const char* const opts_rows_keys[] = { "batch", "delimiter", "quote", "strict" };

void csv_rows_finalizer(JSRuntime* rt, JSValue val)
{
    csv_rows_t* it = (csv_rows_t*)JS_GetOpaque(val, csv_rows_class_id);
    (void)rt;
    if (!it)
        return;
    if (it->loaded)
        csv_view_free(&it->t);
    free(it->path);
    free(it);
}

static void csv_rows_release(csv_rows_t* it)
{
    it->done = 1;
    if (it->loaded) {
        csv_view_free(&it->t);
        it->loaded = 0;
    }
}

static JSValue csv_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue csv_done_value(JSContext* ctx)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "value", JS_UNDEFINED, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "done", JS_TRUE, JS_PROP_C_W_E);
    return o;
}

static JSValue csv_rows_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    csv_rows_t* it = (csv_rows_t*)JS_GetOpaque2(ctx, this_val, csv_rows_class_id);
    CsvView* t;
    JSValue hjs, rjs, val, res;
    size_t total, start, end;
    size_t c, ncols;
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    if (it->done)
        return csv_promise_resolved(ctx, csv_done_value(ctx));
    if (!it->loaded) {
        memset(&it->t, 0, sizeof it->t);
        if (csv_view_load(ctx, it->path, &it->t, it->strict, it->delim, it->quote, 0)) {
            JSValue exc = JS_GetException(ctx);
            JSValue funcs[2], promise, rr;
            promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(promise)) {
                JS_FreeValue(ctx, exc);
                return promise;
            }
            rr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, rr);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return promise;
        }
        it->loaded = 1;
    }
    t = &it->t;
    if (csv_view_nrows(t) == 0) {
        csv_rows_release(it);
        return csv_promise_resolved(ctx, csv_done_value(ctx));
    }
    total = csv_view_nrows(t) - 1;
    ncols = csv_view_ncols(t);
    if ((uint64_t)it->offset >= (uint64_t)total) {
        csv_rows_release(it);
        return csv_promise_resolved(ctx, csv_done_value(ctx));
    }
    start = (size_t)it->offset;
    end = start + (size_t)it->batch;
    if (end > total)
        end = total;
    hjs = JS_NewArray(ctx);
    if (JS_IsException(hjs))
        return JS_EXCEPTION;
    for (c = 0; c < ncols; c++)
        JS_DefinePropertyValueUint32(ctx, hjs, (uint32_t)c, csv_view_cell(ctx, t, 0, c), JS_PROP_C_W_E);
    rjs = csv_view_rows(ctx, t, NULL, ncols, start + 1, end + 1);
    if (JS_IsException(rjs)) {
        JS_FreeValue(ctx, hjs);
        return JS_EXCEPTION;
    }
    val = JS_NewObject(ctx);
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, hjs);
        JS_FreeValue(ctx, rjs);
        return val;
    }
    JS_DefinePropertyValueStr(ctx, val, "headers", hjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, val, "rows", rjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, val, "offset", JS_NewInt64(ctx, it->offset), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, val, "totalRows", JS_NewInt64(ctx, (int64_t)total), JS_PROP_C_W_E);
    it->offset = (int64_t)end;
    if ((uint64_t)it->offset >= (uint64_t)total)
        csv_rows_release(it);
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, val);
        return res;
    }
    JS_DefinePropertyValueStr(ctx, res, "value", val, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "done", JS_FALSE, JS_PROP_C_W_E);
    return csv_promise_resolved(ctx, res);
}

static JSValue csv_rows_return(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    csv_rows_t* it = (csv_rows_t*)JS_GetOpaque2(ctx, this_val, csv_rows_class_id);
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    csv_rows_release(it);
    return csv_promise_resolved(ctx, csv_done_value(ctx));
}

static JSValue csv_rows_async_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry csv_rows_proto[] = {
    JS_CFUNC_DEF("next", 0, csv_rows_next),
    JS_CFUNC_DEF("return", 0, csv_rows_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, csv_rows_async_iter),
};

JSValue js_csv_rows(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    JSValueConst o;
    csv_rows_t* it;
    JSValue obj, proto;
    int64_t batch = 1000;
    uint8_t delim = ',', quote = '"';
    int strict = 0;
    if (!path)
        return JS_EXCEPTION;
    o = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(o)) {
        JSValue v;
        if (opts_check(ctx, o, "csv.rows", opts_rows_keys, countof(opts_rows_keys))) {
            free(path);
            return JS_EXCEPTION;
        }
        if (opts_delim_quote(ctx, o, "csv.rows", &delim, &quote)) {
            free(path);
            return JS_EXCEPTION;
        }
        strict = opt_bool(ctx, o, "strict");
        v = JS_GetPropertyStr(ctx, o, "batch");
        if (JS_IsException(v)) {
            free(path);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &batch, v) < 0) {
                JS_FreeValue(ctx, v);
                free(path);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    } else if (!JS_IsUndefined(o) && !JS_IsNull(o)) {
        free(path);
        return JS_ThrowTypeError(ctx, "csv.rows: opts must be an object");
    }
    if (batch < 1 || batch > 100000) {
        free(path);
        return JS_ThrowRangeError(ctx, "csv.rows: batch must be 1..100000");
    }
    it = (csv_rows_t*)calloc(1, sizeof *it);
    if (!it) {
        free(path);
        return JS_ThrowOutOfMemory(ctx);
    }
    it->path = path;
    it->offset = 0;
    it->batch = batch;
    it->delim = delim;
    it->quote = quote;
    it->strict = strict;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto)) {
        free(it->path);
        free(it);
        return proto;
    }
    if (JS_SetPropertyFunctionList(ctx, proto, csv_rows_proto, countof(csv_rows_proto)) < 0) {
        JS_FreeValue(ctx, proto);
        free(it->path);
        free(it);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectClass(ctx, (int)csv_rows_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, proto);
        free(it->path);
        free(it);
        return obj;
    }
    JS_SetOpaque(obj, it);
    JS_SetPrototype(ctx, obj, proto);
    JS_FreeValue(ctx, proto);
    return obj;
}

#endif
