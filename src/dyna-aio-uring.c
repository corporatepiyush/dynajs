#include <stdio.h>
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <limits.h>
#include <stddef.h>
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
#include <poll.h>
#include <sys/eventfd.h>
#include <liburing.h>
#include "core/dyn-pool.h"
#include "dyna-evloop.h"

#define URING_ENTRIES 8192
#ifndef URING_NBUFS
#define URING_NBUFS 256
#endif
#ifndef URING_BUFSZ
#define URING_BUFSZ 16384
#endif
#define URING_BGID 1

_Static_assert(URING_NBUFS > 0 && URING_NBUFS < UINT_MAX,
    "URING_NBUFS must fit the unsigned per-fd ENOBUFS counter");

enum { UOP_ACCEPT = 0,
    UOP_RECV = 1,
    UOP_SEND = 2,
    UOP_CLOSE = 3,
    UOP_TIMER = 4,
    UOP_RECVFROM = 5,
    UOP_WATCH = 6,
    UOP_CONNECT = 7 };
#define UD_GEN_MASK 0x1fffffffu
#define UD(fd, op, gen) (((uint64_t)(unsigned)(fd) << 10) | ((uint64_t)((gen) & UD_GEN_MASK) << 30) | (unsigned)(op))
#define UD_FD(ud) ((int)(((ud) >> 10) & 0xfffffu))
#define UD_OP(ud) ((int)((ud) & 7))
#define UD_GEN(ud) ((uint32_t)(((ud) >> 30) & UD_GEN_MASK))

typedef struct {
    dyn_aio_cb r_cb;
    void* r_udata;
    uint8_t r_op;
    uint8_t r_multishot;
    uint32_t r_gen;
    unsigned r_enobufs;
    void* r_msg;
    struct uaio_wnode* w_cur;
    struct uaio_wnode *w_qhead, *w_qtail;
} uaio_fd_t;

typedef struct {
    struct msghdr mh;
    struct iovec iov;
    struct sockaddr_storage ss;
} uaio_dgram_t;

#define WNODE_BIT (1ULL << 63)

enum { UW_SEND = 1,
    UW_FILE = 2 };
enum { US_NONE = 0,
    US_IN = 1,
    US_OUT = 2 };

typedef struct uaio_wnode {
    struct uaio_wnode* next;
    uint8_t kind;
    uint8_t step;
    uint8_t eof;
    uint8_t dead;
    int fd;
    dyn_aio_cb cb;
    void* udata;
    uint8_t* buf;
    size_t len, off;
    int in_fd;
    int pp[2];
    off_t rem;
    size_t pipe_cap;
    size_t in_pipe;
    size_t sent;
} uaio_wnode_t;
#define DISK_BIT (1ULL << 62)

#define POOL_BIT (1ULL << 61)
#define CONN_BIT (1ULL << 60)
#define EVL_BIT (1ULL << 59)
typedef struct {
    dyn_aio_cb cb;
    void* udata;
    char* path;
} uaio_disk_t;
typedef struct {
    struct sockaddr_storage sa;
    socklen_t salen;
    int fd;
    uint32_t gen;
    dyn_aio_cb cb;
    void* udata;
} uaio_conn_t;

struct dyn_aio {
    struct io_uring ring;
    struct io_uring_buf_ring* br;
    unsigned char* buf_base;
    unsigned nbufs, bufsz;
    int evfd;
    int tfd;
    unsigned tick_ms;
    uint8_t timer_armed;
    uint64_t tick_buf;
    struct dyn_evloop* evl;
    int evl_armed;
    uaio_fd_t* fds;
    int cap;
    size_t inflight;
    dyn_pool_t* pool;
    dyn_pool_chan_t* chan;
};

static int uaio_fd_ensure(dyn_aio_t* a, int fd)
{
    int nc;
    uaio_fd_t* nf;
    if (fd < 0 || (unsigned)fd >= (1u << 20))
        return -1;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 256;
    while (nc <= fd)
        nc *= 2;
    nf = (uaio_fd_t*)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf)
        return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

static int uaio_pool_ready(dyn_aio_t* a);
static int uaio_arm_timer(dyn_aio_t* a);
static void uaio_cand_done(dyn_aio_t* a, int res, const uint8_t* buf,
    unsigned n, void* ud);

static struct io_uring_sqe* uaio_sqe(dyn_aio_t* a)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&a->ring);
    if (!sqe) {
        io_uring_submit(&a->ring);
        sqe = io_uring_get_sqe(&a->ring);
    }
    return sqe;
}

static void uaio_recycle(dyn_aio_t* a, int bid)
{
    io_uring_buf_ring_add(a->br, a->buf_base + (size_t)bid * a->bufsz, a->bufsz,
        bid, io_uring_buf_ring_mask(a->nbufs), 0);
    io_uring_buf_ring_advance(a->br, 1);
}

static void uaio_wnode_free_res(uaio_wnode_t* w)
{
    if (w->kind == UW_SEND) {
        free(w->buf);
        w->buf = NULL;
    } else {
        if (w->in_fd >= 0) {
            close(w->in_fd);
            w->in_fd = -1;
        }
        if (w->pp[0] >= 0) {
            close(w->pp[0]);
            w->pp[0] = -1;
        }
        if (w->pp[1] >= 0) {
            close(w->pp[1]);
            w->pp[1] = -1;
        }
    }
}

static int uaio_pipe_open(uaio_wnode_t* w)
{
    int fds[2];

    if (pipe(fds) < 0)
        return -1;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    w->pp[0] = fds[0];
    w->pp[1] = fds[1];
    w->pipe_cap = 64 * 1024;
#ifdef F_SETPIPE_SZ
    {
        long cap;
        (void)fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
        cap = fcntl(fds[1], F_GETPIPE_SZ);
        if (cap > 0)
            w->pipe_cap = (size_t)cap;
    }
#endif
    if (w->pipe_cap > (1u << 20))
        w->pipe_cap = 1u << 20;
    return 0;
}

static void uaio_wnode_done(dyn_aio_t* a, int fd, uaio_wnode_t* w, int result)
{
    dyn_aio_cb cb = w->cb;
    void* u = w->udata;

    if (fd >= 0 && fd < a->cap && a->fds[fd].w_cur == w)
        a->fds[fd].w_cur = NULL;
    uaio_wnode_free_res(w);
    free(w);
    if (a->inflight)
        a->inflight--;
    if (cb)
        cb(a, result, NULL, 0, u);
}

static void uaio_wq_pump(dyn_aio_t* a, int fd)
{
    for (;;) {
        uaio_fd_t* s;
        uaio_wnode_t* w;
        struct io_uring_sqe* sqe;

        if (fd < 0 || fd >= a->cap)
            return;
        s = &a->fds[fd];
        if (s->w_cur || !s->w_qhead)
            return;
        w = s->w_qhead;
        s->w_qhead = w->next;
        if (!s->w_qhead)
            s->w_qtail = NULL;
        w->next = NULL;
        s->w_cur = w;
        sqe = uaio_sqe(a);
        if (!sqe) {
            uaio_wnode_done(a, fd, w, -EAGAIN);
            continue;
        }
        if (w->kind == UW_SEND)
            io_uring_prep_send(sqe, fd, w->buf + w->off, w->len - w->off,
                MSG_NOSIGNAL);
        else {
            w->step = US_IN;
            io_uring_prep_splice(sqe, w->in_fd, w->off, w->pp[1], -1,
                w->rem < (off_t)w->pipe_cap
                    ? (unsigned)w->rem
                    : (unsigned)w->pipe_cap,
                SPLICE_F_MOVE);
        }
        io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
        io_uring_submit(&a->ring);
        return;
    }
}

dyn_aio_t* dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t* a = (dyn_aio_t*)calloc(1, sizeof(*a));
    struct io_uring_params p;
    unsigned i;
    int ret = 0;
    (void)entries;
    (void)disk_workers;
    if (!a)
        return NULL;
    a->nbufs = URING_NBUFS;
    a->bufsz = URING_BUFSZ;

    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
        memset(&p, 0, sizeof(p));
        p.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
            memset(&p, 0, sizeof(p));
            if (io_uring_queue_init_params(URING_ENTRIES, &a->ring, &p) < 0) {
                free(a);
                return NULL;
            }
        }
    }
    a->buf_base = (unsigned char*)malloc((size_t)a->nbufs * a->bufsz);
    a->br = a->buf_base ? io_uring_setup_buf_ring(&a->ring, a->nbufs, URING_BGID, 0, &ret)
                        : NULL;
    a->evl = dyn_evloop_new();
    a->tfd = -1;
    a->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!a->buf_base || !a->br || !a->evl || a->evfd < 0 || io_uring_register_eventfd(&a->ring, a->evfd) < 0) {
        if (a->br)
            io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
        if (a->tfd >= 0)
            close(a->tfd);
        if (a->evfd >= 0)
            close(a->evfd);
        if (a->evl)
            dyn_evloop_free(a->evl);
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

static void uaio_wq_destroy(dyn_aio_t* a)
{
    int fd;
    if (!a->fds)
        return;
    for (fd = 0; fd < a->cap; fd++) {
        uaio_wnode_t *q = a->fds[fd].w_qhead, *n;
        if (a->fds[fd].w_cur) {
            uaio_wnode_free_res(a->fds[fd].w_cur);
            free(a->fds[fd].w_cur);
        }
        while (q) {
            n = q->next;
            uaio_wnode_free_res(q);
            free(q);
            q = n;
        }
        free(a->fds[fd].r_msg);
        a->fds[fd].r_msg = NULL;
    }
}

void dyn_aio_free(dyn_aio_t* a)
{
    if (!a)
        return;
    if (a->chan)
        dyn_pool_chan_free(a->chan);
    if (a->pool)
        dyn_pool_free(a->pool);
    if (a->br)
        io_uring_free_buf_ring(&a->ring, a->br, a->nbufs, URING_BGID);
    if (a->tfd >= 0)
        close(a->tfd);
    if (a->evfd >= 0)
        close(a->evfd);
    free(a->buf_base);
    io_uring_queue_exit(&a->ring);
    if (a->evl)
        dyn_evloop_free(a->evl);
    uaio_wq_destroy(a);
    free(a->fds);
    free(a);
}

typedef struct {
    dyn_aio_t* aio;
    void (*work)(void*);
    void (*done)(void*);
    void* arg;
} uaio_offload_t;

static void uaio_offload_work(void* p)
{
    uaio_offload_t* j = (uaio_offload_t*)p;
    if (j->work)
        j->work(j->arg);
}

static void uaio_offload_done(void* p)
{
    uaio_offload_t* j = (uaio_offload_t*)p;
    if (j->aio->inflight)
        j->aio->inflight--;
    if (j->done)
        j->done(j->arg);
    free(j);
}

int dyn_aio_offload(dyn_aio_t* a, void (*work)(void*), void (*done)(void*),
    void* arg)
{
    uaio_offload_t* j;
    if (!a)
        return -1;
    j = (uaio_offload_t*)calloc(1, sizeof(*j));
    if (!j) {
        if (work)
            work(arg);
        if (done)
            done(arg);
        return 1;
    }
    j->aio = a;
    j->work = work;
    j->done = done;
    j->arg = arg;
    a->inflight++;
    if (uaio_pool_ready(a) == 0 && dyn_pool_submit(a->chan, uaio_offload_work, uaio_offload_done, j) == 0)
        return 0;
    uaio_offload_work(j);
    uaio_offload_done(j);
    return 1;
}

int dyn_aio_backend_fd(const dyn_aio_t* a) { return a->evfd; }
size_t dyn_aio_inflight(const dyn_aio_t* a) { return a->inflight; }

static void uaio_rearm_recv(dyn_aio_t* a, int fd, int multishot)
{
    struct io_uring_sqe* sqe = uaio_sqe(a);
    if (!sqe)
        return;
    uaio_fd_t* s = &a->fds[fd];
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    if (multishot)
        io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    else
        io_uring_prep_recv(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV, s->r_gen));
}

static int uaio_arm_recvfrom(dyn_aio_t* a, int fd)
{
    struct io_uring_sqe* sqe;
    uaio_fd_t* s;
    uaio_dgram_t* dg;

    if (fd < 0 || fd >= a->cap)
        return -1;
    s = &a->fds[fd];
    dg = (uaio_dgram_t*)s->r_msg;
    if (s->r_op != UOP_RECVFROM || !dg)
        return -1;
    sqe = uaio_sqe(a);
    if (!sqe)
        return -1;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    io_uring_prep_recvmsg_multishot(sqe, fd, &dg->mh, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECVFROM, s->r_gen));
    return 0;
}

static int uaio_recvfrom_still(const dyn_aio_t* a, int fd, uint64_t ud)
{
    return fd < a->cap && a->fds[fd].r_op == UOP_RECVFROM
        && a->fds[fd].r_gen == UD_GEN(ud);
}

static void uaio_disarm_recvfrom(dyn_aio_t* a, int fd)
{
    uaio_fd_t* s = &a->fds[fd];
    free(s->r_msg);
    s->r_msg = NULL;
    s->r_cb = NULL;
    s->r_udata = NULL;
    s->r_op = 0;
    s->r_multishot = 0;
    if (a->inflight)
        a->inflight--;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
}

static void uaio_pool_arm(dyn_aio_t* a)
{
    struct io_uring_sqe* sqe;
    int wfd = a->chan ? dyn_pool_wake_fd(a->chan) : -1;
    if (wfd < 0)
        return;
    sqe = uaio_sqe(a);
    if (!sqe)
        return;
    io_uring_prep_poll_multishot(sqe, wfd, POLLIN);
    io_uring_sqe_set_data64(sqe, POOL_BIT);
}

static void uaio_evl_arm(dyn_aio_t* a)
{
    struct io_uring_sqe* sqe;
    int efd;

    if (!a->evl || a->evl_armed || dyn_evloop_count(a->evl) == 0)
        return;
    efd = dyn_evloop_backend_fd(a->evl);
    if (efd < 0)
        return;
    sqe = uaio_sqe(a);
    if (!sqe)
        return;
    io_uring_prep_poll_multishot(sqe, efd, POLLIN);
    io_uring_sqe_set_data64(sqe, EVL_BIT);
    io_uring_submit(&a->ring);
    a->evl_armed = 1;
}

static int uaio_pool_ready(dyn_aio_t* a)
{
    if (a->chan)
        return 0;
    if (!a->pool) {
        a->pool = dyn_pool_new(0, 0);
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

static void uaio_dispatch(dyn_aio_t* a, struct io_uring_cqe* cqe)
{
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    int fd, op, res = cqe->res;
    uaio_fd_t* s;

    if (ud & POOL_BIT) {
        if (a->chan)
            dyn_pool_drain(a->chan);
        if (!(cqe->flags & IORING_CQE_F_MORE))
            uaio_pool_arm(a);
        return;
    }
    if (ud & EVL_BIT) {
        if (a->evl)
            dyn_evloop_poll(a->evl, 0);
        if (!(cqe->flags & IORING_CQE_F_MORE))
            a->evl_armed = 0;
        uaio_evl_arm(a);
        return;
    }
    if (ud & DISK_BIT) {
        uaio_disk_t* dc = (uaio_disk_t*)(uintptr_t)(ud & ~DISK_BIT);
        if (a->inflight)
            a->inflight--;
        if (dc->cb)
            dc->cb(a, res, NULL, 0, dc->udata);
        free(dc->path);
        free(dc);
        return;
    }
    if (ud & CONN_BIT) {
        uaio_conn_t* cc = (uaio_conn_t*)(uintptr_t)(ud & ~CONN_BIT);
        int cfd = cc->fd;
        if (cfd < 0 || cfd >= a->cap || a->fds[cfd].r_op != UOP_CONNECT || a->fds[cfd].r_gen != cc->gen) {
            if (cc->cb == uaio_cand_done)
                free(cc->udata);
            free(cc);
            return;
        }
        if (a->inflight)
            a->inflight--;
        a->fds[cfd].r_op = 0;
        if (cc->cb)
            cc->cb(a, res, NULL, 0, cc->udata);
        free(cc);
        return;
    }
    if (ud & WNODE_BIT) {
        uaio_wnode_t* w = (uaio_wnode_t*)(uintptr_t)(ud & ~WNODE_BIT);
        int wfd = w->fd;

        if (w->dead) {
            uaio_wnode_free_res(w);
            free(w);
            if (a->inflight)
                a->inflight--;
            return;
        }
        if (w->kind == UW_SEND) {
            if (res > 0)
                w->off += (size_t)res;
            if ((res > 0 || res == -EAGAIN) && w->off < w->len) {
                struct io_uring_sqe* sqe = uaio_sqe(a);
                if (sqe) {
                    io_uring_prep_send(sqe, wfd, w->buf + w->off,
                        w->len - w->off, MSG_NOSIGNAL);
                    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
                    io_uring_submit(&a->ring);
                    return;
                }
            }
            uaio_wnode_done(a, wfd, w, res < 0 ? res : (int)w->off);
            uaio_wq_pump(a, wfd);
            return;
        }
        if (w->step == US_IN) {
            if (res > 0) {
                w->off += res;
                w->rem -= res;
                w->in_pipe += (size_t)res;
            } else if (res == 0) {
                w->eof = 1;
            } else if (res != -EAGAIN) {
                uaio_wnode_done(a, wfd, w, res);
                uaio_wq_pump(a, wfd);
                return;
            }
            if (w->in_pipe > 0)
                w->step = US_OUT;
            else if (w->eof) {
                uaio_wnode_done(a, wfd, w, (int)w->sent);
                uaio_wq_pump(a, wfd);
                return;
            } else {
                w->step = US_IN;
            }
        } else {
            if (res > 0) {
                w->in_pipe -= (size_t)res;
                w->sent += (size_t)res;
            }
            if (res < 0 && res != -EAGAIN) {
                uaio_wnode_done(a, wfd, w, res);
                uaio_wq_pump(a, wfd);
                return;
            }
            if (w->in_pipe > 0)
                w->step = US_OUT;
            else if (w->rem > 0 && !w->eof)
                w->step = US_IN;
            else {
                uaio_wnode_done(a, wfd, w, (int)w->sent);
                uaio_wq_pump(a, wfd);
                return;
            }
        }
        {
            struct io_uring_sqe* sqe = uaio_sqe(a);
            if (!sqe) {
                uaio_wnode_done(a, wfd, w, -EAGAIN);
                uaio_wq_pump(a, wfd);
                return;
            }
            if (w->step == US_OUT)
                io_uring_prep_splice(sqe, w->pp[0], -1, wfd, -1,
                    (unsigned)w->in_pipe, SPLICE_F_MOVE);
            else
                io_uring_prep_splice(sqe, w->in_fd, w->off, w->pp[1], -1,
                    w->rem < (off_t)w->pipe_cap
                        ? (unsigned)w->rem
                        : (unsigned)w->pipe_cap,
                    SPLICE_F_MOVE);
            io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
            io_uring_submit(&a->ring);
        }
        return;
    }
    fd = UD_FD(ud);
    op = UD_OP(ud);
    if (op == UOP_TIMER) {
        a->timer_armed = 0;
        if (a->tick_ms)
            (void)uaio_arm_timer(a);
        return;
    }
    if (fd < 0 || fd >= a->cap)
        return;
    s = &a->fds[fd];

    if (UD_GEN(ud) != s->r_gen) {
        if (cqe->flags & IORING_CQE_F_BUFFER)
            uaio_recycle(a, (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT));
        return;
    }
    if (op == UOP_ACCEPT) {
        dyn_aio_cb cb = s->r_cb;
        void* u = s->r_udata;
        if (res >= 0) {
            int on = 1;
            dyn_net_set_nonblock(res);
            setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            if (cb)
                cb(a, res, NULL, 0, u);
            s = &a->fds[fd];
            if (UD_GEN(ud) != s->r_gen || s->r_op != UOP_ACCEPT)
                return;
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && res != -ECANCELED) {
            struct io_uring_sqe* sqe = uaio_sqe(a);
            if (sqe) {
                s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
                io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
                io_uring_sqe_set_data64(sqe, UD(fd, UOP_ACCEPT, s->r_gen));
            }
        }
        return;
    }
    if (op == UOP_RECV) {
        dyn_aio_cb cb = s->r_cb;
        void* u = s->r_udata;
        int bid = (cqe->flags & IORING_CQE_F_BUFFER)
            ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
            : -1;
        if (res == -ENOBUFS) {
            if (s->r_enobufs++ < a->nbufs) {
                uaio_rearm_recv(a, fd, s->r_multishot);
                return;
            }
            s->r_enobufs = 0;
            if (a->inflight)
                a->inflight--;
            s->r_op = 0;
            s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
            if (cb)
                cb(a, -ENOBUFS, NULL, 0, u);
            return;
        }
        if (res > 0 && bid >= 0) {
            int single = !s->r_multishot;
            s->r_enobufs = 0;
            if (single) {
                s->r_op = 0;
                s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
                if (a->inflight)
                    a->inflight--;
            }
            if (cb)
                cb(a, res, a->buf_base + (size_t)bid * a->bufsz, (unsigned)res, u);
            uaio_recycle(a, bid);
            if (single)
                return;
        } else {
            if (bid >= 0)
                uaio_recycle(a, bid);
            if (a->inflight)
                a->inflight--;
            s->r_op = 0;
            s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
            if (cb)
                cb(a, res <= 0 ? (res == 0 ? 0 : res) : 0, NULL, 0, u);
            return;
        }
        if (fd >= a->cap)
            return;
        s = &a->fds[fd];
        if (s->r_op != UOP_RECV || s->r_gen != UD_GEN(ud))
            return;
        if (s->r_multishot && !(cqe->flags & IORING_CQE_F_MORE))
            uaio_rearm_recv(a, fd, 1);
        return;
    }
    if (op == UOP_RECVFROM) {
        dyn_aio_dgram_cb dcb = (dyn_aio_dgram_cb)(void*)s->r_cb;
        void* u = s->r_udata;
        int bid = (cqe->flags & IORING_CQE_F_BUFFER)
            ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
            : -1;
        if (res == -ENOBUFS) {
            if (s->r_enobufs++ < a->nbufs) {
                uaio_arm_recvfrom(a, fd);
                return;
            }
            s->r_enobufs = 0;
            if (dcb)
                dcb(a, -ENOBUFS, NULL, 0, NULL, 0, u);
            if (uaio_recvfrom_still(a, fd, ud))
                uaio_disarm_recvfrom(a, fd);
            return;
        }
        if (res > 0 && bid >= 0) {
            unsigned char* b = a->buf_base + (size_t)bid * a->bufsz;
            s->r_enobufs = 0;
            struct msghdr* mh = &((uaio_dgram_t*)s->r_msg)->mh;
            struct io_uring_recvmsg_out* o = io_uring_recvmsg_validate(b, res, mh);
            if (o) {
                if (dcb)
                    dcb(a, (int)o->payloadlen,
                        (const uint8_t*)io_uring_recvmsg_payload(o, mh),
                        o->payloadlen,
                        (const struct sockaddr*)io_uring_recvmsg_name(o),
                        o->namelen, u);
            } else if (dcb) {
                dcb(a, -EPROTO, NULL, 0, NULL, 0, u);
            }
            uaio_recycle(a, bid);
        } else {
            if (bid >= 0)
                uaio_recycle(a, bid);
            if (dcb)
                dcb(a, res < 0 ? res : -EIO, NULL, 0, NULL, 0, u);
            if (uaio_recvfrom_still(a, fd, ud))
                uaio_disarm_recvfrom(a, fd);
            return;
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && uaio_recvfrom_still(a, fd, ud))
            uaio_arm_recvfrom(a, fd);
        return;
    }
    if (op == UOP_WATCH) {
        void (*wcb)(dyn_aio_t*, int, void*) = (void (*)(dyn_aio_t*, int, void*))(void*)s->r_cb;
        void* u = s->r_udata;
        if (res < 0) {
            if (a->inflight)
                a->inflight--;
            s->r_op = 0;
            s->r_cb = NULL;
            s->r_udata = NULL;
            s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
            return;
        }
        if (wcb)
            wcb(a, fd, u);
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            struct io_uring_sqe* sqe;
            if (fd >= a->cap || a->fds[fd].r_op != UOP_WATCH)
                return;
            sqe = uaio_sqe(a);
            if (sqe) {
                s = &a->fds[fd];
                s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
                io_uring_prep_poll_multishot(sqe, fd, POLLIN);
                io_uring_sqe_set_data64(sqe, UD(fd, UOP_WATCH, s->r_gen));
            }
        }
        return;
    }
    (void)op;
}

void dyn_aio_drain(void* aio)
{
    dyn_aio_t* a = (dyn_aio_t*)aio;
    struct io_uring_cqe* cqe;
    unsigned head, n = 0;
    uint64_t v;
    ssize_t rd = read(a->evfd, &v, sizeof(v));
    (void)rd;
    uaio_evl_arm(a);
    io_uring_submit(&a->ring);
    io_uring_for_each_cqe(&a->ring, head, cqe)
    {
        uaio_dispatch(a, cqe);
        n++;
    }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring);
}

int dyn_aio_run(dyn_aio_t* a, int timeout_ms)
{
    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts, *pts = NULL;
    unsigned head, n = 0;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    uaio_evl_arm(a);
    io_uring_submit_and_wait_timeout(&a->ring, &cqe, 1, pts, NULL);
    io_uring_for_each_cqe(&a->ring, head, cqe)
    {
        uaio_dispatch(a, cqe);
        n++;
    }
    io_uring_cq_advance(&a->ring, n);
    io_uring_submit(&a->ring);
    return (int)n;
}

int dyn_aio_listen(dyn_aio_t* a, const char* host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;
    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
        int v6only = 0;
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        {
            const char* h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf))
                hl = sizeof(hbuf) - 1;
            if (h[0] == '[') {
                h++;
                if (hl > 0)
                    hl--;
                if (hl > 0 && h[hl - 1] == ']')
                    hl--;
            }
            memcpy(hbuf, h, hl);
            hbuf[hl] = '\0';
        }
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(port);
        if (inet_pton(AF_INET6, hbuf, &sa6.sin6_addr) != 1) {
            close(fd);
            return -1;
        }
        if (bind(fd, (struct sockaddr*)&sa6, sizeof(sa6)) < 0 || listen(fd, backlog > 0 ? backlog : 1024) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(fd, backlog > 0 ? backlog : 1024) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_accept(dyn_aio_t* a, int listen_fd, dyn_aio_cb cb, void* udata)
{
    struct io_uring_sqe* sqe;
    uaio_fd_t* s;
    if (!a || listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, listen_fd) < 0)
        return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = UOP_ACCEPT;
    s->r_multishot = 1;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        if (a->inflight)
            a->inflight--;
        s->r_op = 0;
        s->r_multishot = 0;
        s->r_cb = NULL;
        s->r_udata = NULL;
        return -1;
    }
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD(listen_fd, UOP_ACCEPT, s->r_gen));
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_recv(dyn_aio_t* a, int fd, int pool, int multishot,
    dyn_aio_cb cb, void* udata)
{
    struct io_uring_sqe* sqe;
    uaio_fd_t* s;
    (void)pool;
    if (!a || !cb || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = UOP_RECV;
    s->r_multishot = multishot ? 1 : 0;
    s->r_enobufs = 0;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        if (a->inflight)
            a->inflight--;
        s->r_op = 0;
        s->r_multishot = 0;
        s->r_cb = NULL;
        s->r_udata = NULL;
        return -1;
    }
    if (s->r_multishot)
        io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    else
        io_uring_prep_recv(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV, s->r_gen));
    return 0;
}

int dyn_aio_send(dyn_aio_t* a, int fd, const void* buf, size_t len, int flags,
    dyn_aio_cb cb, void* udata)
{
    uaio_fd_t* s;
    uaio_wnode_t* w;
    size_t off = 0;
    (void)flags;

    if (!a || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0) {
        if (cb)
            cb(a, 0, NULL, 0, udata);
        return 0;
    }
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];

    if (!s->w_cur && !s->w_qhead) {
        for (;;) {
            ssize_t n = send(fd, (const uint8_t*)buf + off, len - off,
                MSG_NOSIGNAL);
            if (n > 0) {
                off += (size_t)n;
                if (off >= len)
                    break;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return -1;
        }
        if (off >= len) {
            if (cb)
                cb(a, (int)len, NULL, 0, udata);
            return 0;
        }
    }

    w = (uaio_wnode_t*)calloc(1, sizeof(*w));
    if (!w) {
        errno = ENOMEM;
        return -1;
    }
    w->buf = (uint8_t*)malloc(len - off);
    if (!w->buf) {
        free(w);
        errno = ENOMEM;
        return -1;
    }
    memcpy(w->buf, (const uint8_t*)buf + off, len - off);
    w->kind = UW_SEND;
    w->fd = fd;
    w->len = len - off;
    w->cb = cb;
    w->udata = udata;
    a->inflight++;
    if (s->w_qtail)
        s->w_qtail->next = w;
    else
        s->w_qhead = w;
    s->w_qtail = w;
    uaio_wq_pump(a, fd);
    return 0;
}

int dyn_aio_sendfile(dyn_aio_t* a, int out_fd, int in_fd, off_t offset,
    size_t len, dyn_aio_cb cb, void* udata)
{
    uaio_fd_t* s;
    uaio_wnode_t *w, *q;

    if (!a || out_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0) {
        close(in_fd);
        if (cb)
            cb(a, 0, NULL, 0, udata);
        return 0;
    }
    if (len > (size_t)UINT_MAX) {
        close(in_fd);
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, out_fd) < 0) {
        int e = errno;
        close(in_fd);
        errno = e;
        return -1;
    }
    s = &a->fds[out_fd];

    for (w = s->w_cur; w; w = w->next)
        if (w->kind == UW_FILE) {
            close(in_fd);
            errno = EBUSY;
            return -1;
        }
    for (q = s->w_qhead; q; q = q->next)
        if (q->kind == UW_FILE) {
            close(in_fd);
            errno = EBUSY;
            return -1;
        }

    w = (uaio_wnode_t*)calloc(1, sizeof(*w));
    if (!w) {
        close(in_fd);
        errno = ENOMEM;
        return -1;
    }
    if (uaio_pipe_open(w) < 0) {
        int e = errno;
        free(w);
        close(in_fd);
        errno = e;
        return -1;
    }
    w->kind = UW_FILE;
    w->fd = out_fd;
    w->in_fd = in_fd;
    w->off = offset;
    w->rem = (off_t)len;
    w->cb = cb;
    w->udata = udata;
    a->inflight++;
    if (s->w_qtail)
        s->w_qtail->next = w;
    else
        s->w_qhead = w;
    s->w_qtail = w;
    uaio_wq_pump(a, out_fd);
    return 0;
}

int dyn_aio_close(dyn_aio_t* a, int fd)
{
    if (fd >= 0 && fd < a->cap) {
        uaio_fd_t* s = &a->fds[fd];
        uint32_t g = s->r_gen;
        uaio_fd_t dead;
        if (s->r_op && a->inflight)
            a->inflight--;
        dead = *s;
        memset(s, 0, sizeof(*s));
        free(dead.r_msg);
        s->r_gen = (g + 1) & UD_GEN_MASK;
        {
            struct io_uring_sqe* sqe = uaio_sqe(a);
            if (sqe) {
                io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
                io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0));
                io_uring_submit(&a->ring);
            }
        }
        close(fd);
        if (dead.w_cur) {
            uaio_wnode_t* w = dead.w_cur;
            dyn_aio_cb cb = w->cb;
            void* u = w->udata;
            w->dead = 1;
            w->cb = NULL;
            if (cb)
                cb(a, -ECONNRESET, NULL, 0, u);
        }
        {
            uaio_wnode_t* q = dead.w_qhead;
            while (q) {
                uaio_wnode_t* n = q->next;
                uaio_wnode_done(a, fd, q, -ECONNRESET);
                q = n;
            }
        }
        return 0;
    }
    close(fd);
    return 0;
}

size_t dyn_aio_queued(const dyn_aio_t* a, int fd)
{
    const uaio_fd_t* s;
    const uaio_wnode_t* w;
    size_t n = 0;

    if (!a || fd < 0 || fd >= a->cap)
        return 0;
    s = &a->fds[fd];
    for (w = s->w_cur; w; w = w->next)
        n += (w->kind == UW_SEND) ? w->len - w->off
                                  : (size_t)w->rem + w->in_pipe;
    for (w = s->w_qhead; w; w = w->next)
        n += (w->kind == UW_SEND) ? w->len - w->off
                                  : (size_t)w->rem + w->in_pipe;
    return n;
}

int dyn_aio_pool_register(dyn_aio_t* a, unsigned n, unsigned sz)
{
    (void)a;
    (void)n;
    (void)sz;
    return 0;
}
static uaio_disk_t* uaio_disk_new(dyn_aio_cb cb, void* udata, const char* path)
{
    uaio_disk_t* dc = (uaio_disk_t*)calloc(1, sizeof(*dc));
    if (!dc)
        return NULL;
    dc->cb = cb;
    dc->udata = udata;
    if (path) {
        dc->path = strdup(path);
        if (!dc->path) {
            free(dc);
            return NULL;
        }
    }
    return dc;
}

static int uaio_disk_go(dyn_aio_t* a, struct io_uring_sqe* sqe, uaio_disk_t* dc)
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

int dyn_aio_openat(dyn_aio_t* a, int dirfd, const char* path, int flags,
    int mode, dyn_aio_cb cb, void* udata)
{
    uaio_disk_t* dc;
    struct io_uring_sqe* sqe;
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

int dyn_aio_read(dyn_aio_t* a, int fd, void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata)
{
    uaio_disk_t* dc;
    struct io_uring_sqe* sqe;
    if (!a || !buf)
        return -1;
    if (len > (size_t)UINT_MAX) {
        errno = EINVAL;
        return -1;
    }
    dc = uaio_disk_new(cb, udata, NULL);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)
        io_uring_prep_read(sqe, fd, buf, (unsigned)len,
            (off < 0) ? (uint64_t)-1 : (uint64_t)off);
    return uaio_disk_go(a, sqe, dc);
}

int dyn_aio_write(dyn_aio_t* a, int fd, const void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata)
{
    uaio_disk_t* dc;
    struct io_uring_sqe* sqe;
    if (!a || !buf)
        return -1;
    if (len > (size_t)UINT_MAX) {
        errno = EINVAL;
        return -1;
    }
    dc = uaio_disk_new(cb, udata, NULL);
    if (!dc)
        return -1;
    sqe = uaio_sqe(a);
    if (sqe)
        io_uring_prep_write(sqe, fd, buf, (unsigned)len,
            (off < 0) ? (uint64_t)-1 : (uint64_t)off);
    return uaio_disk_go(a, sqe, dc);
}

int dyn_aio_fsync(dyn_aio_t* a, int fd, int datasync, dyn_aio_cb cb, void* ud)
{
    uaio_disk_t* dc;
    struct io_uring_sqe* sqe;
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

int dyn_aio_disk_fd(const dyn_aio_t* a)
{
    (void)a;
    return -1;
}

struct dyn_evloop* dyn_aio_evloop(dyn_aio_t* a)
{
    return a ? a->evl : NULL;
}
static int uaio_connect_on(dyn_aio_t* a, int fd,
    const struct sockaddr_storage* sa,
    socklen_t salen, dyn_aio_cb cb, void* udata)
{
    struct io_uring_sqe* sqe;
    uaio_conn_t* cc;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    cc = (uaio_conn_t*)malloc(sizeof(*cc));
    if (!cc) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(&cc->sa, sa, sizeof(*sa));
    cc->salen = salen;
    cc->fd = fd;
    cc->cb = cb;
    cc->udata = udata;

    if (uaio_fd_ensure(a, fd) < 0) {
        free(cc);
        errno = ENOMEM;
        return -1;
    }
    a->fds[fd].r_op = UOP_CONNECT;
    a->fds[fd].r_gen = (a->fds[fd].r_gen + 1) & UD_GEN_MASK;
    cc->gen = a->fds[fd].r_gen;

    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        free(cc);
        a->fds[fd].r_op = 0;
        if (a->inflight)
            a->inflight--;
        errno = EAGAIN;
        return -1;
    }
    io_uring_prep_connect(sqe, fd, (struct sockaddr*)&cc->sa, cc->salen);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)cc | CONN_BIT);
    io_uring_submit(&a->ring);
    return fd;
}

static int uaio_connect_sa(dyn_aio_t* a, const struct sockaddr_storage* sa,
    socklen_t salen, int fam,
    dyn_aio_cb cb, void* udata)
{
    int fd;

    fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dyn_net_set_nonblock(fd);
    if (fam != AF_UNIX)
        dyn_net_set_nodelay(fd);
    if (uaio_connect_on(a, fd, sa, salen, cb, udata) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_connect_addr(dyn_aio_t* a, const struct sockaddr_storage* sa,
    socklen_t salen, int fam,
    dyn_aio_cb cb, void* udata)
{
    if (!a || !sa) {
        errno = EINVAL;
        return -1;
    }
    return uaio_connect_sa(a, sa, salen, fam, cb, udata);
}

typedef struct {
    dyn_aio_t* aio;
    char* host;
    uint16_t port;
    dyn_aio_cb cb;
    void* udata;
    int fd;
    dyn_aio_cand_t* cand;
    int err;
    int past_return;
} uaio_resolve_t;

static void uaio_resolve_work(void* p)
{
    uaio_resolve_t* c = (uaio_resolve_t*)p;
    if (dyn_aio_cand_collect(c->host, c->port, c->cand) != 0)
        c->err = EINVAL;
}

static void uaio_cand_done(dyn_aio_t* a, int res, const uint8_t* buf,
    unsigned n, void* ud)
{
    dyn_aio_cand_t* cand = (dyn_aio_cand_t*)ud;
    (void)buf;
    (void)n;

    if (res != 0 && cand->i + 1 < cand->n) {
        cand->i++;
        if (dyn_aio_cand_refresh_fd(cand) == 0 && uaio_connect_on(a, cand->fd, &cand->addr[cand->i], cand->len[cand->i], uaio_cand_done, cand) >= 0)
            return;
    }
    if (cand->cb)
        cand->cb(a, res, NULL, 0, cand->udata);
    free(cand);
}

static void uaio_resolve_done(void* p)
{
    uaio_resolve_t* c = (uaio_resolve_t*)p;
    dyn_aio_t* a = c->aio;

    if (c->fd < 0 || c->fd >= a->cap || a->fds[c->fd].r_op != UOP_CONNECT || a->fds[c->fd].r_cb != c->cb || a->fds[c->fd].r_udata != c->udata) {
        free(c->cand);
        free(c->host);
        free(c);
        return;
    }

    if (c->err == 0) {
        if (uaio_connect_on(a, c->fd, &c->cand->addr[0], c->cand->len[0],
                uaio_cand_done, c->cand)
            < 0) {
            c->err = errno ? errno : EIO;
        } else {
            c->cand = NULL;
        }
    }
    if (c->past_return) {
        if (c->err && c->cb)
            c->cb(a, -c->err, NULL, 0, c->udata);
        free(c->cand);
        free(c->host);
        free(c);
    } else if (c->err) {
        close(c->fd);
    }
}

int dyn_aio_connect(dyn_aio_t* a, const char* host, uint16_t port,
    dyn_aio_cb cb, void* udata)
{
    struct addrinfo hints, *res = NULL;
    struct sockaddr_storage sa;
    socklen_t salen = 0;
    int fam = AF_INET, fd, r;
    char portstr[16];
    uaio_resolve_t* c;
#ifdef IPV6_V6ONLY
    int v6only = 0;
#endif

    if (!a || !host) {
        errno = EINVAL;
        return -1;
    }

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(host, portstr, &hints, &res) == 0 && res) {
        int out;
        memcpy(&sa, res->ai_addr, res->ai_addrlen);
        salen = (socklen_t)res->ai_addrlen;
        fam = res->ai_family;
        freeaddrinfo(res);
        out = dyn_aio_connect_addr(a, &sa, salen, fam, cb, udata);
        return out;
    }
    if (res)
        freeaddrinfo(res);

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        if (dyn_aio_resolve(host, port, &sa, &salen, &fam) != 0) {
            errno = EINVAL;
            return -1;
        }
        return dyn_aio_connect_addr(a, &sa, salen, fam, cb, udata);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef IPV6_V6ONLY
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
    dyn_net_set_nonblock(fd);
    dyn_net_set_nodelay(fd);

    c = (uaio_resolve_t*)calloc(1, sizeof(*c));
    if (c) {
        c->host = strdup(host);
        c->cand = (dyn_aio_cand_t*)calloc(1, sizeof(*c->cand));
    }
    if (!c || !c->host || !c->cand) {
        close(fd);
        free(c->cand);
        free(c->host);
        free(c);
        errno = ENOMEM;
        return -1;
    }
    c->aio = a;
    c->port = port;
    c->cb = cb;
    c->udata = udata;
    c->fd = fd;
    c->cand->fd = fd;
    c->cand->cb = cb;
    c->cand->udata = udata;

    if (uaio_fd_ensure(a, fd) < 0) {
        close(fd);
        free(c->cand);
        free(c->host);
        free(c);
        errno = ENOMEM;
        return -1;
    }
    a->fds[fd].r_op = UOP_CONNECT;
    a->fds[fd].r_cb = cb;
    a->fds[fd].r_udata = udata;

    r = dyn_aio_offload(a, uaio_resolve_work, uaio_resolve_done, c);
    if (r == 1) {
        if (c->err) {
            int e = c->err;
            free(c->cand);
            free(c->host);
            free(c);
            a->fds[fd].r_op = 0;
            a->fds[fd].r_cb = NULL;
            a->fds[fd].r_udata = NULL;
            errno = e;
            return -1;
        }
        fd = c->fd;
        free(c->host);
        free(c);
        return fd;
    }
    if (r < 0) {
        close(fd);
        free(c->cand);
        free(c->host);
        free(c);
        a->fds[fd].r_op = 0;
        a->fds[fd].r_cb = NULL;
        a->fds[fd].r_udata = NULL;
        errno = EINVAL;
        return -1;
    }
    c->past_return = 1;
    return fd;
}

int dyn_aio_unix_listen(dyn_aio_t* a, const char* path, int backlog)
{
    struct sockaddr_un sa;
    int fd;
    (void)a;
    if (!path || strlen(path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
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
    unlink(path);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(fd, backlog > 0 ? backlog : 128) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_aio_unix_connect(dyn_aio_t* a, const char* path, dyn_aio_cb cb, void* ud)
{
    struct sockaddr_storage ss;
    struct sockaddr_un* un = (struct sockaddr_un*)&ss;
    size_t n;

    if (!a || !path) {
        errno = EINVAL;
        return -1;
    }
    n = strlen(path);
    if (n >= sizeof(un->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(&ss, 0, sizeof(ss));
    un->sun_family = AF_UNIX;
    memcpy(un->sun_path, path, n + 1);
    return uaio_connect_sa(a, &ss, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1),
        AF_UNIX, cb, ud);
}

int dyn_aio_udp_bind(dyn_aio_t* a, const char* host, uint16_t port)
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
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int dyn_aio_recvfrom(dyn_aio_t* a, int fd, dyn_aio_dgram_cb cb, void* ud)
{
    uaio_fd_t* s;
    uaio_dgram_t* dg;

    if (!a || !cb || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->r_op == UOP_RECVFROM) {
        s->r_cb = (dyn_aio_cb)(void*)cb;
        s->r_udata = ud;
        return 0;
    }
    if (s->r_op) {
        errno = EBUSY;
        return -1;
    }
    dg = (uaio_dgram_t*)calloc(1, sizeof(*dg));
    if (!dg) {
        errno = ENOMEM;
        return -1;
    }
    dg->mh.msg_name = &dg->ss;
    dg->mh.msg_namelen = sizeof(dg->ss);
    dg->mh.msg_iov = &dg->iov;
    dg->mh.msg_iovlen = 1;
    s->r_cb = (dyn_aio_cb)(void*)cb;
    s->r_udata = ud;
    s->r_op = UOP_RECVFROM;
    s->r_multishot = 1;
    s->r_msg = dg;
    a->inflight++;
    if (uaio_arm_recvfrom(a, fd) < 0) {
        uaio_disarm_recvfrom(a, fd);
        errno = EAGAIN;
        return -1;
    }
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_watch_fd(dyn_aio_t* a, int fd,
    void (*cb)(dyn_aio_t*, int, void*), void* ud)
{
    struct io_uring_sqe* sqe;
    uaio_fd_t* s;

    if (!a || !cb || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->r_op == UOP_WATCH) {
        s->r_cb = (dyn_aio_cb)(void*)cb;
        s->r_udata = ud;
        return 0;
    }
    if (s->r_op) {
        errno = EBUSY;
        return -1;
    }
    s->r_cb = (dyn_aio_cb)(void*)cb;
    s->r_udata = ud;
    s->r_op = UOP_WATCH;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        s->r_op = 0;
        s->r_cb = NULL;
        s->r_udata = NULL;
        if (a->inflight)
            a->inflight--;
        errno = EAGAIN;
        return -1;
    }
    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_WATCH, s->r_gen));
    io_uring_submit(&a->ring);
    return 0;
}

int dyn_aio_unwatch_fd(dyn_aio_t* a, int fd)
{
    uaio_fd_t* s;
    uint64_t ud;
    struct io_uring_sqe* sqe;

    if (!a || fd < 0 || fd >= a->cap)
        return -1;
    s = &a->fds[fd];
    if (s->r_op != UOP_WATCH)
        return 0;
    ud = UD(fd, UOP_WATCH, s->r_gen);
    s->r_op = 0;
    s->r_cb = NULL;
    s->r_udata = NULL;
    if (a->inflight)
        a->inflight--;
    s->r_gen = (s->r_gen + 1) & UD_GEN_MASK;
    sqe = uaio_sqe(a);
    if (sqe) {
        io_uring_prep_cancel64(sqe, ud, 0);
        io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0));
        io_uring_submit(&a->ring);
    }
    return 0;
}

int dyn_aio_sendto(dyn_aio_t* a, int fd, const void* buf, size_t len,
    const struct sockaddr* peer, unsigned peerlen)
{
    ssize_t n;
    (void)a;
    do {
        n = peer ? sendto(fd, buf, len, 0, peer, (socklen_t)peerlen)
                 : send(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int)n;
}

static int uaio_arm_timer(dyn_aio_t* a)
{
    struct io_uring_sqe* sqe;
    if (a->timer_armed)
        return 0;
    sqe = uaio_sqe(a);
    if (!sqe)
        return -1;
    io_uring_prep_read(sqe, a->tfd, &a->tick_buf, sizeof(a->tick_buf), 0);
    io_uring_sqe_set_data64(sqe, UD(a->tfd, UOP_TIMER, 0));
    io_uring_submit(&a->ring);
    a->timer_armed = 1;
    return 0;
}

int dyn_aio_set_timer(dyn_aio_t* a, unsigned period_ms)
{
    struct itimerspec its;
    if (!a) {
        errno = EINVAL;
        return -1;
    }
    if (a->tfd < 0) {
        a->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (a->tfd < 0)
            return -1;
    }
    memset(&its, 0, sizeof(its));
    if (period_ms == 0) {
        a->tick_ms = 0;
        return timerfd_settime(a->tfd, 0, &its, NULL);
    }
    a->tick_ms = period_ms;
    its.it_interval.tv_sec = period_ms / 1000;
    its.it_interval.tv_nsec = (long)(period_ms % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (timerfd_settime(a->tfd, 0, &its, NULL) < 0)
        return -1;
    return uaio_arm_timer(a);
}
void dyn_aio_disk_drain(dyn_aio_t* a) { (void)a; }
int dyn_aio_cancel(dyn_aio_t* a, dyn_aio_cb cb, void* udata)
{
    int fd, n = 0;
    if (!a)
        return -1;
    for (fd = 0; fd < a->cap; fd++) {
        uaio_fd_t* s = &a->fds[fd];
        uint64_t ud;
        uint32_t g;
        struct io_uring_sqe* sqe;
        if (s->r_cb != cb || s->r_udata != udata)
            continue;
        if (s->r_op != UOP_ACCEPT && s->r_op != UOP_RECV && s->r_op != UOP_RECVFROM && s->r_op != UOP_WATCH)
            continue;
        g = s->r_gen;
        ud = UD(fd, s->r_op, g);
        s->r_cb = NULL;
        s->r_udata = NULL;
        s->r_op = 0;
        s->r_multishot = 0;
        if (a->inflight)
            a->inflight--;
        if (s->r_msg) {
            free(s->r_msg);
            s->r_msg = NULL;
        }
        s->r_gen = (g + 1) & UD_GEN_MASK;
        sqe = uaio_sqe(a);
        if (sqe) {
            io_uring_prep_cancel64(sqe, ud, 0);
            io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0));
            io_uring_submit(&a->ring);
        }
        n++;
    }
    return n;
}

#endif
