#include "dyna-aio.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && !(defined(CONFIG_IO_URING) && defined(__linux__))

#include "dyna-evloop.h"
#include "core/dyn-pool.h"

#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include "dyna-tls.h"
#include <stdio.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

#ifdef MSG_NOSIGNAL
#define AIO_SEND_FLAGS MSG_NOSIGNAL
#else
#define AIO_SEND_FLAGS 0
#endif

#define AIO_DEFAULT_BUFSZ 65536u

typedef struct aio_wnode {
    struct aio_wnode* next;
    uint8_t* buf;
    size_t len;
    dyn_aio_cb cb;
    void* udata;
} aio_wnode_t;

typedef struct {
    dyn_aio_cb r_cb;
    dyn_aio_dgram_cb dg_cb;
    void* r_udata;
    uint8_t r_op;
    uint8_t r_multishot;
    dyn_aio_cb w_cb;
    void* w_udata;
    const uint8_t* w_buf;
    size_t w_len, w_off;
    size_t w_full;
    uint8_t w_own;
    struct aio_wnode *w_qhead, *w_qtail;
    uint8_t active;
    int w_file_fd;
    off_t w_file_off, w_file_rem;
    dyn_aio_cb w_file_cb;
    void* w_file_udata;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    dyn_aio_cb tls_hs_cb;
    void* tls_hs_udata;
    uint8_t tls_up;
#endif
} aio_fd_t;

enum { AIO_OP_NONE = 0,
    AIO_OP_ACCEPT,
    AIO_OP_RECV,
    AIO_OP_CONNECT,
    AIO_OP_RECVFROM };

static int aio_send_raw(dyn_aio_t* a, int fd, const void* buf, size_t len,
    int flags, dyn_aio_cb cb, void* udata);

typedef struct {
    dyn_aio_t* aio;
    dyn_aio_cb cb;
    void* udata;
    int fd, flags, mode, datasync;
    void* buf;
    const void* cbuf;
    size_t len;
    off_t off;
    char* path;
    int op;
    int res;
} aio_disk_t;

enum { AIO_DISK_OPEN = 1,
    AIO_DISK_READ,
    AIO_DISK_WRITE,
    AIO_DISK_FSYNC };

struct dyn_aio {
    dyn_evloop_t* lp;
    aio_fd_t* fds;
    int cap;
    short n_listen;
    uint8_t timer_armed;
    size_t inflight;
    dyn_pool_t* pool;
    dyn_pool_chan_t* chan;
    uint8_t* rbuf;
    unsigned rcap;
};

_Static_assert(sizeof(struct dyn_aio) <= 64,
    "dyn_aio crossed a cache line: shrink it or move the field out");

static int fd_ensure(dyn_aio_t* a, int fd)
{
    int nc;
    aio_fd_t* nf;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 64;
    while (nc <= fd)
        nc *= 2;
    nf = (aio_fd_t*)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf)
        return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

static void aio_apply_interest(dyn_aio_t* a, int fd)
{
    aio_fd_t* s = &a->fds[fd];
    int mask = 0;
    if (s->r_op == AIO_OP_CONNECT)
        mask |= DYN_EV_WRITE;
    else if (s->r_op != AIO_OP_NONE)
        mask |= DYN_EV_READ;
    if (s->w_cb || s->w_len > s->w_off || s->w_qhead || s->w_file_rem > 0)
        mask |= DYN_EV_WRITE;
    dyn_evloop_mod(a->lp, fd, mask);
}

static void aio_wq_flush(dyn_aio_t* a, aio_fd_t* s, int res)
{
    aio_wnode_t* n = s->w_qhead;

    s->w_qhead = s->w_qtail = NULL;
    while (n) {
        aio_wnode_t* next = n->next;
        dyn_aio_cb cb = n->cb;
        void* ud = n->udata;
        free(n->buf);
        free(n);
        if (cb)
            cb(a, res, NULL, 0, ud);
        n = next;
    }
}

static void aio_read_done(dyn_aio_t* a, int fd)
{
    aio_fd_t* s = &a->fds[fd];
    s->r_op = AIO_OP_NONE;
    if (a->inflight)
        a->inflight--;
}

static int aio_sendfile_step(int sock, int file, off_t* off, off_t* rem)
{
#if defined(__APPLE__)
    while (*rem > 0) {
        off_t n = *rem;
        int r = sendfile(file, sock, *off, &n, NULL, 0);
        *off += n;
        *rem -= n;
        if (r == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN)
            return 0;
        return -1;
    }
    return 0;
#elif defined(__linux__)
    while (*rem > 0) {
        ssize_t n = sendfile(sock, file, off, (size_t)*rem);
        if (n > 0) {
            *rem -= n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    return 0;
#else
    (void)sock;
    (void)file;
    (void)off;
    (void)rem;
    errno = ENOSYS;
    return -1;
#endif
}

static void aio_file_done(dyn_aio_t* a, int fd, int result)
{
    aio_fd_t* s = &a->fds[fd];
    dyn_aio_cb cb = s->w_file_cb;
    void* ud = s->w_file_udata;
    if (s->w_file_fd > 0)
        close(s->w_file_fd);
    s->w_file_fd = 0;
    s->w_file_rem = 0;
    s->w_file_off = 0;
    s->w_file_cb = NULL;
    s->w_file_udata = NULL;
    if (a->inflight)
        a->inflight--;
    aio_apply_interest(a, fd);
    if (cb)
        cb(a, result, NULL, 0, ud);
}

static void aio_accept_drain(dyn_aio_t* a, int fd, dyn_aio_cb acb, void* aud)
{
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECONNABORTED || errno == EPROTO)
                continue;
            break;
        }
        dyn_net_set_nonblock(c);
        dyn_net_set_nodelay(c);
#ifdef SO_NOSIGPIPE
        {
            int on = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        }
#endif
        if (acb)
            acb(a, c, NULL, 0, aud);
    }
}

#define AIO_LISTENER_SWEEP_MS 250

static void aio_listeners_sweep(dyn_aio_t* a)
{
    int fd;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t* s = &a->fds[fd];
        if (s->r_op != AIO_OP_ACCEPT)
            continue;
        aio_accept_drain(a, fd, s->r_cb, s->r_udata);
    }
}

static void aio_dispatch(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_aio_t* a = (dyn_aio_t*)udata;
    aio_fd_t* s = &a->fds[fd];
    (void)lp;

    if ((events & DYN_EV_WRITE) && (s->w_cb || s->w_len > s->w_off)) {
        for (;;) {
            ssize_t n;
            if (s->w_off >= s->w_len) {
                dyn_aio_cb cb = s->w_cb;
                void* ud = s->w_udata;
                size_t sent = s->w_full ? s->w_full : s->w_len;
                aio_wnode_t* next = s->w_qhead;
                if (s->w_own)
                    free(DYN_UNCONST(s->w_buf));
                s->w_cb = NULL;
                s->w_udata = NULL;
                s->w_buf = NULL;
                s->w_len = s->w_off = 0;
                s->w_own = 0;
                s->w_full = 0;
                if (next) {
                    s->w_qhead = next->next;
                    if (!s->w_qhead)
                        s->w_qtail = NULL;
                    s->w_buf = next->buf;
                    s->w_len = next->len;
                    s->w_off = 0;
                    s->w_full = next->len;
                    s->w_own = 1;
                    s->w_cb = next->cb;
                    s->w_udata = next->udata;
                    free(next);
                } else if (a->inflight) {
                    a->inflight--;
                }
                aio_apply_interest(a, fd);
                if (cb)
                    cb(a, (int)sent, NULL, 0, ud);
                s = &a->fds[fd];
                if (s->w_len > s->w_off)
                    continue;
                break;
            }
            n = send(fd, s->w_buf + s->w_off, s->w_len - s->w_off, AIO_SEND_FLAGS);
            if (n > 0) {
                s->w_off += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            {
                dyn_aio_cb cb = s->w_cb;
                void* ud = s->w_udata;
                int err = -errno;
                if (s->w_own)
                    free(DYN_UNCONST(s->w_buf));
                s->w_cb = NULL;
                s->w_buf = NULL;
                s->w_len = s->w_off = 0;
                s->w_own = 0;
                if (a->inflight)
                    a->inflight--;
                aio_apply_interest(a, fd);
                if (cb)
                    cb(a, err, NULL, 0, ud);
                s = &a->fds[fd];
                aio_wq_flush(a, s, err);
            }
            break;
        }
    }

    s = &a->fds[fd];
    if ((events & (DYN_EV_WRITE | DYN_EV_ERROR)) && s->r_op == AIO_OP_CONNECT) {
        dyn_aio_cb ccb = s->r_cb;
        void* cud = s->r_udata;
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0)
            soerr = errno;
        s->r_op = AIO_OP_NONE;
        s->r_cb = NULL;
        s->r_udata = NULL;
        if (a->inflight)
            a->inflight--;
        aio_apply_interest(a, fd);
        if (ccb)
            ccb(a, soerr ? -soerr : 0, NULL, 0, cud);
        return;
    }
    if ((events & DYN_EV_WRITE) && s->w_file_rem > 0 && s->w_len <= s->w_off) {
        if (aio_sendfile_step(fd, s->w_file_fd, &s->w_file_off, &s->w_file_rem) < 0)
            aio_file_done(a, fd, -errno);
        else if (s->w_file_rem == 0)
            aio_file_done(a, fd, 0);
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_ACCEPT) {
        aio_accept_drain(a, fd, s->r_cb, s->r_udata);
        return;
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_RECVFROM) {
        dyn_aio_dgram_cb dcb = s->dg_cb;
        void* dud = s->r_udata;
        for (;;) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            ssize_t n = recvfrom(fd, a->rbuf, a->rcap, 0,
                (struct sockaddr*)&ss, &sl);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (dcb)
                dcb(a, (int)n, a->rbuf, (unsigned)n,
                    (struct sockaddr*)&ss, (unsigned)sl, dud);
            if (a->fds[fd].r_op != AIO_OP_RECVFROM)
                break;
        }
        return;
    }
    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_RECV) {
        ssize_t n;
        dyn_aio_cb cb = s->r_cb;
        void* ud = s->r_udata;
        do {
            n = recv(fd, a->rbuf, a->rcap, 0);
        } while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (!s->r_multishot) {
            aio_read_done(a, fd);
            aio_apply_interest(a, fd);
        }
#ifdef CONFIG_TLS
        if (s->tls && n > 0) {
            dyn_tls_conn_t* t = s->tls;
            uint8_t pt[16384];
            int got;

            if (dyn_tls_feed(t, a->rbuf, (size_t)n) != 0) {
                aio_read_done(a, fd);
                aio_apply_interest(a, fd);
                cb(a, -EPROTO, NULL, 0, ud);
                return;
            }
            if (!s->tls_up) {
                int st = dyn_tls_handshake(t);
                {
                    uint8_t ob[16384];
                    int on;
                    while ((on = dyn_tls_pull(t, ob, sizeof ob)) > 0)
                        if (aio_send_raw(a, fd, ob, (size_t)on, 0, NULL, NULL) < 0) {
                            st = -1;
                            break;
                        }
                }
                if (st < 0) {
                    aio_read_done(a, fd);
                    aio_apply_interest(a, fd);
                    if (s->tls_hs_cb)
                        s->tls_hs_cb(a, -EPROTO, NULL, 0, s->tls_hs_udata);
                    else
                        cb(a, -EPROTO, NULL, 0, ud);
                    return;
                }
                if (st == 0)
                    return;
                s->tls_up = 1;
                if (s->tls_hs_cb)
                    s->tls_hs_cb(a, 0, NULL, 0, s->tls_hs_udata);
                if (a->fds[fd].r_op != AIO_OP_RECV)
                    return;
            }
            while ((got = dyn_tls_read(t, pt, sizeof pt)) > 0) {
                cb(a, got, pt, (unsigned)got, ud);
                if (a->fds[fd].r_op != AIO_OP_RECV)
                    return;
            }
            if (got < 0) {
                aio_read_done(a, fd);
                aio_apply_interest(a, fd);
                cb(a, -EPROTO, NULL, 0, ud);
            }
            return;
        }
#endif
        if (n >= 0)
            cb(a, (int)n, a->rbuf, (unsigned)n, ud);
        else
            cb(a, -errno, NULL, 0, ud);
    }
}

#ifdef CONFIG_TLS
int dyn_aio_tls_attach(dyn_aio_t* a, int fd, dyn_tls_conn_t* tls,
    dyn_aio_cb hs_cb, void* hs_udata)
{
    aio_fd_t* s;
    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->tls)
        return -1;
    s->tls = tls;
    s->tls_hs_cb = hs_cb;
    s->tls_hs_udata = hs_udata;
    s->tls_up = 0;
    return 0;
}

int dyn_aio_tls_start(dyn_aio_t* a, int fd)
{
    aio_fd_t* s;
    uint8_t ob[16384];
    int on, st;
    if (fd_ensure(a, fd) < 0 || !a->fds[fd].tls)
        return -1;
    s = &a->fds[fd];
    st = dyn_tls_handshake(s->tls);
    while ((on = dyn_tls_pull(s->tls, ob, sizeof ob)) > 0)
        if (aio_send_raw(a, fd, ob, (size_t)on, 0, NULL, NULL) < 0)
            return -1;
    return st < 0 ? -1 : 0;
}
#endif

dyn_aio_t* dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t* a = (dyn_aio_t*)calloc(1, sizeof(*a));
    (void)entries;
    (void)disk_workers;
    if (!a)
        return NULL;
    a->lp = dyn_evloop_new();
    a->rcap = AIO_DEFAULT_BUFSZ;
    a->rbuf = (uint8_t*)malloc(a->rcap);
    if (!a->lp || !a->rbuf) {
        if (a->lp)
            dyn_evloop_free(a->lp);
        free(a->rbuf);
        free(a);
        return NULL;
    }
    return a;
}

void dyn_aio_free(dyn_aio_t* a)
{
    int fd;
    if (!a)
        return;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t* s = &a->fds[fd];
        if (s->w_own && s->w_buf)
            free(DYN_UNCONST(s->w_buf));
    }
    if (a->chan)
        dyn_pool_chan_free(a->chan);
    if (a->pool)
        dyn_pool_free(a->pool);
    dyn_evloop_free(a->lp);
    free(a->rbuf);
    free(a->fds);
    free(a);
}

dyn_evloop_t* dyn_aio_evloop(dyn_aio_t* a)
{
    return a ? a->lp : NULL;
}

int dyn_aio_backend_fd(const dyn_aio_t* a)
{
    return dyn_evloop_backend_fd(a->lp);
}

void dyn_aio_drain(void* aio)
{
    dyn_aio_t* a = (dyn_aio_t*)aio;
    dyn_evloop_poll(a->lp, 0);
    if (a->n_listen > 0 && dyn_evloop_timer_fired(a->lp))
        aio_listeners_sweep(a);
    if (a->chan)
        dyn_pool_drain(a->chan);
}

int dyn_aio_run(dyn_aio_t* a, int timeout_ms)
{
    int n = dyn_evloop_poll(a->lp, timeout_ms);
    if (a->n_listen > 0 && dyn_evloop_timer_fired(a->lp))
        aio_listeners_sweep(a);
    return n;
}

size_t dyn_aio_inflight(const dyn_aio_t* a)
{
    return a->inflight;
}

size_t dyn_aio_queued(const dyn_aio_t* a, int fd)
{
    const aio_fd_t* s;
    const aio_wnode_t* q;
    size_t n = 0;

    if (!a || fd < 0 || fd >= a->cap || !a->fds[fd].active)
        return 0;
    s = &a->fds[fd];
    if (s->w_len > s->w_off)
        n += s->w_len - s->w_off;
    for (q = s->w_qhead; q; q = q->next)
        n += q->len;
    if (s->w_file_rem > 0)
        n += (size_t)s->w_file_rem;
    return n;
}

int dyn_aio_listen(dyn_aio_t* a, const char* host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;

    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
#ifdef IPV6_V6ONLY
        int v6only = 0;
#endif
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
#ifdef IPV6_V6ONLY
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
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
        dyn_net_set_nonblock(fd);
        return fd;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(fd, backlog > 0 ? backlog : 1024) < 0) {
        close(fd);
        return -1;
    }
    dyn_net_set_nonblock(fd);
    return fd;
}

static int unix_addr(struct sockaddr_un* sa, const char* path)
{
    size_t n = strlen(path);
    if (n >= sizeof(sa->sun_path))
        return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, path, n + 1);
    return 0;
}

int dyn_aio_unix_listen(dyn_aio_t* a, const char* path, int backlog)
{
    int fd;
    struct sockaddr_un sa;
    (void)a;

    if (!path || unix_addr(&sa, path) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dyn_net_set_nonblock(fd);
    unlink(path);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(fd, backlog > 0 ? backlog : 128) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_unix_connect(dyn_aio_t* a, const char* path, dyn_aio_cb cb,
    void* udata)
{
    int fd;
    struct sockaddr_un sa;
    aio_fd_t* s;

    if (!a || !path || unix_addr(&sa, path) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dyn_net_set_nonblock(fd);
    if (fd_ensure(a, fd) < 0) {
        close(fd);
        return -1;
    }
    s = &a->fds[fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_CONNECT;
    s->r_multishot = 0;
    a->inflight++;
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS && errno != EINTR)
        goto fail;
    if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
        goto fail;
    s->active = 1;
    return fd;

fail:
    s->r_op = AIO_OP_NONE;
    s->r_cb = NULL;
    s->r_udata = NULL;
    if (a->inflight)
        a->inflight--;
    close(fd);
    return -1;
}

int dyn_aio_udp_bind(dyn_aio_t* a, const char* bind_host, uint16_t port)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    dyn_net_set_nonblock(fd);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (bind_host && *bind_host) ? inet_addr(bind_host)
                                                   : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_recvfrom(dyn_aio_t* a, int fd, dyn_aio_dgram_cb cb, void* udata)
{
    aio_fd_t* s;
    if (!a || !cb || fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->dg_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_RECVFROM;
    s->r_multishot = 1;
    a->inflight++;
    if (dyn_evloop_add(a->lp, fd, DYN_EV_READ, aio_dispatch, a) < 0) {
        s->r_op = AIO_OP_NONE;
        a->inflight--;
        return -1;
    }
    s->active = 1;
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

static int aio_connect_on(dyn_aio_t* a, int fd, const struct sockaddr* sa,
    socklen_t salen, dyn_aio_cb cb, void* udata)
{
    aio_fd_t* s;

    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_CONNECT;
    s->r_multishot = 0;
    a->inflight++;

    if (connect(fd, sa, salen) == 0) {
        if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
            goto fail;
        s->active = 1;
        return fd;
    }
    if (errno != EINPROGRESS && errno != EINTR)
        goto fail;
    if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
        goto fail;
    s->active = 1;
    return fd;

fail:
    s->r_op = AIO_OP_NONE;
    s->r_cb = NULL;
    s->r_udata = NULL;
    if (a->inflight)
        a->inflight--;
    return -1;
}

int dyn_aio_connect_addr(dyn_aio_t* a, const struct sockaddr_storage* sa,
    socklen_t salen, int fam,
    dyn_aio_cb cb, void* udata)
{
    int fd;

    if (!a || !sa)
        return -1;
    fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dyn_net_set_nonblock(fd);
    dyn_net_set_nodelay(fd);
#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    if (aio_connect_on(a, fd, (const struct sockaddr*)sa, salen,
            cb, udata)
        < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
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
} aio_resolve_t;

static void aio_resolve_work(void* p)
{
    aio_resolve_t* c = (aio_resolve_t*)p;

    if (dyn_aio_cand_collect(c->host, c->port, c->cand) != 0)
        c->err = EINVAL;
}

static void aio_cand_done(dyn_aio_t* a, int res, const uint8_t* buf,
    unsigned n, void* ud)
{
    dyn_aio_cand_t* cand = (dyn_aio_cand_t*)ud;
    (void)buf;
    (void)n;

    if (res != 0 && cand->i + 1 < cand->n) {
        cand->i++;
        if (dyn_aio_cand_refresh_fd(cand) == 0 && aio_connect_on(a, cand->fd, (const struct sockaddr*)&cand->addr[cand->i], cand->len[cand->i], aio_cand_done, cand) >= 0)
            return;
    }
    if (cand->cb)
        cand->cb(a, res, NULL, 0, cand->udata);
    free(cand);
}

static void aio_resolve_done(void* p)
{
    aio_resolve_t* c = (aio_resolve_t*)p;
    dyn_aio_t* a = c->aio;
    aio_fd_t* s;

    s = &a->fds[c->fd];
    if (s->r_op != AIO_OP_CONNECT || s->r_cb != c->cb || s->r_udata != c->udata) {
        free(c->cand);
        free(c->host);
        free(c);
        return;
    }

    if (c->err == 0) {
        if (aio_connect_on(a, c->fd,
                (const struct sockaddr*)&c->cand->addr[0],
                c->cand->len[0], aio_cand_done, c->cand)
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
    aio_resolve_t* c;
#ifdef IPV6_V6ONLY
    int v6only = 0;
#endif

    if (!host)
        return -1;

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
#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    c = (aio_resolve_t*)calloc(1, sizeof(*c));
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

    if (fd_ensure(a, fd) < 0) {
        close(fd);
        free(c->cand);
        free(c->host);
        free(c);
        errno = ENOMEM;
        return -1;
    }
    a->fds[fd].r_op = AIO_OP_CONNECT;
    a->fds[fd].r_cb = cb;
    a->fds[fd].r_udata = udata;

    r = dyn_aio_offload(a, aio_resolve_work, aio_resolve_done, c);
    if (r == 1) {
        if (c->err) {
            int e = c->err;
            free(c->cand);
            free(c->host);
            free(c);
            a->fds[fd].r_op = AIO_OP_NONE;
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
        a->fds[fd].r_op = AIO_OP_NONE;
        a->fds[fd].r_cb = NULL;
        a->fds[fd].r_udata = NULL;
        errno = EINVAL;
        return -1;
    }
    c->past_return = 1;
    return fd;
}

int dyn_aio_accept(dyn_aio_t* a, int listen_fd, dyn_aio_cb cb, void* udata)
{
    aio_fd_t* s;
    if (fd_ensure(a, listen_fd) < 0)
        return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_ACCEPT;
    s->r_multishot = 1;
    a->inflight++;
    if (a->n_listen == 0 && !a->timer_armed) {
        if (dyn_evloop_set_timer(a->lp, AIO_LISTENER_SWEEP_MS) == 0)
            a->timer_armed = 1;
    }
    a->n_listen++;
    if (dyn_evloop_add(a->lp, listen_fd, DYN_EV_READ, aio_dispatch, a) < 0) {
        s->r_op = AIO_OP_NONE;
        a->inflight--;
        a->n_listen--;
        return -1;
    }
    return 0;
}

int dyn_aio_recv(dyn_aio_t* a, int fd, int pool, int multishot,
    dyn_aio_cb cb, void* udata)
{
    aio_fd_t* s;
    (void)pool;
    if (!a || !cb) {
        errno = EINVAL;
        return -1;
    }
    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_RECV;
    s->r_multishot = multishot ? 1 : 0;
    a->inflight++;
    if (!s->active) {
        s->active = 1;
        if (dyn_evloop_add(a->lp, fd, DYN_EV_READ, aio_dispatch, a) < 0) {
            s->active = 0;
            s->r_op = AIO_OP_NONE;
            a->inflight--;
            return -1;
        }
    } else {
        aio_apply_interest(a, fd);
    }
    return 0;
}

static int aio_send_raw(dyn_aio_t* a, int fd, const void* buf, size_t len,
    int flags, dyn_aio_cb cb, void* udata)
{
    aio_fd_t* s;
    size_t off = 0;
    (void)flags;

    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->w_len > s->w_off || s->w_qhead) {
        aio_wnode_t* node = (aio_wnode_t*)malloc(sizeof(*node));
        if (!node)
            return -1;
        node->buf = (uint8_t*)malloc(len ? len : 1);
        if (!node->buf) {
            free(node);
            return -1;
        }
        memcpy(node->buf, buf, len);
        node->len = len;
        node->cb = cb;
        node->udata = udata;
        node->next = NULL;
        if (s->w_qtail)
            s->w_qtail->next = node;
        else
            s->w_qhead = node;
        s->w_qtail = node;
        return 0;
    }
    for (;;) {
        ssize_t n = send(fd, (const uint8_t*)buf + off, len - off, AIO_SEND_FLAGS);
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
    {
        uint8_t* copy = (uint8_t*)malloc(len - off);
        if (!copy)
            return -1;
        memcpy(copy, (const uint8_t*)buf + off, len - off);
        s->w_buf = copy;
        s->w_len = len - off;
        s->w_off = 0;
        s->w_own = 1;
        s->w_full = len;
        s->w_cb = cb;
        s->w_udata = udata;
        a->inflight++;
        if (!s->active) {
            s->active = 1;
            if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0) {
                s->active = 0;
                if (s->w_own && s->w_buf)
                    free(DYN_UNCONST(s->w_buf));
                s->w_buf = NULL;
                s->w_len = s->w_off = 0;
                s->w_full = 0;
                s->w_own = 0;
                s->w_cb = NULL;
                s->w_udata = NULL;
                if (a->inflight)
                    a->inflight--;
                return -1;
            }
        } else {
            aio_apply_interest(a, fd);
        }
    }
    return 0;
}

int dyn_aio_close(dyn_aio_t* a, int fd)
{
    aio_fd_t* s;
    if (fd < 0 || fd >= a->cap) {
        close(fd);
        return 0;
    }
    s = &a->fds[fd];
    dyn_evloop_del(a->lp, fd);
    if (s->r_op == AIO_OP_ACCEPT) {
        if (--a->n_listen <= 0 && a->timer_armed) {
            a->n_listen = 0;
            dyn_evloop_set_timer(a->lp, 0);
            a->timer_armed = 0;
        }
    }
#ifdef CONFIG_TLS
    if (s->tls) {
        dyn_tls_conn_free(s->tls);
        s->tls = NULL;
        s->tls_hs_cb = NULL;
        s->tls_hs_udata = NULL;
        s->tls_up = 0;
    }
#endif
    {
        aio_fd_t dead = *s;
        if (s->w_own && s->w_buf)
            free(DYN_UNCONST(s->w_buf));
        if (s->w_file_rem > 0) {
            if (s->w_file_fd > 0)
                close(s->w_file_fd);
            if (a->inflight)
                a->inflight--;
        }
        if (s->r_op != AIO_OP_NONE && a->inflight)
            a->inflight--;
        if ((s->w_cb || s->w_len > s->w_off) && a->inflight)
            a->inflight--;
        if (s->r_op == AIO_OP_CONNECT && s->r_cb == aio_cand_done)
            free(s->r_udata);
        memset(s, 0, sizeof(*s));
        close(fd);
        if (dead.w_cb && dead.w_len > dead.w_off)
            dead.w_cb(a, -ECONNRESET, NULL, 0, dead.w_udata);
        if (dead.w_qhead) {
            aio_fd_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.w_qhead = dead.w_qhead;
            tmp.w_qtail = dead.w_qtail;
            aio_wq_flush(a, &tmp, -ECONNRESET);
        }
    }
    return 0;
}

int dyn_aio_pool_register(dyn_aio_t* a, unsigned n, unsigned sz)
{
    (void)a;
    (void)n;
    (void)sz;
    errno = ENOSYS;
    return -1;
}
int dyn_aio_send(dyn_aio_t* a, int fd, const void* buf, size_t len, int flags,
    dyn_aio_cb cb, void* udata)
{
#ifdef CONFIG_TLS
    if (fd_ensure(a, fd) < 0)
        return -1;
    if (a->fds[fd].tls) {
        dyn_tls_conn_t* t = a->fds[fd].tls;
        uint8_t chunk[16384], *ct = NULL, *nb;
        size_t ctn = 0, ctcap = 0, woff = 0;
        int n, rc;

        while (woff < len) {
            int w = dyn_tls_write(t, (const uint8_t*)buf + woff, len - woff);
            if (w <= 0) {
                free(ct);
                return -1;
            }
            woff += (size_t)w;
        }
        while ((n = dyn_tls_pull(t, chunk, sizeof chunk)) > 0) {
            if (ctn + (size_t)n > ctcap) {
                ctcap = (ctn + (size_t)n) * 2;
                nb = (uint8_t*)realloc(ct, ctcap);
                if (!nb) {
                    free(ct);
                    return -1;
                }
                ct = nb;
            }
            memcpy(ct + ctn, chunk, (size_t)n);
            ctn += (size_t)n;
        }
        if (!ctn) {
            free(ct);
            return 0;
        }
        rc = aio_send_raw(a, fd, ct, ctn, flags, cb, udata);
        free(ct);
        return rc;
    }
#endif
    return aio_send_raw(a, fd, buf, len, flags, cb, udata);
}

int dyn_aio_sendfile(dyn_aio_t* a, int out_fd, int in_fd, off_t offset,
    size_t len, dyn_aio_cb cb, void* udata)
{
    aio_fd_t* s;
    if (fd_ensure(a, out_fd) < 0)
        return -1;
    s = &a->fds[out_fd];
    if (s->w_file_rem > 0) {
        errno = EBUSY;
        return -1;
    }
    s->w_file_fd = in_fd;
    s->w_file_off = offset;
    s->w_file_rem = (off_t)len;
    s->w_file_cb = cb;
    s->w_file_udata = udata;
    a->inflight++;
    if (s->w_len <= s->w_off) {
        if (aio_sendfile_step(out_fd, in_fd, &s->w_file_off, &s->w_file_rem) < 0) {
            aio_file_done(a, out_fd, -errno);
            return 0;
        }
        if (s->w_file_rem == 0) {
            aio_file_done(a, out_fd, 0);
            return 0;
        }
    }
    if (!s->active) {
        s->active = 1;
        if (dyn_evloop_add(a->lp, out_fd, DYN_EV_WRITE, aio_dispatch, a) < 0) {
            s->active = 0;
            s->w_file_fd = 0;
            s->w_file_off = 0;
            s->w_file_rem = 0;
            s->w_file_cb = NULL;
            s->w_file_udata = NULL;
            if (a->inflight)
                a->inflight--;
            return -1;
        }
    } else {
        aio_apply_interest(a, out_fd);
    }
    return 0;
}
static void aio_disk_work(void* arg)
{
    aio_disk_t* j = (aio_disk_t*)arg;
    ssize_t n;
    switch (j->op) {
    case AIO_DISK_OPEN:
        n = openat(j->fd, j->path, j->flags, (mode_t)j->mode);
        break;
    case AIO_DISK_READ:
        n = (j->off < 0) ? read(j->fd, j->buf, j->len)
                         : pread(j->fd, j->buf, j->len, j->off);
        break;
    case AIO_DISK_WRITE:
        n = (j->off < 0) ? write(j->fd, j->cbuf, j->len)
                         : pwrite(j->fd, j->cbuf, j->len, j->off);
        break;
    case AIO_DISK_FSYNC:
#ifdef F_FULLFSYNC
        n = j->datasync ? fsync(j->fd) : fcntl(j->fd, F_FULLFSYNC);
        if (n < 0 && errno == ENOTSUP)
            n = fsync(j->fd);
#else
        n = j->datasync ? fdatasync(j->fd) : fsync(j->fd);
#endif
        break;
    default:
        n = -1;
        errno = EINVAL;
        break;
    }
    j->res = (n < 0) ? -errno : (int)n;
}

static void aio_disk_done(void* arg)
{
    aio_disk_t* j = (aio_disk_t*)arg;
    dyn_aio_t* a = j->aio;
    if (a->inflight)
        a->inflight--;
    if (j->cb)
        j->cb(a, j->res, NULL, 0, j->udata);
    free(j->path);
    free(j);
}

static void aio_disk_wake(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_aio_t* a = (dyn_aio_t*)udata;
    (void)lp;
    (void)fd;
    (void)events;
    if (a->chan)
        dyn_pool_drain(a->chan);
}

static int aio_pool_ready(dyn_aio_t* a)
{
    int wfd;
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
    wfd = dyn_pool_wake_fd(a->chan);
    if (wfd >= 0)
        (void)dyn_evloop_add(a->lp, wfd, DYN_EV_READ, aio_disk_wake, a);
    return 0;
}

static int aio_disk_submit(dyn_aio_t* a, aio_disk_t* j)
{
    if (aio_pool_ready(a) == 0 && dyn_pool_submit(a->chan, aio_disk_work, aio_disk_done, j) == 0) {
        a->inflight++;
        return 0;
    }
    a->inflight++;
    aio_disk_work(j);
    aio_disk_done(j);
    return 0;
}

typedef struct {
    dyn_aio_t* aio;
    void (*work)(void*);
    void (*done)(void*);
    void* arg;
} aio_offload_t;

static void aio_offload_work(void* p)
{
    aio_offload_t* j = (aio_offload_t*)p;
    if (j->work)
        j->work(j->arg);
}

static void aio_offload_done(void* p)
{
    aio_offload_t* j = (aio_offload_t*)p;
    dyn_aio_t* a = j->aio;
    if (a->inflight)
        a->inflight--;
    if (j->done)
        j->done(j->arg);
    free(j);
}

int dyn_aio_offload(dyn_aio_t* a, void (*work)(void*), void (*done)(void*),
    void* arg)
{
    aio_offload_t* j;
    if (!a)
        return -1;
    j = (aio_offload_t*)calloc(1, sizeof(*j));
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
    if (aio_pool_ready(a) == 0 && dyn_pool_submit(a->chan, aio_offload_work, aio_offload_done, j) == 0)
        return 0;
    aio_offload_work(j);
    aio_offload_done(j);
    return 1;
}

static aio_disk_t* aio_disk_new(dyn_aio_t* a, int op, dyn_aio_cb cb, void* ud)
{
    aio_disk_t* j = (aio_disk_t*)calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->aio = a;
    j->op = op;
    j->cb = cb;
    j->udata = ud;
    j->off = -1;
    return j;
}

int dyn_aio_openat(dyn_aio_t* a, int dirfd, const char* path, int flags,
    int mode, dyn_aio_cb cb, void* udata)
{
    aio_disk_t* j;
    if (!a || !path)
        return -1;
    j = aio_disk_new(a, AIO_DISK_OPEN, cb, udata);
    if (!j)
        return -1;
    j->path = strdup(path);
    if (!j->path) {
        free(j);
        return -1;
    }
    j->fd = dirfd;
    j->flags = flags;
    j->mode = mode;
    return aio_disk_submit(a, j);
}

int dyn_aio_read(dyn_aio_t* a, int fd, void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata)
{
    aio_disk_t* j;
    if (!a || !buf)
        return -1;
    j = aio_disk_new(a, AIO_DISK_READ, cb, udata);
    if (!j)
        return -1;
    j->fd = fd;
    j->buf = buf;
    j->len = len;
    j->off = off;
    return aio_disk_submit(a, j);
}

int dyn_aio_write(dyn_aio_t* a, int fd, const void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata)
{
    aio_disk_t* j;
    if (!a || !buf)
        return -1;
    j = aio_disk_new(a, AIO_DISK_WRITE, cb, udata);
    if (!j)
        return -1;
    j->fd = fd;
    j->cbuf = buf;
    j->len = len;
    j->off = off;
    return aio_disk_submit(a, j);
}

int dyn_aio_fsync(dyn_aio_t* a, int fd, int datasync, dyn_aio_cb cb, void* ud)
{
    aio_disk_t* j;
    if (!a)
        return -1;
    j = aio_disk_new(a, AIO_DISK_FSYNC, cb, ud);
    if (!j)
        return -1;
    j->fd = fd;
    j->datasync = datasync;
    return aio_disk_submit(a, j);
}

void dyn_aio_disk_drain(dyn_aio_t* a)
{
    if (a && a->chan)
        dyn_pool_drain(a->chan);
}

int dyn_aio_set_timer(dyn_aio_t* a, unsigned period_ms)
{
    return a ? dyn_evloop_set_timer(a->lp, period_ms) : -1;
}

int dyn_aio_disk_fd(const dyn_aio_t* a)
{
    return (a && a->chan) ? dyn_pool_wake_fd(a->chan) : -1;
}
int dyn_aio_cancel(dyn_aio_t* a, dyn_aio_cb cb, void* udata)
{
    int fd, n = 0;
    if (!a)
        return -1;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t* s = &a->fds[fd];
        int touched = 0;
        if (s->r_cb == cb && s->r_udata == udata && s->r_op != AIO_OP_NONE) {
            s->r_cb = NULL;
            s->r_udata = NULL;
            s->r_op = AIO_OP_NONE;
            s->r_multishot = 0;
            if (a->inflight)
                a->inflight--;
            n++;
            touched = 1;
        }
        if (s->w_cb == cb && s->w_udata == udata && s->w_len > s->w_off) {
            if (s->w_own && s->w_buf)
                free(DYN_UNCONST(s->w_buf));
            s->w_buf = NULL;
            s->w_len = s->w_off = 0;
            s->w_own = 0;
            s->w_cb = NULL;
            s->w_udata = NULL;
            if (a->inflight)
                a->inflight--;
            n++;
            touched = 1;
        }
        while (s->w_qhead && s->w_qhead->cb == cb && s->w_qhead->udata == udata) {
            aio_wnode_t* q = s->w_qhead;
            s->w_qhead = q->next;
            if (!s->w_qhead)
                s->w_qtail = NULL;
            free(q->buf);
            free(q);
            n++;
            touched = 1;
        }
        if (s->w_file_cb == cb && s->w_file_udata == udata && s->w_file_rem > 0) {
            if (s->w_file_fd >= 0)
                close(s->w_file_fd);
            s->w_file_fd = -1;
            s->w_file_rem = 0;
            s->w_file_cb = NULL;
            s->w_file_udata = NULL;
            if (a->inflight)
                a->inflight--;
            n++;
            touched = 1;
        }
        if (touched)
            aio_apply_interest(a, fd);
    }
    return n;
}

#endif
