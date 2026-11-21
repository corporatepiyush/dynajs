#include "stream-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

void stream_source_dispose(void* native)
{
    stream_source_t* s = (stream_source_t*)native;
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s->path);
    free(s);
}

JSValue stream_source_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_source_t* s;
    uint8_t* base;
    size_t len = 0;
    uint64_t n;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "ByteSource.read: buf must be a byte-wide view (Uint8Array)");
    base = stream_view_bytes(ctx, argv[0], &len, 1);
    if (!base)
        return JS_EXCEPTION;
    s = (stream_source_t*)dyn_res_native(ctx, this_val,
        stream_source_class_id);
    if (!s)
        return JS_EXCEPTION;

    if (len == 0)
        return stream_promise_resolved(ctx, JS_NewInt32(ctx, 0));

    if (s->kind == STREAM_SRC_BYTES) {
        uint64_t left = (uint64_t)s->len - s->off;
        n = (uint64_t)len < left ? (uint64_t)len : left;
        if (n)
            memcpy(base, s->buf + s->off, (size_t)n);
        s->off += n;
        return stream_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
    if (s->fd < 0) {
        int fd = open(s->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return stream_promise_rejected(ctx,
                stream_errno_throw(ctx, errno, "fromFile", s->path));
        s->fd = fd;
    }
    for (;;) {
        ssize_t r = read(s->fd, base, len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return stream_promise_rejected(ctx,
                stream_errno_throw(ctx, errno, "read", s->path));
        }
        n = (uint64_t)r;
        break;
    }
    s->off += n;
    return stream_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
}

#endif
