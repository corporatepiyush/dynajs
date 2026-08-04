/*
 * dyna-io -- shared disk/network I/O substrate (see dyna-io.h).
 * Engine-core: always compiled, no JSContext / native-module dependency.
 */
#include "dyna-io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Files at least this large are mmap'd (zero read-copy) rather than read into a
 * heap buffer; below it the syscall + fault setup does not pay off. */
#define DYN_IO_MMAP_MIN (64u * 1024u)

/* ==================================================================== *
 *  dyn_iobuf_t                                                          *
 * ==================================================================== */

void dyn_iobuf_init(dyn_iobuf_t *b)
{
    b->data = b->inln;
    b->len = 0;
    b->rpos = 0;
    b->cap = DYN_IOBUF_INLINE_CAP;
    b->kind = DYN_IOBUF_INLINE;
}

int dyn_iobuf_reserve(dyn_iobuf_t *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;

    if (need < b->len) /* size_t overflow */
        return -1;
    if (need <= b->cap)
        return 0;

    nc = b->cap ? b->cap : DYN_IOBUF_INLINE_CAP;
    if (nc < 256)
        nc = 256;
    while (nc < need) {
        size_t d = nc << 1;
        if (d < nc) { /* doubling overflowed: take exactly what's needed */
            nc = need;
            break;
        }
        nc = d;
    }

    if (b->kind == DYN_IOBUF_HEAP) {
        uint8_t *nd = (uint8_t *)realloc(b->data, nc);
        if (!nd)
            return -1;
        b->data = nd;
        b->cap = nc;
        return 0;
    }
    /* inline or mmap view: move into a fresh heap block */
    {
        uint8_t *nd = (uint8_t *)malloc(nc);
        if (!nd)
            return -1;
        if (b->len)
            memcpy(nd, b->data, b->len);
        if (b->kind == DYN_IOBUF_MMAP)
            munmap(b->data, b->cap);
        b->data = nd;
        b->cap = nc;
        b->kind = DYN_IOBUF_HEAP;
    }
    return 0;
}

int dyn_iobuf_append(dyn_iobuf_t *b, const void *src, size_t n)
{
    if (dyn_iobuf_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

uint8_t *dyn_iobuf_tail(dyn_iobuf_t *b, size_t n)
{
    if (dyn_iobuf_reserve(b, n) < 0)
        return NULL;
    return b->data + b->len;
}

void dyn_iobuf_commit(dyn_iobuf_t *b, size_t n)
{
    b->len += n;
}

void dyn_iobuf_consume(dyn_iobuf_t *b, size_t n)
{
    b->rpos += n;
    if (b->rpos > b->len)
        b->rpos = b->len;
}

void dyn_iobuf_compact(dyn_iobuf_t *b)
{
    if (b->rpos == 0)
        return;
    if (b->kind == DYN_IOBUF_MMAP)
        return; /* a read-only view is never front-refilled */
    memmove(b->data, b->data + b->rpos, b->len - b->rpos);
    b->len -= b->rpos;
    b->rpos = 0;
}

int dyn_iobuf_ensure_nul(dyn_iobuf_t *b)
{
    if (b->kind == DYN_IOBUF_MMAP)
        return 0; /* slurp guaranteed a zero-filled page tail at data[len] */
    if (b->len + 1 > b->cap && dyn_iobuf_reserve(b, 1) < 0)
        return -1;
    b->data[b->len] = 0;
    return 0;
}

void dyn_iobuf_reset(dyn_iobuf_t *b)
{
    if (b->kind == DYN_IOBUF_MMAP) {
        dyn_iobuf_free(b); /* no reusable growable allocation to keep */
        return;
    }
    b->len = 0;
    b->rpos = 0;
}

void dyn_iobuf_free(dyn_iobuf_t *b)
{
    if (b->kind == DYN_IOBUF_HEAP)
        free(b->data);
    else if (b->kind == DYN_IOBUF_MMAP)
        munmap(b->data, b->cap);
    dyn_iobuf_init(b);
}

/* ==================================================================== *
 *  Lower-level disk primitives                                          *
 * ==================================================================== */

void dyn_io_advise_seq_read(int fd, off_t size)
{
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    (void)size;
#elif defined(__APPLE__)
    fcntl(fd, F_RDAHEAD, 1);
    if (size > 0) {
        struct radvisory ra;
        ra.ra_offset = 0;
        ra.ra_count = size > INT_MAX ? INT_MAX : (int)size;
        fcntl(fd, F_RDADVISE, &ra); /* async prefetch */
    }
#else
    (void)fd;
    (void)size;
#endif
}

int dyn_io_preallocate(int fd, off_t size)
{
    if (size <= 0)
        return 0;
#if defined(__linux__)
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
    fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, size);
    return 0;
#elif defined(__APPLE__)
    {
        fstore_t fst;
        fst.fst_flags = F_ALLOCATECONTIG;
        fst.fst_posmode = F_PEOFPOSMODE;
        fst.fst_offset = 0;
        fst.fst_length = size;
        fst.fst_bytesalloc = 0;
        if (fcntl(fd, F_PREALLOCATE, &fst) < 0) {
            fst.fst_flags = F_ALLOCATEALL; /* allow non-contiguous */
            fcntl(fd, F_PREALLOCATE, &fst);
        }
    }
    return 0;
#else
    (void)fd;
    return 0;
#endif
}

int dyn_io_durable_sync(int fd)
{
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
    return fsync(fd);
#elif defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

/* Core primitive: plain advise-hinted read (no dependency on the native
 * dyna:uring module -- this TU is in the core lib that dynajsc links). The
 * io_uring bulk-read optimization lives in dyna-uring.c for module callers. */
int dyn_io_read_whole(const char *path, char **out, size_t *outlen)
{
    struct stat st;
    char *buf;
    size_t off = 0, size;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    dyn_io_advise_seq_read(fd, st.st_size);
    size = (size_t)st.st_size;
    buf = (char *)malloc(size ? size : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (off < size) {
        ssize_t r = read(fd, buf + off, size - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    *out = buf;
    *outlen = off;
    return 0;
}

/* ==================================================================== *
 *  Whole-file I/O into a dyn_iobuf_t                                    *
 * ==================================================================== */

/* Read exactly the [0,size) span of `fd` into a fresh heap-backed `out`,
 * appending a NUL when requested. Assumes `out` is empty. Returns 0 or -1. */
static int dyn_io_slurp_heap(int fd, size_t size, dyn_iobuf_t *out, int flags)
{
    size_t off = 0;
    uint8_t *p;

    if (dyn_iobuf_reserve(out, size + ((flags & DYN_SLURP_NUL) ? 1 : 0)) < 0)
        return -1;
    p = out->data;
    while (off < size) {
        ssize_t r = read(fd, p + off, size - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break; /* file shrank under us */
        off += (size_t)r;
    }
    out->len = off;
    if ((flags & DYN_SLURP_NUL) && dyn_iobuf_ensure_nul(out) < 0)
        return -1;
    return 0;
}

/* Path-opening wrapper. A caller that must REFUSE symlink components opens the
 * fd itself, strictly, and calls dyn_io_slurp_fd -- resolving by name here
 * would follow every symlink in the path. */
int dyn_io_slurp(const char *path, dyn_iobuf_t *out, int flags)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dyn_iobuf_init(out);
        return -1;
    }
    return dyn_io_slurp_fd(fd, out, flags);
}

/* Takes ownership of `fd` and ALWAYS closes it, on success and on failure. */
int dyn_io_slurp_fd(int fd, dyn_iobuf_t *out, int flags)
{
    struct stat st;
    size_t size;
    int saved;

    dyn_iobuf_init(out);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    size = (size_t)st.st_size;
    dyn_io_advise_seq_read(fd, st.st_size);

    if (size == 0) { /* empty file: inline buffer, NUL already available */
        if (flags & DYN_SLURP_NUL)
            out->data[0] = 0;
        close(fd);
        return 0;
    }

    if (!(flags & DYN_SLURP_NOMMAP) && size >= DYN_IO_MMAP_MIN) {
        long pg = sysconf(_SC_PAGESIZE);
        /* With DYN_SLURP_NUL we may only mmap when data[len] lands in the
         * zero-filled partial page past EOF (guaranteed readable '\0'); a file
         * whose size is an exact page multiple has no such tail. */
        int nul_ok = !(flags & DYN_SLURP_NUL) ||
                     (pg > 0 && (size % (size_t)pg) != 0);
        if (nul_ok) {
            void *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) {
#if defined(MADV_SEQUENTIAL)
                madvise(m, size, MADV_SEQUENTIAL);
#endif
                out->data = (uint8_t *)m;
                out->len = size;
                out->cap = size;
                out->rpos = 0;
                out->kind = DYN_IOBUF_MMAP;
                close(fd); /* mapping keeps its own reference */
                return 0;
            }
        }
    }

    if (dyn_io_slurp_heap(fd, size, out, flags) < 0) {
        saved = errno ? errno : EIO;
        close(fd);
        dyn_iobuf_free(out);
        errno = saved;
        return -1;
    }
    close(fd);
    return 0;
}

int dyn_io_read_buf(const char *path, dyn_iobuf_t *out, int flags)
{
    struct stat st;
    int fd, saved, rc;

    dyn_iobuf_init(out);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    dyn_io_advise_seq_read(fd, st.st_size);
    rc = dyn_io_slurp_heap(fd, (size_t)st.st_size, out, flags);
    saved = errno;
    close(fd);
    if (rc < 0) {
        dyn_iobuf_free(out);
        errno = saved ? saved : EIO;
        return -1;
    }
    return 0;
}

#ifndef O_DIRECTORY
#define O_DIRECTORY 0   /* hidden by glibc under strict ISO; O_RDONLY suffices */
#endif

/* fsync the directory holding `path`: the data is durable but the NAME is not
   until the directory entry is, so a crash between the two loses the rename
   and leaves the old contents. F_FULLFSYNC is not for dirfds, and a filesystem
   that does not sync directories is not a failure. */
static int dyn_io_sync_dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    char dir[PATH_MAX];
    int fd, rc;

    if (!slash) {
        dir[0] = '.'; dir[1] = '\0';
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0) n = 1;                      /* "/leaf" -> "/" */
        if (n >= sizeof(dir)) { errno = ENAMETOOLONG; return -1; }
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    rc = fsync(fd);
    if (rc < 0 && (errno == ENOTSUP || errno == EINVAL || errno == ENOTTY))
        rc = 0;
    close(fd);
    return rc;
}

int dyn_io_write_whole_atomic(const char *path, const void *data, size_t len,
                              int durable)
{
    char *tmp;
    size_t plen = strlen(path);
    size_t tcap = plen + 64; /* ".dynajs.tmp.<pid>[.<seq>]" */
    int fd, werr = 0;

    tmp = (char *)malloc(tcap);
    if (!tmp)
        return -1;
    /* Sibling temp in the same directory (same filesystem => atomic rename),
     * pid-suffixed so concurrent writers to one path do not share a temp.
     *
     * O_EXCL|O_NOFOLLOW, not O_TRUNC: the name is fully predictable, so without
     * O_EXCL a symlink planted next to `path` is followed and written through.
     * A stale temp (crashed writer) is stepped over by the counter rather than
     * clobbered, so this cannot fail permanently on leftover state. */
    {
        static unsigned long seq;
        int attempt;
        fd = -1;
        for (attempt = 0; attempt < 64 && fd < 0; attempt++) {
            if (attempt == 0)
                snprintf(tmp, tcap, "%s.dynajs.tmp.%ld", path, (long)getpid());
            else
                snprintf(tmp, tcap, "%s.dynajs.tmp.%ld.%lu", path, (long)getpid(), ++seq);
            fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 && errno != EEXIST)
                break;
        }
    }
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    dyn_io_preallocate(fd, (off_t)len);
    {
        const uint8_t *src = (const uint8_t *)data;
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, src + off, len - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                werr = 1;
                break;
            }
            off += (size_t)w;
        }
    }
    if (!werr && durable && dyn_io_durable_sync(fd) < 0)
        werr = 1;
    if (close(fd) < 0)
        werr = 1;
    if (werr || rename(tmp, path) < 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    /* -1 here means the durability asked for was not proven; it does not say
       whether the rename landed. */
    if (durable && dyn_io_sync_dir_of(path) < 0) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* ==================================================================== *
 *  Network primitives                                                   *
 * ==================================================================== */

int dyn_net_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void dyn_net_set_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int dyn_net_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(fd, p + off, len - off, 0);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}
