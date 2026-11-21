#ifndef DYNAJS_EVLOOP_H
#define DYNAJS_EVLOOP_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdint.h>

#define DYN_EV_READ 1
#define DYN_EV_WRITE 2
#define DYN_EV_ERROR 4
#define DYN_EV_VNODE 8

typedef struct dyn_evloop dyn_evloop_t;

typedef void (*dyn_ev_cb)(dyn_evloop_t* lp, int fd, int events, void* udata);

dyn_evloop_t* dyn_evloop_new(void);
void dyn_evloop_free(dyn_evloop_t* lp);

int dyn_evloop_add(dyn_evloop_t* lp, int fd, int interest, dyn_ev_cb cb,
    void* udata);

int dyn_evloop_mod(dyn_evloop_t* lp, int fd, int interest);

int dyn_evloop_del(dyn_evloop_t* lp, int fd);

int dyn_evloop_forget(dyn_evloop_t* lp, int fd);

int dyn_evloop_poll(dyn_evloop_t* lp, int timeout_ms);

int dyn_evloop_poll_us(dyn_evloop_t* lp, int64_t timeout_us);

size_t dyn_evloop_count(const dyn_evloop_t* lp);

int dyn_evloop_backend_fd(dyn_evloop_t* lp);

void dyn_evloop_flush_pending(void);
void dyn_evloop_set_preblock(void (*fn)(void*), void* ud);
void dyn_evloop_clear_preblock(void* ud);

void dyn_evloop_thread_stats(uint64_t* calls, uint64_t* changes);

int dyn_evloop_set_timer(dyn_evloop_t* lp, unsigned period_ms);

int dyn_evloop_timer_fired(const dyn_evloop_t* lp);

int dyn_evloop_user_set(dyn_evloop_t* lp, dyn_ev_cb cb, void* udata);
void dyn_evloop_user_trigger(dyn_evloop_t* lp);

#endif
#endif
