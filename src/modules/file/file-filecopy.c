#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

void dyn_fs_copy_run(dyn_fs_req_t* q)
{
    int in, out = -1, flags;
    struct stat st;

    in = dyn_fs_path_open(q->path, O_RDONLY | O_CLOEXEC, 0);
    if (in < 0) {
        q->err = errno;
        return;
    }
    if (fstat(in, &st) != 0) {
        q->err = errno;
        goto out;
    }
    if (S_ISDIR(st.st_mode)) {
        q->ekind = DYN_FS_EK_ISDIR;
        goto out;
    }
    if (!S_ISREG(st.st_mode)) {
        q->ekind = DYN_FS_EK_NOTREG;
        goto out;
    }
    q->epath = q->path2;
    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (q->flag ? 0 : O_EXCL);
    out = dyn_fs_path_open(q->path2, flags, st.st_mode & 0777);
    if (out < 0) {
        q->err = errno;
        goto out;
    }
    if (q->flag) {
        struct stat dst;
        if (fstat(out, &dst) == 0 && dst.st_dev == st.st_dev && dst.st_ino == st.st_ino) {
            q->ekind = DYN_FS_EK_SAME;
            goto out;
        }
        if (ftruncate(out, 0) != 0) {
            q->err = errno;
            goto out;
        }
    }
    if (dyn_copy_fds(in, out) != 0) {
        q->err = errno;
        if (!q->flag)
            unlink(q->path2);
        goto out;
    }
    q->u.num = (int64_t)st.st_size;
out:
    close(in);
    if (out >= 0)
        close(out);
}

void dyn_fs_move_run(dyn_fs_req_t* q)
{
    int in, out = -1;
    struct stat st, st_to;

    if (rename(q->path, q->path2) == 0)
        return;
    if (errno != EXDEV) {
        q->err = errno;
        return;
    }
    in = dyn_fs_path_open(q->path, O_RDONLY | O_CLOEXEC, 0);
    if (in < 0) {
        q->err = errno;
        return;
    }
    if (fstat(in, &st) != 0) {
        q->err = errno;
        goto out;
    }
    q->epath = q->path2;
    out = dyn_fs_path_open(q->path2, O_WRONLY | O_CREAT | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) {
        q->err = errno;
        goto out;
    }
    if (fstat(out, &st_to) != 0) {
        q->err = errno;
        goto out;
    }
    if (st_to.st_dev == st.st_dev && st_to.st_ino == st.st_ino) {
        q->err = EINVAL;
        goto out;
    }
    if (ftruncate(out, 0) != 0 || dyn_copy_fds(in, out) != 0 || dyn_io_durable_sync(out) != 0) {
        q->err = errno;
        goto out;
    }
    close(in);
    in = -1;
    close(out);
    out = -1;
    if (unlink(q->path) != 0) {
        q->err = errno;
        q->epath = q->path;
    }
out:
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
}

JSValue dyn_fs_copy_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_COPY, argc, argv);
}

JSValue dyn_fs_move(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_MOVE, argc, argv);
}

JSValue dyn_fs_sniff_type(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t head[512];
    size_t n = 0;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sniffType(pathOrBytes): a Path or bytes "
                                      "is required");
    if (!dyn_is_path(argv[0]) && !JS_IsString(argv[0])) {
        size_t blen;
        uint8_t* bp = JS_GetArrayBuffer(ctx, &blen, argv[0]);
        if (!bp) {
            size_t off = 0, vlen = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, argv[0], &off, &vlen, NULL);
            if (JS_IsException(buf))
                return JS_ThrowTypeError(ctx, "sniffType: expected a Path or "
                                              "bytes");
            bp = JS_GetArrayBuffer(ctx, &blen, buf);
            JS_FreeValue(ctx, buf);
            if (!bp)
                return JS_ThrowTypeError(ctx, "sniffType: expected a Path or "
                                              "bytes");
            if (off > blen)
                off = blen;
            if (vlen > blen - off)
                vlen = blen - off;
            bp += off;
            blen = vlen;
        }
        return JS_NewString(ctx, dyn_sniff_bytes(bp, blen));
    } else {
        const char* path = dyn_path_arg(ctx, argv[0], "path");
        ssize_t got;
        int fd;
        if (!path)
            return JS_EXCEPTION;
        fd = dyn_fs_path_open(path, O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "sniffType", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
        got = read(fd, head, sizeof head);
        close(fd);
        dyn_path_unborrow(ctx, path);
        if (got < 0)
            return dyn_fs_throw(ctx, errno, "sniffType", NULL);
        n = (size_t)got;
    }
    return JS_NewString(ctx, dyn_sniff_bytes(head, n));
}

#endif
