#include "dyna-io.h"
#include "core/dyn-common.h"

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
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#endif

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
    size_t need = size + ((flags & DYN_SLURP_NUL) ? 1 : 0);
    if (out->len == 0 && out->kind == DYN_IOBUF_INLINE && need > out->cap) {
        uint8_t* nd = (uint8_t*)malloc(need);
        if (!nd)
            return -1;
        out->data = nd;
        out->cap = need;
        out->kind = DYN_IOBUF_HEAP;
    } else if (dyn_iobuf_reserve(out, need) < 0)
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
    return dyn_fd_set_nonblock(fd);
}

int dyn_net_accept(int lfd, int nonblock)
{
    int fd;
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0));
    if (fd >= 0 || (errno != ENOSYS && errno != EINVAL))
        return fd;
#endif
    fd = accept(lfd, NULL, NULL);
    if (fd < 0)
        return -1;
    if (dyn_fd_set_cloexec(fd) < 0 || (nonblock && dyn_fd_set_nonblock(fd) < 0)) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void dyn_net_set_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

void dyn_net_set_io_timeout(int fd, long long ms)
{
    struct timeval tv;
    if (ms < 0)
        ms = 0;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int dyn_net_socket_cloexec(int domain, int type, int protocol)
{
    int fd;
#ifdef SOCK_CLOEXEC
    fd = socket(domain, type | SOCK_CLOEXEC, protocol);
    if (fd < 0 && (errno == EINVAL || errno == EPROTOTYPE))
        fd = socket(domain, type, protocol);
#else
    fd = socket(domain, type, protocol);
#endif
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_net_udp_addr(const char* host, uint16_t port, int family, struct sockaddr_storage* ss,
    socklen_t* sl)
{
    memset(ss, 0, sizeof(*ss));
    if (strchr(host, ':')) {
        struct addrinfo hints, *res = NULL;
        char ps[8];
        if (family != AF_INET6)
            return -2;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
        snprintf(ps, sizeof ps, "%u", (unsigned)port);
        if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
            return -1;
        memcpy(ss, res->ai_addr, res->ai_addrlen);
        *sl = res->ai_addrlen;
        freeaddrinfo(res);
        return 0;
    }
    {
        struct in_addr a4;
        if (inet_pton(AF_INET, host, &a4) != 1)
            return -1;
        if (family == AF_INET6) {
            struct sockaddr_in6* s6 = (struct sockaddr_in6*)ss;
            s6->sin6_family = AF_INET6;
            s6->sin6_port = htons(port);
            s6->sin6_addr.s6_addr[10] = 0xff;
            s6->sin6_addr.s6_addr[11] = 0xff;
            memcpy(&s6->sin6_addr.s6_addr[12], &a4, 4);
            *sl = sizeof(*s6);
        } else {
            struct sockaddr_in* s4 = (struct sockaddr_in*)ss;
            s4->sin_family = AF_INET;
            s4->sin_port = htons(port);
            s4->sin_addr = a4;
            *sl = sizeof(*s4);
        }
    }
    return 0;
}

int dyn_net_udp_socket(const char* host, uint16_t port)
{
    struct sockaddr_storage ss;
    socklen_t sl = 0;
    int fd, on = 1, off = 0, family = AF_INET;

    memset(&ss, 0, sizeof(ss));
    if (host && strchr(host, ':')) {
        family = AF_INET6;
        if (dyn_net_udp_addr(host, port, AF_INET6, &ss, &sl) < 0) {
            errno = EINVAL;
            return -1;
        }
    } else {
        struct sockaddr_in* s4 = (struct sockaddr_in*)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        s4->sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
        sl = sizeof(*s4);
    }
    fd = dyn_net_socket_cloexec(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (port)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    dyn_net_set_nonblock(fd);
    if (bind(fd, (struct sockaddr*)&ss, sl) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_net_connect_blocking(const char* host, unsigned short port, long long timeout_ms,
    int (*reject)(int fd, void* udata), void* udata, int* why, int* gai_rc)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, rc, reason = DYN_NET_CONNECT_FAILED;
    int wait_ms = timeout_ms > INT_MAX ? INT_MAX
        : timeout_ms < -1              ? -1
                                       : (int)timeout_ms;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, portstr, &hints, &res);
    if (gai_rc)
        *gai_rc = rc;
    if (rc != 0 || !res) {
        if (res)
            freeaddrinfo(res);
        if (why)
            *why = DYN_NET_CONNECT_RESOLVE;
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int flags, connected = 0;
        fd = dyn_net_socket_cloexec(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
#ifdef SO_NOSIGPIPE
        {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        }
#endif
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            connected = 1;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd;
            int pr;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            pr = poll(&pfd, 1, wait_ms);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                    connected = 1;
                else
                    reason = DYN_NET_CONNECT_FAILED;
            } else {
                reason = pr == 0 ? DYN_NET_CONNECT_TIMEOUT : DYN_NET_CONNECT_FAILED;
            }
        } else {
            reason = DYN_NET_CONNECT_FAILED;
        }
        if (connected && reject && reject(fd, udata)) {
            reason = DYN_NET_CONNECT_REFUSED_BY_FILTER;
            connected = 0;
        }
        if (connected) {
            fcntl(fd, F_SETFL, flags);
            dyn_net_set_nodelay(fd);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && why)
        *why = reason;
    return fd;
}

int dyn_net_peer_ip(int fd, char* out, size_t out_len, int unmap_v4)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    const void* ap = NULL;
    int fam;

    if (getpeername(fd, (struct sockaddr*)&ss, &sl) != 0)
        return -1;
    fam = ss.ss_family;
    if (fam == AF_INET6) {
        const struct in6_addr* a6 = &((struct sockaddr_in6*)&ss)->sin6_addr;
        if (unmap_v4 && IN6_IS_ADDR_V4MAPPED(a6)) {
            fam = AF_INET;
            ap = (const unsigned char*)a6 + 12;
        } else {
            ap = a6;
        }
    } else if (fam == AF_INET) {
        ap = &((struct sockaddr_in*)&ss)->sin_addr;
    }
    if (!ap || !inet_ntop(fam, ap, out, (socklen_t)out_len))
        return -1;
    return 0;
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

void dyn_trk_release(void* ptr)
{
    if (ptr)
        dyn_trk_sub(DYN_TRK_USABLE(ptr));
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

#if defined(__linux__) && defined(SYS_close_range) && !defined(DYN_CLOSE_TEST_NO_CLOSE_RANGE)
#define DYN_CLOSE_HAVE_CLOSE_RANGE 1
#endif
#if defined(__linux__) && defined(SYS_getdents64) && !defined(DYN_CLOSE_TEST_NO_GETDENTS)
#define DYN_CLOSE_HAVE_GETDENTS 1
#endif
#if defined(__APPLE__) && defined(PROC_PIDLISTFDS) && !defined(DYN_CLOSE_TEST_NO_PIDINFO)
#define DYN_CLOSE_HAVE_PIDINFO 1
#endif
#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)) && !defined(DYN_CLOSE_TEST_NO_CLOSEFROM)
#define DYN_CLOSE_HAVE_CLOSEFROM 1
#endif

#define DYN_CLOSE_SCAN_CAP 1048576L
#define DYN_CLOSE_LIST_MAX 4096

#ifdef DYN_CLOSE_TEST_TRACE
volatile int dyn_close_path;
#define DYN_CLOSE_USED(p) (dyn_close_path = (p))
#else
#define DYN_CLOSE_USED(p) ((void)0)
#endif
#ifdef DYN_CLOSE_TEST_FAIL_CLOSE_RANGE
#define DYN_CLOSE_RANGE_LOW(low) (~0U)
#define DYN_CLOSE_RANGE_HIGH 0U
#else
#define DYN_CLOSE_RANGE_LOW(low) ((unsigned)(low))
#define DYN_CLOSE_RANGE_HIGH (~0U)
#endif

#ifdef DYN_CLOSE_HAVE_GETDENTS
static int dyn_close_by_getdents(int low)
{
    char buf[4096] __attribute__((aligned(8)));
    int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int closed;
    if (dfd < 0)
        return -1;
    do {
        long n;
        closed = 0;
        if (lseek(dfd, 0, SEEK_SET) < 0) {
            close(dfd);
            return -1;
        }
        while ((n = syscall(SYS_getdents64, dfd, buf, sizeof buf)) > 0) {
            long off = 0;
            while (off < n) {
                const char* rec = buf + off;
                unsigned short reclen;
                const char* name = rec + 19;
                long fd = 0;
                memcpy(&reclen, rec + 16, sizeof reclen);
                if (*name >= '0' && *name <= '9') {
                    while (*name >= '0' && *name <= '9')
                        fd = fd * 10 + (*name++ - '0');
                    if (fd >= low && fd != dfd && fd <= INT_MAX) {
                        close((int)fd);
                        closed++;
                    }
                }
                off += reclen;
            }
        }
        if (n < 0) {
            close(dfd);
            return -1;
        }
    } while (closed);
    close(dfd);
    return 0;
}
#endif

#ifdef DYN_CLOSE_HAVE_PIDINFO
static int dyn_close_by_pidinfo(int low)
{
    struct proc_fdinfo fds[DYN_CLOSE_LIST_MAX];
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, (int)sizeof(fds));
    int i, n;
    if (bytes <= 0 || (size_t)bytes >= sizeof(fds))
        return -1;
    n = bytes / (int)sizeof(fds[0]);
    for (i = 0; i < n; i++)
        if (fds[i].proc_fd >= low)
            close(fds[i].proc_fd);
    return 0;
}
#endif

void dyn_close_fds_from(int low)
{
    long fd, top;
    struct rlimit rl;
#ifdef DYN_CLOSE_HAVE_CLOSE_RANGE
    if (syscall(SYS_close_range, DYN_CLOSE_RANGE_LOW(low), DYN_CLOSE_RANGE_HIGH, 0U) == 0) {
        DYN_CLOSE_USED(1);
        return;
    }
#endif
#ifdef DYN_CLOSE_HAVE_CLOSEFROM
    closefrom(low);
    DYN_CLOSE_USED(2);
    return;
#endif
#ifdef DYN_CLOSE_HAVE_GETDENTS
    if (dyn_close_by_getdents(low) == 0) {
        DYN_CLOSE_USED(3);
        return;
    }
#endif
#ifdef DYN_CLOSE_HAVE_PIDINFO
    if (dyn_close_by_pidinfo(low) == 0) {
        DYN_CLOSE_USED(4);
        return;
    }
#endif
    top = sysconf(_SC_OPEN_MAX);
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && (long)rl.rlim_cur > top)
        top = (long)rl.rlim_cur;
    if (top < 3 || top > DYN_CLOSE_SCAN_CAP)
        top = DYN_CLOSE_SCAN_CAP;
    for (fd = low; fd < top; fd++)
        close((int)fd);
    DYN_CLOSE_USED(5);
}
