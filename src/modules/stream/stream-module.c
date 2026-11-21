#include "stream-internal.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

#define STREAM_SRC_FILE 1

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

JSValue stream_errno_throw(JSContext* ctx, int e, const char* op,
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

JSValue stream_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue funcs[2], promise, r;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, r);
    return promise;
}

void stream_drop_exc(JSContext* ctx)
{
    if (!JS_HasUncatchableException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

JSValue stream_take_exc(JSContext* ctx)
{
    if (JS_HasUncatchableException(ctx))
        return JS_EXCEPTION;
    return JS_GetException(ctx);
}

JSValue stream_ret(JSContext* ctx, JSValue v)
{
    if (!JS_HasUncatchableException(ctx))
        return v;
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

JSValue stream_promise_rejected(JSContext* ctx, JSValue exc)
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

uint8_t* stream_view_bytes(JSContext* ctx, JSValueConst v,
    size_t* plen, int require_bpe1)
{
    size_t n = 0;
    uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);

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

JSClassID dyn_cps_anchor_class_id;

static const JSClassDef dyn_cps_anchor_class = {
    "CpsSettleAnchor",
    .finalizer = dyn_cps_anchor_finalizer,
};

void stream_park_fail(JSContext* ctx, JSRuntime* rt,
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

void stream_op_sweep_tail(JSContext* ctx, JSRuntime* rt,
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

JSClassID stream_source_class_id;

static const JSClassDef stream_source_class = {
    "ByteSource",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry stream_source_proto[] = {
    JS_CFUNC_DEF("read", 1, stream_source_read),
};

JSClassID stream_sink_class_id;

static const JSClassDef stream_sink_class = {
    "ByteSink",
    .finalizer = dyn_res_finalizer,
};

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
        if (strlen(p) != plen) {
            JS_FreeCString(ctx, p);
            JS_ThrowTypeError(ctx,
                "stream.%s: a string path must not contain a NUL byte", what);
            return NULL;
        }
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

static JSValue stream_web_wrap(JSContext* ctx, JSValue obj, int writable)
{
    JSValue r;
    if (JS_IsException(obj))
        return obj;
    r = js_streams_call(ctx, writable ? "toWritableStream" : "toReadableStream", 1, (JSValueConst*)&obj);
    JS_FreeValue(ctx, obj);
    return r;
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
    return stream_web_wrap(ctx, dyn_res_wrap(ctx, JS_UNDEFINED, stream_source_class_id, s,
        stream_source_dispose), 0);
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
    return stream_web_wrap(ctx, dyn_res_wrap(ctx, JS_UNDEFINED, stream_source_class_id, s,
        stream_source_dispose), 0);
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
    w->path = stream_path_arg(ctx, argv[0], "toFile");
    if (!w->path) {
        free(w);
        return JS_EXCEPTION;
    }
    return stream_web_wrap(ctx, dyn_res_wrap(ctx, JS_UNDEFINED, stream_sink_class_id, w,
        stream_sink_dispose), 1);
}

JSClassID lines_iter_class_id;

static const JSClassDef lines_iter_class = {
    "LineIterator",
    .finalizer = lines_iter_finalizer,
    .gc_mark = lines_iter_mark,
};

static const JSCFunctionListEntry lines_iter_proto[] = {
    JS_CFUNC_DEF("next", 0, lines_iter_next),
    JS_CFUNC_DEF("return", 0, lines_iter_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, lines_iter_async_iterator),
};

static JSValue stream_ndjson(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return stream_lines_common(ctx, argc, argv, LINE_MODE_NDJSON);
}

int cbuf_reserve(cbuf_t* b, size_t extra)
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

JSValue stream_bytes_view(JSContext* ctx, const uint8_t* data,
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

static const JSCFunctionListEntry dyn_stream_funcs[] = {
    JS_CFUNC_DEF("fromBytes", 1, stream_from_bytes),
    JS_CFUNC_DEF("fromFile", 1, stream_from_file),
    JS_CFUNC_DEF("toFile", 1, stream_to_file),
    JS_CFUNC_DEF("lines", 1, stream_lines),
    JS_CFUNC_DEF("ndjson", 1, stream_ndjson),
    JS_CFUNC_MAGIC_DEF("toReadableStream", 1, stream_web_bridge, 0),
    JS_CFUNC_MAGIC_DEF("toWritableStream", 1, stream_web_bridge, 1),
    JS_CFUNC_MAGIC_DEF("stdin", 0, stream_web_bridge, 4),
    JS_CFUNC_MAGIC_DEF("stdout", 0, stream_web_bridge, 5),
    JS_CFUNC_MAGIC_DEF("stderr", 0, stream_web_bridge, 6),
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
    dyn_sink_direct_cid = stream_sink_class_id;
    dyn_sink_direct = stream_sink_direct;

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
