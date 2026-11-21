#include "libc-internal.h"
#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)
#include "modules/stream/stream-internal.h"
#endif

static const char* const js_streams_names[] = {
    "ReadableStream",
    "ReadableStreamDefaultReader",
    "ReadableStreamBYOBReader",
    "ReadableStreamDefaultController",
    "ReadableByteStreamController",
    "ReadableStreamBYOBRequest",
    "WritableStream",
    "WritableStreamDefaultWriter",
    "WritableStreamDefaultController",
    "TransformStream",
    "TransformStreamDefaultController",
    "ByteLengthQueuingStrategy",
    "CountQueuingStrategy",
    "TextEncoderStream",
    "TextDecoderStream",
    "CompressionStream",
    "DecompressionStream",
};

#define JS_STREAMS_N ((int)countof(js_streams_names))

_Static_assert(JS_STREAMS_N <= 32, "streams_own is a 32-bit mask");

static int js_streams_fd_arg(JSContext* ctx, JSValueConst v, int* fd)
{
    if (JS_ToInt32(ctx, fd, v) < 0)
        return -1;
    if (*fd < 0 || *fd > 2) {
        JS_ThrowRangeError(ctx, "streams: stdio fd must be 0, 1 or 2");
        return -1;
    }
    return 0;
}

static JSValue js_streams_fd_sync(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct stat st;
    int fd;
    (void)this_val;
    (void)argc;
    if (js_streams_fd_arg(ctx, argv[0], &fd) < 0)
        return JS_EXCEPTION;
    if (fstat(fd, &st) != 0)
        return JS_TRUE;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISBLK(st.st_mode))
        return JS_TRUE;
    return JS_NewBool(ctx, S_ISCHR(st.st_mode) && !isatty(fd));
}

static JSValue js_streams_fd_io(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    size_t off = 0, len = 0, bpe = 0, total = 0;
    uint8_t* base;
    JSValue ab;
    ssize_t n;
    int fd;
    (void)this_val;
    (void)argc;
    if (js_streams_fd_arg(ctx, argv[0], &fd) < 0)
        return JS_EXCEPTION;
    ab = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, &bpe);
    if (JS_IsException(ab))
        return ab;
    base = JS_GetArrayBuffer(ctx, &total, ab);
    JS_FreeValue(ctx, ab);
    if (!base)
        return JS_ThrowTypeError(ctx, "streams: the buffer is detached");
    do
        n = magic ? write(fd, base + off, len) : read(fd, base + off, len);
    while (n < 0 && errno == EINTR);
    if (n >= 0)
        return JS_NewInt64(ctx, (int64_t)n);
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return JS_NewInt32(ctx, -1);
    return JS_ThrowTypeError(ctx, "%s: %s failed: %s", fd == 0 ? "stdin" : fd == 1 ? "stdout" : "stderr",
        magic ? "write" : "read", strerror(errno));
}

static JSValue js_streams_fd_watch(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int w = 0;
    (void)argc;
    if (JS_ToInt32(ctx, &w, argv[1]) < 0)
        return JS_EXCEPTION;
    {
        JSValueConst a[2] = { argv[0], argv[2] };
        return js_os_setReadHandler(ctx, this_val, 2, a, w ? 1 : 0);
    }
}

static JSValue js_streams_fd_flush_stdio(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    (void)this_val;
    (void)argc;
    if (js_streams_fd_arg(ctx, argv[0], &fd) < 0)
        return JS_EXCEPTION;
    dyn_stdio_sync();
    fflush(fd == 2 ? stderr : stdout);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_streams_fd_funcs[] = {
    JS_CFUNC_DEF("fdSync", 1, js_streams_fd_sync),
    JS_CFUNC_MAGIC_DEF("fdRead", 2, js_streams_fd_io, 0),
    JS_CFUNC_MAGIC_DEF("fdWrite", 2, js_streams_fd_io, 1),
    JS_CFUNC_DEF("fdWatch", 3, js_streams_fd_watch),
    JS_CFUNC_DEF("fdFlushStdio", 1, js_streams_fd_flush_stdio),
};

JSValue js_streams_natives(JSContext* ctx)
{
    JSValue natives = JS_NewObjectProto(ctx, JS_NULL);

    if (JS_IsException(natives))
        return natives;
    if (JS_SetPropertyFunctionList(ctx, natives, js_streams_fd_funcs, countof(js_streams_fd_funcs)) < 0) {
        JS_FreeValue(ctx, natives);
        return JS_EXCEPTION;
    }
#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)
    if (dyn_stream_web_natives(ctx, natives) < 0) {
        JS_FreeValue(ctx, natives);
        return JS_EXCEPTION;
    }
#endif
#ifdef USE_WORKER
    if (js_stream_port_natives(ctx, natives) < 0) {
        JS_FreeValue(ctx, natives);
        return JS_EXCEPTION;
    }
#endif
    return natives;
}

JSValue js_streams_ns(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue ns, internal;

    if (!ts)
        return JS_ThrowInternalError(ctx, "streams: no libc thread state in this runtime");
    if (ts->streams_ns_set)
        return JS_DupValue(ctx, ts->streams_ns);
    ns = JS_NewObject(ctx);
    if (JS_IsException(ns))
        return ns;
    internal = JS_NewObject(ctx);
    if (JS_IsException(internal) || JS_DefinePropertyValueStr(ctx, ns, "internal", internal, JS_PROP_C_W_E) < 0
        || wst_ns_install(ctx, ns, 1) < 0) {
        JS_FreeValue(ctx, ns);
        return JS_EXCEPTION;
    }
    ts->streams_ns = JS_DupValue(ctx, ns);
    ts->streams_ns_set = 1;
    return ns;
}

int js_streams_loaded(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return ts && ts->streams_ns_set;
}

JSValue js_streams_call(JSContext* ctx, const char* name, int argc, JSValueConst* argv)
{
    JSValue ns, internal, fn, r;

    ns = js_streams_ns(ctx);
    if (JS_IsException(ns))
        return ns;
    internal = JS_GetPropertyStr(ctx, ns, "internal");
    JS_FreeValue(ctx, ns);
    if (JS_IsException(internal))
        return internal;
    fn = JS_GetPropertyStr(ctx, internal, name);
    r = JS_IsException(fn) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, internal);
    return r;
}

JSValue js_streams_transfer_prepare(JSContext* ctx, JSValueConst msg, JSValueConst transfer)
{
    JSValueConst args[2];

    if (!js_streams_loaded(ctx))
        return JS_UNDEFINED;
    args[0] = msg;
    args[1] = transfer;
    return js_streams_call(ctx, "transferPrepare", 2, args);
}

JSValue js_streams_transfer_revive(JSContext* ctx, JSValue data)
{
    JSValue mark, r;

    if (!JS_IsObject(data))
        return data;
    mark = JS_GetPropertyStr(ctx, data, "@@dyna.streams.envelope");
    if (JS_IsException(mark)) {
        JS_FreeValue(ctx, data);
        return mark;
    }
    if (JS_IsUndefined(mark))
        return data;
    JS_FreeValue(ctx, mark);
    r = js_streams_call(ctx, "transferRevive", 1, (JSValueConst*)&data);
    JS_FreeValue(ctx, data);
    return r;
}

static JSValue js_streams_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue ns, g, want = JS_UNDEFINED;
    int i;

    (void)this_val;
    ns = js_streams_ns(ctx);
    if (JS_IsException(ns))
        return ns;
    g = JS_GetGlobalObject(ctx);
    for (i = 0; i < JS_STREAMS_N; i++) {
        JSValue v = JS_GetPropertyStr(ctx, ns, js_streams_names[i]);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, want);
            want = JS_EXCEPTION;
            break;
        }
        if (i == magic)
            want = JS_DupValue(ctx, v);
        if (ts->streams_own & (1u << i)) {
            ts->streams_own &= ~(1u << i);
            if (JS_DefinePropertyValueStr(ctx, g, js_streams_names[i], v,
                    JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)
                < 0) {
                JS_FreeValue(ctx, want);
                want = JS_EXCEPTION;
                break;
            }
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, ns);
    return want;
}

static JSValue js_streams_set(JSContext* ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue g;

    (void)this_val;
    if (magic < 0 || magic >= JS_STREAMS_N)
        return JS_UNDEFINED;
    ts->streams_own &= ~(1u << magic);
    g = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, g, js_streams_names[magic], JS_DupValue(ctx, val),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, g);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_streams_global_funcs[] = {
    JS_CGETSET_MAGIC_DEF("ReadableStream", js_streams_get, js_streams_set, 0),
    JS_CGETSET_MAGIC_DEF("ReadableStreamDefaultReader", js_streams_get, js_streams_set, 1),
    JS_CGETSET_MAGIC_DEF("ReadableStreamBYOBReader", js_streams_get, js_streams_set, 2),
    JS_CGETSET_MAGIC_DEF("ReadableStreamDefaultController", js_streams_get, js_streams_set, 3),
    JS_CGETSET_MAGIC_DEF("ReadableByteStreamController", js_streams_get, js_streams_set, 4),
    JS_CGETSET_MAGIC_DEF("ReadableStreamBYOBRequest", js_streams_get, js_streams_set, 5),
    JS_CGETSET_MAGIC_DEF("WritableStream", js_streams_get, js_streams_set, 6),
    JS_CGETSET_MAGIC_DEF("WritableStreamDefaultWriter", js_streams_get, js_streams_set, 7),
    JS_CGETSET_MAGIC_DEF("WritableStreamDefaultController", js_streams_get, js_streams_set, 8),
    JS_CGETSET_MAGIC_DEF("TransformStream", js_streams_get, js_streams_set, 9),
    JS_CGETSET_MAGIC_DEF("TransformStreamDefaultController", js_streams_get, js_streams_set, 10),
    JS_CGETSET_MAGIC_DEF("ByteLengthQueuingStrategy", js_streams_get, js_streams_set, 11),
    JS_CGETSET_MAGIC_DEF("CountQueuingStrategy", js_streams_get, js_streams_set, 12),
    JS_CGETSET_MAGIC_DEF("TextEncoderStream", js_streams_get, js_streams_set, 13),
    JS_CGETSET_MAGIC_DEF("TextDecoderStream", js_streams_get, js_streams_set, 14),
    JS_CGETSET_MAGIC_DEF("CompressionStream", js_streams_get, js_streams_set, 15),
    JS_CGETSET_MAGIC_DEF("DecompressionStream", js_streams_get, js_streams_set, 16),
};

_Static_assert(countof(js_streams_global_funcs) == countof(js_streams_names), "one accessor per streams global");

void js_streams_install(JSContext* ctx, JSValueConst global_obj)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!ts || ts->streams_ns_set)
        return;
    ts->streams_own = (1u << JS_STREAMS_N) - 1u;
    JS_SetPropertyFunctionList(ctx, global_obj, js_streams_global_funcs,
        countof(js_streams_global_funcs));
}
