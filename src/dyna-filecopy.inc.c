/* copyFile, move and sniffType for dyna:file (design 23).
   The copy goes through the kernel where the kernel offers it -- fcopyfile on
   Darwin, copy_file_range on Linux -- and falls back to a read/write loop
   everywhere else, because both of those refuse in cases the fallback handles
   (a pipe, a filesystem that cannot clone, an old kernel). */

#if defined(__APPLE__)
#include <copyfile.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

#define DYN_COPY_BUF (256 * 1024)       /* one syscall per 256 KiB on the
                                           fallback path, not per 4 KiB */

/* The fallback every platform shares. Returns 0, or -1 with errno set. */
static int dyn_copy_loop(int in, int out)
{
    char *buf = (char *)malloc(DYN_COPY_BUF);
    int rc = 0;

    if (!buf) {
        errno = ENOMEM;
        return -1;
    }
    for (;;) {
        ssize_t n = read(in, buf, DYN_COPY_BUF), off = 0;
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        while (off < n) {           /* a short write is not an error */
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                goto done;
            }
            off += w;
        }
    }
done:
    free(buf);
    return rc;
}

/* Copy the CONTENT of `in` to `out`, both already open. */
static int dyn_copy_fds(int in, int out)
{
#if defined(__APPLE__)
    /* fcopyfile moves the data inside the kernel and will clone the extents
       outright on APFS. It refuses on some descriptor kinds; fall through. */
    if (fcopyfile(in, out, NULL, COPYFILE_DATA) == 0)
        return 0;
    if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
        return -1;
#elif defined(__linux__)
    {
        struct stat st;
        if (fstat(in, &st) == 0 && S_ISREG(st.st_mode)) {
            off_t done = 0;
            int ok = 1;
            while (done < st.st_size) {
                ssize_t n = (ssize_t)syscall(__NR_copy_file_range, in, NULL,
                                             out, NULL,
                                             (size_t)(st.st_size - done), 0u);
                if (n > 0) { done += n; continue; }
                if (n == 0) break;
                ok = 0;             /* EXDEV, ENOSYS, EINVAL: use the loop */
                break;
            }
            if (ok && done == st.st_size)
                return 0;
            if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
                return -1;
        }
    }
#endif
    return dyn_copy_loop(in, out);
}

/* copyFile(from, to, {overwrite?}) -> bytes copied.
   Refusing an existing destination is the default: a copy that silently
   replaces a file is the one nobody notices until the file is gone. */
static JSValue dyn_fs_copy_file(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *from, *to;
    int in = -1, out = -1, overwrite = 0, flags, e;
    struct stat st;
    JSValue r;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "copyFile(from, to): both are required");
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "overwrite");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        overwrite = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    from = dyn_path_arg(ctx, argv[0], "from");
    if (!from)
        return JS_EXCEPTION;
    to = dyn_path_arg(ctx, argv[1], "to");
    if (!to) {
        dyn_path_unborrow(ctx, from);
        return JS_EXCEPTION;
    }
    in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) { e = errno; r = dyn_fs_throw(ctx, e, "copyFile", from); goto out; }
    if (fstat(in, &st) != 0) { e = errno; r = dyn_fs_throw(ctx, e, "copyFile", from); goto out; }
    if (S_ISDIR(st.st_mode)) {
        r = JS_ThrowTypeError(ctx, "copyFile: '%s' is a directory", from);
        goto out;
    }
    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (overwrite ? O_TRUNC : O_EXCL);
    /* The source's permission bits, minus the umask -- a copy of a private
       key that lands world-readable is the failure worth designing against. */
    out = open(to, flags, st.st_mode & 0777);
    if (out < 0) { e = errno; r = dyn_fs_throw(ctx, e, "copyFile", to); goto out; }
    if (dyn_copy_fds(in, out) != 0) {
        e = errno;
        r = dyn_fs_throw(ctx, e, "copyFile", to);
        goto out;
    }
    r = JS_NewInt64(ctx, (int64_t)st.st_size);
out:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    dyn_path_unborrow(ctx, from);
    dyn_path_unborrow(ctx, to);
    return r;
}

/* move(from, to) -> undefined.
   rename(2) is atomic but only WITHIN one filesystem; across one it fails
   EXDEV, and that is the case no test starting in a single directory ever
   reaches. The fallback is copy-then-unlink, which is not atomic and says so. */
static JSValue dyn_fs_move(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    const char *from, *to;
    int in = -1, out = -1, e;
    struct stat st;
    JSValue r = JS_UNDEFINED;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "move(from, to): both are required");
    from = dyn_path_arg(ctx, argv[0], "from");
    if (!from)
        return JS_EXCEPTION;
    to = dyn_path_arg(ctx, argv[1], "to");
    if (!to) {
        dyn_path_unborrow(ctx, from);
        return JS_EXCEPTION;
    }
    if (rename(from, to) == 0)
        goto out;
    if (errno != EXDEV) {
        e = errno;
        r = dyn_fs_throw(ctx, e, "move", from);
        goto out;
    }
    in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) { e = errno; r = dyn_fs_throw(ctx, e, "move", from); goto out; }
    if (fstat(in, &st) != 0) { e = errno; r = dyn_fs_throw(ctx, e, "move", from); goto out; }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) { e = errno; r = dyn_fs_throw(ctx, e, "move", to); goto out; }
    if (dyn_copy_fds(in, out) != 0) {
        e = errno;
        r = dyn_fs_throw(ctx, e, "move", to);
        goto out;
    }
    close(in); in = -1;
    close(out); out = -1;
    /* Unlink LAST: if it fails the data still exists at both paths, which is
       recoverable. Unlinking first and then failing the write is not. */
    if (unlink(from) != 0) {
        e = errno;
        r = dyn_fs_throw(ctx, e, "move", from);
    }
out:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    dyn_path_unborrow(ctx, from);
    dyn_path_unborrow(ctx, to);
    return r;
}

/* ---- content sniffing ---------------------------------------------------
   Magic bytes, not the extension: the extension is what the sender claims and
   the bytes are what arrived. Ordered longest-prefix first so a container that
   shares a short prefix cannot shadow a longer, more specific one. */

typedef struct {
    uint8_t     len;
    uint8_t     off;
    uint8_t     magic[12];
    const char *mime;
} dyn_sniff_t;

static const dyn_sniff_t DYN_SNIFF[] = {
    { 8, 0, { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A }, "image/png" },
    { 6, 0, { 'G','I','F','8','7','a' },              "image/gif" },
    { 6, 0, { 'G','I','F','8','9','a' },              "image/gif" },
    { 4, 0, { 'O','g','g','S' },                      "audio/ogg" },
    { 4, 0, { 'f','L','a','C' },                      "audio/flac" },
    { 4, 0, { 0x00,0x61,0x73,0x6D },                  "application/wasm" },
    { 4, 0, { 0x7F,'E','L','F' },                     "application/x-elf" },
    { 4, 0, { 0xCA,0xFE,0xBA,0xBE },                  "application/java-vm" },
    { 4, 0, { 0xFD,'7','z','X' },                     "application/x-xz" },
    { 4, 0, { 0x28,0xB5,0x2F,0xFD },                  "application/zstd" },
    { 4, 0, { 'P','K',0x03,0x04 },                    "application/zip" },
    { 4, 0, { 'P','K',0x05,0x06 },                    "application/zip" },
    { 4, 0, { '%','P','D','F' },                      "application/pdf" },
    { 4, 0, { 'I','I','*',0x00 },                     "image/tiff" },
    { 4, 0, { 'M','M',0x00,'*' },                     "image/tiff" },
    { 4, 4, { 'f','t','y','p' },                      "video/mp4" },
    { 4, 0, { 'R','a','r','!' },                      "application/vnd.rar" },
    { 4, 0, { 'W','E','B','P' },                      "image/webp" },  /* off 8 */
    { 3, 0, { 0xFF,0xD8,0xFF },                       "image/jpeg" },
    { 3, 0, { 'B','Z','h' },                          "application/x-bzip2" },
    { 3, 0, { 'I','D','3' },                          "audio/mpeg" },
    { 2, 0, { 0x1F,0x8B },                            "application/gzip" },
    { 2, 0, { 'B','M' },                              "image/bmp" },
    { 2, 0, { 0x4D,0x5A },                            "application/x-msdownload" },
};

/* SQLite and tar carry their magic away from byte 0, so they are checked by
   hand rather than bloating every row of the table with an offset. */
static const char *dyn_sniff_bytes(const uint8_t *p, size_t n)
{
    size_t i;

    if (n >= 16 && memcmp(p, "SQLite format 3", 16) == 0)
        return "application/vnd.sqlite3";
    if (n >= 262 && memcmp(p + 257, "ustar", 5) == 0)
        return "application/x-tar";
    if (n >= 12 && memcmp(p, "RIFF", 4) == 0) {
        if (memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
        if (memcmp(p + 8, "WAVE", 4) == 0) return "audio/wav";
    }
    for (i = 0; i < sizeof DYN_SNIFF / sizeof DYN_SNIFF[0]; i++) {
        const dyn_sniff_t *s = &DYN_SNIFF[i];
        if (s->mime[0] == 'i' && s->magic[0] == 'W')
            continue;               /* WEBP is handled by the RIFF case above */
        if (n >= (size_t)s->off + s->len
            && memcmp(p + s->off, s->magic, s->len) == 0)
            return s->mime;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return "text/plain";        /* UTF-8 BOM */
    /* No magic matched. A NUL in the first block means binary; otherwise call
       it text -- which is a guess, and the caller is told so by the name. */
    for (i = 0; i < n; i++)
        if (p[i] == 0)
            return "application/octet-stream";
    return n ? "text/plain" : "application/octet-stream";
}

/* sniffType(pathOrBytes) -> string. Bytes are sniffed directly; a path is
   opened and its first block read. */
static JSValue dyn_fs_sniff_type(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint8_t head[512];
    size_t n = 0;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sniffType(pathOrBytes): a Path or bytes "
                                      "is required");
    if (!dyn_is_path(argv[0])) {        /* a Path, or bytes -- not a string */
        size_t blen;
        uint8_t *bp = JS_GetArrayBuffer(ctx, &blen, argv[0]);
        if (!bp) {
            JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, NULL, NULL);
            if (JS_IsException(buf))
                return JS_ThrowTypeError(ctx, "sniffType: expected a Path or "
                                              "bytes");
            bp = JS_GetArrayBuffer(ctx, &blen, buf);
            JS_FreeValue(ctx, buf);
            if (!bp)
                return JS_ThrowTypeError(ctx, "sniffType: expected a Path or "
                                              "bytes");
        }
        /* Sniff the caller's bytes IN PLACE. Copying them into a fixed
           512-byte buffer first would put any read past the logical length
           inside that buffer's spare capacity, where no sanitizer can see
           it -- which is exactly the bug class this code can have. */
        return JS_NewString(ctx, dyn_sniff_bytes(bp, blen));
    } else {
        const char *path = dyn_path_arg(ctx, argv[0], "path");
        ssize_t got;
        int fd;
        if (!path)
            return JS_EXCEPTION;
        fd = open(path, O_RDONLY | O_CLOEXEC);
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
