#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif
#include <stdio.h>

#define DYN_STREAM_CHUNK (1u << 17)

static const char* stream_errno_code(int e)
{
    switch (e) {
    case ENOENT:
        return "ENOENT";
    case EACCES:
        return "EACCES";
    case EEXIST:
        return "EEXIST";
    case ENOTDIR:
        return "ENOTDIR";
    case EISDIR:
        return "EISDIR";
    case EPERM:
        return "EPERM";
    case ELOOP:
        return "ELOOP";
    case ENAMETOOLONG:
        return "ENAMETOOLONG";
    case ENOSPC:
        return "ENOSPC";
    case EROFS:
        return "EROFS";
    case EMFILE:
        return "EMFILE";
    case ENFILE:
        return "ENFILE";
    case ENOMEM:
        return "ENOMEM";
    case EIO:
        return "EIO";
    case EBADF:
        return "EBADF";
    case EINVAL:
        return "EINVAL";
    default:
        return NULL;
    }
}

static JSValue stream_errno_throw(JSContext* ctx, int e, const char* op,
    const char* path)
{
    JSValue err;
    char msg[512];

    if (path)
        snprintf(msg, sizeof(msg), "stream.%s(\"%s\"): %s", op, path,
            strerror(e));
    else
        snprintf(msg, sizeof(msg), "stream.%s: %s", op, strerror(e));
    err = JS_NewError(ctx);
    if (JS_IsException(err))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e),
        JS_PROP_C_W_E);
    {
        const char* code = stream_errno_code(e);
        if (code)
            JS_DefinePropertyValueStr(ctx, err, "code",
                JS_NewString(ctx, code), JS_PROP_C_W_E);
    }
    return JS_Throw(ctx, err);
}

static JSValue stream_promise_resolved(JSContext* ctx, JSValue val)
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

static JSValue stream_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;

    if (JS_IsException(exc))
        exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static uint8_t* stream_view_bytes(JSContext* ctx, JSValueConst v,
    size_t* plen, int require_bpe1)
{
    size_t n = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &n, v);

    *plen = 0;
    if (p) {
        *plen = n;
        return p;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;

        if (JS_IsException(ab))
            return NULL;
        if (require_bpe1 && bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx,
                "byte view must be byte-wide (Uint8Array/Int8Array/"
                "Uint8ClampedArray/DataView/ArrayBuffer)");
            return NULL;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return NULL;
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}

typedef struct dyn_cps dyn_cps_t;
typedef struct dyn_cps_arm dyn_cps_arm_t;
typedef void (*dyn_cps_step_fn)(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);

struct dyn_cps {
    dyn_cps_step_fn step;
    dyn_cps_arm_t* arm;
    int dead;
};

#define DYN_CPS_STAYS_FIRST(type) \
    _Static_assert(offsetof(type, cps) == 0, #type ".cps must stay first")

struct dyn_cps_arm {
    dyn_cps_t* cp;
    JSValue anchor;
    int settled;
};

static JSClassID dyn_cps_anchor_class_id;

static void dyn_cps_anchor_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_cps_arm_t* a = JS_GetOpaque(val, dyn_cps_anchor_class_id);
    (void)rt;
    if (!a)
        return;
    if (a->cp && a->cp->arm == a)
        a->cp->arm = NULL;
    JS_SetOpaque(val, NULL);
    free(a);
}

static void dyn_cps_arm_kill(JSContext* ctx, JSRuntime* rt, dyn_cps_t* cp)
{
    dyn_cps_arm_t* a = cp->arm;
    (void)ctx;
    if (!a)
        return;
    cp->arm = NULL;
    a->cp = NULL;
    JS_SetOpaque(a->anchor, NULL);
    a->anchor = JS_UNDEFINED;
    free(a);
}

static const JSClassDef dyn_cps_anchor_class = {
    "CpsSettleAnchor",
    .finalizer = dyn_cps_anchor_finalizer,
};

static JSValue stream_swallow_rejection(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue dyn_cps_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValueConst* func_data)
{
    dyn_cps_arm_t* a;
    dyn_cps_t* cp;

    (void)this_val;
    (void)argc;
    a = JS_GetOpaque(func_data[0], dyn_cps_anchor_class_id);
    if (!a || a->settled || !a->cp)
        return JS_UNDEFINED;
    a->settled = 1;
    cp = a->cp;
    if (cp->dead)
        return JS_UNDEFINED;
    cp->step(ctx, cp, argc > 0 ? argv[0] : JS_UNDEFINED, magic);
    return JS_UNDEFINED;
}

static JSValue dyn_cps_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    dyn_cps_t* cp;
    int64_t p = 0;

    if (JS_ToInt64(ctx, &p, argv[0]))
        return JS_UNDEFINED;
    cp = (dyn_cps_t*)(uintptr_t)p;
    if (cp->dead)
        return JS_UNDEFINED;
    cp->step(ctx, cp, argc > 1 ? argv[1] : JS_UNDEFINED, 0);
    return JS_UNDEFINED;
}

static void dyn_cps_await(JSContext* ctx, dyn_cps_t* cp, dyn_cps_step_fn step,
    JSValue promise)
{
    JSValue dptr, onok, onerr, pthen, tr;
    JSValueConst thenargs[2];

    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), cp);
    if (JS_IsException(promise)) {
        JSValue exc = JS_GetException(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    {
        JSPromiseStateEnum st = JS_PromiseState(ctx, promise);
        if (st == JS_PROMISE_REJECTED) {
            JSValue pthen_v = JS_GetPropertyStr(ctx, promise, "then");
            if (!JS_IsException(pthen_v)) {
                JSValue swallow = JS_NewCFunction(ctx, stream_swallow_rejection,
                    "", 1);
                if (!JS_IsException(swallow)) {
                    JSValueConst na[2] = { JS_UNDEFINED, swallow };
                    JSValue tr_v = JS_Call(ctx, pthen_v, promise, 2, na);
                    if (JS_IsException(tr_v))
                        JS_FreeValue(ctx, JS_GetException(ctx));
                    else
                        JS_FreeValue(ctx, tr_v);
                    JS_FreeValue(ctx, swallow);
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                }
                JS_FreeValue(ctx, pthen_v);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            {
                JSValue reason = JS_PromiseResult(ctx, promise);
                JS_FreeValue(ctx, promise);
                step(ctx, cp, reason, 1);
                JS_FreeValue(ctx, reason);
            }
            return;
        }
        if (st == JS_PROMISE_FULFILLED) {
            JSValue d, result = JS_PromiseResult(ctx, promise);
            JSValueConst jargs[2];
            int rc;
            JS_FreeValue(ctx, promise);
            cp->step = step;
            d = JS_NewInt64(ctx, (int64_t)(uintptr_t)cp);
            jargs[0] = d;
            jargs[1] = result;
            rc = JS_EnqueueJob(ctx, dyn_cps_job, 2, jargs);
            JS_FreeValue(ctx, d);
            JS_FreeValue(ctx, result);
            if (rc < 0) {
                JSValue exc = JS_GetException(ctx);
                step(ctx, cp, exc, 1);
                JS_FreeValue(ctx, exc);
            }
            return;
        }
    }
    dptr = JS_NewObjectClass(ctx, dyn_cps_anchor_class_id);
    if (JS_IsException(dptr)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, promise);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    {
        dyn_cps_arm_t* a = (dyn_cps_arm_t*)calloc(1, sizeof *a);
        if (!a) {
            JS_FreeValue(ctx, dptr);
            JS_FreeValue(ctx, promise);
            JS_ThrowOutOfMemory(ctx);
            {
                JSValue exc = JS_GetException(ctx);
                step(ctx, cp, exc, 1);
                JS_FreeValue(ctx, exc);
            }
            return;
        }
        JS_SetOpaque(dptr, a);
        a->cp = cp;
        a->anchor = dptr;
        cp->arm = a;
    }
    onok = JS_NewCFunctionData(ctx, dyn_cps_settle, 1, 0, 1, &dptr);
    onerr = JS_NewCFunctionData(ctx, dyn_cps_settle, 1, 1, 1, &dptr);
    JS_FreeValue(ctx, dptr);
    if (JS_IsException(onok) || JS_IsException(onerr)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    cp->step = step;
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    if (JS_IsException(pthen)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    if (!JS_IsFunction(ctx, pthen)) {
        JS_FreeValue(ctx, pthen);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        JS_ThrowTypeError(ctx,
            "ByteSource.read: must return a Promise<number> (a pull-based"
            " read resolves its caller's byte count asynchronously)");
        step(ctx, cp, JS_GetException(ctx), 1);
        return;
    }
    thenargs[0] = onok;
    thenargs[1] = onerr;
    tr = JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onok);
    JS_FreeValue(ctx, onerr);
    if (JS_IsException(tr)) {
        JS_FreeValue(ctx, tr);
        {
            JSValue exc = JS_GetException(ctx);
            step(ctx, cp, exc, 1);
            JS_FreeValue(ctx, exc);
        }
    } else {
        JS_FreeValue(ctx, tr);
    }
}

#define STREAM_SRC_BYTES 0
static void stream_park_fail(JSContext* ctx, JSRuntime* rt,
    JSValue* resolve, JSValue* reject,
    const char* what)
{
    if (JS_IsUndefined(*resolve) && JS_IsUndefined(*reject))
        return;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc)) {
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, what), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            exc = JS_GetException(ctx);
        }
        r = JS_Call(ctx, *reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, *resolve);
        JS_FreeValue(ctx, *reject);
    } else {
        JS_FreeValueRT(rt, *resolve);
        JS_FreeValueRT(rt, *reject);
    }
    *resolve = JS_UNDEFINED;
    *reject = JS_UNDEFINED;
}

static void stream_op_sweep_tail(JSContext* ctx, JSRuntime* rt,
    dyn_cps_t* cp, JSValue* first, int n)
{
    int i;
    dyn_cps_arm_kill(ctx, rt, cp);
    cp->dead = 1;
    for (i = 0; i < n; i++) {
        if (ctx)
            JS_FreeValue(ctx, first[i]);
        else
            JS_FreeValueRT(rt, first[i]);
        first[i] = JS_UNDEFINED;
    }
    JS_ShutdownDeferFree(rt, cp);
}

#define STREAM_SRC_FILE 1

typedef struct {
    int kind;
    int fd;
    uint64_t off;
    uint8_t* buf;
    size_t len;
    char* path;
} stream_source_t;

static JSClassID stream_source_class_id;

static void stream_source_dispose(void* native)
{
    stream_source_t* s = (stream_source_t*)native;
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s->path);
    free(s);
}

static const JSClassDef stream_source_class = {
    "ByteSource",
    .finalizer = dyn_res_finalizer,
};

static JSValue stream_source_read(JSContext* ctx, JSValueConst this_val,
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

static const JSCFunctionListEntry stream_source_proto[] = {
    JS_CFUNC_DEF("read", 1, stream_source_read),
};

#define STREAM_SINK_DEFAULT_BUF (1u << 17)
#define STREAM_SINK_MIN_BUF 4096u
#define STREAM_SINK_MAX_BUF (1u << 26)

typedef struct {
    int fd;
    unsigned char* buf;
    unsigned cap;
    unsigned len;
    int append;
    int sticky_errno;
    uint64_t total;
    char* path;
} stream_sink_t;

static JSClassID stream_sink_class_id;

static unsigned stream_sink_clamp_bufsize(int64_t v)
{
    if (v <= 0)
        return STREAM_SINK_DEFAULT_BUF;
    if (v < STREAM_SINK_MIN_BUF)
        return STREAM_SINK_MIN_BUF;
    if (v > STREAM_SINK_MAX_BUF)
        return STREAM_SINK_MAX_BUF;
    return (unsigned)v;
}

static int stream_sink_flush_native(stream_sink_t* w)
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

static void stream_sink_dispose(void* native)
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

static const JSClassDef stream_sink_class = {
    "ByteSink",
    .finalizer = dyn_res_finalizer,
};

static int stream_sink_open(stream_sink_t* w, JSContext* ctx)
{
    int flags;
    int fd;
    if (w->fd >= 0)
        return 0;
    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (w->append ? O_APPEND : O_TRUNC);
    fd = open(w->path, flags, 0644);
    if (fd < 0) {
        w->sticky_errno = errno;
        stream_errno_throw(ctx, errno, "toFile", w->path);
        return -1;
    }
    w->fd = fd;
    return 0;
}

static JSValue stream_sink_write(JSContext* ctx, JSValueConst this_val,
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

    if (w->sticky_errno)
        return stream_promise_rejected(ctx,
            stream_errno_throw(ctx, w->sticky_errno, "write", w->path));

    if (len == 0)
        return stream_promise_resolved(ctx, JS_NewInt32(ctx, 0));

    if (stream_sink_open(w, ctx) < 0)
        return stream_promise_rejected(ctx, JS_GetException(ctx));

    if (w->len > 0 && len > (size_t)(w->cap - w->len)) {
        if (stream_sink_flush_native(w) < 0)
            goto fail;
    }
    if (len >= w->cap) {
        size_t done = 0;
        while (done < len) {
            ssize_t n = write(w->fd, data + done, len - done);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                goto fail;
            }
            done += (size_t)n;
        }
    } else {
        memcpy(w->buf + w->len, data, len);
        w->len += (unsigned)len;
        if (w->len == w->cap) {
            if (stream_sink_flush_native(w) < 0)
                goto fail;
        }
    }
    w->total += len;
    return stream_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)len));

fail:
    {
        int e = errno;
        w->sticky_errno = e;
        return stream_promise_rejected(ctx,
            stream_errno_throw(ctx, e, "write", w->path));
    }
}

static JSValue stream_sink_flush(JSContext* ctx, JSValueConst this_val,
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

static const JSCFunctionListEntry stream_sink_proto[] = {
    JS_CFUNC_DEF("write", 1, stream_sink_write),
    JS_CFUNC_DEF("flush", 0, stream_sink_flush),
};

static char* stream_path_arg(JSContext* ctx, JSValueConst v, const char* what)
{
    if (dyn_value_is_path(v)) {
        size_t plen = 0;
        const char* p = dyn_path_borrow(ctx, v, what, &plen);
        char* out;
        if (!p)
            return NULL;
        out = (char*)malloc(plen + 1);
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        memcpy(out, p, plen + 1);
        return out;
    }
    if (JS_IsString(v)) {
        size_t plen = 0;
        const char* p = JS_ToCStringLen(ctx, &plen, v);
        char* out;
        if (!p)
            return NULL;
        out = (char*)malloc(plen + 1);
        if (!out) {
            JS_FreeCString(ctx, p);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        memcpy(out, p, plen + 1);
        JS_FreeCString(ctx, p);
        return out;
    }
    JS_ThrowTypeError(ctx,
        "stream.%s: path must be a Path (dyna:file) or a string", what);
    return NULL;
}

static JSValue stream_from_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_source_t* s;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "fromBytes(b) requires a Uint8Array or a string");
    s = (stream_source_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->fd = -1;
    if (JS_IsString(argv[0])) {
        size_t len = 0;
        const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) {
            free(s);
            return JS_EXCEPTION;
        }
        s->buf = (uint8_t*)malloc(len ? len : 1);
        if (!s->buf) {
            JS_FreeCString(ctx, str);
            free(s);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(s->buf, str, len);
        s->len = len;
        JS_FreeCString(ctx, str);
    } else {
        const uint8_t* data = stream_view_bytes(ctx, argv[0], &s->len, 1);
        if (!data) {
            free(s);
            return JS_EXCEPTION;
        }
        s->buf = (uint8_t*)malloc(s->len ? s->len : 1);
        if (!s->buf) {
            free(s);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(s->buf, data, s->len);
    }
    s->kind = STREAM_SRC_BYTES;
    return dyn_res_wrap(ctx, JS_UNDEFINED, stream_source_class_id, s,
        stream_source_dispose);
}

static JSValue stream_from_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_source_t* s;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "fromFile(path) requires a Path or a string");
    s = (stream_source_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->fd = -1;
    s->kind = STREAM_SRC_FILE;
    s->path = stream_path_arg(ctx, argv[0], "fromFile");
    if (!s->path) {
        free(s);
        return JS_EXCEPTION;
    }
    return dyn_res_wrap(ctx, JS_UNDEFINED, stream_source_class_id, s,
        stream_source_dispose);
}

static JSValue stream_to_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    stream_sink_t* w;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "toFile(path[, opts]) requires a Path or a string");
    w = (stream_sink_t*)calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    w->fd = -1;
    w->cap = STREAM_SINK_DEFAULT_BUF;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "append");
        w->append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t bs = 0;
            if (JS_ToInt64(ctx, &bs, v)) {
                JS_FreeValue(ctx, v);
                free(w);
                return JS_EXCEPTION;
            }
            w->cap = stream_sink_clamp_bufsize(bs);
        }
        JS_FreeValue(ctx, v);
    }
    w->buf = (unsigned char*)malloc(w->cap);
    if (!w->buf) {
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->path = stream_path_arg(ctx, argv[0], "toFile");
    if (!w->path) {
        free(w->buf);
        free(w);
        return JS_EXCEPTION;
    }
    return dyn_res_wrap(ctx, JS_UNDEFINED, stream_sink_class_id, w,
        stream_sink_dispose);
}

typedef struct {
    dyn_cps_t cps;
    JSValue src, dst;
    JSValue read_fn, write_fn;
    JSValue on_chunk;
    JSValue chunk;
    JSValue jresolve, jreject;
    JSValue exc;
    uint64_t total;
    size_t chunk_read;
    size_t chunk_written;
    int terminal;
} pipe_state_t;

DYN_CPS_STAYS_FIRST(pipe_state_t);

static void pipe_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void pipe_state_free(JSContext* ctx, pipe_state_t* ps)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (ps->cps.dead)
        return;
    dyn_cps_arm_kill(ctx, rt, &ps->cps);
    JS_RemoveShutdownSweep(rt, pipe_op_sweep, ps);
    JS_ShutdownUndeferFree(rt, ps);
    JS_FreeValue(ctx, ps->src);
    JS_FreeValue(ctx, ps->dst);
    JS_FreeValue(ctx, ps->read_fn);
    JS_FreeValue(ctx, ps->write_fn);
    JS_FreeValue(ctx, ps->on_chunk);
    JS_FreeValue(ctx, ps->chunk);
    JS_FreeValue(ctx, ps->jresolve);
    JS_FreeValue(ctx, ps->jreject);
    JS_FreeValue(ctx, ps->exc);
    free(ps);
}

_Static_assert(offsetof(pipe_state_t, exc) == offsetof(pipe_state_t, src) + 8 * sizeof(JSValue),
    "pipe_state_t pins must stay contiguous");

static void pipe_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    pipe_state_t* ps = (pipe_state_t*)opaque;
    stream_park_fail(ctx, rt, &ps->jresolve, &ps->jreject,
        "stream.pipe: aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &ps->cps, &ps->src, 9);
}

static void pipe_close_side(JSContext* ctx, JSValueConst obj)
{
    JSValue close_fn = JS_GetPropertyStr(ctx, obj, "close");
    if (JS_IsException(close_fn)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (JS_IsFunction(ctx, close_fn)) {
        JSValue r = JS_Call(ctx, close_fn, obj, 0, NULL);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, close_fn);
}

static void pipe_finish(JSContext* ctx, pipe_state_t* ps, int failed)
{
    JSValue r;

    if (ps->cps.dead)
        return;
    if (ps->terminal) {
        pipe_state_free(ctx, ps);
        return;
    }
    ps->terminal = 1;
    if (failed) {
        pipe_close_side(ctx, ps->src);
        pipe_close_side(ctx, ps->dst);
        r = JS_Call(ctx, ps->jreject, JS_UNDEFINED, 1,
            (JSValueConst*)&ps->exc);
    } else {
        JSValue total = JS_NewInt64(ctx, (int64_t)ps->total);
        r = JS_Call(ctx, ps->jresolve, JS_UNDEFINED, 1, &total);
        JS_FreeValue(ctx, total);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, r);
    pipe_state_free(ctx, ps);
}

static void pipe_fail(JSContext* ctx, pipe_state_t* ps, JSValue exc)
{
    JS_FreeValue(ctx, ps->exc);
    ps->exc = exc;
    pipe_finish(ctx, ps, 1);
}

static JSValue pipe_chunk_view(JSContext* ctx, pipe_state_t* ps, size_t base,
    size_t len)
{
    JSValue ab;
    size_t boff = 0, blen = 0, bpe = 0;
    JSValue av[3];
    JSValueConst a[3];
    JSValue sub;

    ab = JS_GetTypedArrayBuffer(ctx, ps->chunk, &boff, &blen, &bpe);
    if (JS_IsException(ab))
        return ab;
    av[0] = ab;
    av[1] = JS_NewInt64(ctx, (int64_t)(boff + base));
    av[2] = JS_NewInt64(ctx, (int64_t)len);
    a[0] = av[0];
    a[1] = av[1];
    a[2] = av[2];
    sub = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, av[1]);
    JS_FreeValue(ctx, av[2]);
    JS_FreeValue(ctx, ab);
    return sub;
}

static void pipe_step_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);
static void pipe_step_write_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);

static void pipe_issue_write(JSContext* ctx, pipe_state_t* ps)
{
    JSValue view, p;

    view = pipe_chunk_view(ctx, ps, ps->chunk_written,
        ps->chunk_read - ps->chunk_written);
    if (JS_IsException(view)) {
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    p = JS_Call(ctx, ps->write_fn, ps->dst, 1, (JSValueConst*)&view);
    JS_FreeValue(ctx, view);
    dyn_cps_await(ctx, &ps->cps, pipe_step_write_done, p);
}

static void pipe_step_write_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    pipe_state_t* ps = (pipe_state_t*)cp;
    int64_t accepted = 0;
    size_t remaining = ps->chunk_read - ps->chunk_written;

    if (failed) {
        pipe_fail(ctx, ps, JS_DupValue(ctx, val));
        return;
    }
    if (JS_ToInt64(ctx, &accepted, val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSink.write must resolve to a non-negative byte count");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (accepted < 0 || (uint64_t)accepted > (uint64_t)remaining) {
        JS_ThrowRangeError(ctx,
            "ByteSink.write accepted a byte count outside [0, buf.length]");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (accepted == 0 && remaining > 0) {
        JS_ThrowTypeError(ctx,
            "ByteSink.write accepted 0 bytes of a non-empty chunk");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    ps->chunk_written += (size_t)accepted;
    ps->total += (uint64_t)accepted;
    if (ps->chunk_written < ps->chunk_read) {
        pipe_issue_write(ctx, ps);
        return;
    }
    if (JS_IsFunction(ctx, ps->on_chunk)) {
        JSValue nb = JS_NewInt64(ctx, (int64_t)ps->chunk_read);
        JSValue r = JS_Call(ctx, ps->on_chunk, JS_UNDEFINED, 1, &nb);
        JS_FreeValue(ctx, nb);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, r);
            pipe_fail(ctx, ps, JS_GetException(ctx));
            return;
        }
        JS_FreeValue(ctx, r);
    }
    {
        JSValue p = JS_Call(ctx, ps->read_fn, ps->src, 1,
            (JSValueConst*)&ps->chunk);
        dyn_cps_await(ctx, &ps->cps, pipe_step_read_done, p);
    }
}

static void pipe_step_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    pipe_state_t* ps = (pipe_state_t*)cp;
    int64_t n = 0;

    if (failed) {
        pipe_fail(ctx, ps, JS_DupValue(ctx, val));
        return;
    }
    if (JS_ToInt64(ctx, &n, val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSource.read must resolve to a non-negative byte count");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (n < 0 || n > (int64_t)DYN_STREAM_CHUNK) {
        JS_ThrowRangeError(ctx,
            "ByteSource.read returned a byte count outside [0, buf.length]");
        pipe_fail(ctx, ps, JS_GetException(ctx));
        return;
    }
    if (n == 0) {
        pipe_finish(ctx, ps, 0);
        return;
    }
    ps->chunk_read = (size_t)n;
    ps->chunk_written = 0;
    pipe_issue_write(ctx, ps);
}

static JSValue stream_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pipe_state_t* ps;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise, opts;
    JSValue rf, wf, oc, chunk, p;

    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "pipe(src, dst[, {onChunk}]) requires a ByteSource and a ByteSink");
    opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts) && !JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "pipe: options must be an object");

    rf = JS_GetPropertyStr(ctx, argv[0], "read");
    if (!JS_IsFunction(ctx, rf)) {
        JS_FreeValue(ctx, rf);
        return JS_ThrowTypeError(ctx,
            "pipe: src.read(buf) is not a function");
    }
    wf = JS_GetPropertyStr(ctx, argv[1], "write");
    if (!JS_IsFunction(ctx, wf)) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        return JS_ThrowTypeError(ctx,
            "pipe: dst.write(buf) is not a function");
    }
    oc = JS_UNDEFINED;
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts)) {
        oc = JS_GetPropertyStr(ctx, opts, "onChunk");
        if (JS_IsException(oc)) {
            JS_FreeValue(ctx, rf);
            JS_FreeValue(ctx, wf);
            return oc;
        }
        if (JS_IsUndefined(oc) || JS_IsNull(oc)) {
            JS_FreeValue(ctx, oc);
            oc = JS_UNDEFINED;
        } else if (!JS_IsFunction(ctx, oc)) {
            JS_FreeValue(ctx, rf);
            JS_FreeValue(ctx, wf);
            JS_FreeValue(ctx, oc);
            return JS_ThrowTypeError(ctx, "pipe: onChunk must be a function");
        }
    }
    {
        JSValue len = JS_NewInt64(ctx, DYN_STREAM_CHUNK);
        chunk = JS_NewTypedArray(ctx, 1, &len, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, len);
    }
    if (JS_IsException(chunk)) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        JS_FreeValue(ctx, oc);
        return chunk;
    }

    ps = (pipe_state_t*)calloc(1, sizeof(*ps));
    if (!ps) {
        JS_FreeValue(ctx, rf);
        JS_FreeValue(ctx, wf);
        JS_FreeValue(ctx, oc);
        JS_FreeValue(ctx, chunk);
        return JS_ThrowOutOfMemory(ctx);
    }
    ps->src = JS_DupValue(ctx, argv[0]);
    ps->dst = JS_DupValue(ctx, argv[1]);
    ps->read_fn = rf;
    ps->write_fn = wf;
    ps->on_chunk = oc;
    ps->chunk = chunk;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        pipe_state_free(ctx, ps);
        return promise;
    }
    ps->jresolve = funcs[0];
    ps->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), pipe_op_sweep, ps);

    p = JS_Call(ctx, ps->read_fn, ps->src, 1, (JSValueConst*)&ps->chunk);
    dyn_cps_await(ctx, &ps->cps, pipe_step_read_done, p);
    return promise;
}

#define LINE_MODE_LINES 0
#define LINE_MODE_NDJSON 1

typedef struct {
    JSValue src, read_fn;
    JSValue chunk;
    size_t chunk_cap;
    uint8_t* buf;
    size_t buf_len, buf_cap, buf_pos;
    uint64_t lineno;
    int mode;
    int eof;
    int finished;
    int busy;
} lines_iter_t;

typedef struct {
    dyn_cps_t cps;
    JSValue it_obj;
    JSValue jresolve, jreject;
} lines_op_t;

DYN_CPS_STAYS_FIRST(lines_op_t);

static JSClassID lines_iter_class_id;

static void lines_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

_Static_assert(offsetof(lines_op_t, jreject) == offsetof(lines_op_t, it_obj) + 2 * sizeof(JSValue),
    "lines_op_t pins must stay contiguous");

static void lines_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    lines_op_t* op = (lines_op_t*)opaque;
    stream_park_fail(ctx, rt, &op->jresolve, &op->jreject,
        "stream.lines: next() aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &op->cps, &op->it_obj, 3);
}

static void lines_op_retire(JSRuntime* rt, lines_op_t* op)
{
    if (op->cps.dead)
        return;
    JS_RemoveShutdownSweep(rt, lines_op_sweep, op);
    JS_ShutdownUndeferFree(rt, op);
}

static void lines_iter_finalizer(JSRuntime* rt, JSValue val)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(val, lines_iter_class_id);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->src);
    JS_FreeValueRT(rt, it->read_fn);
    JS_FreeValueRT(rt, it->chunk);
    free(it->buf);
    free(it);
}

static void lines_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(val, lines_iter_class_id);
    if (!it)
        return;
    JS_MarkValue(rt, it->src, mark);
    JS_MarkValue(rt, it->read_fn, mark);
    JS_MarkValue(rt, it->chunk, mark);
}

static const JSClassDef lines_iter_class = {
    "LineIterator",
    .finalizer = lines_iter_finalizer,
    .gc_mark = lines_iter_mark,
};

static JSValue lines_done_value(JSContext* ctx)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result))
        return result;
    JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
    return result;
}

static void lines_op_done(JSContext* ctx, lines_iter_t* it, lines_op_t* op)
{
    JSValue result, r;

    if (op->cps.dead)
        return;
    lines_op_retire(JS_GetRuntime(ctx), op);
    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), &op->cps);
    if (it)
        it->busy = 0;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 0, NULL);
    } else {
        JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
        r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
            (JSValueConst*)&result);
        JS_FreeValue(ctx, result);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static void lines_op_settle(JSContext* ctx, lines_iter_t* it, lines_op_t* op,
    JSValue value, int failed)
{
    if (op->cps.dead) {
        JS_FreeValue(ctx, value);
        return;
    }
    lines_op_retire(JS_GetRuntime(ctx), op);
    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), &op->cps);
    if (it)
        it->busy = 0;
    if (failed) {
        JSValue r = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1,
            (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    } else {
        JSValue result = JS_NewObject(ctx);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, value);
            value = JS_GetException(ctx);
            result = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1,
                (JSValueConst*)&value);
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, value);
        } else {
            JSValue r;
            JS_DefinePropertyValueStr(ctx, result, "value", value,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
                (JSValueConst*)&result);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static int lines_extract_bytes(lines_iter_t* it, const uint8_t** out,
    size_t* out_len)
{
    uint8_t* nl;

    if (it->buf_pos >= it->buf_len)
        return 0;
    nl = (uint8_t*)memchr(it->buf + it->buf_pos, '\n',
        it->buf_len - it->buf_pos);
    if (!nl)
        return 0;
    *out = it->buf + it->buf_pos;
    *out_len = (size_t)(nl - *out);
    it->buf_pos = (size_t)(nl - it->buf) + 1;
    if (*out_len > 0 && (*out)[*out_len - 1] == '\r')
        (*out_len)--;
    return 1;
}

static int lines_buf_reserve(lines_iter_t* it, JSContext* ctx, size_t need)
{
    if (it->buf_pos > 0) {
        if (it->buf_pos == it->buf_len) {
            it->buf_pos = it->buf_len = 0;
        } else if (it->buf_pos >= it->buf_cap / 2) {
            memmove(it->buf, it->buf + it->buf_pos, it->buf_len - it->buf_pos);
            it->buf_len -= it->buf_pos;
            it->buf_pos = 0;
        }
    }
    if (it->buf_len + need + 1 > it->buf_cap) {
        size_t ncap = it->buf_cap ? it->buf_cap * 2 : 2 * DYN_STREAM_CHUNK;
        uint8_t* nb;
        while (ncap < it->buf_len + need + 1)
            ncap *= 2;
        if (ncap > DYN_MAX_INPUT) {
            JS_ThrowRangeError(ctx, "stream.lines: line exceeds %u bytes"
                                    " (DYN_MAX_INPUT)",
                (unsigned)DYN_MAX_INPUT);
            return -1;
        }
        nb = (uint8_t*)realloc(it->buf, ncap);
        if (!nb) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        it->buf = nb;
        it->buf_cap = ncap;
    }
    return 0;
}

static void lines_step_drive(JSContext* ctx, lines_op_t* op);

static void lines_step_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    lines_op_t* op = (lines_op_t*)cp;
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(op->it_obj,
        lines_iter_class_id);
    int64_t n = 0;

    if (failed) {
        lines_op_settle(ctx, it, op, JS_DupValue(ctx, val), 1);
        return;
    }
    if (!it) {
        lines_op_settle(ctx, NULL, op, JS_UNDEFINED, 1);
        return;
    }
    if (JS_ToInt64(ctx, &n, val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSource.read: must resolve to a non-negative byte count");
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
        return;
    }
    if (n < 0 || n > (int64_t)it->chunk_cap) {
        JS_ThrowRangeError(ctx,
            "ByteSource.read: returned a byte count outside [0, buf.length]");
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
        return;
    }
    if (n == 0) {
        it->eof = 1;
        lines_step_drive(ctx, op);
        return;
    }
    if (lines_buf_reserve(it, ctx, (size_t)n) < 0) {
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
        return;
    }
    {
        size_t boff = 0, blen = 0, bpe = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, it->chunk, &boff, &blen, &bpe);
        uint8_t* base = JS_IsException(ab) ? NULL : JS_GetArrayBuffer(ctx, &blen, ab);
        JS_FreeValue(ctx, ab);
        if (!base) {
            lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
            return;
        }
        memcpy(it->buf + it->buf_len, base + boff, (size_t)n);
    }
    it->buf_len += (size_t)n;
    lines_step_drive(ctx, op);
}

static void lines_step_drive(JSContext* ctx, lines_op_t* op)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(op->it_obj,
        lines_iter_class_id);

    if (!it) {
        lines_op_settle(ctx, NULL, op, JS_UNDEFINED, 1);
        return;
    }
    for (;;) {
        const uint8_t* lb = NULL;
        size_t lb_len = 0;
        int have = lines_extract_bytes(it, &lb, &lb_len);
        JSValue value;

        if (!have && it->eof) {
            if (it->buf_pos < it->buf_len) {
                lb = it->buf + it->buf_pos;
                lb_len = it->buf_len - it->buf_pos;
                if (lb_len > 0 && lb[lb_len - 1] == '\r')
                    lb_len--;
                it->buf_pos = it->buf_len;
                have = 1;
            } else {
                it->finished = 1;
                lines_op_done(ctx, it, op);
                return;
            }
        }
        if (!have) {
            JSValue p = JS_Call(ctx, it->read_fn, it->src, 1,
                (JSValueConst*)&it->chunk);
            dyn_cps_await(ctx, &op->cps, lines_step_read_done, p);
            return;
        }
        it->lineno++;
        if (it->mode == LINE_MODE_NDJSON) {
            size_t i = 0;
            while (i < lb_len && (lb[i] == ' ' || lb[i] == '\t' || lb[i] == '\r'))
                i++;
            if (i == lb_len)
                continue;
            {
                uint8_t* endp = (uint8_t*)DYN_UNCONST(lb) + lb_len;
                uint8_t saved = *endp;
                *endp = '\0';
                value = JS_ParseJSON(ctx, (const char*)lb, lb_len,
                    "<dyna:stream:ndjson>");
                *endp = saved;
            }
            if (JS_IsException(value)) {
                JSValue exc = JS_GetException(ctx);
                JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
                const char* mstr = NULL;
                char mbuf[288];
                if (!JS_IsException(msg) && !JS_IsUndefined(msg))
                    mstr = JS_ToCString(ctx, msg);
                snprintf(mbuf, sizeof(mbuf), "ndjson: line %llu: %s",
                    (unsigned long long)it->lineno,
                    mstr ? mstr : "invalid JSON");
                if (mstr)
                    JS_FreeCString(ctx, mstr);
                JS_FreeValue(ctx, msg);
                JS_FreeValue(ctx, exc);
                JS_ThrowSyntaxError(ctx, "%s", mbuf);
                lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
                return;
            }
        } else {
            value = JS_NewStringLen(ctx, (const char*)lb, lb_len);
            if (JS_IsException(value)) {
                lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
                return;
            }
        }
        lines_op_settle(ctx, it, op, value, 0);
        return;
    }
}

static JSValue lines_iter_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque2(ctx, this_val,
        lines_iter_class_id);
    lines_op_t* op;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise;

    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    if (it->finished)
        return stream_promise_resolved(ctx, lines_done_value(ctx));
    if (it->busy)
        return JS_ThrowTypeError(ctx,
            "stream iterator: next() called while a previous next() is"
            " still pending");

    op = (lines_op_t*)calloc(1, sizeof(*op));
    if (!op)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        free(op);
        return promise;
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), lines_op_sweep, op);
    it->busy = 1;
    lines_step_drive(ctx, op);
    return promise;
}

static JSValue lines_iter_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque2(ctx, this_val,
        lines_iter_class_id);
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    if (!it->finished) {
        it->finished = 1;
        pipe_close_side(ctx, it->src);
    }
    return stream_promise_resolved(ctx, lines_done_value(ctx));
}

static JSValue lines_iter_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry lines_iter_proto[] = {
    JS_CFUNC_DEF("next", 0, lines_iter_next),
    JS_CFUNC_DEF("return", 0, lines_iter_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, lines_iter_async_iterator),
};

static JSValue stream_lines_common(JSContext* ctx, int argc,
    JSValueConst* argv, int mode)
{
    lines_iter_t* it;
    JSValue obj, proto, chunk_len;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            mode == LINE_MODE_NDJSON
                ? "ndjson(src) requires a ByteSource"
                : "lines(src[, {encoding}]) requires a ByteSource");
    if (mode == LINE_MODE_LINES && argc > 1 && JS_IsObject(argv[1])) {
        JSValue e = JS_GetPropertyStr(ctx, argv[1], "encoding");
        if (JS_IsException(e))
            return e;
        if (!JS_IsUndefined(e) && !JS_IsNull(e)) {
            const char* es = JS_ToCString(ctx, e);
            int ok = es && (!strcmp(es, "utf-8") || !strcmp(es, "utf8"));
            JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (!ok)
                return JS_ThrowTypeError(ctx,
                    "lines: only encoding \"utf-8\" is supported");
        } else {
            JS_FreeValue(ctx, e);
        }
    }
    {
        JSValue rf = JS_GetPropertyStr(ctx, argv[0], "read");
        if (!JS_IsFunction(ctx, rf)) {
            JS_FreeValue(ctx, rf);
            return JS_ThrowTypeError(ctx, "lines: src.read(buf) is not a function");
        }
        JS_FreeValue(ctx, rf);
    }

    it = (lines_iter_t*)calloc(1, sizeof(*it));
    if (!it)
        return JS_ThrowOutOfMemory(ctx);
    it->mode = mode;
    it->src = JS_DupValue(ctx, argv[0]);
    it->read_fn = JS_GetPropertyStr(ctx, argv[0], "read");
    chunk_len = JS_NewInt64(ctx, DYN_STREAM_CHUNK);
    it->chunk = JS_NewTypedArray(ctx, 1, &chunk_len, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, chunk_len);
    if (JS_IsException(it->read_fn) || JS_IsException(it->chunk)) {
        JS_FreeValue(ctx, it->read_fn);
        JS_FreeValue(ctx, it->chunk);
        free(it);
        return JS_EXCEPTION;
    }
    it->chunk_cap = DYN_STREAM_CHUNK;
    proto = JS_GetClassProto(ctx, lines_iter_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, lines_iter_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, it->read_fn);
        JS_FreeValue(ctx, it->chunk);
        free(it);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

static JSValue stream_lines(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return stream_lines_common(ctx, argc, argv, LINE_MODE_LINES);
}

static JSValue stream_ndjson(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return stream_lines_common(ctx, argc, argv, LINE_MODE_NDJSON);
}

enum {
    DYN_STREAM_CODEC_DEFLATE,
    DYN_STREAM_CODEC_GZIP,
    DYN_STREAM_CODEC_ZSTD,
    DYN_STREAM_CODEC_BROTLI,
    DYN_STREAM_CODEC_LZ4,
};

#if defined(CONFIG_ZSTD)
#include <zstd.h>
#endif
#if defined(__APPLE__)
#include <compression.h>
#endif
#include "core/dyn-compress.h"
#include "core/dyn-hash.h"

typedef struct {
    uint8_t* buf;
    size_t len, cap;
} cbuf_t;

static int cbuf_reserve(cbuf_t* b, size_t extra)
{
    if (b->len + extra > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        uint8_t* nb;
        while (ncap < b->len + extra)
            ncap *= 2;
        nb = (uint8_t*)realloc(b->buf, ncap);
        if (!nb)
            return -1;
        b->buf = nb;
        b->cap = ncap;
    }
    return 0;
}

static int cbuf_put(cbuf_t* b, const void* data, size_t n)
{
    if (cbuf_reserve(b, n) < 0)
        return -1;
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 0;
}

static void cbuf_free(cbuf_t* b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

static uint32_t stream_crc32_feed(uint32_t state, const uint8_t* p, size_t n)
{
    while (n--) {
        state ^= *p++;
        for (int k = 0; k < 8; k++)
            state = (state >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(state & 1));
    }
    return state;
}

static JSValue stream_bytes_view(JSContext* ctx, const uint8_t* data,
    size_t len)
{
    JSValue lenv = JS_NewInt64(ctx, (int64_t)len);
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenv, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, lenv);
    if (JS_IsException(arr))
        return arr;
    if (len) {
        size_t boff = 0, blen = 0, bpe = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, arr, &boff, &blen, &bpe);
        uint8_t* base = JS_IsException(ab) ? NULL
                                           : JS_GetArrayBuffer(ctx, &blen, ab);
        JS_FreeValue(ctx, ab);
        if (!base) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        memcpy(base + boff, data, len);
    }
    return arr;
}

typedef struct {
    int codec;
    int level;
#if defined(CONFIG_ZSTD)
    ZSTD_CCtx* zc;
#endif
#if defined(__APPLE__)
    compression_stream cs;
    int cs_active;
#endif
    uint32_t crc_reg;
    uint32_t isize;
    int header_emitted;
    int finalized;
} codec_enc_t;

static void codec_enc_free(codec_enc_t* e)
{
#if defined(CONFIG_ZSTD)
    if (e->zc) {
        ZSTD_freeCCtx(e->zc);
        e->zc = NULL;
    }
#endif
#if defined(__APPLE__)
    if (e->cs_active) {
        compression_stream_destroy(&e->cs);
        e->cs_active = 0;
    }
#endif
}

static int codec_enc_init(codec_enc_t* e, int codec, int level)
{
    memset(e, 0, sizeof(*e));
    e->codec = codec;
    e->crc_reg = 0xFFFFFFFFu;
    e->level = level;
#if defined(CONFIG_ZSTD)
    if (codec == DYN_STREAM_CODEC_ZSTD) {
        e->zc = ZSTD_createCCtx();
        if (!e->zc)
            return -1;
        ZSTD_CCtx_setParameter(e->zc, ZSTD_c_compressionLevel, level);
    }
#endif
    return 0;
}

static const char* codec_enc_feed(codec_enc_t* e, const uint8_t* in,
    size_t in_len, int finalize, cbuf_t* out)
{
    if (e->finalized)
        return "stream already finished";
    e->finalized = finalize;

    if (e->codec == DYN_STREAM_CODEC_GZIP && !e->header_emitted && (in_len || finalize)) {
        static const uint8_t hdr[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff };
        if (cbuf_put(out, hdr, sizeof(hdr)) < 0)
            return "out of memory";
        e->header_emitted = 1;
    }

    if (in_len && e->codec == DYN_STREAM_CODEC_GZIP) {
        e->crc_reg = stream_crc32_feed(e->crc_reg, in, in_len);
        e->isize += (uint32_t)in_len;
    }

    switch (e->codec) {
#if defined(CONFIG_ZSTD)
    case DYN_STREAM_CODEC_ZSTD: {
        ZSTD_inBuffer zin = { in, in_len, 0 };
        for (;;) {
            size_t need = 64 * 1024;
            ZSTD_outBuffer zout;
            size_t rc;

            if (cbuf_reserve(out, need) < 0)
                return "out of memory";
            zout.dst = out->buf + out->len;
            zout.size = need;
            zout.pos = 0;
            rc = ZSTD_compressStream2(e->zc, &zout, &zin,
                finalize ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(rc))
                return ZSTD_getErrorName(rc);
            out->len += zout.pos;
            if (zin.pos >= zin.size && (!finalize || rc == 0))
                break;
            if (zout.pos == 0 && zin.pos >= zin.size && finalize && rc != 0)
                continue;
        }
        return NULL;
    }
#endif
#if defined(__APPLE__)
    case DYN_STREAM_CODEC_DEFLATE:
    case DYN_STREAM_CODEC_GZIP:
    case DYN_STREAM_CODEC_BROTLI: {
        compression_algorithm algo = e->codec == DYN_STREAM_CODEC_BROTLI ? COMPRESSION_BROTLI
                                                                         : COMPRESSION_ZLIB;
        if (!e->cs_active) {
            if (compression_stream_init(&e->cs, COMPRESSION_STREAM_ENCODE,
                    algo)
                != COMPRESSION_STATUS_OK)
                return "compression stream init failed";
            e->cs_active = 1;
        }
        e->cs.src_ptr = in;
        e->cs.src_size = in_len;
        for (;;) {
            compression_status st;
            size_t produced;

            if (cbuf_reserve(out, 64 * 1024) < 0)
                return "out of memory";
            e->cs.dst_ptr = out->buf + out->len;
            e->cs.dst_size = 64 * 1024;
            st = compression_stream_process(&e->cs,
                finalize ? COMPRESSION_STREAM_FINALIZE : 0);
            produced = 64 * 1024 - e->cs.dst_size;
            out->len += produced;
            if (st == COMPRESSION_STATUS_END)
                break;
            if (st == COMPRESSION_STATUS_ERROR)
                return "compression stream failed";
            if (produced == 0 && e->cs.src_size == 0 && !finalize)
                break;
        }
        if (finalize && e->codec == DYN_STREAM_CODEC_GZIP) {
            uint32_t crc = ~e->crc_reg, isz = e->isize;
            uint8_t t[8];
            t[0] = (uint8_t)crc;
            t[1] = (uint8_t)(crc >> 8);
            t[2] = (uint8_t)(crc >> 16);
            t[3] = (uint8_t)(crc >> 24);
            t[4] = (uint8_t)isz;
            t[5] = (uint8_t)(isz >> 8);
            t[6] = (uint8_t)(isz >> 16);
            t[7] = (uint8_t)(isz >> 24);
            if (cbuf_put(out, t, 8) < 0)
                return "out of memory";
        }
        return NULL;
    }
#endif
    case DYN_STREAM_CODEC_LZ4: {
        if (!e->header_emitted) {
            uint8_t hdr[7];
            hdr[0] = 0x04;
            hdr[1] = 0x22;
            hdr[2] = 0x4D;
            hdr[3] = 0x18;
            hdr[4] = 0x60;
            hdr[5] = 0x70;
            hdr[6] = (uint8_t)((dyn_xxh32(hdr + 4, 2, 0) >> 8) & 0xff);
            if (cbuf_put(out, hdr, sizeof(hdr)) < 0)
                return "out of memory";
            e->header_emitted = 1;
        }
        if (in_len) {
            size_t off = 0;
            while (off < in_len) {
                size_t take = in_len - off;
                uint8_t* blk = NULL;
                size_t blk_len = 0;
                uint8_t sz[4];
                uint32_t n;

                if (take > (4u << 20))
                    take = 4u << 20;
                if (dyn_lz4_compress(in + off, take, NULL, 0, e->level,
                        NULL, &blk, &blk_len)
                    < 0) {
                    free(blk);
                    return "lz4 compress failed";
                }
                if (blk && blk_len < take) {
                    n = (uint32_t)blk_len;
                    sz[0] = (uint8_t)n;
                    sz[1] = (uint8_t)(n >> 8);
                    sz[2] = (uint8_t)(n >> 16);
                    sz[3] = (uint8_t)(n >> 24);
                    if (cbuf_put(out, sz, 4) < 0 || cbuf_put(out, blk, blk_len) < 0) {
                        free(blk);
                        return "out of memory";
                    }
                } else {
                    n = (uint32_t)take | 0x80000000u;
                    sz[0] = (uint8_t)n;
                    sz[1] = (uint8_t)(n >> 8);
                    sz[2] = (uint8_t)(n >> 16);
                    sz[3] = (uint8_t)(n >> 24);
                    if (cbuf_put(out, sz, 4) < 0 || cbuf_put(out, in + off, take) < 0) {
                        free(blk);
                        return "out of memory";
                    }
                }
                free(blk);
                off += take;
            }
        }
        if (finalize) {
            static const uint8_t endmark[4] = { 0, 0, 0, 0 };
            if (cbuf_put(out, endmark, 4) < 0)
                return "out of memory";
        }
        return NULL;
    }
    default:
        return "unsupported codec";
    }
}

typedef struct {
    int codec;
#if defined(CONFIG_ZSTD)
    ZSTD_DStream* zd;
#endif
#if defined(__APPLE__)
    compression_stream cs;
    int cs_active;
#endif
    cbuf_t in;
    cbuf_t out;
    int phase;
    uint32_t crc_reg, isize;
    uint32_t gz_want_crc, gz_want_isize;
    int gz_have_want;
    uint32_t lz4_block_max;
    int lz4_content_checksum;
    dyn_xxh32_ctx_t lz4_xxh;
    int done;
    int failed;
    int progressed;
} codec_dec_t;

static void codec_dec_free(codec_dec_t* d)
{
#if defined(CONFIG_ZSTD)
    if (d->zd) {
        ZSTD_freeDStream(d->zd);
        d->zd = NULL;
    }
#endif
#if defined(__APPLE__)
    if (d->cs_active) {
        compression_stream_destroy(&d->cs);
        d->cs_active = 0;
    }
#endif
    cbuf_free(&d->in);
    cbuf_free(&d->out);
}

static int codec_dec_init(codec_dec_t* d, int codec)
{
    memset(d, 0, sizeof(*d));
    d->codec = codec;
    d->crc_reg = 0xFFFFFFFFu;
#if defined(CONFIG_ZSTD)
    if (codec == DYN_STREAM_CODEC_ZSTD) {
        d->zd = ZSTD_createDStream();
        if (!d->zd)
            return -1;
    }
#endif
    return 0;
}

static void dec_take(cbuf_t* in, size_t n)
{
    in->len -= n;
    if (in->len)
        memmove(in->buf, in->buf + n, in->len);
}

static int gzip_parse_header(codec_dec_t* d, const char** perr)
{
    cbuf_t* in = &d->in;
    size_t need = 10;
    uint8_t flg;

    if (in->len < need)
        return 0;
    if (in->buf[0] != 0x1f || in->buf[1] != 0x8b || in->buf[2] != 0x08) {
        *perr = "not a gzip member (bad magic)";
        return -1;
    }
    flg = in->buf[3];
    if (flg & 0x04) {
        if (in->len < need + 2)
            return 0;
        need += 2 + (size_t)(in->buf[10] | (in->buf[11] << 8));
        if (in->len < need)
            return 0;
    }
    if (flg & 0x08) {
        const uint8_t* z = (const uint8_t*)memchr(in->buf + need, 0,
            in->len - need);
        if (!z)
            return 0;
        need = (size_t)(z - in->buf) + 1;
    }
    if (flg & 0x10) {
        const uint8_t* z = (const uint8_t*)memchr(in->buf + need, 0,
            in->len - need);
        if (!z)
            return 0;
        need = (size_t)(z - in->buf) + 1;
    }
    if (flg & 0x02)
        need += 2;
    if (in->len < need)
        return 0;
    dec_take(in, need);
    d->phase = 1;
    return 1;
}

static const char* codec_dec_run(codec_dec_t* d, int finalize, size_t reserve)
{
#if defined(__APPLE__)
    size_t limit;
#endif
    if (d->failed)
        return "decoder already failed";
    d->progressed = 0;

    switch (d->codec) {
#if defined(CONFIG_ZSTD)
    case DYN_STREAM_CODEC_ZSTD: {
        size_t consumed = 0;
        for (;;) {
            size_t need = 64 * 1024;
            ZSTD_outBuffer zout;
            ZSTD_inBuffer zin;
            size_t rc;

            if (cbuf_reserve(&d->out, need) < 0)
                return "out of memory";
            zin.src = d->in.buf + consumed;
            zin.size = d->in.len - consumed;
            zin.pos = 0;
            zout.dst = d->out.buf + d->out.len;
            zout.size = need;
            zout.pos = 0;
            rc = ZSTD_decompressStream(d->zd, &zout, &zin);
            if (ZSTD_isError(rc))
                return ZSTD_getErrorName(rc);
            d->out.len += zout.pos;
            consumed += zin.pos;
            if (zout.pos > 0 || zin.pos > 0)
                d->progressed = 1;
            if (rc == 0) {
                d->done = 1;
                if (consumed < d->in.len)
                    return "trailing garbage after zstd frame";
                break;
            }
            if (zout.pos == 0 && zin.pos < zin.size)
                break;
            if (zout.pos == 0 && zin.pos >= zin.size) {
                ZSTD_inBuffer empty = { NULL, 0, 0 };
                zin.src = NULL;
                zin.size = 0;
                zin.pos = 0;
                rc = ZSTD_decompressStream(d->zd, &zout, &empty);
                if (ZSTD_isError(rc))
                    return ZSTD_getErrorName(rc);
                d->out.len += zout.pos;
                if (zout.pos == 0)
                    break;
            }
        }
        if (consumed)
            dec_take(&d->in, consumed);
        return NULL;
    }
#endif
    case DYN_STREAM_CODEC_GZIP: {
        for (;;) {
            if (d->phase == 0) {
                const char* err;
                int r = gzip_parse_header(d, &err);
                if (r < 0)
                    return err;
                if (r == 0)
                    return NULL;
#if defined(__APPLE__)
                if (!d->cs_active) {
                    if (compression_stream_init(&d->cs,
                            COMPRESSION_STREAM_DECODE,
                            COMPRESSION_ZLIB)
                        != COMPRESSION_STATUS_OK)
                        return "compression stream init failed";
                    d->cs_active = 1;
                }
                d->cs.src_ptr = d->in.buf;
                d->cs.src_size = d->in.len;
                d->phase = 1;
#else
                return "gzip is not compiled in on this platform";
#endif
            }
#if defined(__APPLE__)
            if (d->phase == 1) {
                limit = d->in.len > reserve ? d->in.len - reserve : 0;
                d->cs.src_ptr = d->in.buf;
                d->cs.src_size = limit;
                for (;;) {
                    compression_status st;
                    size_t produced;

                    if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                        return "out of memory";
                    d->cs.dst_ptr = d->out.buf + d->out.len;
                    d->cs.dst_size = 64 * 1024;
                    st = compression_stream_process(&d->cs,
                        finalize ? COMPRESSION_STREAM_FINALIZE : 0);
                    produced = 64 * 1024 - d->cs.dst_size;
                    d->out.len += produced;
                    d->crc_reg = stream_crc32_feed(d->crc_reg,
                        d->out.buf + (d->out.len - produced),
                        produced);
                    d->isize += (uint32_t)produced;
                    if (st == COMPRESSION_STATUS_END) {
                        d->progressed = produced > 0 || d->cs.src_size < limit;
                        dec_take(&d->in,
                            (size_t)(d->cs.src_ptr - d->in.buf));
                        d->phase = 2;
                        break;
                    }
                    if (st == COMPRESSION_STATUS_ERROR)
                        return "malformed deflate stream in gzip member";
                    if (d->cs.src_size == 0) {
                        size_t used = (size_t)(d->cs.src_ptr - d->in.buf);
                        if (used) {
                            dec_take(&d->in, used);
                            d->progressed = 1;
                        }
                        return NULL;
                    }
                    if (produced == 0) {
                        return finalize
                            ? "truncated compressed stream (no gzip trailer)"
                            : "malformed deflate stream in gzip member";
                    }
                }
            }
#endif
            if (d->phase == 2) {
                if (!d->gz_have_want)
                    return NULL;
                if (~d->crc_reg != d->gz_want_crc)
                    return "gzip member CRC-32 mismatch";
                if (d->isize != d->gz_want_isize)
                    return "gzip member length (ISIZE) mismatch";
#if defined(__APPLE__)
                if (d->cs_active) {
                    compression_stream_destroy(&d->cs);
                    d->cs_active = 0;
                }
#endif
                if (d->in.len > reserve)
                    return "trailing garbage after gzip member";
                d->done = 1;
                return NULL;
            }
        }
    }
    case DYN_STREAM_CODEC_LZ4: {
        cbuf_t* in = &d->in;
        for (;;) {
            if (d->phase == 0) {
                size_t hdr = 6;
                uint8_t flg;

                if (in->len < hdr)
                    return NULL;
                if (in->buf[0] != 0x04 || in->buf[1] != 0x22 || in->buf[2] != 0x4D || in->buf[3] != 0x18)
                    return "not an LZ4 frame (bad magic)";
                flg = in->buf[4];
                if (((flg >> 6) & 3) != 1)
                    return "unsupported LZ4 frame version";
                if (flg & 0x08)
                    hdr += 8;
                d->lz4_content_checksum = (flg >> 2) & 1;
                {
                    static const uint32_t bmax[8] = {
                        0, 0, 0, 0, 64u * 1024, 256u * 1024,
                        1024u * 1024, 4u * 1024 * 1024
                    };
                    uint32_t code = ((uint32_t)in->buf[5] >> 4) & 7;
                    if (code < 4)
                        return "invalid LZ4 block-max size";
                    d->lz4_block_max = bmax[code];
                }
                if (in->len < hdr + 1)
                    return NULL;
                {
                    uint8_t want = (uint8_t)((dyn_xxh32(in->buf + 4, hdr - 4, 0)
                                                 >> 8)
                        & 0xff);
                    if (in->buf[hdr] != want)
                        return "LZ4 frame descriptor checksum mismatch";
                }
                dec_take(in, hdr + 1);
                if (d->lz4_content_checksum) {
                    dyn_xxh32_init(&d->lz4_xxh, 0);
                }
                d->phase = 1;
                continue;
            }
            if (d->phase == 1) {
                uint32_t bsize;

                if (in->len < 4)
                    return NULL;
                bsize = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) | ((uint32_t)in->buf[3] << 24);
                if (bsize == 0) {
                    dec_take(in, 4);
                    if (d->lz4_content_checksum) {
                        uint32_t want;
                        if (in->len < 4)
                            return NULL;
                        want = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) | ((uint32_t)in->buf[3] << 24);
                        dec_take(in, 4);
                        if (dyn_xxh32_digest(&d->lz4_xxh) != want)
                            return "LZ4 content checksum mismatch";
                    }
                    if (in->len)
                        return "trailing garbage after LZ4 frame";
                    d->done = 1;
                    return NULL;
                }
                {
                    int stored = (bsize & 0x80000000u) != 0;
                    size_t blen = bsize & 0x7FFFFFFFu;

                    if (in->len < 4 + blen)
                        return NULL;
                    dec_take(in, 4);
                    if (stored) {
                        if (cbuf_put(&d->out, in->buf, blen) < 0)
                            return "out of memory";
                        if (d->lz4_content_checksum)
                            dyn_xxh32_update(&d->lz4_xxh, in->buf, blen);
                    } else {
                        dyn_outbuf_t o;
                        memset(&o, 0, sizeof(o));
                        if (dyn_lz4_decompress(in->buf, blen, NULL, 0, &o) < 0) {
                            free(o.buf);
                            return "invalid LZ4 block";
                        }
                        if (cbuf_reserve(&d->out, o.len) < 0) {
                            free(o.buf);
                            return "out of memory";
                        }
                        memcpy(d->out.buf + d->out.len, o.buf, o.len);
                        d->out.len += o.len;
                        if (d->lz4_content_checksum)
                            dyn_xxh32_update(&d->lz4_xxh, o.buf, o.len);
                        free(o.buf);
                    }
                    dec_take(in, blen);
                    d->progressed = 1;
                }
                continue;
            }
            return NULL;
        }
    }
    default:
#if defined(__APPLE__)
        if (d->codec == DYN_STREAM_CODEC_DEFLATE || d->codec == DYN_STREAM_CODEC_BROTLI) {
            if (!d->cs_active) {
                compression_algorithm algo = d->codec == DYN_STREAM_CODEC_BROTLI ? COMPRESSION_BROTLI
                                                                                 : COMPRESSION_ZLIB;
                if (compression_stream_init(&d->cs,
                        COMPRESSION_STREAM_DECODE,
                        algo)
                    != COMPRESSION_STATUS_OK)
                    return "compression stream init failed";
                d->cs_active = 1;
            }
            d->cs.src_ptr = d->in.buf;
            d->cs.src_size = d->in.len;
            for (;;) {
                compression_status st;
                size_t produced;

                if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                    return "out of memory";
                d->cs.dst_ptr = d->out.buf + d->out.len;
                d->cs.dst_size = 64 * 1024;
                st = compression_stream_process(&d->cs, 0);
                produced = 64 * 1024 - d->cs.dst_size;
                d->out.len += produced;
                if (st == COMPRESSION_STATUS_ERROR)
                    return "malformed compressed stream";
                if (st == COMPRESSION_STATUS_END)
                    break;
                if (produced == 0 && d->cs.src_size == 0)
                    break;
            }
            {
                size_t used = (size_t)(d->cs.src_ptr - d->in.buf);
                if (used)
                    dec_take(&d->in, used);
            }
            return NULL;
        }
#endif
        return "unsupported codec";
    }
}

static const char* codec_dec_finish(codec_dec_t* d)
{
    if (d->failed)
        return "decoder already failed";
    if (d->done)
        return NULL;
    switch (d->codec) {
#if defined(__APPLE__)
    case DYN_STREAM_CODEC_DEFLATE:
    case DYN_STREAM_CODEC_BROTLI: {
        if (!d->cs_active)
            return "truncated compressed stream (no data)";
        for (;;) {
            compression_status st;
            size_t produced;

            if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                return "out of memory";
            d->cs.dst_ptr = d->out.buf + d->out.len;
            d->cs.dst_size = 64 * 1024;
            st = compression_stream_process(&d->cs,
                COMPRESSION_STREAM_FINALIZE);
            produced = 64 * 1024 - d->cs.dst_size;
            d->out.len += produced;
            if (st == COMPRESSION_STATUS_END) {
                if (d->cs.src_size)
                    return "trailing garbage after compressed stream";
                d->done = 1;
                return NULL;
            }
            if (st == COMPRESSION_STATUS_ERROR)
                return "truncated or malformed compressed stream";
            if (produced == 0)
                return "truncated compressed stream";
        }
    }
#endif
    case DYN_STREAM_CODEC_GZIP:
        if (d->in.len >= 8) {
            const uint8_t* t = d->in.buf + d->in.len - 8;
            d->gz_want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
            d->gz_want_isize = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
            d->gz_have_want = 1;
        }
        if (d->phase == 2) {
            if (!d->gz_have_want)
                return "truncated compressed stream (no gzip trailer)";
            if (~d->crc_reg != d->gz_want_crc)
                return "gzip member CRC-32 mismatch";
            if (d->isize != d->gz_want_isize)
                return "gzip member length (ISIZE) mismatch";
#if defined(__APPLE__)
            if (d->cs_active) {
                compression_stream_destroy(&d->cs);
                d->cs_active = 0;
            }
#endif
            d->done = 1;
            return NULL;
        }
        for (;;) {
            const char* err = codec_dec_run(d, 1, 0);
            if (err)
                return err;
            if (d->done)
                return NULL;
            if (!d->progressed)
                return "truncated compressed stream (no gzip trailer)";
        }
    case DYN_STREAM_CODEC_ZSTD:
    case DYN_STREAM_CODEC_LZ4:
        for (;;) {
            const char* err = codec_dec_run(d, 0, 0);
            if (err)
                return err;
            if (d->done)
                return NULL;
            if (!d->progressed)
                return "truncated compressed stream (no end mark)";
        }
    default:
        return "unsupported codec";
    }
}

typedef struct {
    JSValue inner, read_fn;
    JSValue chunk;
    size_t chunk_cap;
    codec_dec_t d;
    int busy;
    int dead;
    int closed;
} codec_source_t;

static JSClassID codec_source_class_id;

static void codec_source_state_release(codec_source_t* cs)
{
    if (cs->busy) {
        cs->dead = 1;
        return;
    }
    codec_dec_free(&cs->d);
}

static void codec_source_state_free(JSRuntime* rt, codec_source_t* cs)
{
    JS_FreeValueRT(rt, cs->inner);
    JS_FreeValueRT(rt, cs->read_fn);
    JS_FreeValueRT(rt, cs->chunk);
    cs->inner = cs->read_fn = cs->chunk = JS_UNDEFINED;
    codec_dec_free(&cs->d);
    free(cs);
}

static void codec_source_finalizer(JSRuntime* rt, JSValue val)
{
    codec_source_t* cs = (codec_source_t*)JS_GetOpaque(val,
        codec_source_class_id);
    if (!cs)
        return;
    codec_source_state_free(rt, cs);
}

static void codec_source_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark)
{
    codec_source_t* cs = (codec_source_t*)JS_GetOpaque(val,
        codec_source_class_id);
    if (!cs)
        return;
    JS_MarkValue(rt, cs->inner, mark);
    JS_MarkValue(rt, cs->read_fn, mark);
    JS_MarkValue(rt, cs->chunk, mark);
}

static const JSClassDef codec_source_class = {
    "InflateSource",
    .finalizer = codec_source_finalizer,
    .gc_mark = codec_source_mark,
};

static codec_source_t* codec_source_of(JSValueConst obj)
{
    if (!JS_IsObject(obj))
        return NULL;
    return (codec_source_t*)JS_GetOpaque(obj, codec_source_class_id);
}

typedef struct {
    dyn_cps_t cps;
    JSValue obj;
    JSValue jresolve, jreject;
    JSValue out_buf;
} codec_read_op_t;

DYN_CPS_STAYS_FIRST(codec_read_op_t);

static void codec_read_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void codec_read_op_free(JSContext* ctx, codec_read_op_t* op)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (op->cps.dead)
        return;
    dyn_cps_arm_kill(ctx, rt, &op->cps);
    JS_RemoveShutdownSweep(rt, codec_read_op_sweep, op);
    JS_ShutdownUndeferFree(rt, op);
    JS_FreeValue(ctx, op->obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    JS_FreeValue(ctx, op->out_buf);
    free(op);
}

_Static_assert(offsetof(codec_read_op_t, out_buf) == offsetof(codec_read_op_t, obj) + 3 * sizeof(JSValue),
    "codec_read_op_t pins must stay contiguous");

static void codec_read_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    codec_read_op_t* op = (codec_read_op_t*)opaque;
    stream_park_fail(ctx, rt, &op->jresolve, &op->jreject,
        "codec read: aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &op->cps, &op->obj, 4);
}

static void codec_source_state_maybe_free(codec_source_t* cs)
{
    if (cs && !cs->busy && cs->dead)
        codec_dec_free(&cs->d);
}

static void codec_read_op_leave(codec_source_t* cs)
{
    if (cs) {
        cs->busy = 0;
        if (cs->dead)
            codec_source_state_maybe_free(cs);
    }
}

static void codec_read_settle(JSContext* ctx, codec_read_op_t* op,
    JSValue value, int failed)
{
    if (op->cps.dead) {
        JS_FreeValue(ctx, value);
        return;
    }
    if (failed) {
        JSValue r = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1,
            (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    } else {
        JSValue r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
            (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    }
    codec_read_op_leave(codec_source_of(op->obj));
    codec_read_op_free(ctx, op);
}

static void codec_read_drive(JSContext* ctx, codec_read_op_t* op);

static void codec_read_read_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    codec_read_op_t* op = (codec_read_op_t*)cp;
    codec_source_t* cs = codec_source_of(op->obj);
    int64_t n = 0;
    const char* err;

    if (cs && cs->dead) {
        cs->busy = 0;
        codec_read_settle(ctx, op, JS_DupValue(ctx, val), failed);
        return;
    }
    if (cs && cs->closed && !cs->dead) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        codec_read_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (failed) {
        codec_read_settle(ctx, op, JS_DupValue(ctx, val), 1);
        return;
    }
    if (!cs) {
        codec_read_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    if (JS_ToInt64(ctx, &n, val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSource.read: must resolve to a non-negative byte count");
        codec_read_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (n < 0 || n > (int64_t)cs->chunk_cap) {
        JS_ThrowRangeError(ctx,
            "ByteSource.read: returned a byte count outside [0, buf.length]");
        codec_read_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (n == 0) {
        err = codec_dec_finish(&cs->d);
        if (err) {
            JS_ThrowInternalError(ctx, "inflate: %s", err);
            codec_read_settle(ctx, op, JS_GetException(ctx), 1);
            return;
        }
        codec_read_drive(ctx, op);
        return;
    }
    {
        size_t boff = 0, blen = 0, bpe = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, cs->chunk, &boff, &blen,
            &bpe);
        uint8_t* base = JS_IsException(ab)
            ? NULL
            : JS_GetArrayBuffer(ctx, &blen, ab);
        JS_FreeValue(ctx, ab);
        if (!base) {
            codec_read_settle(ctx, op, JS_GetException(ctx), 1);
            return;
        }
        if (cbuf_put(&cs->d.in, base + boff, (size_t)n) < 0) {
            codec_read_settle(ctx, op, JS_ThrowOutOfMemory(ctx), 1);
            return;
        }
    }
    err = codec_dec_run(&cs->d, 0,
        cs->d.codec == DYN_STREAM_CODEC_GZIP ? 8 : 0);
    if (err) {
        cs->d.failed = 1;
        JS_ThrowInternalError(ctx, "inflate: %s", err);
        codec_read_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    codec_read_drive(ctx, op);
}

static int64_t codec_read_copy(JSContext* ctx, codec_read_op_t* op,
    codec_source_t* cs)
{
    size_t off = 0, vlen = 0, bpe = 0, ablen = 0;
    JSValue ab;
    uint8_t* base;
    size_t space, take;

    ab = JS_GetArrayBufferView(ctx, op->out_buf, &off, &vlen, &bpe);
    if (JS_IsException(ab))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ablen, ab);
    JS_FreeValue(ctx, ab);
    if (!base)
        return -1;
    if (off > ablen || vlen > ablen - off)
        return -1;
    base += off;
    space = vlen;
    (void)ablen;
    take = cs->d.out.len < space ? cs->d.out.len : space;
    if (take)
        memcpy(base, cs->d.out.buf, take);
    if (take)
        dec_take(&cs->d.out, take);
    return (int64_t)take;
}

static void codec_read_drive(JSContext* ctx, codec_read_op_t* op)
{
    codec_source_t* cs = codec_source_of(op->obj);
    int64_t copied;

    if (!cs) {
        codec_read_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    copied = codec_read_copy(ctx, op, cs);
    if (copied < 0) {
        codec_read_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (copied > 0 || (cs->d.done && cs->d.out.len == 0)) {
        codec_read_settle(ctx, op, JS_NewInt64(ctx, copied), 0);
        return;
    }
    {
        JSValue p = JS_Call(ctx, cs->read_fn, cs->inner, 1,
            (JSValueConst*)&cs->chunk);
        dyn_cps_await(ctx, &op->cps, codec_read_read_done, p);
    }
}

static JSValue codec_source_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_source_t* cs;
    codec_read_op_t* op;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "ByteSource.read: buf must be a byte-wide view (Uint8Array)");
    {
        size_t off = 0, len = 0, bpe = 0;
        JSValue ab = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
        if (JS_IsException(ab))
            return ab;
        JS_FreeValue(ctx, ab);
        if (bpe != 1) {
            JS_ThrowTypeError(ctx,
                "ByteSource.read: buf must be a byte-wide view");
            return JS_EXCEPTION;
        }
    }
    cs = codec_source_of(this_val);
    if (!cs)
        return JS_EXCEPTION;
    if (cs->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return JS_EXCEPTION;
    }
    if (cs->busy)
        return JS_ThrowTypeError(ctx,
            "inflate: previous read() is still in progress");

    op = (codec_read_op_t*)calloc(1, sizeof(*op));
    if (!op)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        free(op);
        return promise;
    }
    op->obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    op->out_buf = JS_DupValue(ctx, argv[0]);
    JS_AddShutdownSweep(JS_GetRuntime(ctx), codec_read_op_sweep, op);
    cs->busy = 1;
    codec_read_drive(ctx, op);
    return promise;
}

static JSValue codec_source_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_source_t* cs = codec_source_of(this_val);

    (void)argc;
    (void)argv;
    if (!cs)
        return JS_UNDEFINED;
    if (cs->closed)
        return JS_UNDEFINED;
    cs->closed = 1;

    codec_source_state_release(cs);
    return JS_UNDEFINED;
}

static JSValue codec_source_closed(JSContext* ctx, JSValueConst this_val)
{
    codec_source_t* cs = codec_source_of(this_val);
    if (!cs)
        return JS_ThrowTypeError(ctx, "not a native resource");
    return JS_NewBool(ctx, cs->closed);
}

static const JSCFunctionListEntry codec_source_proto[] = {
    JS_CFUNC_DEF("read", 1, codec_source_read),
    JS_CFUNC_DEF("close", 0, codec_source_close),
    JS_CFUNC_DEF("dispose", 0, codec_source_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, codec_source_close),
    JS_CGETSET_DEF("closed", codec_source_closed, NULL),
};

typedef struct {
    JSValue inner, write_fn, flush_fn;
    codec_enc_t e;
    cbuf_t pend;
    uint64_t total_raw;
    int failed;
    int busy;
    int dead;
    int closed;
} codec_sink_t;

static JSClassID codec_sink_class_id;

static codec_sink_t* codec_sink_of(JSValueConst obj)
{
    if (!JS_IsObject(obj))
        return NULL;
    return (codec_sink_t*)JS_GetOpaque(obj, codec_sink_class_id);
}

static void codec_sink_state_release(codec_sink_t* cs)
{
    if (cs->busy) {
        cs->dead = 1;
        return;
    }
    codec_enc_free(&cs->e);
    cbuf_free(&cs->pend);
}

static void codec_sink_op_leave(codec_sink_t* cs)
{
    if (cs) {
        cs->busy = 0;
        if (cs->dead)
            codec_sink_state_release(cs);
    }
}

static void codec_sink_finalizer(JSRuntime* rt, JSValue val)
{
    codec_sink_t* cs = (codec_sink_t*)JS_GetOpaque(val, codec_sink_class_id);
    if (!cs)
        return;
    JS_FreeValueRT(rt, cs->inner);
    JS_FreeValueRT(rt, cs->write_fn);
    JS_FreeValueRT(rt, cs->flush_fn);
    cs->inner = cs->write_fn = cs->flush_fn = JS_UNDEFINED;
    codec_enc_free(&cs->e);
    cbuf_free(&cs->pend);
    free(cs);
}

static void codec_sink_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    codec_sink_t* cs = (codec_sink_t*)JS_GetOpaque(val, codec_sink_class_id);
    if (!cs)
        return;
    JS_MarkValue(rt, cs->inner, mark);
    JS_MarkValue(rt, cs->write_fn, mark);
    JS_MarkValue(rt, cs->flush_fn, mark);
}

static const JSClassDef codec_sink_class = {
    "DeflateSink",
    .finalizer = codec_sink_finalizer,
    .gc_mark = codec_sink_mark,
};

typedef struct {
    dyn_cps_t cps;
    JSValue obj;
    JSValue jresolve, jreject;
    size_t pend_off;
    size_t pend_total;
    int finishing;
    int settle_int;
    int64_t ok_value;
} codec_write_op_t;

DYN_CPS_STAYS_FIRST(codec_write_op_t);

static void codec_write_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void codec_write_op_free(JSContext* ctx, codec_write_op_t* op)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (op->cps.dead)
        return;
    dyn_cps_arm_kill(ctx, rt, &op->cps);
    JS_RemoveShutdownSweep(rt, codec_write_op_sweep, op);
    JS_ShutdownUndeferFree(rt, op);
    JS_FreeValue(ctx, op->obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

_Static_assert(offsetof(codec_write_op_t, jreject) == offsetof(codec_write_op_t, obj) + 2 * sizeof(JSValue),
    "codec_write_op_t pins must stay contiguous");

static void codec_write_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    codec_write_op_t* op = (codec_write_op_t*)opaque;
    stream_park_fail(ctx, rt, &op->jresolve, &op->jreject,
        "codec write: aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &op->cps, &op->obj, 3);
}

static void codec_write_settle(JSContext* ctx, codec_write_op_t* op,
    JSValue value, int failed)
{
    if (op->cps.dead) {
        JS_FreeValue(ctx, value);
        return;
    }
    if (failed) {
        JSValue r = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1,
            (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    } else {
        JSValue r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
            (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    }
    codec_sink_op_leave(codec_sink_of(op->obj));
    codec_write_op_free(ctx, op);
}

static void codec_write_drive(JSContext* ctx, codec_write_op_t* op);

static void codec_write_push_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    codec_write_op_t* op = (codec_write_op_t*)cp;
    codec_sink_t* cs = codec_sink_of(op->obj);
    int64_t accepted = 0;

    if (cs && cs->dead) {
        cs->busy = 0;
        codec_write_settle(ctx, op, JS_DupValue(ctx, val), failed);
        return;
    }
    if (failed) {
        codec_write_settle(ctx, op, JS_DupValue(ctx, val), 1);
        return;
    }
    if (!cs) {
        codec_write_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    if (JS_ToInt64(ctx, &accepted, val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx,
            "ByteSink.write: must resolve to a non-negative byte count");
        codec_write_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (accepted < 0 || (size_t)accepted > op->pend_total - op->pend_off) {
        JS_ThrowRangeError(ctx,
            "ByteSink.write: accepted a byte count outside [0, buf.length]");
        codec_write_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (accepted == 0 && op->pend_off < op->pend_total) {
        JS_ThrowTypeError(ctx, "ByteSink.write: accepted 0 bytes");
        codec_write_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    op->pend_off += (size_t)accepted;
    if (op->pend_off < op->pend_total) {
        codec_write_drive(ctx, op);
        return;
    }
    cbuf_free(&cs->pend);
    if (op->finishing) {
        pipe_close_side(ctx, cs->inner);
        cs->closed = 1;
        codec_enc_free(&cs->e);
        cbuf_free(&cs->pend);
        codec_write_settle(ctx, op,
            op->settle_int
                ? JS_NewInt64(ctx, op->ok_value)
                : JS_UNDEFINED,
            0);
        return;
    }
    codec_write_settle(ctx, op,
        op->settle_int ? JS_NewInt64(ctx, op->ok_value)
                       : JS_UNDEFINED,
        0);
}

static void codec_write_drive(JSContext* ctx, codec_write_op_t* op)
{
    codec_sink_t* cs = codec_sink_of(op->obj);
    JSValue view, p;

    if (!cs || cs->dead) {
        codec_write_settle(ctx, op,
            op->settle_int ? JS_NewInt64(ctx, op->ok_value)
                           : JS_UNDEFINED,
            0);
        return;
    }
    if (cs->failed) {
        JS_ThrowInternalError(ctx, "deflate: the compressor already failed");
        codec_write_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    if (!cs) {
        codec_write_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    view = stream_bytes_view(ctx, cs->pend.buf + op->pend_off,
        op->pend_total - op->pend_off);
    if (JS_IsException(view)) {
        codec_write_settle(ctx, op, JS_GetException(ctx), 1);
        return;
    }
    p = JS_Call(ctx, cs->write_fn, cs->inner, 1, (JSValueConst*)&view);
    JS_FreeValue(ctx, view);
    dyn_cps_await(ctx, &op->cps, codec_write_push_done, p);
}

static JSValue codec_sink_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_sink_t* cs;
    const uint8_t* data;
    size_t len = 0;
    const char* err;
    codec_write_op_t* op;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "ByteSink.write: buf must be a byte view (Uint8Array or similar)");
    data = stream_view_bytes(ctx, argv[0], &len, 1);
    if (!data)
        return JS_EXCEPTION;
    cs = codec_sink_of(this_val);
    if (!cs)
        return JS_EXCEPTION;
    if (cs->closed)
        return stream_promise_rejected(ctx,
            JS_ThrowTypeError(ctx, "use of a closed native resource"));
    if (cs->busy)
        return JS_ThrowTypeError(ctx,
            "deflate: previous write()/finish() is still in progress");
    if (cs->failed || cs->e.finalized)
        return stream_promise_rejected(ctx,
            JS_ThrowInternalError(ctx,
                cs->failed ? "deflate: the compressor already failed"
                           : "deflate: stream already finished"));

    err = codec_enc_feed(&cs->e, data, len, 0, &cs->pend);
    if (err) {
        cs->failed = 1;
        JS_ThrowInternalError(ctx, "deflate: %s", err);
        return stream_promise_rejected(ctx, JS_GetException(ctx));
    }
    cs->total_raw += len;

    if (cs->pend.len == 0) {
        return stream_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)len));
    }
    op = (codec_write_op_t*)calloc(1, sizeof(*op));
    if (!op)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        free(op);
        return promise;
    }
    op->obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), codec_write_op_sweep, op);
    op->pend_total = cs->pend.len;
    op->settle_int = 1;
    op->ok_value = (int64_t)len;
    cs->busy = 1;
    codec_write_drive(ctx, op);
    return promise;
}

static JSValue codec_sink_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_sink_t* cs = codec_sink_of(this_val);
    (void)argc;
    (void)argv;
    if (!cs)
        return JS_EXCEPTION;
    if (cs->closed)
        return stream_promise_rejected(ctx,
            JS_ThrowTypeError(ctx, "use of a closed native resource"));
    if (cs->failed)
        return stream_promise_rejected(ctx,
            JS_ThrowInternalError(ctx, "deflate: the compressor already failed"));
    if (cs->e.finalized)
        return stream_promise_rejected(ctx,
            JS_ThrowInternalError(ctx, "deflate: stream already finished"));
    if (JS_IsFunction(ctx, cs->flush_fn))
        return JS_Call(ctx, cs->flush_fn, cs->inner, 0, NULL);
    return stream_promise_resolved(ctx, JS_UNDEFINED);
}

static JSValue codec_sink_finish(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_sink_t* cs = codec_sink_of(this_val);
    const char* err;
    codec_write_op_t* op;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise;

    (void)argc;
    (void)argv;
    if (!cs)
        return JS_EXCEPTION;
    if (cs->closed)
        return stream_promise_rejected(ctx,
            JS_ThrowTypeError(ctx, "use of a closed native resource"));
    if (cs->failed)
        return stream_promise_rejected(ctx,
            JS_ThrowInternalError(ctx, "deflate: the compressor already failed"));
    if (cs->e.finalized)
        return stream_promise_rejected(ctx,
            JS_ThrowInternalError(ctx, "deflate: stream already finished"));
    err = codec_enc_feed(&cs->e, NULL, 0, 1, &cs->pend);
    if (err) {
        cs->failed = 1;
        JS_ThrowInternalError(ctx, "deflate: %s", err);
        return stream_promise_rejected(ctx, JS_GetException(ctx));
    }
    op = (codec_write_op_t*)calloc(1, sizeof(*op));
    if (!op)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        free(op);
        return promise;
    }
    op->obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), codec_write_op_sweep, op);
    op->pend_total = cs->pend.len;
    op->finishing = 1;
    op->settle_int = 1;
    op->ok_value = (int64_t)cs->total_raw;
    cs->busy = 1;
    if (cs->pend.len == 0) {
        pipe_close_side(ctx, cs->inner);
        cs->closed = 1;
        codec_enc_free(&cs->e);
        cbuf_free(&cs->pend);
        codec_sink_op_leave(cs);
        codec_write_op_free(ctx, op);
        return stream_promise_resolved(ctx,
            JS_NewInt64(ctx,
                (int64_t)cs->total_raw));
    }
    codec_write_drive(ctx, op);
    return promise;
}

static JSValue codec_sink_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_sink_t* cs = codec_sink_of(this_val);

    (void)argc;
    (void)argv;
    if (!cs)
        return JS_UNDEFINED;
    if (cs->closed)
        return JS_UNDEFINED;
    cs->closed = 1;

    codec_sink_state_release(cs);
    return JS_UNDEFINED;
}

static JSValue codec_sink_closed(JSContext* ctx, JSValueConst this_val)
{
    codec_sink_t* cs = codec_sink_of(this_val);
    if (!cs)
        return JS_ThrowTypeError(ctx, "not a native resource");
    return JS_NewBool(ctx, cs->closed);
}

static const JSCFunctionListEntry codec_sink_proto[] = {
    JS_CFUNC_DEF("write", 1, codec_sink_write),
    JS_CFUNC_DEF("flush", 0, codec_sink_flush),
    JS_CFUNC_DEF("finish", 0, codec_sink_finish),
    JS_CFUNC_DEF("close", 0, codec_sink_close),
    JS_CFUNC_DEF("dispose", 0, codec_sink_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, codec_sink_close),
    JS_CGETSET_DEF("closed", codec_sink_closed, NULL),
};

static int stream_codec_arg(JSContext* ctx, JSValueConst opts, int* pcod,
    int* plevel)
{
    JSValue c;
    const char* cs;
    int codec = -1;

    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx,
            "inflate/deflate: options with a \"codec\" are required");
        return -1;
    }
    c = JS_GetPropertyStr(ctx, opts, "codec");
    if (JS_IsException(c))
        return -1;
    cs = JS_ToCString(ctx, c);
    JS_FreeValue(ctx, c);
    if (!cs)
        return -1;
    if (!strcmp(cs, "deflate"))
        codec = DYN_STREAM_CODEC_DEFLATE;
    else if (!strcmp(cs, "gzip"))
        codec = DYN_STREAM_CODEC_GZIP;
    else if (!strcmp(cs, "zstd"))
        codec = DYN_STREAM_CODEC_ZSTD;
    else if (!strcmp(cs, "brotli"))
        codec = DYN_STREAM_CODEC_BROTLI;
    else if (!strcmp(cs, "lz4"))
        codec = DYN_STREAM_CODEC_LZ4;
    JS_FreeCString(ctx, cs);
    if (codec < 0) {
        JS_ThrowTypeError(ctx,
            "codec must be \"gzip\", \"zstd\", \"brotli\", \"lz4\" or"
            " \"deflate\"");
        return -1;
    }
#if !defined(CONFIG_ZSTD)
    if (codec == DYN_STREAM_CODEC_ZSTD) {
        JS_ThrowTypeError(ctx,
            "zstd: not compiled in (libzstd missing from this build)");
        return -1;
    }
#endif
#if !defined(__APPLE__) && !defined(CONFIG_BROTLI)
    if (codec == DYN_STREAM_CODEC_BROTLI) {
        JS_ThrowTypeError(ctx,
            "brotli: not compiled in (no libcompression or libbrotli)");
        return -1;
    }
#endif
#if !defined(__APPLE__)
    if (codec == DYN_STREAM_CODEC_GZIP || codec == DYN_STREAM_CODEC_DEFLATE) {
        JS_ThrowTypeError(ctx,
            "this codec needs libcompression (macOS) and is not compiled"
            " in on this platform");
        return -1;
    }
#endif
    *pcod = codec;
    *plevel = 0;
    if (JS_IsObject(opts)) {
        JSValue v = JS_GetPropertyStr(ctx, opts, "level");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t lv = 0;
            if (JS_ToInt64(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            *plevel = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static JSValue stream_inflate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_source_t* cs;
    JSValue obj, proto, chunk_len;
    int codec = -1, level = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "inflate(src, {codec}) requires a ByteSource");
    if (stream_codec_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &codec, &level)
        < 0)
        return JS_EXCEPTION;
    {
        JSValue rf = JS_GetPropertyStr(ctx, argv[0], "read");
        if (!JS_IsFunction(ctx, rf)) {
            JS_FreeValue(ctx, rf);
            return JS_ThrowTypeError(ctx, "inflate: src.read(buf) is not a function");
        }
        JS_FreeValue(ctx, rf);
    }
    cs = (codec_source_t*)calloc(1, sizeof(*cs));
    if (!cs)
        return JS_ThrowOutOfMemory(ctx);
    if (codec_dec_init(&cs->d, codec) < 0) {
        free(cs);
        return JS_ThrowOutOfMemory(ctx);
    }
    cs->inner = JS_DupValue(ctx, argv[0]);
    cs->read_fn = JS_GetPropertyStr(ctx, argv[0], "read");
    chunk_len = JS_NewInt64(ctx, DYN_STREAM_CHUNK);
    cs->chunk = JS_NewTypedArray(ctx, 1, &chunk_len, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, chunk_len);
    cs->chunk_cap = DYN_STREAM_CHUNK;
    if (JS_IsException(cs->read_fn) || JS_IsException(cs->chunk)) {
        JS_FreeValue(ctx, cs->inner);
        JS_FreeValue(ctx, cs->read_fn);
        JS_FreeValue(ctx, cs->chunk);
        codec_dec_free(&cs->d);
        free(cs);
        return JS_EXCEPTION;
    }
    proto = JS_GetClassProto(ctx, codec_source_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, codec_source_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        codec_source_state_free(JS_GetRuntime(ctx), cs);
        return obj;
    }
    JS_SetOpaque(obj, cs);
    return obj;
}

static JSValue stream_deflate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    codec_sink_t* cs;
    JSValue obj, proto;
    int codec = -1, level = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "deflate(sink, {codec}) requires a ByteSink");
    if (stream_codec_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &codec, &level)
        < 0)
        return JS_EXCEPTION;
    if (codec == DYN_STREAM_CODEC_ZSTD) {
#if defined(CONFIG_ZSTD)
        if (level == 0)
            level = ZSTD_defaultCLevel();
        if (level < ZSTD_minCLevel())
            level = ZSTD_minCLevel();
        if (level > ZSTD_maxCLevel())
            level = ZSTD_maxCLevel();
#endif
    } else if (codec == DYN_STREAM_CODEC_LZ4) {
        if (level == 0)
            level = 1;
        if (level < 1)
            level = 1;
        if (level > 12)
            level = 12;
    } else {
        level = 0;
    }
    {
        JSValue wf = JS_GetPropertyStr(ctx, argv[0], "write");
        if (!JS_IsFunction(ctx, wf)) {
            JS_FreeValue(ctx, wf);
            return JS_ThrowTypeError(ctx, "deflate: sink.write(buf) is not a function");
        }
        JS_FreeValue(ctx, wf);
    }
    cs = (codec_sink_t*)calloc(1, sizeof(*cs));
    if (!cs)
        return JS_ThrowOutOfMemory(ctx);
    if (codec_enc_init(&cs->e, codec, level) < 0) {
        free(cs);
        return JS_ThrowOutOfMemory(ctx);
    }
    cs->inner = JS_DupValue(ctx, argv[0]);
    cs->write_fn = JS_GetPropertyStr(ctx, argv[0], "write");
    cs->flush_fn = JS_GetPropertyStr(ctx, argv[0], "flush");
    if (JS_IsException(cs->write_fn) || JS_IsException(cs->flush_fn)) {
        JS_FreeValue(ctx, cs->inner);
        JS_FreeValue(ctx, cs->write_fn);
        JS_FreeValue(ctx, cs->flush_fn);
        codec_enc_free(&cs->e);
        free(cs);
        return JS_EXCEPTION;
    }
    proto = JS_GetClassProto(ctx, codec_sink_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, codec_sink_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        codec_enc_free(&cs->e);
        cbuf_free(&cs->pend);
        free(cs);
        return obj;
    }
    JS_SetOpaque(obj, cs);
    return obj;
}

static int codec_register_classes(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto;

    JS_NewClassID(&codec_source_class_id);
    if (JS_NewClass(rt, codec_source_class_id, &codec_source_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, codec_source_proto,
        (int)countof(codec_source_proto));
    JS_SetClassProto(ctx, codec_source_class_id, proto);

    JS_NewClassID(&codec_sink_class_id);
    if (JS_NewClass(rt, codec_sink_class_id, &codec_sink_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, codec_sink_proto,
        (int)countof(codec_sink_proto));
    JS_SetClassProto(ctx, codec_sink_class_id, proto);
    return 0;
}

static const JSCFunctionListEntry dyn_stream_funcs[] = {
    JS_CFUNC_DEF("pipe", 2, stream_pipe),
    JS_CFUNC_DEF("fromBytes", 1, stream_from_bytes),
    JS_CFUNC_DEF("fromFile", 1, stream_from_file),
    JS_CFUNC_DEF("toFile", 1, stream_to_file),
    JS_CFUNC_DEF("lines", 1, stream_lines),
    JS_CFUNC_DEF("ndjson", 1, stream_ndjson),
    JS_CFUNC_DEF("inflate", 2, stream_inflate),
    JS_CFUNC_DEF("deflate", 2, stream_deflate),
};

static int stream_register_classes(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto;

    JS_NewClassID(&dyn_cps_anchor_class_id);
    if (JS_NewClass(rt, dyn_cps_anchor_class_id, &dyn_cps_anchor_class) < 0)
        return -1;

    JS_NewClassID(&stream_source_class_id);
    if (JS_NewClass(rt, stream_source_class_id, &stream_source_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, stream_source_proto,
        (int)countof(stream_source_proto));
    dyn_res_class_common(ctx, stream_source_class_id, proto);
    JS_SetClassProto(ctx, stream_source_class_id, proto);

    JS_NewClassID(&stream_sink_class_id);
    if (JS_NewClass(rt, stream_sink_class_id, &stream_sink_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, stream_sink_proto,
        (int)countof(stream_sink_proto));
    dyn_res_class_common(ctx, stream_sink_class_id, proto);
    JS_SetClassProto(ctx, stream_sink_class_id, proto);

    JS_NewClassID(&lines_iter_class_id);
    if (JS_NewClass(rt, lines_iter_class_id, &lines_iter_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, lines_iter_proto,
        (int)countof(lines_iter_proto));
    JS_SetClassProto(ctx, lines_iter_class_id, proto);
    return 0;
}

static int dyn_stream_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (stream_register_classes(ctx) < 0)
        return -1;
    if (codec_register_classes(ctx) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_stream_funcs,
        (int)countof(dyn_stream_funcs));
}

int js_nat_init_stream(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:stream", dyn_stream_init_module);
    if (!m)
        return -1;
    JS_AddModuleExportList(ctx, m, dyn_stream_funcs,
        (int)countof(dyn_stream_funcs));
    return 0;
}

#endif
