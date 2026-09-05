/*
 * dyna-aio (io_uring backend) -- the Linux completion-model implementation of
 * the dyn_aio interface (see dyna-aio.h). Selected by CONFIG_IO_URING; the
 * portable readiness backend (dyna-aio.c) is used otherwise. Same interface, so
 * the HTTP server and any dyn_aio user are backend-agnostic.
 *
 * Design: one ring per JS thread, SINGLE_ISSUER|COOP_TASKRUN so completion
 * task-work runs in-thread at reap time. DEFER_TASKRUN is deliberately NOT
 * set: it defers completion processing until the ring is entered, so the
 * registered eventfd never fires. That eventfd is the
 * pollable fd folded into js_std_loop (dyn_aio_backend_fd); when it signals,
 * dyn_aio_drain reaps every CQE and invokes callbacks on the JS thread. Recv
 * uses a provided-buffer ring (kernel picks the buffer -> zero pre-post copy);
 * accept is multishot. user_data encodes (fd<<3)|op so a completion maps back
 * to the per-fd {cb,udata} slot.
 *
 * The WRITE side is a per-fd single-flight FIFO (see uaio_wnode_t): io_uring
 * guarantees nothing about the execution order of independent SQEs, even on
 * the same fd (io_uring(7); liburing #329; audit E11-08), so at most ONE
 * write SQE is outstanding per fd and further writes are queued as heap
 * nodes, submitted from the previous write's completion -- the readiness
 * backend's w_qhead discipline, transplanted. dyn_aio_sendfile streams as a
 * chain of IORING_OP_SPLICE steps (file -> per-transfer pipe -> socket) under
 * the same single-flight rule instead of preading the whole range (E11-03),
 * and dyn_aio_send tries a plain non-blocking send() inline first (E11-07).
 */
#include <stdio.h>
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <stddef.h>   /* offsetof, for the sockaddr_un length */
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
#include <poll.h>     /* POLLIN for the offload pool's multishot poll */
#include <sys/eventfd.h>
#include <liburing.h>
#include "core/dyn-pool.h"

#define URING_ENTRIES 8192
/* Provided-buffer pool = NBUFS*BUFSZ, 4 MiB total. -ENOBUFS re-arms rather
 * than drops, and buffers recycle synchronously inside the drain, so few are
 * held at once. MEASURED (audit E11-09; docker glibc arm64, kernel 6.8,
 * gcc 14 -O2; 1 GiB through a socketpair recv multishot paced by a blocked
 * writer, 3 runs each): 512x8K = 3666-3682 MiB/s; 256x16K = 3847-3882 MiB/s;
 * 128x32K = 3860-4042 MiB/s. 16K is the pick: the 8K->16K step is a stable
 * ~+5%, 32K overlaps 16K within run-to-run noise while halving the buffer
 * COUNT the whole connection set shares. Override with -D for bulk
 * workloads. */
#ifndef URING_NBUFS
#define URING_NBUFS   256
#endif
#ifndef URING_BUFSZ
#define URING_BUFSZ   16384
#endif
#define URING_BGID    1

enum { UOP_ACCEPT = 0, UOP_RECV = 1, UOP_SEND = 2, UOP_CLOSE = 3,
       UOP_TIMER = 4, UOP_RECVFROM = 5, UOP_WATCH = 6, UOP_CONNECT = 7 };
/* user_data for a slot op: (fd << 10) | (gen << 3) | op. gen disambiguates
 * sequential ops on the SAME fd, so a stale -ECANCELED CQE from a disarmed
 * slot can neither double-decrement inflight nor hit a REUSED fd's new op.
 * The timer keeps gen 0: it is exempt from the gen check (its tfd is only
 * closed in dyn_aio_free, after which nothing drains). */
#define UD(fd, op, gen) (((uint64_t)(unsigned)(fd) << 10) | \
                         ((uint64_t)((gen) & 0x7f) << 3) | (unsigned)(op))
#define UD_FD(ud)    ((int)((ud) >> 10))
#define UD_OP(ud)    ((int)((ud) & 7))
#define UD_GEN(ud)   ((int)(((ud) >> 3) & 0x7f))

typedef struct {
    dyn_aio_cb r_cb;   /* accept or recv completion; UOP_RECVFROM/UOP_WATCH
                        * store their (differently-typed) callback here --
                        * r_op says which contract to call it under */
    void *r_udata;
    uint8_t r_op;      /* UOP_ACCEPT / UOP_RECV / UOP_RECVFROM / UOP_WATCH / 0 */
    uint8_t r_multishot;
    uint8_t r_gen;     /* generation of the SQE currently armed for this slot;
                        * bumped on EVERY arm and disarm, so a CQE whose UD_GEN
                        * differs belongs to an earlier incarnation of the fd */
    void *r_msg;       /* UOP_RECVFROM: the per-arm heap msghdr context (see
                        * uaio_dgram_t); the msghdr must outlive the multishot */
    /* write side (audit E11-08): the ONE write currently holding the fd's
     * single-flight slot, and the FIFO behind it. w_cur is NOT on the list
     * (its next is NULL); the queue holds only not-yet-submitted writes. */
    struct uaio_wnode *w_cur;
    struct uaio_wnode *w_qhead, *w_qtail;
} uaio_fd_t;

/* recvmsg multishot context. A recvmsg's msghdr/iov/name must OUTLIVE the SQE
 * (the kernel reads them asynchronously), and for a multishot arm that means
 * the whole arm -- which is why dyn_aio_recvfrom was ENOSYS for so long. One
 * context lives in the fd slot for the arm's lifetime and is reused by every
 * re-arm. With IOSQE_BUFFER_SELECT the kernel picks the payload buffer, and
 * the completion layout INSIDE that buffer is
 * [io_uring_recvmsg_out][name: msg_namelen bytes][control][payload], so the
 * peer address and payload are carved out with liburing's helpers; res on the
 * CQE is that WHOLE prefix, not the payload length. MEASURED on the container
 * kernel 6.8 (probe): name/payload decode correctly, F_MORE stays set across
 * datagrams, and a ZERO-LENGTH datagram clears F_MORE -- the re-arm below is
 * what keeps the arm alive across those. */
typedef struct {
    struct msghdr mh;
    struct iovec iov;
    struct sockaddr_storage ss; /* reserve, not destination: the address lands
                                 * in the selected buffer, never here */
} uaio_dgram_t;

/* One queued write on an fd: either a buffered send or a file transfer
 * (dyn_aio_sendfile). Heap-allocated because both outlive their caller's
 * frame; the pointer travels in the SQE user_data with the top bit set as a
 * discriminator (Linux user-space pointers are < 2^47, so the top bit is
 * free). */
#define WNODE_BIT (1ULL << 63)

enum { UW_SEND = 1, UW_FILE = 2 };     /* w->kind */
enum { US_NONE = 0, US_IN = 1, US_OUT = 2 };  /* which splice step is in flight */

typedef struct uaio_wnode {
    struct uaio_wnode *next;
    uint8_t kind;      /* UW_SEND / UW_FILE */
    uint8_t step;      /* UW_FILE: the splice step currently submitted */
    uint8_t eof;       /* UW_FILE: file hit EOF before `rem` drained */
    uint8_t dead;      /* fd closed under an in-flight write: shell only; the
                        * pending SQE still holds this pointer, so ONLY its CQE
                        * may free it -- freeing at close time is a UAF */
    int fd;            /* the socket (out_fd) */
    dyn_aio_cb cb;
    void *udata;
    /* UW_SEND: the copied payload, freed on completion. */
    uint8_t *buf;
    size_t len, off;
    /* UW_FILE: zero-copy streaming state. `off` doubles as the file cursor
     * (the two kinds never coexist in one node). */
    int in_fd;         /* owned; closed at completion/failure/flush */
    int pp[2];         /* the transfer's own pipe; -1 until opened */
    off_t rem;         /* file bytes not yet moved into the pipe */
    size_t pipe_cap;   /* granted pipe capacity = one splice chunk */
    size_t in_pipe;    /* bytes sitting in the pipe, not yet on the socket */
    size_t sent;       /* bytes handed to the socket so far */
} uaio_wnode_t;
/* Disk goes to the KERNEL here, not to a thread pool: io_uring already makes a
 * regular file asynchronous, so a pool would add a hop the kernel does not
 * need. Second discriminator bit, same free-high-bits trick as WNODE_BIT. */
#define DISK_BIT (1ULL << 62)

/* A CQE from the multishot poll on the offload pool's wake fd. Disk goes to the
 * kernel here, but dyn_aio_offload runs CALLER-SUPPLIED C, which no ring can
 * execute -- so this backend keeps a pool too, purely for that. Third
 * discriminator bit; carries no pointer, so the low bits are unused. */
#define POOL_BIT (1ULL << 61)
/* connect: the sockaddr must outlive the SQE, so it rides a heap context with
 * its own tag, exactly like the disk and send ops. */
#define CONN_BIT (1ULL << 60)
typedef struct {
    dyn_aio_cb cb;
    void *udata;
    char *path;        /* openat only; freed on completion */
} uaio_disk_t;
typedef struct {
    struct sockaddr_storage sa;  /* MUST outlive the SQE: the kernel reads it
                                  * asynchronously, so a stack copy is a UAF */
    socklen_t salen;
    int fd;
    uint8_t gen;                 /* the fd slot's r_gen at submit: the CQE is
                                  * live only while the slot still shows it */
    dyn_aio_cb cb;
    void *udata;
} uaio_conn_t;

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

/* Forward declarations: both are used above their definitions (249 and 366),
 * and a static definition after an implicit use is an error under C99+. This
 * file had never been compiled -- CONFIG_IO_URING is in no gate. */
static int uaio_pool_ready(dyn_aio_t *a);
static int uaio_arm_timer(dyn_aio_t *a);
static void uaio_cand_done(dyn_aio_t *a, int res, const uint8_t *buf,
                           unsigned n, void *ud);

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

/* ---- per-fd write queue (audits E11-08/E11-03) ----------------------------
 *
 * io_uring(7) is explicit that nothing orders two independent SQEs, even on
 * the same fd -- same-socket sends merely serialize on the socket lock in
 * practice, which is undocumented territory (liburing #329). The old code
 * submitted one send SQE per dyn_aio_send call and trusted submission order,
 * so a header+body response or an A,B,C streaming caller was correct only by
 * kernel courtesy. The fix mirrors the readiness backend's w_qhead: at most
 * ONE write SQE per fd; everything else waits on this FIFO and is submitted
 * from the previous write's completion, where order is kernel-guaranteed. */

/* Release the kernel resources a node owns (payload, file fd, pipe). Does NOT
 * free the node itself: an in-flight node's shell outlives its resources when
 * the fd is closed under it -- the pending SQE still holds the pointer. */
static void uaio_wnode_free_res(uaio_wnode_t *w)
{
    if (w->kind == UW_SEND) {
        free(w->buf);
        w->buf = NULL;
    } else {
        if (w->in_fd >= 0) { close(w->in_fd); w->in_fd = -1; }
        if (w->pp[0] >= 0) { close(w->pp[0]); w->pp[0] = -1; }
        if (w->pp[1] >= 0) { close(w->pp[1]); w->pp[1] = -1; }
    }
}

/* The transfer's pipe: one per dyn_aio_sendfile, released on completion. The
 * chunk size IS the pipe capacity (every splice-in targets an EMPTY pipe, so
 * it can never refuse): 1 MiB where the kernel grants it, the default 64 KiB
 * where F_SETPIPE_SZ is refused, so peak cost per transfer is one pipe no
 * matter the caller's cap -- the whole-range pread this replaces allocated
 * the file size TWICE (pread buffer + the send's copy). */
static int uaio_pipe_open(uaio_wnode_t *w)
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
    w->pipe_cap = 64 * 1024;               /* the kernel default capacity */
#ifdef F_SETPIPE_SZ
    {
        long cap;
        /* Unprivileged processes may set up to fs/pipe-max-size (1 MiB by
         * default); a refusal keeps the default and the stream still works. */
        (void)fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
        cap = fcntl(fds[1], F_GETPIPE_SZ);
        if (cap > 0)
            w->pipe_cap = (size_t)cap;
    }
#endif
    if (w->pipe_cap > (1u << 20))          /* chunk cap: see the audit fix */
        w->pipe_cap = 1u << 20;
    return 0;
}

/* Retire a write node: release what it owns, drop the in-flight mark if it
 * still holds it, account, and deliver its ONE completion. Pumping the next
 * queued write is the caller's job, so a callback that enqueues another send
 * is already sorted behind whatever was queued before it. */
static void uaio_wnode_done(dyn_aio_t *a, int fd, uaio_wnode_t *w, int result)
{
    dyn_aio_cb cb = w->cb;
    void *u = w->udata;

    if (fd >= 0 && fd < a->cap && a->fds[fd].w_cur == w)
        a->fds[fd].w_cur = NULL;
    uaio_wnode_free_res(w);
    free(w);
    if (a->inflight) a->inflight--;
    if (cb) cb(a, result, NULL, 0, u);
}

/* Promote the head of `fd`'s write queue into the fd's one in-flight write.
 * Loops rather than recursing so an SQE-exhausted head cannot stack. */
static void uaio_wq_pump(dyn_aio_t *a, int fd)
{
    for (;;) {
        uaio_fd_t *s;
        uaio_wnode_t *w;
        struct io_uring_sqe *sqe;

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
            continue;             /* this head is dead: try the next, or idle */
        }
        if (w->kind == UW_SEND)
            io_uring_prep_send(sqe, fd, w->buf + w->off, w->len - w->off,
                               MSG_NOSIGNAL);
        else {
            /* First step of the stream: fill the (empty) pipe from the file.
             * No SPLICE_F_NONBLOCK -- the pipe ends are already O_NONBLOCK and
             * the target is empty, and WITHOUT the flag the socket-side step
             * pends on writability inside the ring exactly like the send op,
             * which is the completion model this backend is. */
            w->step = US_IN;
            io_uring_prep_splice(sqe, w->in_fd, w->off, w->pp[1], -1,
                                 w->rem < (off_t)w->pipe_cap
                                     ? (unsigned)w->rem : (unsigned)w->pipe_cap,
                                 SPLICE_F_MOVE);
        }
        io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
        io_uring_submit(&a->ring);
        return;
    }
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

/* Teardown-only queue flush: release every write node's resources WITHOUT
 * delivering completions -- the backend is going away and a callback into a
 * half-dismantled runtime is worse than a dropped one (the readiness backend's
 * dyn_aio_free makes the same call). Runs AFTER io_uring_queue_exit, which
 * waits out every in-flight request: before that, the kernel may still be
 * reading a node's buffer. */
static void uaio_wq_destroy(dyn_aio_t *a)
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
        /* a still-armed datagram recvmsg owns its heap context; the ring is
         * already exited above, so nothing will reference it again */
        free(a->fds[fd].r_msg);
        a->fds[fd].r_msg = NULL;
    }
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
    uaio_wq_destroy(a);
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
    uaio_fd_t *s = &a->fds[fd];
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV, s->r_gen));
}

/* (Re-)arm the datagram recvmsg on an already-set-up slot. The context is
 * allocated by dyn_aio_recvfrom and owned by the slot until disarm. Re-fetches
 * the slot: the completion callback ran before a re-arm and may have closed
 * this fd (disarming the slot, freeing r_msg) or grown a->fds. */
static int uaio_arm_recvfrom(dyn_aio_t *a, int fd)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;
    uaio_dgram_t *dg;

    if (fd < 0 || fd >= a->cap)
        return -1;
    s = &a->fds[fd];
    dg = (uaio_dgram_t *)s->r_msg;
    if (s->r_op != UOP_RECVFROM || !dg)
        return -1; /* the callback disarmed or replaced the slot */
    sqe = uaio_sqe(a);
    if (!sqe)
        return -1;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
    io_uring_prep_recvmsg_multishot(sqe, fd, &dg->mh, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECVFROM, s->r_gen));
    return 0;
}

/* Drop a datagram slot's arm: release the heap context. Split from
 * uaio_arm_recvfrom because the -ECANCELED CQE of the canceled arm may still
 * be in flight -- the gen bump makes it a no-op at reap time, and the context
 * is not referenced by the kernel after cancellation. */
static void uaio_disarm_recvfrom(dyn_aio_t *a, int fd)
{
    uaio_fd_t *s = &a->fds[fd];
    free(s->r_msg);
    s->r_msg = NULL;
    s->r_cb = NULL;
    s->r_udata = NULL;
    s->r_op = 0;
    s->r_multishot = 0;
    if (a->inflight) a->inflight--;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
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
    if (ud & CONN_BIT) { /* a connect completion (heap context pointer) */
        uaio_conn_t *cc = (uaio_conn_t *)(uintptr_t)(ud & ~CONN_BIT);
        int cfd = cc->fd;
        /* A connect CQE that lands after its fd was CLOSED must not fire:
         * unlike the readiness backend (where close deregisters and a
         * completion after that simply never happens), an already-posted CQE
         * is immutable in the ring, and dyn_aio_close's cancel cannot retract
         * it. Callers above this adapter free their per-connect state at
         * close (dyna-net-tcp's teardown), so a stale CQE dispatching is a
         * use-after-free there -- measured: test_cov_net segfaulted in
         * eb_on_connect on a freed dyn_tcp_t exactly this way. The fd slot's
         * r_op is the guard: dyn_aio_close clears it (and has already taken
         * the inflight count), so a mismatch means drop, not dispatch. */
        if (cfd < 0 || cfd >= a->cap || a->fds[cfd].r_op != UOP_CONNECT ||
            a->fds[cfd].r_gen != cc->gen) {
            /* A hostname connect's retry chain owns a heap ctx that its own
             * completion frees; this CQE is being dropped without firing it,
             * so the drop happens here or the ctx leaks. */
            if (cc->cb == uaio_cand_done)
                free(cc->udata);
            free(cc);
            return;
        }
        if (a->inflight) a->inflight--;
        a->fds[cfd].r_op = 0;  /* landed: the slot no longer owes anything */
        /* res is 0 or -errno. The fd is the caller's to close either way --
         * the same contract the readiness backend documents. */
        if (cc->cb) cc->cb(a, res, NULL, 0, cc->udata);
        free(cc);
        return;
    }
    if (ud & WNODE_BIT) { /* a write node completed its current step */
        uaio_wnode_t *w = (uaio_wnode_t *)(uintptr_t)(ud & ~WNODE_BIT);
        int wfd = w->fd;

        if (w->dead) { /* its fd was closed under it: retire the shell only */
            free(w);
            if (a->inflight) a->inflight--;
            return;
        }
        if (w->kind == UW_SEND) {
            if (res > 0) w->off += (size_t)res;
            if ((res > 0 || res == -EAGAIN) && w->off < w->len) {
                /* partial send: resubmit the remainder (socket buffer was
                 * full); the node keeps the fd's single-flight slot */
                struct io_uring_sqe *sqe = uaio_sqe(a);
                if (sqe) {
                    io_uring_prep_send(sqe, wfd, w->buf + w->off,
                                       w->len - w->off, MSG_NOSIGNAL);
                    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
                    io_uring_submit(&a->ring);
                    return; /* keep w alive until fully sent */
                }
            }
            uaio_wnode_done(a, wfd, w, res < 0 ? res : (int)w->off);
            uaio_wq_pump(a, wfd);
            return;
        }
        /* UW_FILE: one splice step of the stream settled. */
        if (w->step == US_IN) {                     /* file -> pipe */
            if (res > 0) {
                w->off += res;
                w->rem -= res;
                w->in_pipe += (size_t)res;
            } else if (res == 0) {
                w->eof = 1;  /* file shorter than `len`: complete with what moved */
            } else if (res != -EAGAIN) {
                uaio_wnode_done(a, wfd, w, res);
                uaio_wq_pump(a, wfd);
                return;
            }
            if (w->in_pipe > 0)
                w->step = US_OUT;                  /* pipe -> socket, below */
            else if (w->eof) {
                uaio_wnode_done(a, wfd, w, (int)w->sent);
                uaio_wq_pump(a, wfd);
                return;
            } else {
                w->step = US_IN;                   /* empty-pipe -EAGAIN: refill */
            }
        } else {                                    /* US_OUT: pipe -> socket */
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
                w->step = US_OUT;                  /* partial: drain the rest */
            else if (w->rem > 0 && !w->eof)
                w->step = US_IN;                   /* pipe empty: next chunk */
            else {
                /* Streamed everything the file had. Result is the byte count
                 * put on the wire (this backend's send contract); the 32 MiB
                 * caller cap keeps it inside int. The readiness backend
                 * reports 0-ok here -- a pre-existing divergence, kept so no
                 * uring caller sees its completion values change. */
                uaio_wnode_done(a, wfd, w, (int)w->sent);
                uaio_wq_pump(a, wfd);
                return;
            }
        }
        {
            struct io_uring_sqe *sqe = uaio_sqe(a);
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
                                         ? (unsigned)w->rem : (unsigned)w->pipe_cap,
                                     SPLICE_F_MOVE);
            io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w | WNODE_BIT);
            io_uring_submit(&a->ring);
        }
        return;
    }
    fd = UD_FD(ud); op = UD_OP(ud);
    if (op == UOP_TIMER) {
        /* The timer is keyed by tfd and owns NO slot in the fds table, so the
         * cap bound below does not apply to it. Checking it first dropped the
         * tick CQE whenever the arm preceded any fd_ensure (cap == 0): the
         * read re-armed once at most, and the whole deadline family -- pg/tcp
         * connect timeouts, idle sweeps -- never fired on a quiet loop. */
        if (a->tick_ms)
            (void)uaio_arm_timer(a);
        return;
    }
    if (fd < 0 || fd >= a->cap) return;
    s = &a->fds[fd];

    /* A CQE from a disarmed slot arrives after close/cancel as -ECANCELED,
     * possibly after the fd was reused by a NEW op; its generation no longer
     * matches the slot's, so drop it -- no decrement, no callback, no re-arm. */
    if (UD_GEN(ud) != s->r_gen)
        return;
    if (op == UOP_ACCEPT) {
        dyn_aio_cb cb = s->r_cb; void *u = s->r_udata;
        if (res >= 0) {
            int on = 1;
            /* Non-blocking, exactly as the readiness backend's accept loop:
             * an accepted socket does NOT inherit O_NONBLOCK on Linux, and
             * dyn_aio_send's inline fast path would otherwise block the loop
             * on a full socket buffer (E11-07's send() needs EAGAIN, not a
             * nap). */
            dyn_net_set_nonblock(res);
            setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            if (cb) cb(a, res, NULL, 0, u);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && res != -ECANCELED) {
            /* multishot accept ended: re-arm under a fresh generation */
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) { s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
                       io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
                       io_uring_sqe_set_data64(sqe, UD(fd, UOP_ACCEPT, s->r_gen)); }
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
            s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
            if (cb) cb(a, res <= 0 ? (res == 0 ? 0 : res) : 0, NULL, 0, u); /* closed/err */
            return;
        }
        if (!(cqe->flags & IORING_CQE_F_MORE)) /* multishot recv ended: re-arm */
            uaio_rearm_recv(a, fd);
        return;
    }
    if (op == UOP_RECVFROM) {
        dyn_aio_dgram_cb dcb = (dyn_aio_dgram_cb)(void *)s->r_cb;
        void *u = s->r_udata;
        int bid = (cqe->flags & IORING_CQE_F_BUFFER)
                      ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;
        if (res == -ENOBUFS) { /* provided ring momentarily empty: re-arm, do
                                * NOT deliver -- buffers recycle in this drain */
            uaio_arm_recvfrom(a, fd);
            return;
        }
        if (res > 0 && bid >= 0) {
            unsigned char *b = a->buf_base + (size_t)bid * a->bufsz;
            struct msghdr *mh = &((uaio_dgram_t *)s->r_msg)->mh;
            struct io_uring_recvmsg_out *o =
                io_uring_recvmsg_validate(b, res, mh);
            if (o) {
                /* deliver the PAYLOAD length, not the CQE res (which counts
                 * the out-header + reserved name space the kernel prefixed). */
                if (dcb)
                    dcb(a, (int)o->payloadlen,
                        (const uint8_t *)io_uring_recvmsg_payload(o, mh),
                        o->payloadlen,
                        (const struct sockaddr *)io_uring_recvmsg_name(o),
                        o->namelen, u);
            } else if (dcb) {
                dcb(a, -EPROTO, NULL, 0, NULL, 0, u); /* buffer layout wrong */
            }
            uaio_recycle(a, bid);
        } else {
            if (bid >= 0) uaio_recycle(a, bid);
            if (dcb) dcb(a, res < 0 ? res : -EIO, NULL, 0, NULL, 0, u);
            uaio_disarm_recvfrom(a, fd);
            return;
        }
        /* A zero-length datagram clears F_MORE (measured, kernel 6.8); the
         * arm must survive it exactly as it survives a buffer-pool dip. */
        if (!(cqe->flags & IORING_CQE_F_MORE))
            uaio_arm_recvfrom(a, fd);
        return;
    }
    if (op == UOP_WATCH) {
        void (*wcb)(dyn_aio_t *, int, void *) =
            (void (*)(dyn_aio_t *, int, void *))(void *)s->r_cb;
        void *u = s->r_udata;
        if (res < 0) { /* the multishot poll died (canceled/EBADF): disarm */
            if (a->inflight) a->inflight--;
            s->r_op = 0; s->r_cb = NULL; s->r_udata = NULL;
            s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
            return;
        }
        if (wcb) wcb(a, fd, u); /* the watcher drains the fd inside this call,
                                 * so the kernel's re-check sees it empty and
                                 * the level-triggered contract holds */
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            struct io_uring_sqe *sqe;
            /* Re-fetch and re-check: the callback may have unwatched this fd
             * or grown a->fds, and re-arming a replaced slot would corrupt
             * whatever now owns it. */
            if (fd >= a->cap || a->fds[fd].r_op != UOP_WATCH)
                return;
            sqe = uaio_sqe(a);
            if (sqe) { s = &a->fds[fd];
                       s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
                       io_uring_prep_poll_multishot(sqe, fd, POLLIN);
                       io_uring_sqe_set_data64(sqe, UD(fd, UOP_WATCH, s->r_gen)); }
        }
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
    /* IPv6 parity with the readiness backend (same branch, same bracket
     * parsing): a host containing ':' is an IPv6 literal, bound with V6ONLY
     * off so a wildcard v6 socket also serves v4-mapped peers. Without this
     * an App configured host "::" made dyn_aio_listen fail -> "Connection
     * refused" on a backend where the same app works -- the drift this
     * file's header forbids. */
    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
        int v6only = 0;
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        {   /* accept "[::1]" bracket spelling; copy for inet_pton */
            const char *h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf)) hl = sizeof(hbuf) - 1;
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
        return fd;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
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
    /* fd < 0 is refused everywhere: uaio_fd_ensure's `fd < a->cap` test would
     * pass for -1 and every slot access below would be a->fds[-1] -- an OOB
     * that reached here as a segfault in dyn_aio_send (cov_net, dyna-net-tcp
     * writing on a conn whose fd a close path had already set to -1). */
    if (!a || listen_fd < 0) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, listen_fd) < 0) return -1;
    s = &a->fds[listen_fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_ACCEPT; s->r_multishot = 1;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD(listen_fd, UOP_ACCEPT, s->r_gen));
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
    if (!a || !cb || fd < 0) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, fd) < 0) return -1;
    s = &a->fds[fd];
    s->r_cb = cb; s->r_udata = udata; s->r_op = UOP_RECV; s->r_multishot = 1;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) return -1;
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = URING_BGID;
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_RECV, s->r_gen));
    return 0;
}

int dyn_aio_send(dyn_aio_t *a, int fd, const void *buf, size_t len, int flags,
                 dyn_aio_cb cb, void *udata)
{
    uaio_fd_t *s;
    uaio_wnode_t *w;
    size_t off = 0;
    (void)flags;

    if (!a || fd < 0) { errno = EINVAL; return -1; }
    if (len == 0) {                /* nothing to carry: complete synchronously */
        if (cb) cb(a, 0, NULL, 0, udata);
        return 0;
    }
    if (!buf) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];

    /* Inline fast path (audit E11-07), mirroring the readiness backend's
     * aio_send_raw: with nothing queued on the fd, a plain non-blocking
     * send() usually finishes a small response right here -- no heap node, no
     * SQE, no completion to reap. Only an EAGAIN remainder (or a busy fd, or
     * a hard error, which reports -1 synchronously like the readiness
     * backend) touches the ring at all. MEASURED, docker glibc arm64
     * (kernel 6.8, gcc 14, -O2): 5000 x 4 KiB sends into a drained
     * socketpair, send+drain+reap per iteration: BEFORE (SQE per send)
     * 0.0017 ms/iter, AFTER (inline fast path) 0.0012 ms/iter (-29%). */
    if (!s->w_cur && !s->w_qhead) {
        for (;;) {
            ssize_t n = send(fd, (const uint8_t *)buf + off, len - off,
                             MSG_NOSIGNAL);
            if (n > 0) { off += (size_t)n; if (off >= len) break; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return -1;              /* hard error: -1/errno, no completion */
        }
        if (off >= len) {
            if (cb) cb(a, (int)len, NULL, 0, udata); /* synchronous completion */
            return 0;
        }
    }

    /* The payload must outlive the send: copy the (inline) remainder into a
     * heap node and put it on the fd's FIFO. It becomes the fd's in-flight
     * write immediately if the fd is idle; otherwise the previous write's
     * completion submits it -- which is what makes same-fd order a guarantee
     * rather than a kernel courtesy (E11-08). */
    w = (uaio_wnode_t *)calloc(1, sizeof(*w));
    if (!w) { errno = ENOMEM; return -1; }
    w->buf = (uint8_t *)malloc(len - off);
    if (!w->buf) { free(w); errno = ENOMEM; return -1; }
    memcpy(w->buf, (const uint8_t *)buf + off, len - off);
    w->kind = UW_SEND;
    w->fd = fd;
    w->len = len - off;             /* `off` stays 0: the copy starts fresh */
    w->cb = cb; w->udata = udata;
    a->inflight++;
    if (s->w_qtail) s->w_qtail->next = w;
    else            s->w_qhead = w;
    s->w_qtail = w;
    uaio_wq_pump(a, fd);            /* no-op when a write is already in flight */
    return 0;
}

int dyn_aio_sendfile(dyn_aio_t *a, int out_fd, int in_fd, off_t offset,
                     size_t len, dyn_aio_cb cb, void *udata)
{
    /* Streamed through the ring as IORING_OP_SPLICE file -> pipe -> socket
     * (audit E11-03), one pipe per transfer released on completion, in chunks
     * of the pipe's capacity (<= 1 MiB). The whole-range blocking pread this
     * replaces paused the loop thread once per serve and peaked at TWO
     * allocations of file size; this peaks at one pipe. The transfer rides
     * the fd's single-flight write queue, so it starts strictly after any
     * buffered prefix (the HTTP header) already queued on the fd.
     *
     * MEASURED, docker glibc arm64 (kernel 6.8, gcc 14, -O2): 20 x 16 MiB
     * sendfile into a drained socketpair: BEFORE (whole-file pread + send)
     * 10.7-13.0 ms/iter (~1400 MiB/s), AFTER (splice stream, 1 MiB chunks)
     * 5.3-5.5 ms/iter (~3000 MiB/s; 2.1-2.4x); peak process RSS over the run
     * 35.2 MiB -> 2.8 MiB, the 2x-file-size transient allocations gone. */
    uaio_fd_t *s;
    uaio_wnode_t *w, *q;

    if (!a || out_fd < 0) { errno = EINVAL; return -1; }
    if (len == 0) {                 /* nothing to stream: complete now, and the
                                     * adapter still takes ownership of in_fd */
        close(in_fd);
        if (cb) cb(a, 0, NULL, 0, udata);
        return 0;
    }
    if (uaio_fd_ensure(a, out_fd) < 0)
        return -1;                  /* in_fd untouched: ownership never moved */
    s = &a->fds[out_fd];

    /* The file slot is single-depth exactly as on the readiness backend (its
     * audit E11-02): a second transfer on the same fd while one is queued or
     * running would have to drop or reorder one of them, and this function
     * owns in_fd -- overwriting would leak it and strand its callback.
     * Buffered sends are NOT refused: they queue ahead and the file follows
     * them; only file-on-file refuses. */
    for (w = s->w_cur; w; w = w->next)
        if (w->kind == UW_FILE) { errno = EBUSY; return -1; }
    for (q = s->w_qhead; q; q = q->next)
        if (q->kind == UW_FILE) { errno = EBUSY; return -1; }

    w = (uaio_wnode_t *)calloc(1, sizeof(*w));
    if (!w) { errno = ENOMEM; return -1; }
    if (uaio_pipe_open(w) < 0) {
        int e = errno;
        free(w);
        errno = e;                  /* in_fd untouched: ownership never moved */
        return -1;
    }
    w->kind = UW_FILE;
    w->fd = out_fd;
    w->in_fd = in_fd;               /* owned from here; closed on completion */
    w->off = offset;
    w->rem = (off_t)len;
    w->cb = cb; w->udata = udata;
    a->inflight++;
    if (s->w_qtail) s->w_qtail->next = w;
    else            s->w_qhead = w;
    s->w_qtail = w;
    uaio_wq_pump(a, out_fd);        /* starts now, or after the prefix drains */
    return 0;
}

int dyn_aio_close(dyn_aio_t *a, int fd)
{
    if (fd >= 0 && fd < a->cap) {
        uaio_fd_t *s = &a->fds[fd];
        uint8_t g = s->r_gen;
        uaio_fd_t dead;
        if (s->r_op && a->inflight) a->inflight--;
        /* Clear BEFORE any callback re-enters (the readiness backend's rule):
         * a completion callback that enqueues another send must land in an
         * empty slot, not on the queue being torn down. */
        dead = *s;
        memset(s, 0, sizeof(*s));
        free(dead.r_msg); /* a datagram arm's heap msghdr. Safe to free here:
                           * the kernel copied the msghdr when it first executed
                           * the arm (inside dyn_aio_recvfrom's eager submit --
                           * no SQPOLL, so execution is not deferred past it),
                           * and multishot shots use that kernel copy. The slot
                           * no longer matches the canceled arm's CQE (gen bump
                           * below), so nothing re-arms from it. */
        /* Closing an armed fd provokes a -ECANCELED CQE for its generation;
         * the slot must no longer match it, or the CQE re-decrements inflight
         * and, once the fd is reused, dispatches to the new op. */
        s->r_gen = (uint8_t)((g + 1) & 0x7f);
        /* close(2) alone does NOT hang up on the peer: every in-flight SQE
         * (an armed multishot recv/accept, a queued send) holds its own
         * kernel reference to the file, so the fd number goes away while the
         * socket lives on -- FIN-less -- until the ring itself is torn down.
         * That is why an idle-swept connection was never seen to close: the
         * peer's recv hung until process exit. IORING_OP_ASYNC_CANCEL with
         * FD|ALL (kernel 5.19+) cancels EVERY request on this fd -- FD alone
         * cancels only the first match, and with a send AND an armed recv
         * both live it was the recv that survived, re-creating exactly the
         * hang this exists to fix. Must be submitted BEFORE close(2) because
         * it resolves the fd number at submit time. */
        {
            struct io_uring_sqe *sqe = uaio_sqe(a);
            if (sqe) {
                io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
                io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0)); /* ignored */
                io_uring_submit(&a->ring);
            }
        }
        close(fd);
        /* Every queued write with a callback completes exactly once, here
         * with an error -- they were never submitted, so no CQE will ever
         * reference them and dropping them silently would strand whatever
         * reference the caller holds per send. */
        {
            uaio_wnode_t *q = dead.w_qhead;
            while (q) {
                uaio_wnode_t *n = q->next;
                uaio_wnode_done(a, fd, q, -ECONNRESET);
                q = n;
            }
        }
        /* The one IN-FLIGHT write is different: its SQE still holds the node
         * pointer, so freeing it here would be a use-after-free at reap time.
         * Release its resources, fire its completion, and leave a dead shell
         * for the CQE (guaranteed to arrive) to free. inflight is released
         * there, keeping dyn_aio_inflight honest until the ring reaps. */
        if (dead.w_cur) {
            uaio_wnode_t *w = dead.w_cur;
            dyn_aio_cb cb = w->cb;
            void *u = w->udata;
            uaio_wnode_free_res(w);
            w->dead = 1;
            w->cb = NULL;
            if (cb) cb(a, -ECONNRESET, NULL, 0, u);
        }
        return 0;
    }
    close(fd);
    return 0;
}

/* Bytes this fd still owes the wire: the in-flight node's remainder plus
 * every queued node -- for a file node, what has not reached the socket yet
 * (file remainder + pipe contents; the inline prefix a plain send already
 * handed the kernel is not owed). This is what a streaming caller bounds
 * against; without it a flooding sender grows the queue without limit.
 * (This symbol was missing outright -- the uring build never linked.) */
size_t dyn_aio_queued(const dyn_aio_t *a, int fd)
{
    const uaio_fd_t *s;
    const uaio_wnode_t *w;
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

/* NULL on this backend: there is no dyn_evloop behind a ring. Its only caller
 * is the file watcher's DYN_EV_VNODE registration, which must REFUSE rather
 * than store the NULL -- it had no check, so this symbol being absent is what
 * kept the whole backend from linking. */
struct dyn_evloop *dyn_aio_evloop(dyn_aio_t *a) { (void)a; return NULL; }
/* io_uring has IORING_OP_TIMEOUT, but a periodic wakeup is simpler as the
 * timerfd read armed below: it self-rearms from its own completion and needs
 * no second completion tag. Callers: every dyn_net_on_drain hook (the App's
 * idle-timeout sweep among them). */
/* IORING_OP_CONNECT. The sockaddr rides a heap context (uaio_conn_t) because
 * the kernel reads it after submit -- a stack copy would be a use-after-free.
 * Returns the fd immediately like the readiness backend; the callback fires
 * with 0 or -errno, and the fd is the caller's to close either way. */
static int uaio_connect_on(dyn_aio_t *a, int fd,
                           const struct sockaddr_storage *sa,
                           socklen_t salen, dyn_aio_cb cb, void *udata)
{
    struct io_uring_sqe *sqe;
    uaio_conn_t *cc;

    if (fd < 0) { errno = EINVAL; return -1; }
    cc = (uaio_conn_t *)malloc(sizeof(*cc));
    if (!cc) { errno = ENOMEM; return -1; }
    memcpy(&cc->sa, sa, sizeof(*sa));
    cc->salen = salen; cc->fd = fd; cc->cb = cb; cc->udata = udata;

    /* Mark the fd slot so the CONN_BIT completion can tell a LIVE connect
     * from a stale one (see uaio_dispatch); r_cb/r_udata are NOT written --
     * dyn_aio_cancel must not mistake a connect for a cancelable arm. */
    if (uaio_fd_ensure(a, fd) < 0) { free(cc); errno = ENOMEM; return -1; }
    a->fds[fd].r_op = UOP_CONNECT;
    a->fds[fd].r_gen = (uint8_t)((a->fds[fd].r_gen + 1) & 0x7f);
    cc->gen = a->fds[fd].r_gen;

    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        free(cc);
        a->fds[fd].r_op = 0;
        if (a->inflight) a->inflight--;
        errno = EAGAIN;
        return -1;
    }
    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&cc->sa, cc->salen);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)cc | CONN_BIT);
    /* submit now: connect is issued from timer and promise callbacks too, not
     * only from a drain -- the same reason dyn_aio_send submits eagerly. */
    io_uring_submit(&a->ring);
    return fd;
}

static int uaio_connect_sa(dyn_aio_t *a, const struct sockaddr_storage *sa,
                           socklen_t salen, int fam,
                           dyn_aio_cb cb, void *udata)
{
    int fd;

    fd = socket(fam, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    dyn_net_set_nonblock(fd);
    if (fam != AF_UNIX) dyn_net_set_nodelay(fd);
    /* uaio_connect_on leaves the fd open on failure: the ownership split the
     * readiness backend documents -- here the caller never saw the fd, so WE
     * close it. */
    if (uaio_connect_on(a, fd, sa, salen, cb, udata) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* By-address connect for callers holding a resolved sockaddr (the proxy's
 * upstream path). Same contract as the readiness backend: the fd comes back
 * immediately, `cb` fires with 0 or -errno, fd is the caller's to close.
 * (This symbol was missing outright -- the uring build never linked.) */
int dyn_aio_connect_addr(dyn_aio_t *a, const struct sockaddr_storage *sa,
                         socklen_t salen, int fam,
                         dyn_aio_cb cb, void *udata)
{
    if (!a || !sa) { errno = EINVAL; return -1; }
    return uaio_connect_sa(a, sa, salen, fam, cb, udata);
}

/* ---- connect() by NAME: resolution off the loop thread (audit E11-04's
 * Linux half -- the readiness backend's port, same contract) --------------
 *
 * getaddrinfo on the reactor thread froze every connection the loop held
 * while one slow resolver answered. The lookup now rides dyn_aio_offload
 * (work() is getaddrinfo only, done() arms the connect), and the fd handed
 * back synchronously is a DUAL-STACK socket (AF_INET6, V6ONLY=0) so the
 * fd-return contract holds before the family is known -- v4 answers connect
 * as v4-mapped. Failed answers retry in resolver order on the same fd number
 * (dyn_aio_cand_t: the interposed completion refreshes the socket via dup2,
 * because a failed connect leaves a socket unusable). The fd's slot is
 * RESERVED at submit so a close during the resolve window retires the
 * connect instead of cross-wiring a reused fd number. dyn_aio_offload
 * ALWAYS COMPLETES, so the inline fallback is byte-for-byte the old blocking
 * behavior, including its synchronous -1 on a failed lookup. */
typedef struct {
    dyn_aio_t *aio;
    char *host;             /* the caller's string may not outlive us */
    uint16_t port;
    dyn_aio_cb cb;
    void *udata;
    int fd;                 /* dual-stack socket, opened before submit */
    dyn_aio_cand_t *cand;   /* heap: outlives the resolve into the retry chain */
    int err;                /* 0, or the errno this failure reports */
    int past_return;        /* pooled: failures arrive as the callback only */
} uaio_resolve_t;

static void uaio_resolve_work(void *p)   /* POOL THREAD: no JS, no aio state */
{
    uaio_resolve_t *c = (uaio_resolve_t *)p;
    if (dyn_aio_cand_collect(c->host, c->port, c->cand) != 0)
        c->err = EINVAL;
}

/* The interposed completion of a hostname connect: fire the user's callback,
 * or dial the next resolver answer on the same fd number. */
static void uaio_cand_done(dyn_aio_t *a, int res, const uint8_t *buf,
                           unsigned n, void *ud)
{
    dyn_aio_cand_t *cand = (dyn_aio_cand_t *)ud;
    (void)buf; (void)n;

    if (res != 0 && cand->i + 1 < cand->n) {
        cand->i++;
        if (dyn_aio_cand_refresh_fd(cand) == 0 &&
            uaio_connect_on(a, cand->fd, &cand->addr[cand->i],
                            cand->len[cand->i], uaio_cand_done, cand) >= 0)
            return;               /* the retry's own completion settles the chain */
        /* the retry could not even be armed: report the original failure --
         * the fd is dead either way and the caller's error path closes it */
    }
    if (cand->cb)
        cand->cb(a, res, NULL, 0, cand->udata);
    free(cand);
}

static void uaio_resolve_done(void *p)   /* LOOP THREAD */
{
    uaio_resolve_t *c = (uaio_resolve_t *)p;
    dyn_aio_t *a = c->aio;

    /* The slot was RESERVED at submit (see the readiness backend's identical
     * rule): if the caller closed during the resolve window the reservation
     * is gone and the fd number may be in reuse -- connecting would
     * cross-wire an unrelated descriptor, so retire silently. */
    if (c->fd < 0 || c->fd >= a->cap || a->fds[c->fd].r_op != UOP_CONNECT ||
        a->fds[c->fd].r_cb != c->cb || a->fds[c->fd].r_udata != c->udata) {
        free(c->cand);
        free(c->host);
        free(c);
        return;
    }

    if (c->err == 0) {
        /* Ownership of `cand` moves to the completion chain (uaio_cand_done);
         * it fires the user's callback exactly once and frees itself. */
        if (uaio_connect_on(a, c->fd, &c->cand->addr[0], c->cand->len[0],
                            uaio_cand_done, c->cand) < 0) {
            c->err = errno ? errno : EIO;  /* armed nothing: report it */
        } else {
            c->cand = NULL;
        }
    }
    if (c->past_return) {
        /* The caller holds the fd and is owed the always-completes contract:
         * success fires from the connect CQE, failure fires here. The fd
         * stays OPEN -- the caller's res < 0 path closes it. */
        if (c->err && c->cb)
            c->cb(a, -c->err, NULL, 0, c->udata);
        free(c->cand);
        free(c->host);
        free(c);
    } else if (c->err) {
        /* Inline run, still inside dyn_aio_connect: the caller has NOT seen
         * the fd, so close it here and fire no callback. */
        close(c->fd);
    }
    /* Inline success: the connect is armed and its completion deferred to
     * the CQE; dyn_aio_connect returns the fd and frees the ctx. */
}

int dyn_aio_connect(dyn_aio_t *a, const char *host, uint16_t port,
                    dyn_aio_cb cb, void *udata)
{
    struct addrinfo hints, *res = NULL;
    struct sockaddr_storage sa;
    socklen_t salen = 0;
    int fam = AF_INET, fd, r;
    char portstr[16];
    uaio_resolve_t *c;
#ifdef IPV6_V6ONLY
    int v6only = 0;
#endif

    if (!a || !host) { errno = EINVAL; return -1; }

    /* Fast path first: AI_NUMERICHOST never invokes the name resolution
     * service, so an address literal cannot block and keeps today's
     * synchronous family-true fd. */
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

    /* Real hostname: the lookup can block for seconds, so it must not run on
     * this (often the reactor) thread. Open the dual-stack socket NOW so the
     * fd-return contract holds before the family is known. */
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        /* No IPv6 here: keep the synchronous resolve (there is no safe async
         * alternative without a dual-stack fd to return -- see the readiness
         * backend's block comment for the full rationale). */
        if (dyn_aio_resolve(host, port, &sa, &salen, &fam) != 0) {
            errno = EINVAL;
            return -1;
        }
        return dyn_aio_connect_addr(a, &sa, salen, fam, cb, udata);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef IPV6_V6ONLY
    /* 0 is the platform default; set it so a sysctl cannot flip this socket
     * to v6-only and quietly break every AF_INET answer. */
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
    dyn_net_set_nonblock(fd);
    dyn_net_set_nodelay(fd);

    c = (uaio_resolve_t *)calloc(1, sizeof(*c));
    if (c) {
        c->host = strdup(host);
        c->cand = (dyn_aio_cand_t *)calloc(1, sizeof(*c->cand));
    }
    if (!c || !c->host || !c->cand) {
        /* Nothing was submitted, so no callback is owed. */
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

    /* Reserve the fd's slot now (ownership marker only; the arm does the
     * registration and the inflight count): a dyn_aio_close in the resolve
     * window must retire THIS op, or a reused fd number gets connected. */
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
        /* Pool refused: work+done already ran inline on THIS thread -- the
         * old blocking behavior exactly, down to the synchronous -1/errno on
         * a failed lookup (done() closed the fd on that path). */
        if (c->err) {
            int e = c->err;
            free(c->cand);
            free(c->host);
            free(c);
            a->fds[fd].r_op = 0;      /* retire the reservation */
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
    if (r < 0) {            /* !a rejected by the offload; nothing ran */
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
    /* Pooled: done() runs on a later drain, after the caller has the fd. From
     * here every outcome -- including a failed lookup -- arrives as the
     * callback, never a return code. */
    c->past_return = 1;
    return fd;
}

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

/* Same SQE-lifetime shape as dyn_aio_connect, minus the resolution: a unix
 * path IS the address. sun_path must be NUL-terminated within its bound, so an
 * over-long path is refused here rather than silently truncated into a
 * DIFFERENT path -- truncation is how a client connects to the wrong socket. */
int dyn_aio_unix_connect(dyn_aio_t *a, const char *path, dyn_aio_cb cb, void *ud)
{
    struct sockaddr_storage ss;
    struct sockaddr_un *un = (struct sockaddr_un *)&ss;
    size_t n;

    if (!a || !path) { errno = EINVAL; return -1; }
    n = strlen(path);
    if (n >= sizeof(un->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(&ss, 0, sizeof(ss));
    un->sun_family = AF_UNIX;
    memcpy(un->sun_path, path, n + 1);
    return uaio_connect_sa(a, &ss, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1),
                           AF_UNIX, cb, ud);
}

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

/* Multishot recvmsg into the provided-buffer ring, peer address included (it
 * comes back inside the selected buffer -- see uaio_dgram_t). The contract is
 * the readiness backend's: multishot by default (stays armed), the callback's
 * buf/peer are BORROWED for the call, and a second call on an armed socket
 * just replaces the callback. Eager submit like dyn_aio_accept: a datagram
 * server arms from JS, not from inside a drain, and an unsubmitted SQE would
 * leave the loop waiting on an eventfd that never fires. */
int dyn_aio_recvfrom(dyn_aio_t *a, int fd, dyn_aio_dgram_cb cb, void *ud)
{
    uaio_fd_t *s;
    uaio_dgram_t *dg;

    if (!a || !cb || fd < 0) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->r_op == UOP_RECVFROM) { /* already armed: swap the handler, keep
                                    * the arm and its heap context */
        s->r_cb = (dyn_aio_cb)(void *)cb;
        s->r_udata = ud;
        return 0;
    }
    if (s->r_op) { errno = EBUSY; return -1; } /* one read interest per fd */
    dg = (uaio_dgram_t *)calloc(1, sizeof(*dg));
    if (!dg) { errno = ENOMEM; return -1; }
    dg->mh.msg_name = &dg->ss;
    dg->mh.msg_namelen = sizeof(dg->ss); /* space reserved in each buffer */
    dg->mh.msg_iov = &dg->iov;
    dg->mh.msg_iovlen = 1;               /* base/len 0: buffer select picks */
    s->r_cb = (dyn_aio_cb)(void *)cb;
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

/* Level-triggered "fd is readable" for an fd the adapter does not own the
 * protocol of (the Watcher's inotify fd): IORING_OP_POLL_MULTISHOT, the same
 * arm uaio_pool_arm uses for the offload pool's wake fd. The kernel re-fires
 * the poll while the condition holds, and the callback is expected to DRAIN
 * the fd (the watcher does), which is what makes the behavior level-triggered
 * rather than one-shot. */
int dyn_aio_watch_fd(dyn_aio_t *a, int fd,
                     void (*cb)(dyn_aio_t *, int, void *), void *ud)
{
    struct io_uring_sqe *sqe;
    uaio_fd_t *s;

    if (!a || !cb || fd < 0) { errno = EINVAL; return -1; }
    if (uaio_fd_ensure(a, fd) < 0)
        return -1;
    s = &a->fds[fd];
    if (s->r_op == UOP_WATCH) { /* already watched: swap the handler */
        s->r_cb = (dyn_aio_cb)(void *)cb;
        s->r_udata = ud;
        return 0;
    }
    if (s->r_op) { errno = EBUSY; return -1; }
    s->r_cb = (dyn_aio_cb)(void *)cb;
    s->r_udata = ud;
    s->r_op = UOP_WATCH;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f);
    a->inflight++;
    sqe = uaio_sqe(a);
    if (!sqe) {
        s->r_op = 0; s->r_cb = NULL; s->r_udata = NULL;
        if (a->inflight) a->inflight--;
        errno = EAGAIN;
        return -1;
    }
    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    io_uring_sqe_set_data64(sqe, UD(fd, UOP_WATCH, s->r_gen));
    io_uring_submit(&a->ring); /* armed from JS: must not wait for a drain */
    return 0;
}

/* Detach a dyn_aio_watch_fd registration WITHOUT closing the fd (the watcher
 * owns it). Cancels the armed multishot poll by its exact user_data so the
 * -ECANCELED CQE lands on a disarmed slot. */
int dyn_aio_unwatch_fd(dyn_aio_t *a, int fd)
{
    uaio_fd_t *s;
    uint64_t ud;
    struct io_uring_sqe *sqe;

    if (!a || fd < 0 || fd >= a->cap)
        return -1;
    s = &a->fds[fd];
    if (s->r_op != UOP_WATCH)
        return 0; /* nothing armed: idempotent, like dyn_evloop_del */
    ud = UD(fd, UOP_WATCH, s->r_gen);
    s->r_op = 0; s->r_cb = NULL; s->r_udata = NULL;
    if (a->inflight) a->inflight--;
    s->r_gen = (uint8_t)((s->r_gen + 1) & 0x7f); /* ignore the -ECANCELED */
    sqe = uaio_sqe(a);
    if (sqe) {
        io_uring_prep_cancel64(sqe, ud, 0);
        io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0)); /* reaped, ignored */
        io_uring_submit(&a->ring);
    }
    return 0;
}

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
    io_uring_sqe_set_data64(sqe, UD(a->tfd, UOP_TIMER, 0));
    /* SUBMIT the arm: an SQE sitting in the submission ring arms nothing, and
     * on a quiet loop nothing else enters the ring -- so the timer never fired
     * and the drain hooks (deadlines, idle sweeps) stayed dead until traffic
     * happened to submit something. Measured: poll(backend_fd) woke 0/6
     * windows before this line, 6/6 after. */
    io_uring_submit(&a->ring);
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
        uint8_t g;
        struct io_uring_sqe *sqe;
        if (s->r_cb != cb || s->r_udata != udata)
            continue;
        if (s->r_op != UOP_ACCEPT && s->r_op != UOP_RECV &&
            s->r_op != UOP_RECVFROM && s->r_op != UOP_WATCH)
            continue;
        g = s->r_gen;
        ud = UD(fd, s->r_op, g);     /* match the CURRENTLY armed generation */
        s->r_cb = NULL; s->r_udata = NULL; s->r_op = 0; s->r_multishot = 0;
        if (a->inflight) a->inflight--;
        if (s->r_msg) { /* a datagram arm's heap msghdr: freeing is safe for
                        * the reason dyn_aio_close documents -- the kernel
                        * copied it when the arm first executed */
            free(s->r_msg);
            s->r_msg = NULL;
        }
        s->r_gen = (uint8_t)((g + 1) & 0x7f); /* ignore the -ECANCELED this provokes */
        sqe = uaio_sqe(a);
        if (sqe) {
            io_uring_prep_cancel64(sqe, ud, 0);
            io_uring_sqe_set_data64(sqe, UD(fd, UOP_CLOSE, 0)); /* reaped, ignored */
            io_uring_submit(&a->ring);
        }
        n++;
    }
    return n;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_IO_URING && __linux__ */
