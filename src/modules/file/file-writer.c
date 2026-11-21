#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

static void dyn_fwriter_flush_native(dyn_fwriter_t* w, int* err)
{
    size_t off = 0;
    *err = 0;
    while (off < w->len) {
        ssize_t n = write(w->fd, w->buf + off, w->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            *err = 1;
            break;
        }
        if (n == 0) {
            *err = 1;
            break;
        }
        off += (size_t)n;
    }
    if (off > 0 && off < w->len)
        memmove(w->buf, w->buf + off, w->len - off);
    w->len -= off;
}

static void dyn_fwriter_dispose(void* native)
{
    dyn_fwriter_t* w = (dyn_fwriter_t*)native;
    int err;
    if (w->fd >= 0) {
        dyn_fwriter_flush_native(w, &err);
        close(w->fd);
    }
    free(w->buf);
    free(w);
}

JSValue dyn_fwriter_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_fwriter_t* w;
    const char* path;
    int64_t bufsize = 0, preallocate = 0;
    int append = 0, flags;
    int fd;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileWriter(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bufferSize", "preallocate", "append" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 3))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "preallocate");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &preallocate, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "append");
        {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                return JS_EXCEPTION;
            append = b;
        }
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    fd = dyn_fs_path_open(path, flags, 0644);
    if (fd < 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "FileWriter", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    if (preallocate > 0)
        dyn_io_preallocate(fd, (off_t)preallocate);

    w = (dyn_fwriter_t*)calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->fd = fd;
    w->cap = dyn_file_clamp_bufsize(bufsize);
    w->buf = (unsigned char*)malloc(w->cap);
    if (!w->buf) {
        close(fd);
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_fwriter_class_id, w, dyn_fwriter_dispose);
}

static int dyn_fwriter_put(dyn_fwriter_t* w, const char* data, size_t len)
{
    int err;
    if (len >= w->cap) {
        size_t off = 0;
        dyn_fwriter_flush_native(w, &err);
        w->dirty = 1;
        if (err)
            return -1;
        while (off < len) {
            ssize_t n = write(w->fd, data + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            off += (size_t)n;
        }
        return 0;
    }
    if (w->len + len > w->cap) {
        dyn_fwriter_flush_native(w, &err);
        if (err)
            return -1;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    w->dirty = 1;
    return 0;
}

JSValue dyn_fwriter_write(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    const uint8_t* data = NULL;
    const char* str = NULL;
    size_t len = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "write(data) requires an argument");
    if (dyn_file_payload(ctx, argv[0], &data, &len, &str))
        return JS_EXCEPTION;

    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (dyn_fwriter_put(w, (const char*)data, len) < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "FileWriter: write error");
    }
    if (str)
        JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx, (int64_t)len);
}

JSValue dyn_fwriter_flush(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    int err;
    (void)argc;
    (void)argv;
    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err)
        return JS_ThrowInternalError(ctx, "FileWriter: flush error");
    return JS_UNDEFINED;
}

JSValue dyn_fwriter_sync(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    int err;
    (void)argc;
    (void)argv;
    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err || dyn_io_durable_sync(w->fd) < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: sync error");
    w->dirty = 0;
    return JS_UNDEFINED;
}

#endif
