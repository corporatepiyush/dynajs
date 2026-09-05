/*
 * dyna-aio -- the project-wide async IO adapter: ONE proxy interface over the
 * fixed OS APIs (Linux io_uring, macOS/BSD kqueue), so every disk/network call
 * in the engine goes through an optimized, zero-extra-copy path with a single
 * portable surface. Simplicity of the adapter must NOT cost a copy or a syscall
 * the raw OS API wouldn't.
 *
 * MODEL: completion-oriented (submit an op, get a completion with a result),
 * because that maps 1:1 onto io_uring and cleanly subsumes kqueue:
 *   - io_uring: op -> SQE; completion <- CQE. SINGLE_ISSUER|DEFER_TASKRUN so
 *     completion task-work runs in the JS thread at reap time (no kernel worker
 *     touches JS state); provided-buffer ring for recv; registered fixed buffers
 *     + files for read/write/send; send_zc + splice/sendfile for zero-copy TX.
 *   - kqueue: the backend hides readiness-then-syscall behind the same
 *     completion callback, doing the recv/send/accept directly into/from the
 *     caller's buffer -- no extra copy.
 *
 * DISK: implemented on both backends. The readiness backend runs it on the
 * shared dyn-pool, because no readiness mechanism can make a regular file
 * pollable; io_uring sends the same calls straight to the kernel. Only
 * pool_register is still unimplemented on the readiness backend -- it is an
 * io_uring buffer-ring concept. `disk_workers` is accepted and ignored: the
 * pool is process-wide and sized by --io-threads.
 *
 * THREADING: one dyn_aio per JS thread; ALL submission and completion on that
 * thread (io_uring SINGLE_ISSUER). dyn_aio_backend_fd() folds the whole thing
 * into js_std_loop via js_std_set_io_reactor (see dyna-libc.h).
 *
 * kTLS/offload: a socket may be promoted to a kTLS ULP (setsockopt TLS_TX/RX);
 * send/sendfile/splice on it stay zero-copy. The adapter treats such an fd like
 * any other -- kTLS composes without a separate code path.
 */
#ifndef DYNAJS_AIO_H
#define DYNAJS_AIO_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "dyna-io.h" /* dyn_iobuf_t */

typedef struct dyn_aio dyn_aio_t;

/* Completion result codes: `res >= 0` is a byte count / accepted fd / 0-ok;
 * `res < 0` is -errno. `buf`/`buf_len` are set only for recv completions (a
 * BORROWED view into a pool buffer valid until the callback returns; the adapter
 * recycles it after). `udata` is the per-op cookie passed at submit. */
typedef void (*dyn_aio_cb)(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned buf_len, void *udata);

/* ---- lifecycle -------------------------------------------------------- */

/* Create the per-thread reactor (io_uring on Linux, kqueue/epoll/poll
 * elsewhere). `disk_workers` is RESERVED and currently ignored by both
 * backends. NULL on failure. */
dyn_aio_t *dyn_aio_new(unsigned entries, unsigned disk_workers);
void dyn_aio_free(dyn_aio_t *aio);

/* The single pollable fd (io_uring eventfd / kqueue fd) to fold into an outer
 * event loop. Register with js_std_set_io_reactor(ctx, fd, dyn_aio_drain, aio). */
int dyn_aio_backend_fd(const dyn_aio_t *aio);

/* The reactor's underlying loop, for a subsystem needing a DYN_EV_* interest
 * dyn_aio does not wrap (the file watcher's DYN_EV_VNODE). Registering here
 * keeps the one-reactor-per-thread rule. */
struct dyn_evloop;
struct dyn_evloop *dyn_aio_evloop(dyn_aio_t *aio);

/* Reap all ready completions and invoke their callbacks (non-blocking). This is
 * the drain the JS loop calls when backend_fd signals -- its signature matches
 * js_std_set_io_reactor's drain hook exactly. */
void dyn_aio_drain(void *aio);
/* Standalone step: wait up to timeout_ms and dispatch. Returns #dispatched. */
int dyn_aio_run(dyn_aio_t *aio, int timeout_ms);

/* Count of ops still in flight (for loop-liveness / drain-before-close). */
size_t dyn_aio_inflight(const dyn_aio_t *aio);
/* Bytes queued for `fd` that the kernel has not taken (active send remainder
 * + deferred send nodes + sendfile tail). The backpressure signal for
 * streaming senders; 0 when the fd is not attached. */
size_t dyn_aio_queued(const dyn_aio_t *aio, int fd);

/* ---- generic offload --------------------------------------------------- */

/* Run `work` on a pool thread, then `done` on the LOOP thread. Shares the same
 * pool, completion channel and wake fd the disk ops use, so a completion wakes
 * the loop without a second reactor slot.
 *
 * ALWAYS COMPLETES. If the pool is unavailable or its queue is full, BOTH
 * callbacks run inline before this returns -- refusing would make every caller
 * write the fallback, and blocking the loop thread on a queue it must itself
 * drain is a deadlock. Only the latency differs, so a caller cannot tell.
 * Returns 0 if it went to a thread, 1 if it ran inline, -1 on a bad argument.
 *
 * `work` must touch NOTHING but `arg`, and must NOT call any JS_* function --
 * it is on a worker thread. `done` is where a JSContext may be touched.
 *
 * The op counts toward dyn_aio_inflight() until `done` returns, so the loop
 * stays alive across it. */
int dyn_aio_offload(dyn_aio_t *aio, void (*work)(void *), void (*done)(void *),
                    void *arg);

/* ---- buffer pool (registered fixed buffers on io_uring) --------------- */

/* NOT IMPLEMENTED: returns -1/ENOSYS on the readiness backend and is a no-op on
 * io_uring (its provided-buffer ring is built in dyn_aio_new). */
int dyn_aio_pool_register(dyn_aio_t *aio, unsigned n, unsigned sz);

/* ---- network ---------------------------------------------------------- */

/* Bind+listen a non-blocking TCP socket (SO_REUSEADDR/REUSEPORT, TCP_NODELAY on
 * accepted conns). Returns the listen fd or -1. `host` NULL => all interfaces. */
int dyn_aio_listen(dyn_aio_t *aio, const char *host, uint16_t port, int backlog);

/* Multishot accept: `cb` fires once per accepted connection (res = new fd), for
 * the life of the listener, until dyn_aio_cancel(). One submit, no re-arm. */
int dyn_aio_accept(dyn_aio_t *aio, int listen_fd, dyn_aio_cb cb, void *udata);

/* Implemented on BOTH backends. On io_uring it is IORING_OP_CONNECT with the
 * sockaddr on a heap context (the kernel reads it after submit, so a stack copy
 * would be a use-after-free).
 *
 * Address literals resolve synchronously (AI_NUMERICHOST never consults a
 * resolver) and keep the family-true socket. A real hostname resolves on the
 * OFFLOAD POOL on both backends -- never on the loop thread (audit E11-04).
 * Because every in-tree caller stores the returned fd and treats fd < 0 as
 * "failed synchronously, udata freed", the hostname path hands out a DUAL-STACK
 * socket (AF_INET6, V6ONLY=0) before the family is known; a v4 answer connects
 * as a v4-mapped address (see dyn_aio_cand_t).
 *
 * Answers are tried SEQUENTIALLY (RFC 8305 subset): a failed attempt -- ::1
 * first with an AF_INET-only peer, or no IPv6 route at all -- retries the next
 * resolver answer on the SAME fd number (dyn_aio_cand_refresh_fd), so the fd
 * the caller stored stays valid across the whole chain.
 *
 * Asynchronous connect. Returns the new socket fd immediately (>= 0) or -1; the
 * connection is NOT established yet. `cb` fires once with res == 0 on success or
 * -errno on failure, and the fd is the caller's to close either way.
 * Non-blocking throughout: a blocking connect would stall the loop for a whole
 * TCP handshake, which is the entire reason this exists. */
int dyn_aio_connect(dyn_aio_t *aio, const char *host, uint16_t port,
                    dyn_aio_cb cb, void *udata);

/* Recv into a pool buffer (io_uring provided-buffer / kqueue read). The callback
 * gets a borrowed view (buf,buf_len); res==0 => peer closed. Optionally multishot
 * (re-arms itself) via `multishot`.
 *
 * `cb` IS REQUIRED and a NULL one returns -1/EINVAL -- unlike dyn_aio_send,
 * dyn_aio_connect and dyn_aio_sendto, where NULL means fire-and-forget. A read
 * with nowhere to deliver is meaningless, and the completion path calls the
 * pointer unguarded. */
int dyn_aio_recv(dyn_aio_t *aio, int fd, int pool, int multishot,
                 dyn_aio_cb cb, void *udata);

/* Send `buf`/`len`. DOES NOT TAKE OWNERSHIP: what cannot go out inline is
 * COPIED into the adapter's own buffer, so `buf` is the caller's to free as
 * soon as this returns. `flags`: DYN_AIO_ZC requests zero-copy (io_uring
 * send_zc + registered buffer); the adapter falls back to a plain send where
 * unsupported. */
#define DYN_AIO_ZC 1
#ifdef CONFIG_TLS
/* ===================== PROVEN, NOT YET WIRED =====================
 * tests/test_aio_tls.c (`make test-aio-tls`) drives this end to end: two
 * engines over a socketpair, a real handshake, and 96 KiB through
 * dyn_aio_send arriving byte-identical. 9/9.
 *
 * The drain loop below IS load-bearing, proven by injection: a `break` after
 * the first record drops delivery to a third of the payload. That only shows
 * up with LARGE socket buffers -- with small ones the ciphertext trickles in
 * one record per completion and the fault is invisible, which is why the test
 * sets SO_RCVBUF explicitly.
 *
 * Still no production caller: dyna-net-tcp.c keeps its own pump. Moving it
 * down here, then Redis, then PG (which needs its SSLRequest upgrade first)
 * is the remaining sequence.
 * ================================================================= */

/* Attach a TLS engine to an fd: dyn_aio_send then encrypts and the recv
   completion decrypts, delivering ONE CALLBACK PER PLAINTEXT RECORD -- one
   ciphertext arrival routinely carries several. `hs_cb` fires once when the
   handshake settles (res 0) or fails (res < 0). Takes ownership of `tls`.
   ONE ENGINE PER FD: a second attach is refused rather than overwriting,
   because a silently replaced engine is undebuggable. */
struct dyn_tls_conn;
int dyn_aio_tls_attach(dyn_aio_t *aio, int fd, struct dyn_tls_conn *tls,
                       dyn_aio_cb hs_cb, void *hs_udata);
/* A CLIENT must send its hello before any ciphertext arrives, so the first
   flight cannot wait for a recv completion. A server does not call this. */
int dyn_aio_tls_start(dyn_aio_t *aio, int fd);
#endif

int dyn_aio_send(dyn_aio_t *aio, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata);

/* File -> socket, streaming and allocation-bounded on BOTH backends.
 *
 * READINESS: sendfile(2) -- the kernel moves the bytes.
 *
 * io_uring: IORING_OP_SPLICE through a per-transfer pipe (F_SETPIPE_SZ 1 MiB,
 * 64 KiB fallback), chunked by pipe capacity; no whole-size allocation and no
 * blocking read on the loop thread (measured 16 MiB transfer: ~3000 MiB/s,
 * peak RSS 2.8 MiB where the old pread+send form cost ~1400 MiB/s and
 * 35 MiB). EOF mid-range completes with the bytes moved. The transfer rides
 * the fd's single-flight write queue, so it starts strictly after any
 * buffered prefix (an HTTP header sent first). */
int dyn_aio_sendfile(dyn_aio_t *aio, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata);

int dyn_aio_close(dyn_aio_t *aio, int fd);

/* ---- datagram ---------------------------------------------------------
 * A datagram needs the PEER ADDRESS, which read()/recv() discards -- so these
 * are separate entry points, not a flag on recv/send. `bind_host` NULL binds
 * all interfaces; port 0 lets the OS choose. */
int dyn_aio_udp_bind(dyn_aio_t *aio, const char *bind_host, uint16_t port);

/* DOES NOT TAKE OWNERSHIP AND DOES NOT QUEUE: a datagram is written inline, so
 * `buf` may be a stack buffer and is free the moment this returns. Unlike the
 * stream send there is no partial-write state -- a datagram either fits the
 * socket buffer or is dropped. */

/* ---- IPC: AF_UNIX stream sockets ---------------------------------------
 * Same accept/recv/send/close path as TCP once the fd exists -- only the
 * address family differs, which is why these are two extra entry points and
 * not a whole transport. */

/* Bind+listen on a filesystem path. A stale socket file from a previous run
 * makes bind fail with EADDRINUSE, so it is unlinked first; that unlink is
 * why the DIRECTORY's permissions are the real access control. */
int dyn_aio_unix_listen(dyn_aio_t *aio, const char *path, int backlog);

/* Implemented on BOTH backends. A path over sizeof(sun_path) is REFUSED with
 * ENAMETOOLONG rather than truncated: a silently shortened path is a connection
 * to a DIFFERENT socket.
 *
 * Asynchronous connect to a unix path. Same contract as dyn_aio_connect: the
 * fd comes back immediately, `cb` fires with 0 or -errno. */
int dyn_aio_unix_connect(dyn_aio_t *aio, const char *path,
                         dyn_aio_cb cb, void *udata);

/* Implemented on BOTH backends. On io_uring it is recvmsg-multishot with
 * IOSQE_BUFFER_SELECT into the provided-buffer ring, riding a per-arm heap
 * msghdr (the kernel reads the msghdr when it executes the SQE, so a stack
 * copy would be a use-after-free -- that lifetime requirement is the whole
 * reason this call used to be ENOSYS there). The peer address comes back
 * inside the selected buffer and is carved out before the callback.
 *
 * The callback's `buf`/`buf_len` are the payload; `peer`/`peerlen` are the
 * sender, borrowed for the duration of the call. */
typedef void (*dyn_aio_dgram_cb)(dyn_aio_t *aio, int res, const uint8_t *buf,
                                 unsigned buf_len, const struct sockaddr *peer,
                                 unsigned peerlen, void *udata);
int dyn_aio_recvfrom(dyn_aio_t *aio, int fd, dyn_aio_dgram_cb cb, void *udata);
/* `peer` NULL sends on a connect()ed socket -- BSD rejects a sendto() carrying
 * an address there with EISCONN, so this is a different call, not a no-op. */
int dyn_aio_sendto(dyn_aio_t *aio, int fd, const void *buf, size_t len,
                   const struct sockaddr *peer, unsigned peerlen);

/* ---- external-fd readiness ---------------------------------------------
 * For an fd whose PROTOCOL the adapter does not speak but whose readability
 * is the whole signal -- the file watcher's inotify fd. LEVEL-TRIGGERED
 * readable: the callback fires while the fd has something to read, and the
 * callback is expected to DRAIN it (inotify's queue, here).
 *
 * io_uring backend only as a symbol: implemented as IORING_OP_POLL_MULTISHOT.
 * The readiness backend has no wrapper -- dyn_aio_evloop() + dyn_evloop_add's
 * DYN_EV_READ IS that backend's level-triggered readable, and its users (the
 * watcher) call it directly; defining a second path there would be a copy
 * that drifts. dyn_aio_unwatch_fd detaches WITHOUT closing (the fd is the
 * caller's); both are idempotent. */
int dyn_aio_watch_fd(dyn_aio_t *aio, int fd,
                     void (*cb)(dyn_aio_t *aio, int fd, void *ud), void *ud);
int dyn_aio_unwatch_fd(dyn_aio_t *aio, int fd);

/* ---- disk (io_uring on Linux; thread pool on macOS/BSD) --------------- */

/* Async open/read/write/fsync/statx. read/write use registered fixed buffers
 * when `buf` is pool-backed. `off < 0` means "current offset"/append semantics
 * where applicable. */
int dyn_aio_openat(dyn_aio_t *aio, int dirfd, const char *path, int flags,
                   int mode, dyn_aio_cb cb, void *udata);
int dyn_aio_read(dyn_aio_t *aio, int fd, void *buf, size_t len, off_t off,
                 dyn_aio_cb cb, void *udata);
int dyn_aio_write(dyn_aio_t *aio, int fd, const void *buf, size_t len, off_t off,
                  dyn_aio_cb cb, void *udata);
int dyn_aio_fsync(dyn_aio_t *aio, int fd, int datasync, dyn_aio_cb cb, void *ud);

/* Disk completions arrive on their OWN pollable fd, separate from
 * dyn_aio_backend_fd(), because they come back from the pool rather than from
 * the readiness backend. -1 until the first disk op creates the channel. Fold
 * it into the same loop and call dyn_aio_disk_drain() when it signals. */
/* READINESS BACKEND ONLY. io_uring delivers disk completions on its own ring,
 * so there is no second fd to poll and nothing to drain: disk_fd returns -1 and
 * disk_drain is a no-op there. A harness that waits on this fd therefore spins
 * forever against io_uring and reports zero completions -- ask the backend for
 * ITS wait primitive rather than assuming the one the first backend had. */
int dyn_aio_disk_fd(const dyn_aio_t *aio);
void dyn_aio_disk_drain(dyn_aio_t *aio);

/* Arm a periodic wakeup on the backend so a caller that must act on a CLOCK --
 * an idle-connection sweep -- runs even when no traffic arrives. 0 disarms.
 * Returns -1 where the backend has no timer primitive (the poll(2) fallback),
 * and the caller must then rely on other wakeups. */
/* Arm a periodic wakeup, or disarm with period_ms == 0. Work that must happen
 * ON A CLOCK cannot be driven by the loop's traffic: hung off the drain, it
 * runs only when something else wakes the loop, so the idle peer the timeout
 * exists for never triggers it. Implemented on BOTH backends -- readiness via
 * the event loop, io_uring via a timerfd registered into the ring.
 * Returns -1/errno; callers must NOT discard it. */
int dyn_aio_set_timer(dyn_aio_t *aio, unsigned period_ms);

/* ---- cancel ----------------------------------------------------------- */

/* Cancel an outstanding multishot/op identified by its (cb,udata) cookie. */
int dyn_aio_cancel(dyn_aio_t *aio, dyn_aio_cb cb, void *udata);

/* ---- shared between the backends -------------------------------------- */

/* Resolve host:port into a sockaddr. SHARED because the two backends are
 * mutually exclusive at compile time: a copy in each is a copy that drifts,
 * and this one carries a fix worth keeping -- inet_addr() turned any hostname
 * into 255.255.255.255, so the caller got "address family not supported",
 * an errno naming the wrong cause. Tries AI_NUMERICHOST first, then a real
 * lookup. Returns 0, or -1 with the family/len untouched. */
int dyn_aio_connect_addr(dyn_aio_t *a, const struct sockaddr_storage *sa,
                         socklen_t salen, int fam,
                         dyn_aio_cb cb, void *udata);

static inline int dyn_aio_resolve(const char *host, uint16_t port,
                                  struct sockaddr_storage *ss, socklen_t *slen,
                                  int *fam)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (res) { freeaddrinfo(res); res = NULL; }
        hints.ai_flags = 0;                     /* not an address: resolve it */
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
            return -1;
    }
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *slen = (socklen_t)res->ai_addrlen;
    *fam = res->ai_family;
    freeaddrinfo(res);
    return 0;
}

/* ---- hostname-connect candidates (shared by both backends) --------------
 *
 * The resolve step used to take only the FIRST getaddrinfo answer. glibc sorts
 * ::1 before 127.0.0.1 for "localhost", so an AF_INET-bound upstream behind a
 * hostname proxy got a refused ::1 dial and a 502 although 127.0.0.1 sat unused
 * in the answer set. The hostname path now keeps up to DYN_AIO_MAX_CAND
 * answers and retries them in resolver order on connect failure.
 *
 * All addresses are stored PRE-TRANSLATED for the dual-stack socket (v4
 * answers as ::ffff:a.b.c.d), so the retry arm needs no per-attempt family
 * logic. POOL-THREAD SAFE: collect/translate touch nothing but the struct. */
#define DYN_AIO_MAX_CAND 8
typedef struct dyn_aio_cand {
    struct sockaddr_storage addr[DYN_AIO_MAX_CAND];
    socklen_t len[DYN_AIO_MAX_CAND];
    int n;                 /* candidates collected */
    int i;                 /* index currently in flight */
    int fd;                /* the dual-stack fd the caller holds */
    dyn_aio_cb cb;         /* the user's completion, fired exactly once */
    void *udata;
} dyn_aio_cand_t;

/* v4 -> v4-mapped in place of the translation done at collect time. */
static inline void dyn_aio_cand_map4(struct sockaddr_storage *ss, socklen_t *slen)
{
    const struct sockaddr_in *in4 = (const struct sockaddr_in *)ss;
    struct sockaddr_in6 in6;
    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_port = in4->sin_port;
    /* ::ffff:a.b.c.d is positional: 10 zero bytes, two 0xff, then the 4
     * address bytes -- no inet_pton round-trip needed. */
    ((uint8_t *)&in6.sin6_addr)[10] = 0xff;
    ((uint8_t *)&in6.sin6_addr)[11] = 0xff;
    memcpy((uint8_t *)&in6.sin6_addr + 12, &in4->sin_addr, 4);
    memcpy(ss, &in6, sizeof(in6));
    *slen = (socklen_t)sizeof(in6);
}

/* Collect resolver answers into `c` (translated, resolver order preserved).
 * Returns 0 with c->n >= 1, or -1 when nothing resolved. */
static inline int dyn_aio_cand_collect(const char *host, uint16_t port,
                                       dyn_aio_cand_t *c)
{
    struct addrinfo hints, *res = NULL, *r;
    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    c->n = 0;
    for (r = res; r && c->n < DYN_AIO_MAX_CAND; r = r->ai_next) {
        if (r->ai_addrlen > sizeof(c->addr[0]))
            continue;
        memcpy(&c->addr[c->n], r->ai_addr, r->ai_addrlen);
        c->len[c->n] = (socklen_t)r->ai_addrlen;
        if (r->ai_family == AF_INET)
            dyn_aio_cand_map4(&c->addr[c->n], &c->len[c->n]);
        else if (r->ai_family != AF_INET6)
            continue;               /* served a family we cannot dial: skip it */
        c->n++;
    }
    freeaddrinfo(res);
    return c->n > 0 ? 0 : -1;
}

/* A failed connect leaves a socket unusable (POSIX: state undefined), but the
 * caller stored the fd NUMBER. Refresh it with a fresh dual-stack socket
 * dup2'd onto the same number, configured exactly like the original. Returns
 * 0, or -1 with the old (dead) fd still in place -- the caller's error path
 * closes it. */
static inline int dyn_aio_cand_refresh_fd(dyn_aio_cand_t *c)
{
    int nfd, v6only = 0;

    nfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (nfd < 0)
        return -1;
    fcntl(nfd, F_SETFD, FD_CLOEXEC);
#ifdef IPV6_V6ONLY
    (void)setsockopt(nfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
    dyn_net_set_nonblock(nfd);
    dyn_net_set_nodelay(nfd);
#ifdef SO_NOSIGPIPE
    { int on = 1; setsockopt(nfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
    if (dup2(nfd, c->fd) < 0) {
        close(nfd);
        return -1;
    }
    close(nfd);                    /* the number now refers to the new socket */
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES */
#endif /* DYNAJS_AIO_H */
