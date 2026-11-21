#include "stream-internal.h"

#include <sys/uio.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

#define STREAM_SINK_MIN_BUF 4096u
#define STREAM_SINK_MAX_BUF (1u << 26)

unsigned stream_sink_clamp_bufsize(int64_t v)
{
    if (v <= 0)
        return STREAM_SINK_DEFAULT_BUF;
    if (v < STREAM_SINK_MIN_BUF)
        return STREAM_SINK_MIN_BUF;
    if (v > STREAM_SINK_MAX_BUF)
        return STREAM_SINK_MAX_BUF;
    return (unsigned)v;
}

int stream_sink_flush_native(stream_sink_t* w)
{
    size_t done = 0;
    while (done < w->len) {
        ssize_t n = write(w->fd, w->buf + done, w->len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    w->len = 0;
    return 0;
}

void stream_sink_dispose(void* native)
{
    stream_sink_t* w = (stream_sink_t*)native;
    if (w->fd >= 0 && !w->sticky_errno)
        stream_sink_flush_native(w);
    if (w->fd >= 0)
        close(w->fd);
    free(w->buf);
    free(w->path);
    free(w);
}

int stream_sink_open(stream_sink_t* w, JSContext* ctx)
{
    int flags;
    int fd;
    if (w->fd >= 0)
        return 0;
    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (w->append ? O_APPEND : O_TRUNC);
    if (!w->buf) {
        w->buf = (unsigned char*)malloc(w->cap);
        if (!w->buf) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    fd = open(w->path, flags, 0644);
    if (fd < 0) {
        w->sticky_errno = errno;
        stream_errno_throw(ctx, errno, "toFile", w->path);
        return -1;
    }
    w->fd = fd;
    return 0;
}

static int stream_sink_writev2(int fd, const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    while (an + bn > 0) {
        struct iovec iov[2];
        int k = 0;
        ssize_t r;
        if (an) {
            iov[k].iov_base = (void*)(uintptr_t)a;
            iov[k++].iov_len = an;
        }
        if (bn) {
            iov[k].iov_base = (void*)(uintptr_t)b;
            iov[k++].iov_len = bn;
        }
        r = writev(fd, iov, k);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if ((size_t)r >= an) {
            b += (size_t)r - an;
            bn -= (size_t)r - an;
            an = 0;
        } else {
            a += r;
            an -= (size_t)r;
        }
    }
    return 0;
}

static int stream_sink_put(JSContext* ctx, stream_sink_t* w, const uint8_t* data, size_t len)
{
    if (w->sticky_errno) {
        stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
        return -1;
    }
    if (len == 0)
        return 0;
    if (stream_sink_open(w, ctx) < 0)
        return -1;
    if (len >= w->cap / 4 && w->len + len >= w->cap) {
        if (stream_sink_writev2(w->fd, w->buf, w->len, data, len) < 0)
            goto fail;
        w->len = 0;
    } else {
        if (w->len > 0 && len > (size_t)(w->cap - w->len)) {
            if (stream_sink_flush_native(w) < 0)
                goto fail;
        }
        memcpy(w->buf + w->len, data, len);
        w->len += (unsigned)len;
        if (w->len == w->cap) {
            if (stream_sink_flush_native(w) < 0)
                goto fail;
        }
    }
    w->total += len;
    return 0;
fail:
    w->sticky_errno = errno;
    stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
    return -1;
}

JSValue stream_sink_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_sink_t* w;
    const uint8_t* data;
    size_t len = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "ByteSink.write: buf must be a byte view (Uint8Array or similar)");
    data = stream_view_bytes(ctx, argv[0], &len, 1);
    if (!data)
        return JS_EXCEPTION;
    w = (stream_sink_t*)dyn_res_native(ctx, this_val, stream_sink_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (stream_sink_put(ctx, w, data, len) < 0)
        return stream_promise_rejected(ctx, JS_EXCEPTION);
    return stream_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)len));
}

int stream_sink_direct(JSContext* ctx, JSValueConst sink, JSValueConst write_fn, const uint8_t* p, size_t n)
{
    stream_sink_t* w;
    if (JS_GetClassID(sink) != stream_sink_class_id || !JS_IsCFunctionOf(write_fn, stream_sink_write))
        return 0;
    w = (stream_sink_t*)dyn_res_native(ctx, sink, stream_sink_class_id);
    if (!w)
        return -1;
    return stream_sink_put(ctx, w, p, n) < 0 ? -1 : 1;
}

JSValue stream_sink_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_sink_t* w;
    (void)argc;
    (void)argv;

    w = (stream_sink_t*)dyn_res_native(ctx, this_val, stream_sink_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (w->sticky_errno)
        return stream_promise_rejected(ctx,
            stream_errno_throw(ctx, w->sticky_errno, "flush", w->path));
    if (w->fd < 0 || w->len == 0)
        return stream_promise_resolved(ctx, JS_UNDEFINED);
    if (stream_sink_flush_native(w) < 0) {
        int e = errno;
        w->sticky_errno = e;
        return stream_promise_rejected(ctx,
            stream_errno_throw(ctx, e, "flush", w->path));
    }
    return stream_promise_resolved(ctx, JS_UNDEFINED);
}

#endif
