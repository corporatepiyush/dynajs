#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

static void dyn_flock_dispose(void* native)
{
    dyn_flock_t* l = (dyn_flock_t*)native;
    if (!l)
        return;
    dyn_flock_release(l);
    free(l->lockfile);
    free(l);
}

JSValue dyn_flock_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_flock_t* l;
    const char* path;
    JSValue pathv, res;
    int64_t retry = 0, retry_ms = 100;
    int attempt;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "new FileLock(path[, options]) requires a path");

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "retry", "retryMs" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 2))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "retry");
        if (!JS_IsUndefined(v)) {
            if (!JS_IsNumber(v) || JS_ToInt64(ctx, &retry, v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "FileLock: retry must be a number");
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "retryMs");
        if (!JS_IsUndefined(v)) {
            if (!JS_IsNumber(v) || JS_ToInt64(ctx, &retry_ms, v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "FileLock: retryMs must be a number");
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (retry < 0 || retry_ms < 0)
        return JS_ThrowRangeError(ctx, "FileLock: retry and retryMs must be >= 0");
    if (retry > 86400000ll || retry_ms > 86400000ll || (retry_ms > 0 && retry > 86400000ll / retry_ms))
        return JS_ThrowRangeError(ctx,
            "FileLock: retry/retryMs exceed the 24h total wait cap");

    if (dyn_is_path(argv[0])) {
        pathv = JS_DupValue(ctx, argv[0]);
    } else if (JS_IsString(argv[0])) {
        pathv = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(pathv))
            return pathv;
    } else {
        return JS_ThrowTypeError(ctx, "new FileLock(path): path must be a Path or a string");
    }
    path = dyn_path_arg(ctx, pathv, "path");
    if (!path) {
        JS_FreeValue(ctx, pathv);
        return JS_EXCEPTION;
    }

    l = (dyn_flock_t*)calloc(1, sizeof(*l));
    if (!l) {
        JS_FreeValue(ctx, pathv);
        return JS_ThrowOutOfMemory(ctx);
    }
    l->fd = -1;

    for (attempt = 0;; attempt++) {
        int r = dyn_flock_try(l, path);
        if (r == 0)
            break;
        if (r < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "FileLock", path);
            JS_FreeValue(ctx, pathv);
            dyn_flock_dispose(l);
            return ex;
        }
        if (attempt >= retry) {
            JSValue ex = dyn_fs_throw(ctx, EWOULDBLOCK, "FileLock", path);
            JS_FreeValue(ctx, pathv);
            dyn_flock_dispose(l);
            return ex;
        }
        dyn_flock_sleep_ms(retry_ms);
    }
    JS_FreeValue(ctx, pathv);

    res = dyn_res_wrap(ctx, new_target, dyn_flock_class_id, l, dyn_flock_dispose);
    return res;
}

JSValue dyn_flock_with_lock(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box;
    dyn_flock_t* l;
    JSValue ret;
    (void)argc;

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "FileLock.withLock(fn): fn must be a function");
    box = dyn_res_get(ctx, this_val, dyn_flock_class_id);
    if (!box)
        return JS_EXCEPTION;
    l = (dyn_flock_t*)box->native;
    ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (!box->closed)
        dyn_flock_dispose(l);
    dyn_res_mark_closed(JS_GetRuntime(ctx), this_val, dyn_flock_class_id);
    return ret;
}

#endif
