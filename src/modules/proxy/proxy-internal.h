#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROXY)

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

dyn_aio_t* dyn_net_reactor_acquire(JSContext* ctx);

void dyn_net_reactor_release(JSContext* ctx);

void dyn_net_reactor_release_rt(JSRuntime* rt);

int dyn_net_on_drain(void (*fn)(void*), void* udata);

void dyn_net_off_drain(void* udata);

typedef struct dyn_pxy dyn_pxy_t;

typedef struct dyn_pxy_pair {
    dyn_pxy_t* owner;
    int cfd, ufd;
    int refs;
    unsigned c_eof : 1;
    unsigned u_eof : 1;
    unsigned c_paused : 1;
    unsigned u_paused : 1;
    unsigned connected : 1;
    unsigned gone : 1;
    size_t to_c, to_u;
    uint64_t last_ms;
    uint64_t deadline_ms;
    struct dyn_pxy_pair *lnext, *lprev;
} dyn_pxy_pair_t;

struct dyn_pxy {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    int started;
    int hooked;

    char** up_host;
    uint16_t* up_port;
    struct sockaddr_storage* up_sa;
    socklen_t* up_salen;
    int* up_fam;
    int n_up, next_up;

    int max_conns;
    uint64_t idle_ms;
    uint64_t connect_ms;
    char* bind_host;

    dyn_pxy_pair_t* pairs;
    int npairs;
    uint64_t n_accepted, n_refused, n_idle_closed, n_connect_failed;
    uint64_t bytes_up, bytes_down;
};

static inline void pxy_pair_unref(dyn_pxy_pair_t* pr)
{
    if (--pr->refs == 0)
        free(pr);
}

void pxy_recv_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

void pxy_recv_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

void pxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

void pxy_pair_close(dyn_pxy_pair_t* pr);

void pxy_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

int pxy_read_upstream(JSContext* ctx, dyn_pxy_t* p, JSValueConst o,
    int idx);

int pxy_read_opts(JSContext* ctx, dyn_pxy_t* p, JSValueConst o);

#endif

#endif
