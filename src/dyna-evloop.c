#include "dyna-evloop.h"
#include "core/dyn-common.h"
#include "dyna-io.h"
#include "core/dyn-timer.h"

#ifdef CONFIG_NATIVE_MODULES

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define DYN_EV_KQUEUE 1
#include <sys/event.h>
#include <sys/types.h>
#elif defined(__linux__)
#define DYN_EV_EPOLL 1
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <time.h>
#else
#define DYN_EV_POLL 1
#include <poll.h>
#endif

typedef struct {
    dyn_ev_cb cb;
    void* udata;
    int interest;
    uint32_t gen;
#ifdef DYN_EV_KQUEUE
    int32_t pend[2];
    uint8_t inkern[2];
#endif
} dyn_ev_reg;

struct dyn_evloop {
    int backend_fd;
    dyn_ev_reg* regs;
    int cap;
    size_t nreg;
    int timer_fired;
#ifdef DYN_EV_KQUEUE
    struct kevent* chg;
    int nchg;
    int chg_cap;
    int nested;
    int linked;
    int deadline_armed;
    struct dyn_evloop* pend_next;
    dyn_ev_cb user_cb;
    void* user_ud;
#endif
#ifdef DYN_EV_POLL
    struct pollfd* pfds;
    uint32_t* pgens;
    int pfds_cap;
#endif
#ifdef DYN_EV_EPOLL
    int timer_fd;
#endif
};

#ifdef DYN_EV_KQUEUE
static void kq_unlink(dyn_evloop_t* lp);
#endif

static int reg_ensure(dyn_evloop_t* lp, int fd)
{
    int nc;
    dyn_ev_reg* nr;

    if (fd < 0 || (unsigned)fd >= (1u << 20))
        return -1;
    if (fd < lp->cap)
        return 0;
    nc = lp->cap ? lp->cap * 2 : 4096;
    while (nc <= fd)
        nc *= 2;
    nr = (dyn_ev_reg*)realloc(lp->regs, (size_t)nc * sizeof(*nr));
    if (!nr)
        return -1;
    memset(nr + lp->cap, 0, (size_t)(nc - lp->cap) * sizeof(*nr));
    lp->regs = nr;
    lp->cap = nc;
    return 0;
}

dyn_evloop_t* dyn_evloop_new(void)
{
    dyn_evloop_t* lp = (dyn_evloop_t*)calloc(1, sizeof(*lp));
    if (!lp)
        return NULL;
    lp->backend_fd = -1;
#if defined(DYN_EV_KQUEUE)
    lp->backend_fd = kqueue();
    if (lp->backend_fd < 0) {
        free(lp);
        return NULL;
    }
    dyn_fd_set_cloexec(lp->backend_fd);
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

void dyn_evloop_free(dyn_evloop_t* lp)
{
    if (!lp)
        return;
#ifdef DYN_EV_EPOLL
    if (lp->timer_fd >= 0)
        close(lp->timer_fd);
#endif
#ifdef DYN_EV_KQUEUE
    kq_unlink(lp);
    free(lp->chg);
#endif
    if (lp->backend_fd >= 0)
        close(lp->backend_fd);
    free(lp->regs);
#ifdef DYN_EV_POLL
    free(lp->pfds);
    free(lp->pgens);
#endif
    free(lp);
}

size_t dyn_evloop_count(const dyn_evloop_t* lp)
{
    return lp->nreg;
}

#define DYN_EV_USER_IDENT 0x7ffffffc


int dyn_evloop_user_set(dyn_evloop_t* lp, dyn_ev_cb cb, void* udata)
{
#ifdef DYN_EV_KQUEUE
    struct kevent kev;
    EV_SET(&kev, DYN_EV_USER_IDENT, EVFILT_USER, cb ? EV_ADD | EV_CLEAR : EV_DELETE, 0, 0, NULL);
    if (kevent(lp->backend_fd, &kev, 1, NULL, 0, NULL) < 0 && cb)
        return -1;
    lp->user_cb = cb;
    lp->user_ud = udata;
    return 0;
#else
    (void)lp;
    (void)cb;
    (void)udata;
    errno = ENOTSUP;
    return -1;
#endif
}

void dyn_evloop_user_trigger(dyn_evloop_t* lp)
{
#ifdef DYN_EV_KQUEUE
    struct kevent kev;
    EV_SET(&kev, DYN_EV_USER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(lp->backend_fd, &kev, 1, NULL, 0, NULL);
#else
    (void)lp;
#endif
}

int dyn_evloop_timer_fired(const dyn_evloop_t* lp)
{
    return lp ? lp->timer_fired : 0;
}


#define DYN_EV_CHG_CHUNK 64
#define DYN_EV_CHG_WAIT 256

static _Thread_local uint64_t ev_stat_calls, ev_stat_changes;

void dyn_evloop_thread_stats(uint64_t* calls, uint64_t* changes)
{
    *calls = ev_stat_calls;
    *changes = ev_stat_changes;
}

#define DYN_EV_TIMER_IDENT 0x7ffffffe
#define DYN_EV_DEADLINE_IDENT 0x7ffffffd
#define DYN_EV_PRECISE_MAX_US 1000000

int dyn_evloop_set_timer(dyn_evloop_t* lp, unsigned period_ms)
{
#if defined(DYN_EV_KQUEUE)
    struct kevent kev;
    ev_stat_calls++;
    if (!period_ms) {
        EV_SET(&kev, DYN_EV_TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(lp->backend_fd, &kev, 1, NULL, 0, NULL);
        return 0;
    }
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
    (void)lp;
    (void)period_ms;
    errno = ENOTSUP;
    return -1;
#endif
}

#if defined(DYN_EV_KQUEUE)
static int kq_filter(int kq, int fd, int filter, int add, void* udata)
{
    struct kevent kev;
    ev_stat_calls++;
    ev_stat_changes++;
    EV_SET(&kev, (uintptr_t)fd, (int16_t)filter,
        add ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, udata);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        if (!add && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}

static int kq_vnode(int kq, int fd, int add, void* udata)
{
    struct kevent kev;
    ev_stat_calls++;
    ev_stat_changes++;
    unsigned ff = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK;
    EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE,
        add ? (EV_ADD | EV_ENABLE | EV_CLEAR) : EV_DELETE, ff, 0, udata);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        if (!add && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}

static _Thread_local dyn_evloop_t* ev_pend_head;

static void kq_unlink(dyn_evloop_t* lp)
{
    dyn_evloop_t** pp = &ev_pend_head;
    if (!lp->linked)
        return;
    while (*pp && *pp != lp)
        pp = &(*pp)->pend_next;
    if (*pp)
        *pp = lp->pend_next;
    lp->pend_next = NULL;
    lp->linked = 0;
}

static void kq_failed(dyn_evloop_t* lp, const struct kevent* e)
{
    int fd = (int)e->ident;
    dyn_ev_reg* r;
    if ((e->flags & EV_ADD) && fd >= 0 && fd < lp->cap && (e->filter == EVFILT_READ || e->filter == EVFILT_WRITE))
        lp->regs[fd].inkern[e->filter == EVFILT_WRITE] = 0;
    if ((e->flags & EV_DELETE) || e->data == EBADF || e->data == ENOENT || fd < 0 || fd >= lp->cap)
        return;
    if (e->filter != EVFILT_READ && e->filter != EVFILT_WRITE)
        return;
    r = &lp->regs[fd];
    if (!r->interest || !r->cb || (uint32_t)(uintptr_t)e->udata != r->gen)
        return;
    r->cb(lp, fd, DYN_EV_ERROR | (e->filter == EVFILT_WRITE ? DYN_EV_WRITE : DYN_EV_READ), r->udata);
}

static int kq_room(dyn_evloop_t* lp, int need)
{
    int nc = lp->chg_cap ? lp->chg_cap : 64;
    struct kevent* c;
    if (need <= lp->chg_cap)
        return 1;
    while (nc < need)
        nc *= 2;
    c = (struct kevent*)realloc(lp->chg, (size_t)nc * sizeof(*c));
    if (!c)
        return 0;
    lp->chg = c;
    lp->chg_cap = nc;
    return 1;
}

static int kq_take(dyn_evloop_t* lp)
{
    int i, n = lp->nchg;
    for (i = 0; i < n; i++) {
        int fd = (int)lp->chg[i].ident, w = lp->chg[i].filter == EVFILT_WRITE;
        lp->regs[fd].pend[w] = 0;
        lp->regs[fd].inkern[w] = (lp->chg[i].flags & EV_ADD) != 0;
    }
    lp->nchg = 0;
    kq_unlink(lp);
    return n;
}

static void kq_flush(dyn_evloop_t* lp)
{
    struct kevent out[DYN_EV_CHG_CHUNK];
    struct timespec zero = { 0, 0 };
    struct kevent* c = lp->chg;
    int cap = lp->chg_cap, n = kq_take(lp), off, i;
    if (!n)
        return;
    lp->chg = NULL;
    lp->chg_cap = 0;
    for (i = 0; i < n; i++)
        c[i].flags |= EV_RECEIPT;
    for (off = 0; off < n; off += DYN_EV_CHG_CHUNK) {
        int k = n - off < DYN_EV_CHG_CHUNK ? n - off : DYN_EV_CHG_CHUNK, r, j;
        ev_stat_calls++;
        ev_stat_changes += (uint64_t)k;
        r = kevent(lp->backend_fd, c + off, k, out, k, &zero);
        for (j = 0; j < r; j++)
            if ((out[j].flags & EV_ERROR) && out[j].data)
                kq_failed(lp, &out[j]);
    }
    if (!lp->chg) {
        lp->chg = c;
        lp->chg_cap = cap;
    } else {
        free(c);
    }
}

static void kq_drop(dyn_evloop_t* lp, int fd, int w)
{
    int32_t p = lp->regs[fd].pend[w];
    struct kevent* e = &lp->chg[p - 1];
    struct kevent* last = &lp->chg[--lp->nchg];
    lp->regs[fd].pend[w] = 0;
    if (e != last) {
        *e = *last;
        lp->regs[(int)e->ident].pend[e->filter == EVFILT_WRITE] = p;
    }
}

static int kq_queue(dyn_evloop_t* lp, int fd, int w, int add)
{
    dyn_ev_reg* r = &lp->regs[fd];
    int32_t p = r->pend[w];
    int16_t filt = w ? EVFILT_WRITE : EVFILT_READ;
    void* tag = (void*)(uintptr_t)r->gen;
    if (p) {
        struct kevent* e = &lp->chg[p - 1];
        if (!add && (e->flags & EV_ADD) && !r->inkern[w]) {
            kq_drop(lp, fd, w);
            return 0;
        }
        EV_SET(e, (uintptr_t)fd, filt, add ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, tag);
        return 0;
    }
    if (!kq_room(lp, lp->nchg + 1))
        {
            int rc = kq_filter(lp->backend_fd, fd, filt, add, tag);
            if (rc == 0)
                r->inkern[w] = add ? 1 : 0;
            return rc;
        }
    EV_SET(&lp->chg[lp->nchg], (uintptr_t)fd, filt, add ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, tag);
    r->pend[w] = ++lp->nchg;
    if (lp->nested && !lp->linked) {
        lp->pend_next = ev_pend_head;
        ev_pend_head = lp;
        lp->linked = 1;
    }
    return 0;
}

static int backend_apply(dyn_evloop_t* lp, int fd, int old, int neu, void* udata)
{
    int rc = 0;
    void* tag = (void*)(uintptr_t)lp->regs[fd].gen;
    (void)udata;
    if ((neu & DYN_EV_READ) != (old & DYN_EV_READ))
        rc |= kq_queue(lp, fd, 0, neu & DYN_EV_READ);
    if ((neu & DYN_EV_WRITE) != (old & DYN_EV_WRITE))
        rc |= kq_queue(lp, fd, 1, neu & DYN_EV_WRITE);
    if ((neu & DYN_EV_VNODE) != (old & DYN_EV_VNODE))
        rc |= kq_vnode(lp->backend_fd, fd, neu & DYN_EV_VNODE, tag);
    return rc;
}
#elif defined(DYN_EV_EPOLL)
static int backend_apply(dyn_evloop_t* lp, int fd, int old, int neu, void* udata)
{
    struct epoll_event ev;
    int op;
    (void)udata;
    if (!old && !neu)
        return 0;
    memset(&ev, 0, sizeof(ev));
    ev.data.u64 = ((uint64_t)lp->regs[fd].gen << 32) | (uint32_t)fd;
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
    ev_stat_calls++;
    ev_stat_changes++;
    if (epoll_ctl(lp->backend_fd, op, fd, &ev) < 0) {
        if (op == EPOLL_CTL_DEL && errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}
#else
static int backend_apply(dyn_evloop_t* lp, int fd, int old, int neu, void* ud)
{
    (void)lp;
    (void)fd;
    (void)old;
    (void)neu;
    (void)ud;
    return 0;
}
#endif

int dyn_evloop_backend_fd(dyn_evloop_t* lp)
{
#ifdef DYN_EV_KQUEUE
    lp->nested = 1;
    if (lp->nchg && !lp->linked) {
        lp->pend_next = ev_pend_head;
        ev_pend_head = lp;
        lp->linked = 1;
    }
#endif
    return lp->backend_fd;
}

static _Thread_local void (*ev_preblock_fn)(void*);
static _Thread_local void* ev_preblock_ud;

void dyn_evloop_set_preblock(void (*fn)(void*), void* ud)
{
    ev_preblock_fn = fn;
    ev_preblock_ud = ud;
}

void dyn_evloop_clear_preblock(void* ud)
{
    if (ev_preblock_ud == ud) {
        ev_preblock_fn = NULL;
        ev_preblock_ud = NULL;
    }
}

void dyn_evloop_flush_pending(void)
{
    if (ev_preblock_fn)
        ev_preblock_fn(ev_preblock_ud);
#ifdef DYN_EV_KQUEUE
    while (ev_pend_head)
        kq_flush(ev_pend_head);
#endif
}

int dyn_evloop_add(dyn_evloop_t* lp, int fd, int interest, dyn_ev_cb cb,
    void* udata)
{
    int old;
    if (fd < 0 || reg_ensure(lp, fd) < 0)
        return -1;
#if !defined(DYN_EV_KQUEUE)
    if (interest & DYN_EV_VNODE) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    old = lp->regs[fd].interest;
    lp->regs[fd].cb = cb;
    lp->regs[fd].udata = udata;
    lp->regs[fd].interest = interest;
    if ((old == 0) != (interest == 0))
        lp->regs[fd].gen++;
    if (backend_apply(lp, fd, old, interest, udata) < 0) {
        lp->regs[fd].interest = old;
        if ((old == 0) != (interest == 0))
            lp->regs[fd].gen++;
        return -1;
    }
    if (!old && interest)
        lp->nreg++;
    else if (old && !interest)
        lp->nreg--;
    return 0;
}

int dyn_evloop_mod(dyn_evloop_t* lp, int fd, int interest)
{
    int old;
    if (fd < 0 || fd >= lp->cap)
        return -1;
    old = lp->regs[fd].interest;
    if (old == interest)
        return 0;
    lp->regs[fd].interest = interest;
    if ((old == 0) != (interest == 0))
        lp->regs[fd].gen++;
    if (backend_apply(lp, fd, old, interest, lp->regs[fd].udata) < 0) {
        lp->regs[fd].interest = old;
        if ((old == 0) != (interest == 0))
            lp->regs[fd].gen++;
        return -1;
    }
    if (!old && interest)
        lp->nreg++;
    else if (old && !interest)
        lp->nreg--;
    return 0;
}

int dyn_evloop_del(dyn_evloop_t* lp, int fd)
{
    if (fd < 0 || fd >= lp->cap || !lp->regs[fd].interest)
        return 0;
    return dyn_evloop_mod(lp, fd, 0);
}

int dyn_evloop_forget(dyn_evloop_t* lp, int fd)
{
#ifdef DYN_EV_KQUEUE
    dyn_ev_reg* r;
    if (fd < 0 || fd >= lp->cap)
        return 0;
    r = &lp->regs[fd];
    if (r->interest & DYN_EV_VNODE)
        return dyn_evloop_del(lp, fd);
    if (r->pend[0])
        kq_drop(lp, fd, 0);
    if (r->pend[1])
        kq_drop(lp, fd, 1);
    r->inkern[0] = r->inkern[1] = 0;
    if (r->interest) {
        r->interest = 0;
        r->gen++;
        lp->nreg--;
    }
    return 0;
#else
    return dyn_evloop_del(lp, fd);
#endif
}

#define DYN_EV_BATCH 1024

#if defined(DYN_EV_KQUEUE)
int dyn_evloop_poll(dyn_evloop_t* lp, int timeout_ms)
{
    return dyn_evloop_poll_us(lp, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000);
}

int dyn_evloop_poll_us(dyn_evloop_t* lp, int64_t timeout_us)
{
    struct kevent evs[DYN_EV_BATCH] DYN_IO_SCRATCH;
    struct timespec ts, *pts = NULL;
    int n, i, nch, dispatched = 0;
    int precise;

    lp->timer_fired = 0;
    if (timeout_us >= 0) {
        ts.tv_sec = (time_t)(timeout_us / 1000000);
        ts.tv_nsec = (long)(timeout_us % 1000000) * 1000L;
        pts = &ts;
    }
    if (lp->nchg > DYN_EV_CHG_WAIT)
        kq_flush(lp);
    nch = kq_take(lp);
    precise = timeout_us > 0 && timeout_us < DYN_EV_PRECISE_MAX_US;
    if ((precise || lp->deadline_armed) && kq_room(lp, nch + 1)) {
        if (precise) {
            EV_SET(&lp->chg[nch], DYN_EV_DEADLINE_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
                NOTE_USECONDS | NOTE_CRITICAL, timeout_us, NULL);
            pts = NULL;
        } else {
            EV_SET(&lp->chg[nch], DYN_EV_DEADLINE_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        }
        nch++;
        lp->deadline_armed = precise;
    }
    ev_stat_calls++;
    ev_stat_changes += (uint64_t)nch;
    n = kevent(lp->backend_fd, lp->chg, nch, evs, DYN_EV_BATCH, pts);
    dyn_wall_tick();
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        int fd = (int)evs[i].ident;
        int mask = 0;
        uint32_t evgen;
        dyn_ev_reg* r;
        if (evs[i].filter == EVFILT_USER) {
            if (lp->user_cb) {
                lp->user_cb(lp, -1, DYN_EV_READ, lp->user_ud);
                dispatched++;
            }
            continue;
        }
        if (evs[i].filter == EVFILT_TIMER && (int)(uintptr_t)evs[i].ident == DYN_EV_DEADLINE_IDENT) {
            lp->deadline_armed = 0;
            continue;
        }
        if (evs[i].flags & EV_ERROR) {
            if (evs[i].data)
                kq_failed(lp, &evs[i]);
            continue;
        }
        if ((int)(uintptr_t)evs[i].ident == DYN_EV_TIMER_IDENT) {
            lp->timer_fired = 1;
            continue;
        }
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        evgen = (uint32_t)(uintptr_t)evs[i].udata;
        if (!r->interest || !r->cb || evgen != r->gen)
            continue;
        if (evs[i].filter == EVFILT_READ)
            mask |= DYN_EV_READ;
        else if (evs[i].filter == EVFILT_WRITE)
            mask |= DYN_EV_WRITE;
        else if (evs[i].filter == EVFILT_VNODE)
            mask |= DYN_EV_VNODE;
        if ((evs[i].flags & EV_EOF) && evs[i].filter != EVFILT_VNODE)
            mask |= DYN_EV_ERROR;
        r->cb(lp, fd, mask, r->udata);
        dispatched++;
    }
    return dispatched;
}
#elif defined(DYN_EV_EPOLL)
static _Thread_local int ep_no_pwait2;

static int ep_wait_us(int efd, struct epoll_event* evs, int64_t us)
{
#ifdef SYS_epoll_pwait2
    if (us > 0 && us % 1000 && !ep_no_pwait2) {
        struct timespec ts;
        int n;
        ts.tv_sec = (time_t)(us / 1000000);
        ts.tv_nsec = (long)(us % 1000000) * 1000L;
        n = (int)syscall(SYS_epoll_pwait2, efd, evs, DYN_EV_BATCH, &ts, NULL, (size_t)8);
        if (n >= 0 || (errno != ENOSYS && errno != EPERM))
            return n;
        ep_no_pwait2 = 1;
    }
#endif
    return epoll_wait(efd, evs, DYN_EV_BATCH, us < 0 ? -1 : us >= (int64_t)INT_MAX * 1000 ? INT_MAX : (int)((us + 999) / 1000));
}

int dyn_evloop_poll(dyn_evloop_t* lp, int timeout_ms)
{
    return dyn_evloop_poll_us(lp, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000);
}

int dyn_evloop_poll_us(dyn_evloop_t* lp, int64_t timeout_us)
{
    struct epoll_event evs[DYN_EV_BATCH] DYN_IO_SCRATCH;
    int n, i, dispatched = 0;

    lp->timer_fired = 0;
    ev_stat_calls++;
    n = ep_wait_us(lp->backend_fd, evs, timeout_us);
    dyn_wall_tick();
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        int fd = (int)(uint32_t)evs[i].data.u64;
        uint32_t evgen = (uint32_t)(evs[i].data.u64 >> 32);
        int mask = 0;
        dyn_ev_reg* r;
        if (fd >= 0 && fd == lp->timer_fd) {
            lp->timer_fired = 1;
            uint64_t ticks;
            while (read(lp->timer_fd, &ticks, sizeof(ticks)) == sizeof(ticks))
                ;
            continue;
        }
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        if (!r->interest || !r->cb || evgen != r->gen)
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
#else
int dyn_evloop_poll_us(dyn_evloop_t* lp, int64_t timeout_us)
{
    return dyn_evloop_poll(lp, timeout_us < 0 ? -1 : timeout_us >= (int64_t)INT_MAX * 1000 ? INT_MAX : (int)((timeout_us + 999) / 1000));
}

int dyn_evloop_poll(dyn_evloop_t* lp, int timeout_ms)
{
    int i, nfds = 0, n, dispatched = 0;

    lp->timer_fired = 0;
    if (lp->pfds_cap < lp->cap) {
        struct pollfd* np = (struct pollfd*)realloc(lp->pfds, (size_t)lp->cap * sizeof(*np));
        uint32_t* ng = (uint32_t*)realloc(lp->pgens, (size_t)lp->cap * sizeof(*ng));
        if (!np || !ng) {
            free(np);
            free(ng);
            return -1;
        }
        lp->pfds = np;
        lp->pgens = ng;
        lp->pfds_cap = lp->cap;
    }
    for (i = 0; i < lp->cap; i++) {
        if (!lp->regs[i].interest)
            continue;
        lp->pfds[nfds].fd = i;
        lp->pfds[nfds].events = 0;
        lp->pfds[nfds].revents = 0;
        lp->pgens[nfds] = lp->regs[i].gen;
        if (lp->regs[i].interest & DYN_EV_READ)
            lp->pfds[nfds].events |= POLLIN;
        if (lp->regs[i].interest & DYN_EV_WRITE)
            lp->pfds[nfds].events |= POLLOUT;
        nfds++;
    }
    ev_stat_calls++;
    n = poll(lp->pfds, (nfds_t)nfds, timeout_ms);
    dyn_wall_tick();
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < nfds && dispatched < n; i++) {
        int fd = lp->pfds[i].fd;
        short re = lp->pfds[i].revents;
        int mask = 0;
        dyn_ev_reg* r;
        if (!re)
            continue;
        if (fd < 0 || fd >= lp->cap)
            continue;
        r = &lp->regs[fd];
        if (!r->interest || !r->cb || lp->pgens[i] != r->gen)
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

#endif
