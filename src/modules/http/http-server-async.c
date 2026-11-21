
#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_ACONN_IDLE_MS_DEFAULT 30000

static const char* const http_async_keys[] = { "port", "backlog", "host", "idleTimeoutMs", "maxConns", "routes", "workers" };

static _Thread_local long dyn_http_async_pending;

static _Thread_local JSContext* dyn_http_async_ctx;

static _Thread_local int dyn_http_async_hooked;

static void dyn_http_async_reap(void* unused)
{
    (void)unused;
    while (dyn_http_async_pending > 0 && dyn_http_async_ctx) {
        dyn_http_async_pending--;
        dyn_net_reactor_release(dyn_http_async_ctx);
    }
    if (dyn_http_async_pending == 0 && dyn_http_async_hooked) {
        dyn_http_async_hooked = 0;
        dyn_net_off_drain(&dyn_http_async_hooked);
        dyn_http_async_ctx = NULL;
    }
}

void dyn_http_async_hook(JSContext* ctx)
{
    if (!dyn_http_async_hooked
        && dyn_net_on_drain(dyn_http_async_reap, &dyn_http_async_hooked) >= 0) {
        dyn_http_async_hooked = 1;
        dyn_http_async_ctx = ctx;
    }
}

void dyn_http_async_release(JSContext* ctx)
{
    if (dyn_http_async_hooked)
        dyn_http_async_pending++;
    else
        dyn_net_reactor_release(ctx);
}

static void dyn_aconn_free(dyn_aconn_t* c)
{
    free(c->in.data);
    free(c->out.data);
    free(c);
}

static void dyn_aconn_link(dyn_http_async_t* s, dyn_aconn_t* c)
{
    c->lnext = s->live;
    if (s->live)
        s->live->lprev = c;
    s->live = c;
    s->nconns++;
}

static void dyn_aconn_unlink(dyn_http_async_t* s, dyn_aconn_t* c)
{
    if (c->lprev)
        c->lprev->lnext = c->lnext;
    else if (s->live == c)
        s->live = c->lnext;
    else
        return;
    if (c->lnext)
        c->lnext->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
    if (s->nconns > 0)
        s->nconns--;
}

static void dyn_aconn_close(dyn_evloop_t* lp, dyn_aconn_t* c)
{
    dyn_aconn_unlink(c->srv, c);
    dyn_evloop_del(lp, c->fd);
    close(c->fd);
    dyn_aconn_free(c);
}

static void dyn_aconn_sweep(dyn_evloop_t* lp, dyn_http_async_t* s, uint64_t now)
{
    dyn_aconn_t *c = s->live, *next;
    while (c) {
        next = c->lnext;
        if (now - c->last_ms >= s->idle_ms)
            dyn_aconn_close(lp, c);
        c = next;
    }
}

static int dyn_aconn_process(dyn_aconn_t* c)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
        &c->hdr_scan_from,
        c->srv->routes, c->srv->n_routes, c->srv->route_idx,
        c->srv->max_req);
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

static int dyn_aconn_flush(dyn_evloop_t* lp, dyn_aconn_t* c)
{
    for (;;) {
        while (c->out_off < c->out.len) {
            ssize_t w = send(c->fd, c->out.data + c->out_off,
                c->out.len - c->out_off, 0);
            if (w > 0) {
                c->out_off += (size_t)w;
                c->last_ms = dyn_timer_now_ms();
                continue;
            }
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                dyn_evloop_mod(lp, c->fd, DYN_EV_WRITE);
                return 0;
            }
            dyn_aconn_close(lp, c);
            return 1;
        }
        c->out.len = 0;
        c->out_off = 0;
        if (c->closing) {
            dyn_aconn_close(lp, c);
            return 1;
        }
        if (c->in.len > 0) {
            size_t before = c->in.len;
            if (dyn_aconn_process(c)) {
                c->closing = 1;
                continue;
            }
            if (c->in.len == before && c->out.len == 0)
                break;
            continue;
        }
        break;
    }
    dyn_evloop_mod(lp, c->fd, DYN_EV_READ);
    return 0;
}

static void dyn_aconn_cb(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_aconn_t* c = (dyn_aconn_t*)udata;
    (void)fd;

    if (events & DYN_EV_WRITE) {
        if (dyn_aconn_flush(lp, c))
            return;
    }
    if (events & DYN_EV_READ) {
        for (;;) {
            ssize_t r;
            size_t want;
            if (dyn_bytes_reserve(&c->in, 16384) < 0) {
                dyn_aconn_close(lp, c);
                return;
            }
            want = c->in.cap - c->in.len;
            r = recv(fd, c->in.data + c->in.len, want, 0);
            if (r > 0) {
                c->in.len += (size_t)r;
                if (c->in.len > c->srv->max_req + 16384) {
                    dyn_aconn_close(lp, c);
                    return;
                }
                if ((size_t)r < want)
                    break;
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            c->closing = 1;
            break;
        }
        if (dyn_aconn_process(c))
            c->closing = 1;
        if (c->out.len > c->out_off) {
            if (dyn_aconn_flush(lp, c))
                return;
        } else if (c->closing) {
            dyn_aconn_close(lp, c);
            return;
        }
    }
    if (events & DYN_EV_ERROR) {
        if (c->out_off >= c->out.len)
            dyn_aconn_close(lp, c);
    }
}

static void dyn_alisten_cb(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_http_async_t* s = (dyn_http_async_t*)udata;
    (void)events;
    for (;;) {
        dyn_aconn_t* c;
        int cfd = dyn_net_accept(fd, 0);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || ++s->accept_errs >= 32) {
                dyn_evloop_del(lp, fd);
                s->accept_paused = 1;
                s->accept_resume_ms = dyn_timer_now_ms() + 50;
                s->accept_errs = 0;
            }
            break;
        }
        if (s->max_conns && s->nconns >= s->max_conns) {
            atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
            close(cfd);
            continue;
        }
        s->accept_errs = 0;
        if (dyn_net_set_nonblock(cfd) < 0) {
            close(cfd);
            continue;
        }
        dyn_net_set_nodelay(cfd);
        c = (dyn_aconn_t*)calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        c->srv = s;
        c->fd = cfd;
        c->last_ms = dyn_timer_now_ms();
        if (dyn_evloop_add(lp, cfd, DYN_EV_READ, dyn_aconn_cb, c) < 0) {
            close(cfd);
            dyn_aconn_free(c);
            continue;
        }
        dyn_aconn_link(s, c);
    }
}

#ifdef DYN_HTTP_HAVE_URING
#define DYN_URING_ENTRIES 8192
#define DYN_URING_NBUFS 4096
#define DYN_URING_BUFSZ 2048
#define DYN_URING_BGID 1
#define DYN_URING_ACCEPT_UD 0ULL

enum { DYN_OP_ACCEPT = 0,
    DYN_OP_RECV = 1,
    DYN_OP_SEND = 2,
    DYN_OP_CLOSE = 3 };
#define DYN_UD_TAG(ud) ((int)((ud) & 7))
#define DYN_UD_CONN(ud) ((dyn_uconn_t*)(uintptr_t)((ud) & ~(uint64_t)7))

typedef struct dyn_uconn {
    int fd;
    dyn_bytes_t in;
    dyn_bytes_t out;
    size_t out_off;
    size_t hdr_scan_from;
    int nreq;
    unsigned recv_armed : 1;
    unsigned send_inflight : 1;
    unsigned closing : 1;
    uint64_t last_ms;
    struct dyn_uconn *next, *prev;
} dyn_uconn_t;

typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring* br;
    unsigned char* buf_base;
    int nbufs, bufsz;
    int accept_errs;
    int accept_needs_arm;
    uint64_t accept_resume_ms;
    int nconns;
    dyn_uconn_t* live;
} dyn_uring_ctx;

static struct io_uring_sqe* dyn_ur_sqe(dyn_uring_ctx* u)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&u->ring);
    if (!sqe) {
        io_uring_submit(&u->ring);
        sqe = io_uring_get_sqe(&u->ring);
    }
    if (!sqe) {
        io_uring_submit_and_wait(&u->ring, 1);
        sqe = io_uring_get_sqe(&u->ring);
    }
    return sqe;
}

static int dyn_ur_arm_accept(dyn_uring_ctx* u, int listen_fd)
{
    struct io_uring_sqe* sqe = dyn_ur_sqe(u);
    if (!sqe)
        return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, DYN_URING_ACCEPT_UD);
    return 0;
}

static void dyn_ur_arm_recv(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    struct io_uring_sqe* sqe;
    if (c->fd < 0) {
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        c->closing = 1;
        return;
    }
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = DYN_URING_BGID;
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_RECV);
    c->recv_armed = 1;
}

static void dyn_ur_submit_send(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    struct io_uring_sqe* sqe;
    if (c->fd < 0) {
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        c->closing = 1;
        return;
    }
    io_uring_prep_send(sqe, c->fd, c->out.data + c->out_off,
        c->out.len - c->out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_SEND);
    c->send_inflight = 1;
}

static void dyn_ur_recycle(dyn_uring_ctx* u, int bid)
{
    io_uring_buf_ring_add(u->br, u->buf_base + (size_t)bid * u->bufsz, u->bufsz,
        bid, io_uring_buf_ring_mask(u->nbufs), 0);
    io_uring_buf_ring_advance(u->br, 1);
}

static dyn_uconn_t* dyn_uconn_new(dyn_uring_ctx* u, int fd)
{
    dyn_uconn_t* c = (dyn_uconn_t*)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->last_ms = dyn_timer_now_ms();
    c->next = u->live;
    if (u->live)
        u->live->prev = c;
    u->live = c;
    u->nconns++;
    return c;
}

static void dyn_uconn_destroy(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        u->live = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (u->nconns > 0)
        u->nconns--;
    if (c->fd >= 0)
        close(c->fd);
    free(c->in.data);
    free(c->out.data);
    free(c);
}

static void dyn_uconn_close_step(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    if (!c->closing || c->send_inflight)
        return;
    if (c->recv_armed) {
        if (c->fd >= 0) {
            struct io_uring_sqe* sqe = dyn_ur_sqe(u);
            if (sqe) {
                io_uring_prep_cancel_fd(sqe, c->fd, IORING_ASYNC_CANCEL_ALL);
                io_uring_sqe_set_data64(sqe, (uint64_t)DYN_OP_CLOSE);
            }
            io_uring_submit(&u->ring);
            close(c->fd);
            c->fd = -1;
        }
        return;
    }
    dyn_uconn_destroy(u, c);
}

static void dyn_ur_on_accept(dyn_uring_ctx* u, dyn_http_async_t* s,
    struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE) && res >= 0) {
        if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
            u->accept_needs_arm = 1;
            return;
        }
    }
    if (res < 0) {
        if (++u->accept_errs >= 32) {
            u->accept_resume_ms = dyn_timer_now_ms() + 50;
            u->accept_needs_arm = 1;
            u->accept_errs = 0;
        }
        return;
    }
    u->accept_errs = 0;
    if (s->max_conns && u->nconns >= s->max_conns) {
        atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
        close(res);
        return;
    }
    {
        dyn_uconn_t* c = dyn_uconn_new(u, res);
        if (!c) {
            close(res);
            return;
        }
        dyn_net_set_nodelay(res);
        dyn_ur_arm_recv(u, c);
    }
}

static int dyn_ur_pump(dyn_uconn_t* c, dyn_http_async_t* s)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
        &c->hdr_scan_from,
        s->routes, s->n_routes, s->route_idx, s->max_req);
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

static void dyn_ur_on_recv(dyn_uring_ctx* u, dyn_http_async_t* s,
    dyn_uconn_t* c, struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    int bid = (cqe->flags & IORING_CQE_F_BUFFER)
        ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
        : -1;

    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->recv_armed = 0;

    if (res > 0) {
        int oom = 0;
        if (bid >= 0) {
            if (dyn_bytes_append(&c->in,
                    (char*)(u->buf_base + (size_t)bid * u->bufsz),
                    (size_t)res)
                < 0)
                oom = 1;
            dyn_ur_recycle(u, bid);
        }
        if (oom || c->in.len > DYN_ACONN_MAX_REQ + 16384) {
            c->closing = 1;
        } else if (!c->send_inflight) {
            if (dyn_ur_pump(c, s))
                c->closing = 1;
            if (c->out.len > c->out_off)
                dyn_ur_submit_send(u, c);
        }
        if (!c->closing && !c->recv_armed)
            dyn_ur_arm_recv(u, c);
    } else {
        if (bid >= 0)
            dyn_ur_recycle(u, bid);
        if (res == -ENOBUFS) {
            if (!c->closing && !c->recv_armed)
                dyn_ur_arm_recv(u, c);
        } else {
            if (!c->send_inflight) {
                if (dyn_ur_pump(c, s))
                    c->closing = 1;
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
            }
            c->closing = 1;
        }
    }
    dyn_uconn_close_step(u, c);
}

static void dyn_ur_on_send(dyn_uring_ctx* u, dyn_http_async_t* s,
    dyn_uconn_t* c, struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    c->send_inflight = 0;
    if (res > 0) {
        c->out_off += (size_t)res;
        c->last_ms = dyn_timer_now_ms();
        if (c->out_off < c->out.len) {
            dyn_ur_submit_send(u, c);
        } else {
            c->out.len = 0;
            c->out_off = 0;
            if (!c->closing) {
                if (dyn_ur_pump(c, s))
                    c->closing = 1;
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
                else if (!c->recv_armed && !c->closing)
                    dyn_ur_arm_recv(u, c);
            }
        }
    } else {
        c->closing = 1;
    }
    dyn_uconn_close_step(u, c);
}

static void dyn_ur_sweep(dyn_uring_ctx* u, dyn_http_async_t* s, uint64_t now)
{
    dyn_uconn_t *c = u->live, *next;
    while (c) {
        next = c->next;
        if (now - c->last_ms >= s->idle_ms) {
            c->closing = 1;
            if (c->send_inflight && c->fd >= 0)
                shutdown(c->fd, SHUT_RDWR);
            dyn_uconn_close_step(u, c);
        }
        c = next;
    }
}

static int dyn_http_uring_try_run(dyn_http_async_t* s)
{
    dyn_uring_ctx* u = (dyn_uring_ctx*)calloc(1, sizeof(*u));
    int ret = 0, i;

    if (!u)
        return 0;
    u->nbufs = DYN_URING_NBUFS;
    u->bufsz = DYN_URING_BUFSZ;

    {
        static const unsigned ring_flag_sets[] = {
            IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN,
            IORING_SETUP_COOP_TASKRUN, 0
        };
        if (dyn_uring_ring_open(&u->ring, DYN_URING_ENTRIES, ring_flag_sets,
                (unsigned)(sizeof(ring_flag_sets) / sizeof(ring_flag_sets[0])))
            < 0) {
            free(u);
            return 0;
        }
    }

    u->buf_base = (unsigned char*)malloc((size_t)u->nbufs * u->bufsz);
    if (!u->buf_base) {
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    u->br = io_uring_setup_buf_ring(&u->ring, u->nbufs, DYN_URING_BGID, 0, &ret);
    if (!u->br) {
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    for (i = 0; i < u->nbufs; i++)
        io_uring_buf_ring_add(u->br, u->buf_base + (size_t)i * u->bufsz, u->bufsz,
            i, io_uring_buf_ring_mask(u->nbufs), i);
    io_uring_buf_ring_advance(u->br, u->nbufs);

    if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
        io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    io_uring_submit(&u->ring);

    s->uring = u;
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
        struct io_uring_cqe* cqe;
        unsigned head, count = 0, accepts = 0;

        {
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_ur_sweep(u, s, now);
            }
            if (u->accept_needs_arm && now >= u->accept_resume_ms
                && dyn_ur_arm_accept(u, s->listen_fd) == 0)
                u->accept_needs_arm = 0;
        }
        ret = io_uring_submit_and_wait_timeout(&u->ring, &cqe, 1, &ts, NULL);
        dyn_wall_tick();
        if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -ETIMEDOUT)
            break;
        io_uring_for_each_cqe(&u->ring, head, cqe)
        {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            count++;
            if (ud == DYN_URING_ACCEPT_UD) {
                accepts++;
            } else if (DYN_UD_TAG(ud) == DYN_OP_CLOSE) {
            } else {
                dyn_uconn_t* c = DYN_UD_CONN(ud);
                if (DYN_UD_TAG(ud) == DYN_OP_RECV)
                    dyn_ur_on_recv(u, s, c, cqe);
                else if (DYN_UD_TAG(ud) == DYN_OP_SEND)
                    dyn_ur_on_send(u, s, c, cqe);
            }
        }
        if (accepts) {
            unsigned seen = 0;
            io_uring_for_each_cqe(&u->ring, head, cqe)
            {
                if (seen++ == count)
                    break;
                if (io_uring_cqe_get_data64(cqe) == DYN_URING_ACCEPT_UD)
                    dyn_ur_on_accept(u, s, cqe);
            }
        }
        io_uring_cq_advance(&u->ring, count);
    }

    while (u->live)
        dyn_uconn_destroy(u, u->live);
    io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
    free(u->buf_base);
    io_uring_queue_exit(&u->ring);
    free(u);
    s->uring = NULL;
    return 1;
}
#endif

static void* dyn_http_async_reactor(void* arg)
{
    dyn_http_async_t* s = (dyn_http_async_t*)arg;
    {
        static _Atomic unsigned dyn_reactor_seq;
        char tn[16];
        snprintf(tn, sizeof tn, "dyn-reactor-%u", atomic_fetch_add(&dyn_reactor_seq, 1) % 1000u);
        dyn_thread_name(tn);
    }

#ifdef DYN_HTTP_HAVE_URING
    if (dyn_http_uring_try_run(s))
        return NULL;
#endif

    s->loop = dyn_evloop_new();
    if (!s->loop || dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ, dyn_alisten_cb, s) < 0) {
        atomic_store_explicit(&s->spawn_ok, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        {
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_aconn_sweep(s->loop, s, now);
            }
            if (s->accept_paused && now >= s->accept_resume_ms) {
                s->accept_paused = 0;
                s->accept_errs = 0;
                dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ,
                    dyn_alisten_cb, s);
            }
        }
        if (dyn_evloop_poll(s->loop, 200) < 0)
            break;
    }
    while (s->live)
        dyn_aconn_close(s->loop, s->live);
    return NULL;
}

void dyn_http_async_stop_internal(dyn_http_async_t* s)
{
    int i;
    for (i = 0; i < s->nextra; i++) {
        dyn_http_async_t* e = s->extra[i];
        dyn_http_async_stop_internal(e);
        if (e->listen_fd != s->listen_fd)
            close(e->listen_fd);
        free(e);
    }
    free(s->extra);
    s->extra = NULL;
    s->nextra = 0;
    if (!s->started)
        return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    pthread_join(s->reactor, NULL);
    if (s->loop) {
        dyn_evloop_free(s->loop);
        s->loop = NULL;
    }
    if (s->reactor_held) {
        dyn_net_reactor_release(s->ctx);
        s->reactor_held = 0;
    }
    s->started = 0;
}

static void dyn_http_async_dispose(void* native)
{
    dyn_http_async_t* s = (dyn_http_async_t*)native;
    size_t i;

    dyn_http_async_stop_internal(s);
    if (!s->borrowed) {
        if (s->listen_fd >= 0)
            close(s->listen_fd);
        if (s->routes) {
            for (i = 0; i < s->n_routes; i++) {
                free(s->routes[i].path);
                free(s->routes[i].content_type);
                free(s->routes[i].body);
            }
            free(s->routes);
        }
        dyn_route_index_free(s->route_idx);
    }
    free(s);
}

JSValue dyn_http_async_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char* host_c = NULL;
    char* host_dup = NULL;
    int32_t port = 0, backlog = 0, max_conns = DYN_ACONN_MAX_CONNS_DEFAULT, workers = 1;
    int64_t idle_ms = DYN_ACONN_IDLE_MS_DEFAULT;
    JSPropertyEnum* tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        if (dyn_opts_strict(ctx, opts, http_async_keys, 7))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            host_c = JS_ToCString(ctx, v);
            if (!host_c) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(host_c);
            JS_FreeCString(ctx, host_c);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &idle_ms, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (idle_ms < 0)
            idle_ms = 0;
        v = JS_GetPropertyStr(ctx, opts, "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &max_conns, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (max_conns < 0)
            max_conns = 0;
        v = JS_GetPropertyStr(ctx, opts, "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &workers, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (workers < 1)
            workers = 1;
        if (workers > 64)
            workers = 64;
        routes_val = JS_GetPropertyStr(ctx, opts, "routes");
        if (JS_IsException(routes_val)) {
            free(host_dup);
            return JS_EXCEPTION;
        }
    }

    if (port < 0 || port > 65535) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowRangeError(ctx, "port must be in [0, 65535]");
    }
    if (backlog <= 0)
        backlog = SOMAXCONN;

    s = (dyn_http_async_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->backlog = backlog;
    s->ctx = ctx;
    atomic_init(&s->stop_flag, 0);
    atomic_init(&s->spawn_ok, 0);
    atomic_init(&s->n_refused, 0);
    s->idle_ms = (uint64_t)idle_ms;
    s->max_conns = max_conns;
    s->max_req = DYN_ACONN_MAX_REQ;
    s->nworkers = workers;
    s->reuseport = dyn_http_reuseport_for(workers);

    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t*)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char* path = JS_AtomToCString(ctx, tab[i].atom);
            JSValue rv;
            if (!path)
                goto fail_enum;
            rv = JS_GetProperty(ctx, routes_val, tab[i].atom);
            if (JS_IsException(rv)) {
                JS_FreeCString(ctx, path);
                goto fail_enum;
            }
            if (dyn_route_copy(ctx, path, rv, &s->routes[s->n_routes]) < 0) {
                JS_FreeCString(ctx, path);
                JS_FreeValue(ctx, rv);
                goto fail_enum;
            }
            s->n_routes++;
            JS_FreeCString(ctx, path);
            JS_FreeValue(ctx, rv);
        }
        JS_FreePropertyEnum(ctx, tab, n_routes);
        tab = NULL;
    }
    JS_FreeValue(ctx, routes_val);
    routes_val = JS_UNDEFINED;
    s->route_idx = dyn_route_index_build(s->routes, s->n_routes);

    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind_ex(host_dup, &bound_port, backlog, s->reuseport);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    if (dyn_net_set_nonblock(listen_fd) < 0) {
        close(listen_fd);
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, new_target, dyn_http_async_class_id, s,
        dyn_http_async_dispose);

oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_ThrowOutOfMemory(ctx);

fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_EXCEPTION;
}

int dyn_http_async_spawn(dyn_http_async_t* s)
{
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    atomic_store_explicit(&s->spawn_ok, 0, memory_order_relaxed);
    if (pthread_create(&s->reactor, NULL, dyn_http_async_reactor, s) != 0)
        return -1;
    for (;;) {
        int ok = atomic_load_explicit(&s->spawn_ok, memory_order_acquire);
        if (ok != 0) {
            if (ok < 0) {
                pthread_join(s->reactor, NULL);
                return -1;
            }
            break;
        }
        sched_yield();
    }
    s->started = 1;
    return 0;
}

JSValue dyn_http_async_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    if (dyn_http_async_spawn(s) < 0)
        return JS_ThrowInternalError(ctx, "failed to start reactor thread");
    if (s->nworkers > 1) {
        s->extra = (dyn_http_async_t**)calloc((size_t)s->nworkers - 1, sizeof(*s->extra));
        if (!s->extra) {
            dyn_http_async_stop_internal(s);
            return JS_ThrowOutOfMemory(ctx);
        }
        while (s->nextra < s->nworkers - 1) {
            dyn_http_async_t* a = (dyn_http_async_t*)calloc(1, sizeof(*a));
            if (!a) {
                dyn_http_async_stop_internal(s);
                return JS_ThrowOutOfMemory(ctx);
            }
            a->ctx = s->ctx;
            a->listen_fd = s->listen_fd;
            if (s->reuseport) {
                int cfd = dyn_http_listen_clone(s->listen_fd, s->backlog);
                if (cfd >= 0)
                    a->listen_fd = cfd;
            }
            a->backlog = s->backlog;
            a->routes = s->routes;
            a->n_routes = s->n_routes;
            a->route_idx = s->route_idx;
            a->borrowed = 1;
            a->max_req = s->max_req;
            a->idle_ms = s->idle_ms;
            a->max_conns = s->max_conns;
            atomic_init(&a->stop_flag, 0);
            atomic_init(&a->spawn_ok, 0);
            atomic_init(&a->n_refused, 0);
            if (dyn_http_async_spawn(a) < 0) {
                if (a->listen_fd != s->listen_fd)
                    close(a->listen_fd);
                free(a);
                dyn_http_async_stop_internal(s);
                return JS_ThrowInternalError(ctx, "failed to start reactor thread");
            }
            s->extra[s->nextra++] = a;
        }
    }
    if (!dyn_net_reactor_acquire(ctx)) {
        dyn_http_async_stop_internal(s);
        return dyn_net_throw_no_reactor(ctx, "HTTPServerAsync");
    }
    s->reactor_held = 1;
    if (dyn_res_pin(ctx, this_val, dyn_http_async_class_id) < 0) {
        dyn_http_async_stop_internal(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

JSValue dyn_http_async_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_async_stop_internal(s);
    dyn_res_unpin(JS_GetRuntime(ctx), this_val, dyn_http_async_class_id);
    return JS_UNDEFINED;
}

JSValue dyn_http_async_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}
#endif
