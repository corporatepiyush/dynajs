/*
 * dyna-aio (io_uring backend) -- the Linux completion-model implementation of
 * the dyn_aio interface (see dyna-aio.h). Selected by CONFIG_IO_URING; the
 * portable readiness backend (dyna-aio.c) is used otherwise. Same interface, so
 * the HTTP server and any dyn_aio user are backend-agnostic.
 *
 * Design: one ring per JS thread, SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN so
 * completion task-work runs in-thread at reap time. A registered eventfd is the
 * pollable fd folded into js_std_loop (dyn_aio_backend_fd); when it signals,
 * dyn_aio_drain reaps every CQE and invokes callbacks on the JS thread. Recv
 * uses a provided-buffer ring (kernel picks the buffer -> zero pre-post copy);
 * accept is multishot; send is a plain submit. user_data encodes (fd<<3)|op so a
 * completion maps back to the per-fd {cb,udata} slot.
 */
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <liburing.h>
#include "core/dyn-pool.h"

#define URING_ENTRIES 8192
/* Provided-buffer pool = NBUFS*BUFSZ. Measured (kernel 7.0, arm64): buffers
 * recycle synchronously inside the drain, so few are held at once; -ENOBUFS
 * re-arms rather than drops. 512*8K=4MB matches epoll-class throughput at a
 * fraction of the old 4096*16K=64MB pool. Override with -D for bulk workloads. */
#ifndef URING_NBUFS
#define URING_NBUFS   512
#endif
#ifndef URING_BUFSZ
#define URING_BUFSZ   8192
#endif
#define URING_BGID    1

enum { UOP_ACCEPT = 0, UOP_RECV = 1, UOP_SEND = 2, UOP_CLOSE = 3,
       UOP_TIMER = 4 };
#define UD(fd, op)   (((uint64_t)(unsigned)(fd) << 3) | (unsigned)(op))
#define UD_FD(ud)    ((int)((ud) >> 3))
#define UD_OP(ud)    ((int)((ud) & 7))

typedef struct {
    dyn_aio_cb r_cb;   /* accept or recv completion */
    void *r_udata;
    uint8_t r_op;      /* UOP_ACCEPT / UOP_RECV / 0 */
    uint8_t r_multishot;
} uaio_fd_t;

/* One outstanding send. Heap-allocated so a connection can have several in
 * flight (e.g. a static response's header then body); the pointer travels in
 * the SQE user_data with the top bit set as a discriminator (Linux user-space
 * pointers are < 2^47, so the top bit is free). */
#define SEND_BIT (1ULL << 63)
/* Disk goes to the KERNEL here, not to a thread pool: io_uring already makes a
 * regular file asynchronous, so a pool would add a hop the kernel does not
 * need. Second discriminator bit, same free-high-bits trick as SEND_BIT. */
#define DISK_BIT (1ULL << 62)

/* A CQE from the multishot poll on the offload pool's wake fd. Disk goes to the
 * kernel here, but dyn_aio_offload runs CALLER-SUPPLIED C, which no ring can
 * execute -- so this backend keeps a pool too, purely for that. Third
 * discriminator bit; carries no pointer, so the low bits are unused. */
#define POOL_BIT (1ULL << 61)
typedef struct {
    dyn_aio_cb cb;
    void *udata;
    char *path;        /* openat only; freed on completion */
} uaio_disk_t;
typedef struct {
    uint8_t *buf;      /* copied payload, freed on completion */
    size_t len, off;   /* total and bytes already sent (partial-send resubmit) */
    int fd;
    dyn_aio_cb cb;
    void *udata;
} uaio_send_t;

struct dyn_aio {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    unsigned char *buf_base;
    unsigned nbufs, bufsz;
    int evfd;          /* registered eventfd: the pollable fd for js_std_loop */
    int tfd;           /* periodic timerfd, or -1; see dyn_aio_set_timer */
    unsigned tick_ms;
    uint64_t tick_buf; /* expiration count; discarded, but must be READ */
    uaio_fd_t *fds;
    int cap;
    size_t inflight;
    dyn_pool_t *pool;        /* dyn_aio_offload only; disk goes to the kernel */
    dyn_pool_chan_t *chan;
};

static int uaio_fd_ensure(dyn_aio_t *a, int fd)
{
    int nc;
    uaio_fd_t *nf;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 256;
    while (nc <= fd) nc *= 2;
    nf = (uaio_fd_t *)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf) return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

static struct io_uring_sqe *uaio_sqe(dyn_aio_t *a)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&a->ring);
    if (!sqe) { io_uring_submit(&a->ring); sqe = io_uring_get_sqe(&a->ring); }
    return sqe;
}

static void uaio_recycle(dyn_aio_t *a, int bid)
{
    io_uring_buf_ring_add(a->br, a->buf_base + (size_t)bid * a->bufsz, a->bufsz,
                          bid, io_uring_buf_ring_mask(a->nbufs), 0);
    io_uring_buf_ring_advance(a->br, 1);
}

/* ---- lifecycle ---- */

dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t *a = (dyn_aio_t *)calloc(1, sizeof(*a));
    struct io_uring_params p;
    unsigned i;
    int ret = 0;
    (void)entries; (void)disk_workers;
    if (!a) return NULL;
    a->nbufs = URING_NBUFS;
    a->bufsz = URING_BUFSZ;

    /* NOTE: do NOT use IORING_SETUP_DEFER_TASKRUN here -- it defers completion
     * processing until the ring is actively entered, so the registered eventfd
     * never fires and the poll(2)-based js_std_loop integration would hang.
     * SINGLE_ISSUER|COOP_TASKRUN keeps the single-thread optimization while
     * letting the eventfd notify on each completion. */
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
        memset(&p, 0, sizeof(p)); p.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
            memset(&p, 0, sizeof(p));
            if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
                free(a); return NULL;
            }
        }
    }
    a->buf_base = (unsigned char *)malloc((size_t)a->nbufs * a->bufsz);
    a->br = a->buf_base ? io_uring_setup_buf_ring(&a->ring, a->nbufs, URING_BGID, 0, &ret)
                        : NULL;
    a->tfd = -1;
    a->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!a->buf_base || !a->br || a->evfd < 0 ||
        io_uring_register_eventfd(&a->ring, a->evfd) < 0) {
        if (a->br) io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
        if (a->tfd >= 0) close(a->tfd);
        if (a->evfd >= 0) close(a->evfd);
        free(a->buf_base);
        io_uring_queue_exit(&a->ring);
        free(a);
        return NULL;
    }
    for (i = 0; i < a->nbufs; i++)
        io_uring_buf_ring_add(a->br, a->buf_base + (size_t)i * a->bufsz, a->bufsz,
                              (int)i, io_uring_buf_ring_mask(a->nbufs), (int)i);
    io_uring_buf_ring_advance(a->br, a->nbufs);
    return a;
}

void dyn_aio_free(dyn_aio_t *a)
{
    if (!a) return;
    /* Channel before pool: dyn_pool_free joins every worker, and a worker
     * still running would write into a freed ring. */
    if (a->chan) dyn_pool_chan_free(a->chan);
    if (a->pool) dyn_pool_free(a->pool);
    if (a->br) io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
    if (a->tfd >= 0) close(a->tfd);
    if (a->evfd >= 0) close(a->evfd);
    free(a->buf_base);
    io_uring_queue_exit(&a->ring);
    free(a->fds);
    free(a);
}

/* Same contract as the readiness backend: ALWAYS completes, inline if the pool
 * is unavailable or full. `work` is caller C, so it needs a thread even here. */
typedef struct {
    dyn_aio_t *aio;
    void (*work)(void *);
    void (*done)(void *);
    void *arg;
} uaio_offload_t;

static void uaio_offload_work(void *p)    /* WORKER THREAD: no JS_* in here */
{
    uaio_offload_t *j = (uaio_offload_t *)p;
    if (j->work) j->work(j->arg);
}

static void uaio_offload_done(void *p)    /* LOOP THREAD: JS is safe here */
{
    uaio_offload_t *j = (uaio_offload_t *)p;
    if (j->aio->inflight) j->aio->inflight--;
    if (j->done) j->done(j->arg);
    free(j);
}

int dyn_aio_offload(dyn_aio_t *a, void (*work)(void *), void (*done)(void *),
                    void *arg)
{
    uaio_offload_t *j;
    if (!a) return -1;
    j = (uaio_offload_t *)calloc(1, sizeof(*j));
    if (!j) {
        if (work) work(arg);
        if (done) done(arg);
        return 1;
    }
    j->aio = a; j->work = work; j->done = done; j->arg = arg;
    a->inflight++;
    if (uaio_pool_ready(a) == 0 &&
        dyn_pool_submit(a->chan, uaio_offload_work, uaio_offload_done, j) == 0)
        return 0;
    uaio_offload_work(j);
    uaio_offload_done(j);
    return 1;
}

int dyn_aio_backend_fd(const dyn_aio_t *a) { return a->evfd; }
size_t dyn_aio_inflight(const dyn_aio_t *a) { return a->inflight; }

/* Re-arm a multishot recv (its slot is still armed; nothing to do but keep the
 * interest -- io_uring keeps multishot alive unless IORING_CQE_F_MORE is clear). */
static void uaio_rearm_recv(dyn_aio_t *a, int fd)
{
    struct io_uring_sqe *sqe = uaio_sqe(a);
    if (!sqe) return;
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV));
}

/* Watch the pool's wake fd from the RING, so the one fd js_std_loop polls
 * (evfd) becomes readable for an offload completion too -- the backend fd is
 * the multiplexer, and no second reactor slot is needed. */
static void uaio_pool_arm(dyn_aio_t *a)
{
    struct io_uring_sqe *sqe;
    int wfd = a->chan ? dyn_pool_wake_fd(a->chan) : -1;
    if (wfd < 0)
        return;
    sqe = uaio_sqe(a);
    if (!sqe)
        return;
    io_uring_prep_poll_multishot(sqe, wfd, POLLIN);
    io_uring_sqe_set_data64(sqe, POOL_BIT);
}

static int uaio_pool_ready(dyn_aio_t *a)
{
    if (a->chan)
        return 0;
    if (!a->pool) {
        a->pool = dyn_pool_new(0, 0);   /* max(ncpu,4), --io-threads honoured */
        if (!a->pool)
            return -1;
    }
    a->chan = dyn_pool_chan_new(a->pool);
    if (!a->chan)
        return -1;
    uaio_pool_arm(a);
    io_uring_submit(&a->ring);
    return 0;
}

static void uaio_dispatch(dyn_aio_t *a, struct io_uring_cqe *cqe)
{
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    int fd, op, res = cqe->res;
    uaio_fd_t *s;

    if (ud & POOL_BIT) { /* offload pool wake fd is readable */
        /* Multishot: the kernel keeps the interest while IORING_CQE_F_MORE is
         * set. Without the re-arm, the SECOND offload never wakes the loop. */
        if (a->chan)
            dyn_pool_drain(a->chan);
        if (!(cqe->flags & IORING_CQE_F_MORE))
            uaio_pool_arm(a);
        return;
    }
    if (ud & DISK_BIT) { /* a disk completion (heap context pointer) */
        uaio_disk_t *dc = (uaio_disk_t *)(uintptr_t)(ud & ~DISK_BIT);
        if (a->inflight) a->inflight--;
        if (dc->cb) dc->cb(a, res, NULL, 0, dc->udata);
        free(dc->path);
        free(dc);
        return;
    }
    if (ud & SEND_BIT) { /* a send completion (heap context pointer) */
        uaio_send_t *sc = (uaio_send_t *)(uintptr_t)(ud & ~SEND_BIT);
        if (res > 0) sc->off += (size_t)res;
        if ((res > 0 || res == -EAGAIN) && sc->off < sc->len) {
            /* partial send: resubmit the remainder (socket buffer was full) */
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) {
                io_uring_prep_send(sqe, sc->fd, sc->buf + sc->off,
                                   sc->len - sc->off, MSG_NOSIGNAL);
                io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)sc | SEND_BIT);
                io_uring_submit(&a->ring);
                return; /* keep sc alive until fully sent */
            }
        }
        { dyn_aio_cb cb = sc->cb; void *u = sc->udata;
          int result = res < 0 ? res : (int)sc->len;
          free(sc->buf); free(sc);
          if (a->inflight) a->inflight--;
          if (cb) cb(a, result, NULL, 0, u); }
        return;
    }
    fd = UD_FD(ud); op = UD_OP(ud);
    if (fd < 0 || fd >= a->cap) return;
    s = &a->fds[fd];

    if (op == UOP_TIMER) {
        /* Re-arm regardless of res: a spurious 0 or -EAGAIN must not stop the
         * clock, which is the failure this whole function exists to prevent. */
        if (a->tick_ms)
            (void)uaio_arm_timer(a);
        return;
    }
    if (op == UOP_ACCEPT) {
        dyn_aio_cb cb = s->r_cb; void *u = s->r_udata;
        if (res >= 0) {
            int on = 1;
            setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            if (cb) cb(a, res, NULL, 0, u);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && res != -ECANCELED) {
            /* multishot accept ended: re-arm */
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) { io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
                       io_uring_sqe_set_data64(sqe, UD(fd, UOP_ACCEPT)); }
        }
        return;
    }
    if (op == UOP_RECV) {
        dyn_aio_cb cb = s->r_cb; void *u = s->r_udata;
        int bid = (cqe->flags & IORING_CQE_F_BUFFER)
                      ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;
        if (res == -ENOBUFS) { /* pool momentarily empty: re-arm, do NOT close --
                                * buffers recycle within this same drain */
            uaio_rearm_recv(a, fd);
            return;
        }
        if (res > 0 && bid >= 0) {
            if (cb) cb(a, res, a->buf_base + (size_t)bid * a->bufsz, (unsigned)res, u);
            uaio_recycle(a, bid);
        } else {
            if (bid >= 0) uaio_recycle(a, bid);
            if (a->inflight) a->inflight--;
            s->r_op = 0;
            if (cb) cb(a, res <= 0 ? (res == 0 ? 0 : res) : 0, NULL, 0, u); /* closed/err */
            return;
        }
        if (!(cqe->flags & IORING_CQE_F_MORE)) /* multishot recv ended: re-arm */
            uaio_rearm_recv(a, fd);
        return;
    }
    /* UOP_CLOSE and any other op: nothing */
    (void)op;
}

void dyn_aio_drain(void *aio)
{
    dyn_aio_t *a = (dyn_aio_t *)aio;
    struct io_uring_cqe *cqe;
    unsigned head, n = 0;
    uint64_t v;
    ssize_t rd = read(a->evfd, &v, sizeof(v)); /* clear the eventfd counter */
    (void)rd;
    io_uring_submit(&a->ring);
    io_uring_for_each_cqe(&a->ring, head, cqe) { uaio_dispatch(a, cqe); n++; }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring); /* flush any SQEs queued by the callbacks */
}

int dyn_aio_run(dyn_aio_t *a, int timeout_ms)
{
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts, *pts = NULL;
    unsigned head, n = 0;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    io_uring_submit_and_wait_timeout(&a->ring, &cqe, 1, pts, NULL);
    io_uring_for_each_cqe(&a->ring, head, cqe) { uaio_dispatch(a, cqe); n++; }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring);
    return (int)n;
}

/* ---- network ---- */

int dyn_aio_listen(dyn_aio_t *a, const char *host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 1024) < 0) { close(fd); return -1; }
    return fd;
}

int dyn_aio_accept(dyn_aio_t *a, int listen_fd, dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;
    if (uaio_fd_ensure(a, listen_fd) < 0) return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_ACCEPT; s->r_multishot = 1;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD(listen_fd, UOP_ACCEPT));
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_recv(dyn_aio_t *a, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;
    (void)pool; (void)multishot;
    /* Same rule as the readiness backend: the callback is required, because the
     * completion path calls it unguarded and a NULL indirect call can spin
     * rather than fault. */
    if (!a || !cb) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, fd) < 0) return -1;
    s = &a->fds[fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_RECV; s->r_multishot = 1;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV));
    return 0;
}

int dyn_aio_send(dyn_aio_t *a, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_send_t *sc;
    (void)flags;
    /* the payload must outlive the async send: copy it into a heap context so a
     * connection can have several sends (header, body, ...) in flight at once */
    sc = (uaio_send_t *)malloc(sizeof(*sc));
    if (!sc) return -1;
    sc->buf = (uint8_t *)malloc(len ? len : 1);
    if (!sc->buf) { free(sc); return -1; }
    memcpy(sc->buf, buf, len);
    sc->len = len; sc->off = 0; sc->fd = fd;
    sc->cb = cb; sc->udata = udata;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) { free(sc->buf); free(sc); a->inflight--; return -1; }
    io_uring_prep_send(sqe, fd, sc->buf, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)sc | SEND_BIT);
    io_uring_submit(&a->ring); /* submit now: sends are queued from timer/promise
                                * callbacks too, not only from a drain */
    return 0;
}

int dyn_aio_sendfile(dyn_aio_t *a, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata)
{
    /* Read the file (bounded by the static maxFileSize) and queue it as an
     * ordered io_uring send after the header. True zero-copy io_uring splice is
     * a follow-up; this preserves the completion model + ordering correctly. */
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    off_t off = offset;
    size_t got = 0;
    int rc = 0;
    if (!buf) { close(in_fd); if (cb) cb(a, -1, NULL, 0, udata); return 0; }
    while (got < len) {
        ssize_t r = pread(in_fd, buf + got, len - got, off + (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(in_fd);
    if (got > 0)
        rc = dyn_aio_send(a, out_fd, buf, got, 0, cb, udata);
    else if (cb)
        cb(a, 0, NULL, 0, udata);
    free(buf);
    return rc;
}

int dyn_aio_close(dyn_aio_t *a, int fd)
{
    if (fd >= 0 && fd < a->cap) {
        uaio_fd_t *s = &a->fds[fd];
        if (s->r_op && a->inflight) a->inflight--;
        memset(s, 0, sizeof(*s));
    }
    close(fd); /* cancels outstanding multishot recv/accept on this fd */
    return 0;
}

int dyn_aio_pool_register(dyn_aio_t *a, unsigned n, unsigned sz)
{ (void)a; (void)n; (void)sz; return 0; /* provided-buffer ring is set up in _new */ }
/* ---- disk: straight to the kernel ---- */

static uaio_disk_t *uaio_disk_new(dyn_aio_cb cb, void *udata, const char *path)
{
    uaio_disk_t *dc = (uaio_disk_t *)calloc(1, sizeof(*dc));
    if (!dc)
        return NULL;
    dc->cb = cb;
    dc->udata = udata;
    if (path) {
        dc->path = strdup(path);   /* the caller's string may not outlive us */
        if (!dc->path) { free(dc); return NULL; }
    }
    return dc;
}

/* Tag, submit, account. Frees `dc` and returns -1 if no SQE is available. */
static int uaio_disk_go(dyn_aio_t *a, struct io_uring_sqe *sqe, uaio_disk_t *dc)
{
    if (!sqe) {
        free(dc->path);
        free(dc);
        errno = EAGAIN;
        return -1;
    }
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)dc | DISK_BIT);
    a->inflight++;
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_openat(dyn_aio_t *a, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata)
{
    uaio_disk_t *dc;
    struct io_uring_sqe *sqe;
    if (!a || !path)
        return -1;
    dc = uaio_disk_new(cb, udata, path);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)
        io_uring_prep_openat(sqe, dirfd, dc->path, flags, (mode_t)mode);
    return uaio_disk_go(a, sqe, dc);
}

int dyn_aio_read(dyn_aio_t *a, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata)
{
    uaio_disk_t *dc;
    struct io_uring_sqe *sqe;
    if (!a || !buf)
        return -1;
    dc = uaio_disk_new(cb, udata, NULL);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)  /* -1 offset means "use the file's current position" to io_uring */
        io_uring_prep_read(sqe, fd, buf, (unsigned)len,
                           (off < 0) ? (uint64_t)-1 : (uint64_t)off);
    return uaio_disk_go(a, sqe, dc);
}

int dyn_aio_write(dyn_aio_t *a, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata)
{
    uaio_disk_t *dc;
    struct io_uring_sqe *sqe;
    if (!a || !buf)
        return -1;
    dc = uaio_disk_new(cb, udata, NULL);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)
        io_uring_prep_write(sqe, fd, buf, (unsigned)len,
                            (off < 0) ? (uint64_t)-1 : (uint64_t)off);
    return uaio_disk_go(a, sqe, dc);
}

int dyn_aio_fsync(dyn_aio_t *a, int fd, int datasync, dyn_aio_cb cb, void *ud)
{
    uaio_disk_t *dc;
    struct io_uring_sqe *sqe;
    if (!a)
        return -1;
    dc = uaio_disk_new(cb, ud, NULL);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)
        io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
    return uaio_disk_go(a, sqe, dc);
}

/* Disk completions arrive on the ring itself here, so there is no separate fd
 * and nothing extra to drain -- dyn_aio_drain() already handles them. */
int dyn_aio_disk_fd(const dyn_aio_t *a) { (void)a; return -1; }
/* io_uring has IORING_OP_TIMEOUT, but a periodic wakeup here would need a
 * self-rearming SQE and its own completion tag. Not needed yet: the only caller
 * is the App's idle sweep and the App runs on the readiness backend. */
/* io_uring has IORING_OP_CONNECT; wiring it needs a sockaddr that outlives the
 * SQE, so it shares the disk context's heap-and-tag shape. Not built yet -- the
 * only caller today is the App, which accepts rather than connects. */
int dyn_aio_connect(dyn_aio_t *a, const char *host, uint16_t port,
                    dyn_aio_cb cb, void *udata)
{ (void)a;(void)host;(void)port;(void)cb;(void)udata; errno = ENOSYS; return -1; }

/* THREE OF THESE NEED NO io_uring AT ALL and are implemented here rather than
 * stubbed: binding and listening are synchronous socket setup, and a datagram
 * send is inline by nature -- a UDP write either fits the socket buffer or is
 * dropped, so there is no completion to wait for. Stubbing them made half of
 * dyna:net fail under CONFIG_IO_URING for no reason. What genuinely needs a
 * submission queue -- an asynchronous connect and a recvmsg, whose sockaddr,
 * msghdr and iovec must outlive the SQE -- is still below. */
int dyn_aio_unix_listen(dyn_aio_t *a, const char *path, int backlog)
{
    struct sockaddr_un sa;
    int fd;
    (void)a;
    if (!path || strlen(path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;        /* REFUSED, never truncated */
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path));
    /* A stale socket file makes bind fail with EADDRINUSE; the DIRECTORY's
     * permissions are the real access control, exactly as on the other backend. */
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 128) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_aio_unix_connect(dyn_aio_t *a, const char *path, dyn_aio_cb cb, void *ud)
{ (void)a;(void)path;(void)cb;(void)ud; errno = ENOSYS; return -1; }

int dyn_aio_udp_bind(dyn_aio_t *a, const char *host, uint16_t port)
{
    struct sockaddr_in sa;
    int fd, on = 1;
    (void)a;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = host ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_aio_recvfrom(dyn_aio_t *a, int fd, dyn_aio_dgram_cb cb, void *ud)
{ (void)a;(void)fd;(void)cb;(void)ud; errno = ENOSYS; return -1; }

int dyn_aio_sendto(dyn_aio_t *a, int fd, const void *buf, size_t len,
                   const struct sockaddr *peer, unsigned peerlen)
{
    ssize_t n;
    (void)a;
    /* Inline, like the readiness backend and for the same reason: a datagram
     * either fits the socket buffer or is dropped, so there is no partial-write
     * state to keep and queueing would only add a copy. */
    do {
        /* NULL peer means the socket is connect()ed; sendto() with an address
         * on such a socket is EISCONN on BSD, so these are different calls. */
        n = peer ? sendto(fd, buf, len, 0, peer, (socklen_t)peerlen)
                 : send(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int)n;
}

/* Arm the periodic wakeup this backend previously did not have.
 *
 * The readiness backend delegates to its event loop; there is none here, so a
 * timerfd is registered into the ring and re-armed from its own completion.
 * Reading it is MANDATORY: a timerfd is level-triggered and stays readable
 * until its expiration count is consumed, so a wakeup that never reads spins
 * at 100% CPU.
 *
 * Until this existed every dyn_net_on_drain hook -- the App idle-timeout
 * sweep, the DNS deadline, the Redis and Postgres command timeouts -- ran only
 * when other traffic woke the loop, so the quiet server that needs them most
 * never ran them. */
static int uaio_arm_timer(dyn_aio_t *a)
{
    struct io_uring_sqe *sqe = uaio_sqe(a);
    if (!sqe)
        return -1;
    io_uring_prep_read(sqe, a->tfd, &a->tick_buf, sizeof(a->tick_buf), 0);
    io_uring_sqe_set_data64(sqe, UD(a->tfd, UOP_TIMER));
    return 0;
}

int dyn_aio_set_timer(dyn_aio_t *a, unsigned period_ms)
{
    struct itimerspec its;
    if (!a) { errno = EINVAL; return -1; }
    if (a->tfd < 0) {
        a->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (a->tfd < 0)
            return -1;
    }
    memset(&its, 0, sizeof(its));
    if (period_ms == 0) {          /* 0 disarms, matching the readiness backend */
        a->tick_ms = 0;
        return timerfd_settime(a->tfd, 0, &its, NULL);
    }
    a->tick_ms = period_ms;
    its.it_interval.tv_sec  = period_ms / 1000;
    its.it_interval.tv_nsec = (long)(period_ms % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (timerfd_settime(a->tfd, 0, &its, NULL) < 0)
        return -1;
    return uaio_arm_timer(a);
}
void dyn_aio_disk_drain(dyn_aio_t *a) { (void)a; }
/* Disarm every armed op matching the (cb, udata) cookie. Same contract as the
 * readiness backend: the callback is NOT invoked for a cancelled op. The local
 * state is cleared first so the CQE that IORING_OP_ASYNC_CANCEL provokes finds
 * r_op == 0 and dispatches nothing. */
int dyn_aio_cancel(dyn_aio_t *a, dyn_aio_cb cb, void *udata)
{
    int fd, n = 0;
    if (!a)
        return -1;
    for (fd = 0; fd < a->cap; fd++) {
        uaio_fd_t *s = &a->fds[fd];
        uint64_t ud;
        struct io_uring_sqe *sqe;
        if (s->r_cb != cb || s->r_udata != udata)
            continue;
        if (s->r_op != UOP_ACCEPT && s->r_op != UOP_RECV)
            continue;
        ud = UD(fd, s->r_op);
        s->r_cb = NULL; s->r_udata = NULL; s->r_op = 0; s->r_multishot = 0;
        if (a->inflight) a->inflight--;
        sqe = uaio_sqe(a);
        if (sqe) {
            io_uring_prep_cancel64(sqe, ud, 0);
            io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE)); /* reaped, ignored */
            io_uring_submit(&a->ring);
        }
        n++;
    }
    return n;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_IO_URING && __linux__ */
