/*
 * dyna:uring -- io_uring disk I/O (Linux only, CONFIG_IO_URING).
 *
 *   import { readFile, readFileSync, checksum } from "dyna:uring";
 *
 *   const text = readFile("/etc/hostname");        // whole file via io_uring
 *   const same = readFileSync("/etc/hostname");     // pread reference
 *   const { bytes, sum } = checksum(path, true);    // uring; false => pread
 *
 * The reader submits many block reads at a high queue depth and reaps their
 * completions, so a large file is fetched with one submit/complete cycle per
 * batch instead of a blocking pread per block. Correctness is proven against
 * the pread path (identical bytes / identical checksum). A faithful THROUGHPUT
 * comparison needs O_DIRECT on real storage; on a page-cache-backed or
 * virtualized filesystem both paths hit cache and the syscall-count difference
 * dominates rather than device latency.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dyna-io.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_URING_DISK_QD 64            /* in-flight reads */
#define DYN_URING_DISK_BS (256 * 1024)  /* per-read block size */

typedef struct {
    off_t off; /* next byte of this chunk still to read */
    off_t end; /* one past this chunk's last byte */
} dyn_blk_t;

static int dyn_disk_submit(struct io_uring *ring, int fd, char *buf,
                             dyn_blk_t *ch, size_t i)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        int r = io_uring_submit(ring);
        if (r < 0) {
            errno = -r;               /* liburing returns -errno */
            return -1;
        }
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            errno = EAGAIN;           /* the ring stayed full after a flush */
            return -1;
        }
    }
    io_uring_prep_read(sqe, fd, buf + ch[i].off, (unsigned)(ch[i].end - ch[i].off),
                       (uint64_t)ch[i].off);
    io_uring_sqe_set_data64(sqe, i);
    return 0;
}

/* Read the entire file at `path` into a fresh malloc'd buffer using io_uring at
 * queue depth QD. Returns 0 (caller free()s *out) or -1 with errno set.
 * Internal to this module: the decl in dyna-nat.h is historical, and dyna:file
 * has its own reader and never calls this. */
int dyn_uring_read_all(const char *path, char **out, size_t *outlen)
{
    struct io_uring ring;
    struct stat st;
    dyn_blk_t *ch = NULL;
    char *buf = NULL;
    size_t size, nblocks, next, done, i;
    int fd, ring_ok = 0, rc = -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
        goto done;
    if ((size_t)st.st_size > DYN_MAX_INPUT) {
        errno = EFBIG;
        goto done;
    }
    size = (size_t)st.st_size;
    buf = (char *)malloc(size ? size : 1);
    if (!buf)
        goto done;
    if (size == 0) {
        *out = buf;
        *outlen = 0;
        buf = NULL;
        rc = 0;
        goto done;
    }

    {
        int r = io_uring_queue_init(DYN_URING_DISK_QD, &ring, 0);
        if (r < 0) {
            errno = -r;
            goto done;
        }
    }
    ring_ok = 1;

    nblocks = (size + DYN_URING_DISK_BS - 1) / DYN_URING_DISK_BS;
    ch = (dyn_blk_t *)malloc(nblocks * sizeof(*ch));
    if (!ch)
        goto done;
    for (i = 0; i < nblocks; i++) {
        ch[i].off = (off_t)(i * DYN_URING_DISK_BS);
        ch[i].end = (off_t)((i + 1) * DYN_URING_DISK_BS);
        if ((size_t)ch[i].end > size)
            ch[i].end = (off_t)size;
    }

    next = 0;
    done = 0;
    size_t total_read = 0;
    int saw_eof = 0;
    while (next < nblocks && (next < DYN_URING_DISK_QD)) {
        if (dyn_disk_submit(&ring, fd, buf, ch, next) < 0)
            goto done;
        next++;
    }
    if (io_uring_submit(&ring) < 0)
        goto done;                    /* submit() left errno set */

    while (done < nblocks) {
        struct io_uring_cqe *cqe;
        size_t bi;
        int res;
        int wrc = io_uring_wait_cqe(&ring, &cqe);
        if (wrc == -EINTR)
            continue;                 /* a signal must not abort the read --
                                         the pread path retries EINTR too */
        if (wrc < 0) {
            errno = -wrc;
            goto done;
        }
        bi = (size_t)io_uring_cqe_get_data64(cqe);
        res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        if (res < 0) {
            errno = -res;             /* the SQE carries the read error */
            goto done;
        }
        if (res == 0) {
            /* EOF before this chunk's end: the file shrank after fstat. The
               tail is left UNWRITTEN: *outlen reports total_read, so the
               discarded bytes past it are never read (the zero-fill here used
               to scrub up to 256 KiB that the length then threw away). */
            ch[bi].off = ch[bi].end;
            saw_eof = 1;
        } else {
            ch[bi].off += res;
            total_read += (size_t)res;
        }
        if (ch[bi].off < ch[bi].end) {
            if (dyn_disk_submit(&ring, fd, buf, ch, bi) < 0)
                goto done;
            if (io_uring_submit(&ring) < 0)
                goto done;            /* submit() left errno set */
        } else {
            done++;
            if (next < nblocks) {
                if (dyn_disk_submit(&ring, fd, buf, ch, next) < 0)
                    goto done;
                next++;
                if (io_uring_submit(&ring) < 0)
                    goto done;        /* submit() left errno set */
            }
        }
    }
    /* If the file shrank, return the actual bytes on disk, not the
       stale fstat size with zero padding or heap. The pread reference
       returns the short count, so match it: when EOF was seen the
       true size is total_read, otherwise fstat's size. */
    {
        size_t actual = saw_eof ? total_read : size;
        if (actual > size) actual = size;
        *out = buf;
        *outlen = actual;
        buf = NULL;
        rc = 0;
    }

 done:
    if (ring_ok)
        io_uring_queue_exit(&ring);
    free(ch);
    free(buf);
    close(fd);
    return rc;
}

/* Blocking pread reference reader (correctness oracle + perf baseline). */
static int dyn_pread_read_all(const char *path, char **out, size_t *outlen)
{
    struct stat st;
    char *buf = NULL;
    size_t size, off = 0;
    int fd, rc = -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
        goto done;
    if ((size_t)st.st_size > DYN_MAX_INPUT) {
        errno = EFBIG;
        goto done;
    }
    size = (size_t)st.st_size;
    buf = (char *)malloc(size ? size : 1);
    if (!buf)
        goto done;
    while (off < size) {
        ssize_t r = pread(fd, buf + off, size - off, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    *out = buf;
    *outlen = off;
    buf = NULL;
    rc = 0;

 done:
    free(buf);
    close(fd);
    return rc;
}

static JSValue dyn_uring_read_common(JSContext *ctx, JSValueConst path_val,
                                     int use_uring)
{
    const char *path;
    char *data = NULL;
    size_t len = 0;
    JSValue result;

    path = dyn_path_borrow(ctx, path_val, "path", NULL);
    if (!path)
        return JS_EXCEPTION;
    if ((use_uring ? dyn_uring_read_all(path, &data, &len)
                   : dyn_pread_read_all(path, &data, &len)) < 0) {
        return JS_ThrowInternalError(ctx,
            "dyna:uring: %s read failed: errno %d (%s)",
            use_uring ? "io_uring" : "pread fallback",
            errno, strerror(errno));
    }
    result = JS_NewStringLen(ctx, data ? data : "", len);
    free(data);
    return result;
}

static JSValue dyn_uring_read_file(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_uring_read_common(ctx, argv[0], 1);
}

static JSValue dyn_uring_read_file_sync(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_uring_read_common(ctx, argv[0], 0);
}

/* Read a whole file and return { bytes, sum } (a 32-bit rolling checksum),
 * forcing every byte to be touched without materialising a giant JS string --
 * for benchmarking the read path. Second arg true => io_uring, false => pread. */
static JSValue dyn_uring_checksum(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    char *data = NULL;
    size_t len = 0, i;
    uint32_t sum = 2166136261u; /* FNV-1a basis */
    int use_uring = 1;
    JSValue obj;

    (void)this_val;
    if (argc > 1)
        use_uring = JS_ToBool(ctx, argv[1]);
    path = dyn_path_borrow(ctx, argv[0], "path", NULL);
    if (!path)
        return JS_EXCEPTION;
    if ((use_uring ? dyn_uring_read_all(path, &data, &len)
                   : dyn_pread_read_all(path, &data, &len)) < 0) {
        return JS_ThrowInternalError(ctx,
            "dyna:uring: %s read failed: errno %d (%s)",
            use_uring ? "io_uring" : "pread fallback",
            errno, strerror(errno));
    }
    for (i = 0; i < len; i++) {
        sum ^= (uint8_t)data[i];
        sum *= 16777619u;
    }
    free(data);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueStr(ctx, obj, "bytes",
                              JS_NewInt64(ctx, (int64_t)len), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "sum",
                              JS_NewUint32(ctx, sum), JS_PROP_C_W_E);
    return obj;
}

static const JSCFunctionListEntry dyn_uring_funcs[] = {
    JS_CFUNC_DEF("readFile", 1, dyn_uring_read_file),
    JS_CFUNC_DEF("readFileSync", 1, dyn_uring_read_file_sync),
    JS_CFUNC_DEF("checksum", 1, dyn_uring_checksum),
};

static int dyn_uring_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_uring_funcs,
                                  (int)countof(dyn_uring_funcs));
}

int js_nat_init_uring(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:uring", dyn_uring_init_module);
    if (!m)
        return -1;
    JS_AddModuleExportList(ctx, m, dyn_uring_funcs, (int)countof(dyn_uring_funcs));
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_IO_URING && __linux__ */
