#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#define DYN_FH_FORWARD(name, fn, maxargs)                                \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_fh_t* f = dyn_fh_of(ctx, this_val);                          \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!f)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = f->path;                                                  \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

JSClassID dyn_path_class_id;

static const JSClassDef dyn_path_class = {
    "Path",
    .finalizer = dyn_path_finalizer,
};

int dyn_value_is_path(JSValueConst v)
{
    return dyn_is_path(v);
}

static const JSCFunctionListEntry dyn_path_proto[] = {
    JS_CGETSET_DEF("dirname", dyn_path_get_dirname, NULL),
    JS_CGETSET_DEF("basename", dyn_path_get_basename, NULL),
    JS_CGETSET_DEF("extname", dyn_path_get_extname, NULL),
    JS_CGETSET_DEF("isAbsolute", dyn_path_get_is_absolute, NULL),
    JS_CFUNC_MAGIC_DEF("join", 0, dyn_path_compose, 0),
    JS_CFUNC_MAGIC_DEF("resolve", 0, dyn_path_compose, 1),
    JS_CFUNC_DEF("relativeTo", 1, dyn_path_relative_to),
    JS_CFUNC_DEF("equals", 1, dyn_path_equals),
    JS_CFUNC_DEF("basenameWithout", 1, dyn_path_basename_without),
    JS_CFUNC_DEF("toString", 0, dyn_path_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_path_to_string),
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, dyn_path_to_string),
};

JSClassID dyn_freader_class_id;

static const JSClassDef dyn_freader_class = {
    "FileReader",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_freader_proto[] = {
    JS_CFUNC_DEF("read", 0, dyn_freader_read),
    JS_CFUNC_DEF("readLine", 0, dyn_freader_read_line),
    JS_CFUNC_DEF("readAll", 0, dyn_freader_read),
    JS_CFUNC_DEF("readInto", 1, dyn_freader_read_into),
    JS_CFUNC_DEF("readBytes", 0, dyn_freader_read_bytes),
};

JSClassID dyn_fwriter_class_id;

static const JSClassDef dyn_fwriter_class = {
    "FileWriter",
    .finalizer = dyn_res_finalizer,
};

int dyn_file_payload(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned)
{
    *powned = NULL;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_ThrowTypeError(ctx,
            "data must be a string, Uint8Array, or ArrayBuffer");
        return -1;
    }
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLenUTF8(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
    {
        size_t n;
        uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
        if (p) {
            *pdata = p;
            *plen = n;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        size_t off, len, bpe, ab_size;
        uint8_t* base;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1;
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        size_t n;
        const char* s = JS_ToCStringLenUTF8(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
}

static const JSCFunctionListEntry dyn_fwriter_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_fwriter_write),
    JS_CFUNC_DEF("flush", 0, dyn_fwriter_flush),
    JS_CFUNC_DEF("sync", 0, dyn_fwriter_sync),
};

int dyn_file_slurp_path(const char* path, dyn_iobuf_t* src, int* perr)
{
    int rfd = dyn_fs_path_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (rfd < 0) {
        *perr = errno;
        return -1;
    }
    if (dyn_io_slurp_fd(rfd, src, 0, DYN_MAX_INPUT) < 0) {
        *perr = errno;
        return -2;
    }
    return 0;
}

int dyn_file_write_path(const char* path, const uint8_t* data, size_t len,
    int append, int atomic, int* stage)
{
    int fd, werrno = 0;
    size_t off = 0;
    *stage = 0;
    if (atomic)
        return dyn_fs_write_atomic(path, data, len);
    fd = dyn_fs_path_open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) {
        *stage = 1;
        return errno;
    }
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            werrno = errno ? errno : EIO;
            break;
        }
        off += (size_t)n;
    }
    close(fd);
    *stage = 2;
    return werrno;
}

static int dyn_file_slurp_arg(JSContext* ctx, JSValueConst pathv,
    const char* op, dyn_iobuf_t* src)
{
    const char* path;
    path = dyn_path_arg(ctx, pathv, "path");
    if (!path)
        return -1;
    {
        int se = 0;
        int rc = DYN_OFFLOAD_IO(dyn_file_slurp_path(path, src, &se));
        if (rc == -1) {
            dyn_fs_throw(ctx, se, op, path);
            dyn_path_unborrow(ctx, path);
            return -1;
        }
        if (rc < 0) {
            if (se == EFBIG)
                JS_ThrowRangeError(ctx,
                    "%s: file exceeds %u bytes (DYN_MAX_INPUT)",
                    op, (unsigned)DYN_MAX_INPUT);
            else
                dyn_fs_throw(ctx, se ? se : EIO, op, path);
            dyn_path_unborrow(ctx, path);
            return -1;
        }
    }
    dyn_path_unborrow(ctx, path);
    return 0;
}

JSValue dyn_file_bytes_take(JSContext* ctx, dyn_iobuf_t* src)
{
    JSValue ab, out;
    JSValueConst ta[3];

    if (src->kind != DYN_IOBUF_HEAP || src->rpos != 0 || src->len == 0) {
        out = dyn_nat_u8_copy(ctx, dyn_iobuf_rdata(src), dyn_iobuf_rlen(src));
        dyn_iobuf_free(src);
        return out;
    }
    ab = dyn_nat_ab_take(ctx, src->data, src->len);
    dyn_iobuf_init(src);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_file_read_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    JSValue out;
    int as_bytes = 0;
    (void)this_val;

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bytes" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bytes");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        as_bytes = b;
    }
    if (dyn_file_slurp_arg(ctx, argv[0], "readFile", &src))
        return JS_EXCEPTION;
    if (as_bytes)
        return dyn_file_bytes_take(ctx, &src);
    out = dyn_utf8_lossy_string(ctx, dyn_iobuf_rdata(&src),
        dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return out;
}

static JSValue dyn_file_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    (void)this_val;
    (void)argc;

    if (dyn_file_slurp_arg(ctx, argv[0], "readBytes", &src))
        return JS_EXCEPTION;
    return dyn_file_bytes_take(ctx, &src);
}

JSValue dyn_file_write_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *path = NULL, *str = NULL;
    const uint8_t* data = NULL;
    size_t len = 0;
    int append = 0, atomic = 0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, options])");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (dyn_file_payload(ctx, argv[1], &data, &len, &str)) {
        dyn_path_unborrow(ctx, path);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        static const char* const keys[] = { "append", "atomic" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[2], keys, 2)) {
            dyn_path_unborrow(ctx, path);
            if (str)
                JS_FreeCString(ctx, str);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "append");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0) {
            dyn_path_unborrow(ctx, path);
            if (str)
                JS_FreeCString(ctx, str);
            return JS_EXCEPTION;
        }
        append = b;
        v = JS_GetPropertyStr(ctx, argv[2], "atomic");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0 || (b && append)) {
            dyn_path_unborrow(ctx, path);
            if (str)
                JS_FreeCString(ctx, str);
            return b < 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "writeFile: atomic and append cannot be combined");
        }
        atomic = b;
    }

    {
        uint8_t* hold = NULL;
        int stage = 0, e;
        JSValue r;
        if (!str && len && dyn_fiber_can_offload()) {
            hold = (uint8_t*)malloc(len);
            if (!hold) {
                dyn_path_unborrow(ctx, path);
                return JS_ThrowOutOfMemory(ctx);
            }
            memcpy(hold, data, len);
            data = hold;
        }
        e = DYN_OFFLOAD_IO(dyn_file_write_path(path, data, len, append, atomic, &stage));
        free(hold);
        r = e ? dyn_fs_throw(ctx, e, "writeFile", stage == 2 ? NULL : path) : JS_NewInt64(ctx, (int64_t)len);
        dyn_path_unborrow(ctx, path);
        if (str)
            JS_FreeCString(ctx, str);
        return r;
    }
}

typedef struct dyn_fa_rt {
    struct dyn_fa_rt* next;
    JSRuntime* rt;
} dyn_fa_rt_t;

static _Thread_local dyn_fa_job_t* dyn_fa_jobs;
static _Thread_local dyn_fa_rt_t* dyn_fa_rts;

static const char* const dyn_fa_names[] = { "readFile", "readBytes", "writeFile" };

static void dyn_fa_free(dyn_fa_job_t* j)
{
    if (j->linked) {
        if (j->prev)
            j->prev->next = j->next;
        else
            dyn_fa_jobs = j->next;
        if (j->next)
            j->next->prev = j->prev;
    }
    if (j->op == DYN_FA_HANDLE) {
        dyn_fab_free(j->u.h.own_fb, j->u.h.own_fd);
        if (j->u.h.own_fd >= 0)
            close(j->u.h.own_fd);
        free(j->u.h.buf);
        free(j->u.h.out);
    } else if (j->op == DYN_FA_FS) {
        dyn_fs_req_clear(&j->u.fs);
    } else {
        dyn_iobuf_free(&j->u.io.out);
        free(j->u.io.fixed);
        free(j->u.io.data);
    }
    free(j);
}

static void dyn_fa_reap(void)
{
    while (dyn_fa_jobs)
        dyn_fa_free(dyn_fa_jobs);
}

static void dyn_fa_work(void* arg)
{
    dyn_fa_job_t* j = (dyn_fa_job_t*)arg;
    const uint8_t* p;
    size_t n, need;
    int changed;

    if (j->op == DYN_FA_HANDLE) {
        dyn_fah_run(&j->u.h);
        return;
    }
    if (j->op == DYN_FA_FS) {
        dyn_fs_req_run(&j->u.fs);
        return;
    }
    if (j->op == DYN_FA_WRITE) {
        j->u.io.err = dyn_file_write_path(j->u.io.path, j->u.io.data, j->u.io.len, j->u.io.append, j->u.io.atomic, &j->u.io.stage);
        return;
    }
    j->u.io.rc = dyn_file_slurp_path(j->u.io.path, &j->u.io.out, &j->u.io.err);
    if (j->u.io.rc || j->op != DYN_FA_TEXT)
        return;
    p = dyn_iobuf_rdata(&j->u.io.out);
    n = dyn_iobuf_rlen(&j->u.io.out);
    if (dyn_utf8_valid(p, n))
        return;
    need = dyn_utf8_lossy_into(p, n, NULL, &changed);
    j->u.io.fixed = (uint8_t*)malloc(need);
    if (!j->u.io.fixed) {
        j->u.io.rc = -3;
        return;
    }
    j->u.io.fixed_len = dyn_utf8_lossy_into(p, n, j->u.io.fixed, &changed);
}

static void dyn_fa_kill(JSContext* ctx, JSRuntime* rt, dyn_fa_job_t* j)
{
    if (ctx) {
        JSValue e = JS_NewError(ctx), r;
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message",
                JS_NewString(ctx, "dyna:file request aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst*)&e);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValueRT(rt, j->resolve);
    JS_FreeValueRT(rt, j->reject);
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
    j->dead = 1;
    j->ctx = NULL;
    if (j->op == DYN_FA_HANDLE)
        dyn_fah_kill(rt, j);
    if (j->held) {
        j->held = 0;
        dyn_net_reactor_release_rt(rt);
    }
}

static void dyn_fa_rt_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_fa_rt_t *h = (dyn_fa_rt_t*)opaque, **pp;
    dyn_fa_job_t* j;
    for (j = dyn_fa_jobs; j; j = j->next)
        if (j->rt == rt && !j->dead)
            dyn_fa_kill(ctx, rt, j);
    for (pp = &dyn_fa_rts; *pp; pp = &(*pp)->next)
        if (*pp == h) {
            *pp = h->next;
            break;
        }
    free(h);
    dyn_net_reactor_weak_drop(rt);
}

static int dyn_fa_rt_arm(JSRuntime* rt)
{
    dyn_fa_rt_t* h;
    for (h = dyn_fa_rts; h; h = h->next)
        if (h->rt == rt)
            return 0;
    h = (dyn_fa_rt_t*)malloc(sizeof(*h));
    if (!h)
        return -1;
    h->rt = rt;
    if (JS_AddShutdownSweep(rt, dyn_fa_rt_sweep, h) < 0) {
        free(h);
        return -1;
    }
    h->next = dyn_fa_rts;
    dyn_fa_rts = h;
    dyn_net_reactor_weak_hold();
    return 0;
}

static JSValue dyn_fa_result(JSContext* ctx, dyn_fa_job_t* j, int* bad)
{
    const char* op = j->name;
    *bad = 1;
    if (j->op == DYN_FA_HANDLE)
        return dyn_fah_result(ctx, j, bad);
    if (j->op == DYN_FA_FS) {
        *bad = 0;
        return dyn_fs_req_result(ctx, &j->u.fs);
    }
    if (j->op == DYN_FA_WRITE) {
        if (j->u.io.err)
            return dyn_fs_throw(ctx, j->u.io.err, op, j->u.io.stage == 2 ? NULL : j->u.io.path);
        *bad = 0;
        return JS_NewInt64(ctx, (int64_t)j->u.io.len);
    }
    if (j->u.io.rc == -3)
        return JS_ThrowOutOfMemory(ctx);
    if (j->u.io.rc == -1)
        return dyn_fs_throw(ctx, j->u.io.err, op, j->u.io.path);
    if (j->u.io.rc < 0) {
        if (j->u.io.err == EFBIG)
            return JS_ThrowRangeError(ctx, "%s: file exceeds %u bytes (DYN_MAX_INPUT)",
                op, (unsigned)DYN_MAX_INPUT);
        return dyn_fs_throw(ctx, j->u.io.err ? j->u.io.err : EIO, op, j->u.io.path);
    }
    *bad = 0;
    if (j->op == DYN_FA_BYTES)
        return dyn_file_bytes_take(ctx, &j->u.io.out);
    if (j->u.io.fixed)
        return JS_NewStringLen(ctx, (const char*)j->u.io.fixed, j->u.io.fixed_len);
    return JS_NewStringLen(ctx, (const char*)dyn_iobuf_rdata(&j->u.io.out), dyn_iobuf_rlen(&j->u.io.out));
}

static void dyn_fa_done(void* arg)
{
    dyn_fa_job_t* j = (dyn_fa_job_t*)arg;
    JSContext* ctx = j->ctx;
    JSValue v, r;
    int bad;

    if (j->dead) {
        dyn_fa_free(j);
        return;
    }
    v = dyn_fa_result(ctx, j, &bad);
    if (JS_IsException(v)) {
        if (JS_HasUncatchableException(ctx))
            bad = -1;
        else {
            v = JS_GetException(ctx);
            bad = 1;
        }
    }
    if (bad >= 0) {
        r = JS_Call(ctx, bad ? j->reject : j->resolve, JS_UNDEFINED, 1, (JSValueConst*)&v);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, v);
    }
    if (j->op == DYN_FA_HANDLE)
        dyn_fah_after(ctx, j);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    if (j->held)
        dyn_net_reactor_release_rt(j->rt);
    dyn_fa_free(j);
}

static dyn_fa_job_t* dyn_fa_new(JSContext* ctx, JSValueConst pathv, int op)
{
    size_t plen = 0;
    dyn_fa_job_t* j;
    const char* p = dyn_path_arg_len(ctx, pathv, "path", &plen);
    if (!p)
        return NULL;
    j = (dyn_fa_job_t*)malloc(sizeof(*j) + plen + 1);
    if (!j) {
        dyn_path_unborrow(ctx, p);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memset(j, 0, sizeof(*j));
    dyn_iobuf_init(&j->u.io.out);
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
    j->op = op;
    j->name = dyn_fa_names[op];
    j->u.io.path = (char*)(j + 1);
    memcpy((char*)(j + 1), p, plen);
    ((char*)(j + 1))[plen] = 0;
    dyn_path_unborrow(ctx, p);
    return j;
}

static JSValue dyn_fa_reject_now(JSContext* ctx, dyn_fa_job_t* j, JSValue promise)
{
    JSValue e, r;
    if (JS_HasUncatchableException(ctx)) {
        JS_FreeValue(ctx, j->resolve);
        JS_FreeValue(ctx, j->reject);
        JS_FreeValue(ctx, promise);
        dyn_fa_free(j);
        return JS_EXCEPTION;
    }
    e = JS_GetException(ctx);
    r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    dyn_fa_free(j);
    return promise;
}

JSValue dyn_fa_arm(JSContext* ctx, dyn_fa_job_t* j, int* armed)
{
    JSValue funcs[2], promise;

    *armed = 0;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        dyn_fa_free(j);
        return promise;
    }
    j->ctx = ctx;
    j->rt = JS_GetRuntime(ctx);
    j->resolve = funcs[0];
    j->reject = funcs[1];
    j->aio = dyn_net_reactor_acquire(ctx);
    if (!j->aio) {
        dyn_net_throw_no_reactor(ctx, j->name);
        return dyn_fa_reject_now(ctx, j, promise);
    }
    if (dyn_fa_rt_arm(j->rt) < 0 || dyn_net_on_reactor_free(dyn_fa_reap) != 0) {
        dyn_net_reactor_release_rt(j->rt);
        JS_FreeValue(ctx, j->resolve);
        JS_FreeValue(ctx, j->reject);
        JS_FreeValue(ctx, promise);
        dyn_fa_free(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    j->held = 1;
    j->linked = 1;
    j->prev = NULL;
    j->next = dyn_fa_jobs;
    if (dyn_fa_jobs)
        dyn_fa_jobs->prev = j;
    dyn_fa_jobs = j;
    *armed = 1;
    return promise;
}

int dyn_fa_offload(dyn_fa_job_t* j)
{
    return dyn_aio_offload(j->aio, dyn_fa_work, dyn_fa_done, j);
}

void dyn_fa_refuse(JSContext* ctx, dyn_fa_job_t* j)
{
    JSValue e, r;
    j->held = 0;
    dyn_net_reactor_release_rt(j->rt);
    JS_ThrowInternalError(ctx, "file.%s: the I/O pool refused the request", j->name);
    e = JS_GetException(ctx);
    r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    if (j->op == DYN_FA_HANDLE)
        dyn_fah_drop(ctx, j);
    dyn_fa_free(j);
}

static JSValue dyn_fa_submit(JSContext* ctx, dyn_fa_job_t* j)
{
    int armed;
    JSValue promise = dyn_fa_arm(ctx, j, &armed);
    if (armed && dyn_fa_offload(j) < 0)
        dyn_fa_refuse(ctx, j);
    return promise;
}

static JSValue dyn_fa_read_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_fa_job_t* j;
    int as_bytes = 0;
    (void)this_val;

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bytes" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bytes");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        as_bytes = b;
    }
    j = dyn_fa_new(ctx, argv[0], as_bytes ? DYN_FA_BYTES : DYN_FA_TEXT);
    if (!j)
        return JS_EXCEPTION;
    j->name = "readFile";
    return dyn_fa_submit(ctx, j);
}

static JSValue dyn_fa_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_fa_job_t* j;
    (void)this_val;
    (void)argc;

    j = dyn_fa_new(ctx, argv[0], DYN_FA_BYTES);
    if (!j)
        return JS_EXCEPTION;
    return dyn_fa_submit(ctx, j);
}

static int dyn_fa_write_opts(JSContext* ctx, JSValueConst o, dyn_fa_job_t* j)
{
    static const char* const keys[] = { "append", "atomic" };
    JSValue v;
    int b;
    if (dyn_opts_strict(ctx, o, keys, 2))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "append");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (b < 0)
        return -1;
    j->u.io.append = b;
    v = JS_GetPropertyStr(ctx, o, "atomic");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (b < 0)
        return -1;
    if (b && j->u.io.append) {
        JS_ThrowTypeError(ctx, "writeFile: atomic and append cannot be combined");
        return -1;
    }
    j->u.io.atomic = b;
    return 0;
}

static JSValue dyn_fa_write_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str = NULL;
    const uint8_t* data = NULL;
    size_t len = 0;
    dyn_fa_job_t* j;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, options])");
    j = dyn_fa_new(ctx, argv[0], DYN_FA_WRITE);
    if (!j)
        return JS_EXCEPTION;
    if (dyn_file_payload(ctx, argv[1], &data, &len, &str)) {
        dyn_fa_free(j);
        return JS_EXCEPTION;
    }
    if (len) {
        j->u.io.data = (uint8_t*)malloc(len);
        if (!j->u.io.data) {
            if (str)
                JS_FreeCString(ctx, str);
            dyn_fa_free(j);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(j->u.io.data, data, len);
    }
    j->u.io.len = len;
    if (str)
        JS_FreeCString(ctx, str);
    if (argc > 2 && JS_IsObject(argv[2]) && dyn_fa_write_opts(ctx, argv[2], j) < 0) {
        dyn_fa_free(j);
        return JS_EXCEPTION;
    }
    return dyn_fa_submit(ctx, j);
}

static JSValue dyn_fa_fs(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_fs_req_t q;
    dyn_fa_job_t* j;
    size_t l1, l2;
    char* s;
    (void)this_val;

    memset(&q, 0, sizeof(q));
    if (dyn_fs_req_parse(ctx, magic, argc, argv, &q) < 0)
        return JS_EXCEPTION;
    l1 = strlen(q.path);
    l2 = q.path2 ? strlen(q.path2) : 0;
    j = (dyn_fa_job_t*)malloc(sizeof(*j) + l1 + l2 + 2);
    if (!j) {
        dyn_fs_req_unborrow(ctx, &q);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(j, 0, sizeof(*j));
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
    j->op = DYN_FA_FS;
    j->name = dyn_fs_op_names[magic];
    j->u.fs.op = magic;
    j->u.fs.flag = q.flag;
    j->u.fs.mode = q.mode;
    s = (char*)(j + 1);
    memcpy(s, q.path, l1 + 1);
    j->u.fs.path = s;
    if (q.path2) {
        memcpy(s + l1 + 1, q.path2, l2 + 1);
        j->u.fs.path2 = s + l1 + 1;
    }
    dyn_fs_req_unborrow(ctx, &q);
    return dyn_fa_submit(ctx, j);
}

static const JSCFunctionListEntry dyn_file_async_funcs[] = {
    JS_CFUNC_DEF("readFile", 1, dyn_fa_read_file),
    JS_CFUNC_DEF("readBytes", 1, dyn_fa_read_bytes),
    JS_CFUNC_DEF("writeFile", 2, dyn_fa_write_file),
    JS_CFUNC_MAGIC_DEF("stat", 1, dyn_fa_fs, DYN_FS_STAT),
    JS_CFUNC_MAGIC_DEF("lstat", 1, dyn_fa_fs, DYN_FS_LSTAT),
    JS_CFUNC_MAGIC_DEF("exists", 1, dyn_fa_fs, DYN_FS_EXISTS),
    JS_CFUNC_MAGIC_DEF("readDir", 1, dyn_fa_fs, DYN_FS_READDIR),
    JS_CFUNC_MAGIC_DEF("makeDir", 1, dyn_fa_fs, DYN_FS_MKDIR),
    JS_CFUNC_MAGIC_DEF("remove", 1, dyn_fa_fs, DYN_FS_REMOVE),
    JS_CFUNC_MAGIC_DEF("removeAll", 1, dyn_fa_fs, DYN_FS_REMOVEALL),
    JS_CFUNC_MAGIC_DEF("rename", 2, dyn_fa_fs, DYN_FS_RENAME),
    JS_CFUNC_MAGIC_DEF("copyFile", 2, dyn_fa_fs, DYN_FS_COPY),
    JS_CFUNC_MAGIC_DEF("move", 2, dyn_fa_fs, DYN_FS_MOVE),
    JS_CFUNC_MAGIC_DEF("symlink", 2, dyn_fa_fs, DYN_FS_SYMLINK),
    JS_CFUNC_MAGIC_DEF("readLink", 1, dyn_fa_fs, DYN_FS_READLINK),
    JS_CFUNC_MAGIC_DEF("realPath", 1, dyn_fa_fs, DYN_FS_REALPATH),
    JS_CFUNC_MAGIC_DEF("chmod", 2, dyn_fa_fs, DYN_FS_CHMOD),
};

static int dyn_file_export_async(JSContext* ctx, JSModuleDef* m)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, o, dyn_file_async_funcs,
            (int)countof(dyn_file_async_funcs))
        < 0) {
        JS_FreeValue(ctx, o);
        return -1;
    }
    if (dyn_fah_init(ctx, o) < 0) {
        JS_FreeValue(ctx, o);
        return -1;
    }
    return JS_SetModuleExport(ctx, m, "async", o);
}

JSClassID dyn_flock_class_id;

static const JSCFunctionListEntry dyn_flock_proto[] = {
    JS_CFUNC_DEF("withLock", 1, dyn_flock_with_lock),
};

static const JSClassDef dyn_flock_class = {
    "FileLock",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_file_funcs[] = {
    JS_CFUNC_DEF("readFile", 1, dyn_file_read_file),
    JS_CFUNC_DEF("readBytes", 1, dyn_file_read_bytes),
    JS_CFUNC_DEF("writeFile", 2, dyn_file_write_file),
    JS_CFUNC_DEF("stat", 1, dyn_fs_stat),
    JS_CFUNC_DEF("lstat", 1, dyn_fs_lstat),
    JS_CFUNC_DEF("exists", 1, dyn_fs_exists),
    JS_CFUNC_DEF("readDir", 1, dyn_fs_read_dir),
    JS_CFUNC_DEF("makeDir", 1, dyn_fs_make_dir),
    JS_CFUNC_DEF("remove", 1, dyn_fs_remove),
    JS_CFUNC_DEF("removeAll", 1, dyn_fs_remove_all),
    JS_CFUNC_DEF("rename", 2, dyn_fs_rename),
    JS_CFUNC_DEF("copyFile", 2, dyn_fs_copy_file),
    JS_CFUNC_DEF("move", 2, dyn_fs_move),
    JS_CFUNC_DEF("sniffType", 1, dyn_fs_sniff_type),
    JS_CFUNC_DEF("symlink", 2, dyn_fs_symlink),
    JS_CFUNC_DEF("readLink", 1, dyn_fs_read_link),
    JS_CFUNC_DEF("realPath", 1, dyn_fs_real_path),
    JS_CFUNC_DEF("chmod", 2, dyn_fs_chmod),
    JS_CFUNC_DEF("glob", 1, dyn_fs_glob),
    JS_CFUNC_DEF("tempDir", 0, dyn_fs_temp_dir),
    JS_CFUNC_DEF("makeTempDir", 0, dyn_fs_make_temp_dir),
    JS_CFUNC_DEF("makeTempFile", 0, dyn_fs_make_temp_file),
    JS_CFUNC_MAGIC_DEF("dataDir", 0, dyn_pdir_get, 0),
    JS_CFUNC_MAGIC_DEF("configDir", 0, dyn_pdir_get, 1),
    JS_CFUNC_MAGIC_DEF("cacheDir", 0, dyn_pdir_get, 2),
    JS_CFUNC_MAGIC_DEF("dataDirSite", 0, dyn_pdir_get, 4),
    JS_CFUNC_MAGIC_DEF("configDirSite", 0, dyn_pdir_get, 5),
    JS_CFUNC_MAGIC_DEF("cacheDirSite", 0, dyn_pdir_get, 6),
};

JSClassID dyn_fh_class_id;

static const JSClassDef dyn_fh_class = {
    "File",
    .finalizer = dyn_fh_finalizer,
    .gc_mark = dyn_fh_mark,
};

DYN_FH_FORWARD(dyn_fh_read_text, dyn_file_read_file, 0)

DYN_FH_FORWARD(dyn_fh_read_bytes, dyn_file_read_bytes, 0)

DYN_FH_FORWARD(dyn_fh_write_text, dyn_file_write_file, 2)

DYN_FH_FORWARD(dyn_fh_stat, dyn_fs_stat, 0)

DYN_FH_FORWARD(dyn_fh_lstat, dyn_fs_lstat, 0)

DYN_FH_FORWARD(dyn_fh_exists, dyn_fs_exists, 0)

DYN_FH_FORWARD(dyn_fh_remove, dyn_fs_remove, 0)

DYN_FH_FORWARD(dyn_fh_real_path, dyn_fs_real_path, 0)

DYN_FH_FORWARD(dyn_fh_chmod, dyn_fs_chmod, 1)

static const JSCFunctionListEntry dyn_fh_proto[] = {
    JS_CGETSET_DEF("path", dyn_fh_get_path, NULL),
    JS_CFUNC_DEF("readText", 0, dyn_fh_read_text),
    JS_CFUNC_DEF("readBytes", 0, dyn_fh_read_bytes),
    JS_CFUNC_DEF("writeText", 1, dyn_fh_write_text),
    JS_CFUNC_DEF("writeBytes", 1, dyn_fh_write_text),
    JS_CFUNC_DEF("append", 1, dyn_fh_append),
    JS_CFUNC_DEF("stat", 0, dyn_fh_stat),
    JS_CFUNC_DEF("lstat", 0, dyn_fh_lstat),
    JS_CFUNC_DEF("exists", 0, dyn_fh_exists),
    JS_CFUNC_DEF("remove", 0, dyn_fh_remove),
    JS_CFUNC_DEF("realPath", 0, dyn_fh_real_path),
    JS_CFUNC_DEF("chmod", 1, dyn_fh_chmod),
    JS_CFUNC_DEF("moveTo", 1, dyn_fh_move_to),
    JS_CFUNC_DEF("copyTo", 1, dyn_fh_copy_to),
    JS_CFUNC_MAGIC_DEF("reader", 0, dyn_fh_stream, 0),
    JS_CFUNC_MAGIC_DEF("writer", 0, dyn_fh_stream, 1),
    JS_CFUNC_DEF("toString", 0, dyn_fh_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_fh_to_string),
};

JSClassID dyn_gl_class_id;

static const JSClassDef dyn_gl_class = {
    "Glob",
    .finalizer = dyn_gl_finalizer,
};

static const JSCFunctionListEntry dyn_gl_proto[] = {
    JS_CFUNC_DEF("matches", 1, dyn_gl_matches),
    JS_CFUNC_DEF("expand", 0, dyn_gl_expand),
    JS_CFUNC_DEF("filter", 1, dyn_gl_filter),
    JS_CGETSET_MAGIC_DEF("pattern", dyn_gl_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("hasWildcard", dyn_gl_get, NULL, 1),
};

static int dyn_register_path_class(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    char sep[2] = { DYN_PATH_SEP, 0 };
    char delim[2] = { DYN_PATH_DELIM, 0 };

    JS_NewClassID(&dyn_path_class_id);
    if (JS_NewClass(rt, dyn_path_class_id, &dyn_path_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_path_proto,
        (int)countof(dyn_path_proto));
    JS_SetClassProto(ctx, dyn_path_class_id, proto);

    ctor = JS_NewCFunction2(ctx, dyn_path_ctor, "Path", 1,
        JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, ctor, "cwd",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "cwd", 0,
            JS_CFUNC_generic_magic, 0),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "home",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "home", 0,
            JS_CFUNC_generic_magic, 1),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "temp",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "temp", 0,
            JS_CFUNC_generic_magic, 2),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "isPath",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "isPath", 1,
            JS_CFUNC_generic_magic, 3),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "sep", JS_NewString(ctx, sep), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "delimiter", JS_NewString(ctx, delim), JS_PROP_C_W_E);
    return JS_SetModuleExport(ctx, m, "Path", ctor);
}

static int dyn_file_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_path_class(ctx, m) < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_fh_class_id, &dyn_fh_class,
            dyn_fh_proto, (int)countof(dyn_fh_proto),
            dyn_fh_ctor, "File")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_gl_class_id, &dyn_gl_class,
            dyn_gl_proto, (int)countof(dyn_gl_proto),
            dyn_gl_ctor, "Glob")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_gl_class_id);
        JSValue ctor;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        if (JS_DefinePropertyValueStr(ctx, ctor, "match",
                JS_NewCFunction(ctx, dyn_gl_static_match, "match", 2),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_class(ctx, m, &dyn_freader_class_id, &dyn_freader_class,
            dyn_freader_proto, countof(dyn_freader_proto),
            dyn_freader_ctor, "FileReader")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_fwriter_class_id, &dyn_fwriter_class,
            dyn_fwriter_proto, countof(dyn_fwriter_proto),
            dyn_fwriter_ctor, "FileWriter")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_watch_class_id, &dyn_watch_class,
            dyn_watch_proto, countof(dyn_watch_proto),
            dyn_watch_ctor, "Watcher")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_flock_class_id, &dyn_flock_class,
            dyn_flock_proto, countof(dyn_flock_proto),
            dyn_flock_ctor, "FileLock")
        < 0)
        return -1;
    if (dyn_file_export_async(ctx, m) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_file_funcs,
        (int)countof(dyn_file_funcs));
}

int js_nat_init_file(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:file", dyn_file_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Path");
    JS_AddModuleExport(ctx, m, "File");
    JS_AddModuleExport(ctx, m, "Glob");
    JS_AddModuleExport(ctx, m, "FileReader");
    JS_AddModuleExport(ctx, m, "FileWriter");
    JS_AddModuleExport(ctx, m, "Watcher");
    JS_AddModuleExport(ctx, m, "FileLock");
    JS_AddModuleExport(ctx, m, "async");
    JS_AddModuleExportList(ctx, m, dyn_file_funcs, (int)countof(dyn_file_funcs));
    return 0;
}

#endif
