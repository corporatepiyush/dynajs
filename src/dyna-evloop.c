/*
 * dyna-evloop -- portable readiness reactor (see dyna-evloop.h).
 *
 * A per-fd registration table (indexed by the fd number, which the kernel keeps
 * small and dense) holds {callback, udata, interest}. The active backend mirrors
 * that interest into its kernel object. Level-triggered readiness means a
 * spurious wakeup is always safe: callbacks treat readiness as advisory and
 * loop on EAGAIN, so a stale event on a reused fd is a harmless no-op.
 */
#include "dyna-evloop.h"

#ifdef CONFIG_NATIVE_MODULES

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define DYN_EV_KQUEUE 1
#include <sys/event.h>
#include <sys/types.h>
#elif defined(__linux__)
/* The evloop is the portable READINESS core (used on macOS and as the Linux
 * fallback). The real io_uring path is the completion-model reactor in
 * dyna-http.c, not a poll-mode shim here -- so Linux uses epoll here even
 * when CONFIG_IO_URING is set. */
#define DYN_EV_EPOLL 1
#include <sys/epoll.h>
#include <sys/timerfd.h>
#else
#define DYN_EV_POLL 1
#include <poll.h>
#endif

typedef struct {
    dyn_ev_cb cb;
    void *udata;
    int interest; /* mask of DYN_EV_*; 0 => slot free */
} dyn_ev_reg;

struct dyn_evloop {
    int backend_fd;   /* kqueue/epoll fd; -1 for the poll(2) backend */
    dyn_ev_reg *regs; /* indexed by fd */
    int cap;          /* length of regs[] */
    size_t nreg;      /* number of slots with interest != 0 */
#ifdef DYN_EV_POLL
    struct pollfd *pfds; /* rebuilt each poll() from the table */
    int pfds_cap;
#endif
#ifdef DYN_EV_EPOLL
    int timer_fd;     /* timerfd for the periodic wakeup; -1 when disarmed */
#endif
};

/* Grow regs[] so index `fd` is valid; new slots are zeroed (interest 0). */
static int reg_ensure(dyn_evloop_t *lp, int fd)
{
    int nc;
    dyn_ev_reg *nr;

    if (fd < lp->cap)
        return 0;
    /* Floor at 4096 (~96 KB): a high first fd would otherwise walk
       64,128,256,... reallocating and zeroing at each step. */
    nc = lp->cap ? lp->cap * 2 : 4096;
    while (nc <= fd)
        nc *= 2;
    nr = (dyn_ev_reg *)realloc(lp->regs, (size_t)nc * sizeof(*nr));
    if (!nr)
        return -1;
    memset(nr + lp->cap, 0, (size_t)(nc - lp->cap) * sizeof(*nr));
    lp->regs = nr;
    lp->cap = nc;
    return 0;
}

dyn_evloop_t *dyn_evloop_new(void)
{
    dyn_evloop_t *lp = (dyn_evloop_t *)calloc(1, sizeof(*lp));
    if (!lp)
        return NULL;
    lp->backend_fd = -1;
    /* CLOEXEC: os.exec() forks. An inherited backend fd leaks across exec, or
       shares the interest set with the parent if the child never execs.
       kqueue() takes no flags, hence the fcntl. */
#if defined(DYN_EV_KQUEUE)
    lp->backend_fd = kqueue();
    if (lp->backend_fd < 0) {
        free(lp);
        return NULL;
    }
    fcntl(lp->backend_fd, F_SETFD, FD_CLOEXEC);
#elif defined(DYN_EV_EPOLL)
    lp->backend_fd = epoll_create1(EPOLL_CLOEXEC);
    if (lp->backend_fd < 0) {
        free(lp);
        return NULL;
    }
    lp->timer_fd = -1;
#endif
    return lp;
}

void dyn_evloop_free(dyn_evloop_t *lp)
{
    if (!lp)
        return;
#ifdef DYN_EV_EPOLL
    if (lp->timer_fd >= 0)
        close(lp->timer_fd);
#endif
    if (lp->backend_fd >= 0)
        close(lp->backend_fd);
    free(lp->regs);
#ifdef DYN_EV_POLL
    free(lp->pfds);
#endif
    free(lp);
}

size_t dyn_evloop_count(const dyn_evloop_t *lp)
{
    return lp->nreg;
}

int dyn_evloop_backend_fd(const dyn_evloop_t *lp)
{
    return lp->backend_fd; /* kqueue/epoll fd, or -1 for the poll(2) fallback */
}

/* The ident/fd the periodic timer reports as. Not a real descriptor on kqueue,
 * so it is deliberately outside any plausible fd range and never indexes regs[]. */
#define DYN_EV_TIMER_IDENT 0x7ffffffe

int dyn_evloop_set_timer(dyn_evloop_t *lp, unsigned period_ms)
{
#if defined(DYN_EV_KQUEUE)
    struct kevent kev;
    if (!period_ms) {
        EV_SET(&kev, DYN_EV_TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(lp->backend_fd, &kev, 1, NULL, 0, NULL);   /* ENOENT is fine */
        return 0;
    }
    /* fflags 0: milliseconds is EVFILT_TIMER's default unit on both macOS and
     * the BSDs, and NOTE_MSECONDS is not in every SDK. No fd is involved -- the
     * timer is an ident on the kqueue itself. */
    EV_SET(&kev, DYN_EV_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE,
           0, (int64_t)period_ms, NULL);
    return kevent(lp->backend_fd, &kev, 1, NULL, 0, NULL) < 0 ? -1 : 0;
#elif defined(DYN_EV_EPOLL)
    struct itimerspec its;
    struct epoll_event ev;
    if (!period_ms) {
        if (lp->timer_fd >= 0) {
            epoll_ctl(lp->backend_fd, EPOLL_CTL_DEL, lp->timer_fd, NULL);
            close(lp->timer_fd);
            lp->timer_fd = -1;
        }
        return 0;
    }
    if (lp->timer_fd < 0) {
        lp->timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                      TFD_NONBLOCK | TFD_CLOEXEC);
        if (lp->timer_fd < 0)
            return -1;
        ev.events = EPOLLIN;
        ev.data.fd = lp->timer_fd;
        if (epoll_ctl(lp->backend_fd, EPOLL_CTL_ADD, lp->timer_fd, &ev) < 0) {
            close(lp->timer_fd);
            lp->timer_fd = -1;
            return -1;
        }
    }
    its.it_interval.tv_sec = period_ms / 1000u;
    its.it_interval.tv_nsec = (long)(period_ms % 1000u) * 1000000L;
    its.it_value = its.it_interval;
    return timerfd_settime(lp->timer_fd, 0, &its, NULL) < 0 ? -1 : 0;
#else
    (void)lp; (void)period_ms;
    errno = ENOTSUP;   /* poll(2) has no timer primitive; caller drives its own */
    return -1;
#endif
}

/* --- backend: apply a change of interest for `fd` (old = regs[fd].interest,
 *     already updated to `neu` by the caller before calling this) --- */

#if defined(DYN_EV_KQUEUE)
static int kq_filter(int kq, int fd, int filter, int add, void *udata)
{
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)fd, (int16_t)filter,
           add ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, udata);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        if (!add && errno == ENOENT)
            return 0; /* filter was not registered: not an error */
        return -1;
    }
    return 0;
}

/* A vnode watch carries its interest in fflags, not in the filter, so it needs
 * its own EV_SET rather than kq_filter's. NOTE_LINK is what reports a child
 * added to or removed from a watched DIRECTORY. */
static int kq_vnode(int kq, int fd, int add, void *udata)
{
    struct kevent kev;
    unsigned ff = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND |
                  NOTE_ATTRIB | NOTE_LINK;
    EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE,
           add ? (EV_ADD | EV_ENABLE | EV_CLEAR) : EV_DELETE, ff, 0, udata);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        if (!add && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}

static int backend_apply(dyn_evloop_t *lp, int fd, int old, int neu, void *udata)
{
    int kq = lp->backend_fd, rc = 0;
    if ((neu & DYN_EV_READ) != (old & DYN_EV_READ))
        rc |= kq_filter(kq, fd, EVFILT_READ, neu & DYN_EV_READ, udata);
    if ((neu & DYN_EV_WRITE) != (old & DYN_EV_WRITE))
        rc |= kq_filter(kq, fd, EVFILT_WRITE, neu & DYN_EV_WRITE, udata);
    if ((neu & DYN_EV_VNODE) != (old & DYN_EV_VNODE))
        rc |= kq_vnode(kq, fd, neu & DYN_EV_VNODE, udata);
    return rc;
}
#elif defined(DYN_EV_EPOLL)
static int backend_apply(dyn_evloop_t *lp, int fd, int old, int neu, void *udata)
{
    struct epoll_event ev;
    int op;
    (void)udata;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    if (neu & DYN_EV_READ)
        ev.events |= EPOLLIN;
    if (neu & DYN_EV_WRITE)
        ev.events |= EPOLLOUT;
    if (!old && neu)
        op = EPOLL_CTL_ADD;
    else if (old && !neu)
        op = EPOLL_CTL_DEL;
    else
        op = EPOLL_CTL_MOD;
    if (epoll_ctl(lp->backend_fd, op, fd, &ev) < 0) {
        if (op == EPOLL_CTL_DEL && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}
#else /* DYN_EV_POLL: nothing to mirror; the pollfd set is rebuilt each poll */
static int backend_apply(dyn_evloop_t *lp, int fd, int old, int neu, void *ud)
{
    (void)lp; (void)fd; (void)old; (void)neu; (void)ud;
    return 0;
}
#endif

int dyn_evloop_add(dyn_evloop_t *lp, int fd, int interest, dyn_ev_cb cb,
                   void *udata)
{
    int old;
    if (fd < 0 || reg_ensure(lp, fd) < 0)
        return -1;
#if !defined(DYN_EV_KQUEUE)
    /* REFUSE rather than ignore. Silently dropping the bit would register the
       directory for nothing and the caller would wait forever for an event
       that cannot arrive -- indistinguishable from "the tree never changed". */
    if (interest & DYN_EV_VNODE) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    old = lp->regs[fd].interest;
    lp->regs[fd].cb = cb;
    lp->regs[fd].udata = udata;
    lp->regs[fd].interest = interest;
    if (backend_apply(lp, fd, old, interest, udata) < 0) {
        lp->regs[fd].interest = old;
        return -1;
    }
    if (!old && interest)
        lp->nreg++;
    else if (old && !interest)
        lp->nreg--;
    return 0;
}

int dyn_evloop_mod(dyn_evloop_t *lp, int fd, int interest)
{
    int old;
    if (fd < 0 || fd >= lp->cap)
        return -1;
    old = lp->regs[fd].interest;
    lp->regs[fd].interest = interest;
    if (backend_apply(lp, fd, old, interest, lp->regs[fd].udata) < 0) {
        lp->regs[fd].interest = old;
        return -1;
    }
    if (!old && interest)
        lp->nreg++;
    else if (old && !interest)
        lp->nreg--;
    return 0;
}

int dyn_evloop_del(dyn_evloop_t *lp, int fd)
{
    if (fd < 0 || fd >= lp->cap || !lp->regs[fd].interest)
        return 0;
    return dyn_evloop_mod(lp, fd, 0);
}

/* --- one wait + dispatch iteration --- */

#define DYN_EV_BATCH 1024

#if defined(DYN_EV_KQUEUE)
int dyn_evloop_poll(dyn_evloop_t *lp, int timeout_ms)
{
    struct kevent evs[DYN_EV_BATCH];
    struct timespec ts, *pts = NULL;
    int n, i, dispatched = 0;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    n = kevent(lp->backend_fd, NULL, 0, evs, DYN_EV_BATCH, pts);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        int fd = (int)evs[i].ident;
        int mask = 0;
        dyn_ev_reg *r;
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        if (!r->interest || !r->cb)
            continue; /* deleted by an earlier callback in this batch */
        if (evs[i].filter == EVFILT_READ)
            mask |= DYN_EV_READ;
        else if (evs[i].filter == EVFILT_WRITE)
            mask |= DYN_EV_WRITE;
        else if (evs[i].filter == EVFILT_VNODE)
            mask |= DYN_EV_VNODE;
        /* EV_EOF on a vnode means the file was deleted, which the watcher
           reports as an event -- not the error EOF means on a socket. */
        if ((evs[i].flags & EV_EOF) && evs[i].filter != EVFILT_VNODE)
            mask |= DYN_EV_ERROR;
        r->cb(lp, fd, mask, r->udata);
        dispatched++;
    }
    return dispatched;
}
#elif defined(DYN_EV_EPOLL)
int dyn_evloop_poll(dyn_evloop_t *lp, int timeout_ms)
{
    struct epoll_event evs[DYN_EV_BATCH];
    int n, i, dispatched = 0;

    n = epoll_wait(lp->backend_fd, evs, DYN_EV_BATCH, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        int fd = evs[i].data.fd;
        int mask = 0;
        dyn_ev_reg *r;
        if (fd >= 0 && fd == lp->timer_fd) {
            /* MUST be read: a timerfd is level-triggered, so leaving the
             * expiration count unread spins epoll_wait at 100% CPU. The value
             * is discarded -- the wakeup itself is the whole point. */
            uint64_t ticks;
            while (read(lp->timer_fd, &ticks, sizeof(ticks)) == sizeof(ticks))
                ;
            continue;
        }
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        if (!r->interest || !r->cb)
            continue;
        if (evs[i].events & (EPOLLIN | EPOLLHUP))
            mask |= DYN_EV_READ;
        if (evs[i].events & EPOLLOUT)
            mask |= DYN_EV_WRITE;
        if (evs[i].events & (EPOLLERR | EPOLLHUP))
            mask |= DYN_EV_ERROR;
        r->cb(lp, fd, mask, r->udata);
        dispatched++;
    }
    return dispatched;
}
#else /* DYN_EV_POLL */
int dyn_evloop_poll(dyn_evloop_t *lp, int timeout_ms)
{
    int i, nfds = 0, n, dispatched = 0;

    if (lp->pfds_cap < lp->cap) {
        struct pollfd *np =
            (struct pollfd *)realloc(lp->pfds, (size_t)lp->cap * sizeof(*np));
        if (!np)
            return -1;
        lp->pfds = np;
        lp->pfds_cap = lp->cap;
    }
    for (i = 0; i < lp->cap; i++) {
        if (!lp->regs[i].interest)
            continue;
        lp->pfds[nfds].fd = i;
        lp->pfds[nfds].events = 0;
        lp->pfds[nfds].revents = 0;
        if (lp->regs[i].interest & DYN_EV_READ)
            lp->pfds[nfds].events |= POLLIN;
        if (lp->regs[i].interest & DYN_EV_WRITE)
            lp->pfds[nfds].events |= POLLOUT;
        nfds++;
    }
    n = poll(lp->pfds, (nfds_t)nfds, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < nfds && dispatched < n; i++) {
        int fd = lp->pfds[i].fd;
        short re = lp->pfds[i].revents;
        int mask = 0;
        dyn_ev_reg *r;
        if (!re)
            continue;
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        if (!r->interest || !r->cb)
            continue;
        if (re & (POLLIN | POLLHUP))
            mask |= DYN_EV_READ;
        if (re & POLLOUT)
            mask |= DYN_EV_WRITE;
        if (re & (POLLERR | POLLHUP | POLLNVAL))
            mask |= DYN_EV_ERROR;
        r->cb(lp, fd, mask, r->udata);
        dispatched++;
    }
    return dispatched;
}
#endif

#endif /* CONFIG_NATIVE_MODULES */
