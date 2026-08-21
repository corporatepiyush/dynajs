/*
 * dyna-aio -- the async IO adapter (see dyna-aio.h).
 *
 * This translation unit currently implements the READINESS backend (kqueue on
 * macOS/BSD, epoll on Linux) via dyn_evloop, presenting the completion-model
 * surface: the adapter hides "wait for readiness, then do the syscall" behind a
 * completion callback, reading/writing directly into the caller's buffer -- no
 * extra copy versus the raw OS call. The io_uring backend (Linux
 * true-completion) sits behind the same interface.
 *
 * Disk is serviced by the shared dyn-pool here, because no readiness mechanism
 * can make a regular file pollable; the io_uring backend sends the same calls
 * straight to the kernel instead. Only dyn_aio_pool_register() is still
 * unimplemented on this backend -- it is an io_uring buffer-ring concept.
 */
#include "dyna-aio.h"

/* The io_uring backend (dyna-aio-uring.c) replaces this readiness backend when
 * CONFIG_IO_URING is set on Linux; compile this file out then to avoid a clash. */
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
#include <netdb.h>      /* getaddrinfo: connect takes a name or an address */
#include "dyna-tls.h"   /* no-op without CONFIG_TLS */
#include <stdio.h>      /* snprintf, for the service string */
#include <sys/types.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

#ifdef MSG_NOSIGNAL
#define AIO_SEND_FLAGS MSG_NOSIGNAL
#else
#define AIO_SEND_FLAGS 0 /* macOS: SO_NOSIGPIPE is set on the fd instead */
#endif

#define AIO_DEFAULT_BUFSZ 65536u

/* One send queued behind the fd's in-flight write. Owns its copy. */
typedef struct aio_wnode {
    struct aio_wnode *next;
    uint8_t *buf;
    size_t len;
    dyn_aio_cb cb;
    void *udata;
} aio_wnode_t;

/* Per-fd op state. A connection can have a read side (accept or recv) and a
 * write side (send) outstanding at once, so both are tracked. */
typedef struct {
    /* read side */
    dyn_aio_cb r_cb;
    dyn_aio_dgram_cb dg_cb;   /* AIO_OP_RECVFROM: carries the peer address */
    void *r_udata;
    uint8_t r_op;        /* AIO_OP_* */
    uint8_t r_multishot; /* keep the read armed after each completion */
    /* write side: a buffered send, optionally followed by a file (sendfile) */
    dyn_aio_cb w_cb;
    void *w_udata;
    const uint8_t *w_buf;
    size_t w_len, w_off;
    /* What the CALLER asked for. The buffered remainder is len-off, so
     * reporting w_len would tell a caller doing byte accounting that fewer
     * bytes went out than it handed over -- the inline prefix would vanish. */
    size_t w_full;
    uint8_t w_own; /* free(w_buf) when the send completes */
    /* Sends issued while the slot above is still draining. Without this a
     * second send overwrote the first -- losing its bytes, leaking its buffer
     * and never running its callback -- and any inline send jumped ahead of
     * queued bytes, reordering the stream. Only a streaming caller hits it. */
    struct aio_wnode *w_qhead, *w_qtail;
    uint8_t active; /* slot in use */
    /* zero-copy file body (sendfile), sent after any buffered prefix drains */
    int w_file_fd;      /* -1/0 when none; valid iff w_file_rem > 0 */
    off_t w_file_off, w_file_rem;
    dyn_aio_cb w_file_cb;
    void *w_file_udata;
#ifdef CONFIG_TLS
    /* NULL = plaintext. Every TLS branch below is guarded on this, so the
       plaintext path is byte-for-byte what it was. */
    dyn_tls_conn_t *tls;
    dyn_aio_cb tls_hs_cb;     /* fires once, when the handshake settles */
    void *tls_hs_udata;
    uint8_t tls_up;
#endif
} aio_fd_t;

enum { AIO_OP_NONE = 0, AIO_OP_ACCEPT, AIO_OP_RECV, AIO_OP_CONNECT,
       AIO_OP_RECVFROM };

/* Defined below; the TLS paths in the dispatcher need it. */
static int aio_send_raw(dyn_aio_t *a, int fd, const void *buf, size_t len,
                        int flags, dyn_aio_cb cb, void *udata);

/* One disk job, allocated per operation. The pool never allocates, but a disk
 * op outlives its caller's frame and carries a result back, so it needs a home;
 * it is freed by the completion, on the loop thread. */
typedef struct {
    dyn_aio_t *aio;
    dyn_aio_cb cb;
    void *udata;
    int fd, flags, mode, datasync;
    void *buf;
    const void *cbuf;
    size_t len;
    off_t off;
    char *path;
    int op;              /* AIO_DISK_* */
    int res;             /* >=0 result, or -errno */
} aio_disk_t;

enum { AIO_DISK_OPEN = 1, AIO_DISK_READ, AIO_DISK_WRITE, AIO_DISK_FSYNC };

struct dyn_aio {
    dyn_evloop_t *lp;
    aio_fd_t *fds;
    int cap;
    size_t inflight; /* armed read/write sides */
    /* Disk is serviced by the shared pool: kqueue/epoll/poll cannot make a
     * regular file readiness-driven, so there is no alternative on this
     * backend. Created lazily on the first disk op -- most programs never do
     * one, and threads that are never used are pure cost. */
    dyn_pool_t *pool;
    dyn_pool_chan_t *chan;
    /* ONE shared recv buffer: the readiness backend recvs synchronously inside
     * a callback and dispatches fds one at a time, so a single buffer is reused
     * across every connection (borrowed to the callback, valid until it returns)
     * -- no per-connection recv allocation. */
    uint8_t *rbuf;
    unsigned rcap;
};

/* ONE cache line exactly. A field added carelessly makes it two, and nothing
   would report that -- which is what a silent invariant means. */
_Static_assert(sizeof(struct dyn_aio) <= 64,
               "dyn_aio crossed a cache line: shrink it or move the field out");

static int fd_ensure(dyn_aio_t *a, int fd)
{
    int nc;
    aio_fd_t *nf;
    if (fd < a->cap)
        return 0;
    nc = a->cap ? a->cap * 2 : 64;
    while (nc <= fd)
        nc *= 2;
    nf = (aio_fd_t *)realloc(a->fds, (size_t)nc * sizeof(*nf));
    if (!nf)
        return -1;
    memset(nf + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*nf));
    a->fds = nf;
    a->cap = nc;
    return 0;
}

/* Recompute the reactor interest mask for `fd` from its armed sides. */
static void aio_apply_interest(dyn_aio_t *a, int fd)
{
    aio_fd_t *s = &a->fds[fd];
    int mask = 0;
    /* A pending connect is the one read-side op that waits on WRITABILITY. */
    if (s->r_op == AIO_OP_CONNECT)
        mask |= DYN_EV_WRITE;
    else if (s->r_op != AIO_OP_NONE)
        mask |= DYN_EV_READ;
    if (s->w_cb || s->w_len > s->w_off || s->w_qhead || s->w_file_rem > 0)
        mask |= DYN_EV_WRITE;
    dyn_evloop_mod(a->lp, fd, mask);
}

/* Complete every queued send with `res` and drop it. A caller that takes a
 * reference per send releases it here exactly once, so a torn-down fd cannot
 * strand one. Runs the callbacks AFTER unlinking: one may queue another send. */
static void aio_wq_flush(dyn_aio_t *a, aio_fd_t *s, int res)
{
    aio_wnode_t *n = s->w_qhead;

    s->w_qhead = s->w_qtail = NULL;
    while (n) {
        aio_wnode_t *next = n->next;
        dyn_aio_cb cb = n->cb;
        void *ud = n->udata;
        free(n->buf);
        free(n);
        if (cb)
            cb(a, res, NULL, 0, ud);
        n = next;
    }
}

static void aio_read_done(dyn_aio_t *a, int fd)
{
    aio_fd_t *s = &a->fds[fd];
    s->r_op = AIO_OP_NONE;
    if (a->inflight)
        a->inflight--;
}

/* Send as much of the file as the socket accepts; advance the offset/remaining.
 * Returns 0 (progress, possibly EAGAIN with bytes left) or -1 (hard error). */
static int aio_sendfile_step(int sock, int file, off_t *off, off_t *rem)
{
#if defined(__APPLE__)
    while (*rem > 0) {
        off_t n = *rem;
        int r = sendfile(file, sock, *off, &n, NULL, 0);
        *off += n; *rem -= n;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return 0;
        return -1;
    }
    return 0;
#elif defined(__linux__)
    while (*rem > 0) {
        ssize_t n = sendfile(sock, file, off, (size_t)*rem); /* updates *off */
        if (n > 0) { *rem -= n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    return 0;
#else
    (void)sock; (void)file; (void)off; (void)rem; errno = ENOSYS; return -1;
#endif
}

/* Finish a file send on `fd`: close the file, clear state, fire the cb. */
static void aio_file_done(dyn_aio_t *a, int fd, int result)
{
    aio_fd_t *s = &a->fds[fd];
    dyn_aio_cb cb = s->w_file_cb;
    void *ud = s->w_file_udata;
    if (s->w_file_fd > 0) close(s->w_file_fd);
    s->w_file_fd = 0; s->w_file_rem = 0; s->w_file_off = 0;
    s->w_file_cb = NULL; s->w_file_udata = NULL;
    if (a->inflight) a->inflight--;
    aio_apply_interest(a, fd);
    if (cb) cb(a, result, NULL, 0, ud);
}

/* The single dyn_evloop callback: adapt readiness into completions. */
static void aio_dispatch(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_aio_t *a = (dyn_aio_t *)udata;
    aio_fd_t *s = &a->fds[fd];
    (void)lp;

    if ((events & DYN_EV_WRITE) && (s->w_cb || s->w_len > s->w_off)) {
        for (;;) {
            ssize_t n;
            if (s->w_off >= s->w_len) { /* fully sent */
                dyn_aio_cb cb = s->w_cb;
                void *ud = s->w_udata;
                size_t sent = s->w_full ? s->w_full : s->w_len;
                aio_wnode_t *next = s->w_qhead;
                if (s->w_own)
                    free((void *)s->w_buf);
                s->w_cb = NULL; s->w_udata = NULL; s->w_buf = NULL;
                s->w_len = s->w_off = 0; s->w_own = 0; s->w_full = 0;
                /* Promote the next queued send into the slot BEFORE running
                   the callback: the callback may queue another. */
                if (next) {
                    s->w_qhead = next->next;
                    if (!s->w_qhead) s->w_qtail = NULL;
                    s->w_buf = next->buf; s->w_len = next->len; s->w_off = 0;
                    s->w_full = next->len;
                    s->w_own = 1; s->w_cb = next->cb; s->w_udata = next->udata;
                    free(next);
                } else if (a->inflight) {
                    a->inflight--;
                }
                aio_apply_interest(a, fd);
                if (cb) cb(a, (int)sent, NULL, 0, ud);
                if (s->w_len > s->w_off)
                    continue;           /* keep draining the promoted send */
                break;
            }
            n = send(fd, s->w_buf + s->w_off, s->w_len - s->w_off, AIO_SEND_FLAGS);
            if (n > 0) { s->w_off += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            { /* hard error: the queue behind it can never go out either */
                dyn_aio_cb cb = s->w_cb; void *ud = s->w_udata;
                int err = -errno;
                if (s->w_own) free((void *)s->w_buf);
                s->w_cb = NULL; s->w_buf = NULL; s->w_len = s->w_off = 0; s->w_own = 0;
                if (a->inflight) a->inflight--;
                aio_apply_interest(a, fd);
                if (cb) cb(a, err, NULL, 0, ud);
                aio_wq_flush(a, s, err);
            }
            break;
        }
    }

    /* zero-copy file body: stream once the buffered prefix has drained. Re-fetch
     * `s` in case a completion callback above grew a->fds. */
    s = &a->fds[fd];
    /* A connect resolves on WRITABILITY -- but writable does NOT mean connected:
     * a refused connection is reported writable too. SO_ERROR is the only thing
     * that distinguishes them, and skipping it is the classic bug here. */
    if ((events & (DYN_EV_WRITE | DYN_EV_ERROR)) && s->r_op == AIO_OP_CONNECT) {
        dyn_aio_cb ccb = s->r_cb;
        void *cud = s->r_udata;
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
        return;                 /* `s` may dangle: the callback can realloc fds */
    }
    if ((events & DYN_EV_WRITE) && s->w_file_rem > 0 && s->w_len <= s->w_off) {
        if (aio_sendfile_step(fd, s->w_file_fd, &s->w_file_off, &s->w_file_rem) < 0)
            aio_file_done(a, fd, -errno);
        else if (s->w_file_rem == 0)
            aio_file_done(a, fd, 0);
        /* else partial: still armed for DYN_EV_WRITE */
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_ACCEPT) {
        /* Capture cb/udata into locals: the callback (dyn_aio_recv on the new fd)
         * can grow a->fds via realloc, dangling `s` -- never deref it in the loop. */
        dyn_aio_cb acb = s->r_cb;
        void *aud = s->r_udata;
        for (;;) {
            int c = accept(fd, NULL, NULL);
            if (c < 0) {
                if (errno == EINTR) continue;
                break; /* EAGAIN: drained the backlog */
            }
            dyn_net_set_nonblock(c);
            dyn_net_set_nodelay(c);
#ifdef SO_NOSIGPIPE
            { int on = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
            if (acb) acb(a, c, NULL, 0, aud);
        }
        return;
    }

    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_RECVFROM) {
        dyn_aio_dgram_cb dcb = s->dg_cb;
        void *dud = s->r_udata;
        for (;;) {
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            ssize_t n = recvfrom(fd, a->rbuf, a->rcap, 0,
                                 (struct sockaddr *)&ss, &sl);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;           /* EAGAIN: drained */
            }
            /* A zero-length datagram is legal and must be delivered; only a
             * negative return means "nothing there". */
            if (dcb)
                dcb(a, (int)n, a->rbuf, (unsigned)n,
                    (struct sockaddr *)&ss, (unsigned)sl, dud);
            if (a->fds[fd].r_op != AIO_OP_RECVFROM)
                break;           /* the callback disarmed us */
        }
        return;
    }
    if ((events & (DYN_EV_READ | DYN_EV_ERROR)) && s->r_op == AIO_OP_RECV) {
        ssize_t n;
        dyn_aio_cb cb = s->r_cb;
        void *ud = s->r_udata;
        do { n = recv(fd, a->rbuf, a->rcap, 0); } while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; /* spurious: stay armed */
        if (!s->r_multishot) {
            aio_read_done(a, fd);
            aio_apply_interest(a, fd);
        }
#ifdef CONFIG_TLS
        if (s->tls && n > 0) {
            dyn_tls_conn_t *t = s->tls;
            uint8_t pt[16384];
            int got;

            if (dyn_tls_feed(t, a->rbuf, (size_t)n) != 0) {
                cb(a, -EPROTO, NULL, 0, ud);
                return;
            }
            if (!s->tls_up) {
                int st = dyn_tls_handshake(t);
                /* Flush BEFORE judging: the engine's reply must reach the wire
                   even on the last round of the handshake. */
                {
                    uint8_t ob[16384];
                    int on;
                    while ((on = dyn_tls_pull(t, ob, sizeof ob)) > 0)
                        if (aio_send_raw(a, fd, ob, (size_t)on, 0, NULL, NULL) < 0)
                            { st = -1; break; }
                }
                if (st < 0) {
                    if (s->tls_hs_cb) s->tls_hs_cb(a, -EPROTO, NULL, 0, s->tls_hs_udata);
                    else cb(a, -EPROTO, NULL, 0, ud);
                    return;
                }
                if (st == 0)
                    return;                     /* needs more ciphertext */
                s->tls_up = 1;
                if (s->tls_hs_cb)
                    s->tls_hs_cb(a, 0, NULL, 0, s->tls_hs_udata);
                if (a->fds[fd].r_op != AIO_OP_RECV)
                    return;                     /* the callback disarmed us */
            }
            /* DRAIN EVERY RECORD. One ciphertext arrival routinely carries
               several, and stopping at the first strands the rest until more
               happens to come in. */
            while ((got = dyn_tls_read(t, pt, sizeof pt)) > 0) {
                cb(a, got, pt, (unsigned)got, ud);
                if (a->fds[fd].r_op != AIO_OP_RECV)
                    return;                     /* the callback disarmed us */
            }
            if (got < 0)
                cb(a, -EPROTO, NULL, 0, ud);
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
/* Attach an engine to an fd. From here dyn_aio_send encrypts and the recv
   completion decrypts; `hs_cb` fires once when the handshake settles (res 0)
   or fails (res < 0), which is what a caller reports as `connect`.
   Takes ownership: dyn_aio_close frees it. */
int dyn_aio_tls_attach(dyn_aio_t *a, int fd, dyn_tls_conn_t *tls,
                       dyn_aio_cb hs_cb, void *hs_udata)
{
    aio_fd_t *s;
    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->tls)
        return -1;                 /* one engine per fd, not last-writer-wins */
    s->tls = tls;
    s->tls_hs_cb = hs_cb;
    s->tls_hs_udata = hs_udata;
    s->tls_up = 0;
    return 0;
}

/* Drive the first handshake flight -- a CLIENT must send its hello before any
   ciphertext arrives, so this cannot wait for the recv completion. */
int dyn_aio_tls_start(dyn_aio_t *a, int fd)
{
    aio_fd_t *s;
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

/* ---- lifecycle -------------------------------------------------------- */

dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers)
{
    dyn_aio_t *a = (dyn_aio_t *)calloc(1, sizeof(*a));
    (void)entries; (void)disk_workers; /* used by the io_uring/disk-pool backend */
    if (!a)
        return NULL;
    a->lp = dyn_evloop_new();
    a->rcap = AIO_DEFAULT_BUFSZ;
    a->rbuf = (uint8_t *)malloc(a->rcap);
    if (!a->lp || !a->rbuf) {
        if (a->lp) dyn_evloop_free(a->lp);
        free(a->rbuf);
        free(a);
        return NULL;
    }
    return a;
}

void dyn_aio_free(dyn_aio_t *a)
{
    int fd;
    if (!a)
        return;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t *s = &a->fds[fd];
        if (s->w_own && s->w_buf) free((void *)s->w_buf);
    }
    /* Channel first: it waits out any disk job still on a worker, so nothing
     * can call back into this reactor after it is gone. Then the pool. */
    if (a->chan)
        dyn_pool_chan_free(a->chan);
    if (a->pool)
        dyn_pool_free(a->pool);
    dyn_evloop_free(a->lp);
    free(a->rbuf);
    free(a->fds);
    free(a);
}

/* The shared reactor's loop, so a subsystem that needs an interest dyn_aio has
 * no wrapper for -- the watcher's DYN_EV_VNODE -- registers into the ONE loop
 * rather than standing up a second one. Its callback routes by fd through the
 * same table, so nothing in dyn_aio has to know. */
dyn_evloop_t *dyn_aio_evloop(dyn_aio_t *a)
{
    return a ? a->lp : NULL;
}

int dyn_aio_backend_fd(const dyn_aio_t *a)
{
    return dyn_evloop_backend_fd(a->lp);
}

void dyn_aio_drain(void *aio)
{
    dyn_aio_t *a = (dyn_aio_t *)aio;
    dyn_evloop_poll(a->lp, 0);
    /* Disk completions arrive on the pool's fd, not the readiness backend's.
     * The outer loop has ONE reactor slot and it holds the backend fd, so this
     * is what delivers them -- whenever the loop wakes for any reason. A
     * program doing ONLY disk still needs dyn_aio_disk_fd() registered
     * separately to be woken at all; see the pool notes. */
    if (a->chan)
        dyn_pool_drain(a->chan);
}

int dyn_aio_run(dyn_aio_t *a, int timeout_ms)
{
    return dyn_evloop_poll(a->lp, timeout_ms);
}

size_t dyn_aio_inflight(const dyn_aio_t *a)
{
    return a->inflight;
}

/* Bytes this fd still owes the wire: the active slot's remainder, every
 * deferred send node, and any sendfile tail. This is the number a streaming
 * caller (ws/sse push) bounds against -- without it a handler flooding a
 * peer that never reads grows the queue without limit. */
size_t dyn_aio_queued(const dyn_aio_t *a, int fd)
{
    const aio_fd_t *s;
    const aio_wnode_t *q;
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

/* ---- network ---------------------------------------------------------- */

int dyn_aio_listen(dyn_aio_t *a, const char *host, uint16_t port, int backlog)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;

    /* A host containing ':' is an IPv6 literal: bind v6, and with V6ONLY off
     * (the platform default on macOS; set explicitly where the kernel needs
     * it) a wildcard v6 socket also serves v4-mapped peers. */
    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
#ifdef IPV6_V6ONLY
        int v6only = 0;
#endif
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
#ifdef IPV6_V6ONLY
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        {   /* accept "[::1]" bracket spelling; copy for inet_pton */
            const char *h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf))
                hl = sizeof(hbuf) - 1;
            if (h[0] == '[') { h++; if (hl > 0) hl--; 
                               if (hl > 0 && h[hl - 1] == ']') hl--; }
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
        if (bind(fd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0 ||
            listen(fd, backlog > 0 ? backlog : 1024) < 0) {
            close(fd);
            return -1;
        }
        dyn_net_set_nonblock(fd);
        return fd;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 1024) < 0) {
        close(fd);
        return -1;
    }
    dyn_net_set_nonblock(fd);
    return fd;
}

/* ---- IPC: AF_UNIX ------------------------------------------------------ */

/* Fill `sa` from `path`. -1 if the path cannot fit sun_path, which is a fixed
 * ~104 bytes -- a silent truncation here would bind the WRONG path. */
static int unix_addr(struct sockaddr_un *sa, const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof(sa->sun_path))
        return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, path, n + 1);
    return 0;
}

int dyn_aio_unix_listen(dyn_aio_t *a, const char *path, int backlog)
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
    dyn_net_set_nonblock(fd);
    /* A leftover socket file makes bind fail with EADDRINUSE. Unlinking is
     * standard, and it is also why the directory -- not the socket -- carries
     * the access control: anyone who can unlink here can hijack the endpoint. */
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog > 0 ? backlog : 128) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_unix_connect(dyn_aio_t *a, const char *path, dyn_aio_cb cb,
                         void *udata)
{
    int fd;
    struct sockaddr_un sa;
    aio_fd_t *s;

    if (!a || !path || unix_addr(&sa, path) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
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
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 &&
        errno != EINPROGRESS && errno != EINTR)
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

/* ---- datagram ---------------------------------------------------------- */

int dyn_aio_udp_bind(dyn_aio_t *a, const char *bind_host, uint16_t port)
{
    int fd, on = 1;
    struct sockaddr_in sa;
    (void)a;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    dyn_net_set_nonblock(fd);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (bind_host && *bind_host) ? inet_addr(bind_host)
                                                   : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int dyn_aio_recvfrom(dyn_aio_t *a, int fd, dyn_aio_dgram_cb cb, void *udata)
{
    aio_fd_t *s;
    if (!a || !cb || fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    s->dg_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_RECVFROM;
    s->r_multishot = 1;          /* a datagram socket stays armed */
    a->inflight++;
    if (dyn_evloop_add(a->lp, fd, DYN_EV_READ, aio_dispatch, a) < 0) {
        s->r_op = AIO_OP_NONE;
        a->inflight--;
        return -1;
    }
    s->active = 1;
    return 0;
}

int dyn_aio_sendto(dyn_aio_t *a, int fd, const void *buf, size_t len,
                   const struct sockaddr *peer, unsigned peerlen)
{
    ssize_t n;
    (void)a;
    /* Datagrams are sent inline, not queued: a UDP send either fits in the
     * socket buffer or is dropped, so there is no partial-write state to keep
     * and queueing would only add a copy. */
    do {
        /* A NULL peer means the socket is connect()ed: BSD returns EISCONN for
         * a sendto() carrying an address on such a socket, so the two cases
         * need different calls, not a NULL pointer passed through. */
        n = peer ? sendto(fd, buf, len, 0, peer, (socklen_t)peerlen)
                 : send(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int)n;
}

/* AI_NUMERICHOST FIRST, so a literal address costs no resolver at all and only
 * a real hostname pays for one. That second lookup BLOCKS the calling thread --
 * the same trade dyn_http_connect already makes -- which is acceptable because
 * connect() is invoked from a JS call rather than from inside a loop turn.
 * Moving it onto the pool is the next step and is noted at the declaration. */

int dyn_aio_connect(dyn_aio_t *a, const char *host, uint16_t port,
                    dyn_aio_cb cb, void *udata)
{
    int fd, fam = AF_INET;
    struct sockaddr_storage sa;
    socklen_t salen = 0;
    aio_fd_t *s;

    if (!a || !host)
        return -1;
    /* Resolve BEFORE the socket: the family decides what to open, and the old
       inet_addr() turned any hostname into 255.255.255.255, so the caller was
       told "address family not supported" -- an errno naming the wrong cause
       entirely. */
    if (dyn_aio_resolve(host, port, &sa, &salen, &fam) != 0) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    dyn_net_set_nonblock(fd);
    dyn_net_set_nodelay(fd);
#ifdef SO_NOSIGPIPE
    /* The accept path sets this (:352); a connect()'d socket without it kills
       the process with SIGPIPE on the first send to an RST'd peer (macOS has
       no MSG_NOSIGNAL, so AIO_SEND_FLAGS is 0 there). */
    { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
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

    if (connect(fd, (struct sockaddr *)&sa, salen) == 0) {
        /* Connected immediately (loopback commonly does). Do NOT call back from
         * here -- the caller has not yet seen the fd we are about to return, so
         * a synchronous callback would hand it a descriptor it cannot correlate.
         * Arm for writability instead and let the normal path deliver it. */
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
    close(fd);
    return -1;
}

int dyn_aio_accept(dyn_aio_t *a, int listen_fd, dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    if (fd_ensure(a, listen_fd) < 0)
        return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb;
    s->r_udata = udata;
    s->r_op = AIO_OP_ACCEPT;
    s->r_multishot = 1;
    a->inflight++;
    if (dyn_evloop_add(a->lp, listen_fd, DYN_EV_READ, aio_dispatch, a) < 0) {
        s->r_op = AIO_OP_NONE;
        a->inflight--;
        return -1;
    }
    return 0;
}

int dyn_aio_recv(dyn_aio_t *a, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    (void)pool; /* provided-buffer pool is an io_uring optimization; shared buf here */
    /* The callback is REQUIRED here, unlike send/connect/sendto where NULL means
     * fire-and-forget -- the dispatcher calls it unguarded, and a NULL indirect
     * call does not reliably fault: on some targets it re-executes forever at
     * 100% CPU with no signal. Refuse at the call, where the caller can see it.
     * dyn_aio_recvfrom has always checked this; recv had not. */
    if (!a || !cb) { errno = EINVAL; return -1; }
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
            s->r_op = AIO_OP_NONE; a->inflight--; return -1;
        }
    } else {
        aio_apply_interest(a, fd);
    }
    return 0;
}

static int aio_send_raw(dyn_aio_t *a, int fd, const void *buf, size_t len,
                        int flags, dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    size_t off = 0;
    (void)flags; /* DYN_AIO_ZC is an io_uring send_zc option; plain send here */

    if (fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    /* Anything already pending on this fd means this send must go BEHIND it.
     * Sending inline here would put these bytes ahead of the queued ones and
     * silently reorder the stream. */
    if (s->w_len > s->w_off || s->w_qhead) {
        aio_wnode_t *node = (aio_wnode_t *)malloc(sizeof(*node));
        if (!node)
            return -1;
        node->buf = (uint8_t *)malloc(len ? len : 1);
        if (!node->buf) { free(node); return -1; }
        memcpy(node->buf, buf, len);
        node->len = len;
        node->cb = cb;
        node->udata = udata;
        node->next = NULL;
        if (s->w_qtail) s->w_qtail->next = node;
        else            s->w_qhead = node;
        s->w_qtail = node;
        return 0;
    }
    /* fast path: try to send inline; most small responses complete now */
    for (;;) {
        ssize_t n = send(fd, (const uint8_t *)buf + off, len - off, AIO_SEND_FLAGS);
        if (n > 0) { off += (size_t)n; if (off >= len) break; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1; /* hard error */
    }
    if (off >= len) {
        if (cb) cb(a, (int)len, NULL, 0, udata); /* synchronous completion */
        return 0;
    }
    /* partial: buffer the remainder and finish on WRITE readiness */
    {
        uint8_t *copy = (uint8_t *)malloc(len - off);
        if (!copy)
            return -1;
        memcpy(copy, (const uint8_t *)buf + off, len - off);
        s->w_buf = copy; s->w_len = len - off; s->w_off = 0; s->w_own = 1;
        s->w_full = len;
        s->w_cb = cb; s->w_udata = udata;
        a->inflight++;
        if (!s->active) {
            s->active = 1;
            if (dyn_evloop_add(a->lp, fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
                return -1;
        } else {
            aio_apply_interest(a, fd);
        }
    }
    return 0;
}

int dyn_aio_close(dyn_aio_t *a, int fd)
{
    aio_fd_t *s;
    if (fd < 0 || fd >= a->cap)
        { close(fd); return 0; }
    s = &a->fds[fd];
    dyn_evloop_del(a->lp, fd);
#ifdef CONFIG_TLS
    /* The attach took ownership, so this is where the engine goes. */
    if (s->tls) {
        dyn_tls_conn_free(s->tls);
        s->tls = NULL;
        s->tls_hs_cb = NULL;
        s->tls_hs_udata = NULL;
        s->tls_up = 0;
    }
#endif
    /* Every send that carries a callback completes exactly once, here with an
     * error. Dropping them silently strands whatever the caller was holding
     * for the send -- a reference, or a query that then never resolves. */
    {
        aio_fd_t dead = *s;
        if (s->w_own && s->w_buf) free((void *)s->w_buf);
        if (s->w_file_rem > 0) { /* file transfer interrupted (peer gone) */
            if (s->w_file_fd > 0) close(s->w_file_fd);
            if (a->inflight) a->inflight--;
        }
        if (s->r_op != AIO_OP_NONE && a->inflight) a->inflight--;
        if ((s->w_cb || s->w_len > s->w_off) && a->inflight) a->inflight--;
        memset(s, 0, sizeof(*s));   /* clear BEFORE any callback re-enters */
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

/* ---- not-yet-landed entry points (io_uring / disk pool increment) ----- */

int dyn_aio_pool_register(dyn_aio_t *a, unsigned n, unsigned sz)
{ (void)a; (void)n; (void)sz; errno = ENOSYS; return -1; }
/* The public send. With TLS the plaintext goes through the engine and the
   CIPHERTEXT is handed to the ordinary path, so queueing, ordering and
   partial-write handling below are exercised identically for both. The
   plaintext path is byte-for-byte unchanged: nothing here runs unless the fd
   has a conn attached. */
int dyn_aio_send(dyn_aio_t *a, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata)
{
#ifdef CONFIG_TLS
    if (fd_ensure(a, fd) < 0)
        return -1;
    if (a->fds[fd].tls) {
        dyn_tls_conn_t *t = a->fds[fd].tls;
        uint8_t chunk[16384], *ct = NULL, *nb;
        size_t ctn = 0, ctcap = 0, woff = 0;
        int n, rc;

        while (woff < len) {
            int w = dyn_tls_write(t, (const uint8_t *)buf + woff, len - woff);
            if (w <= 0) { free(ct); return -1; }
            woff += (size_t)w;
        }
        while ((n = dyn_tls_pull(t, chunk, sizeof chunk)) > 0) {
            if (ctn + (size_t)n > ctcap) {
                ctcap = (ctn + (size_t)n) * 2;
                nb = (uint8_t *)realloc(ct, ctcap);
                if (!nb) { free(ct); return -1; }
                ct = nb;
            }
            memcpy(ct + ctn, chunk, (size_t)n);
            ctn += (size_t)n;
        }
        if (!ctn) { free(ct); return 0; }
        rc = aio_send_raw(a, fd, ct, ctn, flags, cb, udata);
        free(ct);                       /* aio_send_raw copies what it queues */
        return rc;
    }
#endif
    return aio_send_raw(a, fd, buf, len, flags, cb, udata);
}

int dyn_aio_sendfile(dyn_aio_t *a, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata)
{
    aio_fd_t *s;
    if (fd_ensure(a, out_fd) < 0)
        return -1;
    s = &a->fds[out_fd];
    s->w_file_fd = in_fd; /* adapter owns it: closed on completion/close */
    s->w_file_off = offset;
    s->w_file_rem = (off_t)len;
    s->w_file_cb = cb;
    s->w_file_udata = udata;
    a->inflight++;
    /* if a buffered prefix (e.g. the HTTP header) is still draining, the file is
     * sent after it by the WRITE dispatch; otherwise try to send inline now. */
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
        if (dyn_evloop_add(a->lp, out_fd, DYN_EV_WRITE, aio_dispatch, a) < 0)
            return -1;
    } else {
        aio_apply_interest(a, out_fd);
    }
    return 0;
}
/* ---- disk, on the shared pool -------------------------------------------
 * The routing rule: a handoff costs ~1-5us, so it only pays
 * when the work is longer than that. Everything here is a syscall that can
 * block for milliseconds on a cache miss; when the pool refuses, the operation
 * runs INLINE, which is always correct and merely blocks the loop briefly. */

static void aio_disk_work(void *arg)   /* WORKER THREAD: no JS, no aio state */
{
    aio_disk_t *j = (aio_disk_t *)arg;
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
        /* fsync(2) on Darwin does not flush the drive's own write cache;
         * F_FULLFSYNC does, and is what durability actually requires here. */
        n = j->datasync ? fsync(j->fd) : fcntl(j->fd, F_FULLFSYNC);
        if (n < 0 && errno == ENOTSUP)
            n = fsync(j->fd);          /* not every filesystem supports it */
#else
        n = j->datasync ? fdatasync(j->fd) : fsync(j->fd);
#endif
        break;
    default:
        n = -1; errno = EINVAL; break;
    }
    j->res = (n < 0) ? -errno : (int)n;
}

static void aio_disk_done(void *arg)   /* LOOP THREAD: safe to call back */
{
    aio_disk_t *j = (aio_disk_t *)arg;
    dyn_aio_t *a = j->aio;
    if (a->inflight)
        a->inflight--;
    if (j->cb)
        j->cb(a, j->res, NULL, 0, j->udata);
    free(j->path);
    free(j);
}

/* The pool's wake fd is readable; drain the completions it announces. */
static void aio_disk_wake(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_aio_t *a = (dyn_aio_t *)udata;
    (void)lp; (void)fd; (void)events;
    if (a->chan)
        dyn_pool_drain(a->chan);
}

/* Lazily bring up the pool + this reactor's channel. 0 on success. */
static int aio_pool_ready(dyn_aio_t *a)
{
    int wfd;
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
    /* Fold the wake fd into OUR OWN backend rather than asking the engine for a
     * second reactor slot: the outer loop polls one fd, and adding this here
     * makes that fd readable when a disk completion lands. Without it a program
     * whose only pending work is disk parks in poll() and never wakes. */
    wfd = dyn_pool_wake_fd(a->chan);
    if (wfd >= 0)
        (void)dyn_evloop_add(a->lp, wfd, DYN_EV_READ, aio_disk_wake, a);
    return 0;
}

static int aio_disk_submit(dyn_aio_t *a, aio_disk_t *j)
{
    if (aio_pool_ready(a) == 0 &&
        dyn_pool_submit(a->chan, aio_disk_work, aio_disk_done, j) == 0) {
        a->inflight++;
        return 0;
    }
    /* Refused or unavailable: do it here. The caller still gets its callback,
     * so behaviour is identical -- only the latency differs. */
    a->inflight++;
    aio_disk_work(j);
    aio_disk_done(j);
    return 0;
}

/* A caller-supplied blocking job. Wraps their two callbacks so the inflight
 * count is released on the loop thread whichever way the job ran. */
typedef struct {
    dyn_aio_t *aio;
    void (*work)(void *);
    void (*done)(void *);
    void *arg;
} aio_offload_t;

static void aio_offload_work(void *p)     /* WORKER THREAD: no JS_* in here */
{
    aio_offload_t *j = (aio_offload_t *)p;
    if (j->work)
        j->work(j->arg);
}

static void aio_offload_done(void *p)     /* LOOP THREAD: JS is safe here */
{
    aio_offload_t *j = (aio_offload_t *)p;
    dyn_aio_t *a = j->aio;
    if (a->inflight)
        a->inflight--;
    if (j->done)
        j->done(j->arg);
    free(j);
}

int dyn_aio_offload(dyn_aio_t *a, void (*work)(void *), void (*done)(void *),
                    void *arg)
{
    aio_offload_t *j;
    if (!a)
        return -1;
    j = (aio_offload_t *)calloc(1, sizeof(*j));
    if (!j) {
        /* Out of memory is not a reason to drop the caller's completion. */
        if (work) work(arg);
        if (done) done(arg);
        return 1;
    }
    j->aio = a; j->work = work; j->done = done; j->arg = arg;
    a->inflight++;
    if (aio_pool_ready(a) == 0 &&
        dyn_pool_submit(a->chan, aio_offload_work, aio_offload_done, j) == 0)
        return 0;
    /* Refused or unavailable: run it here. Blocking the loop thread on a queue
     * its own drain must empty is a deadlock; inline is always correct. */
    aio_offload_work(j);
    aio_offload_done(j);
    return 1;
}

static aio_disk_t *aio_disk_new(dyn_aio_t *a, int op, dyn_aio_cb cb, void *ud)
{
    aio_disk_t *j = (aio_disk_t *)calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->aio = a; j->op = op; j->cb = cb; j->udata = ud; j->off = -1;
    return j;
}

int dyn_aio_openat(dyn_aio_t *a, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata)
{
    aio_disk_t *j;
    if (!a || !path)
        return -1;
    j = aio_disk_new(a, AIO_DISK_OPEN, cb, udata);
    if (!j)
        return -1;
    j->path = strdup(path);        /* the caller's string may not outlive us */
    if (!j->path) { free(j); return -1; }
    j->fd = dirfd; j->flags = flags; j->mode = mode;
    return aio_disk_submit(a, j);
}

int dyn_aio_read(dyn_aio_t *a, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata)
{
    aio_disk_t *j;
    if (!a || !buf)
        return -1;
    j = aio_disk_new(a, AIO_DISK_READ, cb, udata);
    if (!j)
        return -1;
    j->fd = fd; j->buf = buf; j->len = len; j->off = off;
    return aio_disk_submit(a, j);
}

int dyn_aio_write(dyn_aio_t *a, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata)
{
    aio_disk_t *j;
    if (!a || !buf)
        return -1;
    j = aio_disk_new(a, AIO_DISK_WRITE, cb, udata);
    if (!j)
        return -1;
    j->fd = fd; j->cbuf = buf; j->len = len; j->off = off;
    return aio_disk_submit(a, j);
}

int dyn_aio_fsync(dyn_aio_t *a, int fd, int datasync, dyn_aio_cb cb, void *ud)
{
    aio_disk_t *j;
    if (!a)
        return -1;
    j = aio_disk_new(a, AIO_DISK_FSYNC, cb, ud);
    if (!j)
        return -1;
    j->fd = fd; j->datasync = datasync;
    return aio_disk_submit(a, j);
}

/* Drain disk completions. The caller folds dyn_aio_disk_fd() into its loop. */
void dyn_aio_disk_drain(dyn_aio_t *a)
{
    if (a && a->chan)
        dyn_pool_drain(a->chan);
}

int dyn_aio_set_timer(dyn_aio_t *a, unsigned period_ms)
{
    return a ? dyn_evloop_set_timer(a->lp, period_ms) : -1;
}

int dyn_aio_disk_fd(const dyn_aio_t *a)
{
    return (a && a->chan) ? dyn_pool_wake_fd(a->chan) : -1;
}
/* Disarm every armed operation matching the (cb, udata) cookie. Returns how
 * many were cancelled. The callback is NOT invoked for a cancelled op: the
 * caller asked for it to stop, and firing it would be a completion it did not
 * ask for. Disk jobs already on a worker are NOT cancellable -- they hold a
 * borrowed buffer, so they run to completion and their callback still fires. */
int dyn_aio_cancel(dyn_aio_t *a, dyn_aio_cb cb, void *udata)
{
    int fd, n = 0;
    if (!a)
        return -1;
    for (fd = 0; fd < a->cap; fd++) {
        aio_fd_t *s = &a->fds[fd];
        int touched = 0;
        /* NOT gated on `active`: that flag marks a registered WRITE side, and
         * an accept/recv arms the read side without ever setting it. Each side
         * carries its own armed condition, which is the honest predicate. */
        if (s->r_cb == cb && s->r_udata == udata && s->r_op != AIO_OP_NONE) {
            s->r_cb = NULL; s->r_udata = NULL;
            s->r_op = AIO_OP_NONE; s->r_multishot = 0;
            if (a->inflight) a->inflight--;
            n++; touched = 1;
        }
        if (s->w_cb == cb && s->w_udata == udata && s->w_len > s->w_off) {
            if (s->w_own && s->w_buf) free((void *)s->w_buf);
            s->w_buf = NULL; s->w_len = s->w_off = 0; s->w_own = 0;
            s->w_cb = NULL; s->w_udata = NULL;
            if (a->inflight) a->inflight--;
            n++; touched = 1;
        }
        /* Queued sends carry the same cookie and must go with it, or their
           buffers leak and their callbacks never run. */
        while (s->w_qhead && s->w_qhead->cb == cb && s->w_qhead->udata == udata) {
            aio_wnode_t *q = s->w_qhead;
            s->w_qhead = q->next;
            if (!s->w_qhead) s->w_qtail = NULL;
            free(q->buf); free(q);
            n++; touched = 1;
        }
        if (s->w_file_cb == cb && s->w_file_udata == udata && s->w_file_rem > 0) {
            if (s->w_file_fd >= 0) close(s->w_file_fd);  /* the adapter owns it */
            s->w_file_fd = -1; s->w_file_rem = 0;
            s->w_file_cb = NULL; s->w_file_udata = NULL;
            if (a->inflight) a->inflight--;
            n++; touched = 1;
        }
        if (touched)
            aio_apply_interest(a, fd);   /* stop asking the loop for readiness */
    }
    return n;
}

#endif /* CONFIG_NATIVE_MODULES */
