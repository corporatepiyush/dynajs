#include "libc-internal.h"
#include "core/dyn-stdio.h"
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/uio.h>

typedef struct {
    uint8_t* buf;
    uint32_t len;
    int err;
    int unreported;
} DynOut;

static uint8_t dyn_out_mem[2][DYN_STDIO_CAP];
static DynOut dyn_out[3] = { { NULL, 0, 0, 0 }, { dyn_out_mem[0], 0, 0, 0 }, { dyn_out_mem[1], 0, 0, 0 } };
static pthread_mutex_t dyn_out_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int dyn_out_pending;

#if defined(__GLIBC__) || defined(__linux__)
#include <stdio_ext.h>
#endif

static inline int out_cstdio_pending(FILE* f)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
    return (f->_flags & __SWR) && f->_p != f->_bf._base;
#elif defined(__GLIBC__) || defined(__linux__)
    return __fpending(f) != 0;
#else
    (void)f;
    return 1;
#endif
}

static void out_wait(int fd)
{
    struct pollfd q;
    q.fd = fd;
    q.events = POLLOUT;
    q.revents = 0;
    (void)poll(&q, 1, -1);
}

static int out_drain(int fd, const uint8_t* p, size_t n)
{
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            out_wait(fd);
        } else {
            return r < 0 ? errno : EIO;
        }
    }
    return 0;
}

static void out_fail(int fd, int e, int reported)
{
    DynOut* o = &dyn_out[fd];
    if (!o->err) {
        o->err = e;
        o->unreported = !reported;
    }
    o->len = 0;
    atomic_fetch_and_explicit(&dyn_out_pending, ~(1 << fd), memory_order_relaxed);
}

static int out_flush_locked(int fd)
{
    DynOut* o = &dyn_out[fd];
    int e;
    if (!o->len)
        return 0;
    e = out_drain(fd, o->buf, o->len);
    o->len = 0;
    atomic_fetch_and_explicit(&dyn_out_pending, ~(1 << fd), memory_order_relaxed);
    if (e)
        out_fail(fd, e, 0);
    return e;
}

__attribute__((destructor)) static void out_at_exit(void)
{
    dyn_stdio_sync();
    (void)dyn_stdio_report_unreported();
}

void dyn_stdio_sync(void)
{
    if (likely(!atomic_load_explicit(&dyn_out_pending, memory_order_relaxed)))
        return;
    pthread_mutex_lock(&dyn_out_mu);
    out_flush_locked(1);
    out_flush_locked(2);
    pthread_mutex_unlock(&dyn_out_mu);
}

void dyn_stdio_idle(void)
{
    dyn_stdio_sync();
    fflush(stdout);
    fflush(stderr);
}

void dyn_stdio_sync_try(void)
{
    if (!atomic_load_explicit(&dyn_out_pending, memory_order_relaxed))
        return;
    if (pthread_mutex_trylock(&dyn_out_mu) != 0)
        return;
    out_flush_locked(1);
    out_flush_locked(2);
    pthread_mutex_unlock(&dyn_out_mu);
}

static int out_direct(int fd, DynOut* o, const uint8_t* b, size_t n, size_t* done)
{
    size_t head = 0, off = 0;
    struct iovec iov[2];
    ssize_t r;
    int k;
    while (off < n) {
        k = 0;
        if (head < o->len) {
            iov[k].iov_base = o->buf + head;
            iov[k].iov_len = o->len - head;
            k++;
        }
        iov[k].iov_base = (void*)(uintptr_t)(b + off);
        iov[k].iov_len = n - off;
        k++;
        r = writev(fd, iov, k);
        if (r > 0) {
            size_t w = (size_t)r, h = o->len - head;
            if (w <= h) {
                head += w;
            } else {
                head = o->len;
                off += w - h;
            }
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (head < o->len) {
                out_wait(fd);
                continue;
            }
            break;
        }
        o->len = 0;
        atomic_fetch_and_explicit(&dyn_out_pending, ~(1 << fd), memory_order_relaxed);
        *done = off;
        return r < 0 ? errno : EIO;
    }
    o->len = 0;
    atomic_fetch_and_explicit(&dyn_out_pending, ~(1 << fd), memory_order_relaxed);
    *done = off;
    return off == n ? 0 : -1;
}

int dyn_stdio_write(int fd, const uint8_t* b, size_t n, size_t* done)
{
    DynOut* o = &dyn_out[fd];
    int e;
    *done = 0;
    pthread_mutex_lock(&dyn_out_mu);
    if (unlikely(o->err)) {
        e = o->err;
        o->unreported = 0;
        pthread_mutex_unlock(&dyn_out_mu);
        return e;
    }
    if (unlikely(dyn_out[3 - fd].len))
        out_flush_locked(3 - fd);
    if (out_cstdio_pending(stdout))
        fflush(stdout);
    if (fd == 2 && out_cstdio_pending(stderr))
        fflush(stderr);
    if (n >= DYN_STDIO_DIRECT) {
        e = out_direct(fd, o, b, n, done);
        if (e > 0)
            out_fail(fd, e, 1);
        pthread_mutex_unlock(&dyn_out_mu);
        return e;
    }
    if (o->len + n > DYN_STDIO_CAP) {
        e = out_flush_locked(fd);
        if (e) {
            o->unreported = 0;
            pthread_mutex_unlock(&dyn_out_mu);
            return e;
        }
    }
    memcpy(o->buf + o->len, b, n);
    o->len += (uint32_t)n;
    *done = n;
    atomic_fetch_or_explicit(&dyn_out_pending, 1 << fd, memory_order_relaxed);
    pthread_mutex_unlock(&dyn_out_mu);
    return 0;
}

int dyn_stdio_flush(int fd)
{
    int e;
    pthread_mutex_lock(&dyn_out_mu);
    out_flush_locked(3 - fd);
    out_flush_locked(fd);
    e = dyn_out[fd].err;
    if (e)
        dyn_out[fd].unreported = 0;
    pthread_mutex_unlock(&dyn_out_mu);
    return e;
}

static int out_unreported(int* fd)
{
    int i, e = 0;
    pthread_mutex_lock(&dyn_out_mu);
    for (i = 1; i <= 2 && !e; i++) {
        if (dyn_out[i].err && dyn_out[i].unreported) {
            e = dyn_out[i].err;
            dyn_out[i].unreported = 0;
            *fd = i;
        }
    }
    pthread_mutex_unlock(&dyn_out_mu);
    return e;
}

int dyn_stdio_report_unreported(void)
{
    int fd = 0, e, k = 0;
    while ((e = out_unreported(&fd)) != 0) {
        fprintf(stderr, "TypeError: %s: write failed: %s\n", fd == 2 ? "stderr" : "stdout", strerror(e));
        k++;
    }
    return k;
}
