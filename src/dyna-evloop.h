/*
 * dyna-evloop -- a small, portable readiness-based event loop.
 *
 * One reactor multiplexes many non-blocking fds on a SINGLE thread. It is the
 * shared I/O core beneath both native-async servers (Model A: native handlers)
 * and the JS-integrated async path (Model B: JS handlers driven through
 * js_std_loop). Backends, selected at compile time:
 *
 *     kqueue  -- macOS / *BSD          (this host)
 *     epoll   -- Linux
 *     poll(2) -- portable fallback
 *
 * The io_uring backend slots in behind exactly this interface (a completion
 * model mapped onto the same add/mod/del + poll surface); it is Linux-only and
 * cannot be exercised on this host, so it is left as a documented seam.
 *
 * Readiness is LEVEL-triggered (a ready fd re-fires until drained) -- the
 * simplest correct contract for HTTP connection state machines; no busy-loop
 * because a fd with nothing to do carries no interest bit.
 */
#ifndef DYNAJS_EVLOOP_H
#define DYNAJS_EVLOOP_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>

#define DYN_EV_READ  1  /* fd is readable / accept()able */
#define DYN_EV_WRITE 2  /* fd is writable */
#define DYN_EV_ERROR 4  /* delivered to the callback on hangup/error */
/* The one NON-readiness interest: "this vnode changed" (kqueue EVFILT_VNODE).
 * The ident is a real directory fd, so it routes through the same per-fd table
 * as READ/WRITE and needs no second reactor. KQUEUE ONLY -- epoll has no vnode
 * filter, and dyn_evloop_add returns -1/ENOTSUP there. Linux watches files with
 * inotify, whose single fd is an ordinary DYN_EV_READ registration. */
#define DYN_EV_VNODE 8

typedef struct dyn_evloop dyn_evloop_t;

/* Dispatched when a registered fd is ready. `events` is a mask of DYN_EV_*.
 * The callback may add/mod/del any fd (including its own) and may free its
 * udata after calling dyn_evloop_del on the fd. */
typedef void (*dyn_ev_cb)(dyn_evloop_t *lp, int fd, int events, void *udata);

/* Create/destroy a reactor. dyn_evloop_new returns NULL on failure. */
dyn_evloop_t *dyn_evloop_new(void);
void dyn_evloop_free(dyn_evloop_t *lp);

/* Register `fd` with the given interest mask (DYN_EV_READ|DYN_EV_WRITE) and its
 * callback/udata. Returns 0, or -1 on error. fd must be non-blocking. */
int dyn_evloop_add(dyn_evloop_t *lp, int fd, int interest, dyn_ev_cb cb,
                   void *udata);

/* Change the interest mask for an already-registered fd. Returns 0 or -1. */
int dyn_evloop_mod(dyn_evloop_t *lp, int fd, int interest);

/* Unregister `fd` (does NOT close it). Safe to call from within a callback,
 * including on the fd currently being dispatched. Returns 0 or -1. */
int dyn_evloop_del(dyn_evloop_t *lp, int fd);

/* Wait up to `timeout_ms` (<0 = forever, 0 = non-blocking) for readiness and
 * dispatch every ready fd's callback once. Returns the number of fds
 * dispatched, 0 on timeout, or -1 on a hard error (EINTR yields 0). This is the
 * single step js_std_loop calls to fold the reactor into the JS event loop. */
int dyn_evloop_poll(dyn_evloop_t *lp, int timeout_ms);

/* Count of fds currently registered (interest != 0). */
size_t dyn_evloop_count(const dyn_evloop_t *lp);

/* The reactor's backend kernel fd (kqueue/epoll), suitable for adding to an
 * OUTER poll(2)/epoll set so the whole reactor folds into another event loop
 * (this is how it integrates into the JS thread's js_std_loop). Returns -1 for
 * the portable poll(2) fallback, which has no single pollable fd. When this fd
 * signals readable, drain with dyn_evloop_poll(lp, 0). */
int dyn_evloop_backend_fd(const dyn_evloop_t *lp);

/* Arm ONE periodic wakeup on this loop, so the backend fd becomes readable on a
 * schedule even when no descriptor is active. Needed by anything that must run
 * on a clock rather than on traffic -- an idle-connection sweep will otherwise
 * never run on a quiet server.
 *
 * `period_ms` of 0 disarms. Returns 0, or -1 where the backend has no timer
 * primitive (the poll(2) fallback), in which case the caller must drive its own
 * timeouts. The three backends share nothing here: kqueue uses EVFILT_TIMER,
 * epoll a timerfd, and poll(2) has neither. */
int dyn_evloop_set_timer(dyn_evloop_t *lp, unsigned period_ms);

#endif /* CONFIG_NATIVE_MODULES */
#endif /* DYNAJS_EVLOOP_H */
