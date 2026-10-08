#include "dyna-io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define DYN_IO_MMAP_MIN (64u * 1024u)

void dyn_iobuf_init(dyn_iobuf_t* b)
{
    b->data = b->inln;
    b->len = 0;
    b->rpos = 0;
    b->cap = DYN_IOBUF_INLINE_CAP;
    b->kind = DYN_IOBUF_INLINE;
}

int dyn_iobuf_reserve(dyn_iobuf_t* b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;

    if (need < b->len)
        return -1;
    if (need <= b->cap)
        return 0;

    nc = b->cap ? b->cap : DYN_IOBUF_INLINE_CAP;
    if (nc < 256)
        nc = 256;
    while (nc < need) {
        size_t d = nc << 1;
        if (d < nc) {
            nc = need;
            break;
        }
        nc = d;
    }

    if (b->kind == DYN_IOBUF_HEAP) {
        uint8_t* nd = (uint8_t*)realloc(b->data, nc);
        if (!nd)
            return -1;
        b->data = nd;
        b->cap = nc;
        return 0;
    }
    {
        uint8_t* nd = (uint8_t*)malloc(nc);
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

int dyn_iobuf_append(dyn_iobuf_t* b, const void* src, size_t n)
{
    if (dyn_iobuf_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

uint8_t* dyn_iobuf_tail(dyn_iobuf_t* b, size_t n)
{
    if (dyn_iobuf_reserve(b, n) < 0)
        return NULL;
    return b->data + b->len;
}

int dyn_iobuf_commit(dyn_iobuf_t* b, size_t n)
{
    if (b->len > b->cap || n > b->cap - b->len)
        return -1;
    b->len += n;
    return 0;
}

void dyn_iobuf_consume(dyn_iobuf_t* b, size_t n)
{
    b->rpos += n;
    if (b->rpos > b->len)
        b->rpos = b->len;
}

void dyn_iobuf_compact(dyn_iobuf_t* b)
{
    if (b->rpos == 0)
        return;
    if (b->kind == DYN_IOBUF_MMAP)
        return;
    memmove(b->data, b->data + b->rpos, b->len - b->rpos);
    b->len -= b->rpos;
    b->rpos = 0;
}

int dyn_iobuf_ensure_nul(dyn_iobuf_t* b)
{
    if (b->kind == DYN_IOBUF_MMAP) {
        long pg = sysconf(_SC_PAGESIZE);
        if (pg > 0 && b->len % (size_t)pg != 0)
            return 0;
        errno = ENOTSUP;
        return -1;
    }
    if (b->len + 1 > b->cap && dyn_iobuf_reserve(b, 1) < 0)
        return -1;
    b->data[b->len] = 0;
    return 0;
}

void dyn_iobuf_reset(dyn_iobuf_t* b)
{
    if (b->kind == DYN_IOBUF_MMAP) {
        dyn_iobuf_free(b);
        return;
    }
    b->len = 0;
    b->rpos = 0;
}

void dyn_iobuf_free(dyn_iobuf_t* b)
{
    if (b->kind == DYN_IOBUF_HEAP)
        free(b->data);
    else if (b->kind == DYN_IOBUF_MMAP)
        munmap(b->data, b->cap);
    dyn_iobuf_init(b);
}

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
        fcntl(fd, F_RDADVISE, &ra);
    }
#else
    (void)fd;
    (void)size;
#endif
}

size_t dyn_esc_ctrl(char* dst, size_t cap, const char* src)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0, i;

    for (i = 0; src[i]; i++) {
        unsigned char ch = (unsigned char)src[i];
        size_t w = (ch < 0x20 || ch == 0x7f) ? 4 : 1;
        if (n + w < cap) {
            if (w == 4) {
                dst[n++] = '\\';
                dst[n++] = 'x';
                dst[n++] = hex[ch >> 4];
                dst[n++] = hex[ch & 15];
            } else {
                dst[n++] = (char)ch;
            }
        } else {
            n += w;
        }
    }
    if (cap)
        dst[n < cap ? n : cap - 1] = 0;
    return n;
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
            fst.fst_flags = F_ALLOCATEALL;
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

int dyn_io_read_whole(const char* path, char** out, size_t* outlen)
{
    struct stat st;
    char* buf;
    size_t off = 0, size;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    dyn_io_advise_seq_read(fd, st.st_size);
    if (st.st_size <= 0) {
        size = 0;
    } else {
        size = (size_t)st.st_size;
        if (size > DYN_MAX_INPUT) {
            close(fd);
            errno = EFBIG;
            return -1;
        }
    }
    buf = (char*)malloc(size ? size : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (1) {
        ssize_t r;
        if (off == size) {
            if (size == DYN_MAX_INPUT) {
                char probe;
                ssize_t pr = read(fd, &probe, 1);
                if (pr < 0) {
                    if (errno == EINTR)
                        continue;
                    free(buf);
                    close(fd);
                    return -1;
                }
                if (pr == 0)
                    break;
                free(buf);
                close(fd);
                errno = EFBIG;
                return -1;
            }
            size_t want = size ? size : 1;
            size_t ns = size + want;
            if (ns < size || ns > DYN_MAX_INPUT)
                ns = DYN_MAX_INPUT;
            if (ns > DYN_MAX_INPUT) {
                free(buf);
                close(fd);
                errno = EFBIG;
                return -1;
            }
            char* nb = (char*)realloc(buf, ns);
            if (!nb) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = nb;
            size = ns;
        }
        r = read(fd, buf + off, size - off);
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

static int dyn_io_slurp_heap(int fd, size_t size, size_t maxsize,
    dyn_iobuf_t* out, int flags)
{
    if (dyn_iobuf_reserve(out, size + ((flags & DYN_SLURP_NUL) ? 1 : 0)) < 0)
        return -1;
    while (out->len < size) {
        ssize_t r = read(fd, out->data + out->len, size - out->len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        out->len += (size_t)r;
    }
    if (out->len == size) {
        uint8_t tmp[4096];
        while (1) {
            size_t want = maxsize - out->len;
            ssize_t r;
            if (want == 0) {
                uint8_t probe;
                ssize_t pr = read(fd, &probe, 1);
                if (pr < 0) {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                if (pr == 0)
                    break;
                errno = EFBIG;
                return -1;
            }
            if (want > sizeof(tmp))
                want = sizeof(tmp);
            r = read(fd, tmp, want);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (r == 0)
                break;
            if (dyn_iobuf_append(out, tmp, (size_t)r) < 0)
                return -1;
        }
    }
    if ((flags & DYN_SLURP_NUL) && dyn_iobuf_ensure_nul(out) < 0)
        return -1;
    return 0;
}

static int dyn_io_slurp_stream(int fd, size_t maxsize, dyn_iobuf_t* out, int flags)
{
    size_t want = maxsize < DYN_IO_MMAP_MIN ? maxsize : DYN_IO_MMAP_MIN;
    if (want == 0)
        want = 1;
    while (1) {
        ssize_t r;
        uint8_t* p;
        if (dyn_iobuf_reserve(out, want) < 0)
            return -1;
        p = out->data + out->len;
        size_t room = out->cap - out->len;
        if (room > maxsize - out->len)
            room = maxsize - out->len;
        r = read(fd, p, room);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        out->len += (size_t)r;
        if (out->len == maxsize) {
            uint8_t probe;
            ssize_t pr = read(fd, &probe, 1);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (pr == 0)
                break;
            errno = EFBIG;
            return -1;
        }
        if (out->len + want > maxsize)
            want = maxsize - out->len;
    }
    if ((flags & DYN_SLURP_NUL) && dyn_iobuf_ensure_nul(out) < 0)
        return -1;
    return 0;
}

int dyn_io_slurp(const char* path, dyn_iobuf_t* out, int flags, size_t maxsize)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dyn_iobuf_init(out);
        return -1;
    }
    return dyn_io_slurp_fd(fd, out, flags, maxsize);
}

int dyn_io_slurp_fd(int fd, dyn_iobuf_t* out, int flags, size_t maxsize)
{
    struct stat st;
    size_t size;
    int saved;

    dyn_iobuf_init(out);
    {
        int fstat_rc = fstat(fd, &st);
        if (fstat_rc < 0 || !S_ISREG(st.st_mode)) {
            if (fstat_rc == 0 && S_ISDIR(st.st_mode))
                saved = EISDIR;
            else
                saved = errno ? errno : EINVAL;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    dyn_io_advise_seq_read(fd, st.st_size);

    if (st.st_size <= 0) {
        if (dyn_io_slurp_stream(fd, maxsize, out, flags) < 0) {
            saved = errno ? errno : EIO;
            close(fd);
            dyn_iobuf_free(out);
            errno = saved;
            return -1;
        }
        close(fd);
        return 0;
    }

    size = (size_t)st.st_size;
    if (size > maxsize) {
        saved = EFBIG;
        close(fd);
        errno = saved;
        return -1;
    }

    if ((flags & DYN_SLURP_MMAP) && size >= DYN_IO_MMAP_MIN) {
        long pg = sysconf(_SC_PAGESIZE);
        int nul_ok = !(flags & DYN_SLURP_NUL) || (pg > 0 && (size % (size_t)pg) != 0);
        if (nul_ok) {
            void* m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) {
#if defined(MADV_SEQUENTIAL)
                madvise(m, size, MADV_SEQUENTIAL);
#endif
                out->data = (uint8_t*)m;
                out->len = size;
                out->cap = size;
                out->rpos = 0;
                out->kind = DYN_IOBUF_MMAP;
                close(fd);
                return 0;
            }
        }
    }

    if (dyn_io_slurp_heap(fd, size, maxsize, out, flags) < 0) {
        saved = errno ? errno : EIO;
        close(fd);
        dyn_iobuf_free(out);
        errno = saved;
        return -1;
    }
    close(fd);
    return 0;
}

int dyn_io_read_buf(const char* path, dyn_iobuf_t* out, int flags, size_t maxsize)
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
    if (st.st_size <= 0)
        rc = dyn_io_slurp_stream(fd, maxsize, out, flags);
    else if ((size_t)st.st_size > maxsize) {
        saved = EFBIG;
        close(fd);
        errno = saved;
        return -1;
    } else {
        rc = dyn_io_slurp_heap(fd, (size_t)st.st_size, maxsize, out, flags);
    }
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
#define O_DIRECTORY 0
#endif

static int dyn_io_sync_dir_of(const char* path)
{
    const char* slash = strrchr(path, '/');
    char dir[PATH_MAX];
    int fd, rc;

    if (!slash) {
        dir[0] = '.';
        dir[1] = '\0';
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0)
            n = 1;
        if (n >= sizeof(dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
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

int dyn_io_write_whole_atomic(const char* path, const void* data, size_t len,
    int durable)
{
    char* tmp;
    size_t plen = strlen(path);
    size_t tcap = plen + 64;
    int fd, werr = 0;
    struct stat dst_st;
    mode_t dst_mode = 0;
    int have_mode = 0;

    if (stat(path, &dst_st) == 0 && S_ISREG(dst_st.st_mode)) {
        dst_mode = dst_st.st_mode & 07777;
        have_mode = 1;
    }
    tmp = (char*)malloc(tcap);
    if (!tmp)
        return -1;
    {
        static _Atomic unsigned long seq;
        int attempt;
        fd = -1;
        for (attempt = 0; attempt < 64 && fd < 0; attempt++) {
            if (attempt == 0)
                snprintf(tmp, tcap, "%s.dynajs.tmp.%ld", path, (long)getpid());
            else
                snprintf(tmp, tcap, "%s.dynajs.tmp.%ld.%lu", path,
                    (long)getpid(),
                    atomic_fetch_add_explicit(&seq, 1ul,
                        memory_order_relaxed));
            fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                have_mode ? 0600 : 0666);
            if (fd < 0 && errno != EEXIST)
                break;
        }
    }
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    if (have_mode && fchmod(fd, dst_mode) < 0)
        werr = 1;
    dyn_io_preallocate(fd, (off_t)len);
    {
        const uint8_t* src = (const uint8_t*)data;
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
    if (durable && dyn_io_sync_dir_of(path) < 0) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

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

#ifndef DYN_MSG_NOSIGNAL
#if defined(MSG_NOSIGNAL)
#define DYN_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define DYN_MSG_NOSIGNAL 0
#endif
#endif

int dyn_net_send_all(int fd, const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(fd, p + off, len - off, DYN_MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
#undef strndup

#include <stdatomic.h>
#if defined(CONFIG_MIMALLOC)
#include <mimalloc.h>
#define DYN_TRK_USABLE(p) mi_usable_size(p)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define DYN_TRK_USABLE(p) malloc_size(p)
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#define DYN_TRK_USABLE(p) malloc_usable_size(p)
#elif defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <malloc.h>
#define DYN_TRK_USABLE(p) malloc_usable_size(p)
#else
#define DYN_TRK_USABLE(p) ((size_t)0)
#endif

static _Atomic uint64_t dyn_trk_live_bytes;
static _Atomic uint64_t dyn_trk_byte_limit;
_Atomic int dyn_trk_armed;
static uint64_t (*_Atomic dyn_trk_peer_bytes)(void);

uint64_t dyn_trk_bytes(void)
{
    return atomic_load_explicit(&dyn_trk_live_bytes, memory_order_relaxed);
}

void dyn_trk_set_limit(uint64_t limit)
{
    atomic_store_explicit(&dyn_trk_byte_limit, limit, memory_order_relaxed);
    if (limit)
        atomic_store_explicit(&dyn_trk_armed, 1, memory_order_relaxed);
}

void dyn_trk_set_peer(uint64_t (*exact_bytes)(void))
{
    atomic_store_explicit(&dyn_trk_peer_bytes, exact_bytes,
        memory_order_relaxed);
}

static int dyn_trk_admit(size_t size)
{
    uint64_t limit = atomic_load_explicit(&dyn_trk_byte_limit,
        memory_order_relaxed);
    uint64_t used, peer;
    uint64_t (*peer_fn)(void);
    if (!limit)
        return 0;
    used = atomic_load_explicit(&dyn_trk_live_bytes, memory_order_relaxed);
    peer_fn = atomic_load_explicit(&dyn_trk_peer_bytes, memory_order_relaxed);
    peer = peer_fn ? peer_fn() : 0;
    if (peer > UINT64_MAX - used || used + peer >= limit
        || (uint64_t)size > limit - (used + peer)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static void dyn_trk_add(void* p)
{
    atomic_fetch_add_explicit(&dyn_trk_live_bytes,
        (uint64_t)DYN_TRK_USABLE(p), memory_order_relaxed);
}

static void dyn_trk_sub(size_t size)
{
    uint64_t live = atomic_load_explicit(&dyn_trk_live_bytes,
        memory_order_relaxed);
    for (;;) {
        uint64_t next = live < (uint64_t)size ? 0 : live - (uint64_t)size;
        if (atomic_compare_exchange_weak_explicit(&dyn_trk_live_bytes,
                &live, next, memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

void* dyn_trk_malloc_plumbing(size_t size)
{
    void* p = malloc(size);
    if (p)
        dyn_trk_add(p);
    return p;
}

void* dyn_trk_calloc_plumbing(size_t nmemb, size_t size)
{
    void* p = calloc(nmemb, size);
    if (p)
        dyn_trk_add(p);
    return p;
}

void* dyn_trk_realloc_plumbing(void* ptr, size_t size)
{
    void* p;
    size_t old;
    if (!ptr)
        return dyn_trk_malloc_plumbing(size);
    old = DYN_TRK_USABLE(ptr);
    p = realloc(ptr, size);
    if (!p)
        return NULL;
    dyn_trk_sub(old);
    dyn_trk_add(p);
    return p;
}

void* dyn_trk_malloc(size_t size)
{
    void* p;
    if (dyn_trk_admit(size))
        return NULL;
    p = malloc(size);
    if (p)
        dyn_trk_add(p);
    return p;
}

void* dyn_trk_calloc(size_t nmemb, size_t size)
{
    void* p;
    if (size && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    if (dyn_trk_admit(nmemb * size))
        return NULL;
    p = calloc(nmemb, size);
    if (p)
        dyn_trk_add(p);
    return p;
}

void* dyn_trk_realloc(void* ptr, size_t size)
{
    void* p;
    size_t old;
    if (!ptr)
        return dyn_trk_malloc(size);
    old = DYN_TRK_USABLE(ptr);
    if (size > old && dyn_trk_admit(size - old))
        return NULL;
    p = realloc(ptr, size);
    if (!p)
        return NULL;
    dyn_trk_sub(old);
    dyn_trk_add(p);
    return p;
}

void dyn_trk_free(void* ptr)
{
    if (ptr)
        dyn_trk_sub(DYN_TRK_USABLE(ptr));
    free(ptr);
}

char* dyn_trk_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)dyn_trk_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

char* dyn_trk_strndup(const char* s, size_t n)
{
    size_t len = strnlen(s, n);
    char* p = (char*)dyn_trk_malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}
