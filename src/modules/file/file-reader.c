#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

static void dyn_freader_dispose(void* native)
{
    dyn_freader_t* r = (dyn_freader_t*)native;
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    free(r);
}

static ssize_t dyn_freader_fill(dyn_freader_t* r)
{
    ssize_t n;
    if (r->start < r->end)
        return (ssize_t)(r->end - r->start);
    r->start = r->end = 0;
    if (r->eof)
        return 0;
    for (;;) {
        n = read(r->fd, r->buf, r->cap);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        break;
    }
    r->end = (size_t)n;
    if (n == 0)
        r->eof = 1;
    return n;
}

JSValue dyn_freader_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    const char* path;
    int64_t bufsize = 0;
    struct stat st;
    int fd;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileReader(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bufferSize" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    fd = dyn_fs_path_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "FileReader", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    if (fstat(fd, &st) == 0)
        dyn_io_advise_seq_read(fd, st.st_size);
    dyn_path_unborrow(ctx, path);

    r = (dyn_freader_t*)calloc(1, sizeof(*r));
    if (!r) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->fd = fd;
    r->cap = dyn_file_clamp_bufsize(bufsize);
    r->buf = (unsigned char*)malloc(r->cap);
    if (!r->buf) {
        close(fd);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_freader_class_id, r, dyn_freader_dispose);
}

static JSValue dyn_freader_read_raw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    const char* op, int as_bytes)
{
    dyn_freader_t* r;
    int64_t want = -1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &want, argv[0]))
            return JS_EXCEPTION;
    }
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    {
        char* acc = NULL;
        size_t acc_len = 0, acc_cap = 0;
        JSValue out;
        for (;;) {
            size_t avail, take;
            ssize_t f;
            if (want >= 0 && (int64_t)acc_len >= want)
                break;
            f = dyn_freader_fill(r);
            if (f < 0) {
                free(acc);
                return JS_ThrowInternalError(ctx, "FileReader: read error");
            }
            if (f == 0)
                break;
            avail = r->end - r->start;
            take = avail;
            if (want >= 0 && take > (size_t)(want - (int64_t)acc_len))
                take = (size_t)(want - (int64_t)acc_len);
            if (acc_len + take + 1 > acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8192;
                char* na;
                while (nc < acc_len + take + 1)
                    nc *= 2;
                if (nc > DYN_MAX_INPUT) {
                    free(acc);
                    return JS_ThrowRangeError(ctx,
                        "%s: accumulated read exceeds %u bytes"
                        " (DYN_MAX_INPUT)",
                        op, (unsigned)DYN_MAX_INPUT);
                }
                na = (char*)realloc(acc, nc);
                if (!na) {
                    free(acc);
                    return JS_ThrowOutOfMemory(ctx);
                }
                acc = na;
                acc_cap = nc;
            }
            memcpy(acc + acc_len, r->buf + r->start, take);
            acc_len += take;
            r->start += take;
        }
        if (as_bytes)
            out = dyn_nat_u8_copy(ctx, (const uint8_t*)(acc ? acc : ""),
                acc_len);
        else
            out = dyn_utf8_lossy_string(ctx, (const uint8_t*)(acc ? acc : ""), acc_len);
        free(acc);
        return out;
    }
}

JSValue dyn_freader_read(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return dyn_freader_read_raw(ctx, this_val, argc, argv, "FileReader.read", 0);
}

JSValue dyn_freader_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_freader_read_raw(ctx, this_val, argc, argv,
        "FileReader.readBytes", 1);
}

JSValue dyn_freader_read_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    JSValue ab;
    uint8_t *base, *dst;
    size_t off, len, bpe, ab_size, total = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx,
            "FileReader.readInto(buf) requires a Uint8Array (or any "
            "TypedArray/DataView)");
    if (JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
    ab = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, ab);
    if (!base) {
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    if (off > ab_size || len > ab_size - off) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r) {
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    dst = base + off;
    while (total < len) {
        size_t avail, take;
        ssize_t f = dyn_freader_fill(r);
        if (f < 0) {
            JS_FreeValue(ctx, ab);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break;
        avail = r->end - r->start;
        take = avail < len - total ? avail : len - total;
        memcpy(dst + total, r->buf + r->start, take);
        r->start += take;
        total += take;
    }
    JS_FreeValue(ctx, ab);
    return JS_NewInt64(ctx, (int64_t)total);
}

JSValue dyn_freader_read_line(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    char* acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    int saw_any = 0;
    JSValue out;

    (void)argc;
    (void)argv;
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    for (;;) {
        ssize_t f = dyn_freader_fill(r);
        unsigned char* nl;
        size_t avail, take;
        if (f < 0) {
            free(acc);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break;
        saw_any = 1;
        avail = r->end - r->start;
        nl = (unsigned char*)memchr(r->buf + r->start, '\n', avail);
        take = nl ? (size_t)(nl - (r->buf + r->start)) : avail;
        if (acc_len + take + 1 > acc_cap) {
            size_t nc = acc_cap ? acc_cap * 2 : 256;
            char* na;
            while (nc < acc_len + take + 1)
                nc *= 2;
            if (nc > DYN_MAX_INPUT) {
                free(acc);
                return JS_ThrowRangeError(ctx,
                    "FileReader.readLine: line exceeds %u bytes"
                    " (DYN_MAX_INPUT)",
                    (unsigned)DYN_MAX_INPUT);
            }
            na = (char*)realloc(acc, nc);
            if (!na) {
                free(acc);
                return JS_ThrowOutOfMemory(ctx);
            }
            acc = na;
            acc_cap = nc;
        }
        memcpy(acc + acc_len, r->buf + r->start, take);
        acc_len += take;
        r->start += take;
        if (nl) {
            r->start++;
            if (acc_len > 0 && acc[acc_len - 1] == '\r')
                acc_len--;
            out = dyn_utf8_lossy_string(ctx, (const uint8_t*)acc, acc_len);
            free(acc);
            return out;
        }
    }
    if (!saw_any && acc_len == 0) {
        free(acc);
        return JS_NULL;
    }
    out = dyn_utf8_lossy_string(ctx, (const uint8_t*)(acc ? acc : ""), acc_len);
    free(acc);
    return out;
}

#endif
