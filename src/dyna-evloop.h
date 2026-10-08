#ifndef DYNAJS_EVLOOP_H
#define DYNAJS_EVLOOP_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>

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

int dyn_evloop_poll(dyn_evloop_t* lp, int timeout_ms);

size_t dyn_evloop_count(const dyn_evloop_t* lp);

int dyn_evloop_backend_fd(const dyn_evloop_t* lp);

int dyn_evloop_set_timer(dyn_evloop_t* lp, unsigned period_ms);

int dyn_evloop_timer_fired(const dyn_evloop_t* lp);

#endif
#endif
